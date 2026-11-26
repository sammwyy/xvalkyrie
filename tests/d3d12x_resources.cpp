#include "d3d12x_title.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

// CreateCommittedResourceX, the Series device's slot 76: CreateCommittedResource with the
// console's description of the resource, which is the desktop's D3D12_RESOURCE_DESC with
// fields of the console's own after its flags, to 0x40 bytes. AMD's upscaler makes its
// textures with it on Xbox Series S. The host's resource is the one the desktop's part
// describes, made as CreateCommittedResource (27) makes it.
namespace {
using namespace d3d12x_test;

// the console's description: the desktop's, then the console's own fields from 0x34, where
// the desktop's has padding
struct ConsoleDesc {
    D3D12_RESOURCE_DESC desktop{};
    std::uint64_t more = 0;
    ConsoleDesc(const D3D12_RESOURCE_DESC &desc, std::uint32_t options) : desktop(desc) {
        std::memcpy(reinterpret_cast<std::uint8_t *>(this) + 0x34, &options, sizeof options);
    }
};
static_assert(sizeof(ConsoleDesc) == 0x40);

D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format,
                                 D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}
// CreateCommittedResourceX (76) or CreateCommittedResource (27) in the default heap
HRESULT committed(std::uint32_t slot, const void *desc, D3D12_RESOURCE_STATES state,
                  ID3D12Resource *&made) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    made = nullptr;
    return title().call(device(), slot, &heap, D3D12_HEAP_FLAG_NONE, desc, state,
                        static_cast<const D3D12_CLEAR_VALUE *>(nullptr), &__uuidof(ID3D12Resource),
                        reinterpret_cast<void **>(&made));
}
bool same(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) {
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
           a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
           a.Layout == b.Layout && a.Flags == b.Flags;
}
} // namespace

TEST_CASE("CreateCommittedResourceX makes the texture the desktop's part of the console's "
          "description asks for, drawn to and read as any other") {
    if (!device())
        return;
    // as the upscaler asks: a target shaders write, in a tile mode of the console's (0x101),
    // and 3 in the console's first field
    auto desc = texture_desc(64, 32, DXGI_FORMAT_R8G8B8A8_UNORM,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    const ConsoleDesc console(desc, 3);
    ID3D12Resource *texture = nullptr;
    REQUIRE(committed(76, &console, D3D12_RESOURCE_STATE_RENDER_TARGET, texture) == S_OK);
    REQUIRE(texture);
    const auto made = texture->GetDesc();
    CHECK(made.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    CHECK(made.Width == 64);
    CHECK(made.Height == 32);
    CHECK(made.MipLevels == 1);
    CHECK(made.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(made.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
    CHECK(made.Flags ==
          (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));

    // drawn to: CreateRenderTargetView (20), ClearRenderTargetView (52)
    const Heap targets(1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_RENDER_TARGET_VIEW_DESC view{DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RTV_DIMENSION_TEXTURE2D};
    title().call<void>(device(), 20, texture, &view, targets.cpu);
    Work work;
    const float colour[4]{1, 0, 1, 1};
    work.record(52, targets.cpu, colour, 0u, static_cast<const D3D12_RECT *>(nullptr));
    // and read: GetCopyableFootprints (38), ResourceBarrier (26), CopyTextureRegion (16) into
    // a readback buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    title().call<void>(device(), 38, &made, 0u, 1u, UINT64{0}, &footprint,
                       static_cast<UINT *>(nullptr), static_cast<UINT64 *>(nullptr), &bytes);
    REQUIRE(bytes != ~0ull);
    std::uint64_t address = 0;
    auto *const readback = buffer(bytes, address, D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    work.record(26, 1u, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION target{
        readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    const D3D12_TEXTURE_COPY_LOCATION source{
        texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
    work.record(16, &target, 0u, 0u, 0u, &source, static_cast<const D3D12_BOX *>(nullptr));
    work.run();
    const auto *const texels = reinterpret_cast<const std::uint8_t *>(address);
    const auto *const last_row = texels + footprint.Footprint.RowPitch * 31;
    for (const auto *const texel : {texels, last_row + 63 * 4}) {
        CHECK(texel[0] == 0xff);
        CHECK(texel[1] == 0);
        CHECK(texel[2] == 0xff);
        CHECK(texel[3] == 0xff);
    }
    CHECK(removed_reason() == S_OK);
    readback->Release();
    texture->Release();
}

TEST_CASE("CreateCommittedResourceX makes what CreateCommittedResource makes of the same "
          "description, whatever the console's own fields hold") {
    if (!device())
        return;
    struct Case {
        D3D12_RESOURCE_DESC desc;
        D3D12_RESOURCE_STATES state;
    };
    auto buffer_desc =
        texture_desc(1000, 1, DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto mips = texture_desc(256, 128, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    mips.MipLevels = 0;
    const Case cases[]{
        // the upscaler's kinds: targets computed into, at the size it upscales to
        {texture_desc(1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
         D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {texture_desc(1280, 720, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        {mips, D3D12_RESOURCE_STATE_COPY_DEST},
        // a console colour format and a resource flag of the console's (0x100000), which the
        // host has its own for
        {texture_desc(
             16, 16, static_cast<DXGI_FORMAT>(116),
             static_cast<D3D12_RESOURCE_FLAGS>(0x100000 | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)),
         D3D12_RESOURCE_STATE_RENDER_TARGET},
        {buffer_desc, D3D12_RESOURCE_STATE_COMMON}};
    for (const auto &[desc, state] : cases) {
        INFO("dimension " << desc.Dimension << " format " << desc.Format);
        ID3D12Resource *desktop = nullptr;
        REQUIRE(committed(27, &desc, state, desktop) == S_OK);
        for (const std::uint32_t options : {0u, 3u, 0xffffffffu}) {
            INFO("options " << options);
            ConsoleDesc console(desc, options);
            console.more = options ? ~std::uint64_t{0} : 0;
            ID3D12Resource *made = nullptr;
            REQUIRE(committed(76, &console, state, made) == S_OK);
            REQUIRE(made);
            CHECK(same(made->GetDesc(), desktop->GetDesc()));
            made->Release();
        }
        desktop->Release();
    }
    CHECK(removed_reason() == S_OK);
}
