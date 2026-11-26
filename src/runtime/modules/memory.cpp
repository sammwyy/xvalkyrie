#include "runtime/modules/memory.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/placement.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>
#include <windows.h>

#include <psapi.h> // after windows.h

// Physical pages: a 16 GiB section mapped in 64 KiB views over placeholders.
namespace xv::runtime::modules {
namespace {
constexpr DWORD mem_large_pages = 0x20000000;
constexpr DWORD mem_4mb_pages = 0x80000000;
constexpr DWORD page_sizes = mem_large_pages | mem_4mb_pages;
constexpr std::uintptr_t page = 1 << 16;
constexpr std::uint64_t physical_size = 16ull << 30;
// A title is told of a page by the number of its first 4 KiB frame, as the console does: one
// keeps such numbers without their low four bits (shifted left by 4 before
// XMemMapPhysicalPagesScatter).
constexpr ULONG_PTR frames = page >> 12;
constexpr DWORD allocation_types =
    MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_RESET_UNDO;
constexpr DWORD protections = 0xff | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;

struct Mapping {
    ULONG_PTR page;
    bool owned; // taken by a plain commit
};
// large or 4 MiB pages: one placeholder per 64 KiB
struct Reservation {
    std::uintptr_t size;
    std::map<std::uintptr_t, Mapping> views;
};
struct Physical {
    std::mutex mutex;
    HANDLE section =
        CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_READ | FILE_MAP_WRITE,
                           PAGE_READWRITE, SEC_RESERVE, physical_size, nullptr, nullptr, 0);
    // the first page is never given out: its number, 0, is none to a title
    std::vector<bool> used = [] {
        std::vector<bool> taken(physical_size / page);
        taken[0] = true;
        return taken;
    }();
    std::vector<bool> dirty = std::vector<bool>(physical_size / page);
    std::map<std::uintptr_t, Reservation> reservations;
    // XV_MEM_CHECK: where each page is mapped, to tell of one that is at two addresses or
    // is given back while it is still mapped
    bool check = std::getenv("XV_MEM_CHECK");
    std::multimap<ULONG_PTR, std::uintptr_t> where;
    int told = 0;
    std::multimap<ULONG_PTR, std::uintptr_t> mapped_at; // where each page is mapped
    std::uint8_t *view = nullptr; // the whole section, made when the host first needs it
};
Physical &physical() {
    static Physical state;
    return state;
}
Reservation *reservation_at(Physical &state, std::uintptr_t address, std::uintptr_t *base) {
    auto entry = state.reservations.upper_bound(address);
    if (entry == state.reservations.begin())
        return nullptr;
    --entry;
    if (address - entry->first >= entry->second.size)
        return nullptr;
    *base = entry->first;
    return &entry->second;
}
bool take_pages(Physical &state, std::uintptr_t count, std::uintptr_t run, ULONG_PTR *pages) {
    std::uintptr_t taken = 0;
    for (std::uintptr_t i = 0; i + run <= state.used.size() && taken < count;) {
        std::uintptr_t free = 0;
        while (free < run && !state.used[i + free])
            ++free;
        if (free < run) {
            i += free + 1;
            continue;
        }
        for (std::uintptr_t j = 0; j < run; ++j) {
            state.used[i + j] = true;
            pages[taken++] = i + j;
        }
        i += run;
    }
    if (taken == count)
        return true;
    for (std::uintptr_t j = 0; j < taken; ++j)
        state.used[pages[j]] = false;
    return false;
}
// the page a title names by the number of its first frame, where there is one
bool page_of(const Physical &state, ULONG_PTR frame, ULONG_PTR *number) {
    *number = frame / frames;
    return *number < state.used.size();
}
void release_page(Physical &state, ULONG_PTR number) {
    if (state.check && state.where.contains(number) && state.told++ < 200) {
        std::fprintf(stderr, "[Memory] page %llu is given back while mapped at %llx%c",
                     static_cast<unsigned long long>(number),
                     static_cast<unsigned long long>(state.where.find(number)->second), 10);
        std::fflush(stderr);
    }
    state.used[number] = false;
    state.dirty[number] = true;
}
void unmap(Physical &state, Reservation &reservation, std::uintptr_t address) {
    const auto view = reservation.views.find(address);
    if (view == reservation.views.end())
        return;
    UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void *>(address),
                     MEM_PRESERVE_PLACEHOLDER);
    if (state.check) {
        const auto [first, last] = state.where.equal_range(view->second.page);
        for (auto entry = first; entry != last; ++entry)
            if (entry->second == address) {
                state.where.erase(entry);
                break;
            }
    }
    if (view->second.owned)
        release_page(state, view->second.page);
    for (auto [at, end] = state.mapped_at.equal_range(view->second.page); at != end; ++at)
        if (at->second == address) {
            state.mapped_at.erase(at);
            break;
        }
    reservation.views.erase(view);
}
// the title's memory mapped or committed since take_newly_mapped, a page's run at a time
std::mutex newly_mutex;
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> newly;
void note_mapped(std::uintptr_t start, std::uintptr_t end) {
    const std::lock_guard lock(newly_mutex);
    if (!newly.empty() && newly.back().second == start)
        newly.back().second = end;
    else
        newly.emplace_back(start, end);
    // one that nothing takes is kept as all it spans
    if (newly.size() > 65536) {
        std::uintptr_t first = ~std::uintptr_t{0}, last = 0;
        for (const auto &[from, to] : newly)
            first = std::min(first, from), last = std::max(last, to);
        newly.assign(1, {first, last});
    }
}
bool map(Physical &state, Reservation &reservation, std::uintptr_t address, ULONG_PTR number,
         DWORD protect, bool owned) {
    unmap(state, reservation, address);
    auto *const at = reinterpret_cast<void *>(address);
    if (!MapViewOfFile3(state.section, nullptr, at, number * page, page, MEM_REPLACE_PLACEHOLDER,
                        PAGE_READWRITE, nullptr, 0) ||
        !VirtualAlloc(at, page, MEM_COMMIT, PAGE_READWRITE))
        return false;
    if (owned && state.dirty[number])
        std::memset(at, 0, page);
    state.dirty[number] = false;
    DWORD previous;
    if ((protect & protections) != PAGE_READWRITE)
        VirtualProtect(at, page, protect & protections, &previous);
    reservation.views[address] = {number, owned};
    if (state.check) {
        if (state.where.contains(number) && state.told++ < 200) {
            std::fprintf(stderr, "[Memory] page %llu is mapped at %llx while it is at %llx%c",
                         static_cast<unsigned long long>(number),
                         static_cast<unsigned long long>(address),
                         static_cast<unsigned long long>(state.where.find(number)->second), 10);
            std::fflush(stderr);
        }
        state.where.emplace(number, address);
    }
    state.mapped_at.emplace(number, address);
    note_mapped(address, address + page);
    return true;
}
// The console's addresses have 40 bits, and a title may count on it: its GPU takes no more (a
// library of the console's turns down an acceleration structure above them). What a title is
// given where it names no address is below 1 TiB, as there.
MEM_ADDRESS_REQUIREMENTS console_range(const void *address, std::uintptr_t alignment = 0) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    if (!address) {
        requirements.Alignment = alignment;
        requirements.HighestEndingAddress = reinterpret_cast<void *>((1ull << 40) - 1);
    }
    return requirements;
}
void *reserve(void *address, std::uintptr_t size, DWORD type) {
    auto &state = physical();
    if (!state.section)
        return nullptr;
    size = (size + page - 1) & ~(page - 1);
    auto requirements = console_range(address, type & mem_4mb_pages ? 4u << 20 : 0);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    // over a placeholder the title made and split itself, the placeholder is the reservation
    auto *const base =
        type & MEM_REPLACE_PLACEHOLDER
            ? address
            : VirtualAlloc2(GetCurrentProcess(), address, size,
                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &parameter, 1);
    if (!base)
        return nullptr;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    for (std::uintptr_t offset = 0; offset + page < size; offset += page)
        VirtualFree(reinterpret_cast<void *>(start + offset), page,
                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    const std::lock_guard lock(state.mutex);
    state.reservations[start] = {size, {}};
    return base;
}
// memory the title asked for and is not given, the first few times: a title that does not
// get it often stops at a check of its own far from the call
void tell_short(const char *what, std::uintptr_t bytes) {
    if (static std::atomic<int> told{0}; told++ < 8) {
        const auto error = GetLastError();
        std::fprintf(stderr, "[Runtime] %s of %llu bytes failed: %lu\n", what,
                     static_cast<unsigned long long>(bytes), error);
        std::fflush(stderr);
        SetLastError(error);
    }
}
bool commit(std::uintptr_t address, std::uintptr_t size, DWORD protect) {
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    std::uintptr_t base = 0;
    auto *const reservation = reservation_at(state, address, &base);
    if (!reservation)
        return false;
    const auto end = std::min(address + size, base + reservation->size);
    for (auto at = address & ~(page - 1); at < end; at += page) {
        if (reservation->views.contains(at)) {
            DWORD previous;
            VirtualProtect(reinterpret_cast<void *>(at), page, protect & protections, &previous);
            continue;
        }
        ULONG_PTR number;
        if (!take_pages(state, 1, 1, &number) ||
            !map(state, *reservation, at, number, protect, true)) {
            tell_short("a commit of physical pages", size);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
    }
    return true;
}

void *WINAPI virtual_alloc_ex(HANDLE process, void *address, SIZE_T size, DWORD type,
                              DWORD protect) {
    // XV_MEM_TRACE: what a title asks of VirtualAlloc
    if (static std::atomic<int> shown{0}; std::getenv("XV_MEM_TRACE") && shown++ < 40000) {
        std::fprintf(stderr, "[Runtime] VirtualAlloc(%p, 0x%zx, type 0x%lx, protect 0x%lx)\n",
                     address, static_cast<std::size_t>(size), type, protect);
        std::fflush(stderr);
    }
    const bool own = GetProcessId(process) == GetCurrentProcessId();
    auto requirements = console_range(address);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    if (own) {
        if (type & MEM_RESERVE && type & page_sizes) {
            auto *const base = reserve(address, size, type);
            if (base && type & MEM_COMMIT &&
                !commit(reinterpret_cast<std::uintptr_t>(base), size, protect))
                return nullptr;
            return base;
        }
        if (type & MEM_COMMIT && commit(reinterpret_cast<std::uintptr_t>(address), size, protect))
            return address;
        // a placeholder, which the title splits with VirtualFree and reserves piece by piece
        if (type & MEM_RESERVE_PLACEHOLDER)
            return VirtualAlloc2(process, address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                 PAGE_NOACCESS, &parameter, 1);
    }
    auto *result = own && !address ? VirtualAlloc2(process, nullptr, size, type & allocation_types,
                                                   protect & protections, &parameter, 1)
                                   : nullptr;
    if (!result)
        result =
            VirtualAllocEx(process, address, size, type & allocation_types, protect & protections);
    if (result && type & MEM_COMMIT)
        note_mapped(reinterpret_cast<std::uintptr_t>(result),
                    reinterpret_cast<std::uintptr_t>(result) + size);
    if (!result && type & MEM_COMMIT && GetLastError() == ERROR_COMMITMENT_LIMIT)
        tell_short("VirtualAlloc", size);
    if (!result && (type & ~(allocation_types | page_sizes) || protect & ~protections)) {
        const auto error = GetLastError();
        std::fprintf(stderr,
                     "[Runtime] VirtualAlloc of %zu bytes, type 0x%lx protect 0x%lx failed: %lu\n",
                     static_cast<std::size_t>(size), type, protect, error);
        std::fflush(stderr);
        SetLastError(error);
    }
    return result;
}
void *WINAPI virtual_alloc(void *address, SIZE_T size, DWORD type, DWORD protect) {
    return virtual_alloc_ex(GetCurrentProcess(), address, size, type, protect);
}
BOOL WINAPI virtual_free_ex(HANDLE process, void *address, SIZE_T size, DWORD type) {
    if (GetProcessId(process) == GetCurrentProcessId()) {
        auto &state = physical();
        const std::lock_guard lock(state.mutex);
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        std::uintptr_t base = 0;
        if (auto *const reservation = reservation_at(state, at, &base)) {
            const bool release = type & MEM_RELEASE;
            if (release && at != base) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            const auto end = release || !size ? base + reservation->size : at + size;
            for (auto view = at & ~(page - 1); view < end; view += page)
                unmap(state, *reservation, view);
            if (release) {
                for (auto piece = base; piece < end; piece += page)
                    VirtualFree(reinterpret_cast<void *>(piece), 0, MEM_RELEASE);
                state.reservations.erase(base);
            }
            return TRUE;
        }
    }
    return VirtualFreeEx(process, address, size, type);
}
BOOL WINAPI virtual_free(void *address, SIZE_T size, DWORD type) {
    return virtual_free_ex(GetCurrentProcess(), address, size, type);
}
BOOL WINAPI virtual_protect(void *address, SIZE_T size, DWORD protect, DWORD *previous) {
    note_mapped(reinterpret_cast<std::uintptr_t>(address),
                reinterpret_cast<std::uintptr_t>(address) + size);
    return VirtualProtect(address, size, protect & protections, previous);
}

BOOL WINAPI allocate_title_physical_pages(HANDLE, DWORD type, ULONG_PTR *count, ULONG_PTR *pages) {
    const auto sizes = type & page_sizes;
    if (!count || !pages || !sizes || sizes == page_sizes ||
        (type & mem_4mb_pages && *count & 63)) {
        if (count)
            *count = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    if (!state.section || !take_pages(state, *count, type & mem_4mb_pages ? 64 : 1, pages)) {
        tell_short("an allocation of physical pages", *count * page);
        *count = 0;
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    for (ULONG_PTR i = 0; i < *count; ++i)
        pages[i] *= frames;
    return TRUE;
}
BOOL WINAPI free_title_physical_pages(HANDLE, ULONG_PTR count, ULONG_PTR *pages) {
    if (count && !pages) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    for (ULONG_PTR i = 0; i < count; ++i)
        if (pages[i] && pages[i] / frames < state.used.size())
            release_page(state, pages[i] / frames);
    return TRUE;
}
void *WINAPI map_title_physical_pages(void *address, ULONG_PTR count, DWORD type, DWORD protect,
                                      ULONG_PTR *pages) {
    const auto sizes = type & page_sizes;
    if (!pages || !sizes || sizes == page_sizes || (type & mem_4mb_pages && count & 63) ||
        reinterpret_cast<std::uintptr_t>(address) & (page - 1)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto &state = physical();
    if (!address && !(address = reserve(nullptr, count * page, type)))
        return nullptr;
    const std::lock_guard lock(state.mutex);
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    std::uintptr_t base = 0;
    auto *const reservation = reservation_at(state, start, &base);
    if (!reservation || start + count * page > base + reservation->size) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    for (ULONG_PTR i = 0; i < count; ++i)
        if (!pages[i])
            unmap(state, *reservation, start + i * page);
        else if (pages[i] / frames >= state.used.size() ||
                 !map(state, *reservation, start + i * page, pages[i] / frames, protect, false)) {
            tell_short("a mapping of physical pages", count * page);
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
    return address;
}

// TOOLINGMEMORYSTATUS: none on retail
struct ToolingMemory {
    DWORD length, reserved;
    ULONGLONG total, available, peak, page_tables;
};
BOOL WINAPI tooling_memory_status(ToolingMemory *status) {
    if (!status || status->length < sizeof *status) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *status = {sizeof *status, 0, 0, 0, 0, 0};
    return TRUE;
}

// a fresh 64K-aligned region satisfies any attributes
void *WINAPI xmem_alloc_default(SIZE_T size, ULONGLONG) {
    auto *const memory =
        virtual_alloc(nullptr, size ? size : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        tell_short("XMemAlloc", size);
    return memory;
}
void WINAPI xmem_free_default(void *address, ULONGLONG) {
    if (address)
        VirtualFree(address, 0, MEM_RELEASE);
}
using XMemAlloc = void *(WINAPI *)(SIZE_T, ULONGLONG);
using XMemFree = void(WINAPI *)(void *, ULONGLONG);
std::atomic<XMemAlloc> xmem_alloc_hook{&xmem_alloc_default};
std::atomic<XMemFree> xmem_free_hook{&xmem_free_default};
void *WINAPI xmem_alloc(SIZE_T size, ULONGLONG attributes) {
    return xmem_alloc_hook.load()(size, attributes);
}
void WINAPI xmem_free(void *address, ULONGLONG attributes) {
    xmem_free_hook.load()(address, attributes);
}
void WINAPI xmem_set_allocation_hooks(XMemAlloc alloc, XMemFree free) {
    xmem_alloc_hook = alloc ? alloc : &xmem_alloc_default;
    xmem_free_hook = alloc ? free : &xmem_free_default;
}
void WINAPI xmem_check_default_heaps() {}
BOOL WINAPI not_implemented() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
SIZE_T WINAPI xmem_get_allocation_hysteresis() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return static_cast<SIZE_T>(-1);
}
HANDLE WINAPI xmem_get_auxiliary_title_memory() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}
void WINAPI xmem_release_auxiliary_title_memory(HANDLE) {}

} // namespace

void *commit_title_memory(void *address, std::size_t size) {
    return virtual_alloc(address, size, MEM_COMMIT, PAGE_READWRITE);
}

void *allocate_aligned(std::size_t size, std::uint64_t alignment, void *address) {
    auto requirements = console_range(address, alignment <= page ? 0 : alignment);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    auto *const memory = VirtualAlloc2(GetCurrentProcess(), address, size, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE, &parameter, 1);
    if (!memory)
        tell_short("an allocation of graphics memory", size);
    return memory;
}

namespace {
HRESULT WINAPI d3d_allocate_graphics_memory(SIZE_T size, UINT64 alignment, UINT64 address, UINT,
                                            void **result) {
    if (!result || alignment > 0x20000)
        return E_INVALIDARG;
    *result = allocate_aligned(size, alignment, reinterpret_cast<void *>(address));
    return *result ? S_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI d3d_free_graphics_memory(void *address) {
    return VirtualFree(address, 0, MEM_RELEASE) ? S_OK : E_INVALIDARG;
}

// xmem.dll, the GDK's names for the same memory. XMemVirtualAlloc is VirtualAlloc with
// attributes the host has no use for; the physical page functions take no process.
void *WINAPI xmem_virtual_alloc(void *address, SIZE_T size, DWORD type, ULONGLONG, DWORD protect) {
    return virtual_alloc(address, size, type, protect);
}
BOOL WINAPI xmem_allocate_physical_pages(ULONGLONG flags, ULONG_PTR *count, ULONG_PTR *pages) {
    const auto sizes = static_cast<DWORD>(flags) & page_sizes;
    return allocate_title_physical_pages(nullptr, sizes ? sizes : mem_large_pages, count, pages);
}
BOOL WINAPI xmem_free_physical_pages(ULONG_PTR count, ULONG_PTR *pages) {
    return free_title_physical_pages(nullptr, count, pages);
}
// the address comes back, which a title goes on to use; null is failure either way
void *WINAPI xmem_map_physical_pages(void *address, ULONG_PTR count, ULONG_PTR *pages) {
    return map_title_physical_pages(address, count, mem_large_pages, PAGE_READWRITE, pages);
}
// XMemMapPhysicalPagesScatter (xmem.h): each page at an address of its own; without pages,
// what is at those addresses is unmapped
BOOL WINAPI xmem_map_physical_pages_scatter(void **addresses, ULONG_PTR count, ULONG_PTR *pages) {
    if (count && !addresses) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    for (ULONG_PTR i = 0; i < count; ++i) {
        const auto at = reinterpret_cast<std::uintptr_t>(addresses[i]);
        std::uintptr_t base = 0;
        auto *const reservation = at & (page - 1) ? nullptr : reservation_at(state, at, &base);
        if (!reservation) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        // a page number 0 unmaps as well: a title clears the addresses it lets go of that way
        if (!pages || !pages[i])
            unmap(state, *reservation, at);
        else if (pages[i] / frames >= state.used.size() ||
                 !map(state, *reservation, at, pages[i] / frames, PAGE_READWRITE, false)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    return TRUE;
}
// XMEM_BASIC_INFORMATION (xmem.h): what VirtualQuery says, then the attributes and the tag
// the memory was allocated with, which are not kept here
struct BasicInformation {
    void *base, *allocation_base;
    DWORD allocation_protect;
    SIZE_T region_size;
    DWORD state, protect, type;
    ULONGLONG flags;
    DWORD tag;
    char tag_name[4];
};
BOOL WINAPI xmem_virtual_query(HANDLE process, void *address, BasicInformation *information) {
    MEMORY_BASIC_INFORMATION host;
    if (!information || !VirtualQueryEx(process, address, &host, sizeof host)) {
        if (!information)
            SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *information = {host.BaseAddress,
                    host.AllocationBase,
                    host.AllocationProtect,
                    host.RegionSize,
                    host.State,
                    host.Protect,
                    host.Type,
                    0,
                    0,
                    {}};
    return TRUE;
}
// XMEM_WORKING_SET_STATISTICS: what a title may use, from what the host has
struct WorkingSet {
    SIZE_T game_limit, game_used, tools_limit, tools_used;
    UINT32 stack_used, kernel_pool_used;
    SIZE_T gpu_limit, gpu_used, tools_gpu_limit, tools_gpu_used;
};
BOOL WINAPI xmem_working_set_statistics(DWORD, WorkingSet *statistics) {
    if (!statistics) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MEMORYSTATUSEX host{sizeof host};
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof process;
    if (!GlobalMemoryStatusEx(&host) ||
        !K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process),
                                 sizeof process))
        return FALSE;
    // What is used is what the title has taken, not what the rest of the host has: a title
    // sizes its pools and what it streams in by what is left of its limit.
    const SIZE_T limit = std::min<ULONGLONG>(title_memory(console_model()), host.ullTotalPhys);
    const SIZE_T used = std::min<ULONGLONG>(process.PrivateUsage, limit);
    *statistics = {limit, used, 0, 0, 0, 0, limit, used, 0, 0};
    return TRUE;
}
BOOL WINAPI no() { return FALSE; }
// XMemTransferMemory moves memory between the title's and the system's budgets, which the
// host does not keep apart
BOOL WINAPI xmem_transfer_memory(UINT, SIZE_T) { return TRUE; }
ULONGLONG WINAPI xmem_make_tag(UINT32 tag) { return ULONGLONG{tag} << 32; }

template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
const ApiFunction functions[] = {
    {"VirtualAlloc", erase(virtual_alloc)},
    {"VirtualAllocEx", erase(virtual_alloc_ex)},
    {"VirtualFree", erase(virtual_free)},
    {"VirtualFreeEx", erase(virtual_free_ex)},
    {"VirtualProtect", erase(virtual_protect)},
    {"AllocateTitlePhysicalPages", erase(allocate_title_physical_pages)},
    {"FreeTitlePhysicalPages", erase(free_title_physical_pages)},
    {"MapTitlePhysicalPages", erase(map_title_physical_pages)},
    {"ToolingMemoryStatus", erase(tooling_memory_status)},
    {"XMemAllocDefault", erase(xmem_alloc_default)},
    {"XMemFreeDefault", erase(xmem_free_default)},
    {"XMemAlloc", erase(xmem_alloc)},
    {"XMemFree", erase(xmem_free)},
    {"XMemSetAllocationHooks", erase(xmem_set_allocation_hooks)},
    {"XMemCheckDefaultHeaps", erase(xmem_check_default_heaps)},
    {"XMemSetAllocationHysteresis", erase(not_implemented)},
    {"XMemGetAllocationHysteresis", erase(xmem_get_allocation_hysteresis)},
    {"XMemPreallocateFreeSpace", erase(not_implemented)},
    {"XMemGetAllocationStatistics", erase(not_implemented)},
    {"XMemGetAuxiliaryTitleMemory", erase(xmem_get_auxiliary_title_memory)},
    {"XMemReleaseAuxiliaryTitleMemory", erase(xmem_release_auxiliary_title_memory)}};
} // namespace

std::span<const ApiFunction> memory_functions() { return functions; }
HRESULT WINAPI allocate_graphics_memory(SIZE_T size, UINT64 alignment, UINT64 address, UINT flags,
                                        void **result) {
    return d3d_allocate_graphics_memory(size, alignment, address, flags, result);
}
HRESULT WINAPI free_graphics_memory(void *address) { return d3d_free_graphics_memory(address); }

namespace {
// XMemSetOption(option, value): tuning of the console's allocator, which is not this one
HRESULT WINAPI xmem_set_option(UINT, std::uint64_t) { return S_OK; }
} // namespace

void install_xmem(Registry &registry) {
    for (const auto &[name, function] : functions)
        if (std::string_view(name).starts_with("XMem") &&
            !std::string_view(name).ends_with("AuxiliaryTitleMemory") &&
            std::string_view(name) != "XMemCheckDefaultHeaps")
            registry.add("xmem.dll", name, function);
    const std::pair<const char *, void *> more[] = {
        {"XMemVirtualAlloc", erase(xmem_virtual_alloc)},
        {"XMemVirtualQuery", erase(xmem_virtual_query)},
        {"XMemAllocatePhysicalPages", erase(xmem_allocate_physical_pages)},
        {"XMemFreePhysicalPages", erase(xmem_free_physical_pages)},
        {"XMemMapPhysicalPages", erase(xmem_map_physical_pages)},
        {"XMemMapPhysicalPagesScatter", erase(xmem_map_physical_pages_scatter)},
        {"XMemGetWorkingSetStatistics", erase(xmem_working_set_statistics)},
        {"XMemGetWin32HeapStatistics", erase(not_implemented)},
        {"XMemIsToolsMemAvailable", erase(no)},
        {"XMemSetOption", erase(xmem_set_option)},
        {"XMemTransferMemory", erase(xmem_transfer_memory)},
        {"XMemMakeTag", erase(xmem_make_tag)}};
    for (const auto &[name, function] : more)
        registry.add("xmem.dll", name, function);
    // a GDK title asks the API sets for memory in the console's page sizes
    for (const auto *name :
         {"VirtualAlloc", "VirtualAllocEx", "VirtualFree", "VirtualFreeEx", "VirtualProtect"})
        for (const auto &[known, function] : functions)
            if (std::string_view(known) == name)
                registry.add_api(name, function);
}
bool copy_physical_pages(const ULONG_PTR *pages, std::size_t offset, std::uint8_t *data,
                         std::size_t size, bool write) {
    if (!size)
        return true;
    if (!pages || offset >= page)
        return false;
    auto &state = physical();
    // each page's place in the view, and where the title has it mapped
    std::vector<std::pair<std::uint8_t *, std::vector<std::uintptr_t>>> spans;
    {
        const std::lock_guard lock(state.mutex);
        if (!state.section)
            return false;
        if (!state.view)
            state.view = static_cast<std::uint8_t *>(MapViewOfFile3(
                state.section, nullptr, nullptr, 0, physical_size, 0, PAGE_READWRITE, nullptr, 0));
        if (!state.view)
            return false;
        for (std::size_t index = 0, left = offset + size; left; ++index) {
            ULONG_PTR number = 0;
            if (!page_of(state, pages[index], &number) || !state.used[number])
                return false;
            auto *const at = state.view + number * page;
            if (!VirtualAlloc(at, page, MEM_COMMIT, PAGE_READWRITE))
                return false;
            std::vector<std::uintptr_t> addresses;
            if (write)
                for (auto [it, end] = state.mapped_at.equal_range(number); it != end; ++it)
                    addresses.push_back(it->second);
            spans.emplace_back(at, std::move(addresses));
            left -= std::min<std::size_t>(left, page);
        }
    }
    for (std::size_t index = 0, done = 0; done < size; ++index) {
        const auto first = index ? 0 : offset;
        const auto here = std::min<std::size_t>(size - done, page - first);
        const auto &[at, addresses] = spans[index];
        for (const auto address : addresses)
            prepare_title_write(reinterpret_cast<const void *>(address + first), here);
        if (write)
            std::memcpy(at + first, data + done, here);
        else
            std::memcpy(data + done, at + first, here);
        done += here;
    }
    return true;
}
bool map_alias(std::uintptr_t to, std::uintptr_t from, std::uintptr_t size) {
    auto &state = physical();
    bool mapped = true;
    {
        const std::lock_guard lock(state.mutex);
        for (std::uintptr_t offset = 0; offset < size; offset += page) {
            const auto at = (to + offset) & ~(page - 1);
            std::uintptr_t base = 0;
            auto *const target = reservation_at(state, at, &base);
            if (!target) {
                mapped = false;
                continue;
            }
            std::optional<ULONG_PTR> shown;
            if (from) {
                const auto source = (from + offset) & ~(page - 1);
                if (auto *const pool = reservation_at(state, source, &base))
                    if (const auto view = pool->views.find(source); view != pool->views.end())
                        shown = view->second.page;
            }
            if (!shown) {
                unmap(state, *target, at);
                mapped = mapped && !from;
                continue;
            }
            if (const auto now = target->views.find(at);
                now != target->views.end() && now->second.page == *shown)
                continue;
            mapped = map(state, *target, at, *shown, PAGE_READWRITE, false) && mapped;
        }
    }
    // what a texture placed there shows has changed
    prepare_title_write(reinterpret_cast<const void *>(to), size);
    return mapped;
}
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> take_newly_mapped() {
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> taken;
    const std::lock_guard lock(newly_mutex);
    taken.swap(newly);
    return taken;
}
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> other_views(std::uintptr_t start,
                                                                   std::uintptr_t end) {
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> views;
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    for (auto at = start & ~(page - 1); at < end; at += page) {
        std::uintptr_t base = 0;
        auto *const reservation = reservation_at(state, at, &base);
        if (!reservation)
            continue;
        const auto view = reservation->views.find(at);
        if (view == reservation->views.end())
            continue;
        const auto first = std::max(start, at), last = std::min(end, at + page);
        for (auto [it, stop] = state.mapped_at.equal_range(view->second.page); it != stop; ++it)
            if (it->second != at)
                views.emplace_back(it->second + (first - at), it->second + (last - at));
    }
    return views;
}
} // namespace xv::runtime::modules
