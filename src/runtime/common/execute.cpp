#include "backends/audio/audio.hpp"
#include "formats/pe/inspect.hpp"
#include "host/files.hpp"
#include "host/threading.hpp"
#include "loader/image.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/common/static_tls.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include <tlhelp32.h> // after windows.h

// One guest per process, running on its own thread. Same ABI as the host,
// so an import is just a function pointer.
namespace xv::runtime {
namespace {
// A DLL shipped with the title, loaded from its folder the same way as the executable.
struct TitleModule {
    std::string name; // lowercase file name
    std::wstring path;
    std::unique_ptr<loader::MappedImage> image;
    std::uint64_t base = 0, size = 0, entry = 0;
    std::map<std::string, std::uint64_t> exports; // "Name" or "#ordinal" -> address
    PRUNTIME_FUNCTION functions = nullptr;
    DWORD function_count = 0;
};
using Resolver = std::function<std::optional<std::uint64_t>(std::string_view, std::string_view)>;
struct Guest {
    std::uint64_t base = 0, size = 0;
    std::wstring path;
    std::filesystem::path directory;
    std::deque<TitleModule> modules; // dependencies before what needs them
    Resolver resolve;                // imports of the title's DLLs loaded while it runs
    const Registry *registry = nullptr;
    std::filesystem::path system_directory; // the console's System32 the title shipped with
    std::map<std::string, std::unique_ptr<std::string>> own_handles; // LoadLibrary of ours
    std::recursive_mutex loading;
    std::vector<std::string> missing; // "module!symbol", indexed by stub number
    std::uint8_t *stubs = nullptr;
    // --trace: one thunk per imported function, right after the stubs
    std::vector<std::string> traced; // "module!symbol", indexed by thunk number
    std::unique_ptr<std::atomic<std::uint64_t>[]> calls;
    std::atomic<std::uint64_t> sequence{0};
    std::array<std::atomic<std::uint32_t>, 64> recent{}; // thunk numbers, by sequence
};
Guest *guest = nullptr;
constexpr std::size_t stub_size = 32, stub_limit = 16384, trace_size = 128, trace_limit = 16384;

bool is_guest(const void *address) {
    const auto value = reinterpret_cast<std::uint64_t>(address);
    return value >= guest->base && value - guest->base < guest->size;
}
HMODULE guest_module() { return reinterpret_cast<HMODULE>(guest->base); }
const TitleModule *module_at(const void *address) {
    const auto value = reinterpret_cast<std::uint64_t>(address);
    const std::lock_guard lock(guest->loading);
    for (const auto &module : guest->modules)
        if (value >= module.base && value - module.base < module.size)
            return &module;
    return nullptr;
}
// Path of the guest image or of one of the title's DLLs, null for a module of the host
const std::wstring *guest_path(HMODULE module) {
    if (!module || module == guest_module())
        return &guest->path;
    const auto *title = module_at(module);
    return title && title->base == reinterpret_cast<std::uint64_t>(module) ? &title->path : nullptr;
}

// The process is xvalkyrie.exe, but the guest expects to be the main module.
HMODULE WINAPI get_module_handle_w(LPCWSTR name) {
    return name ? GetModuleHandleW(name) : guest_module();
}
HMODULE WINAPI get_module_handle_a(LPCSTR name) {
    return name ? GetModuleHandleA(name) : guest_module();
}
BOOL WINAPI get_module_handle_ex_w(DWORD flags, LPCWSTR name, HMODULE *module) {
    const bool by_address = flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    if (module && (by_address ? is_guest(name) : !name)) {
        *module = guest_module();
        return TRUE;
    }
    if (const auto *title = module && by_address ? module_at(name) : nullptr) {
        *module = reinterpret_cast<HMODULE>(title->base);
        return TRUE;
    }
    return GetModuleHandleExW(flags, name, module);
}
DWORD WINAPI get_module_file_name_w(HMODULE module, LPWSTR buffer, DWORD size) {
    const auto *path = guest_path(module);
    if (!path)
        return GetModuleFileNameW(module, buffer, size);
    if (!buffer || !size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const auto count = std::min<std::size_t>(path->size(), size - 1);
    std::memcpy(buffer, path->data(), count * sizeof(wchar_t));
    buffer[count] = 0;
    SetLastError(count < path->size() ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
    return count < path->size() ? size : static_cast<DWORD>(count);
}
DWORD WINAPI get_module_file_name_a(HMODULE module, LPSTR buffer, DWORD size) {
    const auto *path = guest_path(module);
    if (!path)
        return GetModuleFileNameA(module, buffer, size);
    if (!buffer || !size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const auto needed =
        WideCharToMultiByte(CP_ACP, 0, path->c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> text(static_cast<std::size_t>(std::max(needed, 1)));
    WideCharToMultiByte(CP_ACP, 0, path->c_str(), -1, text.data(), needed, nullptr, nullptr);
    const auto length = std::strlen(text.data()), count = std::min<std::size_t>(length, size - 1);
    std::memcpy(buffer, text.data(), count);
    buffer[count] = 0;
    SetLastError(count < length ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
    return count < length ? size : static_cast<DWORD>(count);
}
// vcruntime uses RtlPcToFileHeader to find the image base of the thrown type, which
// fails for a manually mapped image and breaks every catch. Raise it ourselves.
[[noreturn]] void WINAPI cxx_throw_exception(void *object, void *throw_info) {
    void *base = nullptr;
    if (throw_info && is_guest(throw_info))
        base = guest_module();
    else if (const auto *title = throw_info ? module_at(throw_info) : nullptr)
        base = reinterpret_cast<void *>(title->base);
    else if (throw_info)
        RtlPcToFileHeader(throw_info, &base);
    std::uint32_t attributes = 0;
    if (throw_info)
        std::memcpy(&attributes, throw_info, 4);
    const ULONG_PTR parameters[4] = {
        attributes & 8 ? 0x01994000u : 0x19930520u, reinterpret_cast<ULONG_PTR>(object),
        reinterpret_cast<ULONG_PTR>(throw_info), reinterpret_cast<ULONG_PTR>(base)};
    RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 4, parameters);
    std::abort();
}
// FreeLibraryWhenCallbackReturns with a module of the title's: the C++ runtime holds its own
// DLL while a thread pool callback runs and lets go of it this way. The host's loader does
// not know that module and raises when the callback returns; there is nothing to free.
void WINAPI free_library_when_callback_returns(PTP_CALLBACK_INSTANCE instance, HMODULE module) {
    if (module && (is_guest(module) || module_at(module)))
        return;
    FreeLibraryWhenCallbackReturns(instance, module);
}
struct Override {
    std::string_view symbol;
    void *function;
};
// The command line is xvalkyrie's; the title gets one of its own with nothing but its path,
// as when the console starts it. Built once, the first time it is asked for.
struct CommandLine {
    std::wstring wide;
    std::string narrow;
    std::vector<wchar_t *> wide_argv;
    std::vector<char *> narrow_argv;
    int argc = 1;
};
CommandLine &command_line() {
    static CommandLine line = [] {
        CommandLine result;
        result.wide = L"\"" + guest->path + L"\"";
        const int size =
            WideCharToMultiByte(CP_ACP, 0, result.wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        result.narrow.resize(static_cast<std::size_t>(std::max(size, 1)));
        WideCharToMultiByte(CP_ACP, 0, result.wide.c_str(), -1, result.narrow.data(), size, nullptr,
                            nullptr);
        result.narrow.resize(std::strlen(result.narrow.c_str()));
        static std::wstring wide_path;
        static std::string narrow_path;
        wide_path = guest->path;
        narrow_path = result.narrow.substr(1, result.narrow.size() - 2);
        result.wide_argv = {wide_path.data(), nullptr};
        result.narrow_argv = {narrow_path.data(), nullptr};
        return result;
    }();
    return line;
}
LPWSTR WINAPI get_command_line_w() { return command_line().wide.data(); }
LPSTR WINAPI get_command_line_a() { return command_line().narrow.data(); }
int *__cdecl argc_pointer() { return &command_line().argc; }
char ***__cdecl argv_pointer() {
    static char **argv = command_line().narrow_argv.data();
    return &argv;
}
wchar_t ***__cdecl wargv_pointer() {
    static wchar_t **argv = command_line().wide_argv.data();
    return &argv;
}
// vccorlib's Platform::Details::GetCmdArguments, how C++/CX titles read their arguments
wchar_t **__cdecl cmd_arguments(int *argc) {
    if (argc)
        *argc = command_line().argc;
    return command_line().wide_argv.data();
}
// The title's own filter for unhandled exceptions, which runs after ours has reported them
LPTOP_LEVEL_EXCEPTION_FILTER title_filter = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
set_unhandled_exception_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    return std::exchange(title_filter, filter);
}
// stdout and stderr are the session log's, the title's own included
FILE *__cdecl wfreopen(const wchar_t *path, const wchar_t *mode, FILE *stream) {
    if (stream != stdout && stream != stderr)
        return _wfreopen(path, mode, stream);
    std::fprintf(stderr, "[Runtime] the title sends std%s to %ls, kept in the log\n",
                 stream == stdout ? "out" : "err", path ? path : L"(null)");
    return stream;
}
FILE *__cdecl freopen_narrow(const char *path, const char *mode, FILE *stream) {
    if (stream != stdout && stream != stderr)
        return std::freopen(path, mode, stream);
    std::fprintf(stderr, "[Runtime] the title sends std%s to %s, kept in the log\n",
                 stream == stdout ? "out" : "err", path ? path : "(null)");
    return stream;
}
template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
// ResolveDelayLoadedAPI: an import a module of the title's left for its first use. The
// host's does not know the title's modules; the import is bound the way the others are.
void *WINAPI resolve_delay_loaded(void *module, const IMAGE_DELAYLOAD_DESCRIPTOR *descriptor,
                                  void *, void *, IMAGE_THUNK_DATA *thunk, ULONG) {
    if (!guest || !module || !descriptor || !thunk)
        return nullptr;
    const auto *const base = static_cast<const std::uint8_t *>(module);
    const auto *const table =
        reinterpret_cast<const IMAGE_THUNK_DATA *>(base + descriptor->ImportAddressTableRVA);
    const auto *const names =
        reinterpret_cast<const IMAGE_THUNK_DATA *>(base + descriptor->ImportNameTableRVA);
    const auto &entry = names[thunk - table];
    const std::string library = reinterpret_cast<const char *>(base + descriptor->DllNameRVA);
    const std::string symbol =
        IMAGE_SNAP_BY_ORDINAL(entry.u1.Ordinal)
            ? "#" + std::to_string(IMAGE_ORDINAL(entry.u1.Ordinal))
            : reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + entry.u1.AddressOfData)->Name;
    std::optional<std::uint64_t> found;
    {
        // binding may make a stub, and the stubs can only be written while a module loads
        const std::lock_guard lock(guest->loading);
        DWORD previous;
        VirtualProtect(guest->stubs, stub_size * stub_limit + trace_size * trace_limit,
                       PAGE_EXECUTE_READWRITE, &previous);
        found = guest->resolve(library, symbol);
        VirtualProtect(guest->stubs, stub_size * stub_limit + trace_size * trace_limit,
                       PAGE_EXECUTE_READ, &previous);
    }
    if (!found)
        return nullptr;
    DWORD old = 0;
    VirtualProtect(thunk, sizeof *thunk, PAGE_READWRITE, &old);
    thunk->u1.Function = *found;
    VirtualProtect(thunk, sizeof *thunk, old, &old);
    return reinterpret_cast<void *>(*found);
}
const Override overrides[] = {
    {"ResolveDelayLoadedAPI", erase(resolve_delay_loaded)},
    {"_wfreopen", erase(wfreopen)},
    {"freopen", erase(freopen_narrow)},
    {"GetModuleHandleW", erase(get_module_handle_w)},
    {"GetModuleHandleA", erase(get_module_handle_a)},
    {"GetModuleHandleExW", erase(get_module_handle_ex_w)},
    {"GetModuleFileNameW", erase(get_module_file_name_w)},
    {"GetModuleFileNameA", erase(get_module_file_name_a)},
    {"_CxxThrowException", erase(cxx_throw_exception)},
    {"FreeLibraryWhenCallbackReturns", erase(free_library_when_callback_returns)},
    {"GetCommandLineW", erase(get_command_line_w)},
    {"GetCommandLineA", erase(get_command_line_a)},
    {"__p___argc", erase(argc_pointer)},
    {"__p___argv", erase(argv_pointer)},
    {"__p___wargv", erase(wargv_pointer)},
    {"?GetCmdArguments@Details@Platform@@YAPEAPEA_WPEAH@Z", erase(cmd_arguments)},
    {"SetUnhandledExceptionFilter", erase(set_unhandled_exception_filter)}};

void report_recent_calls();
[[noreturn]] void unimplemented(std::uint32_t index) {
    std::fprintf(stderr, "[Runtime] title called unimplemented import %s\n",
                 guest->missing.at(index).c_str());
    report_recent_calls();
    std::fflush(nullptr);
    ExitProcess(exit_unimplemented_import);
}
// mov ecx, index
// mov rax, &unimplemented
// jmp rax
std::uint64_t make_stub(const std::string &name) {
    if (guest->missing.size() == stub_limit)
        throw std::runtime_error("Too many unimplemented imports");
    const auto index = static_cast<std::uint32_t>(guest->missing.size());
    guest->missing.push_back(name);
    auto *code = guest->stubs + index * stub_size;
    const auto target = reinterpret_cast<std::uint64_t>(&unimplemented);
    code[0] = 0xb9;
    std::memcpy(code + 1, &index, 4);
    code[5] = 0x48;
    code[6] = 0xb8;
    std::memcpy(code + 7, &target, 8);
    code[15] = 0xff;
    code[16] = 0xe0;
    return reinterpret_cast<std::uint64_t>(code);
}

// Called by a trace thunk before it jumps to the import. Must not change what the guest
// can observe, which here means the last error.
void trace_call(std::uint32_t index) {
    const auto error = GetLastError();
    guest->recent[guest->sequence++ % guest->recent.size()] = index;
    if (!guest->calls[index]++) {
        std::fprintf(stderr, "[Runtime] [%lu] %s\n", GetCurrentThreadId(),
                     guest->traced[index].c_str());
        std::fflush(stderr);
    }
    SetLastError(error);
}
// Saves the four argument registers of both kinds, calls trace_call(index) and jumps to
// the import with everything as the guest left it.
std::uint64_t make_trace(const std::string &name, void *target) {
    MEMORY_BASIC_INFORMATION memory{};
    const auto executable =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    // imported data must keep its address
    if (!VirtualQuery(target, &memory, sizeof memory) || !(memory.Protect & executable) ||
        guest->traced.size() == trace_limit)
        return reinterpret_cast<std::uint64_t>(target);
    const auto index = static_cast<std::uint32_t>(guest->traced.size());
    guest->traced.push_back(name);
    auto *code = guest->stubs + stub_size * stub_limit + index * trace_size;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        code = std::copy(bytes.begin(), bytes.end(), code);
    };
    const auto value = [&](const void *data, std::size_t size) {
        std::memcpy(code, data, size);
        code += size;
    };
    const auto logger = reinterpret_cast<std::uint64_t>(&trace_call);
    const auto destination = reinterpret_cast<std::uint64_t>(target);
    emit({0x51, 0x52, 0x41, 0x50, 0x41, 0x51}); // push rcx, rdx, r8, r9
    emit({0x48, 0x83, 0xec, 0x68});             // sub rsp, 0x68
    for (std::uint8_t i = 0; i < 4; ++i)        // movdqu [rsp+0x20+16*i], xmm<i>
        emit({0xf3, 0x0f, 0x7f, static_cast<std::uint8_t>(0x44 + 8 * i), 0x24,
              static_cast<std::uint8_t>(0x20 + 16 * i)});
    emit({0xb9}); // mov ecx, index
    value(&index, 4);
    emit({0x48, 0xb8}); // mov rax, &trace_call
    value(&logger, 8);
    emit({0xff, 0xd0});                  // call rax
    for (std::uint8_t i = 0; i < 4; ++i) // movdqu xmm<i>, [rsp+0x20+16*i]
        emit({0xf3, 0x0f, 0x6f, static_cast<std::uint8_t>(0x44 + 8 * i), 0x24,
              static_cast<std::uint8_t>(0x20 + 16 * i)});
    emit({0x48, 0x83, 0xc4, 0x68});             // add rsp, 0x68
    emit({0x41, 0x59, 0x41, 0x58, 0x5a, 0x59}); // pop r9, r8, rdx, rcx
    emit({0x48, 0xb8});                         // mov rax, target
    value(&destination, 8);
    emit({0xff, 0xe0}); // jmp rax
    return reinterpret_cast<std::uint64_t>(guest->stubs + stub_size * stub_limit +
                                           index * trace_size);
}
void report_recent_calls() {
    if (guest->traced.empty())
        return;
    const auto end = guest->sequence.load(), size = std::uint64_t{guest->recent.size()};
    std::fprintf(stderr, "[Runtime] last imports called, oldest first:\n");
    for (auto i = end > size ? end - size : 0; i < end; ++i)
        std::fprintf(stderr, "[Runtime]   %s\n", guest->traced[guest->recent[i % size]].c_str());
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto &c : result)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    return result;
}
// The title's own DLL of this name, loading it on first use. Null if the title has none.
// found by name in the title's folder unless `file_path` says where it is
const TitleModule *title_module(std::string_view file_name, const Resolver &resolve,
                                const std::filesystem::path &file_path = {}) {
    const auto name = lower(file_name);
    if (name.empty() || name.find_first_of("\\/:") != std::string::npos)
        return nullptr;
    for (const auto &module : guest->modules)
        if (module.name == name)
            return &module;
    const auto path = file_path.empty() ? detail::title_dll(guest->directory, name) : file_path;
    if (path.empty())
        return nullptr;
    const auto file = host::read_snapshot(path, 256 * 1024 * 1024);
    const auto pe = pe::inspect(file, pe::Detail::light);
    const auto &contents = pe.at("directory_contents");
    if (!(pe.at("characteristics").get<std::uint64_t>() & 0x2000))
        throw std::runtime_error(name + " is not a DLL");
    loader::MapOptions options;
    options.executable = true;
    options.resolve = resolve;
    // A module of the console's system is not held to the list of APIs a title may import:
    // what nothing of ours answers goes to the host's function of that name.
    if (!file_path.empty())
        options.resolve = [resolve](std::string_view from,
                                    std::string_view symbol) -> std::optional<std::uint64_t> {
            if (!detail::resolve(*guest->registry, from, symbol).function &&
                detail::title_dll(guest->directory, from).empty())
                if (auto *const host = detail::host_export(from, symbol))
                    return reinterpret_cast<std::uint64_t>(host);
            return resolve(from, symbol);
        };
    TitleModule module;
    module.name = name;
    module.path = std::filesystem::absolute(path).wstring();
    module.image = std::make_unique<loader::MappedImage>(file, pe, options);
    const auto &layout = module.image->report();
    module.base = module.image->guest_base();
    module.size = layout.at("size_of_image");
    if (contents.contains("tls"))
        install_static_tls(contents.at("tls"), module.base, pe.at("image_base"));
    if (pe.at("entry_rva").get<std::uint64_t>())
        module.entry = layout.at("entry_va");
    if (contents.contains("exports"))
        for (const auto &symbol : contents.at("exports").at("symbols")) {
            std::uint64_t address = module.base + symbol.at("rva").get<std::uint64_t>();
            // "module.Name": the export is another module's, found the way an import is
            if (symbol.contains("forwarder")) {
                const auto &target = symbol.at("forwarder").get_ref<const std::string &>();
                const auto dot = target.rfind('.');
                if (dot == std::string::npos || dot + 1 == target.size())
                    continue;
                const auto found = resolve(target.substr(0, dot) + ".dll", target.substr(dot + 1));
                if (!found)
                    continue;
                address = *found;
            }
            module.exports["#" + std::to_string(symbol.at("ordinal").get<std::uint64_t>())] =
                address;
            for (const auto &exported : symbol.at("names"))
                module.exports[exported.get<std::string>()] = address;
        }
    if (contents.contains("exceptions")) {
        const auto &directory = pe.at("directory_descriptors").at(3);
        module.functions = reinterpret_cast<PRUNTIME_FUNCTION>(
            module.base + directory.at("rva").get<std::uint64_t>());
        module.function_count = static_cast<DWORD>(directory.at("size").get<std::uint64_t>() / 12);
        if (!RtlAddFunctionTable(module.functions, module.function_count, module.base))
            throw std::runtime_error("Cannot register the exception table of " + name);
    }
    guest->modules.push_back(std::move(module));
    return &guest->modules.back();
}

#ifdef _MSC_VER
// Decorated name of the first type an MSVC C++ exception can be caught as, e.g.
// ".?AVbad_alloc@std@@". Reads the throw information the compiler emitted.
void report_cxx_type(const EXCEPTION_RECORD *record) {
    if (record->ExceptionCode != 0xE06D7363 || record->NumberParameters < 4)
        return;
    __try {
        const auto *info = reinterpret_cast<const std::uint8_t *>(record->ExceptionInformation[2]);
        const auto *base = reinterpret_cast<const std::uint8_t *>(record->ExceptionInformation[3]);
        if (!info || !base)
            return;
        const auto rva = [](const std::uint8_t *at) {
            std::uint32_t value;
            std::memcpy(&value, at, 4);
            return value;
        };
        const auto *types = base + rva(info + 12);
        if (!rva(types))
            return;
        const auto *type = base + rva(types + 4);
        const auto *name = reinterpret_cast<const char *>(base + rva(type + 4) + 16);
        std::fprintf(stderr, "[Runtime] C++ exception of type %.200s\n", name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// Not an unwind: every stack slot that holds an address inside the guest image, nearest
// first. Most are return addresses, some are stale or plain pointers.
void report_guest_stack(const CONTEXT *context) {
    __try {
        const auto *slot =
            reinterpret_cast<const std::uint64_t *>(context->Rsp & ~std::uint64_t{7});
        const auto *limit = reinterpret_cast<const std::uint64_t *>(
            reinterpret_cast<const NT_TIB *>(NtCurrentTeb())->StackBase);
        std::fprintf(stderr, "[Runtime] guest addresses on the stack:");
        for (int found = 0; slot < limit && found < 24; ++slot) {
            if (!is_guest(reinterpret_cast<const void *>(*slot)))
                continue;
            std::fprintf(stderr, " image+0x%llx",
                         static_cast<unsigned long long>(*slot - guest->base));
            ++found;
        }
        std::fprintf(stderr, "\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void print_location(DWORD64 address) {
    std::fprintf(stderr, " %s", detail::guest_location(reinterpret_cast<void *>(address)).c_str());
}
// An unwind, through the function tables of the host and of the guest's images
void report_unwound_stack(const CONTEXT *fault) {
    CONTEXT context = *fault;
    std::fprintf(stderr, "[Runtime] unwound:");
    __try {
        // a call through a null pointer: its caller is the first thing on the stack
        if (!context.Rip) {
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8;
        }
        for (int depth = 0; depth < 24 && context.Rip; ++depth) {
            print_location(context.Rip);
            DWORD64 base = 0;
            auto *const function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (!function) {
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void *handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context,
                             &handler_data, &frame, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    std::fprintf(stderr, "\n");
}
// what is on a stopped thread's stack from `from` up
int copy_stack(DWORD64 from, DWORD64 *into, int most) {
    int count = 0;
    __try {
        for (; count < most; ++count)
            into[count] = reinterpret_cast<const DWORD64 *>(from & ~DWORD64{7})[count];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}
// the return addresses of a thread that is suspended, innermost first
int unwind_frames(CONTEXT context, DWORD64 *frames, int most) {
    int count = 0;
    __try {
        while (count < most && context.Rip) {
            frames[count++] = context.Rip;
            DWORD64 base = 0;
            auto *const function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (!function) {
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void *handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context,
                             &handler_data, &frame, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}
int report_fault(EXCEPTION_POINTERS *info, bool last = true) {
    const auto *record = info->ExceptionRecord;
    const auto address = reinterpret_cast<std::uint64_t>(record->ExceptionAddress);
    if (is_guest(record->ExceptionAddress))
        std::fprintf(stderr, "[Runtime] guest exception 0x%08lx at image+0x%llx\n",
                     record->ExceptionCode, static_cast<unsigned long long>(address - guest->base));
    else if (const auto *module = module_at(record->ExceptionAddress))
        std::fprintf(stderr, "[Runtime] guest exception 0x%08lx at %s+0x%llx\n",
                     record->ExceptionCode, module->name.c_str(),
                     static_cast<unsigned long long>(address - module->base));
    else {
        HMODULE host = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(record->ExceptionAddress), &host))
            GetModuleFileNameW(host, name, MAX_PATH);
        std::fprintf(
            stderr, "[Runtime] guest exception 0x%08lx at host address 0x%llx (%ls+0x%llx)\n",
            record->ExceptionCode, static_cast<unsigned long long>(address),
            std::filesystem::path(name).filename().c_str(),
            static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(host)));
    }
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        MEMORY_BASIC_INFORMATION region{};
        VirtualQuery(reinterpret_cast<void *>(record->ExceptionInformation[1]), &region,
                     sizeof region);
        std::fprintf(stderr,
                     "[Runtime] %s address 0x%llx (allocation %p, region %p of 0x%zx bytes, "
                     "state 0x%lx, protection 0x%lx)\n",
                     record->ExceptionInformation[0] == 1   ? "writing"
                     : record->ExceptionInformation[0] == 8 ? "running"
                                                            : "reading",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]),
                     region.AllocationBase, region.BaseAddress, region.RegionSize, region.State,
                     region.Protect);
    }
    report_cxx_type(record);
    // the registers, for a check of the title's that stops it (an int3 after a compare)
    const auto &c = *info->ContextRecord;
    std::fprintf(stderr,
                 "[Runtime] rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx "
                 "rsp %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx "
                 "r15 %llx\n",
                 c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11,
                 c.R12, c.R13, c.R14, c.R15);
    report_guest_stack(info->ContextRecord);
    report_unwound_stack(info->ContextRecord);
    report_recent_calls();
    std::fflush(nullptr);
    if (!last) // one the title may yet handle
        return EXCEPTION_CONTINUE_SEARCH;
    // Unwinding a guest in an unknown state can block or fault again.
    TerminateProcess(GetCurrentProcess(), exit_guest_fault);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
struct Start {
    void *entry;
    // DllMain of every title DLL, in load order, and what to pass it
    std::vector<std::pair<void *, void *>> libraries;
};
// The title's DLLs whose entry point has run. The system tells a DLL of every thread that
// starts and ends, which is how a garbage collector gets to know the threads it has to
// stop; these DLLs are mapped here, so the telling is ours to do. The list is replaced, not
// changed: a thread that starts reads it without a lock.
using DllEntry = BOOL(WINAPI *)(void *, DWORD, void *);
struct Attached {
    DllEntry entry;
    void *base;
};
std::atomic<const std::vector<Attached> *> attached{nullptr};
void attach(std::uint64_t entry, void *base) {
    const auto function = reinterpret_cast<DllEntry>(entry);
    function(base, DLL_PROCESS_ATTACH, nullptr);
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    const auto *const before = attached.load();
    auto *const after = before ? new std::vector<Attached>(*before) : new std::vector<Attached>;
    after->push_back({function, base});
    attached = after; // the one before stays: a thread may be reading it
}
void NTAPI on_thread(void *, DWORD reason, void *) {
    if (reason != DLL_THREAD_ATTACH && reason != DLL_THREAD_DETACH)
        return;
    if (const auto *const libraries = attached.load())
        for (const auto &library : *libraries)
            library.entry(library.base, reason, nullptr);
}
DWORD run_guest(const Start &start) {
    start_static_tls();
    for (const auto &[entry, base] : start.libraries)
        attach(reinterpret_cast<std::uint64_t>(entry), base);
    return reinterpret_cast<DWORD (*)()>(start.entry)();
}
#ifdef _MSC_VER
// Threads the title creates itself have no handler of ours around them: this reports
// their faults the same way before the process ends.
LONG WINAPI unhandled(EXCEPTION_POINTERS *info) {
    if (guest) {
        std::fprintf(stderr, "[Runtime] on thread %lu:\n", GetCurrentThreadId());
        report_fault(info);
    }
    return title_filter ? title_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
// XV_FIRST_CHANCE: exceptions as they happen, also those the title handles itself (engines
// that catch a thread's fault and only say that it crashed); not thread names or debug text
LONG WINAPI first_chance(EXCEPTION_POINTERS *info) {
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code != 0x406d1388 && code != DBG_PRINTEXCEPTION_C && code != DBG_PRINTEXCEPTION_WIDE_C) {
        static std::atomic<int> reported{0};
        if (reported++ < 8) {
            std::fprintf(stderr, "[Runtime] first chance, thread %lu:\n", GetCurrentThreadId());
            report_fault(info, false);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// XV_BREAK=<rva>[,<rva>...]: the registers each time the title gets to one of these addresses
// of its image (hexadecimal), up to 40 times each or XV_BREAK_LIMIT. A breakpoint is put there; the
// handler puts the instruction back, steps over it and sets the breakpoint again.
struct Breakpoint {
    std::uint8_t *address;
    std::uint8_t original;
    std::atomic<int> hits{0};
};
std::deque<Breakpoint> breakpoints;
int break_limit = 40;
thread_local Breakpoint *stepping = nullptr;
void poke(std::uint8_t *address, std::uint8_t value) {
    DWORD previous;
    VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &previous);
    *address = value;
    VirtualProtect(address, 1, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), address, 1);
}
// XV_WATCH=<offset>[+<more>]: when the first breakpoint of XV_BREAK is hit, the 8 bytes at
// [rax + offset] + more are watched for writes with a debug register, on every thread there
// is then, and each write says where it came from.
std::atomic<int> watch_reports{0}, all_hits{0};
void watch(std::uint64_t address, CONTEXT &own) {
    const auto arm = [&](CONTEXT &context) {
        context.Dr0 = address;
        context.Dr7 = (context.Dr7 & ~0xf0003ull) | 1 | 1ull << 16 | 2ull << 18;
    };
    own.ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    arm(own);
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{sizeof entry};
    int armed = 1;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
            entry.th32ThreadID == GetCurrentThreadId())
            continue;
        const auto thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                       entry.th32ThreadID);
        if (!thread)
            continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                arm(context);
                armed += SetThreadContext(thread, &context) != 0;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    std::fprintf(stderr, "[Watch] writes to %llx are watched on %d threads\n", address, armed);
    std::fflush(stderr);
}
LONG WINAPI at_breakpoint(EXCEPTION_POINTERS *info) {
    auto &context = *info->ContextRecord;
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && context.Dr6 & 1) {
        context.Dr6 = 0;
        if (watch_reports++ < 40) {
            std::uint64_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void *>(context.Dr0), sizeof value);
            std::fprintf(stderr, "[Watch] thread %lu wrote %llx before %s, called from %s\n",
                         GetCurrentThreadId(), value,
                         detail::guest_location(reinterpret_cast<void *>(context.Rip)).c_str(),
                         detail::guest_location(*reinterpret_cast<void **>(context.Rsp)).c_str());
            std::fflush(stderr);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && stepping) {
        if (stepping->hits < break_limit)
            poke(stepping->address, 0xcc);
        stepping = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    for (auto &point : breakpoints) {
        if (point.address != info->ExceptionRecord->ExceptionAddress)
            continue;
        if (point.hits++ < break_limit) {
            std::uint64_t at_rcx[4]{}, at_rdx[4]{}, at_r8[5]{};
            // no further than the region the address is in: past it may be no memory
            const auto read = [](std::uint64_t address, void *into, std::size_t size) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof memory) &&
                    memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    std::memcpy(into, reinterpret_cast<void *>(address),
                                std::min<std::uint64_t>(
                                    size, reinterpret_cast<std::uint64_t>(memory.BaseAddress) +
                                              memory.RegionSize - address));
            };
            read(context.Rcx, at_rcx, sizeof at_rcx);
            read(context.Rdx, at_rdx, sizeof at_rdx);
            read(context.R8, at_r8, sizeof at_r8);
            std::fprintf(stderr,
                         "[Break] at %llu ms image+0x%llx thread %lu rax %llx (image at %llx) rcx "
                         "%llx rdx %llx r8 %llx r9 %llx "
                         "[rcx] %llx %llx %llx %llx [rdx] %llx %llx %llx %llx [r8] %llx %llx %llx "
                         "%llx %llx from %s\n",
                         GetTickCount64(),
                         static_cast<unsigned long long>(
                             reinterpret_cast<std::uint64_t>(point.address) - guest->base),
                         GetCurrentThreadId(), context.Rax,
                         static_cast<unsigned long long>(guest->base), context.Rcx, context.Rdx,
                         context.R8, context.R9, at_rcx[0], at_rcx[1], at_rcx[2], at_rcx[3],
                         at_rdx[0], at_rdx[1], at_rdx[2], at_rdx[3], at_r8[0], at_r8[1], at_r8[2],
                         at_r8[3], at_r8[4],
                         detail::guest_location(*reinterpret_cast<void **>(context.Rsp)).c_str());
            // XV_BREAK_AT=1: the 8 qwords at rax itself, and rbx, r14, r15
            if (std::getenv("XV_BREAK_AT")) {
                std::uint64_t words[8]{};
                read(context.Rax, words, sizeof words);
                std::fprintf(stderr, "[Break]   rbx %llx r14 %llx r15 %llx [rax]:", context.Rbx,
                             context.R14, context.R15);
                for (const auto word : words)
                    std::fprintf(stderr, " %016llx", word);
                std::fprintf(stderr, "\n");
                read(context.R15, words, sizeof words);
                std::fprintf(stderr, "[Break]   [r15]:");
                for (const auto word : words)
                    std::fprintf(stderr, " %016llx", word);
                std::fprintf(stderr, "\n");
                read(context.R14, words, sizeof words);
                std::fprintf(stderr, "[Break]   [r14]:");
                for (const auto word : words)
                    std::fprintf(stderr, " %016llx", word);
                std::fprintf(stderr, "\n");
            }
            // XV_BREAK_MEM=<register>[-<back>]:<length>[,...]: the bytes at a register's value
            // (rax rcx rdx rbx rsi rdi rbp rsp r8-r15), from <back> bytes before it, in hexadecimal
            if (const char *list = std::getenv("XV_BREAK_MEM"))
                while (*list) {
                    static const struct {
                        const char *name;
                        DWORD64 CONTEXT::*value;
                    } registers[] = {
                        {"rax", &CONTEXT::Rax}, {"rcx", &CONTEXT::Rcx}, {"rdx", &CONTEXT::Rdx},
                        {"rbx", &CONTEXT::Rbx}, {"rsi", &CONTEXT::Rsi}, {"rdi", &CONTEXT::Rdi},
                        {"rbp", &CONTEXT::Rbp}, {"rsp", &CONTEXT::Rsp}, {"r8", &CONTEXT::R8},
                        {"r9", &CONTEXT::R9},   {"r10", &CONTEXT::R10}, {"r11", &CONTEXT::R11},
                        {"r12", &CONTEXT::R12}, {"r13", &CONTEXT::R13}, {"r14", &CONTEXT::R14},
                        {"r15", &CONTEXT::R15}};
                    std::uint64_t base = 0;
                    const char *name = "?";
                    for (const auto &entry : registers)
                        if (!std::strncmp(list, entry.name, std::strlen(entry.name))) {
                            base = context.*entry.value;
                            name = entry.name;
                            list += std::strlen(entry.name);
                            break;
                        }
                    char *end = nullptr;
                    const auto back = *list == '-' ? std::strtoull(list + 1, &end, 16) : 0;
                    if (end)
                        list = end;
                    const auto length =
                        *list == ':'
                            ? std::min<std::uint64_t>(std::strtoull(list + 1, &end, 16), 0x1000)
                            : 0;
                    if (!length)
                        break;
                    list = *end ? end + 1 : end;
                    std::vector<std::uint8_t> bytes(length);
                    read(base - back, bytes.data(), bytes.size());
                    std::fprintf(stderr, "[Break]   %s %llx - %llx:", name, base, back);
                    for (std::size_t at = 0; at < bytes.size(); ++at)
                        std::fprintf(stderr, at % 4 ? "%02x" : " %02x", bytes[at]);
                    std::fprintf(stderr, "\n");
                }
            // XV_BREAK_RAX=<offset>[,<offset>...]: the 16 qwords [[rax + offset]] leads to
            if (const char *list = std::getenv("XV_BREAK_RAX"))
                for (char *end = nullptr; *list; list = *end ? end + 1 : end) {
                    const auto offset = std::strtoull(list, &end, 16);
                    if (end == list)
                        break;
                    std::uint64_t pointer = 0, words[16]{};
                    read((std::getenv("XV_BREAK_R8") ? context.R8 : context.Rax) + offset, &pointer,
                         sizeof pointer);
                    read(pointer, words, sizeof words);
                    std::fprintf(stderr, "[Break]   [[rax+0x%llx]] = %llx:", offset, pointer);
                    for (const auto word : words)
                        std::fprintf(stderr, " %llx", word);
                    std::fprintf(stderr, "\n");
                }
            std::fflush(stderr);
            if (const char *text = std::getenv("XV_WATCH");
                text &&
                // XV_WATCH_ANY: at that hit of all the breakpoints together, not of the first
                (std::getenv("XV_WATCH_ANY")      ? ++all_hits
                 : &point == &breakpoints.front() ? static_cast<int>(point.hits)
                                                  : 0) ==
                    (std::getenv("XV_WATCH_HIT") ? std::atoi(std::getenv("XV_WATCH_HIT")) : 1)) {
                char *end = nullptr;
                const auto offset = std::strtoull(text, &end, 16);
                const auto more = *end == '+' ? std::strtoull(end + 1, nullptr, 16) : 0;
                std::uint64_t pointer = 0;
                // XV_WATCH_RCX: from rcx instead of rax
                read((std::getenv("XV_WATCH_RCX") ? context.Rcx : context.Rax) + offset, &pointer,
                     sizeof pointer);
                // XV_WATCH_DIRECT: the register's value + offset itself, not what it leads to
                if (std::getenv("XV_WATCH_DIRECT"))
                    pointer = (std::getenv("XV_WATCH_RCX") ? context.Rcx : context.Rax) + offset;
                if (pointer)
                    watch(pointer + more, context);
            }
        }
        poke(point.address, point.original);
        stepping = &point;
        context.EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// CLWB (66 0F AE /6), which the Series console's processor has and not every host's: where
// one stops the title it is made a CLFLUSH (0F AE /7 under a prefix that does nothing),
// which writes the line back as well.
// A title also writes back the lines of memory that is its own only on the console (what a
// resource of the GPU's is made of, which the host keeps elsewhere): there is nothing to
// write back, and the instruction is passed over.
bool passed_over_flush(CONTEXT &context) {
    const auto *at = reinterpret_cast<const std::uint8_t *>(context.Rip);
    if (!detail::title_image_base(at))
        return false;
    const auto *opcode = at;
    while (*opcode == 0x3e || *opcode == 0x66)
        ++opcode;
    if ((*opcode & 0xf0) == 0x40)
        ++opcode;
    const auto modrm = opcode[2];
    if (opcode[0] != 0x0f || opcode[1] != 0xae || (modrm >> 3 & 7) < 6 || modrm >> 6 == 3)
        return false;
    std::size_t length = 3;
    const auto mod = modrm >> 6, rm = modrm & 7;
    if (rm == 4)
        length += 1 + (mod == 0 && (opcode[3] & 7) == 5 ? 4 : 0);
    length += mod == 1 ? 1 : mod == 2 || (mod == 0 && rm == 5) ? 4 : 0;
    context.Rip = reinterpret_cast<DWORD64>(opcode + length);
    return true;
}
LONG WINAPI missing_instruction(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        passed_over_flush(*info->ContextRecord))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    auto *const at = reinterpret_cast<std::uint8_t *>(info->ContextRecord->Rip);
    if (!detail::title_image_base(at) || at[0] != 0x66)
        return EXCEPTION_CONTINUE_SEARCH;
    auto *const opcode = at + ((at[1] & 0xf0) == 0x40 ? 2 : 1);
    if (opcode[0] != 0x0f || opcode[1] != 0xae || (opcode[2] >> 3 & 7) != 6 || opcode[2] >> 6 == 3)
        return EXCEPTION_CONTINUE_SEARCH;
    poke(opcode + 2, opcode[2] | 8);
    poke(at, 0x3e);
    return EXCEPTION_CONTINUE_EXECUTION;
}
// XV_PATCH=<rva>:<bytes>:<bytes>[,...]: the bytes at an address of the title's image (all in
// hexadecimal) changed from the first to the second, only where they are the first: a
// title's own settings that no API reaches (a title's settings in titles/<id>.ini set it).
void apply_patches(std::uint8_t *base) {
    const char *text = std::getenv("XV_PATCH");
    if (!text)
        return;
    const auto bytes = [](std::string_view hex) {
        std::vector<std::uint8_t> out;
        for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
            out.push_back(
                static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(at, 2)), nullptr, 16)));
        return out;
    };
    std::string_view all(text);
    while (!all.empty()) {
        const auto one = all.substr(0, all.find(','));
        all.remove_prefix(std::min(all.size(), one.size() + 1));
        const auto first = one.find(':'), second = one.find(':', first + 1);
        if (first == one.npos || second == one.npos)
            continue;
        const auto rva = std::strtoull(std::string(one.substr(0, first)).c_str(), nullptr, 16);
        const auto expected = bytes(one.substr(first + 1, second - first - 1));
        const auto wanted = bytes(one.substr(second + 1));
        const bool matches = !expected.empty() && expected.size() == wanted.size() &&
                             std::equal(expected.begin(), expected.end(), base + rva);
        if (matches)
            for (std::size_t n = 0; n < wanted.size(); ++n)
                poke(base + rva + n, wanted[n]);
        std::fprintf(stderr, "[Runtime] %zu bytes at image+0x%llx %s\n", wanted.size(), rva,
                     matches ? "patched" : "left as they are: they are not the expected ones");
    }
    std::fflush(stderr);
}
void set_breakpoints(std::uint8_t *base) {
    apply_patches(base);
    const char *text = std::getenv("XV_BREAK");
    if (!text)
        return;
    if (const char *limit = std::getenv("XV_BREAK_LIMIT"))
        break_limit = std::atoi(limit);
    AddVectoredExceptionHandler(1, &at_breakpoint);
    for (char *end = nullptr; *text; text = *end ? end + 1 : end) {
        const auto rva = std::strtoull(text, &end, 16);
        if (end == text)
            break;
        auto &point = breakpoints.emplace_back();
        point.address = base + rva;
        point.original = *point.address;
        poke(point.address, 0xcc);
    }
}
#endif
DWORD WINAPI guest_thread(void *start) {
#ifdef _MSC_VER
    __try {
        return run_guest(*static_cast<const Start *>(start));
    } __except (report_fault(GetExceptionInformation())) {
        return exit_guest_fault;
    }
#else
    return run_guest(*static_cast<const Start *>(start));
#endif
}
struct Stubs {
    Stubs() {
        memory = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!memory)
            throw std::runtime_error("Cannot allocate import stubs");
    }
    ~Stubs() { VirtualFree(memory, 0, MEM_RELEASE); }
    static constexpr std::size_t size = stub_size * stub_limit + trace_size * trace_limit;
    std::uint8_t *memory;
};
} // namespace

#ifdef _MSC_VER
// a TLS callback of this executable: the system runs it in every thread that starts or ends
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:xv_thread_callback")
#pragma const_seg(".CRT$XLX")
extern "C" const PIMAGE_TLS_CALLBACK xv_thread_callback = &on_thread;
#pragma const_seg()
#endif

std::uint64_t detail::guest_offset(const void *address) {
    return guest && is_guest(address) ? reinterpret_cast<std::uint64_t>(address) - guest->base : 0;
}

bool detail::in_title(const void *address) {
    return guest && (is_guest(address) || module_at(address));
}

std::string detail::guest_location(const void *address) {
    char text[300];
    const auto value = reinterpret_cast<unsigned long long>(address);
    if (guest && is_guest(address))
        std::snprintf(text, sizeof text, "image+0x%llx", value - guest->base);
    else if (const auto *module = guest ? module_at(address) : nullptr)
        std::snprintf(text, sizeof text, "%s+0x%llx", module->name.c_str(), value - module->base);
    else if (HMODULE host = nullptr;
             GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(address), &host)) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(host, name, MAX_PATH);
        std::snprintf(text, sizeof text, "%s+0x%llx",
                      std::filesystem::path(name).filename().string().c_str(),
                      value - reinterpret_cast<unsigned long long>(host));
    } else
        std::snprintf(text, sizeof text, "0x%llx", value);
    return text;
}

namespace {
const TitleModule *load_title(std::string_view module, const std::filesystem::path &file = {}) {
    const std::lock_guard lock(guest->loading);
    const auto loaded = guest->modules.size();
    // a DLL loaded now may need stubs or trace thunks of its own
    DWORD previous;
    if (!VirtualProtect(guest->stubs, Stubs::size, PAGE_EXECUTE_READWRITE, &previous))
        return nullptr;
    const TitleModule *library = nullptr;
    try {
        library = title_module(module, guest->resolve, file);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] cannot load the title's %.*s: %s\n",
                     static_cast<int>(module.size()), module.data(), e.what());
    }
    VirtualProtect(guest->stubs, Stubs::size, PAGE_EXECUTE_READ, &previous);
    for (auto i = loaded; i < guest->modules.size(); ++i) {
        const auto &added = guest->modules[i];
        std::fprintf(stderr, "[Runtime] loaded the title's %s\n", added.name.c_str());
        std::fflush(stderr);
        if (added.entry)
            attach(added.entry, reinterpret_cast<void *>(added.base));
    }
    return library;
}
void *export_of(const TitleModule *library, std::string_view symbol) {
    if (!library)
        return nullptr;
    const auto exported = library->exports.find(std::string(symbol));
    return exported == library->exports.end() ? nullptr
                                              : reinterpret_cast<void *>(exported->second);
}
// a console system module of the allowed ones in the title's system folder, if it is there
std::filesystem::path system_file(std::string_view module) {
    if (!guest || guest->system_directory.empty() || !console_system_module(module))
        return {};
    std::error_code error;
    const auto file = guest->system_directory / std::string(module);
    return std::filesystem::is_regular_file(file, error) ? file : std::filesystem::path{};
}
} // namespace

void *detail::system_export(std::string_view module, std::string_view symbol) {
    const auto file = system_file(module);
    return file.empty() ? nullptr : export_of(load_title(lower(module), file), symbol);
}

void *detail::title_export(std::string_view module, std::string_view symbol) {
    return guest ? export_of(load_title(module), symbol) : nullptr;
}

void *detail::guest_library(const wchar_t *path) {
    if (!guest || !path)
        return nullptr;
    auto name = lower(std::filesystem::path(path).filename().string());
    if (name.find('.') == std::string::npos)
        name += ".dll";
    if (const auto file = system_file(name); !file.empty())
        if (const auto *library = load_title(name, file))
            return reinterpret_cast<void *>(library->base);
    if (guest->registry->has(name)) {
        const std::lock_guard lock(guest->loading);
        auto &handle = guest->own_handles[name];
        if (!handle)
            handle = std::make_unique<std::string>(name);
        return handle.get();
    }
    // by its path, if that is in the title's folder (Unity's Data\Native)
    std::filesystem::path file;
    if (std::filesystem::path(path).has_parent_path()) {
        std::error_code error;
        const auto full = std::filesystem::weakly_canonical(path, error);
        const auto relative =
            full.lexically_relative(std::filesystem::weakly_canonical(guest->directory, error));
        if (!error && !relative.empty() && *relative.begin() != ".." &&
            std::filesystem::is_regular_file(full, error))
            file = full;
    }
    const auto *library = load_title(name, file);
    return library ? reinterpret_cast<void *>(library->base) : nullptr;
}

void *detail::guest_symbol(void *module, const char *symbol) {
    if (!guest || !module)
        return nullptr;
    const auto label = reinterpret_cast<std::uintptr_t>(symbol) < 0x10000
                           ? "#" + std::to_string(reinterpret_cast<std::uintptr_t>(symbol))
                           : std::string(symbol);
    {
        const std::lock_guard lock(guest->loading);
        for (const auto &[name, handle] : guest->own_handles)
            if (handle.get() == module)
                return guest->registry->find(name, label);
    }
    const auto *library = module_at(module);
    return library && library->base == reinterpret_cast<std::uint64_t>(module)
               ? export_of(library, label)
               : nullptr;
}

void *detail::title_image_base(const void *address) {
    if (!guest || !address)
        return nullptr;
    if (is_guest(address))
        return guest_module();
    const auto *const title = module_at(address);
    return title ? reinterpret_cast<void *>(title->base) : nullptr;
}

void *detail::runtime_override(std::string_view symbol) {
    for (const auto &entry : overrides)
        if (entry.symbol == symbol)
            return entry.function;
    return nullptr;
}

void *detail::host_export(std::string_view module, std::string_view symbol) {
    // file names only, loaded from System32
    if (module.empty() || module.size() > 255 ||
        module.find_first_of("\\/:") != std::string_view::npos)
        return nullptr;
    const std::wstring name(module.begin(), module.end());
    const auto library = LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
        return nullptr;
    if (!symbol.starts_with('#'))
        return reinterpret_cast<void *>(GetProcAddress(library, std::string(symbol).c_str()));
    const auto ordinal = std::strtoul(std::string(symbol.substr(1)).c_str(), nullptr, 10);
    if (!ordinal || ordinal > 0xffff)
        return nullptr;
    return reinterpret_cast<void *>(GetProcAddress(library, MAKEINTRESOURCEA(ordinal)));
}

std::uint32_t execute(const std::filesystem::path &path, std::span<const std::uint8_t> file,
                      const Registry &registry, const ExecuteOptions &execution) {
    if (guest)
        throw std::logic_error("A guest image is already running in this process");
    const auto pe = pe::inspect(file, pe::Detail::light);
    const auto &contents = pe.at("directory_contents");
    if (pe.at("characteristics").get<std::uint64_t>() & 0x2000)
        throw std::runtime_error("The image is a DLL, not an executable");
    if (!pe.at("entry_rva").get<std::uint64_t>())
        throw std::runtime_error("The image has no entry point");
    // Delay-load imports are bound by the image's own helper on first use, through
    // LoadLibrary and GetProcAddress or ResolveDelayLoadedAPI, which are ours.

    // XV_MUTE does the same as the option, for runs started from the window
    if ((execution.mute || std::getenv("XV_MUTE")) && !audio::mute_process())
        std::fprintf(stderr, "[Runtime] could not mute the audio of this process\n");

    detail::set_save_directory(execution.save_directory);
    detail::set_shader_cache_directory(execution.shader_cache_directory);
    detail::set_console_settings(execution.console);
    Stubs stubs;
    Guest state;
    state.path = std::filesystem::absolute(path).wstring();
    state.directory = std::filesystem::absolute(path).parent_path();
    // a title finds its files relative to its own folder, wherever xvalkyrie was started
    if (!SetCurrentDirectoryW(state.directory.c_str()))
        throw std::runtime_error("Cannot enter the title's folder");
    state.stubs = stubs.memory;
    if (execution.trace)
        state.calls = std::make_unique<std::atomic<std::uint64_t>[]>(trace_limit);
    guest = &state;
    struct Reset {
        ~Reset() { guest = nullptr; }
    } reset;

    loader::MapOptions options;
    options.executable = true;
    Resolver resolve;
    resolve = [&](std::string_view module,
                  std::string_view symbol) -> std::optional<std::uint64_t> {
        const auto name = std::string(module) + "!" + std::string(symbol);
        // the console's own computation modules win over ours; a DLL the title brings wins
        // over the host's, but not over ours
        if (const auto file = system_file(module); !file.empty())
            if (const auto *library = title_module(module, resolve, file)) {
                const auto exported = library->exports.find(std::string(symbol));
                return exported == library->exports.end() ? make_stub(name) : exported->second;
            }
        if (!registry.find(module, symbol) && registry.forwarded(module).empty()) {
            if (const auto *library = title_module(module, resolve)) {
                const auto exported = library->exports.find(std::string(symbol));
                return exported == library->exports.end() ? make_stub(name) : exported->second;
            }
        }
        auto *const function = detail::resolve(registry, module, symbol).function;
        if (!function)
            return make_stub(name);
        return execution.trace ? make_trace(name, function)
                               : reinterpret_cast<std::uint64_t>(function);
    };
    options.resolve = resolve;
    state.resolve = resolve;
    state.registry = &registry;
    state.system_directory = execution.system_directory.empty()
                                 ? console_system_directory(state.path)
                                 : execution.system_directory;
    if (!state.system_directory.empty())
        std::fprintf(stderr, "[Runtime] console system modules from %ls\n",
                     state.system_directory.c_str());
    const loader::MappedImage image(file, pe, options);
    const auto &layout = image.report();
    if (!layout.at("imports").at("modules_without_symbols").empty())
        throw std::runtime_error("Unsupported: the image has a bound import table");
    if (!execution.allow_missing && !state.missing.empty()) {
        std::string text =
            std::to_string(state.missing.size()) + " imports have no implementation:";
        for (std::size_t i = 0; i < std::min<std::size_t>(state.missing.size(), 8); ++i)
            text += " " + state.missing[i];
        throw std::runtime_error(text + (state.missing.size() > 8 ? " ..." : ""));
    }
    DWORD previous;
    if (!VirtualProtect(stubs.memory, Stubs::size, PAGE_EXECUTE_READ, &previous))
        throw std::runtime_error("Cannot protect import stubs");
    state.base = image.guest_base();
    state.size = layout.at("size_of_image");
    if (contents.contains("tls"))
        install_static_tls(contents.at("tls"), state.base, pe.at("image_base"), true);

    // needed for SEH and C++ exceptions inside the guest
    const auto &directories = pe.at("directory_descriptors");
    std::uint64_t table = 0, count = 0;
    if (contents.contains("exceptions")) {
        table = directories.at(3).at("rva").get<std::uint64_t>();
        count = directories.at(3).at("size").get<std::uint64_t>() / 12;
    }
    auto *const functions = reinterpret_cast<PRUNTIME_FUNCTION>(state.base + table);
    if (count && !RtlAddFunctionTable(functions, static_cast<DWORD>(count), state.base))
        throw std::runtime_error("Cannot register the guest exception table");

    Start start{reinterpret_cast<void *>(layout.at("entry_va").get<std::uint64_t>()), {}};
    for (const auto &module : state.modules) {
        std::fprintf(stderr, "[Runtime] loaded the title's %s\n", module.name.c_str());
        if (module.entry)
            start.libraries.emplace_back(reinterpret_cast<void *>(module.entry),
                                         reinterpret_cast<void *>(module.base));
    }
#ifdef _MSC_VER
    SetUnhandledExceptionFilter(&unhandled);
    AddVectoredExceptionHandler(1, &missing_instruction);
    if (std::getenv("XV_FIRST_CHANCE"))
        AddVectoredExceptionHandler(0, &first_chance);
    set_breakpoints(reinterpret_cast<std::uint8_t *>(state.base));
#endif
    // A console ends a wait within about a millisecond of its time, and titles pace themselves
    // on that with short waits (an audio mixer that sleeps 2 ms between blocks). Their Sleep and
    // waits are the host's, as are ours that keep the frame's events, so for as long as the
    // title runs this process gets millisecond timers.
    const host::TimerResolution console_timers(1);
    std::fflush(nullptr);
    const auto thread = CreateThread(nullptr, 0, guest_thread, &start, 0, nullptr);
    if (!thread) {
        if (count)
            RtlDeleteFunctionTable(functions);
        throw std::runtime_error("Cannot create the guest thread");
    }
    WaitForSingleObject(thread, INFINITE);
    DWORD code = exit_guest_fault;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    if (count)
        RtlDeleteFunctionTable(functions);
    return code;
}
// Where every other thread is, for something one of them has just done and no call says
// which: each is stopped for as long as its stack is read.
void detail::report_thread_stacks(std::uint64_t own_stack) {
    static std::atomic<bool> once{false};
    if (once.exchange(true))
        return;
    // Every other thread is stopped first and stays so: the title ends the process when it
    // sees the device gone. Nothing here takes a lock one of them may hold: the addresses
    // go out as they are, each with the base of what it is in.
    static HANDLE threads[512];
    static DWORD ids[512];
    int count = 0;
    // the process's threads straight from the system: a snapshot of every thread there is
    // takes longer than the title does to end
    using Next = LONG(NTAPI *)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, HANDLE *);
    const auto next =
        reinterpret_cast<Next>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread"));
    HANDLE thread = nullptr;
    while (next && count < 512 &&
           next(GetCurrentProcess(), thread,
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, 0, 0,
                &thread) >= 0) {
        const auto id = GetThreadId(thread);
        if (id == GetCurrentThreadId() || SuspendThread(thread) == static_cast<DWORD>(-1))
            continue;
        ids[count] = id;
        threads[count++] = thread;
    }
    const auto out = GetStdHandle(STD_ERROR_HANDLE);
    const auto say = [&](const char *text, int length) {
        DWORD written = 0;
        WriteFile(out, text, static_cast<DWORD>(length), &written, nullptr);
    };
    static DWORD64 stack[600];
    static char line[8192];
    // the calling thread's own first, from where it says its stack is
    for (int index = own_stack ? -1 : 0; index < count; ++index) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (index < 0)
            context.Rsp = own_stack;
        else if (!GetThreadContext(threads[index], &context))
            continue;
        const int words = copy_stack(context.Rsp, stack, static_cast<int>(std::size(stack)));
        int length =
            std::snprintf(line, sizeof line,
                          "[Stacks] thread %lu%s:", index < 0 ? GetCurrentThreadId() : ids[index],
                          index < 0 ? " (the caller)" : "");
        const auto place = [&](DWORD64 address) {
            MEMORY_BASIC_INFORMATION region{};
            if (address < 0x10000 ||
                !VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof region) ||
                region.State != MEM_COMMIT ||
                !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY)))
                return false;
            length += std::snprintf(
                line + length, sizeof line - length, " %llx+%llx",
                static_cast<unsigned long long>(reinterpret_cast<DWORD64>(region.AllocationBase)),
                static_cast<unsigned long long>(address -
                                                reinterpret_cast<DWORD64>(region.AllocationBase)));
            return true;
        };
        place(context.Rip);
        for (int i = 0, shown = 0; i < words && shown < (index < 0 ? 90 : 40) && length < 7000; ++i)
            shown += place(stack[i]);
        line[length++] = 10;
        say(line, length);
    }
    for (int index = 0; index < count; ++index)
        ResumeThread(threads[index]);
}
} // namespace xv::runtime
