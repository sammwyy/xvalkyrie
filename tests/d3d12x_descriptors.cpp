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

TEST_CASE("structured buffer views with explicit formats and cross-dimension texture views keep "
          "the device alive") {
    if (!device())
        return;
    std::uint64_t buf_addr = 0;
    auto *const buf = buffer(4096, buf_addr);
    const Heap cbv_srv_uav(8);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer = {0, 4, 16, D3D12_BUFFER_SRV_FLAG_NONE};
    title().call<void>(device(), 18, buf, &srv, cbv_srv_uav.cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer = {0, 4, 16, 0, D3D12_BUFFER_UAV_FLAG_NONE};
    title().call<void>(device(), 19, buf, static_cast<ID3D12Resource *>(nullptr), &uav,
                       cbv_srv_uav.cpu + 32);

    const D3D12_HEAP_PROPERTIES def_heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 4;
    tex_desc.Height = 4;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 2;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc = {1, 0};
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *tex = nullptr;
    REQUIRE(title().call(device(), 27, &def_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                         D3D12_RESOURCE_STATE_COMMON, static_cast<D3D12_CLEAR_VALUE *>(nullptr),
                         &__uuidof(ID3D12Resource), reinterpret_cast<void **>(&tex)) == S_OK);
    REQUIRE(tex);

    // Map (8) on a multi-mip default texture returns CPU scratch covering GetResourceAllocationInfo
    void *mapped = nullptr;
    REQUIRE(title().call(tex, 8, 0u, static_cast<D3D12_RANGE *>(nullptr), &mapped) == S_OK);
    REQUIRE(mapped);
    const auto alloc = device()->GetResourceAllocationInfo(0, 1, &tex_desc);
    std::memset(mapped, 0, static_cast<std::size_t>(alloc.SizeInBytes));
    title().call<void>(tex, 9, 0u, static_cast<D3D12_RANGE *>(nullptr));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv3d{};
    srv3d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv3d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv3d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv3d.Texture3D = {0, 1, 0.f};
    title().call<void>(device(), 18, tex, &srv3d, cbv_srv_uav.cpu + 64);

    const Heap rtv_heap(4, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC rtv3d{};
    rtv3d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv3d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
    rtv3d.Texture3D = {0, 5, 1};
    title().call<void>(device(), 20, tex, &rtv3d, rtv_heap.cpu);

    // Unaligned CreateConstantBufferView (17) inside a mapped buffer copies to a 256-byte aligned
    // slot
    void *buf_cpu = nullptr;
    REQUIRE(buf->Map(0, nullptr, &buf_cpu) == S_OK);
    std::memset(buf_cpu, 0x2a, 256);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC unaligned_cbv{buf->GetGPUVirtualAddress() + 0x30, 176};
    title().call<void>(device(), 17, &unaligned_cbv, cbv_srv_uav.cpu + 96);
    buf->Unmap(0, nullptr);

    // ResourceBarrier (26) on an UPLOAD heap buffer is ignored without dropping the device
    ID3D12CommandAllocator *alloc_cmd = nullptr;
    ID3D12GraphicsCommandList *cmd = nullptr;
    REQUIRE(device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&alloc_cmd)) == S_OK);
    REQUIRE(title().call(device(), 12, 0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_cmd,
                         static_cast<ID3D12PipelineState *>(nullptr),
                         &__uuidof(ID3D12GraphicsCommandList),
                         reinterpret_cast<void **>(&cmd)) == S_OK);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER};
    title().call<void>(cmd, 26, 1u, &barrier);
    CHECK(title().call(cmd, 9) == S_OK);
    cmd->Release();
    alloc_cmd->Release();

    CHECK(removed_reason() == S_OK);
    tex->Release();
    buf->Release();
}
