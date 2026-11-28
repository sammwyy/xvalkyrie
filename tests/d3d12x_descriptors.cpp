#include "d3d12x_title.hpp"

#include <cstdint>

// The host's descriptors of a heap shaders see are made in a twin the CPU reads, and copied
// to the heap shaders see when a queue is given work: the CPU may write a shader-visible heap
// but not read one, and a title copies descriptors within the heaps it binds.
namespace {
using namespace d3d12x_test;
} // namespace

TEST_CASE("descriptors made and copied in a heap shaders see are the ones shaders read") {
    if (!device())
        return;
    std::uint64_t data_address = 0, output = 0;
    auto *const data = buffer(4096, data_address);
    auto *const results = buffer(4096, output);
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2};
    const D3D12_ROOT_PARAMETER parameters[]{table(&range, 1),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 1)};
    auto *const signature = root_signature({2, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        RWByteAddressBuffer copied : register(u0);
        RWByteAddressBuffer output : register(u1);
        [numthreads(1, 1, 1)] void main() {
            output.Store(0, copied.Load(0));
            output.Store(4, 1u);
        })");
    const Heap heap(3);
    // CreateUnorderedAccessView (19) at slot 0
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
    view.Buffer = {0, 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    title().call<void>(device(), 19, data, static_cast<ID3D12Resource *>(nullptr), &view, heap.cpu);
    Work work;
    work.record(32, 1u, &heap.heap);
    // ClearUnorderedAccessViewUint (53) by the slot, whose CPU handle is in the heap
    const UINT value[4]{0x1234u, 0x1234u, 0x1234u, 0x1234u};
    work.record(53, heap.gpu, heap.cpu, data, value, 0u, static_cast<const D3D12_RECT *>(nullptr));
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    work.record(26, 1u, &barrier);
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(35, 0u, heap.gpu);
    work.record(45, 1u, output);
    work.record(14, 1u, 1u, 1u);
    // CopyDescriptorsSimple (24) after the list is recorded: slot 0 to slot 2, which the table
    // reads, before the list runs
    title().call<void>(device(), 24, 1u, heap.cpu + 64, heap.cpu,
                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(words(output)[1] == 1);
    CHECK(words(output)[0] == 0x1234u);
    pipeline->Release();
    signature->Release();
    results->Release();
    data->Release();
}

TEST_CASE("unwritten shader-visible heap slots read as null textures without dropping the device") {
    if (!device())
        return;
    std::uint64_t output = 0;
    auto *const results = buffer(4096, output);
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 3};
    const D3D12_ROOT_PARAMETER parameters[]{table(&range, 1),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({2, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        Texture2D<float4> unwritten : register(t0);
        RWByteAddressBuffer output : register(u0);
        [numthreads(1, 1, 1)] void main() {
            float4 v = unwritten.Load(int3(0, 0, 0));
            output.Store4(0, asuint(v));
            output.Store(16, 1u);
        })");
    const Heap heap(8);
    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(35, 0u, heap.gpu);
    work.record(45, 1u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(words(output)[4] == 1u);
    for (int i = 0; i < 4; ++i)
        CHECK(words(output)[i] == 0u);
    pipeline->Release();
    signature->Release();
    results->Release();
}
