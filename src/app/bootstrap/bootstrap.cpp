#include "app/bootstrap/bootstrap.hpp"
#include "app/session.hpp"
#include "app/version.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include "backends/surface/surface.hpp"
#include "host/files.hpp"
#include "launcher/config.hpp"
#include "launcher/launch.hpp"
#include "launcher/library.hpp"
#include "runtime/console.hpp"
#include "runtime/modules/console_shader.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace xv::bootstrap {
namespace {
std::wstring wide(const std::string &utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())).wstring();
}
// what config.ini says, the way the runtime takes it
runtime::ConsoleSettings console_settings() {
    const auto saved = load_console_settings();
    return {
        wide(saved.gamertag), wide(saved.xbox_user_id), saved.gamerscore,     saved.age_group,
        saved.headset,        saved.keyboard_dialog,    wide(saved.language), saved.system_storage,
        saved.series_model,   saved.one_model};
}
// "<title> [<title ID>] | <console> | <video driver> | <n> FPS | XValkyrie <version> <commit>",
// with the console the title is told it runs on and the frame rate over the time since it was
// last asked for. The driver is what the title's device was made on, and the backend selected
// until it has one.
struct Caption {
    using Clock = std::chrono::steady_clock;
    std::wstring name;
    std::wstring console;
    Clock::time_point since = Clock::now();
    std::uint64_t frames = graphics::frames_presented();
    std::wstring operator()() {
        const auto now = Clock::now();
        const auto presented = graphics::frames_presented();
        const double seconds = std::chrono::duration<double>(now - since).count();
        const auto rate = seconds > 0 ? static_cast<double>(presented - frames) / seconds : 0;
        since = now;
        frames = presented;
        auto driver = graphics::video_driver();
        if (driver.empty())
            driver = graphics::selected_backend() == graphics::Backend::vulkan ? "Vulkan"
                                                                               : "Direct3D 11";
        auto text = name + L" | " + (console.empty() ? L"" : console + L" | ") + wide(driver) +
                    L" | " + std::to_wstring(static_cast<long long>(rate + .5)) +
                    L" FPS | XValkyrie " + wide(release);
        if (*commit)
            text += L" " + wide(commit);
        return text;
    }
};
// Commands on standard input, one a line, for driving a title without a person:
//   screenshot_temp                        the window's picture to xvboot_screenshot.bmp in
//                                          the temporary folder
//   joypad_press_key <control> [<ms>]      holds a control (a, b, dpad_down... as in
//                                          config.ini) for 150 ms or as long as said
void read_commands() {
    std::thread([] {
        for (std::string line; std::getline(std::cin, line);) {
            std::istringstream words(line);
            std::string command;
            if (!(words >> command))
                continue;
            if (command == "screenshot_temp") {
                const auto file = std::filesystem::temp_directory_path() / "xvboot_screenshot.bmp";
                std::cerr << "[Boot] screenshot_temp: "
                          << (surface::save_title_picture(file) ? app::text(file) : "failed")
                          << std::endl;
            } else if (command == "joypad_press_key") {
                std::string control;
                double milliseconds = 150;
                words >> control >> milliseconds;
                std::cerr << "[Boot] joypad_press_key " << control << ": "
                          << (input::press(control, milliseconds / 1000) ? "ok" : "unknown control")
                          << std::endl;
            } else {
                std::cerr << "[Boot] unknown command: " << command << std::endl;
            }
        }
    }).detach();
}
// The shaders earlier runs of the title asked for that the shader cache lacks are made
// before it starts, on most of the processor, with a window that says how far that is and
// lets the title start before it is done; the rest are made behind it. XV_PRECOMPILE=0
// starts the title at once; XV_PRECOMPILE_THREADS says on how many threads.
void precompile(const std::filesystem::path &cache, const std::filesystem::path &journal,
                const std::string &name) {
    runtime::modules::set_shader_journal(journal);
    if (const auto *const text = std::getenv("XV_PRECOMPILE"); text && *text == '0')
        return;
    const auto cores = std::max(std::thread::hardware_concurrency(), 2u);
    const auto *const asked = std::getenv("XV_PRECOMPILE_THREADS");
    const auto threads =
        asked ? static_cast<unsigned>(std::max(std::atoi(asked), 1)) : std::min(cores - 1, 8u);
    const auto started = std::chrono::steady_clock::now();
    const auto work = runtime::modules::precompile_shaders(cache, journal, threads);
    // a journal its index knows is looked at in a moment, and needs no window
    for (int waited = 0; !work->scanned && waited < 5; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (work->finished)
        return;
    // what the window says goes to the log too, every few seconds
    bool told = false;
    auto said = started;
    const auto say = [&](const std::string &text) {
        if (const auto now = std::chrono::steady_clock::now();
            now - said >= std::chrono::seconds(5)) {
            said = now;
            std::cerr << "[Boot] " << text << std::endl;
        }
    };
    {
        surface::ProgressWindow window(L"XValkyrie", L"Preparing shaders for " + wide(name));
        while (!work->finished && !window.skipped()) {
            if (!work->scanned) {
                const auto text = std::to_string(work->looked.load()) + " of " +
                                  std::to_string(work->journal.load()) +
                                  " shaders of earlier runs looked at";
                window.update(work->looked, work->journal, wide(text));
                say(text);
            } else {
                if (!told) {
                    told = true;
                    std::cerr << "[Boot] " << work->total
                              << " shaders of earlier runs to make for the host, on " << threads
                              << " threads" << std::endl;
                }
                const auto text = std::to_string(work->done.load()) + " of " +
                                  std::to_string(work->total.load()) + " shaders (" +
                                  std::to_string(work->failed.load()) + " do not translate yet)";
                window.update(work->done, work->total, wide(text));
                say(text);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (window.skipped())
            std::cerr << "[Boot] the title starts now; the other shaders are made behind it"
                      << std::endl;
    }
    std::cerr << "[Boot] shaders: " << work->made << " made, " << work->failed
              << " that do not translate, in "
              << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                  started)
                     .count()
              << " s" << std::endl;
}
// a title's folder in the shader cache: by its title ID, or by its name's characters
std::string journal_name(const std::string &id, const std::string &name) {
    std::string folder = id.empty() ? name : id;
    for (auto &c : folder)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.')
            c = '_';
    return folder.empty() ? "title" : folder;
}
} // namespace

std::uint32_t run(const std::filesystem::path &path, runtime::ExecuteOptions options) {
    const auto file = title_executable(path);
    const auto bytes = host::read_snapshot(file, 256 * 1024 * 1024);
    options.save_directory = title_save_directory(path);
    options.shader_cache_directory = cache_directory() / "shaders";
    options.console = console_settings();
    std::cerr << "[Boot] a Series title is told it runs on an "
              << model_name(options.console.series_model) << ", an Xbox One one on an "
              << model_name(options.console.one_model) << std::endl;
    // the title by its name, and by its title ID where it has one
    const auto id = title_id(path);
    const auto name = title_name(path) + (id.empty() ? "" : " [" + id + "]");
    std::wstring console;
    if (const auto target = runtime::title_target(bytes))
        console = wide(std::string(model_short_name(
            model_for(*target, *target == Target::scarlett ? options.console.series_model
                                                           : options.console.one_model))));
    surface::set_title_caption(Caption{wide(name), console});
    if (const auto icon = title_icon(path); !icon.empty())
        surface::set_title_icon(icon);
    surface::use_exclusive_fullscreen(graphics::fullscreen_mode() == graphics::Fullscreen::native);
    // Closing its window ends the title. One that notices leaves by itself, with its own
    // exit code; the others, which is most of them, are ended a moment later.
    surface::on_title_closed([] {
        std::cerr << "[Boot] the title's window was closed" << std::endl;
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::cerr << "[Boot] the title is still running: ending it" << std::endl;
            host::end_now(0);
        }).detach();
    });
    read_commands();
    std::cerr << "[Boot] " << name << std::endl;
    precompile(options.shader_cache_directory,
               options.shader_cache_directory / "titles" / journal_name(id, title_name(path)),
               name);
    std::cerr << "[Boot] starting " << app::text(file) << ", saves in "
              << app::text(options.save_directory) << std::endl;
    return runtime::execute(file, bytes, runtime::title_registry(bytes), options);
}
} // namespace xv::bootstrap
