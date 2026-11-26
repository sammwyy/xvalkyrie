#include "app/cli/cli.hpp"
#include "analysis/assessment.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/gdk/game_config.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "host/files.hpp"
#include "launcher/debug.hpp"
#include "launcher/launch.hpp"
#include "loader/image.hpp"
#include "runtime/console.hpp"
#include "runtime/modules/console_shader.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace xv::cli {
namespace {
std::vector<std::uint8_t> read_title(const std::filesystem::path &path) {
    return host::read_snapshot(title_executable(path), 256 * 1024 * 1024);
}
// The root signature of the pipeline a dumped shader was met in, which XV_SHADER_CODE
// writes next to it as .rts0 when the shader has none of its own
std::vector<std::uint8_t> pipeline_root_signature(std::filesystem::path shader) {
    shader.replace_extension(".rts0");
    std::error_code error;
    if (!std::filesystem::is_regular_file(shader, error))
        return {};
    return host::read_snapshot(shader, 1024 * 1024);
}
nlohmann::json inspect_file(const std::filesystem::path &path) {
    return pe::inspect(read_title(path));
}
nlohmann::json config_report(const std::filesystem::path &path) {
    return gdk::inspect_config(host::read_snapshot(path, gdk::config_limit));
}
} // namespace

const std::string_view commands =
    "  xvboot inspect FILE | inspect-config MicrosoftGame.config\n"
    "  xvboot inspect-manifest AppxManifest.xml\n"
    "  xvboot assess FILE [--config MicrosoftGame.config --entry NAME]\n"
    "  xvboot map FILE [--base HEX]     lay out the image; never runs it\n"
    "  xvboot inspect-package FILE      identify an XVD/XVC header; never opens it\n"
    "  xvboot bind FILE                 show where each import would bind; never runs it\n"
    "  xvboot properties FILE           all of that in one report, as the window shows it\n"
    "  xvboot modules durango|scarlett  the console modules and functions implemented here\n"
    "  xvboot shader-probe              what stops the translation of the shader code on each\n"
    "                                   line of the input, as hexadecimal words\n"
    "  xvboot shader-source FILE        the HLSL made for one of the shaders of such a folder\n"
    "  xvboot shader-corpus FOLDER [--compile]\n"
    "                                   how many of the shaders a run left there with\n"
    "                                   XV_SHADER_CODE are translated, and what stops the rest;\n"
    "                                   with --compile, the host's compiler is asked as well\n"
    "  xvboot shader-journal JOURNAL OUT [--all]\n"
    "                                   the shaders of a title's journal in the shader cache that\n"
    "                                   this version has not made (all with --all), as such a\n"
    "                                   folder: OUT/heaps for those that read the heaps, which\n"
    "                                   shader-corpus takes with XV_HEAP_SHADERS=1, OUT/plain\n";

std::optional<int> run(int argc, wchar_t **argv) {
    if (argc < 2)
        return std::nullopt;
    // XV_HEAP_SHADERS=1: the shader commands translate as a run with it does
    if (const auto *const heaps = std::getenv("XV_HEAP_SHADERS"); heaps && *heaps == '1')
        runtime::modules::use_heap_shaders(true);
    const std::filesystem::path command(argv[1]);
    if (argc == 3 && command == "inspect") {
        std::cout << inspect_file(argv[2]).dump(2) << '\n';
        return 0;
    }
    if (argc == 3 && command == "inspect-config") {
        std::cout << config_report(argv[2]).dump(2) << '\n';
        return 0;
    }
    if (command == "assess" &&
        (argc == 3 || (argc == 7 && std::filesystem::path(argv[3]) == "--config" &&
                       std::filesystem::path(argv[5]) == "--entry"))) {
        const auto pe = inspect_file(argv[2]);
        auto report = analysis::assess(pe);
        if (argc == 7) {
            const auto entry = std::filesystem::path(argv[6]).u8string();
            report["config_evidence"] = analysis::compare_declaration(
                pe, config_report(argv[4]), std::string(entry.begin(), entry.end()));
        }
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    if (argc == 3 && command == "inspect-manifest") {
        const auto bytes = host::read_snapshot(argv[2], appx::manifest_limit);
        std::cout << appx::inspect_manifest(bytes).dump(2) << '\n';
        return 0;
    }
    if (argc == 3 && command == "inspect-package") {
        const auto prefix = host::read_prefix(argv[2], xvd::header_size);
        std::cout << xvd::identify(prefix.bytes, prefix.file_size).dump(2) << '\n';
        return 0;
    }
    if (command == "map" &&
        (argc == 3 || (argc == 5 && std::filesystem::path(argv[3]) == "--base"))) {
        loader::MapOptions options;
        if (argc == 5) {
            const auto text = std::filesystem::path(argv[4]).string();
            std::size_t used = 0;
            options.guest_base = std::stoull(text, &used, 16);
            if (used != text.size())
                throw std::invalid_argument("--base expects a hexadecimal guest address");
        }
        const auto bytes = read_title(argv[2]);
        std::cout << loader::MappedImage(bytes, options).report().dump(2) << '\n';
        return 0;
    }
    if (argc == 3 && command == "properties") {
        std::cout << title_properties(argv[2]).dump(2, ' ', false,
                                                    nlohmann::json::error_handler_t::replace)
                  << '\n';
        return 0;
    }
    if (argc == 3 && command == "bind") {
        const auto bytes = read_title(argv[2]);
        std::cout << runtime::bind_plan(
                         bytes, runtime::title_registry(bytes),
                         std::filesystem::absolute(title_executable(argv[2])).parent_path(),
                         runtime::console_system_directory(title_executable(argv[2])))
                         .dump(2)
                  << '\n';
        return 0;
    }
    if (argc >= 5 && argc <= 7 && command == "shader-program") {
        // v|p, the program of a cached pipeline (XV_SHADER_CODE's pso_g_<n>_<part>.bin) and
        // its root signature (.rts0): the HLSL, or what stops it
        const auto read = [](const wchar_t *path) {
            std::ifstream file(path, std::ios::binary);
            return std::vector<char>(std::istreambuf_iterator<char>(file), {});
        };
        const auto code = read(argv[3]), signature = read(argv[4]),
                   layout = argc >= 6 ? read(argv[5]) : std::vector<char>{},
                   records = argc == 7 ? read(argv[6]) : std::vector<char>{};
        const std::span<const std::uint8_t> bound{
            reinterpret_cast<const std::uint8_t *>(records.data()), records.size()};
        std::vector<std::uint32_t> words(code.size() / 4);
        std::memcpy(words.data(), code.data(), words.size() * 4);
        std::string why;
        const auto source = runtime::modules::console_program_source(
            argv[2][0] == L'v' ? runtime::modules::ShaderStage::vertex
                               : runtime::modules::ShaderStage::pixel,
            words, {reinterpret_cast<const std::uint8_t *>(signature.data()), signature.size()},
            &why, {reinterpret_cast<const std::uint8_t *>(layout.data()), layout.size()}, bound);
        std::cout << (source ? *source : "stopped: " + why) << std::endl;
        if (source) {
            const auto stage = argv[2][0] == L'v' ? runtime::modules::ShaderStage::vertex
                                                  : runtime::modules::ShaderStage::pixel;
            const auto shader = runtime::modules::console_program_shader(
                stage, words,
                {reinterpret_cast<const std::uint8_t *>(signature.data()), signature.size()},
                {reinterpret_cast<const std::uint8_t *>(layout.data()), layout.size()}, &why,
                bound);
            std::cout << (shader.empty() ? "not compiled: " + why
                                         : "compiled: " + std::to_string(shader.size()) + " bytes")
                      << std::endl;
        }
        return source ? 0 : 1;
    }
    if ((argc == 2 || (argc == 3 && std::filesystem::path(argv[2]) == "--compile")) &&
        command == "shader-probe") {
        // one shader a line, as hexadecimal words of machine code: what stops its
        // translation, or nothing
        auto report = nlohmann::json::array();
        for (std::string line; std::getline(std::cin, line);) {
            std::vector<std::uint32_t> words;
            for (std::size_t at = 0, used = 0; at < line.size(); at += used) {
                while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])))
                    ++at;
                if (at == line.size())
                    break;
                words.push_back(static_cast<std::uint32_t>(std::stoul(line.substr(at), &used, 16)));
            }
            report.push_back(runtime::modules::console_shader_probe(words, argc == 3));
        }
        std::cout << report.dump() << '\n';
        return 0;
    }
    if ((argc == 3 || (argc == 4 && std::filesystem::path(argv[3]) == "--compile")) &&
        command == "shader-corpus") {
        // <hash>-<stage>.dxbc files, as XV_SHADER_CODE writes them
        std::map<std::string, std::vector<std::string>> stopped;
        std::map<std::string, std::uint32_t> where; // of each reason's first shader
        std::size_t translated = 0, shaders = 0;
        for (const auto &entry : std::filesystem::directory_iterator(argv[2])) {
            const auto name = entry.path().stem().string();
            if (entry.path().extension() != ".dxbc" || name.size() < 2)
                continue;
            const auto stage = name.back() == 'v'   ? runtime::modules::ShaderStage::vertex
                               : name.back() == 'p' ? runtime::modules::ShaderStage::pixel
                               : name.back() == 'c' ? runtime::modules::ShaderStage::compute
                               : name.back() == 'm' ? runtime::modules::ShaderStage::mesh
                                                    : runtime::modules::ShaderStage(-1);
            if (stage == runtime::modules::ShaderStage(-1))
                continue;
            ++shaders;
            std::string why;
            std::uint32_t at = ~0u;
            const auto bytes = host::read_snapshot(entry.path(), 64 * 1024 * 1024);
            const auto source = runtime::modules::console_shader_source(
                stage, bytes, &why, &at, pipeline_root_signature(entry.path()));
            if (source && argc == 4) {
                // the first error the compiler has, without the place in the source
                why = runtime::modules::console_shader_errors(stage, *source);
                if (const auto error = why.find("error"); error != std::string::npos)
                    why = why.substr(error);
                why = why.substr(0, why.find('\n'));
                if (!why.empty())
                    why = "the host's compiler: " + why;
            }
            if (source && why.empty()) {
                ++translated;
                continue;
            }
            if (stopped[why].empty())
                where[why] = at;
            stopped[why].push_back(name);
        }
        auto reasons = nlohmann::json::array();
        for (const auto &[why, names] : stopped) {
            reasons.push_back({{"reason", why}, {"shaders", names.size()}, {"first", names[0]}});
            if (where[why] != ~0u) // in bytes, as a disassembly counts
                reasons.back()["at"] = 4 * where[why];
        }
        std::sort(reasons.begin(), reasons.end(), [](const auto &a, const auto &b) {
            return a["shaders"].template get<std::size_t>() >
                   b["shaders"].template get<std::size_t>();
        });
        std::cout << nlohmann::json{{"shaders", shaders},
                                    {"translated", translated},
                                    {"stopped", reasons}}
                         .dump(2)
                  << '\n';
        return 0;
    }
    if ((argc == 4 || (argc == 5 && std::filesystem::path(argv[4]) == "--all")) &&
        command == "shader-journal") {
        const std::filesystem::path out(argv[3]);
        std::size_t written = 0;
        for (const auto &shader :
             runtime::modules::journal_shaders(shader_cache_folder(), argv[2])) {
            if (argc == 4 && shader.state == runtime::modules::JournalShader::State::made)
                continue;
            const auto bytes = host::read_snapshot(shader.file, 64 * 1024 * 1024);
            if (bytes.size() < 32)
                continue;
            std::uint32_t sizes[2];
            std::memcpy(sizes, bytes.data() + 24, sizeof sizes);
            if (32ull + sizes[0] + sizes[1] > bytes.size())
                continue;
            const auto folder = out / (shader.heaps ? "heaps" : "plain");
            std::filesystem::create_directories(folder);
            const auto name = shader.hash + "-" + "vpc123m"[static_cast<int>(shader.stage)];
            std::ofstream(folder / (name + ".dxbc"), std::ios::binary)
                .write(reinterpret_cast<const char *>(bytes.data() + 32), sizes[0]);
            // the machine code, as XV_SHADER_CODE has it
            const auto code = runtime::modules::console_shader_code(
                std::span<const std::uint8_t>(bytes).subspan(32, sizes[0]));
            std::ofstream(folder / (name + ".bin"), std::ios::binary)
                .write(reinterpret_cast<const char *>(code.data()),
                       static_cast<std::streamsize>(code.size() * 4));
            if (sizes[1])
                std::ofstream(folder / (name + ".rts0"), std::ios::binary)
                    .write(reinterpret_cast<const char *>(bytes.data() + 32 + sizes[0]), sizes[1]);
            ++written;
        }
        std::cout << written << " shaders\n";
        return 0;
    }
    if ((argc == 4 || argc == 5) && command == "shader-precompile") {
        // <shader cache> <title's journal> [threads]: what a title's start makes, without it
        const auto work = runtime::modules::precompile_shaders(
            argv[2], argv[3], argc == 5 ? static_cast<unsigned>(std::stoul(argv[4])) : 8);
        while (!work->finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << nlohmann::json{{"total", work->total.load()},
                                    {"made", work->made.load()},
                                    {"failed", work->failed.load()}}
                         .dump()
                  << '\n';
        return 0;
    }
    if (argc == 3 && command == "shader-host") {
        // <hash>-<stage>.dxbc: how many bytes the host's shader made for it has, translated or
        // a stand-in, as a title's pipeline would get it
        const std::filesystem::path file(argv[2]);
        const auto name = file.stem().string();
        const auto stage = name.ends_with('v')   ? runtime::modules::ShaderStage::vertex
                           : name.ends_with('p') ? runtime::modules::ShaderStage::pixel
                           : name.ends_with('m') ? runtime::modules::ShaderStage::mesh
                                                 : runtime::modules::ShaderStage::compute;
        const auto made = runtime::modules::host_shader(
            stage, host::read_snapshot(file, 64 * 1024 * 1024),
            stage == runtime::modules::ShaderStage::mesh ? runtime::modules::HostShaderModel::sm6
                                                         : runtime::modules::HostShaderModel::sm5);
        std::cout << made.size() << " bytes, "
                  << runtime::modules::host_shader_note(stage, host::read_snapshot(file, 64 << 20))
                  << '\n';
        return made.empty() ? 1 : 0;
    }
    if (argc == 3 && command == "shader-source") {
        const std::filesystem::path file(argv[2]);
        const auto name = file.stem().string();
        const auto stage = name.ends_with('v')   ? runtime::modules::ShaderStage::vertex
                           : name.ends_with('p') ? runtime::modules::ShaderStage::pixel
                           : name.ends_with('m') ? runtime::modules::ShaderStage::mesh
                                                 : runtime::modules::ShaderStage::compute;
        std::string why;
        std::uint32_t at = ~0u;
        const auto source = runtime::modules::console_shader_source(
            stage, host::read_snapshot(file, 64 * 1024 * 1024), &why, &at,
            pipeline_root_signature(file));
        if (!source) {
            if (at != ~0u) // in words, from the first instruction
                why += " at " + std::to_string(at);
            std::cerr << why << '\n';
            return 1;
        }
        std::cout << *source;
        return 0;
    }
    if (argc == 3 && command == "modules") {
        const std::filesystem::path console(argv[2]);
        if (console != "durango" && console != "scarlett")
            throw std::invalid_argument("modules expects durango or scarlett");
        const auto target = console == "durango" ? Target::durango : Target::scarlett;
        std::cout << runtime::console_registry(target).describe().dump(2) << '\n';
        return 0;
    }
    return std::nullopt;
}
} // namespace xv::cli
