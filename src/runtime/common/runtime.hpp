#pragma once
#include "core/target.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace xv::runtime {
// exit codes produced by the runtime, not by the guest
inline constexpr std::uint32_t exit_unimplemented_import = 0xE0585301;
inline constexpr std::uint32_t exit_guest_fault = 0xE0585302;

// Our implementations of guest imports. Module names are case-insensitive,
// symbols are "Name" or "#ordinal". Functions must use the MS x64 ABI.
class Registry {
  public:
    void add(std::string_view module, std::string_view symbol, void *function);
    void *find(std::string_view module, std::string_view symbol) const;
    // A function of the host's API the console answers differently, whichever API set or
    // system DLL the title imports it from. A module's own entry wins over it.
    void add_api(std::string_view symbol, void *function);
    void *find_api(std::string_view symbol) const;
    bool has(std::string_view module) const;
    // Every entry `module` has so far under the name `other` as well
    void alias(std::string_view other, std::string_view module);
    // Lets every import of `module` that has no entry here go to the export of
    // `host_module` with the same name or ordinal, whatever the host policy says.
    void forward(std::string_view module, std::string_view host_module);
    // lowercase host module `module` is forwarded to, empty if it is not
    std::string_view forwarded(std::string_view module) const;
    // Lets every import nothing here answers go to the host module it names, whatever the
    // host policy says: for a Windows app, which is not held to the console's list of APIs.
    void open_to_host() { open_ = true; }
    bool open() const { return open_; }
    // What is in it, without the functions: the symbols of each module, the modules that go
    // to the host and the functions answered whatever they are imported from.
    nlohmann::json describe() const;

  private:
    std::map<std::pair<std::string, std::string>, void *> entries_;
    std::map<std::string, std::string> forwards_;
    std::map<std::string, void *, std::less<>> apis_;
    bool open_ = false;
};

// only true on Windows x64
// Where each import would end up (own / host / title / system / missing). Does not run
// anything. "title" is a DLL in `directory`, the folder of the executable, that exports the
// symbol; without a directory there are no such DLLs. "system" is a console system module
// in `system_directory` (see console_system_directory), which execute() loads first.
nlohmann::json bind_plan(std::span<const std::uint8_t> file, const Registry &registry,
                         const std::filesystem::path &directory = {},
                         const std::filesystem::path &system_directory = {});
// Options > Console
struct ConsoleSettings {
    std::wstring gamertag = L"Player";
    std::wstring xbox_user_id = L"2814600000000001";
    std::uint32_t gamerscore = 0;
    std::uint32_t age_group = 3; // UserAgeGroup: unknown, child, teen, adult
    bool headset = false;
    bool keyboard_dialog = true; // otherwise the keyboard returns the default text
    // the console's language and region as a locale name; empty for the host user's
    std::wstring language{};
    // The console's system apps, each unpacked in a folder of its own; empty if there are
    // none. The packages a title depends on are looked for there as well as next to it.
    std::filesystem::path system_storage{};
    // the console a title is told it runs on, by its generation
    ConsoleModel series_model = default_model(Target::scarlett);
    ConsoleModel one_model = default_model(Target::durango);
};
struct ExecuteOptions {
    // start even if some imports can't be resolved; calling one of them ends the process
    bool allow_missing = false;
    // report the first call of every imported function on stderr, and the last calls
    // before a fault
    bool trace = false;
    // no sound from the title
    bool mute = false;
    // host folder where the title's saved games go, see save_directory()
    std::filesystem::path save_directory{};
    // host folder for the shaders made for the host from console ones; none are kept if empty
    std::filesystem::path shader_cache_directory{};
    ConsoleSettings console{};
    // the console's System32 the title shipped with; found next to the package when empty
    std::filesystem::path system_directory{};
};
// <package>\EmbeddedXvd\Windows\System32, next to the folder with the title's manifest or
// one up from it; empty if there is none
std::filesystem::path console_system_directory(const std::filesystem::path &executable);
// Modules of the console's system that are only computation (texture layouts, the shader
// compiler's back end), taken from that folder instead of ours or the host's when it has them
bool console_system_module(std::string_view module);
// Maps and runs the image, returns its exit code. No sandbox!
// A guest that faults ends the process with exit_guest_fault.
std::uint32_t execute(const std::filesystem::path &path, std::span<const std::uint8_t> file,
                      const Registry &registry, const ExecuteOptions &options = {});

// The package a title's executable belongs to: the nearest folder at or above `directory`
// with an AppxManifest.xml, `directory` itself if there is none. A title's own DLLs are
// looked for from the executable's folder up to that one.
std::filesystem::path package_directory(const std::filesystem::path &directory);

// The folder of the running title's saved games, from ExecuteOptions; empty before execute
// or when none was given.
const std::filesystem::path &save_directory();
// The folder of the shader cache, from ExecuteOptions; empty when none was given.
const std::filesystem::path &shader_cache_directory();
const ConsoleSettings &console_settings();
// the console the running title is told it is: the model of its generation in the settings
ConsoleModel console_model();
// what a title has of the memory of each console: 13 of a Series X's 16 GiB (13.5 there,
// rounded down), 8 of a Series S's 10, 9 of a One X's 12, 5 of the others' 8
constexpr std::uint64_t title_memory(ConsoleModel model) {
    constexpr std::uint64_t gib[]{5, 5, 9, 8, 13}; // as ConsoleModel
    return gib[static_cast<int>(model)] << 30;
}

namespace detail {
void set_save_directory(std::filesystem::path directory);
void set_shader_cache_directory(std::filesystem::path directory);
void set_console_settings(ConsoleSettings settings);
// the generation the title is of, which its modules are installed for
void set_console_target(Target target);
struct Binding {
    void *function;     // null if missing
    const char *origin; // "own", "host" or "missing"
};
// The one place that decides where an import goes.
Binding resolve(const Registry &registry, std::string_view module, std::string_view symbol);
// replacement for a host function that cannot be used as is inside a guest
void *runtime_override(std::string_view symbol);
// export of a system DLL, by "Name" or "#ordinal"
void *host_export(std::string_view module, std::string_view symbol);
// Whether the import is one the title's own DLL of that name provides: a DLL in
// `directory` that is neither ours nor forwarded and exports the symbol. Never loads it.
// The title's own DLL `module` for an executable in `directory`, empty if it has none
std::filesystem::path title_dll(const std::filesystem::path &directory, std::string_view module);
bool title_provides(const Registry &registry, const std::filesystem::path &directory,
                    std::string_view module, std::string_view symbol);
// offset of an address inside the running guest image, 0 if it is not in it
std::uint64_t guest_offset(const void *address);
bool in_title(const void *address);
// "image+0x...", "<dll>+0x..." or the address
// the base of the title's image or DLL `address` is in, null for anything else
void *title_image_base(const void *address);
std::string guest_location(const void *address);
// XV_D3D_LOST: where every other thread is, by its return addresses
void report_thread_stacks(std::uint64_t own_stack = 0);
// an export of a console system module from the title's system folder, mapping it on first
// use; null without that folder or module
void *system_export(std::string_view module, std::string_view symbol);
// maps the title's DLL and runs its DllMain on first use
void *title_export(std::string_view module, std::string_view symbol);
// LoadLibrary and GetProcAddress for modules of ours and of the title; null for the host's
void *guest_library(const wchar_t *path);
void *guest_symbol(void *module, const char *symbol);
bool host_policy_allows(std::string_view module, std::string_view symbol);
void set_fatal_cleanup(void (*fn)());
} // namespace detail
} // namespace xv::runtime
