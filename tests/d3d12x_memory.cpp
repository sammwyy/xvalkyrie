#include "d3d12x_title.hpp"
#include "runtime/modules/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Memory of a Series title's that no buffer of the host's holds: the host's GPU cannot see it,
// and an address of it the host is given removes the device. Every view and binding of such
// memory views nothing instead, and the device stays.
namespace {
using namespace d3d12x_test;

constexpr std::uint32_t pattern = 0x5a5a5a5a;
// memory of the title's, filled with `pattern`; `readable` false makes it no-access
struct TitleMemory {
    std::uint8_t *base = nullptr;
    static constexpr std::size_t size = 64 << 10;
    explicit TitleMemory(bool readable = true) {
        base = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(base);
        for (std::size_t at = 0; at < size; at += 4)
            std::memcpy(base + at, &pattern, 4);
        DWORD old = 0;
        if (!readable)
            REQUIRE(VirtualProtect(base, size, PAGE_NOACCESS, &old));
    }
    ~TitleMemory() { VirtualFree(base, 0, MEM_RELEASE); }
    std::uint64_t address(std::size_t offset = 0) const {
        return reinterpret_cast<std::uint64_t>(base + offset);
    }
    std::uint32_t word(std::size_t offset) const {
        std::uint32_t value;
        std::memcpy(&value, base + offset, sizeof value);
        return value;
    }
};
} // namespace

TEST_CASE("constant buffer and root views of memory no buffer holds read a copy of it, or "
          "nothing where the CPU cannot read it") {
    if (!device())
        return;
    const TitleMemory memory, hidden(false);
    std::uint64_t output = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0};
    const D3D12_ROOT_PARAMETER parameters[]{
        table(&range, 1), root_view(D3D12_ROOT_PARAMETER_TYPE_CBV, 1),
        root_view(D3D12_ROOT_PARAMETER_TYPE_SRV, 0), root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0),
        root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 1)};
    auto *const signature = root_signature({5, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        cbuffer Table : register(b0) { uint4 from_table; };
        cbuffer Root : register(b1) { uint4 from_root; };
        ByteAddressBuffer root_view : register(t0);
        RWByteAddressBuffer output : register(u0);
        RWByteAddressBuffer unseen : register(u1);
        [numthreads(1, 1, 1)] void main() {
            output.Store4(0, from_table);
            output.Store4(16, from_root);
            output.Store4(32, root_view.Load4(0));
            unseen.Store(0, 7u);
            output.Store(48, 1u);
        })");
    // CreateConstantBufferView (17) of the title's memory, into a table: on the Series console
    // a copy of what is there when the view is made
    const Heap heap(1);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC view{memory.address(), 256};
    title().call<void>(device(), 17, &view, heap.cpu);
    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(35, 0u, heap.gpu);
    // a root constant buffer view of memory the CPU can read is a copy of what is there; a
    // root view of memory it cannot read reads nothing, and root writes there go nowhere
    work.record(41, 1u, memory.address());
    work.record(43, 2u, hidden.address());
    work.record(45, 3u, output);
    work.record(45, 4u, memory.address(4096));
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    const auto *const result = words(output);
    CHECK(result[12] == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(result[i] == pattern);
        CHECK(result[4 + i] == pattern);
        CHECK(result[8 + i] == 0);
    }
    // what the GPU wrote went nowhere near the title's memory
    CHECK(memory.word(4096) == pattern);
    pipeline->Release();
    signature->Release();
    buffer->Release();
}

TEST_CASE("vertex, index and stream output buffers in memory no buffer holds are none") {
    if (!device())
        return;
    const TitleMemory memory, hidden(false);
    auto *const signature = root_signature(
        {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
    auto *const code = compile(
        "float4 main(float4 position : POSITION) : SV_Position { return position; }", "vs_5_0");
    const D3D12_INPUT_ELEMENT_DESC element{
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
        0,          0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
        0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature;
    desc.VS = {code->GetBufferPointer(), code->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.InputLayout = {&element, 1};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc.Count = 1;
    // CreateGraphicsPipelineState (10)
    ID3D12PipelineState *pipeline = nullptr;
    CHECK(title().call(device(), 10, &desc, &__uuidof(ID3D12PipelineState),
                       reinterpret_cast<void **>(&pipeline)) == S_OK);
    code->Release();
    REQUIRE(pipeline);
    Work work;
    const D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
    const D3D12_RECT scissor{0, 0, 64, 64};
    const D3D12_VERTEX_BUFFER_VIEW vertices{memory.address(), 4096, 16};
    const D3D12_INDEX_BUFFER_VIEW indices{hidden.address(), 4096, DXGI_FORMAT_R16_UINT};
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW stream{memory.address(8192), 4096, memory.address(12288)};
    work.record(34, signature);
    work.record(25, pipeline);
    work.record(20, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    work.record(21, 1u, &viewport);
    work.record(22, 1u, &scissor);
    work.record(48, 0u, 1u, &vertices);
    work.record(47, &indices);
    work.record(49, 0u, 1u, &stream);
    work.record(12, 3u, 1u, 0u, 0u);
    work.record(13, 3u, 1u, 0u, 0, 0u);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(memory.word(8192) == pattern);
    CHECK(memory.word(12288) == pattern);
    pipeline->Release();
    signature->Release();
}

TEST_CASE("WriteBufferImmediate writes the title's own memory itself") {
    if (!device())
        return;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    if (FAILED(device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options,
                                             sizeof options)) ||
        !(options.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT)) {
        WARN("the host has no WriteBufferImmediate on a direct queue");
        return;
    }
    const TitleMemory memory;
    std::uint64_t output = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameters[]{{memory.address(16384), 0x1234},
                                                            {output + 64, 0x4321}};
    Work work;
    // list slot 111 on the Series console, ID3D12GraphicsCommandList2's only method
    work.record(111, 2u, parameters, static_cast<const D3D12_WRITEBUFFERIMMEDIATE_MODE *>(nullptr));
    // both when the list runs: the title's memory when a queue is given it
    CHECK(memory.word(16384) == pattern);
    CHECK(words(output)[16] == 0);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(memory.word(16384) == 0x1234);
    CHECK(words(output)[16] == 0x4321);
    buffer->Release();
}

TEST_CASE("a view of memory at an address reads a copy of it, and a descriptor the title "
          "writes reads nothing there") {
    if (!device())
        return;
    const LayoutsShown layouts; // a view at an address is a later kit's (see view_at)
    const TitleMemory memory;
    std::uint64_t output = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    const D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 1},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 2}};
    const D3D12_ROOT_PARAMETER parameters[]{table(ranges, 3),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({2, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        ByteAddressBuffer seen : register(t0);
        ByteAddressBuffer written_by_title : register(t1);
        RWByteAddressBuffer output : register(u0);
        RWByteAddressBuffer unseen : register(u1);
        [numthreads(1, 1, 1)] void main() {
            output.Store4(0, seen.Load4(0));
            output.Store4(16, written_by_title.Load4(0));
            unseen.Store(0, 9u);
            output.Store(32, 1u);
        })");
    const Heap heap(3);
    // device slots 70 and 71: the memory's description, the host's view and the address
    D3D12_RESOURCE_DESC of{};
    of.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    of.Width = TitleMemory::size;
    of.Height = of.DepthOrArraySize = of.MipLevels = 1;
    of.SampleDesc.Count = 1;
    of.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        std::uint64_t address;
    } shader_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER,
                   D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
                  memory.address()};
    shader_view.view.Buffer = {0, 16, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    struct {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view;
        std::uint64_t address, counter;
    } access_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER}, memory.address(4096), 0};
    access_view.view.Buffer = {0, 16, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    title().call<void>(device(), 70, &of, &shader_view, heap.cpu);
    title().call<void>(device(), 71, &of, &access_view, heap.cpu + 32);
    // a buffer descriptor (V#) the title writes itself: its address, its record count, and
    // type 0 in the top of its last dword
    const std::array<std::uint32_t, 4> descriptor{
        static_cast<std::uint32_t>(memory.address(8192)),
        static_cast<std::uint32_t>(memory.address(8192) >> 32) & 0xffff, 64, 0x00000fac};
    std::memcpy(reinterpret_cast<void *>(heap.cpu + 64), descriptor.data(), sizeof descriptor);
    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(35, 0u, heap.gpu);
    work.record(45, 1u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    const auto *const result = words(output);
    CHECK(result[8] == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(result[i] == pattern);
        CHECK(result[4 + i] == 0);
    }
    CHECK(memory.word(4096) == pattern);
    pipeline->Release();
    signature->Release();
    buffer->Release();
}

// The host drops the device over a buffer view that reaches past the end of its buffer, or
// has no elements: a title's view near the end of a large buffer stops at the end, and reads
// past it give 0.
TEST_CASE("a buffer view near the end of a large buffer reaches no further than the buffer") {
    if (!device())
        return;
    const LayoutsShown layouts; // a view at an address is a later kit's (see view_at)
    constexpr std::uint64_t size = 64 << 20;
    std::uint64_t output = 0, end = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    auto *const large = d3d12x_test::buffer(size, end);
    end += size;
    for (std::uint64_t at = end - 4096; at < end; at += 4)
        std::memcpy(reinterpret_cast<void *>(at), &pattern, 4);
    const D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 3}};
    const D3D12_ROOT_PARAMETER parameters[]{table(ranges, 2),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({2, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        ByteAddressBuffer at_address : register(t0);
        ByteAddressBuffer of_resource : register(t1);
        ByteAddressBuffer written_by_title : register(t2);
        RWByteAddressBuffer output : register(u0);
        RWByteAddressBuffer near_end : register(u1);
        [numthreads(1, 1, 1)] void main() {
            output.Store4(0, at_address.Load4(48));
            output.Store4(16, at_address.Load4(64));
            output.Store4(32, of_resource.Load4(48));
            output.Store4(48, of_resource.Load4(64));
            output.Store4(64, written_by_title.Load4(0));
            for (uint i = 0; i < 16; ++i)
                near_end.Store(i * 4, 100 + i);
            output.Store(80, 1u);
        })");
    const Heap heap(4);
    D3D12_RESOURCE_DESC of{};
    of.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    of.Width = 4096;
    of.Height = of.DepthOrArraySize = of.MipLevels = 1;
    of.SampleDesc.Count = 1;
    of.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // device slot 70: 1024 words of memory 64 bytes before the buffer's end
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        std::uint64_t address;
    } shader_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER,
                   D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
                  end - 64};
    shader_view.view.Buffer = {0, 1024, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    title().call<void>(device(), 70, &of, &shader_view, heap.cpu);
    // CreateShaderResourceView (18) of the buffer itself, from the same word on
    auto of_buffer = shader_view.view;
    of_buffer.Buffer.FirstElement = (size - 64) / 4;
    title().call<void>(device(), 18, large, &of_buffer, heap.cpu + 32);
    // a buffer descriptor (V#) the title writes itself, 2 bytes before the end: not one word
    // of it is in the buffer
    const std::array<std::uint32_t, 4> descriptor{
        static_cast<std::uint32_t>(end - 2), static_cast<std::uint32_t>((end - 2) >> 32) & 0xffff,
        64, 0x00000fac};
    std::memcpy(reinterpret_cast<void *>(heap.cpu + 64), descriptor.data(), sizeof descriptor);
    // device slot 71: 64 words to write, 32 bytes before the end
    struct {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view;
        std::uint64_t address, counter;
    } access_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER}, end - 32, 0};
    access_view.view.Buffer = {0, 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    title().call<void>(device(), 71, &of, &access_view, heap.cpu + 96);
    CHECK(removed_reason() == S_OK);
    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(35, 0u, heap.gpu);
    work.record(45, 1u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    const auto *const result = words(output);
    CHECK(result[20] == 1);
    for (int i = 0; i < 4; ++i) {
        // the last words of the buffer, and none past it
        CHECK(result[i] == pattern);
        CHECK(result[4 + i] == 0);
        CHECK(result[8 + i] == pattern);
        CHECK(result[12 + i] == 0);
        CHECK(result[16 + i] == 0);
    }
    // the writes that were in the buffer
    for (std::uint32_t i = 0; i < 8; ++i)
        CHECK(words(end - 32)[i] == 100 + i);
    pipeline->Release();
    signature->Release();
    large->Release();
    buffer->Release();
}

// The host takes a raw view only from 16 bytes into its buffer on, and drops the device over
// one from elsewhere, where the console's starts at any word: a raw view to read is of a copy
// of what it views, made again with the view, and one to write is of nothing.
TEST_CASE("a raw view that does not start on 16 bytes reads a copy, and one to write is none") {
    if (!device())
        return;
    const LayoutsShown layouts; // a view at an address is a later kit's (see view_at)
    std::uint64_t output = 0, memory = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    auto *const viewed = d3d12x_test::buffer(4096, memory);
    // word i of the memory is base + i
    const auto fill = [&](std::uint32_t base) {
        for (std::uint32_t i = 0; i < 1024; ++i) {
            const auto value = base + i;
            std::memcpy(reinterpret_cast<void *>(memory + i * 4), &value, 4);
        }
    };
    const D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 3}};
    const D3D12_ROOT_PARAMETER parameters[]{table(ranges, 2),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({2, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        ByteAddressBuffer at_address : register(t0);
        ByteAddressBuffer of_resource : register(t1);
        ByteAddressBuffer written_by_title : register(t2);
        RWByteAddressBuffer output : register(u0);
        RWByteAddressBuffer to_write : register(u1);
        [numthreads(1, 1, 1)] void main() {
            output.Store4(0, at_address.Load4(0));
            output.Store4(16, of_resource.Load4(0));
            output.Store4(32, written_by_title.Load4(0));
            to_write.Store(0, 77u);
            output.Store(48, 1u);
        })");
    const Heap heap(4);
    D3D12_RESOURCE_DESC of{};
    of.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    of.Width = 4096;
    of.Height = of.DepthOrArraySize = of.MipLevels = 1;
    of.SampleDesc.Count = 1;
    of.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        std::uint64_t address;
    } shader_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER,
                   D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
                  memory + 4};
    shader_view.view.Buffer = {0, 8, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    auto of_buffer = shader_view.view;
    of_buffer.Buffer.FirstElement = 2;
    // a buffer descriptor (V#) 20 bytes in, of 32 bytes
    const std::array<std::uint32_t, 4> descriptor{
        static_cast<std::uint32_t>(memory + 20),
        static_cast<std::uint32_t>((memory + 20) >> 32) & 0xffff, 32, 0x00000fac};
    struct {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view;
        std::uint64_t address, counter;
    } access_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER}, memory + 4, 0};
    access_view.view.Buffer = {0, 8, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    // the views made (device slots 70, 18 and 71, and the descriptor), then a dispatch
    const auto run = [&] {
        title().call<void>(device(), 70, &of, &shader_view, heap.cpu);
        title().call<void>(device(), 18, viewed, &of_buffer, heap.cpu + 32);
        std::memcpy(reinterpret_cast<void *>(heap.cpu + 64), descriptor.data(), sizeof descriptor);
        title().call<void>(device(), 71, &of, &access_view, heap.cpu + 96);
        CHECK(removed_reason() == S_OK);
        Work work;
        work.record(32, 1u, &heap.heap);
        work.record(33, signature);
        work.record(25, pipeline);
        work.record(35, 0u, heap.gpu);
        work.record(45, 1u, output);
        work.record(14, 1u, 1u, 1u);
        work.run();
        CHECK(removed_reason() == S_OK);
    };
    // what is there when the views are made, and again when they are made again
    for (const std::uint32_t base : {0x1000u, 0x2000u}) {
        fill(base);
        run();
        const auto *const result = words(output);
        CHECK(result[12] == 1);
        for (std::uint32_t i = 0; i < 4; ++i) {
            CHECK(result[i] == base + 1 + i);
            CHECK(result[4 + i] == base + 2 + i);
            CHECK(result[8 + i] == base + 5 + i);
        }
        // the write went nowhere
        CHECK(words(memory)[1] == base + 1);
    }
    pipeline->Release();
    signature->Release();
    viewed->Release();
    buffer->Release();
}

TEST_CASE("a texture placed in the title's memory has that memory's address") {
    if (!device())
        return;
    const TitleMemory memory;
    const PlacementsGone placements;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 64;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    // device slot 44, CreatePlacedResourceX, then the resource's GetGPUVirtualAddress (11)
    const auto place = [&](std::uint64_t address) {
        void *texture = nullptr;
        REQUIRE(title().call(device(), 44, address, &desc, D3D12_RESOURCE_STATE_COMMON,
                             static_cast<const D3D12_CLEAR_VALUE *>(nullptr),
                             &__uuidof(ID3D12Resource), &texture) == S_OK);
        REQUIRE(texture);
        return static_cast<ID3D12Resource *>(texture);
    };
    auto *const first = place(memory.address());
    auto *const second = place(memory.address(32768));
    CHECK(title().call<std::uint64_t>(first, 11) == memory.address());
    CHECK(title().call<std::uint64_t>(second, 11) == memory.address(32768));
    first->Release();
    second->Release();
    CHECK(removed_reason() == S_OK);
}

TEST_CASE("a heap opened over a committed section in a placeholder is coherent with the GPU") {
    if (!device())
        return;
    ID3D12Device3 *dev3 = nullptr;
    REQUIRE(SUCCEEDED(device()->QueryInterface(IID_PPV_ARGS(&dev3))));
    constexpr std::size_t chunk = 2u << 20;
    constexpr std::size_t total = 8 * chunk;
    auto *const proc = GetCurrentProcess();
    auto *const base = static_cast<std::uint8_t *>(VirtualAlloc2(
        proc, nullptr, total, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    REQUIRE(base);
    auto *const slot0 = base + chunk;
    auto *const slot1 = base + 3 * chunk;
    REQUIRE(VirtualFree(slot0, chunk, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    REQUIRE(VirtualFree(slot1, chunk, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_ALL_ACCESS,
                                        PAGE_READWRITE, SEC_COMMIT, chunk, nullptr, nullptr, 0);
    REQUIRE(section);
    REQUIRE(MapViewOfFile3(section, proc, slot0, 0, chunk, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                           nullptr, 0) == slot0);

    ID3D12Heap *heap = nullptr;
    REQUIRE(SUCCEEDED(dev3->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&heap))));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = chunk;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *placed = nullptr;
    REQUIRE(SUCCEEDED(device()->CreatePlacedResource(heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&placed))));
    const auto gpu = placed->GetGPUVirtualAddress();
    REQUIRE(gpu != 0);

    const D3D12_ROOT_PARAMETER params[]{root_view(D3D12_ROOT_PARAMETER_TYPE_SRV, 0),
                                        root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const sig = root_signature({2, params});
    auto *const pipe = compute_pipeline(sig, R"(
        ByteAddressBuffer src : register(t0);
        RWByteAddressBuffer dst : register(u0);
        [numthreads(1, 1, 1)] void main() {
            dst.Store(0, src.Load(0) ^ 0x5a5a0000u);
        })");

    const auto run_at = [&](std::uint64_t src_off, std::uint64_t dst_off) {
        Work work;
        work.record(33, sig);
        work.record(25, pipe);
        work.record(43, 0u, gpu + src_off);
        work.record(45, 1u, gpu + dst_off);
        work.record(14, 1u, 1u, 1u);
        work.run();
        CHECK(removed_reason() == S_OK);
    };

    auto *w0 = reinterpret_cast<std::uint32_t *>(slot0);
    w0[0] = 0x12340001u;
    run_at(0, 4096);
    CHECK(w0[1024] == (0x12340001u ^ 0x5a5a0000u));

    // a later CPU write is seen without copying, and remapping the view leaves the heap live
    w0[0] = 0xabcd0002u;
    DWORD old_prot = 0;
    REQUIRE(VirtualProtect(slot0, 65536, PAGE_READONLY, &old_prot));
    REQUIRE(VirtualProtect(slot0, 65536, PAGE_READWRITE, &old_prot));
    REQUIRE(UnmapViewOfFile2(proc, slot0, MEM_PRESERVE_PLACEHOLDER));
    REQUIRE(MapViewOfFile3(section, proc, slot1, 0, chunk, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                           nullptr, 0) == slot1);
    run_at(0, 8192);
    auto *w1 = reinterpret_cast<std::uint32_t *>(slot1);
    CHECK(w1[2048] == (0xabcd0002u ^ 0x5a5a0000u));

    pipe->Release();
    sig->Release();
    placed->Release();
    heap->Release();
    dev3->Release();
    CHECK(UnmapViewOfFile2(proc, slot1, MEM_PRESERVE_PLACEHOLDER));
    CHECK(CloseHandle(section));
    // coalesce placeholders before releasing
    CHECK(VirtualFree(base, total, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
    CHECK(VirtualFree(base, 0, MEM_RELEASE));
}

TEST_CASE("title physical pages are read past 64 KiB and written directly by the GPU") {
    if (!device())
        return;
    static xv::runtime::Registry xmem_reg = [] {
        xv::runtime::Registry made;
        xv::runtime::modules::install_xmem(made);
        return made;
    }();
    const auto xmem_fn = [&](const char *name) {
        auto *const fn = std::string_view(name).starts_with("XMem")
                             ? xmem_reg.find("xmem.dll", name)
                             : xmem_reg.find_api(name);
        REQUIRE(fn);
        return fn;
    };
    constexpr DWORD large_pages = 0x20000000;
    constexpr std::size_t page = 1 << 16, count = 4; // 256 KiB (> 64 KiB)
    ULONG_PTR pages[count]{}, got = count;
    REQUIRE(reinterpret_cast<BOOL(WINAPI *)(ULONGLONG, ULONG_PTR *, ULONG_PTR *)>(
        xmem_fn("XMemAllocatePhysicalPages"))(0, &got, pages));
    REQUIRE(got == count);
    auto *const base = static_cast<std::uint8_t *>(
        reinterpret_cast<void *(WINAPI *)(void *, SIZE_T, DWORD, ULONGLONG, DWORD)>(
            xmem_fn("XMemVirtualAlloc"))(nullptr, count * page, MEM_RESERVE | large_pages, 0,
                                         PAGE_READWRITE));
    REQUIRE(base);
    REQUIRE(reinterpret_cast<void *(WINAPI *)(void *, ULONG_PTR, ULONG_PTR *)>(
                xmem_fn("XMemMapPhysicalPages"))(base, count, pages) == base);

    const auto addr = reinterpret_cast<std::uint64_t>(base);
    // write a word at +192 KiB (well past 64 KiB) and let the GPU read it via root SRV and
    // write back via root UAV into +128 KiB of the title's own physical pages
    constexpr std::uint32_t sentinel = 0xCAFE1234u;
    std::memcpy(base + 3 * page + 16, &sentinel, sizeof sentinel);

    const D3D12_ROOT_PARAMETER params[]{root_view(D3D12_ROOT_PARAMETER_TYPE_SRV, 0),
                                        root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const sig = root_signature({2, params});
    auto *const pipe = compute_pipeline(sig, R"(
        ByteAddressBuffer src : register(t0);
        RWByteAddressBuffer dst : register(u0);
        [numthreads(1, 1, 1)] void main() {
            dst.Store(0, src.Load(196624) ^ 0x00FF00FFu);
        })");
    Work work;
    work.record(33, sig);
    work.record(25, pipe);
    work.record(43, 0u, addr);
    work.record(45, 1u, addr + 2 * page);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    std::uint32_t out = 0;
    std::memcpy(&out, base + 2 * page, sizeof out);
    CHECK(out == (sentinel ^ 0x00FF00FFu));

    pipe->Release();
    sig->Release();
    CHECK(reinterpret_cast<BOOL(WINAPI *)(void *, SIZE_T, DWORD)>(xmem_fn("VirtualFree"))(
        base, 0, MEM_RELEASE));
    CHECK(reinterpret_cast<BOOL(WINAPI *)(ULONG_PTR, ULONG_PTR *)>(
        xmem_fn("XMemFreePhysicalPages"))(count, pages));
}

TEST_CASE("unaligned constant buffers and views at addresses in title physical pages work") {
    if (!device())
        return;
    const LayoutsShown layouts;
    static xv::runtime::Registry xmem_reg = [] {
        xv::runtime::Registry made;
        xv::runtime::modules::install_xmem(made);
        return made;
    }();
    const auto xmem_fn = [&](const char *name) {
        auto *const fn = std::string_view(name).starts_with("XMem")
                             ? xmem_reg.find("xmem.dll", name)
                             : xmem_reg.find_api(name);
        REQUIRE(fn);
        return fn;
    };
    constexpr DWORD large_pages = 0x20000000;
    constexpr std::size_t page = 1 << 16, count = 2;
    ULONG_PTR pages[count]{}, got = count;
    REQUIRE(reinterpret_cast<BOOL(WINAPI *)(ULONGLONG, ULONG_PTR *, ULONG_PTR *)>(
        xmem_fn("XMemAllocatePhysicalPages"))(0, &got, pages));
    REQUIRE(got == count);
    auto *const base = static_cast<std::uint8_t *>(
        reinterpret_cast<void *(WINAPI *)(void *, SIZE_T, DWORD, ULONGLONG, DWORD)>(
            xmem_fn("XMemVirtualAlloc"))(nullptr, count * page, MEM_RESERVE | large_pages, 0,
                                         PAGE_READWRITE));
    REQUIRE(base);
    REQUIRE(reinterpret_cast<void *(WINAPI *)(void *, ULONG_PTR, ULONG_PTR *)>(
                xmem_fn("XMemMapPhysicalPages"))(base, count, pages) == base);
    const auto addr = reinterpret_cast<std::uint64_t>(base);

    const std::uint32_t c0 = 0x11223344u, c1 = 0x55667788u, s0 = 0x99aabbccu;
    std::memcpy(base + 64, &c0, 4);
    std::memcpy(base + 320, &c1, 4);
    std::memcpy(base + 4096, &s0, 4);

    const D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    const D3D12_ROOT_PARAMETER params[]{table(ranges, 3),
                                        root_view(D3D12_ROOT_PARAMETER_TYPE_CBV, 1)};
    auto *const sig = root_signature({2, params});
    auto *const pipe = compute_pipeline(sig, R"(
        cbuffer TableC : register(b0) { uint tbl_c; };
        cbuffer RootC : register(b1) { uint root_c; };
        ByteAddressBuffer srv_mem : register(t0);
        RWByteAddressBuffer uav_mem : register(u0);
        [numthreads(1, 1, 1)] void main() {
            uav_mem.Store(0, tbl_c);
            uav_mem.Store(4, root_c);
            uav_mem.Store(8, srv_mem.Load(0));
        })");

    const Heap heap(3);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{addr + 64, 256};
    title().call<void>(device(), 17, &cbv, heap.cpu);

    D3D12_RESOURCE_DESC of{};
    of.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    of.Width = 4096;
    of.Height = of.DepthOrArraySize = of.MipLevels = 1;
    of.SampleDesc.Count = 1;
    of.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        std::uint64_t address;
    } srv_at{{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER,
              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
             addr + 4096};
    srv_at.view.Buffer = {0, 16, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    struct {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view;
        std::uint64_t address, counter;
    } uav_at{{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER}, addr + 8192, 0};
    uav_at.view.Buffer = {0, 16, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    title().call<void>(device(), 70, &of, &srv_at, heap.cpu + 32);
    title().call<void>(device(), 71, &of, &uav_at, heap.cpu + 64);

    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, sig);
    work.record(25, pipe);
    work.record(35, 0u, heap.gpu);
    work.record(41, 1u, addr + 320);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    const auto *const out = reinterpret_cast<const std::uint32_t *>(base + 8192);
    CHECK(out[0] == c0);
    CHECK(out[1] == c1);
    CHECK(out[2] == s0);

    pipe->Release();
    sig->Release();
    CHECK(reinterpret_cast<BOOL(WINAPI *)(void *, SIZE_T, DWORD)>(xmem_fn("VirtualFree"))(
        base, 0, MEM_RELEASE));
    CHECK(reinterpret_cast<BOOL(WINAPI *)(ULONG_PTR, ULONG_PTR *)>(
        xmem_fn("XMemFreePhysicalPages"))(count, pages));
}

TEST_CASE("a placed texture written in one format is read through a view at its address in "
          "another format of the same texel width") {
    if (!device())
        return;
    const LayoutsShown layouts;
    const PlacementsGone placements;
    const TitleMemory memory;
    std::uint64_t output = 0;
    auto *const results = d3d12x_test::buffer(4096, output);

    D3D12_RESOURCE_DESC uav_tex_desc{};
    uav_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    uav_tex_desc.Width = uav_tex_desc.Height = 16;
    uav_tex_desc.DepthOrArraySize = uav_tex_desc.MipLevels = 1;
    uav_tex_desc.Format = DXGI_FORMAT_R32_UINT;
    uav_tex_desc.SampleDesc.Count = 1;
    uav_tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    void *placed_raw = nullptr;
    REQUIRE(title().call(device(), 44, memory.address(), &uav_tex_desc,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         static_cast<const D3D12_CLEAR_VALUE *>(nullptr), &__uuidof(ID3D12Resource),
                         &placed_raw) == S_OK);
    auto *const uav_tex = static_cast<ID3D12Resource *>(placed_raw);

    auto srv_tex_desc = uav_tex_desc;
    srv_tex_desc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    srv_tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const Heap heap(2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
    title().call<void>(device(), 19, uav_tex, static_cast<ID3D12Resource *>(nullptr), &uav_desc,
                       heap.cpu);
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        std::uint64_t address;
    } srv_at{{DXGI_FORMAT_R9G9B9E5_SHAREDEXP, D3D12_SRV_DIMENSION_TEXTURE2D,
              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
             memory.address()};
    srv_at.view.Texture2D.MipLevels = 1;
    title().call<void>(device(), 70, &srv_tex_desc, &srv_at, heap.cpu + 32);

    const D3D12_DESCRIPTOR_RANGE write_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    const D3D12_ROOT_PARAMETER write_params[]{table(&write_range, 1)};
    auto *const write_sig = root_signature({1, write_params});
    // R9G9B9E5: exponent field 24 (2^(24 - 15 - 9) = 1.0) with R=1, G=2, B=4 -> (1, 2, 4)
    auto *const write_pipe = compute_pipeline(write_sig, R"(
        RWTexture2D<uint> tex : register(u0);
        [numthreads(1, 1, 1)] void main() {
            tex[uint2(0, 0)] = (24u << 27) | (4u << 18) | (2u << 9) | 1u;
        })");

    const D3D12_DESCRIPTOR_RANGE read_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1};
    const D3D12_ROOT_PARAMETER read_params[]{table(&read_range, 1),
                                             root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const read_sig = root_signature({2, read_params});
    auto *const read_pipe = compute_pipeline(read_sig, R"(
        Texture2D<float4> tex : register(t0);
        RWByteAddressBuffer out_buf : register(u0);
        [numthreads(1, 1, 1)] void main() {
            out_buf.Store4(0, asuint(tex.Load(int3(0, 0, 0))));
        })");

    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, write_sig);
    work.record(25, write_pipe);
    work.record(35, 0u, heap.gpu);
    work.record(14, 1u, 1u, 1u);
    work.run();

    // next ExecuteCommandLists syncs aliased textures through an L1 staging buffer
    work.record(32, 1u, &heap.heap);
    work.record(33, read_sig);
    work.record(25, read_pipe);
    work.record(35, 0u, heap.gpu);
    work.record(45, 1u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();

    CHECK(removed_reason() == S_OK);
    CHECK(words(output)[0] == 0x3f800000u); // 1.0f
    CHECK(words(output)[1] == 0x40000000u); // 2.0f
    CHECK(words(output)[2] == 0x40800000u); // 4.0f

    read_pipe->Release();
    read_sig->Release();
    write_pipe->Release();
    write_sig->Release();
    uav_tex->Release();
    results->Release();
}
