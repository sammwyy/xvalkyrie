#include "d3d12x_title.hpp"

#include <cstdint>
#include <cstring>

// The ways a Series list dispatches besides Dispatch (14): DispatchX (78), which is Dispatch
// with flags that only tune the console's GPU, and ExecuteIndirect (59) with the desktop's
// dispatch argument or the console's own (DISPATCHX, 104). Each runs the groups it asks for,
// with what the list was set up with.
namespace {
using namespace d3d12x_test;

// a compute pipeline that counts its groups and marks each in a buffer of the title's
struct Counting {
    std::uint64_t output = 0;
    ID3D12Resource *buffer = nullptr;
    ID3D12RootSignature *signature = nullptr;
    ID3D12PipelineState *pipeline = nullptr;
    Counting() {
        buffer = d3d12x_test::buffer(4096, output);
        const D3D12_ROOT_PARAMETER parameters[]{root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
        signature = root_signature({1, parameters});
        pipeline = compute_pipeline(signature, R"(
            RWByteAddressBuffer output : register(u0);
            [numthreads(1, 1, 1)] void main(uint3 group : SV_GroupID) {
                uint before;
                output.InterlockedAdd(0, 1u, before);
                output.Store(4 + 4 * group.x, 7u);
            })");
    }
    ~Counting() {
        pipeline->Release();
        signature->Release();
        buffer->Release();
    }
    Counting(const Counting &) = delete;
    Counting &operator=(const Counting &) = delete;
    // SetComputeRootSignature (33), SetPipelineState (25), SetComputeRootUnorderedAccessView (45)
    void set(Work &work) const {
        work.record(33, signature);
        work.record(25, pipeline);
        work.record(45, 0u, output);
    }
    void check(std::uint32_t groups) const {
        const auto *const result = words(output);
        CHECK(result[0] == groups);
        for (std::uint32_t group = 0; group < groups; ++group)
            CHECK(result[1 + group] == 7);
    }
};
// CreateCommandSignature (41) as a title calls it: one dispatch argument of `type`
ID3D12CommandSignature *dispatch_signature(UINT type) {
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(type);
    const D3D12_COMMAND_SIGNATURE_DESC desc{12, 1, &argument, 0};
    ID3D12CommandSignature *made = nullptr;
    CHECK(title().call(device(), 41, &desc, static_cast<ID3D12RootSignature *>(nullptr),
                       &__uuidof(ID3D12CommandSignature),
                       reinterpret_cast<void **>(&made)) == S_OK);
    REQUIRE(made);
    return made;
}
// the host's buffer of an indirect dispatch's three counts
ID3D12Resource *counts(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *made = nullptr;
    REQUIRE(SUCCEEDED(device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&made))));
    void *mapped = nullptr;
    REQUIRE(SUCCEEDED(made->Map(0, nullptr, &mapped)));
    const std::uint32_t words[]{x, y, z};
    std::memcpy(mapped, words, sizeof words);
    made->Unmap(0, nullptr);
    return made;
}
} // namespace

TEST_CASE("DispatchX dispatches as Dispatch does, whatever its flags") {
    if (!device())
        return;
    const Counting counting;
    Work work;
    counting.set(work);
    work.record(78, 3u, 1u, 1u, 0x5u);
    work.run();
    CHECK(removed_reason() == S_OK);
    counting.check(3);
}

TEST_CASE("ExecuteIndirect dispatches the groups its arguments give, with the desktop's or the "
          "console's dispatch argument") {
    if (!device())
        return;
    for (const UINT type : {static_cast<UINT>(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH), 104u}) {
        CAPTURE(type);
        const Counting counting;
        auto *const signature = dispatch_signature(type);
        auto *const arguments = counts(4, 1, 1);
        Work work;
        counting.set(work);
        work.record(59, signature, 1u, arguments, std::uint64_t{0},
                    static_cast<ID3D12Resource *>(nullptr), std::uint64_t{0});
        work.run();
        CHECK(removed_reason() == S_OK);
        counting.check(4);
        arguments->Release();
        signature->Release();
    }
}
