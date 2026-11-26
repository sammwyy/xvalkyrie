#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xv {
// Settings for looking into how a title runs: each one of the XV_* variables the runtime reads,
// with a name and a word on what it does. They are saved in debug.ini, for every title, and in
// a title's own file, where a setting of the title's takes the place of the one for all.
// xvboot gives them to the runtime as its environment; one that is already there is kept.
struct DebugOption {
    enum class Kind { flag, number, text };
    const char *key;   // in the files
    const char *group; // the heading it is shown under
    const char *name, *hint;
    Kind kind;
    const char *variable; // XV_...
    // A flag sets the variable to `value`. Flags that share a variable with a `letter` each
    // make it a list of the letters of those on, or with `inverse` of those off.
    const char *value = "1";
    char letter = 0;
    bool inverse = false;
};
std::span<const DebugOption> debug_options();
// by key, the value as typed: "true" or "false" for a flag, digits for a number; what is not
// there is not set
using DebugValues = std::map<std::string, std::string>;
DebugValues load_debug_settings();
void save_debug_settings(const DebugValues &values);

// What a title has of its own: settings in place of those for all, and the shaders it is to
// get a stand-in for, as "<hash>-<stage letter>".
struct TitleDebugSettings {
    DebugValues debug;
    std::set<std::string> stand_ins;
};
// the name of a title's folders and files of its own: its title ID, or its name
std::string title_folder_name(const std::filesystem::path &title);
std::filesystem::path title_settings_file(const std::filesystem::path &title);
TitleDebugSettings load_title_debug_settings(const std::filesystem::path &title);
void save_title_debug_settings(const std::filesystem::path &title,
                               const TitleDebugSettings &settings);
// where the shaders a title asked for are noted, in the shader cache (see precompile_shaders)
std::filesystem::path shader_cache_folder();
std::filesystem::path title_shader_journal(const std::filesystem::path &title);

// the variables those settings make, the title's over the others
std::vector<std::pair<std::string, std::string>> debug_environment(const DebugValues &all,
                                                                   const TitleDebugSettings &title);
// Sets them in this process for the title, but those its environment has already. Returns
// what was set, as NAME=value.
std::vector<std::string> apply_debug_environment(const std::filesystem::path &title);
} // namespace xv
