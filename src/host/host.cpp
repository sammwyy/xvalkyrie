#include "host/files.hpp"
#include "host/memory.hpp"
#include "host/module.hpp"
#include "host/threading.hpp"
#include "host/usage.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <windows.h>
// after windows.h, which it needs
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <timeapi.h>

namespace xv::host {
namespace {
[[noreturn]] void fail(const char *operation, DWORD error = GetLastError());
}
ProcessUsage process_usage() {
    static std::mutex mutex;
    static std::uint64_t busy_before = 0, wall_before = 0;
    const auto ticks = [](const FILETIME &time) {
        return std::uint64_t{time.dwHighDateTime} << 32 | time.dwLowDateTime;
    };
    ProcessUsage usage;
    FILETIME created, ended, kernel, user, now;
    GetSystemTimeAsFileTime(&now);
    if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) {
        const std::lock_guard lock(mutex);
        const auto busy = ticks(kernel) + ticks(user), wall = ticks(now);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        if (wall_before && wall > wall_before)
            usage.processor = std::min(1.0, static_cast<double>(busy - busy_before) /
                                                (static_cast<double>(wall - wall_before) *
                                                 std::max<DWORD>(system.dwNumberOfProcessors, 1)));
        busy_before = busy;
        wall_before = wall;
    }
    PROCESS_MEMORY_COUNTERS counters{sizeof counters};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters))
        usage.memory = counters.WorkingSetSize;
    MEMORYSTATUSEX status{sizeof status};
    if (GlobalMemoryStatusEx(&status))
        usage.installed = status.ullTotalPhys;
    return usage;
}
std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}
std::uint32_t run_beside(const std::wstring &name, const std::vector<std::wstring> &arguments,
                         std::atomic<void *> &process,
                         const std::function<void(std::string_view)> &output) {
    const auto path = (executable_directory() / name).wstring();
    auto command = L"\"" + path + L"\"";
    for (const auto &argument : arguments) {
        // backslashes right before the closing quote would escape it: doubled, they are
        // backslashes again
        const auto last = argument.find_last_not_of(L'\\');
        const auto trailing = argument.size() - (last == std::wstring::npos ? 0 : last + 1);
        command += L" \"" + argument + std::wstring(trailing, L'\\') + L"\"";
    }
    // a job that ends its processes when this one goes, however it goes
    static const HANDLE job = [] {
        const auto handle = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (handle)
            SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits,
                                    sizeof limits);
        return handle;
    }();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    PROCESS_INFORMATION child{};
    // without a console here, a console program would open one of its own
    DWORD flags = CREATE_SUSPENDED | (GetConsoleWindow() ? 0 : CREATE_NO_WINDOW);
    // The child's stdout and stderr are a pipe read here. It inherits the pipe and no other
    // handle of this process.
    HANDLE read = nullptr, write = nullptr;
    std::vector<std::byte> attributes;
    if (output) {
        SECURITY_ATTRIBUTES inherited{sizeof inherited, nullptr, TRUE};
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes.resize(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!CreatePipe(&read, &write, &inherited, 1 << 16) ||
            !SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) ||
            !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) ||
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof write,
                                       nullptr, nullptr))
            fail("Cannot make the pipe for the output of a process");
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    const bool started =
        CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, output ? TRUE : FALSE, flags,
                       nullptr, nullptr, &startup.StartupInfo, &child);
    const auto error = GetLastError();
    if (output) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(write);
    }
    if (!started) {
        if (read)
            CloseHandle(read);
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Cannot start " + std::filesystem::path(name).string());
    }
    if (job)
        AssignProcessToJobObject(job, child.hProcess);
    ResumeThread(child.hThread);
    CloseHandle(child.hThread);
    process = child.hProcess;
    if (output) {
        // until the child and whatever it gave the pipe to are gone
        char buffer[8192];
        DWORD count = 0;
        while (ReadFile(read, buffer, sizeof buffer, &count, nullptr) && count)
            output({buffer, count});
        CloseHandle(read);
    }
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    process = nullptr;
    CloseHandle(child.hProcess);
    return code;
}
void end_process(void *process) {
    if (process)
        TerminateProcess(process, 0);
}
void end_now(std::uint32_t code) {
    TerminateProcess(GetCurrentProcess(), code);
    std::abort(); // not reached
}
std::string status_text(std::uint32_t code) {
    // the exceptions a title's code ends with, by the names the host's headers give them
    static constexpr std::pair<std::uint32_t, const char *> exceptions[] = {
        {EXCEPTION_ACCESS_VIOLATION, "Access violation"},
        {EXCEPTION_STACK_OVERFLOW, "Stack overflow"},
        {EXCEPTION_ILLEGAL_INSTRUCTION, "Illegal instruction"},
        {EXCEPTION_PRIV_INSTRUCTION, "Privileged instruction"},
        {EXCEPTION_INT_DIVIDE_BY_ZERO, "Integer division by zero"},
        {EXCEPTION_BREAKPOINT, "Breakpoint"},
        {EXCEPTION_IN_PAGE_ERROR, "In-page error"},
        {EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "Array bounds exceeded"},
        {EXCEPTION_DATATYPE_MISALIGNMENT, "Datatype misalignment"},
        {EXCEPTION_NONCONTINUABLE_EXCEPTION, "Noncontinuable exception"},
        {0xC0000409u, "Fail fast (stack buffer overrun or __fastfail)"},
        {0xC0000374u, "Heap corruption"},
        {0xE06D7363u, "Unhandled C++ exception"},
        {0xC000013Au, "Ended from its console (Ctrl+C)"},
    };
    for (const auto &[value, name] : exceptions)
        if (value == code)
            return name;
    // any other status: the host's own message for it, if it has no blanks to fill in
    wchar_t message[512];
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"ntdll.dll"), code, 0, message, 512, nullptr);
    std::wstring text(message, length);
    if (code < 0x80000000u || text.empty() || text.find(L'%') != std::wstring::npos)
        return {};
    // "{Title}" and line breaks
    if (text.front() == L'{')
        if (const auto end = text.find(L'}'); end != std::wstring::npos)
            text = text.substr(1, end - 1) + L". " + text.substr(end + 1);
    std::wstring line;
    for (const auto c : text)
        if (c != L'\r' && c != L'\n')
            line += c;
        else if (!line.empty() && line.back() != L' ')
            line += L' ';
    while (!line.empty() && line.back() == L' ')
        line.pop_back();
    const auto utf8 = std::filesystem::path(line).u8string();
    return {utf8.begin(), utf8.end()};
}
bool show_in_file_manager(const std::filesystem::path &path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error))
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr,
                                                       nullptr, SW_SHOWNORMAL)) > 32;
    const auto select = L"/select,\"" + path.wstring() + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                                   select.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}
std::filesystem::path create_desktop_shortcut(const std::wstring &name, const std::wstring &program,
                                              const std::wstring &arguments) {
    // what a file name may not have
    auto file = name;
    for (auto &c : file)
        if (c < 32 || std::wstring_view(L"<>:\"/\|?*").find(c) != std::wstring_view::npos)
            c = L'_';
    if (file.empty())
        return {};
    wchar_t *desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        return {};
    const auto shortcut = std::filesystem::path(desktop) / (file + L".lnk");
    CoTaskMemFree(desktop);
    const auto target = executable_directory() / program;
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IShellLinkW *link = nullptr;
    IPersistFile *saved = nullptr;
    const bool made = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(&link))) &&
                      SUCCEEDED(link->SetPath(target.c_str())) &&
                      SUCCEEDED(link->SetArguments(arguments.c_str())) &&
                      SUCCEEDED(link->SetWorkingDirectory(target.parent_path().c_str())) &&
                      SUCCEEDED(link->SetDescription(name.c_str())) &&
                      SUCCEEDED(link->SetShowCmd(SW_SHOWMINNOACTIVE)) &&
                      SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&saved))) &&
                      SUCCEEDED(saved->Save(shortcut.c_str(), TRUE));
    if (saved)
        saved->Release();
    if (link)
        link->Release();
    if (initialized)
        CoUninitialize();
    return made ? shortcut : std::filesystem::path{};
}
bool open_with_host(const std::filesystem::path &path) {
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
namespace {
void check_path(const std::filesystem::path &path) {
    if (path.empty() || path.native().find('\0') != std::filesystem::path::string_type::npos)
        throw std::invalid_argument("Expected nonempty path without embedded NUL");
}
std::size_t checked_size(std::uintmax_t bytes, std::size_t limit) {
    if (bytes > limit)
        throw std::length_error("File exceeds configured byte limit");
    return static_cast<std::size_t>(bytes);
}
[[noreturn]] void fail(const char *operation, DWORD error) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
class Handle {
  public:
    explicit Handle(HANDLE h, const char *operation) : value(h) {
        if (!h || h == INVALID_HANDLE_VALUE)
            fail(operation);
    }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE value;
};
Handle open_input(const std::filesystem::path &path) {
    check_path(path);
    return Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr),
                  "CreateFileW");
}
std::size_t input_size(const Handle &file, std::size_t limit) {
    if (GetFileType(file.value) != FILE_TYPE_DISK)
        throw std::invalid_argument("Expected a regular host file");
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.value, &size))
        fail("GetFileSizeEx");
    if (size.QuadPart < 0)
        throw std::runtime_error("Invalid host file size");
    return checked_size(static_cast<std::uintmax_t>(size.QuadPart), limit);
}
} // namespace

std::size_t page_size() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}
Reservation::Reservation(std::size_t bytes) : page_(page_size()) {
    if (!bytes || bytes > std::numeric_limits<std::size_t>::max() - (page_ - 1))
        throw std::invalid_argument("Invalid host reservation size");
    size_ = ((bytes + page_ - 1) / page_) * page_;
    address_ = static_cast<std::byte *>(VirtualAlloc(nullptr, size_, MEM_RESERVE, PAGE_NOACCESS));
    if (!address_)
        fail("VirtualAlloc reserve");
}
Reservation::~Reservation() { VirtualFree(address_, 0, MEM_RELEASE); }
void Reservation::commit() {
    if (committed_)
        throw std::logic_error("Reservation is already committed");
    if (!VirtualAlloc(address_, size_, MEM_COMMIT, PAGE_READWRITE))
        fail("VirtualAlloc commit");
    committed_ = true;
}
void Reservation::protect(std::size_t offset, std::size_t length, Access access) {
    if (!committed_)
        throw std::logic_error("Reservation is not committed");
    if (!length || offset % page_ || length % page_ || offset > size_ || length > size_ - offset)
        throw std::invalid_argument("Protection requires whole pages inside the reservation");
    DWORD flags, previous;
    switch (access) {
    case Access::none:
        flags = PAGE_NOACCESS;
        break;
    case Access::read:
        flags = PAGE_READONLY;
        break;
    case Access::read_write:
        flags = PAGE_READWRITE;
        break;
    case Access::read_execute:
        flags = PAGE_EXECUTE_READ;
        break;
    default:
        throw std::invalid_argument("Unknown host page access");
    }
    if (!VirtualProtect(address_ + offset, length, flags, &previous))
        fail("VirtualProtect");
}

namespace {
std::vector<std::uint8_t> read_leading(const Handle &file, std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        DWORD got;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - pos, 1 << 20));
        if (!ReadFile(file.value, bytes.data() + pos, chunk, &got, nullptr))
            fail("ReadFile");
        if (!got)
            throw std::runtime_error("Host file truncated during read");
        pos += got;
    }
    return bytes;
}
} // namespace
std::vector<std::uint8_t> read_snapshot(const std::filesystem::path &path, std::size_t limit) {
    const auto file = open_input(path);
    return read_leading(file, input_size(file, limit));
}
Prefix read_prefix(const std::filesystem::path &path, std::size_t count) {
    const auto file = open_input(path);
    const auto size = input_size(file, std::numeric_limits<std::size_t>::max());
    return {read_leading(file, std::min(size, count)), size};
}
FileMapping::FileMapping(const std::filesystem::path &path, std::size_t limit) {
    const auto file = open_input(path);
    const auto size = input_size(file, limit);
    if (size) {
        Handle mapping(CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr),
                       "CreateFileMappingW");
        const auto *address = MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, size);
        if (!address)
            fail("MapViewOfFile");
        bytes_ = {static_cast<const std::uint8_t *>(address), size};
    }
}
FileMapping::~FileMapping() {
    if (!bytes_.empty())
        UnmapViewOfFile(bytes_.data());
}

ThreadSlot::ThreadSlot() : key_(TlsAlloc()) {
    if (key_ == TLS_OUT_OF_INDEXES)
        fail("TlsAlloc");
}
ThreadSlot::~ThreadSlot() { TlsFree(key_); }
void ThreadSlot::set(void *value) {
    if (!TlsSetValue(key_, value))
        fail("TlsSetValue");
}
void *ThreadSlot::get() const {
    auto value = TlsGetValue(key_);
    if (!value) {
        if (DWORD error = GetLastError())
            fail("TlsGetValue", error);
    }
    return value;
}
void ManualEvent::signal() {
    {
        std::lock_guard lock(mutex_);
        signaled_ = true;
    }
    changed_.notify_all();
}
void ManualEvent::reset() {
    std::lock_guard lock(mutex_);
    signaled_ = false;
}
bool ManualEvent::wait(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    return changed_.wait(lock, stop, [this] { return signaled_; });
}

// timeBeginPeriod and timeEndPeriod, and the process power throttling that would ignore the
// request (SetProcessInformation, ProcessPowerThrottling), in Microsoft's Windows reference
TimerResolution::TimerResolution(std::uint32_t milliseconds) {
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0; // the request is always honoured
    // fails on hosts before Windows 11, which do not ignore it
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling,
                          sizeof throttling);
    if (timeBeginPeriod(milliseconds) == TIMERR_NOERROR)
        milliseconds_ = milliseconds;
}
// The throttling setting stays: it only concerns requests, and this one ends here.
TimerResolution::~TimerResolution() {
    if (milliseconds_)
        timeEndPeriod(milliseconds_);
}

NativeModule::NativeModule(const std::filesystem::path &path) {
    check_path(path);
    if (!path.is_absolute())
        throw std::invalid_argument("Trusted host module path must be absolute");
    handle_ = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle_)
        fail("LoadLibraryExW");
}
NativeModule::~NativeModule() { FreeLibrary(static_cast<HMODULE>(handle_)); }
NativeModule::Function NativeModule::function(const char *name) const {
    if (!name || !*name)
        throw std::invalid_argument("Expected a host function name");
    auto address = GetProcAddress(static_cast<HMODULE>(handle_), name);
    if (!address)
        fail("GetProcAddress");
    return std::bit_cast<Function>(address);
}
} // namespace xv::host
