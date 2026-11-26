#include "launcher/debug.hpp"
#include "launcher/config.hpp"
#include "launcher/launch.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xv {
namespace {
using Kind = DebugOption::Kind;
constexpr const char *graphics = "Graphics", *shaders = "Shaders", *gpu = "GPU diagnostics",
                     *traces = "Traces", *title = "Title";
constexpr DebugOption options[] = {
    // what is drawn
    {"skip_draws", graphics, "Skip draws",
     "Draws (DrawInstanced, DrawIndexedInstanced) do nothing.", Kind::flag, "XV_D3D_NO_WORK", "",
     'd'},
    {"skip_mesh", graphics, "Skip mesh dispatches", "DispatchMesh does nothing.", Kind::flag,
     "XV_D3D_NO_WORK", "", 'm'},
    {"skip_indirect", graphics, "Skip indirect executions",
     "ExecuteIndirect does nothing: neither its draws nor its dispatches.", Kind::flag,
     "XV_D3D_NO_WORK", "", 'i'},
    {"skip_dispatch", graphics, "Skip compute dispatches",
     "Dispatch does nothing. Titles that wait for what their compute shaders make may stop.",
     Kind::flag, "XV_D3D_NO_WORK", "", 'c'},
    {"no_texture_uploads", graphics, "Leave textures empty",
     "What the title writes into its textures is not copied to the GPU: less work and memory "
     "traffic, nothing to see in them.",
     Kind::flag, "XV_D3D_NO_UPLOADS"},
    {"no_queries", graphics, "No queries", "BeginQuery, EndQuery and ResolveQueryData do nothing.",
     Kind::flag, "XV_D3D_NO_QUERIES"},
    {"no_indirect_clamp", graphics, "Indirect records as they are",
     "The records of ExecuteIndirect are not copied and bounded on the GPU first.", Kind::flag,
     "XV_D3D_INDIRECT_CLAMP", "0"},
    {"sync_submissions", graphics, "Wait for every submission",
     "The GPU finishes each submission before the title goes on (slow).", Kind::flag,
     "XV_D3D_SYNC"},
    {"slow_submissions", graphics, "Delay each submission",
     "Milliseconds the CPU sleeps before each ExecuteCommandLists.", Kind::number, "XV_D3D_SLOW"},
    {"pipeline_cache", graphics, "Title's cached pipelines",
     "A Series title is given the pipeline cache it asks for, whose pipelines draw nothing.",
     Kind::flag, "XV_PSO_CACHE"},
    {"pipelines_at_once", graphics, "Make pipelines at once", "Not when a list first uses them.",
     Kind::flag, "XV_PSO_DEFER", "0"},
    // what the shaders are made of
    {"stand_in_vertex", shaders, "Stand-ins for vertex shaders",
     "Shaders that draw nothing in place of the translated ones.", Kind::flag,
     "XV_SHADER_STAND_INS", "", 'v', true},
    {"stand_in_pixel", shaders, "Stand-ins for pixel shaders", nullptr, Kind::flag,
     "XV_SHADER_STAND_INS", "", 'p', true},
    {"stand_in_compute", shaders, "Stand-ins for compute shaders", nullptr, Kind::flag,
     "XV_SHADER_STAND_INS", "", 'c', true},
    {"stand_in_mesh", shaders, "Stand-ins for mesh shaders", nullptr, Kind::flag,
     "XV_SHADER_STAND_INS", "", 'm', true},
    {"solid_pixels", shaders, "Solid pixel shaders",
     "Every translated pixel shader writes magenta, to see what gets drawn at all.", Kind::flag,
     "XV_SHADER_SOLID"},
    {"no_heap_shaders", shaders, "Title's root signatures",
     "Series shaders keep the title's tables instead of reading the descriptor heaps "
     "themselves.",
     Kind::flag, "XV_HEAP_SHADERS", "0"},
    {"cached_programs_stand_in", shaders, "Stand-ins for cached programs",
     "The pipelines of an Xbox One title's cached programs draw nothing.", Kind::flag,
     "XV_PSO_STAND_IN"},
    {"no_precompile", shaders, "Start without preparing shaders",
     "The shaders of earlier runs are made while the title runs, not before.", Kind::flag,
     "XV_PRECOMPILE", "0"},
    {"precompile_threads", shaders, "Threads for preparing shaders",
     "How many; most of the processor, if not set.", Kind::number, "XV_PRECOMPILE_THREADS"},
    {"compile_seconds", shaders, "Compiler time limit",
     "Seconds the host's compiler is given for a shader before it is left for the next run "
     "(20).",
     Kind::number, "XV_SHADER_SECONDS"},
    {"older_compiler", shaders, "Older compiler (FXC)", "Not DXC, for Shader Model 5 shaders.",
     Kind::flag, "XV_SHADER_FXC"},
    {"shader_why", shaders, "Remake and explain every shader",
     "The shader cache is not read, and the log counts what stops each translation (slow).",
     Kind::flag, "XV_SHADER_WHY"},
    {"shader_code", shaders, "Keep the shaders in",
     "A folder for the code, the HLSL and the root signature of every shader met.", Kind::text,
     "XV_SHADER_CODE"},
    // what the GPU did
    {"d3d_debug", gpu, "Direct3D debug layer", "The host's checks of every call (slow).",
     Kind::flag, "XV_D3D_DEBUG"},
    {"dred", gpu, "Device removed data (DRED)",
     "What the GPU was doing, and in which of the title's shaders, when the device is lost.",
     Kind::flag, "XV_D3D_DRED"},
    {"lost_checks", gpu, "Look for a lost device",
     "After every call, so that the log says which one lost it (slow).", Kind::flag, "XV_D3D_LOST"},
    {"capture_frame", gpu, "Capture a frame",
     "The frame's draws and targets, as pictures, at this frame number.", Kind::number,
     "XV_CAPTURE"},
    {"capture_folder", gpu, "Capture folder", "Where the captured pictures go.", Kind::text,
     "XV_CAPTURE_DIR"},
    // what the log says
    {"d3d_trace", traces, "Direct3D calls", "The first calls of each kind.", Kind::flag,
     "XV_D3D_TRACE"},
    {"frame_trace", traces, "Frame pacing", "How the title paces its frames.", Kind::flag,
     "XV_FRAME_TRACE"},
    {"pipeline_time", traces, "Pipeline times", "How long each pipeline takes to make.", Kind::flag,
     "XV_PSO_TIME"},
    {"file_trace", traces, "File reads", "Reads at an offset, DirectStorage requests, failures.",
     Kind::flag, "XV_FILE_TRACE"},
    {"storage_time", traces, "DirectStorage timing", "When the first requests come, and where.",
     Kind::flag, "XV_DS_TIME"},
    {"memory_trace", traces, "Memory", "What the title asks of VirtualAlloc.", Kind::flag,
     "XV_MEM_TRACE"},
    {"memory_check", traces, "Memory mapping checks",
     "A page mapped twice, or given back while mapped.", Kind::flag, "XV_MEM_CHECK"},
    {"thread_trace", traces, "Threads", "The threads the title makes.", Kind::flag,
     "XV_THREAD_TRACE"},
    {"com_trace", traces, "COM", "The objects the title makes.", Kind::flag, "XV_COM_TRACE"},
    {"first_chance", traces, "Every exception",
     "First-chance exceptions too, not just those nothing handles.", Kind::flag, "XV_FIRST_CHANCE"},
    {"watchdog", traces, "Stalls", "Every thread's stack once frames stop for 5 seconds.",
     Kind::flag, "XV_WATCHDOG"},
    {"thread_dump", traces, "Stacks at a frame", "Every thread's stack once, at this frame.",
     Kind::number, "XV_THREAD_DUMP"},
    {"profile", traces, "Profile", "Samples the stacks of the threads that run every so many ms.",
     Kind::number, "XV_PROFILE"},
    // the title itself
    {"mute", title, "Mute", "The title plays no sound.", Kind::flag, "XV_MUTE"},
    {"no_network", title, "No network",
     "The title's sockets and WinHTTP fail as with no connection.", Kind::flag, "XV_NETWORK", "0"},
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}
const DebugOption *find_option(const std::string &key) {
    for (const auto &option : options)
        if (key == option.key)
            return &option;
    return nullptr;
}
// whether a value is one its option takes
bool valid(const DebugOption &option, const std::string &value) {
    if (option.kind == Kind::flag)
        return value == "true" || value == "false";
    if (option.kind == Kind::number)
        return !value.empty() && value.size() <= 9 &&
               std::all_of(value.begin(), value.end(),
                           [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    return value.find_first_of("\r\n") == std::string::npos;
}
// The [debug] section of a file, and the stand-ins of its [shaders]. What is not known or
// not valid is left out.
void read_file(const std::filesystem::path &path, DebugValues &values,
               std::set<std::string> *stand_ins) {
    std::ifstream file(path);
    std::string line, section;
    while (file && std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos)
            continue;
        const auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
        if (section == "debug") {
            if (const auto *const option = find_option(key); option && valid(*option, value))
                values[key] = value;
        } else if (section == "shaders" && key == "stand_in" && stand_ins && value.size() >= 3 &&
                   value[value.size() - 2] == '-') {
            stand_ins->insert(value);
        }
    }
}
void write_file(const std::filesystem::path &path, const DebugValues &values,
                const std::set<std::string> *stand_ins) {
    std::filesystem::create_directories(path.parent_path());
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        file << "[debug]\n";
        for (const auto &option : options)
            if (const auto found = values.find(option.key);
                found != values.end() && valid(option, found->second))
                file << option.key << " = " << found->second << '\n';
        if (stand_ins && !stand_ins->empty()) {
            file << "\n[shaders]\n";
            for (const auto &shader : *stand_ins)
                file << "stand_in = " << shader << '\n';
        }
        if (!file)
            throw std::runtime_error("Cannot write " + temp);
    }
    if (!MoveFileExW(std::filesystem::path(temp).c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace " + path.string());
}
std::filesystem::path debug_file() { return config_directory() / "debug.ini"; }
} // namespace

std::span<const DebugOption> debug_options() { return options; }

DebugValues load_debug_settings() {
    DebugValues values;
    read_file(debug_file(), values, nullptr);
    return values;
}
void save_debug_settings(const DebugValues &values) { write_file(debug_file(), values, nullptr); }

std::string title_folder_name(const std::filesystem::path &path) {
    const auto id = title_id(path);
    std::string folder = id.empty() ? title_name(path) : id;
    for (auto &c : folder)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.')
            c = '_';
    return folder.empty() ? "title" : folder;
}
std::filesystem::path title_settings_file(const std::filesystem::path &path) {
    return config_directory() / "titles" / (title_folder_name(path) + ".ini");
}
TitleDebugSettings load_title_debug_settings(const std::filesystem::path &path) {
    TitleDebugSettings settings;
    read_file(title_settings_file(path), settings.debug, &settings.stand_ins);
    return settings;
}
void save_title_debug_settings(const std::filesystem::path &path,
                               const TitleDebugSettings &settings) {
    write_file(title_settings_file(path), settings.debug, &settings.stand_ins);
}
std::filesystem::path shader_cache_folder() { return cache_directory() / "shaders"; }
std::filesystem::path title_shader_journal(const std::filesystem::path &path) {
    return shader_cache_folder() / "titles" / title_folder_name(path);
}

std::vector<std::pair<std::string, std::string>>
debug_environment(const DebugValues &all, const TitleDebugSettings &title) {
    std::vector<std::pair<std::string, std::string>> environment;
    const auto value_of = [&](const DebugOption &option) -> const std::string * {
        if (const auto found = title.debug.find(option.key); found != title.debug.end())
            return &found->second;
        if (const auto found = all.find(option.key); found != all.end())
            return &found->second;
        return nullptr;
    };
    std::map<std::string, std::string> lists; // of letters, by variable
    std::set<std::string> listed;             // variables one of whose letters is on
    for (const auto &option : options) {
        const auto *const value = value_of(option);
        const bool on = value && *value == "true";
        if (option.letter) {
            auto &letters = lists[option.variable];
            if (on != option.inverse)
                letters += option.letter;
            if (on)
                listed.insert(option.variable);
        } else if (option.kind == Kind::flag) {
            if (on)
                environment.emplace_back(option.variable, option.value);
        } else if (value && !value->empty()) {
            environment.emplace_back(option.variable, *value);
        }
    }
    // a list of the stages still translated may have none of them: "-" says so
    for (const auto &variable : listed)
        environment.emplace_back(variable, lists[variable].empty() ? "-" : lists[variable]);
    if (!title.stand_ins.empty()) {
        std::string text;
        for (const auto &shader : title.stand_ins)
            text += (text.empty() ? "" : ",") + shader;
        environment.emplace_back("XV_SHADER_FORCE", text);
    }
    return environment;
}

std::vector<std::string> apply_debug_environment(const std::filesystem::path &path) {
    std::vector<std::string> applied;
    TitleDebugSettings title;
    try {
        title = load_title_debug_settings(path);
    } catch (const std::exception &) {
    }
    for (const auto &[variable, value] : debug_environment(load_debug_settings(), title)) {
        if (std::getenv(variable.c_str()))
            continue;
        if (_putenv_s(variable.c_str(), value.c_str()) == 0)
            applied.push_back(variable + "=" + value);
    }
    return applied;
}
} // namespace xv
