#include "d3d12x_title.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

// What orders the GPU's work on the console without a fence: FlushPipelineX in a list, and
// Wait32BitValueX / Wait64BitValueX, a wait for a value another queue's work writes in memory.
// The host has no wait on memory: a list with one runs after what the other queues were given
// before it, up to a wait of theirs that is not over. A list's own work on memory by address
// (copies, fills, values written) is done when a queue is given the list, in its order.
namespace {
using namespace d3d12x_test;

// The later kits' layout of queues and lists, which the title's first Signal at the queue's
// slot 16 shows (see queue_by_layout): every list method from 59 on is two slots further, and so
// are the queue's Signal (16) and Wait (17).
constexpr std::uint32_t queue_signal = 16, queue_wait = 17, copy_memory = 62, fill_32 = 63,
                        flush_pipeline = 69, wait_32 = 73, write_32 = 90;
ID3D12Fence *fence() {
    ID3D12Fence *made = nullptr;
    REQUIRE(SUCCEEDED(device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&made))));
    return made;
}
// that layout, for as long as a test holds it
struct LaterLayout : LayoutsShown {
    LaterLayout() {
        static auto *const marker = fence();
        CHECK(title().call(queue(), queue_signal, marker, std::uint64_t{1}) == S_OK);
    }
};
// the title's compute queue (CreateCommandQueue, 8), and a list of its own type
struct Compute {
    void *queue = nullptr, *list = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    Compute() {
        const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_COMPUTE};
        REQUIRE(title().call(device(), 8, &desc, &__uuidof(ID3D12CommandQueue), &queue) == S_OK);
        REQUIRE(title().call(device(), 9, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                             &__uuidof(ID3D12CommandAllocator),
                             reinterpret_cast<void **>(&allocator)) == S_OK);
        REQUIRE(title().call(device(), 12, 0u, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator,
                             static_cast<ID3D12PipelineState *>(nullptr),
                             &__uuidof(ID3D12GraphicsCommandList), &list) == S_OK);
    }
    ~Compute() {
        static_cast<IUnknown *>(list)->Release();
        allocator->Release();
        static_cast<IUnknown *>(queue)->Release();
    }
};
// a list with nothing in it, closed and given to the title's direct queue
void submit_nothing(Work &work) {
    CHECK(title().call(work.list, 9) == S_OK);
    void *const lists[]{work.list};
    title().call<void>(queue(), 10, 1u, lists);
}
bool reaches(ID3D12Fence *fence, UINT64 value, DWORD milliseconds) {
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(value, event)));
    const bool reached = WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
    CloseHandle(event);
    return reached;
}
// a compute list that waits on memory, given to its queue, which then signals `done`
void wait_on_memory(Compute &compute, ID3D12Fence *done) {
    static std::uint32_t value = 1;
    title().call<void>(compute.list, wait_32, reinterpret_cast<std::uint64_t>(&value),
                       std::uint64_t{1}, 0u, 0u);
    CHECK(title().call(compute.list, 9) == S_OK);
    void *const lists[]{compute.list};
    title().call<void>(compute.queue, 10, 1u, lists);
    CHECK(title().call(compute.queue, queue_signal, done, std::uint64_t{1}) == S_OK);
}
} // namespace

TEST_CASE("a list that waits on memory runs after the work other queues were given before it") {
    if (!device())
        return;
    const LaterLayout layout;
    auto *const held = fence();
    auto *const done = fence();
    // the direct queue is held by a wait the title did not make, then given a list
    auto *const direct = xv::runtime::modules::d3d12x::direct_queue();
    REQUIRE(direct);
    REQUIRE(SUCCEEDED(direct->Wait(held, 1)));
    Work work;
    submit_nothing(work);
    Compute compute;
    wait_on_memory(compute, done);
    CHECK_FALSE(reaches(done, 1, 300));
    REQUIRE(SUCCEEDED(held->Signal(1)));
    CHECK(reaches(done, 1, 10000));
    CHECK(removed_reason() == S_OK);
    done->Release();
    held->Release();
}

TEST_CASE("a list that waits on memory does not wait past another queue's wait not yet over") {
    if (!device())
        return;
    const LaterLayout layout;
    auto *const held = fence();
    auto *const done = fence();
    // the title's own Wait: what the direct queue is given after it may wait for the compute
    // queue, which therefore does not wait for it
    CHECK(title().call(queue(), queue_wait, held, std::uint64_t{1}) == S_OK);
    Work work;
    submit_nothing(work);
    Compute compute;
    wait_on_memory(compute, done);
    CHECK(reaches(done, 1, 10000));
    REQUIRE(SUCCEEDED(held->Signal(1)));
    auto *const direct = xv::runtime::modules::d3d12x::direct_queue();
    auto *const idle = fence();
    REQUIRE(SUCCEEDED(direct->Signal(idle, 1)));
    CHECK(reaches(idle, 1, 10000));
    CHECK(removed_reason() == S_OK);
    idle->Release();
    done->Release();
    held->Release();
}

TEST_CASE("FlushPipelineX orders a list's work as a barrier does") {
    if (!device())
        return;
    const LaterLayout layout;
    std::uint64_t output = 0;
    auto *const results = buffer(4096, output);
    const D3D12_ROOT_PARAMETER parameters[]{root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({1, parameters});
    auto *const add = compute_pipeline(signature, R"(
        RWByteAddressBuffer output : register(u0);
        [numthreads(1, 1, 1)] void main() { output.Store(0, output.Load(0) + 1u); })");
    Work work;
    work.record(33, signature);
    work.record(25, add);
    work.record(45, 0u, output);
    for (int i = 0; i < 4; ++i) {
        work.record(14, 1u, 1u, 1u);
        // flags, address, size: the flags' values are the console's own
        work.record(flush_pipeline, 0xffffffffu, std::uint64_t{0}, std::uint64_t{0});
    }
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(words(output)[0] == 4);
    add->Release();
    signature->Release();
    results->Release();
}

TEST_CASE("a pipeline is made for the host when a list is first given it, of what it was made of") {
    if (!device())
        return;
    const LaterLayout layout;
    std::uint64_t output = 0;
    auto *const results = buffer(4096, output);
    const D3D12_ROOT_PARAMETER parameters[]{root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({1, parameters});
    auto *const code = compile(R"(
        RWByteAddressBuffer output : register(u0);
        [numthreads(1, 1, 1)] void main() { output.Store(0, 42u); })",
                               "cs_5_0");
    // the title's memory, which it may reuse once the pipeline is made
    std::vector<std::uint8_t> shader(static_cast<const std::uint8_t *>(code->GetBufferPointer()),
                                     static_cast<const std::uint8_t *>(code->GetBufferPointer()) +
                                         code->GetBufferSize());
    code->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature;
    desc.CS = {shader.data(), shader.size()};
    ID3D12PipelineState *pipeline = nullptr;
    REQUIRE(title().call(device(), 11, &desc, &__uuidof(ID3D12PipelineState),
                         reinterpret_cast<void **>(&pipeline)) == S_OK);
    std::fill(shader.begin(), shader.end(), std::uint8_t{0});
    // what a title asks of it before it draws with it
    void *same = nullptr;
    CHECK(title().call(pipeline, 0, &__uuidof(ID3D12Pageable), &same) == S_OK);
    CHECK(same == pipeline);
    title().call<ULONG>(pipeline, 2);
    ID3D12Device *parent = nullptr;
    CHECK(title().call(pipeline, 7, &__uuidof(ID3D12Device), reinterpret_cast<void **>(&parent)) ==
          S_OK);
    CHECK(parent);
    if (parent)
        parent->Release();
    Work work;
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(45, 0u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(words(output)[0] == 42);
    CHECK(xv::runtime::modules::d3d12x::made_pipeline(pipeline) == S_OK);
    pipeline->Release();
    signature->Release();
    results->Release();
}

TEST_CASE("a list's copies, fills and values in memory are done when it runs, in its order") {
    if (!device())
        return;
    const LaterLayout layout;
    auto *const memory = static_cast<std::uint32_t *>(
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(memory);
    const auto at = [memory](std::size_t word) {
        return reinterpret_cast<std::uint64_t>(memory + word);
    };
    Work work;
    // CopyMemoryX after FillMemoryWith32BitValueX copies what the fill wrote
    work.record(fill_32, at(0), std::uint64_t{64}, 7u, 0u);
    work.record(copy_memory, at(32), at(0), std::uint64_t{16}, 0u);
    work.record(write_32, at(64), 9u, 0u);
    CHECK(memory[0] == 0);
    CHECK(memory[64] == 0);
    work.run();
    CHECK(memory[15] == 7);
    CHECK(memory[16] == 0);
    CHECK(memory[32] == 7);
    CHECK(memory[35] == 7);
    CHECK(memory[36] == 0);
    CHECK(memory[64] == 9);
    VirtualFree(memory, 0, MEM_RELEASE);
}

TEST_CASE("a copy queue executes both DMA command lists and copy lists with shader transitions") {
    if (!device())
        return;
    std::uint64_t src_addr = 0, dst_addr = 0;
    auto *const src = buffer(4096, src_addr, D3D12_HEAP_TYPE_UPLOAD);
    auto *const dst = buffer(4096, dst_addr);
    const std::uint32_t v0 = 0xdeadbeefu, v1 = 0xcafebabeu;
    std::memcpy(reinterpret_cast<void *>(src_addr), &v0, 4);
    std::memcpy(reinterpret_cast<void *>(src_addr + 4), &v1, 4);

    // CreateCommandQueueX (48) with D3D12_COMMAND_LIST_TYPE_COPY (3), engine 1
    const UINT qdesc[4]{D3D12_COMMAND_LIST_TYPE_COPY, 0, 1, 0};
    void *copy_queue = nullptr;
    REQUIRE(title().call(device(), 48, qdesc, &__uuidof(ID3D12CommandQueue), &copy_queue) == S_OK);
    REQUIRE(copy_queue);
    D3D12_COMMAND_QUEUE_DESC got_qdesc{};
    title().call<D3D12_COMMAND_QUEUE_DESC *>(copy_queue, 26, &got_qdesc);
    CHECK(got_qdesc.Type == D3D12_COMMAND_LIST_TYPE_COPY);

    // CreateCommandListX (73) with DMA list type (16), submitted on the COPY queue
    const UINT dma_desc[4]{16, 2, 0, 0};
    void *dma_list = nullptr;
    REQUIRE(title().call(device(), 73, dma_desc, static_cast<ID3D12CommandAllocator *>(nullptr),
                         static_cast<ID3D12PipelineState *>(nullptr), &__uuidof(ID3D12CommandList),
                         &dma_list) == S_OK);
    REQUIRE(dma_list);
    // DMA CopyBufferRegion (11) and Close (9)
    title().call<void>(dma_list, 11, dst, UINT64{0}, src, UINT64{0}, UINT64{4});
    CHECK(title().call(dma_list, 9) == S_OK);

    // Also a COPY command list (CreateCommandList1 at slot 90, + allocator at 9 + Reset at 10)
    ID3D12CommandAllocator *copy_alloc = nullptr;
    REQUIRE(title().call(device(), 9, UINT{D3D12_COMMAND_LIST_TYPE_COPY},
                         &__uuidof(ID3D12CommandAllocator),
                         reinterpret_cast<void **>(&copy_alloc)) == S_OK);
    void *copy_list = nullptr;
    REQUIRE(title().call(device(), 90, 0u, D3D12_COMMAND_LIST_TYPE_COPY,
                         D3D12_COMMAND_LIST_FLAG_NONE, &__uuidof(ID3D12GraphicsCommandList),
                         &copy_list) == S_OK);
    CHECK(title().call<UINT>(copy_list, 8) == D3D12_COMMAND_LIST_TYPE_COPY);
    REQUIRE(title().call(copy_list, 10, copy_alloc, static_cast<ID3D12PipelineState *>(nullptr)) ==
            S_OK);
    title().call<void>(copy_list, 15, dst, UINT64{4}, src, UINT64{4}, UINT64{4});
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    title().call<void>(copy_list, 26, 1u, &barrier);
    CHECK(title().call(copy_list, 9) == S_OK);

    void *const lists[]{dma_list, copy_list};
    title().call<void>(copy_queue, 10, 2u, lists);
    auto *const done = fence();
    REQUIRE(title().call(copy_queue, 14, done, UINT64{1}) == S_OK);
    CHECK(reaches(done, 1, 10000));
    CHECK(removed_reason() == S_OK);
    CHECK(words(dst_addr)[0] == v0);
    CHECK(words(dst_addr)[1] == v1);

    done->Release();
    static_cast<IUnknown *>(copy_list)->Release();
    copy_alloc->Release();
    static_cast<IUnknown *>(dma_list)->Release();
    static_cast<IUnknown *>(copy_queue)->Release();
    dst->Release();
    src->Release();
}
