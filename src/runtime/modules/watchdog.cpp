#include "runtime/modules/watchdog.hpp"
#include "runtime/common/runtime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include <tlhelp32.h> // after windows.h
#include <wct.h>

#pragma comment(lib, "advapi32.lib")

// XV_WATCHDOG: when no frame has been presented for 5 seconds, the stack of every thread of
// the process goes to the log, three times per stall, to see what the title is waiting on.
namespace xv::runtime::modules {
namespace {
std::atomic<std::int64_t> last_frame{0};
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// The return addresses of a suspended thread, from where it is, into `frames`: nothing is
// allocated while it is suspended, as it may hold the heap's lock.
template <std::size_t Most>
std::size_t frames_of(HANDLE thread, std::array<DWORD64, Most> &frames) {
    std::size_t count = 0;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context))
        for (; count < Most && context.Rip; ++count) {
            frames[count] = context.Rip;
            DWORD64 base = 0;
            auto *const function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (!function) { // a leaf: the return address is on top of the stack
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void *handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context,
                             &handler_data, &frame, nullptr);
        }
    return count;
}
void dump_thread(DWORD id) {
    auto *const thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
    if (!thread)
        return;
    // how many times it was suspended already: one that is, and holds a lock, holds it for good
    const auto suspended = SuspendThread(thread);
    if (suspended == static_cast<DWORD>(-1)) {
        CloseHandle(thread);
        return;
    }
    std::array<DWORD64, 32> at{};
    const auto count = frames_of(thread, at);
    ResumeThread(thread);
    std::string frames;
    for (std::size_t n = 0; n < count; ++n)
        frames += " " + detail::guest_location(reinterpret_cast<void *>(at[n]));
    wchar_t *description = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
        for (const wchar_t *at = description; *at; ++at)
            name += *at < 0x80 ? static_cast<char>(*at) : '?';
        LocalFree(description);
    }
    CloseHandle(thread);
    // What it waits on, as far as Windows follows it (critical sections, mutexes, other
    // threads): the thread at the end of the chain is the one that holds things up.
    std::string chain;
    if (static auto *const session = OpenThreadWaitChainSession(0, nullptr); session) {
        WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT];
        DWORD count = WCT_MAX_NODE_COUNT;
        BOOL cycle = FALSE;
        if (GetThreadWaitChain(session, 0, 0, id, &count, nodes, &cycle) && count > 1) {
            chain = cycle ? " [wait chain, a cycle:" : " [wait chain:";
            for (DWORD n = 1; n < count; ++n) {
                const auto &node = nodes[n];
                if (node.ObjectType == WctThreadType)
                    chain += " thread " + std::to_string(node.ThreadObject.ThreadId);
                else
                    chain += " object " + std::to_string(node.ObjectType) +
                             (node.ObjectStatus == WctStatusOwned ? " owned" : "");
            }
            chain += "]";
        }
    }
    std::fprintf(stderr, "[Runtime] thread %lu (%s%s):%s%s\n", id, name.c_str(),
                 suspended ? (name.empty() ? "suspended" : ", suspended") : "", frames.c_str(),
                 chain.c_str());
}
void dump_threads() {
    const auto self = GetCurrentThreadId();
    auto *const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{sizeof entry};
    for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != self)
            dump_thread(entry.th32ThreadID);
    CloseHandle(snapshot);
    std::fflush(stderr);
}
} // namespace

namespace {
// Engines name their threads the old way, with an exception for a debugger (0x406D1388).
// With the watchdog on, such a name is given to the thread, so that its stack says whose it is.
LONG WINAPI thread_named(EXCEPTION_POINTERS *info) {
    const auto *const record = info->ExceptionRecord;
    if (record->ExceptionCode != 0x406d1388 || record->NumberParameters < 3)
        return EXCEPTION_CONTINUE_SEARCH;
    struct Name {
        DWORD type;
        const char *name;
        DWORD thread, flags;
    };
    const auto *const name = reinterpret_cast<const Name *>(record->ExceptionInformation);
    if (name->type != 0x1000 || !name->name)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto id = name->thread == ~0u ? GetCurrentThreadId() : name->thread;
    if (auto *const thread = OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, id)) {
        const std::wstring wide(name->name, name->name + std::strlen(name->name));
        SetThreadDescription(thread, wide.c_str());
        CloseHandle(thread);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// three looks 5 seconds apart, to tell a thread that waits from one that spins
void watch() {
    std::thread([] {
        int reported = 0;
        for (;;) {
            Sleep(1000);
            const auto stalled = now() - last_frame.load();
            if (stalled < 5000) {
                reported = 0;
            } else if (reported < 3 && stalled > 5000ll * (reported + 1)) {
                std::fprintf(stderr, "[Runtime] no frame for %lld seconds, the threads:\n",
                             static_cast<long long>(stalled / 1000));
                dump_threads();
                ++reported;
            }
        }
    }).detach();
}
// XV_PROFILE=<ms>: every so many milliseconds, the stack of each thread that has run since the
// last look, and every 10 seconds the 30 stacks seen most (their first 12 frames), with how
// many looks found each: where the process spends its time.
void profile(unsigned period) {
    std::thread([period] {
        std::map<DWORD, std::pair<HANDLE, ULONG64>> threads; // the cycles each had run
        std::map<std::vector<DWORD64>, unsigned> seen;
        unsigned looks = 0;
        const auto self = GetCurrentThreadId();
        for (auto last = now();;) {
            Sleep(period);
            if (auto *const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{sizeof entry};
                for (auto more = Thread32First(snapshot, &entry); more;
                     more = Thread32Next(snapshot, &entry))
                    if (entry.th32OwnerProcessID == GetCurrentProcessId() &&
                        entry.th32ThreadID != self && !threads.contains(entry.th32ThreadID))
                        if (auto *const thread =
                                OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                               THREAD_QUERY_INFORMATION,
                                           FALSE, entry.th32ThreadID))
                            threads[entry.th32ThreadID] = {thread, 0};
                CloseHandle(snapshot);
            }
            ++looks;
            for (auto at = threads.begin(); at != threads.end();) {
                auto &[thread, cycles] = at->second;
                ULONG64 now_cycles = 0;
                if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0 ||
                    !QueryThreadCycleTime(thread, &now_cycles)) {
                    CloseHandle(thread);
                    at = threads.erase(at);
                    continue;
                }
                // a thread that has run for a hundredth of the period at least
                const bool ran = cycles && now_cycles - cycles > 30000ull * period;
                cycles = now_cycles;
                if (ran && SuspendThread(thread) != static_cast<DWORD>(-1)) {
                    std::array<DWORD64, 12> frames{};
                    const auto count = frames_of(thread, frames);
                    ResumeThread(thread);
                    ++seen[{frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(count)}];
                }
                ++at;
            }
            if (now() - last < 10000)
                continue;
            last = now();
            std::vector<std::pair<unsigned, const std::vector<DWORD64> *>> most;
            for (const auto &[frames, count] : seen)
                most.emplace_back(count, &frames);
            std::sort(most.begin(), most.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
            std::fprintf(stderr, "[Profile] %u looks, %zu stacks:\n", looks, seen.size());
            for (std::size_t n = 0; n < most.size() && n < 30; ++n) {
                std::string text;
                for (const auto at : *most[n].second)
                    text += " " + detail::guest_location(reinterpret_cast<void *>(at));
                std::fprintf(stderr, "[Profile] %u:%s\n", most[n].first, text.c_str());
            }
            std::fflush(stderr);
            seen.clear();
            looks = 0;
        }
    }).detach();
}
} // namespace

// The watch starts before the title does, so that one that never presents is seen too; not
// with the process, whose environment the settings of xvboot may add to before it runs.
void start_watchdog() {
    static const bool started = [] {
        if (std::getenv("XV_WATCHDOG") || std::getenv("XV_THREAD_DUMP") ||
            std::getenv("XV_PROFILE"))
            AddVectoredExceptionHandler(1, &thread_named);
        if (const auto *const period = std::getenv("XV_PROFILE"))
            profile(std::max(1, std::atoi(period)));
        if (std::getenv("XV_WATCHDOG")) {
            last_frame = now();
            watch();
        }
        return true;
    }();
    static_cast<void>(started);
}

void frame_watchdog() {
    start_watchdog();
    // XV_THREAD_DUMP=<frame>: the same stacks once, at that frame, of a title that goes on
    // presenting while something in it waits
    static const char *const dump_at = std::getenv("XV_THREAD_DUMP");
    if (static std::atomic<int> frames{0}; dump_at && ++frames == std::atoi(dump_at)) {
        std::fprintf(stderr, "[Runtime] the threads at frame %s:\n", dump_at);
        dump_threads();
    }
    last_frame = now();
}
} // namespace xv::runtime::modules
