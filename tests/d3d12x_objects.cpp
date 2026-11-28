#include "runtime/modules/d3d12x.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <thread>

// The host's command allocators and fences as a title holds them, with the console's methods
// past the desktop's, on a WARP device. The calls come from code that calls the way a title's
// does: `call [rax + slot * 8]`.
namespace {
using namespace xv::runtime::modules;

// One piece of code per slot, which calls that slot of the object in RCX and passes RDX, R8
// and R9 on: sub rsp, 28h; mov rax, [rcx]; call [rax + slot * 8]; add rsp, 28h; ret. Each test
// has its own, which calls as the title only while the test runs.
class TitleCode {
  public:
    TitleCode() {
        code_ = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        REQUIRE(code_);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            static constexpr std::uint8_t start[]{0x48, 0x83, 0xec, 0x28, 0x48,
                                                  0x8b, 0x01, 0xff, 0x90};
            static constexpr std::uint8_t end[]{0x48, 0x83, 0xc4, 0x28, 0xc3};
            auto *const at = code_ + slot * stride;
            const std::uint32_t offset = slot * 8;
            std::memcpy(at, start, sizeof start);
            std::memcpy(at + sizeof start, &offset, sizeof offset);
            std::memcpy(at + sizeof start + sizeof offset, end, sizeof end);
        }
        FlushInstructionCache(GetCurrentProcess(), code_, size);
        d3d12x::call_as_title(code_, size);
    }
    ~TitleCode() {
        d3d12x::stop_calling_as_title(code_);
        VirtualFree(code_, 0, MEM_RELEASE);
    }
    TitleCode(const TitleCode &) = delete;
    TitleCode &operator=(const TitleCode &) = delete;
    template <class Result, class... Arguments>
    Result call(void *object, std::uint32_t slot, Arguments... arguments) const {
        REQUIRE(slot < slots);
        using Function = Result(STDMETHODCALLTYPE *)(void *, Arguments...);
        return reinterpret_cast<Function>(code_ + slot * stride)(object, arguments...);
    }

  private:
    static constexpr std::uint32_t slots = 16, stride = 32, size = slots * stride;
    std::uint8_t *code_ = nullptr;
};

struct Warp {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
};
const Warp &warp() {
    static const Warp made = [] {
        Warp warp;
        IDXGIFactory4 *factory = nullptr;
        IDXGIAdapter *adapter = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(
                D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp.device)))) {
            const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
            warp.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&warp.queue));
        }
        if (adapter)
            adapter->Release();
        if (factory)
            factory->Release();
        return warp;
    }();
    REQUIRE(made.device);
    REQUIRE(made.queue);
    return made;
}
// the GPU done with everything the queue was given so far
void finish(ID3D12CommandQueue *queue) {
    ID3D12Fence *fence = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    REQUIRE(SUCCEEDED(queue->Signal(fence, 1)));
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
    CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CloseHandle(event);
    fence->Release();
}

// what a title gives SetAllocatorCallbackX: it hands out memory for commands when asked
std::atomic<int> memory_asked{0};
void *STDMETHODCALLTYPE memory_for_commands(void *, void *, void *, std::uint64_t) {
    ++memory_asked;
    return nullptr;
}

// a fence as the device makes it for the title
ID3D12Fence *title_fence(UINT64 initial) {
    ID3D12Fence *fence = nullptr;
    REQUIRE(SUCCEEDED(
        warp().device->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    d3d12x::console_fence(fence);
    return fence;
}
// GetGpuAddressX (12), as the title reads what it gives
const volatile std::uint64_t *completed_value(const TitleCode &title, ID3D12Fence *fence) {
    const auto address = title.call<std::uint64_t>(fence, 12);
    REQUIRE(address);
    REQUIRE(address % 8 == 0);
    return reinterpret_cast<const volatile std::uint64_t *>(address);
}
// the value the title reads is `wanted` within a few seconds
bool reaches(const volatile std::uint64_t *value, std::uint64_t wanted) {
    for (int waited = 0; *value != wanted && waited < 5000; ++waited)
        Sleep(1);
    return *value == wanted;
}
// Private data that counts the references a fence keeps to it, which it lets go of when it is
// destroyed
constexpr GUID watch_guid{
    0x5f0e6a4c, 0x2b7d, 0x4c1e, {0x9a, 0x3f, 0x61, 0x0d, 0x8e, 0x2b, 0x47, 0xc5}};
class Watch final : public IUnknown {
  public:
    explicit Watch(ID3D12Fence *fence) {
        REQUIRE(SUCCEEDED(fence->SetPrivateDataInterface(watch_guid, this)));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = iid == __uuidof(IUnknown) ? this : nullptr;
        if (!*object)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    // whether the fence is gone, within a few seconds
    bool gone() const {
        for (int waited = 0; references_ != 0 && waited < 5000; ++waited)
            Sleep(1);
        return references_ == 0;
    }

  private:
    std::atomic<ULONG> references_{0};
};
} // namespace

TEST_CASE("a command allocator's console methods leave the title's memory alone") {
    const TitleCode title;
    ID3D12CommandAllocator *allocator = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator))));
    d3d12x::console_command_allocator(allocator);
    // SetAllocatorCallbackX (9), with a context the title's callback would allocate from
    std::array<std::uint8_t, 4096> context;
    context.fill(0xcd);
    CHECK(title.call<HRESULT>(allocator, 9, &memory_for_commands, context.data()) == S_OK);
    CHECK(title.call<HRESULT>(allocator, 9, nullptr, nullptr) == S_OK);
    // the two console methods after it
    CHECK(title.call<HRESULT>(allocator, 10, context.data(), context.data()) == S_OK);
    CHECK(title.call<HRESULT>(allocator, 11, context.data(), context.data()) == S_OK);
    // the allocator records and runs commands as before, Reset through the title's call as well
    CHECK(title.call<HRESULT>(allocator, 9, &memory_for_commands, context.data()) == S_OK);
    ID3D12GraphicsCommandList *list = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                       nullptr, IID_PPV_ARGS(&list))));
    CHECK(SUCCEEDED(list->Close()));
    ID3D12CommandList *const lists[]{list};
    warp().queue->ExecuteCommandLists(1, lists);
    finish(warp().queue);
    CHECK(title.call<HRESULT>(allocator, 8) == S_OK);
    CHECK(SUCCEEDED(list->Reset(allocator, nullptr)));
    CHECK(SUCCEEDED(list->Close()));
    warp().queue->ExecuteCommandLists(1, lists);
    finish(warp().queue);
    CHECK(SUCCEEDED(allocator->Reset()));
    // the callback was never asked, and nothing was written where it allocates from
    CHECK(memory_asked == 0);
    CHECK(std::all_of(context.begin(), context.end(),
                      [](std::uint8_t byte) { return byte == 0xcd; }));
    list->Release();
    allocator->Release();
}

TEST_CASE("a command allocator keeps its desktop methods for the title and the host") {
    const TitleCode title;
    ID3D12CommandAllocator *allocator = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator))));
    d3d12x::console_command_allocator(allocator);
    // once is enough: an allocator that has the console's methods keeps them
    d3d12x::console_command_allocator(allocator);
    ID3D12Device *device = nullptr;
    CHECK(title.call<HRESULT>(allocator, 7, &__uuidof(ID3D12Device),
                              reinterpret_cast<void **>(&device)) == S_OK);
    CHECK(device == warp().device);
    if (device)
        device->Release();
    ID3D12Pageable *pageable = nullptr;
    CHECK(title.call<HRESULT>(allocator, 0, &__uuidof(ID3D12Pageable),
                              reinterpret_cast<void **>(&pageable)) == S_OK);
    CHECK(pageable == static_cast<ID3D12Pageable *>(allocator));
    if (pageable)
        title.call<ULONG>(pageable, 2);
    CHECK(title.call<HRESULT>(allocator, 6, L"title's allocator") == S_OK);
    CHECK(SUCCEEDED(allocator->SetName(L"host's allocator")));
    CHECK(title.call<ULONG>(allocator, 1) == 2);
    CHECK(title.call<ULONG>(allocator, 2) == 1);
    CHECK(allocator->Release() == 0);
}

TEST_CASE("a fence's GetGpuAddressX gives where its completed value is, the same place each time") {
    const TitleCode title;
    auto *const fence = title_fence(3);
    const auto *const value = completed_value(title, fence);
    CHECK(*value == 3);
    CHECK(completed_value(title, fence) == value);
    // another fence has a place of its own
    auto *const other = title_fence(5);
    const auto *const other_value = completed_value(title, other);
    CHECK(other_value != value);
    CHECK(*other_value == 5);
    CHECK(*value == 3);
    CHECK(title.call<ULONG>(other, 2) == 0);
    CHECK(title.call<ULONG>(fence, 2) == 0);
}

TEST_CASE("the completed value a title reads follows Signal from the CPU and from a queue") {
    const TitleCode title;
    auto *const fence = title_fence(0);
    const auto *const value = completed_value(title, fence);
    CHECK(*value == 0);
    // the title's own Signal (10) is there at once
    CHECK(title.call<HRESULT>(fence, 10, UINT64{7}) == S_OK);
    CHECK(*value == 7);
    // one from the host's side, as DirectStorage's
    CHECK(SUCCEEDED(fence->Signal(9)));
    CHECK(reaches(value, 9));
    // a queue's
    CHECK(SUCCEEDED(warp().queue->Signal(fence, 12)));
    CHECK(reaches(value, 12));
    // a queue's behind a wait: only once the GPU is past it
    ID3D12Fence *gate = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    CHECK(SUCCEEDED(warp().queue->Wait(gate, 1)));
    CHECK(SUCCEEDED(warp().queue->Signal(fence, 20)));
    Sleep(100);
    CHECK(*value == 12);
    CHECK(SUCCEEDED(gate->Signal(1)));
    CHECK(reaches(value, 20));
    // a lower value from the title, then the queue's after it
    CHECK(title.call<HRESULT>(fence, 10, UINT64{2}) == S_OK);
    CHECK(*value == 2);
    CHECK(SUCCEEDED(warp().queue->Signal(fence, 4)));
    CHECK(reaches(value, 4));
    finish(warp().queue);
    gate->Release();
    CHECK(title.call<ULONG>(fence, 2) == 0);
}

TEST_CASE("a fence keeps its desktop methods for the title and the host") {
    const TitleCode title;
    auto *const fence = title_fence(1);
    const auto *const value = completed_value(title, fence);
    CHECK(title.call<std::uint64_t>(fence, 8) == 1);
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(event);
    CHECK(title.call<HRESULT>(fence, 9, UINT64{3}, event) == S_OK);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    CHECK(SUCCEEDED(warp().queue->Signal(fence, 3)));
    CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CHECK(reaches(value, 3));
    CHECK(title.call<std::uint64_t>(fence, 8) == 3);
    CHECK(fence->GetCompletedValue() == 3);
    CloseHandle(event);
    // ID3D12Fence1 is the same object, with the flags it was made with (11)
    ID3D12Fence1 *fence1 = nullptr;
    CHECK(title.call<HRESULT>(fence, 0, &__uuidof(ID3D12Fence1),
                              reinterpret_cast<void **>(&fence1)) == S_OK);
    CHECK(static_cast<ID3D12Fence *>(fence1) == fence);
    CHECK(title.call<UINT>(fence, 11) == D3D12_FENCE_FLAG_NONE);
    if (fence1)
        title.call<ULONG>(fence1, 2);
    ID3D12Device *device = nullptr;
    CHECK(title.call<HRESULT>(fence, 7, &__uuidof(ID3D12Device),
                              reinterpret_cast<void **>(&device)) == S_OK);
    CHECK(device == warp().device);
    if (device)
        device->Release();
    CHECK(title.call<HRESULT>(fence, 6, L"title's fence") == S_OK);
    // and the host's own calls of the methods the title's go elsewhere for
    CHECK(SUCCEEDED(fence->Signal(5)));
    CHECK(reaches(value, 5));
    CHECK(title.call<ULONG>(fence, 2) == 0);
}

TEST_CASE("a fence whose value a title reads goes when the title lets go of it, either way") {
    const TitleCode title;
    // the title's last Release takes ours along
    auto *const fence = title_fence(0);
    Watch watch(fence);
    completed_value(title, fence);
    CHECK(title.call<ULONG>(fence, 2) == 0);
    CHECK(watch.gone());
    // the last but ours through the host's side, as DirectStorage's after it signals: ours goes
    // the next time a fence moves
    auto *const held = title_fence(0);
    Watch held_watch(held);
    completed_value(title, held);
    CHECK(held->Release() == 1);
    auto *const moving = title_fence(0);
    completed_value(title, moving);
    CHECK(SUCCEEDED(moving->Signal(1)));
    CHECK(held_watch.gone());
    CHECK(title.call<ULONG>(moving, 2) == 0);
}

TEST_CASE("the title's last Release of a fence whose value it reads gives 0 while fences move") {
    const TitleCode title;
    // another fence that moves all the time, so that its value is copied all the time
    auto *const moving = title_fence(0);
    completed_value(title, moving);
    std::atomic<bool> done{false};
    std::thread signals([&] {
        for (UINT64 value = 1; !done; ++value)
            moving->Signal(value);
    });
    int not_last = 0;
    for (int i = 0; i < 2000; ++i) {
        auto *const fence = title_fence(0);
        completed_value(title, fence);
        if (title.call<ULONG>(fence, 2) != 0)
            ++not_last;
    }
    done = true;
    signals.join();
    CHECK(not_last == 0);
    CHECK(title.call<ULONG>(moving, 2) == 0);
}

TEST_CASE(
    "control flow guard indirect calls reach the console vtable for both slot 0 and later slots") {
    // Layout in executable memory:
    //   [0x00..0x07]: pointer to [0x08] (__guard_dispatch_icall_fptr)
    //   [0x08..0x0a]: ff e0 (jmp rax)
    //   [0x20..0x3f]: sub rsp, 28h; mov rax, [rcx]; mov rax, [rax]; call [rip - disp]; add rsp,
    //   28h; ret [0x40..0x6f]: sub rsp, 28h; mov rax, [rcx]; mov rax, [rax + 0x60]; call [rip -
    //   disp]; add rsp, 28h; ret
    auto *const page = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, 256, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    REQUIRE(page);
    const auto thunk = reinterpret_cast<std::uint64_t>(page + 8);
    std::memcpy(page, &thunk, 8);
    page[8] = 0xff;
    page[9] = 0xe0; // jmp rax

    static constexpr std::uint8_t slot0[]{
        0x48, 0x83, 0xec, 0x28,             // sub rsp, 28h
        0x48, 0x8b, 0x01,                   // mov rax, [rcx]
        0x48, 0x8b, 0x00,                   // mov rax, [rax]
        0xff, 0x15, 0xd0, 0xff, 0xff, 0xff, // call [rip - 0x30] -> [page + 0]
        0x48, 0x83, 0xc4, 0x28,             // add rsp, 28h
        0xc3                                // ret
    };
    std::memcpy(page + 0x20, slot0, sizeof slot0);

    static constexpr std::uint8_t slot12[]{
        0x48, 0x83, 0xec, 0x28,                   // sub rsp, 28h
        0x48, 0x8b, 0x01,                         // mov rax, [rcx]
        0x48, 0x8b, 0x80, 0x60, 0x00, 0x00, 0x00, // mov rax, [rax + 60h] (slot 12)
        0xff, 0x15, 0xac, 0xff, 0xff, 0xff,       // call [rip - 0x54] -> [page + 0]
        0x48, 0x83, 0xc4, 0x28,                   // add rsp, 28h
        0xc3                                      // ret
    };
    std::memcpy(page + 0x40, slot12, sizeof slot12);
    FlushInstructionCache(GetCurrentProcess(), page, 256);
    d3d12x::call_as_title(page, 256);

    auto *const fence = title_fence(77);
    using Call0 = HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **);
    using Call12 = std::uint64_t(STDMETHODCALLTYPE *)(void *);
    IUnknown *queried = nullptr;
    CHECK(reinterpret_cast<Call0>(page + 0x20)(fence, __uuidof(IUnknown),
                                               reinterpret_cast<void **>(&queried)) == S_OK);
    REQUIRE(queried);
    queried->Release();
    const auto gpu_addr = reinterpret_cast<Call12>(page + 0x40)(fence);
    REQUIRE(gpu_addr != 0);
    CHECK(*reinterpret_cast<const std::uint64_t *>(gpu_addr) == 77);

    d3d12x::stop_calling_as_title(page);
    VirtualFree(page, 0, MEM_RELEASE);
    fence->Release();
}
