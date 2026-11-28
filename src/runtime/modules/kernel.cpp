#include "runtime/modules/kernel.hpp"
#include "runtime/common/com_forward.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/locale.hpp"
#include "runtime/modules/placement.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>
#include <filesystem>
#include <intrin.h>
#include <map>
#include <mutex>
#include <share.h>
#include <shlwapi.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>
#include <windows.h>

#include <mmdeviceapi.h>
#include <objbase.h> // after windows.h

// The Win32 functions a console answers differently from the host: libraries that are ours
// or the title's, the console's drives, what a title writes for a debugger, and where it
// puts its threads. ERA titles import them from kernelx.dll, GDK titles from API sets.
namespace xv::runtime::modules {
namespace {
std::wstring console_path(const wchar_t *name);
// ours and the title's first, then the host's; a path on one of the console's drives is
// the host's for it
HMODULE WINAPI load_library(const wchar_t *name, HANDLE file, DWORD flags) {
    const auto path = console_path(name);
    if (name)
        name = path.c_str();
    auto module = static_cast<HMODULE>(detail::guest_library(name));
    if (!module)
        module = LoadLibraryExW(name, file, flags);
    const auto error = GetLastError();
    std::fprintf(stderr, "[Runtime] LoadLibraryEx(%ls) -> %p\n", name ? name : L"(null)",
                 static_cast<void *>(module));
    std::fflush(stderr);
    SetLastError(error);
    return module;
}
HMODULE WINAPI load_library_w(const wchar_t *name) { return load_library(name, nullptr, 0); }
HMODULE WINAPI load_packaged_library(const wchar_t *name, DWORD) {
    return load_library(name, nullptr, 0);
}
HMODULE WINAPI load_library_ex_a(const char *name, HANDLE file, DWORD flags) {
    if (!name)
        return load_library(nullptr, file, flags);
    const auto size = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 1)), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), size);
    return load_library(wide.c_str(), file, flags);
}
HMODULE WINAPI load_library_a(const char *name) { return load_library_ex_a(name, nullptr, 0); }
// A file opened without buffering is read on the host only at offsets, sizes and buffers its
// sectors align, and turned down otherwise (ERROR_INVALID_PARAMETER); the console reads it
// anyway, and at its end gives what is left, even nothing (a title may read the rest
// of a file's header from past its end). Such a read is made again through a handle of
// the same file with buffering, and ends as the title's would have.
BOOL read_buffered(HANDLE file, void *buffer, DWORD bytes, DWORD *read, OVERLAPPED *overlapped) {
    auto *const again =
        ReOpenFile(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (again == INVALID_HANDLE_VALUE)
        return FALSE;
    OVERLAPPED at{};
    if (overlapped)
        at.Offset = overlapped->Offset, at.OffsetHigh = overlapped->OffsetHigh;
    DWORD done = 0;
    const auto result = ReadFile(again, buffer, bytes, &done, overlapped ? &at : nullptr);
    const auto error = GetLastError();
    CloseHandle(again);
    if (!result && error != ERROR_HANDLE_EOF) {
        SetLastError(error);
        return FALSE;
    }
    if (read)
        *read = done;
    if (overlapped) {
        overlapped->Internal = 0; // STATUS_SUCCESS
        overlapped->InternalHigh = done;
        if (overlapped->hEvent)
            SetEvent(overlapped->hEvent);
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}
// reads into memory placement resources live in
BOOL WINAPI read_file(HANDLE file, void *buffer, DWORD bytes, DWORD *read, OVERLAPPED *overlapped) {
    const TitleWrite writing(buffer, bytes);
    auto result = ReadFile(file, buffer, bytes, read, overlapped);
    if (!result && GetLastError() == ERROR_INVALID_PARAMETER) {
        if (static std::atomic<int> shown{0}; shown++ < 4) {
            std::fprintf(stderr,
                         "[Runtime] a read of a file without buffering the host turns down (%lu "
                         "bytes at %llx) is made with buffering\n",
                         bytes,
                         overlapped ? ULONGLONG{overlapped->OffsetHigh} << 32 | overlapped->Offset
                                    : 0);
            std::fflush(stderr);
        }
        result = read_buffered(file, buffer, bytes, read, overlapped);
    }
    // XV_FILE_TRACE: the reads at an offset and those the host turns down, with what came of
    // each. A read that goes well leaves the last error as it was, so only one that fails has
    // an error to show; one of a handle opened for overlapped reads may still be pending.
    if (static const bool trace = std::getenv("XV_FILE_TRACE") != nullptr;
        trace && (overlapped || !result)) {
        const auto error = GetLastError();
        char outcome[32];
        if (result)
            std::snprintf(outcome, sizeof outcome, "%lu bytes read",
                          read         ? *read
                          : overlapped ? static_cast<DWORD>(overlapped->InternalHigh)
                                       : 0);
        else if (error == ERROR_IO_PENDING)
            std::snprintf(outcome, sizeof outcome, "pending");
        else
            std::snprintf(outcome, sizeof outcome, "error %lu", error);
        std::fprintf(stderr, "[Runtime] ReadFile(%p, %lu bytes at %llx, buffer %p): %s\n", file,
                     bytes,
                     overlapped ? ULONGLONG{overlapped->OffsetHigh} << 32 | overlapped->Offset : 0,
                     buffer, outcome);
        std::fflush(stderr);
        SetLastError(error);
    }
    return result;
}
BOOL WINAPI read_file_ex(HANDLE file, void *buffer, DWORD bytes, OVERLAPPED *overlapped,
                         LPOVERLAPPED_COMPLETION_ROUTINE completion) {
    prepare_title_write(buffer, bytes);
    return ReadFileEx(file, buffer, bytes, overlapped, completion);
}
// Music is streamed from one file per track, so the files a title opens tell which track it
// starts and when. Worth a line each: there are few of them.
void note_music(const wchar_t *name, HANDLE file) {
    if (!name || !StrStrIW(name, L".binka"))
        return;
    const auto error = GetLastError();
    std::fprintf(stderr, "[Runtime] music: opens %ls%s\n", name,
                 file == INVALID_HANDLE_VALUE ? " (failed)" : "");
    std::fflush(stderr);
    SetLastError(error);
}
// The console's drives: G:\ (and the GAME:\ device name) is the title's package, T:\ its
// temporary storage, kept with its saves. Other paths are the host's.
std::wstring console_path(const wchar_t *name) {
    if (!name)
        return {};
    const std::wstring_view path(name);
    const auto drive = [&](std::wstring_view prefix) {
        return path.size() >= prefix.size() &&
               CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()), prefix.data(),
                                    static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL &&
               (path.size() == prefix.size() || path[prefix.size()] == L'\\' ||
                path[prefix.size()] == L'/');
    };
    const auto after_drive = [&](std::size_t colon) {
        const auto pos = colon + 1;
        return pos < path.size() && (path[pos] == L'\\' || path[pos] == L'/') ? path.substr(pos + 1)
                                                                              : path.substr(pos);
    };
    std::error_code error;
    if (drive(L"G:") || drive(L"GAME:")) {
        // nothing is above the root of a drive: G:\..\data is G:\data
        std::filesystem::path rest;
        for (const auto &part :
             std::filesystem::path(after_drive(path.find(L':'))).lexically_normal())
            if (part != L"..")
                rest /= part;
        const auto joined = package_directory(std::filesystem::current_path(error)) / rest;
        return joined.wstring();
    }
    if (drive(L"T:") && !save_directory().empty()) {
        const auto folder = save_directory() / "Temp";
        std::filesystem::create_directories(folder, error);
        const auto tail = after_drive(1);
        return tail.empty() ? folder.wstring() : (folder / tail).wstring();
    }
    // C:\Users\DefaultUser and C:\Users\<Xbox user id> are where a GDK title keeps what it
    // writes on the console. On the host they are another user's folders, or nobody's.
    if (drive(L"C:") && path.size() > 9 &&
        CompareStringOrdinal(path.data() + 3, 5, L"Users", 5, TRUE) == CSTR_EQUAL &&
        (path[8] == L'\\' || path[8] == L'/')) {
        const auto rest = path.substr(9);
        const auto name = rest.substr(0, rest.find_first_of(L"\\/"));
        const bool user_id = !name.empty() && name.find_first_not_of(L"0123456789") == name.npos;
        if (user_id || CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                            L"DefaultUser", 11, TRUE) == CSTR_EQUAL) {
            const auto folder = save_directory() / "Users";
            std::filesystem::create_directories(folder / name, error);
            return (folder / rest).wstring();
        }
    }
    // from the root of the current drive, which is the package's on the console
    if (path.size() > 1 && (path[0] == L'\\' || path[0] == L'/') && path[1] != L'\\' &&
        path[1] != L'/')
        return (package_directory(std::filesystem::current_path(error)) / path.substr(1)).wstring();
    // A title that takes the root of its drive for its package (which it is on the console)
    // names files of the package from the root of the host's drive.
    if (path.size() > 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        const auto package = package_directory(std::filesystem::current_path(error));
        const auto root = package.root_name().wstring();
        if (root.size() == 2 && towupper(root[0]) == towupper(path[0]) &&
            GetFileAttributesW(name) == INVALID_FILE_ATTRIBUTES) {
            auto inside = (package / path.substr(3)).wstring();
            if (GetFileAttributesW(inside.c_str()) != INVALID_FILE_ATTRIBUTES)
                return inside;
        }
    }
    // A relative path whose `..` climbs above the package's root (as UE3's `../../Engine/`
    // does on a console where the package is `G:\`): clamped at the package's root when the
    // host has nothing at the unclamped path.
    if (path.find(L':') == std::wstring_view::npos && !path.empty() && path[0] != L'\\' &&
        path[0] != L'/' && path.find(L"..") != std::wstring_view::npos &&
        GetFileAttributesW(name) == INVALID_FILE_ATTRIBUTES) {
        const auto cwd = std::filesystem::current_path(error);
        const auto package = package_directory(cwd);
        std::error_code rel_err;
        auto rel = std::filesystem::relative(cwd, package, rel_err);
        if (rel_err || rel.native().starts_with(L".."))
            rel.clear();
        std::filesystem::path clamped;
        for (const auto &part : (rel / path).lexically_normal())
            if (part != L".." && part != L".")
                clamped /= part;
        const auto candidate = (package / clamped).wstring();
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    // Any other drive of the console is a folder of its own, kept apart for each title:
    // nand\<title>\disk_<letter> in the configuration folder. Paths the host handed out
    // stay the host's: the title's dump, the user's folders, the system's.
    if (path.size() >= 2 && path[1] == L':' &&
        (path.size() == 2 || path[2] == L'\\' || path[2] == L'/') && iswalpha(path[0]) &&
        !save_directory().empty()) {
        static const std::vector<std::wstring> hosts = [] {
            std::vector<std::wstring> folders;
            std::error_code ignored;
            folders.push_back(
                package_directory(std::filesystem::current_path(ignored)).parent_path().wstring());
            // profiles\<profile>\saves\<title>, in the configuration folder
            folders.push_back(
                save_directory().parent_path().parent_path().parent_path().parent_path().wstring());
            for (const auto *variable : {L"USERPROFILE", L"SystemRoot", L"ProgramData", L"TEMP"})
                if (const auto *value = _wgetenv(variable); value && *value)
                    folders.emplace_back(value);
            return folders;
        }();
        std::wstring plain(path);
        std::replace(plain.begin(), plain.end(), L'/', L'\\');
        const auto inside = [&](const std::wstring &folder) {
            return plain.size() >= folder.size() &&
                   CompareStringOrdinal(plain.data(), static_cast<int>(folder.size()),
                                        folder.data(), static_cast<int>(folder.size()),
                                        TRUE) == CSTR_EQUAL;
        };
        if (std::none_of(hosts.begin(), hosts.end(), inside)) {
            const auto disk = std::filesystem::path(hosts[1]) / "nand" /
                              save_directory().filename() /
                              (std::wstring(L"disk_") + static_cast<wchar_t>(towlower(path[0])));
            std::filesystem::create_directories(disk, error);
            return path.size() > 3 ? (disk / path.substr(3)).wstring() : disk.wstring();
        }
    }
    return std::wstring(path);
}
// the same for the ANSI functions, whose results stay in the title's code page
std::string console_path(const char *name) {
    if (!name)
        return {};
    const auto size = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 1)), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), size);
    const auto path = console_path(wide.c_str());
    const auto bytes =
        WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(std::max(bytes, 1)), '\0');
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, narrow.data(), bytes, nullptr, nullptr);
    narrow.resize(std::strlen(narrow.c_str()));
    return narrow;
}

// XV_FILE_TRACE: every file the title looks for and does not find, to see what content it
// expects where
void note_missing(const char *call, const wchar_t *name, bool failed) {
    static const bool enabled = std::getenv("XV_FILE_TRACE") != nullptr;
    if (!enabled || !failed)
        return;
    const auto error = GetLastError();
    std::fprintf(stderr, "[Runtime] %s(%ls): error %lu\n", call, name ? name : L"(null)", error);
    std::fflush(stderr);
    SetLastError(error);
}
HANDLE WINAPI create_file_w(const wchar_t *name, DWORD access, DWORD share,
                            SECURITY_ATTRIBUTES *security, DWORD creation, DWORD flags,
                            HANDLE model) {
    const auto file =
        CreateFileW(console_path(name).c_str(), access, share, security, creation, flags, model);
    note_music(name, file);
    note_missing("CreateFileW", name, file == INVALID_HANDLE_VALUE);
    return file;
}
HANDLE WINAPI create_file2(const wchar_t *name, DWORD access, DWORD share, DWORD creation,
                           CREATEFILE2_EXTENDED_PARAMETERS *parameters) {
    const auto file = CreateFile2(console_path(name).c_str(), access, share, creation, parameters);
    note_music(name, file);
    note_missing("CreateFile2", name, file == INVALID_HANDLE_VALUE);
    return file;
}
BOOL WINAPI get_file_attributes_ex_w(const wchar_t *name, GET_FILEEX_INFO_LEVELS level,
                                     void *information) {
    const auto found = GetFileAttributesExW(console_path(name).c_str(), level, information);
    note_missing("GetFileAttributesExW", name, !found);
    return found;
}
DWORD WINAPI get_file_attributes_w(const wchar_t *name) {
    const auto attributes = GetFileAttributesW(console_path(name).c_str());
    note_missing("GetFileAttributesW", name, attributes == INVALID_FILE_ATTRIBUTES);
    return attributes;
}
HANDLE WINAPI find_first_file_ex_w(const wchar_t *name, FINDEX_INFO_LEVELS level, void *data,
                                   FINDEX_SEARCH_OPS search, void *filter, DWORD flags) {
    const auto find =
        FindFirstFileExW(console_path(name).c_str(), level, data, search, filter, flags);
    note_missing("FindFirstFileExW", name, find == INVALID_HANDLE_VALUE);
    return find;
}
HANDLE WINAPI find_first_file_w(const wchar_t *name, WIN32_FIND_DATAW *data) {
    const auto find = FindFirstFileW(console_path(name).c_str(), data);
    note_missing("FindFirstFileW", name, find == INVALID_HANDLE_VALUE);
    return find;
}
void note_missing(const char *call, const char *name, bool failed) {
    const auto error = GetLastError();
    const std::wstring wide(name ? name : "", name ? name + std::strlen(name) : "");
    SetLastError(error);
    note_missing(call, wide.c_str(), failed);
}
HANDLE WINAPI create_file_a(const char *name, DWORD access, DWORD share,
                            SECURITY_ATTRIBUTES *security, DWORD creation, DWORD flags,
                            HANDLE model) {
    const auto file =
        CreateFileA(console_path(name).c_str(), access, share, security, creation, flags, model);
    note_missing("CreateFileA", name, file == INVALID_HANDLE_VALUE);
    return file;
}
BOOL WINAPI get_file_attributes_ex_a(const char *name, GET_FILEEX_INFO_LEVELS level,
                                     void *information) {
    const auto found = GetFileAttributesExA(console_path(name).c_str(), level, information);
    note_missing("GetFileAttributesExA", name, !found);
    return found;
}
DWORD WINAPI get_file_attributes_a(const char *name) {
    const auto attributes = GetFileAttributesA(console_path(name).c_str());
    note_missing("GetFileAttributesA", name, attributes == INVALID_FILE_ATTRIBUTES);
    return attributes;
}
HANDLE WINAPI find_first_file_ex_a(const char *name, FINDEX_INFO_LEVELS level, void *data,
                                   FINDEX_SEARCH_OPS search, void *filter, DWORD flags) {
    const auto find =
        FindFirstFileExA(console_path(name).c_str(), level, data, search, filter, flags);
    note_missing("FindFirstFileExA", name, find == INVALID_HANDLE_VALUE);
    return find;
}
HANDLE WINAPI find_first_file_a(const char *name, WIN32_FIND_DATAA *data) {
    const auto find = FindFirstFileA(console_path(name).c_str(), data);
    note_missing("FindFirstFileA", name, find == INVALID_HANDLE_VALUE);
    return find;
}
BOOL WINAPI create_directory_w(const wchar_t *name, SECURITY_ATTRIBUTES *security) {
    return CreateDirectoryW(console_path(name).c_str(), security);
}
BOOL WINAPI remove_directory_w(const wchar_t *name) {
    return RemoveDirectoryW(console_path(name).c_str());
}
BOOL WINAPI delete_file_w(const wchar_t *name) { return DeleteFileW(console_path(name).c_str()); }
BOOL WINAPI move_file_ex_w(const wchar_t *from, const wchar_t *to, DWORD flags) {
    return MoveFileExW(console_path(from).c_str(), to ? console_path(to).c_str() : nullptr, flags);
}
BOOL WINAPI set_file_attributes_w(const wchar_t *name, DWORD attributes) {
    return SetFileAttributesW(console_path(name).c_str(), attributes);
}
// the ANSI ones, which titles of the older kits use for their own folders on the drives
BOOL WINAPI create_directory_a(const char *name, SECURITY_ATTRIBUTES *security) {
    return CreateDirectoryA(console_path(name).c_str(), security);
}
BOOL WINAPI remove_directory_a(const char *name) {
    return RemoveDirectoryA(console_path(name).c_str());
}
BOOL WINAPI delete_file_a(const char *name) { return DeleteFileA(console_path(name).c_str()); }
BOOL WINAPI move_file_ex_a(const char *from, const char *to, DWORD flags) {
    return MoveFileExA(console_path(from).c_str(), to ? console_path(to).c_str() : nullptr, flags);
}
BOOL WINAPI set_file_attributes_a(const char *name, DWORD attributes) {
    return SetFileAttributesA(console_path(name).c_str(), attributes);
}
// Free space of a console drive: that of the host's volume the drive's folder is on. The
// host wants a directory that exists; the nearest parent that does is asked.
std::wstring existing_directory(const wchar_t *name) {
    std::error_code error;
    std::filesystem::path path = name ? console_path(name) : std::wstring();
    if (path.empty())
        path = std::filesystem::current_path(error);
    while (!std::filesystem::is_directory(path, error) && path.has_parent_path() &&
           path.parent_path() != path)
        path = path.parent_path();
    return path.wstring();
}
BOOL WINAPI get_disk_free_space_ex_w(const wchar_t *name, ULARGE_INTEGER *available,
                                     ULARGE_INTEGER *total, ULARGE_INTEGER *free) {
    return GetDiskFreeSpaceExW(existing_directory(name).c_str(), available, total, free);
}
BOOL WINAPI get_disk_free_space_w(const wchar_t *name, DWORD *sectors, DWORD *bytes,
                                  DWORD *free_clusters, DWORD *clusters) {
    return GetDiskFreeSpaceW(existing_directory(name).c_str(), sectors, bytes, free_clusters,
                             clusters);
}
// The C runtime's file functions open files without going through the functions above, so
// the console's drives are put in their paths here as well.
// They are the functions of the C runtime the caller uses, not of this program's: a FILE of
// one runtime means nothing to another's fread. Which runtime that is, the caller's image
// says in its imports: msvcr110 or msvcr120 for titles of the older kits, the system's
// universal one for the newer. A process may hold several (a title and its middleware).
HMODULE crt_of(void *caller) {
    static std::mutex mutex;
    static std::map<void *, HMODULE> known;
    auto *const image = static_cast<std::uint8_t *>(detail::title_image_base(caller));
    const std::lock_guard lock(mutex);
    if (const auto found = known.find(image); found != known.end())
        return found->second;
    HMODULE runtime = nullptr;
    if (image) {
        const auto *const headers = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
            image + *reinterpret_cast<LONG *>(image + 0x3c));
        const auto &directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (auto *entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(
                 image + directory.VirtualAddress);
             directory.VirtualAddress && entry->Name && !runtime; ++entry) {
            const auto *const name = reinterpret_cast<const char *>(image + entry->Name);
            if (_strnicmp(name, "msvcr", 5) == 0)
                runtime = GetModuleHandleA(name);
            else if (_strnicmp(name, "api-ms-win-crt-", 15) == 0 ||
                     _stricmp(name, "ucrtbase.dll") == 0)
                runtime = GetModuleHandleA("ucrtbase.dll");
        }
    }
    known[image] = runtime;
    return runtime;
}
template <class Function> Function *title_crt(void *caller, const char *name, Function *own) {
    if (const auto runtime = crt_of(caller))
        if (const auto found = GetProcAddress(runtime, name))
            return reinterpret_cast<Function *>(found);
    return own;
}
#define XV_TITLE_CRT(function) title_crt(_ReturnAddress(), #function, &function)
FILE *crt_wfopen(const wchar_t *name, const wchar_t *mode) {
    return XV_TITLE_CRT(_wfsopen)(console_path(name).c_str(), mode, _SH_DENYNO);
}
errno_t crt_wfopen_s(FILE **file, const wchar_t *name, const wchar_t *mode) {
    return XV_TITLE_CRT(_wfopen_s)(file, console_path(name).c_str(), mode);
}
FILE *crt_wfsopen(const wchar_t *name, const wchar_t *mode, int share) {
    return XV_TITLE_CRT(_wfsopen)(console_path(name).c_str(), mode, share);
}
FILE *crt_fopen(const char *name, const char *mode) {
    return XV_TITLE_CRT(_fsopen)(console_path(name).c_str(), mode, _SH_DENYNO);
}
errno_t crt_fopen_s(FILE **file, const char *name, const char *mode) {
    return XV_TITLE_CRT(fopen_s)(file, console_path(name).c_str(), mode);
}
FILE *crt_fsopen(const char *name, const char *mode, int share) {
    return XV_TITLE_CRT(_fsopen)(console_path(name).c_str(), mode, share);
}
int crt_wstat64(const wchar_t *name, struct _stat64 *status) {
    return XV_TITLE_CRT(_wstat64)(console_path(name).c_str(), status);
}
int crt_stat64i32(const char *name, struct _stat64i32 *status) {
    return XV_TITLE_CRT(_stat64i32)(console_path(name).c_str(), status);
}
int crt_remove(const char *name) { return XV_TITLE_CRT(remove)(console_path(name).c_str()); }
int crt_wremove(const wchar_t *name) { return XV_TITLE_CRT(_wremove)(console_path(name).c_str()); }
int crt_wrename(const wchar_t *from, const wchar_t *to) {
    return XV_TITLE_CRT(_wrename)(console_path(from).c_str(), console_path(to).c_str());
}
int crt_wrmdir(const wchar_t *name) { return XV_TITLE_CRT(_wrmdir)(console_path(name).c_str()); }
int crt_wmkdir(const wchar_t *name) { return XV_TITLE_CRT(_wmkdir)(console_path(name).c_str()); }
#undef XV_TITLE_CRT
// What a title writes for a debugger: its own log, which is what says why it gives up.
// Lines go to stderr as they come, without the trailing newline titles usually add.
void WINAPI output_debug_string_w(const wchar_t *text) {
    if (!text)
        return;
    std::wstring line = text;
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r'))
        line.pop_back();
    std::fprintf(stderr, "[Title] %ls\n", line.c_str());
    std::fflush(stderr);
}
void WINAPI output_debug_string_a(const char *text) {
    if (!text)
        return;
    std::string line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    std::fprintf(stderr, "[Title] %s\n", line.c_str());
    std::fflush(stderr);
}
BOOL WINAPI set_threadpool_affinity_mask(PTP_POOL, DWORD_PTR) { return TRUE; }
// XV_THREAD_TRACE: where the title puts its threads
DWORD_PTR WINAPI set_thread_affinity_mask(HANDLE thread, DWORD_PTR mask) {
    auto previous = SetThreadAffinityMask(thread, mask);
    // processors the console has and the host has not: the thread stays where it is
    if (DWORD_PTR process = 0, system = 0;
        !previous && GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
        previous = process;
    if (std::getenv("XV_THREAD_TRACE")) {
        std::fprintf(stderr, "[Runtime] SetThreadAffinityMask(%lu, 0x%llx) -> 0x%llx from %s\n",
                     GetThreadId(thread), static_cast<unsigned long long>(mask),
                     static_cast<unsigned long long>(previous),
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return previous;
}
DWORD WINAPI get_current_processor_number() {
    const auto number = GetCurrentProcessorNumber();
    if (static std::atomic<int> shown{0}; std::getenv("XV_THREAD_TRACE") && shown++ < 12) {
        std::fprintf(stderr, "[Runtime] GetCurrentProcessorNumber -> %lu from %s\n", number,
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return number;
}
void WINAPI get_system_info(SYSTEM_INFO *info) {
    GetSystemInfo(info);
    if (std::getenv("XV_THREAD_TRACE")) {
        std::fprintf(stderr, "[Runtime] GetSystemInfo: %lu processors, from %s\n",
                     info->dwNumberOfProcessors, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
}
FARPROC WINAPI get_proc_address_of(HMODULE module, const char *name) {
    auto function = reinterpret_cast<FARPROC>(detail::guest_symbol(module, name));
    if (!function)
        function = GetProcAddress(module, name);
    const auto error = GetLastError();
    static std::atomic<unsigned> logged{0};
    if (!function || logged.fetch_add(1, std::memory_order_relaxed) < 64 ||
        std::getenv("XV_FILE_TRACE")) {
        const bool ordinal = reinterpret_cast<std::uintptr_t>(name) < 0x10000;
        std::fprintf(stderr, "[Runtime] GetProcAddress(%s%s) -> %p\n", ordinal ? "#" : name,
                     ordinal ? std::to_string(reinterpret_cast<std::uintptr_t>(name)).c_str() : "",
                     reinterpret_cast<void *>(function));
        std::fflush(stderr);
    }
    SetLastError(error);
    return function;
}
// a library of ours has nothing to free, and the host knows nothing of the title's
BOOL WINAPI free_library(HMODULE module) {
    if (!detail::in_title(module))
        FreeLibrary(module);
    return TRUE;
}
// Which console this is. The GDK's XSystemGetDeviceType reads it from the registry, the
// ConsoleMode value of this key, the console's hardware revision: 0x10 to 0x31 an Xbox One,
// 0x40 a One S, 0x50 a One X, 0x60 a Series S, 0x64 a Series X. Without the key the title is
// told it runs on a PC, which a console build has no case for.
Target console = Target::durango;
const HKEY console_key = reinterpret_cast<HKEY>(static_cast<std::uintptr_t>(0x58530001));
constexpr wchar_t oem_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
LSTATUS WINAPI reg_open_key_ex_w(HKEY root, const wchar_t *name, DWORD options, REGSAM access,
                                 HKEY *key) {
    if (root == HKEY_LOCAL_MACHINE && name && key && !_wcsicmp(name, oem_key)) {
        *key = console_key;
        return ERROR_SUCCESS;
    }
    return RegOpenKeyExW(root, name, options, access, key);
}
LSTATUS WINAPI reg_open_key_w(HKEY root, const wchar_t *name, HKEY *key) {
    return reg_open_key_ex_w(root, name, 0, KEY_READ, key);
}
LSTATUS WINAPI reg_query_value_ex_w(HKEY key, const wchar_t *name, DWORD *reserved, DWORD *type,
                                    BYTE *data, DWORD *size) {
    if (key != console_key)
        return RegQueryValueExW(key, name, reserved, type, data, size);
    if (!name || _wcsicmp(name, L"ConsoleMode"))
        return ERROR_FILE_NOT_FOUND;
    constexpr DWORD modes[]{0x10, 0x40, 0x50, 0x60, 0x64}; // as ConsoleModel
    const DWORD mode = modes[static_cast<int>(console_model())];
    if (type)
        *type = REG_DWORD;
    const bool fits = !data || (size && *size >= sizeof mode);
    if (data && fits)
        std::memcpy(data, &mode, sizeof mode);
    if (size)
        *size = sizeof mode;
    return fits ? ERROR_SUCCESS : ERROR_MORE_DATA;
}
LSTATUS WINAPI reg_close_key(HKEY key) {
    return key == console_key ? ERROR_SUCCESS : RegCloseKey(key);
}
// COM classes of the host's: which one a title asked for and did not get
// The console's device enumerator has an interface the host's lacks
// ({8B557ADC-555E-47A0-B223-43477E481DAD}). A title calls its method 8 with two counters
// and takes the larger for the channels of its output: stereo here. The other methods are
// not known.
struct ConsoleAudioInfo {
    void **vtable;
    static HRESULT STDMETHODCALLTYPE query(ConsoleAudioInfo *self, const GUID *, void **object) {
        *object = self;
        return S_OK;
    }
    static ULONG STDMETHODCALLTYPE reference(ConsoleAudioInfo *) { return 1; }
    static HRESULT STDMETHODCALLTYPE unknown(ConsoleAudioInfo *) { return E_NOTIMPL; }
    static HRESULT STDMETHODCALLTYPE channels(ConsoleAudioInfo *, UINT *first, UINT *second) {
        *first = *second = 2;
        return S_OK;
    }
};
constexpr GUID console_audio_info{
    0x8b557adc, 0x555e, 0x47a0, {0xb2, 0x23, 0x43, 0x47, 0x7e, 0x48, 0x1d, 0xad}};
// Another class of the console's device API ({730C5326-...} as {15B673D7-...}), by a
// title's calls: its first method gives a device (T **), which the title activates an
// audio client on. The host's default output stands for it.
struct ConsoleAudioOutput {
    void **vtable;
    static HRESULT STDMETHODCALLTYPE query(ConsoleAudioOutput *self, const GUID *, void **object) {
        *object = self;
        return S_OK;
    }
    static ULONG STDMETHODCALLTYPE reference(ConsoleAudioOutput *) { return 1; }
    static HRESULT STDMETHODCALLTYPE unknown(ConsoleAudioOutput *) { return E_NOTIMPL; }
    static HRESULT STDMETHODCALLTYPE device(ConsoleAudioOutput *, IMMDevice **device) {
        if (!device)
            return E_POINTER;
        *device = nullptr;
        IMMDeviceEnumerator *devices = nullptr;
        auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&devices));
        if (SUCCEEDED(result)) {
            result = devices->GetDefaultAudioEndpoint(eRender, eConsole, device);
            devices->Release();
        }
        return result;
    }
};
constexpr GUID console_audio_output{
    0x15b673d7, 0xbd21, 0x4441, {0xa3, 0x0e, 0x58, 0xea, 0xaa, 0xd3, 0x1e, 0x73}};
HRESULT WINAPI co_create_instance(const GUID *class_id, IUnknown *outer, DWORD context,
                                  const GUID *iid, void **object) {
    if (*iid == console_audio_output && object) {
        static void *vtable[16];
        static ConsoleAudioOutput output{[] {
            for (auto &slot : vtable)
                slot = reinterpret_cast<void *>(&ConsoleAudioOutput::unknown);
            vtable[0] = reinterpret_cast<void *>(&ConsoleAudioOutput::query);
            vtable[1] = vtable[2] = reinterpret_cast<void *>(&ConsoleAudioOutput::reference);
            vtable[3] = reinterpret_cast<void *>(&ConsoleAudioOutput::device);
            return vtable;
        }()};
        *object = &output;
        return S_OK;
    }
    const auto audio_info = [] {
        static void *vtable[16];
        static ConsoleAudioInfo info{[] {
            for (auto &slot : vtable)
                slot = reinterpret_cast<void *>(&ConsoleAudioInfo::unknown);
            vtable[0] = reinterpret_cast<void *>(&ConsoleAudioInfo::query);
            vtable[1] = vtable[2] = reinterpret_cast<void *>(&ConsoleAudioInfo::reference);
            vtable[8] = reinterpret_cast<void *>(&ConsoleAudioInfo::channels);
            return vtable;
        }()};
        return &info;
    };
    if (*iid == console_audio_info && object) {
        *object = audio_info();
        return S_OK;
    }
    auto result = CoCreateInstance(*class_id, outer, context, *iid, object);
    // a console title's threads have COM without asking for it
    if (result == CO_E_NOTINITIALIZED && SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        result = CoCreateInstance(*class_id, outer, context, *iid, object);
    // the host's device enumerator, which a title may ask for that interface later
    if (SUCCEEDED(result) && object && *iid == __uuidof(IMMDeviceEnumerator)) {
        static ConsoleAudioInfo *info;
        info = audio_info();
        *object = make_forwarder(static_cast<IUnknown *>(*object), 8,
                                 +[](Forwarder *self, REFIID wanted, void **out) -> HRESULT {
                                     if (wanted == console_audio_info) {
                                         *out = info;
                                         return S_OK;
                                     }
                                     if (wanted != __uuidof(IUnknown) &&
                                         wanted != __uuidof(IMMDeviceEnumerator))
                                         return forwarder_host(self)->QueryInterface(wanted, out);
                                     forwarder_add_ref(self);
                                     *out = self;
                                     return S_OK;
                                 },
                                 {});
    }
    if (FAILED(result) || std::getenv("XV_COM_TRACE")) {
        wchar_t text[64] = L"", wanted[64] = L"";
        StringFromGUID2(*class_id, text, 64);
        StringFromGUID2(*iid, wanted, 64);
        std::fprintf(stderr, "[Runtime] CoCreateInstance(%ls) as %ls: 0x%08lx\n", text, wanted,
                     static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    return result;
}
// RtlPcToFileHeader for the title's own modules, which the host's loader does not know:
// the C++ runtime a title brings (std::exception_ptr in msvcp140) asks for the image a
// thrown type is in
void *WINAPI pc_to_file_header(void *address, void **base) {
    auto *const found = detail::title_image_base(address);
    if (!found)
        return RtlPcToFileHeader(address, base);
    if (base)
        *base = found;
    return found;
}
template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
const ApiFunction functions[] = {{"OutputDebugStringW", erase(output_debug_string_w)},
                                 {"OutputDebugStringA", erase(output_debug_string_a)},
                                 {"SetThreadpoolAffinityMask", erase(set_threadpool_affinity_mask)},
                                 {"CreateFileW", erase(create_file_w)},
                                 {"CreateFile2", erase(create_file2)},
                                 {"GetFileAttributesExW", erase(get_file_attributes_ex_w)},
                                 {"GetFileAttributesW", erase(get_file_attributes_w)},
                                 {"FindFirstFileExW", erase(find_first_file_ex_w)},
                                 {"FindFirstFileW", erase(find_first_file_w)},
                                 {"SetThreadAffinityMask", erase(set_thread_affinity_mask)},
                                 {"GetCurrentProcessorNumber", erase(get_current_processor_number)},
                                 {"GetSystemInfo", erase(get_system_info)},
                                 {"CreateFileA", erase(create_file_a)},
                                 {"GetFileAttributesExA", erase(get_file_attributes_ex_a)},
                                 {"GetFileAttributesA", erase(get_file_attributes_a)},
                                 {"FindFirstFileExA", erase(find_first_file_ex_a)},
                                 {"FindFirstFileA", erase(find_first_file_a)},
                                 {"CreateDirectoryW", erase(create_directory_w)},
                                 {"RemoveDirectoryW", erase(remove_directory_w)},
                                 {"DeleteFileW", erase(delete_file_w)},
                                 {"MoveFileExW", erase(move_file_ex_w)},
                                 {"SetFileAttributesW", erase(set_file_attributes_w)},
                                 {"CreateDirectoryA", erase(create_directory_a)},
                                 {"RemoveDirectoryA", erase(remove_directory_a)},
                                 {"DeleteFileA", erase(delete_file_a)},
                                 {"MoveFileExA", erase(move_file_ex_a)},
                                 {"SetFileAttributesA", erase(set_file_attributes_a)},
                                 {"GetDiskFreeSpaceExW", erase(get_disk_free_space_ex_w)},
                                 {"GetDiskFreeSpaceW", erase(get_disk_free_space_w)},
                                 {"_wfopen", erase(crt_wfopen)},
                                 {"_wfopen_s", erase(crt_wfopen_s)},
                                 {"_wfsopen", erase(crt_wfsopen)},
                                 {"fopen", erase(crt_fopen)},
                                 {"fopen_s", erase(crt_fopen_s)},
                                 {"_fsopen", erase(crt_fsopen)},
                                 {"_wstat64", erase(crt_wstat64)},
                                 {"_stat64i32", erase(crt_stat64i32)},
                                 {"remove", erase(crt_remove)},
                                 {"_wremove", erase(crt_wremove)},
                                 {"_wrename", erase(crt_wrename)},
                                 {"_wrmdir", erase(crt_wrmdir)},
                                 {"_wmkdir", erase(crt_wmkdir)},
                                 {"LoadLibraryExW", erase(load_library)},
                                 {"LoadLibraryW", erase(load_library_w)},
                                 {"LoadLibraryExA", erase(load_library_ex_a)},
                                 {"LoadLibraryA", erase(load_library_a)},
                                 {"LoadPackagedLibrary", erase(load_packaged_library)},
                                 {"GetProcAddress", erase(get_proc_address_of)},
                                 {"FreeLibrary", erase(free_library)},
                                 {"ReadFile", erase(read_file)},
                                 {"ReadFileEx", erase(read_file_ex)}};
} // namespace

std::span<const ApiFunction> kernel_functions() { return functions; }
FARPROC get_proc_address(HMODULE module, const char *name) {
    return get_proc_address_of(module, name);
}
std::wstring console_file_path(const wchar_t *path) { return console_path(path); }

void install_kernel(Registry &registry, Target target) {
    console = target;
    detail::set_console_target(target);
    for (const auto &[name, function] : functions)
        registry.add_api(name, function);
    install_locale(registry);
    registry.add_api("RegOpenKeyExW", erase(reg_open_key_ex_w));
    registry.add_api("RegOpenKeyW", erase(reg_open_key_w));
    registry.add_api("RegQueryValueExW", erase(reg_query_value_ex_w));
    registry.add_api("RegCloseKey", erase(reg_close_key));
    registry.add_api("CoCreateInstance", erase(co_create_instance));
    // not in the list of APIs a title may import, and imported all the same. Not an entry
    // of kernel32.dll: a module with entries is ours to LoadLibrary, and this one is not.
    registry.add_api("__C_specific_handler", erase(__C_specific_handler));
    // Series only: with it the first-kit Xbox One title stops presenting after a few frames,
    // for a reason not looked into
    if (target == Target::scarlett)
        registry.add_api("RtlPcToFileHeader", erase(pc_to_file_header));
    // system DLLs the libraries a GDK title ships with (its HTTP client, the C++ runtime)
    // use more of than the list of functions a title itself may import
    for (const auto *module :
         {"crypt32.dll", "msvcp_win.dll", "winhttp.dll", "bcrypt.dll", "advapi32.dll"})
        registry.forward(module, module);
}
} // namespace xv::runtime::modules
