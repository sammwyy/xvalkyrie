#include "runtime/durango/d3d11x/layout.hpp"
#include "backends/graphics/d3d11/host.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/descriptors.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/durango/winrt/application.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/inflate.hpp"
#include "runtime/modules/placement.hpp"
#include "runtime/modules/watchdog.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <initializer_list>
#include <intrin.h>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

// Where each method is. Host slots are the desktop vtable indices of d3d11_1.h; the
// console positions come from how titles call them.
namespace xv::runtime::durango::d3d11x {
namespace d3d12x = modules::d3d12x;
using modules::placement_data, modules::readable_memory, modules::track_placement,
    modules::upload_placements;
void *unwrap(void *object);
namespace {
constexpr Fix in(std::uint8_t argument) { return {Fix::in, argument}; }
constexpr Fix in_array(std::uint8_t argument, std::uint8_t count) {
    return {Fix::in_array, argument, count};
}
constexpr Fix out(std::uint8_t argument, Kind kind) { return {Fix::out, argument, 0, kind}; }
constexpr Fix created(std::uint8_t argument, Kind kind) {
    return {Fix::out_checked, argument, 0, kind};
}
constexpr Fix out_array(std::uint8_t argument, std::uint8_t count, Kind kind) {
    return {Fix::out_array, argument, count, kind};
}
constexpr Fix out_iid(std::uint8_t argument, std::uint8_t iid) {
    return {Fix::out_iid, argument, iid};
}
Method forward(std::size_t host, std::initializer_list<Fix> fixes = {}) {
    Method method{Method::forward, static_cast<std::uint16_t>(host)};
    std::size_t index = 0;
    for (const auto &fix : fixes)
        method.fixes[index++] = fix;
    return method;
}
Method custom(auto *function) {
    return {Method::custom, 0, {}, reinterpret_cast<void *>(function)};
}

// ID3D11DeviceChild: the four desktop methods, then two console ones (private data for
// graphics interfaces, object name).
void device_child(Layout &result) {
    result.vtable[3] = forward(3, {out(1, Kind::device)}); // GetDevice
    for (std::size_t slot = 4; slot <= 6; ++slot)
        result.vtable[slot] = forward(slot);
    result.vtable[7] = result.vtable[8] = {Method::ignore};
}
// A device child whose own methods (host slots 7 to `last`) follow at 9
Layout child(const char *name, std::size_t last) {
    Layout result{name};
    device_child(result);
    for (std::size_t host = 7; host <= last; ++host)
        result.vtable[host + 2] = forward(host);
    return result;
}
// Console resources and views keep fields of their own that titles read when they write
// packets themselves (GPU addresses, descriptors); zeros here, in an object this large
constexpr std::size_t child_words = 32;
// ID3D11Resource: three desktop methods at 9, a console one at 12, then the type's own
void STDMETHODCALLTYPE get_descriptor(void *self, std::uint32_t *descriptor);
Layout resource(const char *name, std::size_t last) {
    Layout result{name, child_words};
    device_child(result);
    result.vtable[12] = custom(&get_descriptor);
    for (std::size_t host = 7; host <= 9; ++host)
        result.vtable[host + 2] = forward(host);
    for (std::size_t host = 10; host <= last; ++host)
        result.vtable[host + 3] = forward(host);
    return result;
}
Layout view() {
    auto result = child("view", 8);
    result.words = child_words;
    result.vtable[9] = forward(7, {out(1, Kind::resource)}); // GetResource
    return result;
}

// CreateTexture2D: the console description has flags and values the host rejects
HRESULT STDMETHODCALLTYPE create_texture2d(void *self, const D3D11_TEXTURE2D_DESC *description,
                                           const D3D11_SUBRESOURCE_DATA *initial,
                                           ID3D11Texture2D **texture) {
    if (!description)
        return E_INVALIDARG;
    auto host = *description;
    // generate mips, cube, indirect arguments, raw and structured buffers, clamp
    host.MiscFlags &= 0x1 | 0x4 | 0x10 | 0x20 | 0x40 | 0x80;
    if (host.SampleDesc.Count <= 1)
        host.SampleDesc = {1, 0};
    auto *const device = static_cast<ID3D11Device *>(unwrap(self));
    // The quality of a multisampled texture is the console's own count of fragments and
    // their layout: where the host has no such level, its first one stands for it
    if (host.SampleDesc.Count > 1 && host.SampleDesc.Quality < 0xfffffffe) {
        UINT levels = 0;
        device->CheckMultisampleQualityLevels(host.Format, host.SampleDesc.Count, &levels);
        if (host.SampleDesc.Quality >= levels)
            host.SampleDesc.Quality = 0;
    }
    // the console lets the CPU read a dynamic texture too, and write a default one on UMA
    if (host.Usage == D3D11_USAGE_DYNAMIC) {
        host.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    } else if (host.Usage == D3D11_USAGE_DEFAULT && host.CPUAccessFlags) {
        if ((host.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) &&
            host.BindFlags == D3D11_BIND_SHADER_RESOURCE && host.MipLevels == 1 &&
            host.ArraySize == 1 && host.SampleDesc.Count == 1) {
            host.Usage = D3D11_USAGE_DYNAMIC;
            host.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        } else {
            host.CPUAccessFlags = 0;
        }
    }
    host.Width = std::max(host.Width, 1u);
    host.Height = std::max(host.Height, 1u);
    if (host.BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) {
        const bool also_depth = (host.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
        if (host.Format == DXGI_FORMAT_D16_UNORM)
            host.Format = also_depth ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R16_UNORM;
        else if (host.Format == DXGI_FORMAT_D32_FLOAT)
            host.Format = also_depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT;
        else if (host.Format == DXGI_FORMAT_D24_UNORM_S8_UINT)
            host.Format = DXGI_FORMAT_R24G8_TYPELESS;
        else if (host.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
            host.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    }
    ID3D11Texture2D *created = nullptr;
    const auto result = device->CreateTexture2D(&host, initial, texture ? &created : nullptr);
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "[Runtime] CreateTexture2D %ux%u mips %u array %u format %u samples %u/%u "
                     "usage %u bind 0x%x cpu 0x%x misc 0x%x: 0x%08lx\n",
                     host.Width, host.Height, host.MipLevels, host.ArraySize,
                     static_cast<unsigned>(host.Format), description->SampleDesc.Count,
                     description->SampleDesc.Quality, static_cast<unsigned>(host.Usage),
                     host.BindFlags, host.CPUAccessFlags, description->MiscFlags,
                     static_cast<unsigned long>(result));
        std::fflush(stderr);
    } else if (texture) {
        *texture = static_cast<ID3D11Texture2D *>(wrap(created, Kind::texture2d));
    }
    // every format once: what the title draws with
    static std::atomic<std::uint64_t> seen[3]{};
    const auto format = static_cast<unsigned>(host.Format);
    if (format < 192 &&
        !(seen[format / 64].fetch_or(1ull << (format % 64)) & (1ull << (format % 64)))) {
        std::fprintf(stderr, "[Runtime] first 2D texture of format %u: %ux%u usage %u bind 0x%x\n",
                     format, host.Width, host.Height, static_cast<unsigned>(host.Usage),
                     host.BindFlags);
        std::fflush(stderr);
    }
    return result;
}

// the readable or the depth format of a depth texture's family, which the console lets
// views name loosely
DXGI_FORMAT depth_family_format(ID3D11Resource *resource, bool depth) {
    ID3D11Texture2D *texture = nullptr;
    if (!resource || FAILED(resource->QueryInterface(&texture)))
        return DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC description;
    texture->GetDesc(&description);
    texture->Release();
    switch (description.Format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return depth ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
HRESULT STDMETHODCALLTYPE create_render_target_view(void *self, void *resource,
                                                    const D3D11_RENDER_TARGET_VIEW_DESC *desc,
                                                    ID3D11RenderTargetView **view) {
    auto *const host = static_cast<ID3D11Resource *>(unwrap(resource));
    auto *const device = static_cast<ID3D11Device *>(unwrap(self));
    ID3D11RenderTargetView *created = nullptr;
    auto result = device->CreateRenderTargetView(host, desc, view ? &created : nullptr);
    // the console takes descriptions the host does not; the resource's own format and
    // first mip then
    if (result == E_INVALIDARG && desc)
        result = device->CreateRenderTargetView(host, nullptr, view ? &created : nullptr);
    if (FAILED(result)) {
        D3D11_TEXTURE2D_DESC texture{};
        if (ID3D11Texture2D *texture2d = nullptr;
            host && SUCCEEDED(host->QueryInterface(&texture2d))) {
            texture2d->GetDesc(&texture);
            texture2d->Release();
        }
        std::fprintf(stderr,
                     "[Runtime] CreateRenderTargetView format %u dimension %u of a %ux%u texture "
                     "of format %u bind 0x%x: 0x%08lx\n",
                     desc ? static_cast<unsigned>(desc->Format) : 0u,
                     desc ? static_cast<unsigned>(desc->ViewDimension) : 0u, texture.Width,
                     texture.Height, static_cast<unsigned>(texture.Format), texture.BindFlags,
                     static_cast<unsigned long>(result));
    } else if (view)
        *view = static_cast<ID3D11RenderTargetView *>(wrap(created, Kind::view));
    return result;
}
HRESULT STDMETHODCALLTYPE create_depth_stencil_view(void *self, void *resource,
                                                    const D3D11_DEPTH_STENCIL_VIEW_DESC *desc,
                                                    ID3D11DepthStencilView **view) {
    auto *const host = static_cast<ID3D11Resource *>(unwrap(resource));
    auto *const device = static_cast<ID3D11Device *>(unwrap(self));
    ID3D11DepthStencilView *created = nullptr;
    auto result = device->CreateDepthStencilView(host, desc, view ? &created : nullptr);
    if (result == E_INVALIDARG && desc) {
        auto retry = *desc;
        retry.Flags &= D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        if (const auto format = depth_family_format(host, true); format != DXGI_FORMAT_UNKNOWN)
            retry.Format = format;
        result = device->CreateDepthStencilView(host, &retry, view ? &created : nullptr);
        if (result == E_INVALIDARG)
            result = device->CreateDepthStencilView(host, nullptr, view ? &created : nullptr);
    }
    if (FAILED(result))
        std::fprintf(stderr, "[Runtime] CreateDepthStencilView format %u dimension %u: 0x%08lx\n",
                     desc ? static_cast<unsigned>(desc->Format) : 0u,
                     desc ? static_cast<unsigned>(desc->ViewDimension) : 0u,
                     static_cast<unsigned long>(result));
    else if (view)
        *view = static_cast<ID3D11DepthStencilView *>(wrap(created, Kind::view));
    return result;
}
// CreateShaderResourceView, which says what it was given when the host refuses it
// CreateBuffer (3): the console's own miscellaneous flags (above the desktop's 16 bits) mean
// nothing to the host, which turns the buffer down for them
HRESULT STDMETHODCALLTYPE create_buffer(void *self, const D3D11_BUFFER_DESC *desc,
                                        const D3D11_SUBRESOURCE_DATA *data, ID3D11Buffer **buffer) {
    if (!desc)
        return E_INVALIDARG;
    auto host = *desc;
    host.MiscFlags &= 0xffff;
    ID3D11Buffer *created = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))
                            ->CreateBuffer(&host, data, buffer ? &created : nullptr);
    if (FAILED(result))
        std::fprintf(stderr,
                     "[Runtime] CreateBuffer of %u bytes, usage %u bind 0x%x access 0x%x misc "
                     "0x%x stride %u: 0x%08lx\n",
                     desc->ByteWidth, desc->Usage, desc->BindFlags, desc->CPUAccessFlags,
                     desc->MiscFlags, desc->StructureByteStride,
                     static_cast<unsigned long>(result));
    else if (buffer)
        *buffer = static_cast<ID3D11Buffer *>(wrap(created, Kind::buffer));
    return result;
}
// The inputs a vertex shader reads that `elements` does not name, from the input signature
// of its DXBC. The console reads zeros for them and the host refuses the layout, so they
// are given a vertex stream of their own that nothing is ever bound to.
constexpr UINT unbound_stream = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1;
void add_missing_inputs(std::vector<D3D11_INPUT_ELEMENT_DESC> &elements, const void *code,
                        SIZE_T size) {
    const auto *const bytes = static_cast<const std::uint8_t *>(code);
    const auto word = [&](std::size_t at) {
        std::uint32_t value = 0;
        if (at + 4 <= size)
            std::memcpy(&value, bytes + at, 4);
        return value;
    };
    if (!code || size < 32 || std::memcmp(code, "DXBC", 4))
        return;
    for (std::uint32_t chunk = 0; chunk < word(28); ++chunk) {
        const std::size_t at = word(32 + 4 * chunk);
        const bool wide = at + 4 <= size && !std::memcmp(bytes + at, "ISG1", 4);
        if (at + 16 > size || (!wide && std::memcmp(bytes + at, "ISGN", 4)))
            continue;
        const std::size_t data = at + 8, stride = wide ? 32 : 24;
        for (std::uint32_t index = 0; index < word(data); ++index) {
            const auto entry = data + 8 + index * stride + (wide ? 4 : 0);
            const auto name = data + word(entry);
            if (entry + 24 > size || name >= size || word(entry + 8)) // a system value
                continue;
            const auto *const semantic = reinterpret_cast<const char *>(bytes + name);
            if (!std::memchr(semantic, 0, size - name))
                continue;
            const auto number = word(entry + 4);
            const bool named = std::any_of(elements.begin(), elements.end(), [&](const auto &e) {
                return e.SemanticIndex == number && !_stricmp(e.SemanticName, semantic);
            });
            if (named)
                continue;
            const auto type = word(entry + 12); // 1 unsigned, 2 signed, 3 float
            elements.push_back({semantic, number,
                                type == 1   ? DXGI_FORMAT_R32G32B32A32_UINT
                                : type == 2 ? DXGI_FORMAT_R32G32B32A32_SINT
                                            : DXGI_FORMAT_R32G32B32A32_FLOAT,
                                unbound_stream, 0, D3D11_INPUT_PER_VERTEX_DATA, 0});
        }
    }
}
// CreateInputLayout (11): the console's R10G10B10_SNORM_A2_UNORM (189), what packed normals
// are in, is no vertex format of the host's. Its bits are read unsigned there.
HRESULT STDMETHODCALLTYPE create_input_layout(void *self, const D3D11_INPUT_ELEMENT_DESC *elements,
                                              UINT count, const void *code, SIZE_T size,
                                              ID3D11InputLayout **layout) {
    std::vector<D3D11_INPUT_ELEMENT_DESC> host(elements, elements + (elements ? count : 0));
    for (auto &element : host)
        if (element.Format == static_cast<DXGI_FORMAT>(189))
            element.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    ID3D11InputLayout *created = nullptr;
    auto *const device = static_cast<ID3D11Device *>(unwrap(self));
    auto result =
        device->CreateInputLayout(host.data(), count, code, size, layout ? &created : nullptr);
    if (result == E_INVALIDARG) {
        add_missing_inputs(host, code, size);
        if (host.size() > count)
            result = device->CreateInputLayout(host.data(), static_cast<UINT>(host.size()), code,
                                               size, layout ? &created : nullptr);
    }
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "[Runtime] CreateInputLayout: 0x%08lx, signature of %zu bytes%s, elements:",
                     static_cast<unsigned long>(result), size,
                     code && size >= 4 && !std::memcmp(code, "DXBC", 4) ? "" : " (not DXBC)");
        for (const auto &element : host)
            std::fprintf(
                stderr, " %s%u/%u@%u+%u(%u,%u)", element.SemanticName, element.SemanticIndex,
                static_cast<unsigned>(element.Format), element.InputSlot, element.AlignedByteOffset,
                static_cast<unsigned>(element.InputSlotClass), element.InstanceDataStepRate);
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    } else if (layout) {
        *layout = static_cast<ID3D11InputLayout *>(wrap(created, Kind::child));
    }
    return result;
}
// CreateTexture3D (6), likewise
HRESULT STDMETHODCALLTYPE create_texture3d(void *self, const D3D11_TEXTURE3D_DESC *desc,
                                           const D3D11_SUBRESOURCE_DATA *data,
                                           ID3D11Texture3D **texture) {
    if (!desc)
        return E_INVALIDARG;
    auto host = *desc;
    host.MiscFlags &= 0xffff;
    ID3D11Texture3D *created = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))
                            ->CreateTexture3D(&host, data, texture ? &created : nullptr);
    if (FAILED(result))
        std::fprintf(stderr,
                     "[Runtime] CreateTexture3D %ux%ux%u mips %u format %u bind 0x%x: %lx\n",
                     desc->Width, desc->Height, desc->Depth, desc->MipLevels,
                     static_cast<unsigned>(desc->Format), desc->BindFlags,
                     static_cast<unsigned long>(result));
    else if (texture)
        *texture = static_cast<ID3D11Texture3D *>(wrap(created, Kind::texture3d));
    return result;
}
// CreateDeferredContext (27) and CreateDeferredContextX (68): the console's flags say how the
// title means to use the context's command lists (long lived, as bundles, with objects it
// keeps alive itself). The host takes no flags at all and needs none of that told.
HRESULT STDMETHODCALLTYPE create_deferred_context(void *self, UINT, void **context) {
    ID3D11DeviceContext *created = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))
                            ->CreateDeferredContext(0, context ? &created : nullptr);
    if (SUCCEEDED(result) && context)
        *context = wrap(created, Kind::context);
    return result;
}
HRESULT STDMETHODCALLTYPE create_shader_resource_view(void *self, void *resource,
                                                      const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
                                                      ID3D11ShaderResourceView **view) {
    auto *const host = static_cast<ID3D11Resource *>(unwrap(resource));
    auto *const device = static_cast<ID3D11Device *>(unwrap(self));
    ID3D11ShaderResourceView *created = nullptr;
    auto result = device->CreateShaderResourceView(host, desc, view ? &created : nullptr);
    if (result == E_INVALIDARG && desc) {
        auto retry = *desc;
        retry.Format = depth_family_format(host, false);
        if (retry.Format != DXGI_FORMAT_UNKNOWN && retry.Format != desc->Format)
            result = device->CreateShaderResourceView(host, &retry, view ? &created : nullptr);
    }
    if (FAILED(result)) {
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (host)
            host->GetType(&dimension);
        std::fprintf(
            stderr,
            "[Runtime] CreateShaderResourceView of a resource of dimension %u: format %u "
            "view dimension %u, words %08x %08x %08x %08x: 0x%08lx\n",
            static_cast<unsigned>(dimension), desc ? static_cast<unsigned>(desc->Format) : 0u,
            desc ? static_cast<unsigned>(desc->ViewDimension) : 0u,
            desc ? desc->Buffer.FirstElement : 0u, desc ? desc->Buffer.NumElements : 0u,
            desc ? desc->Texture2DArray.FirstArraySlice : 0u,
            desc ? desc->Texture2DArray.ArraySize : 0u, static_cast<unsigned long>(result));
        if (ID3D11Texture2D *texture = nullptr; host && SUCCEEDED(host->QueryInterface(&texture))) {
            D3D11_TEXTURE2D_DESC of;
            texture->GetDesc(&of);
            texture->Release();
            std::fprintf(stderr, "[Runtime] the texture: %ux%u format %u samples %u bind 0x%x\n",
                         of.Width, of.Height, static_cast<unsigned>(of.Format), of.SampleDesc.Count,
                         of.BindFlags);
        }
        std::fflush(stderr);
    } else if (view) {
        *view = static_cast<ID3D11ShaderResourceView *>(wrap(created, Kind::view));
    }
    return result;
}

// ID3D11DMAEngineContextX: copies, fills and fences of the console's
// copy engine, recorded on a deferred context of the host and run by Submit
ID3D11DeviceContext *dma_host(void *self) {
    return static_cast<ID3D11DeviceContext *>(unwrap(self));
}
std::atomic<UINT64> dma_fences{0};
UINT STDMETHODCALLTYPE dma_type(void *) { return D3D11_DEVICE_CONTEXT_DEFERRED; }
void STDMETHODCALLTYPE dma_copy_resource(void *self, void *destination, void *source, UINT) {
    dma_host(self)->CopyResource(static_cast<ID3D11Resource *>(unwrap(destination)),
                                 static_cast<ID3D11Resource *>(unwrap(source)));
}
void STDMETHODCALLTYPE dma_copy_region(void *self, void *destination, UINT subresource, UINT x,
                                       UINT y, UINT z, void *source, UINT source_subresource,
                                       const D3D11_BOX *box, UINT) {
    dma_host(self)->CopySubresourceRegion(
        static_cast<ID3D11Resource *>(unwrap(destination)), subresource, x, y, z,
        static_cast<ID3D11Resource *>(unwrap(source)), source_subresource, box);
}
UINT64 STDMETHODCALLTYPE dma_insert_fence(void *, UINT) { return ++dma_fences; }
HRESULT STDMETHODCALLTYPE dma_submit(void *self) {
    auto *const deferred = dma_host(self);
    ID3D11CommandList *list = nullptr;
    auto result = deferred->FinishCommandList(FALSE, &list);
    if (FAILED(result))
        return result;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *immediate = nullptr;
    deferred->GetDevice(&device);
    device->GetImmediateContext(&immediate);
    immediate->ExecuteCommandList(list, FALSE);
    immediate->Release();
    device->Release();
    list->Release();
    return S_OK;
}
// on memory the CPU sees, which is where the title points the copy engine at
void STDMETHODCALLTYPE dma_copy_memory(void *, void *destination, const void *source, UINT64 size) {
    std::memmove(destination, source, size);
}
void STDMETHODCALLTYPE dma_fill_memory(void *, void *destination, UINT64 size, UINT value) {
    auto *const words = static_cast<UINT *>(destination);
    for (UINT64 i = 0; i < size / 4; ++i)
        words[i] = value;
}
void STDMETHODCALLTYPE dma_timestamp(void *, UINT64 *destination) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    *destination = static_cast<UINT64>(now.QuadPart);
}
void STDMETHODCALLTYPE dma_write_value(void *, UINT *destination, UINT value) {
    *destination = value;
}
void STDMETHODCALLTYPE dma_write_value64(void *, UINT64 *destination, UINT64 value, UINT) {
    *destination = value;
}
// LZDecompressMemory (25): the copy engine inflates what a title packed with zlib, with the
// stream's header or without. Nothing says how much room there is: what comes out is
// what the title packed.
std::atomic<UINT> dma_error{0};
HRESULT STDMETHODCALLTYPE dma_inflate(void *, void *destination, const void *source, UINT size,
                                      UINT) {
    const auto *const bytes = static_cast<const std::uint8_t *>(source);
    std::vector<std::uint8_t> plain;
    bool done = modules::inflate_zlib({bytes, size}, plain);
    if (!done) {
        std::vector<std::uint8_t> headed{0x78, 0x01};
        headed.insert(headed.end(), bytes, bytes + size);
        plain.clear();
        done = modules::inflate_zlib(headed, plain);
    }
    if (!done) {
        if (static std::atomic<int> shown{0}; shown++ < 4) {
            std::fprintf(stderr,
                         "[Runtime] LZDecompressMemory of %u bytes that are not a zlib "
                         "stream:",
                         size);
            for (UINT i = 0; i < std::min(size, 16u); ++i)
                std::fprintf(stderr, " %02x", bytes[i]);
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
        dma_error = 1;
        return E_FAIL;
    }
    std::memcpy(destination, plain.data(), plain.size());
    dma_error = 0;
    return S_OK;
}
void STDMETHODCALLTYPE dma_error_code(void *, UINT *destination) { *destination = dma_error; }
// GetDescX (10) of a state. Only a sampler has it: the hardware's words for the sampler, of
// which the addressing, the filters, the comparison and the levels are filled in from the
// host's description. The other states have a host method at that place.
std::uint64_t STDMETHODCALLTYPE state_desc_x(void *self, std::uint64_t a, std::uint64_t b,
                                             std::uint64_t c) {
    auto *const host = static_cast<IUnknown *>(unwrap(self));
    ID3D11SamplerState *sampler = nullptr;
    if (FAILED(host->QueryInterface(&sampler))) {
        using Raw =
            std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t, std::uint64_t);
        return (*reinterpret_cast<Raw **>(host))[8](host, a, b, c);
    }
    D3D11_SAMPLER_DESC desc{};
    sampler->GetDesc(&desc);
    sampler->Release();
    const auto clamp = [](D3D11_TEXTURE_ADDRESS_MODE mode) -> std::uint32_t {
        return mode == D3D11_TEXTURE_ADDRESS_WRAP          ? 0
               : mode == D3D11_TEXTURE_ADDRESS_MIRROR      ? 1
               : mode == D3D11_TEXTURE_ADDRESS_MIRROR_ONCE ? 3
               : mode == D3D11_TEXTURE_ADDRESS_BORDER      ? 6
                                                           : 2;
    };
    const auto level = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 15.99f) * 256.0f) & 0xfff;
    };
    const bool compares = desc.Filter & 0x80, anisotropic = (desc.Filter & 0x55) == 0x55;
    const std::uint32_t min_linear = desc.Filter >> 4 & 1, mag_linear = desc.Filter >> 2 & 1,
                        mip_linear = desc.Filter & 1;
    std::uint32_t ratio = 0;
    for (UINT most = desc.MaxAnisotropy; anisotropic && most > 1 && ratio < 4; most >>= 1)
        ++ratio;
    const std::uint32_t words[4] = {
        clamp(desc.AddressU) | clamp(desc.AddressV) << 3 | clamp(desc.AddressW) << 6 | ratio << 9 |
            (compares ? (desc.ComparisonFunc - 1u) & 7 : 0u) << 12,
        level(desc.MinLOD) | level(desc.MaxLOD) << 12,
        (static_cast<std::uint32_t>(static_cast<int>(desc.MipLODBias * 256.0f)) & 0x3fff) |
            mag_linear << 20 | min_linear << 22 | (1u + mip_linear) << 26,
        0};
    std::memcpy(reinterpret_cast<void *>(a), words, sizeof words);
    return 0;
}
// CreateSamplerStateX (67): a sampler from the hardware's words, the other way round
HRESULT STDMETHODCALLTYPE create_sampler_state_x(void *self, const std::uint32_t *words,
                                                 void **state) {
    if (!words || !state)
        return E_INVALIDARG;
    const auto address = [](std::uint32_t clamp) {
        return clamp == 0   ? D3D11_TEXTURE_ADDRESS_WRAP
               : clamp == 1 ? D3D11_TEXTURE_ADDRESS_MIRROR
               : clamp == 3 ? D3D11_TEXTURE_ADDRESS_MIRROR_ONCE
               : clamp == 6 ? D3D11_TEXTURE_ADDRESS_BORDER
                            : D3D11_TEXTURE_ADDRESS_CLAMP;
    };
    const auto level = [](std::uint32_t value) {
        return value >= 0xff0 ? D3D11_FLOAT32_MAX : static_cast<float>(value) / 256.0f;
    };
    D3D11_SAMPLER_DESC desc{};
    desc.AddressU = address(words[0] & 7);
    desc.AddressV = address(words[0] >> 3 & 7);
    desc.AddressW = address(words[0] >> 6 & 7);
    const auto ratio = words[0] >> 9 & 7, compare = words[0] >> 12 & 7;
    desc.MaxAnisotropy = 1u << std::min(ratio, 4u);
    UINT filter = ratio ? 0x55
                        : (words[2] >> 22 & 1) << 4 | (words[2] >> 20 & 1) << 2 |
                              ((words[2] >> 26 & 3) == 2 ? 1u : 0u);
    if (compare)
        filter |= 0x80;
    desc.Filter = static_cast<D3D11_FILTER>(filter);
    desc.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(compare ? compare + 1 : 1);
    desc.MinLOD = level(words[1] & 0xfff);
    desc.MaxLOD = level(words[1] >> 12 & 0xfff);
    desc.MipLODBias = static_cast<float>(static_cast<int>(words[2] << 18) >> 18) / 256.0f;
    ID3D11SamplerState *host = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))->CreateSamplerState(&desc, &host);
    *state = SUCCEEDED(result) ? wrap(host, Kind::state) : nullptr;
    return result;
}
Layout dma() {
    Layout result{"DMA engine context"};
    device_child(result);
    result.vtable[9] = custom(&dma_type);
    result.vtable[10] = custom(&dma_copy_resource);
    result.vtable[11] = custom(&dma_copy_region);
    result.vtable[17] = custom(&dma_insert_fence);
    result.vtable[18] = {Method::ignore}; // InsertWaitOnFence: everything is done in order
    result.vtable[19] = custom(&dma_submit);
    result.vtable[20] = custom(&dma_error_code);
    result.vtable[21] = {Method::ignore}; // CopyLastErrorCodeToBuffer
    result.vtable[22] = custom(&dma_copy_memory);
    result.vtable[23] = custom(&dma_fill_memory);
    result.vtable[27] = custom(&dma_timestamp);
    result.vtable[29] = custom(&dma_write_value);
    result.vtable[25] = custom(&dma_inflate);
    result.vtable[30] = {Method::ignore}; // InsertWaitOnMemory
    result.vtable[31] = custom(&dma_write_value64);
    for (std::size_t slot : {32, 33, 34}) // PIX events
        result.vtable[slot] = {Method::ignore};
    return result;
}
// ID3D11ComputeContextX: compute work a title records apart from its drawing, on a deferred
// context of the host that is run where the title flushes it or puts a fence in. The
// methods after the device child's are, from 9: GetType, Dispatch, DispatchIndirect, the
// CSSet... ones, the placement ones, copies, clears, InsertFence (25), InsertWaitOnFence,
// Flush, ClearState.
template <class T> std::vector<T *> hosts_of(T *const *objects, UINT count) {
    std::vector<T *> hosts(count);
    for (UINT i = 0; objects && i < count; ++i)
        hosts[i] = static_cast<T *>(unwrap(objects[i]));
    return hosts;
}
void STDMETHODCALLTYPE compute_dispatch(void *self, UINT x, UINT y, UINT z) {
    dma_host(self)->Dispatch(x, y, z);
}
void STDMETHODCALLTYPE compute_dispatch_indirect(void *self, void *arguments, UINT offset) {
    dma_host(self)->DispatchIndirect(static_cast<ID3D11Buffer *>(unwrap(arguments)), offset);
}
void STDMETHODCALLTYPE compute_resources(void *self, UINT first, UINT count,
                                         ID3D11ShaderResourceView *const *views) {
    dma_host(self)->CSSetShaderResources(first, count, hosts_of(views, count).data());
}
void STDMETHODCALLTYPE compute_unordered(void *self, UINT first, UINT count,
                                         ID3D11UnorderedAccessView *const *views,
                                         const UINT *initial) {
    dma_host(self)->CSSetUnorderedAccessViews(first, count, hosts_of(views, count).data(), initial);
}
void STDMETHODCALLTYPE compute_shader(void *self, void *shader) {
    dma_host(self)->CSSetShader(static_cast<ID3D11ComputeShader *>(unwrap(shader)), nullptr, 0);
}
void STDMETHODCALLTYPE compute_samplers(void *self, UINT first, UINT count,
                                        ID3D11SamplerState *const *samplers) {
    dma_host(self)->CSSetSamplers(first, count, hosts_of(samplers, count).data());
}
void STDMETHODCALLTYPE compute_constants(void *self, UINT first, UINT count,
                                         ID3D11Buffer *const *buffers) {
    dma_host(self)->CSSetConstantBuffers(first, count, hosts_of(buffers, count).data());
}
void STDMETHODCALLTYPE compute_clear_uint(void *self, void *view, const UINT *values) {
    dma_host(self)->ClearUnorderedAccessViewUint(
        static_cast<ID3D11UnorderedAccessView *>(unwrap(view)), values);
}
void STDMETHODCALLTYPE compute_clear_float(void *self, void *view, const FLOAT *values) {
    dma_host(self)->ClearUnorderedAccessViewFloat(
        static_cast<ID3D11UnorderedAccessView *>(unwrap(view)), values);
}
UINT64 STDMETHODCALLTYPE compute_fence(void *self, UINT) {
    dma_submit(self);
    return ++dma_fences;
}
void STDMETHODCALLTYPE compute_flush(void *self) { dma_submit(self); }
void STDMETHODCALLTYPE compute_clear_state(void *self) { dma_host(self)->ClearState(); }
Layout compute() {
    Layout result{"compute context"};
    device_child(result);
    for (std::size_t slot = 9; slot < 60; ++slot)
        result.vtable[slot] = {Method::ignore}; // events, limits, cache flushes, waits
    result.vtable[10] = custom(&compute_dispatch);
    result.vtable[11] = custom(&compute_dispatch_indirect);
    result.vtable[12] = custom(&compute_resources);
    result.vtable[13] = custom(&compute_unordered);
    result.vtable[14] = custom(&compute_shader);
    result.vtable[15] = custom(&compute_samplers);
    result.vtable[16] = result.vtable[17] = custom(&compute_constants);
    for (std::size_t slot : {18, 19, 20}) // the placement ones: not seen yet
        result.vtable[slot] = {};
    result.vtable[21] = custom(&dma_copy_resource);
    result.vtable[22] = custom(&dma_copy_region);
    result.vtable[23] = custom(&compute_clear_uint);
    result.vtable[24] = custom(&compute_clear_float);
    result.vtable[25] = custom(&compute_fence);
    result.vtable[27] = custom(&compute_flush);
    result.vtable[28] = custom(&compute_clear_state);
    result.vtable[29] = custom(&dma_copy_memory);
    result.vtable[30] = custom(&dma_fill_memory);
    return result;
}
// CreateComputeContextX (73): the description sizes the console's rings
HRESULT STDMETHODCALLTYPE create_compute_context(void *self, const void *, void **context) {
    if (!context)
        return E_INVALIDARG;
    ID3D11DeviceContext *deferred = nullptr;
    const auto result =
        static_cast<ID3D11Device *>(unwrap(self))->CreateDeferredContext(0, &deferred);
    if (SUCCEEDED(result))
        *context = wrap(deferred, Kind::compute);
    return result;
}
HRESULT STDMETHODCALLTYPE create_dma_engine_context(void *self, const void *, void **context) {
    if (!context)
        return E_INVALIDARG;
    ID3D11DeviceContext *deferred = nullptr;
    const auto result =
        static_cast<ID3D11Device *>(unwrap(self))->CreateDeferredContext(0, &deferred);
    if (SUCCEEDED(result))
        *context = wrap(deferred, Kind::dma);
    return result;
}

// Placement resources live at memory the title allocated and writes itself: the host's
// copy starts from it and follows it (placement.cpp)
HRESULT STDMETHODCALLTYPE create_placement_buffer(void *self, const D3D11_BUFFER_DESC *desc,
                                                  void *address, ID3D11Buffer **buffer) {
    if (!desc)
        return E_INVALIDARG;
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_TRACE") && shown++ < 4) {
        std::fprintf(stderr,
                     "[Runtime] placement buffer of %u bytes, usage %u bind 0x%x misc 0x%x at %p, "
                     "from %s\n",
                     desc->ByteWidth, desc->Usage, desc->BindFlags, desc->MiscFlags, address,
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    auto host = *desc;
    host.MiscFlags &= 0x1 | 0x4 | 0x10 | 0x20 | 0x40 | 0x80;
    if (host.Usage == D3D11_USAGE_IMMUTABLE)
        host.Usage = D3D11_USAGE_DEFAULT;
    if (host.Usage == D3D11_USAGE_DEFAULT)
        host.CPUAccessFlags = 0;
    if (host.Usage == D3D11_USAGE_DYNAMIC)
        host.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const D3D11_SUBRESOURCE_DATA initial{address, 0, 0};
    ID3D11Buffer *created = nullptr;
    auto *const device = static_cast<ID3D11Device *>(unwrap(self));
    auto result = device->CreateBuffer(&host, address ? &initial : nullptr, &created);
    if (FAILED(result) && address)
        result = device->CreateBuffer(&host, nullptr, &created);
    if (FAILED(result))
        return result;
    track_placement(created, address);
    if (buffer) {
        *buffer = static_cast<ID3D11Buffer *>(wrap(created, Kind::buffer));
        describe_buffer(*buffer, created, address);
    } else
        created->Release();
    return result;
}
HRESULT STDMETHODCALLTYPE create_placement_texture1d(void *self, const D3D11_TEXTURE1D_DESC *desc,
                                                     UINT, UINT, void *,
                                                     ID3D11Texture1D **texture) {
    if (!desc)
        return E_INVALIDARG;
    auto host = *desc;
    host.MiscFlags &= 0x1 | 0x4 | 0x10 | 0x20 | 0x40 | 0x80;
    ID3D11Texture1D *created = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))
                            ->CreateTexture1D(&host, nullptr, texture ? &created : nullptr);
    if (SUCCEEDED(result) && texture)
        *texture = static_cast<ID3D11Texture1D *>(wrap(created, Kind::texture1d));
    return result;
}
HRESULT STDMETHODCALLTYPE create_placement_texture2d(void *self, const D3D11_TEXTURE2D_DESC *desc,
                                                     UINT tile_mode, UINT pitch,
                                                     const void *address,
                                                     ID3D11Texture2D **texture) {
    if (!desc)
        return E_INVALIDARG;
    auto host = *desc;
    if (host.Usage == D3D11_USAGE_IMMUTABLE)
        host.Usage = D3D11_USAGE_DEFAULT;
    std::vector<D3D11_SUBRESOURCE_DATA> data(std::size_t{std::max(host.MipLevels, 1u)} *
                                             std::max(host.ArraySize, 1u));
    const bool initial = host.MipLevels &&
                         placement_data(host, address, data.data(), static_cast<UINT>(data.size()));
    ID3D11Texture2D *created = nullptr;
    auto result = create_texture2d(self, &host, initial ? data.data() : nullptr, &created);
    if (FAILED(result) && initial)
        result = create_texture2d(self, &host, nullptr, &created);
    if (FAILED(result))
        return result;
    if (initial)
        track_placement(static_cast<ID3D11Texture2D *>(unwrap(created)), address, tile_mode, pitch);
    if (texture)
        *texture = created;
    else
        reinterpret_cast<IUnknown *>(created)->Release();
    return result;
}
// the addresses of a renderable texture are where the GPU writes it: nothing to read there
HRESULT STDMETHODCALLTYPE create_renderable_texture2d(void *self, const D3D11_TEXTURE2D_DESC *desc,
                                                      UINT, UINT, const void *,
                                                      ID3D11Texture2D **texture) {
    return create_texture2d(self, desc, nullptr, texture);
}
HRESULT STDMETHODCALLTYPE create_placement_texture3d(void *self, const D3D11_TEXTURE3D_DESC *desc,
                                                     UINT, UINT, void *,
                                                     ID3D11Texture3D **texture) {
    if (!desc)
        return E_INVALIDARG;
    auto host = *desc;
    host.MiscFlags &= 0x1 | 0x4 | 0x10 | 0x20 | 0x40 | 0x80;
    // what the title has at the address is not given to the host: nothing to be immutable with
    if (host.Usage == D3D11_USAGE_IMMUTABLE)
        host.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Texture3D *created = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))
                            ->CreateTexture3D(&host, nullptr, texture ? &created : nullptr);
    if (SUCCEEDED(result) && texture)
        *texture = static_cast<ID3D11Texture3D *>(wrap(created, Kind::texture3d));
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "[Runtime] CreatePlacementTexture3D %ux%ux%u mips %u format %u usage %u bind "
                     "0x%x misc 0x%x: 0x%08lx\n",
                     desc->Width, desc->Height, desc->Depth, desc->MipLevels,
                     static_cast<unsigned>(desc->Format), static_cast<unsigned>(desc->Usage),
                     desc->BindFlags, desc->MiscFlags, static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    return result;
}

// GetDescriptor (12) of a resource, and the device's Compose methods (74 to 78): an engine
// that keeps descriptors of its own asks the resource for its descriptor and the device for
// the descriptor of a view of it, and binds those with the fast-resource calls. The views
// are the host's, made once for each resource and description, and never let go of.
void STDMETHODCALLTYPE get_descriptor(void *self, std::uint32_t *descriptor) {
    if (descriptor)
        describe_resource(descriptor, self, static_cast<ID3D11Resource *>(unwrap(self)));
}
// D3D11X_RESOURCE_VIEW_DESC
struct ComposedView {
    UINT flags, unused;
    UINT64 address;
    UINT data_format, number_format;
    UINT swizzle[4];
    UINT memory_type, dimension;
    UINT words[8]; // of the view: a buffer's flags, first and count; a texture's levels, then
                   // its slices
};
// the format of a view of a typeless resource, by the console's number format
DXGI_FORMAT numbered_format(DXGI_FORMAT format, UINT number) {
    struct Family {
        DXGI_FORMAT typeless, unorm, snorm, uint, sint, real, srgb;
    };
    static constexpr DXGI_FORMAT none = DXGI_FORMAT_UNKNOWN;
    static constexpr Family families[] = {
        {DXGI_FORMAT_R32G32B32A32_TYPELESS, none, none, DXGI_FORMAT_R32G32B32A32_UINT,
         DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_FLOAT, none},
        {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
         DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT,
         DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_FLOAT, none},
        {DXGI_FORMAT_R32G32_TYPELESS, none, none, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT,
         DXGI_FORMAT_R32G32_FLOAT, none},
        {DXGI_FORMAT_R32G32B32_TYPELESS, none, none, DXGI_FORMAT_R32G32B32_UINT,
         DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32_FLOAT, none},
        {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, none,
         DXGI_FORMAT_R10G10B10A2_UINT, none, none, none},
        {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
         DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, none,
         DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
        {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, none, none, none, none,
         DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
        {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM,
         DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_FLOAT, none},
        {DXGI_FORMAT_R32_TYPELESS, none, none, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT,
         DXGI_FORMAT_R32_FLOAT, none},
        {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, none,
         DXGI_FORMAT_X24_TYPELESS_G8_UINT, none, none, none},
        {DXGI_FORMAT_R32G8X24_TYPELESS, none, none, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, none,
         DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, none},
        {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM,
         DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT, none, none},
        {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM,
         DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16_FLOAT, none},
        {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT,
         DXGI_FORMAT_R8_SINT, none, none},
        {DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, none, none, none, none,
         DXGI_FORMAT_BC1_UNORM_SRGB},
        {DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, none, none, none, none,
         DXGI_FORMAT_BC2_UNORM_SRGB},
        {DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, none, none, none, none,
         DXGI_FORMAT_BC3_UNORM_SRGB},
        {DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM, none, none, none,
         none},
        {DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM, none, none, none,
         none},
        {DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_SF16, none, none,
         DXGI_FORMAT_BC6H_UF16, none},
        {DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM, none, none, none, none,
         DXGI_FORMAT_BC7_UNORM_SRGB},
    };
    for (const auto &family : families) {
        if (family.typeless != format)
            continue;
        const auto picked = number == 1 || number == 6 ? family.snorm
                            : number == 4              ? family.uint
                            : number == 5              ? family.sint
                            : number == 7              ? family.real
                            : number == 9              ? family.srgb
                                                       : family.unorm;
        for (const auto one : {picked, family.unorm, family.real, family.uint})
            if (one != none)
                return one;
    }
    return format;
}
// A buffer has no format of its own: the view's, by the console's data and number formats.
// One with a structure is viewed by that.
DXGI_FORMAT buffer_view_format(ID3D11Resource *resource, UINT data, UINT number) {
    if (ID3D11Buffer *buffer = nullptr; SUCCEEDED(resource->QueryInterface(&buffer))) {
        D3D11_BUFFER_DESC desc;
        buffer->GetDesc(&desc);
        buffer->Release();
        if (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
            return DXGI_FORMAT_UNKNOWN;
    }
    static constexpr std::pair<UINT, DXGI_FORMAT> typeless[] = {
        {0x01, DXGI_FORMAT_R8_TYPELESS},           {0x02, DXGI_FORMAT_R16_TYPELESS},
        {0x03, DXGI_FORMAT_R8G8_TYPELESS},         {0x04, DXGI_FORMAT_R32_TYPELESS},
        {0x05, DXGI_FORMAT_R16G16_TYPELESS},       {0x06, DXGI_FORMAT_R11G11B10_FLOAT},
        {0x08, DXGI_FORMAT_R10G10B10A2_TYPELESS},  {0x09, DXGI_FORMAT_R10G10B10A2_TYPELESS},
        {0x0a, DXGI_FORMAT_R8G8B8A8_TYPELESS},     {0x0b, DXGI_FORMAT_R32G32_TYPELESS},
        {0x0c, DXGI_FORMAT_R16G16B16A16_TYPELESS}, {0x0d, DXGI_FORMAT_R32G32B32_TYPELESS},
        {0x0e, DXGI_FORMAT_R32G32B32A32_TYPELESS}};
    for (const auto &[console, host] : typeless)
        if (console == data)
            return numbered_format(host, number);
    return DXGI_FORMAT_UNKNOWN;
}
DXGI_FORMAT resource_format(ID3D11Resource *resource, D3D11_RESOURCE_DIMENSION &dimension) {
    resource->GetType(&dimension);
    if (ID3D11Texture2D *texture = nullptr; SUCCEEDED(resource->QueryInterface(&texture))) {
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        texture->Release();
        return desc.Format;
    }
    if (ID3D11Texture3D *texture = nullptr; SUCCEEDED(resource->QueryInterface(&texture))) {
        D3D11_TEXTURE3D_DESC desc;
        texture->GetDesc(&desc);
        texture->Release();
        return desc.Format;
    }
    if (ID3D11Texture1D *texture = nullptr; SUCCEEDED(resource->QueryInterface(&texture))) {
        D3D11_TEXTURE1D_DESC desc;
        texture->GetDesc(&desc);
        texture->Release();
        return desc.Format;
    }
    return DXGI_FORMAT_UNKNOWN;
}
std::mutex composed_mutex;
std::map<std::string, IUnknown *> composed; // by resource, kind and description
std::string composed_key(char kind, const void *resource, const void *desc, std::size_t size) {
    std::string key(1, kind);
    key.append(reinterpret_cast<const char *>(&resource), sizeof resource);
    if (desc)
        key.append(static_cast<const char *>(desc), size);
    return key;
}
void report_composed(const char *what, ID3D11Resource *resource, const ComposedView *desc,
                     HRESULT result) {
    static std::atomic<int> shown{0};
    if (shown++ >= 8)
        return;
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    const auto format = resource ? resource_format(resource, dimension) : DXGI_FORMAT_UNKNOWN;
    std::fprintf(stderr,
                 "[Runtime] Compose%s of a resource of dimension %d format %d: view dimension %u "
                 "flags 0x%x format %u/%u words %u %u %u %u: 0x%08lx\n",
                 what, static_cast<int>(dimension), static_cast<int>(format),
                 desc ? desc->dimension : 0u, desc ? desc->flags : 0u,
                 desc ? desc->data_format : 0u, desc ? desc->number_format : 0u,
                 desc ? desc->words[0] : 0u, desc ? desc->words[1] : 0u, desc ? desc->words[2] : 0u,
                 desc ? desc->words[3] : 0u, static_cast<unsigned long>(result));
    if (ID3D11Buffer *buffer = nullptr; resource && SUCCEEDED(resource->QueryInterface(&buffer))) {
        D3D11_BUFFER_DESC of;
        buffer->GetDesc(&of);
        buffer->Release();
        std::fprintf(stderr, "[Runtime] the buffer: %u bytes, bind 0x%x misc 0x%x stride %u\n",
                     of.ByteWidth, of.BindFlags, of.MiscFlags, of.StructureByteStride);
    }
    std::fflush(stderr);
}
void STDMETHODCALLTYPE compose_shader_resource_view(void *self, const std::uint32_t *source,
                                                    const ComposedView *desc,
                                                    std::uint32_t *descriptor) {
    if (!descriptor)
        return;
    std::memset(descriptor, 0, 32);
    auto *const resource = source ? described_resource(source) : nullptr;
    if (!resource)
        return report_composed("ShaderResourceView", nullptr, desc, E_INVALIDARG);
    const auto key = composed_key('s', resource, desc, sizeof *desc);
    const std::lock_guard lock(composed_mutex);
    auto &view = composed[key];
    if (!view) {
        D3D11_RESOURCE_DIMENSION dimension;
        const auto format = resource_format(resource, dimension);
        D3D11_SHADER_RESOURCE_VIEW_DESC host{};
        if (desc) {
            const auto levels = desc->words[1] ? desc->words[1] : ~0u;
            host.Format = numbered_format(format, desc->number_format);
            host.ViewDimension = static_cast<D3D11_SRV_DIMENSION>(desc->dimension);
            switch (desc->dimension) {
            case 1:
                host.Format = buffer_view_format(resource, desc->data_format, desc->number_format);
                host.Buffer.FirstElement = desc->words[1];
                host.Buffer.NumElements = desc->words[2];
                if (desc->words[0] & 1) {
                    host.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
                    host.Format = DXGI_FORMAT_R32_TYPELESS;
                    host.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
                }
                break;
            case 6:
                break;
            case 7:
                host.Texture2DMSArray = {desc->words[0], desc->words[1]};
                break;
            case 3:
            case 5:
            case 10:
                host.Texture2DArray = {desc->words[0], levels, desc->words[2], desc->words[3]};
                break;
            default:
                host.Texture2D = {desc->words[0], levels};
            }
        }
        auto *const device = static_cast<ID3D11Device *>(unwrap(self));
        ID3D11ShaderResourceView *created = nullptr;
        auto result = device->CreateShaderResourceView(resource, desc ? &host : nullptr, &created);
        if (FAILED(result) && desc)
            result = device->CreateShaderResourceView(resource, nullptr, &created);
        if (FAILED(result))
            return report_composed("ShaderResourceView", resource, desc, result);
        view = created;
    }
    describe_composed(descriptor, static_cast<ID3D11ShaderResourceView *>(view));
}
void STDMETHODCALLTYPE compose_unordered_access_view(void *self, const std::uint32_t *source,
                                                     const ComposedView *desc,
                                                     std::uint32_t *descriptor) {
    if (!descriptor)
        return;
    std::memset(descriptor, 0, 32);
    auto *const resource = source ? described_resource(source) : nullptr;
    if (!resource)
        return report_composed("UnorderedAccessView", nullptr, desc, E_INVALIDARG);
    const auto key = composed_key('u', resource, desc, sizeof *desc);
    const std::lock_guard lock(composed_mutex);
    auto &view = composed[key];
    if (!view) {
        D3D11_RESOURCE_DIMENSION dimension;
        const auto format = resource_format(resource, dimension);
        D3D11_UNORDERED_ACCESS_VIEW_DESC host{};
        if (desc) {
            host.Format = numbered_format(format, desc->number_format);
            host.ViewDimension = static_cast<D3D11_UAV_DIMENSION>(desc->dimension);
            switch (desc->dimension) {
            case 1:
                host.Format = buffer_view_format(resource, desc->data_format, desc->number_format);
                host.Buffer.FirstElement = desc->words[1];
                host.Buffer.NumElements = desc->words[2];
                if (desc->words[0] & 1) {
                    host.Format = DXGI_FORMAT_R32_TYPELESS;
                    host.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
                }
                break;
            case 3:
            case 5:
            case 8:
                host.Texture2DArray = {desc->words[0], desc->words[2], desc->words[3]};
                break;
            default:
                host.Texture2D.MipSlice = desc->words[0];
            }
        }
        auto *const device = static_cast<ID3D11Device *>(unwrap(self));
        ID3D11UnorderedAccessView *created = nullptr;
        auto result = device->CreateUnorderedAccessView(resource, desc ? &host : nullptr, &created);
        if (FAILED(result) && desc)
            result = device->CreateUnorderedAccessView(resource, nullptr, &created);
        if (FAILED(result))
            return report_composed("UnorderedAccessView", resource, desc, result);
        view = created;
    }
    describe_composed(descriptor, static_cast<ID3D11UnorderedAccessView *>(view));
}
// a constant buffer's or a vertex buffer's: the buffer's address, moved on by the view's
// offset, with its stride above it, and the bytes left
void STDMETHODCALLTYPE compose_buffer_view(void *, const std::uint32_t *source,
                                           const ComposedView *desc, std::uint32_t *descriptor) {
    if (!descriptor)
        return;
    std::memset(descriptor, 0, 16);
    if (!source || !described_resource(source))
        return report_composed("BufferView", nullptr, desc, E_INVALIDARG);
    std::uint64_t address = 0;
    std::memcpy(&address, source + 2, 8);
    auto size = source[4];
    if (desc && desc->dimension == 1) {
        const auto offset = std::min(desc->words[1], size);
        address += offset;
        size -= offset;
        address |= std::uint64_t{desc->words[2] & 0x3fff} << 48;
    }
    std::memcpy(descriptor, &address, 8);
    descriptor[2] = size;
}
// D3D11X_SAMPLER_STATE_DESC
struct ComposedSampler {
    UINT flags, magnify, minify, mip, z, mode;
    D3D11_TEXTURE_ADDRESS_MODE u, v, w;
    FLOAT least, most, bias, second_bias;
    UINT anisotropy;
    FLOAT anisotropy_bias;
    UINT threshold;
    D3D11_COMPARISON_FUNC comparison;
};
void STDMETHODCALLTYPE compose_sampler_state(void *self, const ComposedSampler *desc,
                                             std::uint32_t *descriptor) {
    if (!descriptor)
        return;
    std::memset(descriptor, 0, 16);
    if (!desc)
        return;
    const auto key = composed_key('a', nullptr, desc, sizeof *desc);
    const std::lock_guard lock(composed_mutex);
    auto &sampler = composed[key];
    if (!sampler) {
        // filters: 0 point, 1 linear, from 2 anisotropic; a mip filter of 0 is none
        D3D11_SAMPLER_DESC host{};
        const bool anisotropic = desc->magnify >= 2 || desc->minify >= 2;
        const bool compares =
            desc->comparison > D3D11_COMPARISON_NEVER && desc->comparison < D3D11_COMPARISON_ALWAYS;
        UINT filter = anisotropic ? 0x55
                                  : (desc->minify & 1) << 4 | (desc->magnify & 1) << 2 |
                                        (desc->mip == 2 ? 1u : 0u);
        if (compares)
            filter |= 0x80;
        host.Filter = static_cast<D3D11_FILTER>(filter);
        const auto address = [](D3D11_TEXTURE_ADDRESS_MODE mode) {
            return mode >= D3D11_TEXTURE_ADDRESS_WRAP && mode <= D3D11_TEXTURE_ADDRESS_MIRROR_ONCE
                       ? mode
                       : D3D11_TEXTURE_ADDRESS_CLAMP;
        };
        host.AddressU = address(desc->u);
        host.AddressV = address(desc->v);
        host.AddressW = address(desc->w);
        host.MinLOD = desc->least;
        host.MaxLOD = desc->mip ? desc->most : 0.0f;
        host.MipLODBias = desc->bias;
        host.MaxAnisotropy = std::clamp(desc->anisotropy, 1u, 16u);
        host.ComparisonFunc = compares ? desc->comparison : D3D11_COMPARISON_NEVER;
        ID3D11SamplerState *created = nullptr;
        const auto result =
            static_cast<ID3D11Device *>(unwrap(self))->CreateSamplerState(&host, &created);
        if (FAILED(result)) {
            std::fprintf(stderr,
                         "[Runtime] ComposeSamplerState filters %u %u %u mode %u address %d %d %d "
                         "comparison %d: 0x%08lx\n",
                         desc->magnify, desc->minify, desc->mip, desc->mode,
                         static_cast<int>(desc->u), static_cast<int>(desc->v),
                         static_cast<int>(desc->w), static_cast<int>(desc->comparison),
                         static_cast<unsigned long>(result));
            std::fflush(stderr);
            return;
        }
        sampler = created;
    }
    describe_composed(descriptor, static_cast<ID3D11SamplerState *>(sampler));
}
void STDMETHODCALLTYPE get_timestamps(void *, UINT64 *gpu, UINT64 *cpu) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (gpu)
        *gpu = static_cast<UINT64>(now.QuadPart);
    if (cpu)
        *cpu = __rdtsc();
}
void STDMETHODCALLTYPE get_driver_statistics(void *, UINT size, void *statistics) {
    if (statistics)
        std::memset(statistics, 0, size);
}

Layout device() {
    Layout result{"device"};
    const auto set = [&](std::size_t slot, std::initializer_list<Fix> fixes = {}) {
        result.vtable[slot] = forward(slot, fixes);
    };
    for (std::size_t slot = 3; slot <= 49; ++slot)
        set(slot);
    result.vtable[3] = custom(&create_buffer);
    set(4, {created(3, Kind::texture1d)});
    result.vtable[5] = custom(&create_texture2d);
    result.vtable[6] = custom(&create_texture3d);
    for (std::size_t slot = 7; slot <= 10; ++slot) // views of a resource
        set(slot, {in(1), created(3, Kind::view)});
    result.vtable[7] = custom(&create_shader_resource_view);
    result.vtable[9] = custom(&create_render_target_view);
    result.vtable[10] = custom(&create_depth_stencil_view);
    result.vtable[11] = custom(&create_input_layout);
    for (std::size_t slot : {12, 13, 15, 16, 17, 18}) // shaders
        set(slot, {in(3), created(4, Kind::child)});
    set(14, {in(8), created(9, Kind::child)});      // geometry shader with stream output
    set(19, {created(1, Kind::state)});             // class linkage
    for (std::size_t slot = 20; slot <= 26; ++slot) // states, queries, counters
        set(slot, {created(2, Kind::state)});
    result.vtable[27] = custom(&create_deferred_context);
    set(40, {out(1, Kind::context)});
    set(43, {out(1, Kind::context)});
    set(44, {created(2, Kind::context)});
    set(45, {created(2, Kind::state)});
    set(46, {created(2, Kind::state)});
    for (std::size_t slot = 50; slot <= 53; ++slot)
        set(slot);
    set(50, {out(1, Kind::context)}); // GetImmediateContext2
    set(51, {created(2, Kind::context)});
    // shared resources and context states are not something a console title has
    for (std::size_t slot : {28, 47, 48, 49})
        result.vtable[slot] = {};
    // ID3D11DeviceX (ABI 6.2.11064 on):
    // SetPrivateDataInterfaceGraphics comes after SetPrivateDataInterface, the desktop
    // methods follow one slot later, then the console's own.
    for (std::size_t slot = 54; slot > 37; --slot)
        result.vtable[slot] = result.vtable[slot - 1];
    result.vtable[37] = {Method::ignore};
    result.vtable[55] = forward(40, {out(1, Kind::context)}); // GetImmediateContextX
    for (std::size_t slot : {58, 60, 61, 69, 80, 81, 82, 83, 84, 85})
        result.vtable[slot] = {Method::ignore}; // hints, fences done, debug, priority
    result.vtable[59] = custom(&create_dma_engine_context);
    result.vtable[62] = custom(&create_placement_buffer);
    result.vtable[63] = custom(&create_placement_texture1d);
    result.vtable[64] = custom(&create_placement_texture2d);
    result.vtable[65] = custom(&create_placement_texture3d);
    result.vtable[66] = custom(&get_timestamps);
    result.vtable[67] = custom(&create_sampler_state_x);
    result.vtable[68] = custom(&create_deferred_context);
    result.vtable[70] = forward(21, {created(2, Kind::state)}); // CreateDepthStencilStateX
    result.vtable[71] = custom(&create_renderable_texture2d);   // ...RenderableTexture2D
    result.vtable[72] = custom(&get_driver_statistics);
    result.vtable[73] = custom(&create_compute_context);
    result.vtable[74] = custom(&compose_shader_resource_view);
    result.vtable[75] = custom(&compose_unordered_access_view);
    result.vtable[76] = result.vtable[77] = custom(&compose_buffer_view);
    result.vtable[78] = custom(&compose_sampler_state);
    return result;
}

// what follows each frame the title presents, whichever way it does
void presented() {
    modules::frame_watchdog();
    signal_frame_notifications();
    if (auto *const window = winrt::current_title_window()) {
        window->show();
        static std::atomic<int> frames{0};
        if (++frames == 120) { // once it has had time to appear
            std::fprintf(stderr, "[Runtime] title surface %s\n", window->describe().c_str());
            std::fflush(stderr);
        }
    }
}
void average(ID3D11Texture2D *texture, const char *what) {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    texture->GetDevice(&device);
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, nullptr);
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            d3d12x::report_picture(static_cast<const std::uint8_t *>(mapped.pData), mapped.RowPitch,
                                   desc.Width, desc.Height, desc.Format, what);
            context->Unmap(staging, 0);
        }
        staging->Release();
    }
    context->Release();
    device->Release();
}
bool probing() {
    static const bool enabled = std::getenv("XV_D3D_PROBE") != nullptr;
    return enabled;
}
void probe(IDXGISwapChain *swap_chain) {
    static std::atomic<int> frames{0};
    // XV_D3D_PROBE=<frame>, 300 when it is not a number
    static const int at = [] {
        const auto *const text = std::getenv("XV_D3D_PROBE");
        const int frame = text ? std::atoi(text) : 0;
        return frame > 1 ? frame : 300;
    }();
    if (!probing() || ++frames != at)
        return;
    ID3D11Texture2D *buffer = nullptr;
    if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) {
        average(buffer, "back buffer");
        buffer->Release();
        return;
    }
    // a Direct3D 12 swap chain: the buffer about to be presented, read back on the host
    IDXGISwapChain3 *third = nullptr;
    if (FAILED(swap_chain->QueryInterface(IID_PPV_ARGS(&third))))
        return;
    ID3D12Resource *resource = nullptr;
    if (SUCCEEDED(third->GetBuffer(third->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&resource)))) {
        std::vector<std::uint8_t> data;
        UINT pitch = 0, width = 0, height = 0;
        DXGI_FORMAT format{};
        if (d3d12x::read_back(resource, data, pitch, width, height, format))
            d3d12x::report_picture(
                data.data(), pitch, width, height, format,
                ("back buffer (" + std::to_string(d3d12x::draws_into_target(resource)) + " draws)")
                    .c_str());
        int index = 0;
        for (auto *const target : d3d12x::bound_render_targets())
            if (d3d12x::read_back(target, data, pitch, width, height, format,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET))
                d3d12x::report_picture(data.data(), pitch, width, height, format,
                                       ("render target " + std::to_string(index++) + " (" +
                                        std::to_string(d3d12x::draws_into_target(target)) +
                                        " draws)")
                                           .c_str());
        d3d12x::report_failure();
        resource->Release();
    }
    third->Release();
}
// What a console's Present never says: the desktop's statuses (occluded, mode changed,
// present required...) are plain success. A hidden window (covered, minimized, the display
// off) does not wait for the display either, so that wait is made here; titles that see
// DXGI_STATUS_OCCLUDED stop drawing.
HRESULT console_present_result(HRESULT result, UINT interval) {
    if (result == DXGI_STATUS_OCCLUDED)
        Sleep(16 * std::max(interval, 1u));
    if (FAILED(result)) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            std::fprintf(stderr, "[Runtime] Present failed on the host: 0x%08lx\n",
                         static_cast<unsigned long>(result));
            d3d12x::report_failure();
        }
        return result;
    }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE present(void *self, UINT interval, UINT flags) {
    auto *const host = static_cast<IDXGISwapChain *>(unwrap(self));
    probe(host);
    graphics::d3d11::frame_presented(host);
    const auto result = host->Present(interval, flags);
    presented();
    return console_present_result(result, interval);
}
// Present1: the console takes no parameters to mean the whole frame, the host crashes
HRESULT STDMETHODCALLTYPE present1(void *self, UINT interval, UINT flags,
                                   const DXGI_PRESENT_PARAMETERS *parameters) {
    auto *const host = static_cast<IDXGISwapChain1 *>(unwrap(self));
    probe(host);
    graphics::d3d11::frame_presented(host);
    const DXGI_PRESENT_PARAMETERS whole{};
    const auto result = host->Present1(interval, flags, parameters ? parameters : &whole);
    presented();
    return console_present_result(result, interval);
}

} // namespace

long present_swap_chain(void *swap_chain, unsigned interval) {
    return present(swap_chain, interval, 0);
}

namespace {
// GetBuffer: the console hands out every back buffer by index; a Direct3D 11 flip swap chain
// of the host only buffer 0, which is always the one being drawn. Direct3D 12's has one
// resource per index, as the console's.
HRESULT STDMETHODCALLTYPE get_buffer(void *self, UINT index, REFIID iid, void **surface) {
    const bool d3d12 = iid == __uuidof(ID3D12Resource);
    const auto result =
        static_cast<IDXGISwapChain *>(unwrap(self))->GetBuffer(d3d12 ? index : 0, iid, surface);
    if (SUCCEEDED(result) && surface && d3d12)
        d3d12x::console_d3d12_resource(*surface);
    if (SUCCEEDED(result) && surface &&
        (iid == __uuidof(ID3D11Texture2D) || iid == __uuidof(ID3D11Resource)))
        *surface = wrap(*surface, Kind::texture2d);
    return result;
}

Layout swap_chain() {
    Layout result{"swap chain"};
    for (std::size_t slot = 3; slot <= 28; ++slot)
        result.vtable[slot] = forward(slot);
    result.vtable[7] = forward(7, {out_iid(2, 1)}); // GetDevice
    result.vtable[8] = custom(&present);
    result.vtable[22] = custom(&present1);
    result.vtable[9] = custom(&get_buffer);
    return result;
}

// Methods with a float argument or result, which the generic path does not carry
void STDMETHODCALLTYPE clear_depth_stencil(void *self, void *view, UINT flags, FLOAT depth,
                                           UINT8 stencil) {
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->ClearDepthStencilView(static_cast<ID3D11DepthStencilView *>(unwrap(view)), flags, depth,
                                stencil);
}
void STDMETHODCALLTYPE set_min_lod(void *self, void *resource, FLOAT lod) {
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->SetResourceMinLOD(static_cast<ID3D11Resource *>(unwrap(resource)), lod);
}
FLOAT STDMETHODCALLTYPE get_min_lod(void *self, void *resource) {
    return static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->GetResourceMinLOD(static_cast<ID3D11Resource *>(unwrap(resource)));
}

// XXSetShaderResources through the in-object table: one view per call, passed as
// (address of the view pointer, slot, a hardware packet header that means nothing here).
// A pixel shader resource stays bound on the console when its resource becomes a render
// target; the host unbinds it. What the title bound is kept, and put back before each draw
// where the host dropped it.
constexpr UINT kept_views = 16;
struct KeptViews {
    std::array<ID3D11ShaderResourceView *, kept_views> views{};
};
std::mutex kept_mutex;
std::map<void *, KeptViews> kept;
void keep_pixel_view(void *self, UINT slot, ID3D11ShaderResourceView *view) {
    if (slot >= kept_views)
        return;
    const std::lock_guard lock(kept_mutex);
    auto &held = kept[self].views[slot];
    if (view)
        view->AddRef();
    if (held)
        held->Release();
    held = view;
}
void restore_pixel_views(void *self) {
    std::array<ID3D11ShaderResourceView *, kept_views> wanted;
    {
        const std::lock_guard lock(kept_mutex);
        const auto found = kept.find(self);
        if (found == kept.end())
            return;
        wanted = found->second.views;
    }
    auto *const host = static_cast<ID3D11DeviceContext *>(unwrap(self));
    std::array<ID3D11ShaderResourceView *, kept_views> bound{};
    host->PSGetShaderResources(0, kept_views, bound.data());
    for (UINT slot = 0; slot < kept_views; ++slot) {
        if (wanted[slot] && !bound[slot])
            host->PSSetShaderResources(slot, 1, &wanted[slot]);
        if (bound[slot])
            bound[slot]->Release();
    }
}
template <void (STDMETHODCALLTYPE ID3D11DeviceContext::*Set)(UINT, UINT,
                                                             ID3D11ShaderResourceView *const *)>
void STDMETHODCALLTYPE set_shader_resource(void *self, void **view, UINT slot, UINT) {
    auto *const host = static_cast<ID3D11ShaderResourceView *>(view ? unwrap(*view) : nullptr);
    if constexpr (Set == &ID3D11DeviceContext::PSSetShaderResources)
        keep_pixel_view(self, slot, host);
    (static_cast<ID3D11DeviceContext *>(unwrap(self))->*Set)(slot, 1, &host);
}

// IASetIndexBuffer through the in-object table takes the index size first, as the
// hardware names it: 0 for 16-bit indices, 1 for 32-bit ones.
void STDMETHODCALLTYPE set_index_buffer(void *self, UINT hardware_format, void *buffer,
                                        UINT offset) {
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->IASetIndexBuffer(static_cast<ID3D11Buffer *>(unwrap(buffer)),
                           hardware_format ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, offset);
}

// State a title sets without calling anything: the console headers write the input layout,
// the vertex and pixel shaders and the primitive topology into the context object and mark
// them dirty; the console applies them at the next draw. So do we.
namespace state {
constexpr std::size_t dirty = 0x168, topology = 0x170, input_layout = 0x178, vertex_shader = 0x180,
                      pixel_shader = 0x188;
// What the headers store for each D3D11_PRIMITIVE_TOPOLOGY, indexed by it: a hardware
// register value. Entries equal to the first one are topologies that do not exist.
constexpr std::uint64_t topologies[] = {
    0x1ffc0009e00,  0x3ffc0009e00,  0x5ffc0009e00,  0x7ffc0009e00, 0x9ffc0009e00,
    0xdffc0009e00,  0xbffc0009e00,  0x1ffc0009e00,  0x1ffc0009e00, 0x1ffc0009e00,
    0x157fc0009e00, 0x177fc0009e00, 0x197fc0009e00, 0x1b7fc0009e00};
template <class T> T read(void *object, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<char *>(object) + offset, sizeof value);
    return value;
}
} // namespace state
constexpr std::size_t command_write = 0x50 / 8, command_end = 0x58 / 8, scratch_bytes = 1 << 20;
std::mutex scratch_mutex;
std::map<void *, std::unique_ptr<std::byte[]>> scratch;
void apply_context_state(void *self) {
    upload_placements(static_cast<ID3D11DeviceContext *>(unwrap(self)));
    const std::byte *stream = nullptr;
    {
        const std::lock_guard lock(scratch_mutex);
        if (const auto found = scratch.find(self); found != scratch.end())
            stream = found->second.get();
    }
    if (stream)
        apply_descriptors(static_cast<ID3D11DeviceContext *>(unwrap(self)), stream,
                          static_cast<std::byte **>(self)[command_write]);
    restore_pixel_views(self);
    if (!state::read<std::uint32_t>(self, state::dirty))
        return;
    const std::uint32_t clean = 0;
    std::memcpy(static_cast<char *>(self) + state::dirty, &clean, sizeof clean);
    auto *const host = static_cast<ID3D11DeviceContext *>(unwrap(self));
    host->IASetInputLayout(
        static_cast<ID3D11InputLayout *>(unwrap(state::read<void *>(self, state::input_layout))));
    host->VSSetShader(
        static_cast<ID3D11VertexShader *>(unwrap(state::read<void *>(self, state::vertex_shader))),
        nullptr, 0);
    host->PSSetShader(
        static_cast<ID3D11PixelShader *>(unwrap(state::read<void *>(self, state::pixel_shader))),
        nullptr, 0);
    const auto value = state::read<std::uint64_t>(self, state::topology);
    for (std::size_t topology = 1; topology < std::size(state::topologies); ++topology)
        if (state::topologies[topology] == value && value != state::topologies[0]) {
            host->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(topology));
            break;
        }
}
// DrawIndexed through the in-object table: (index count << 32 | first index, base vertex)
void STDMETHODCALLTYPE draw_indexed(void *self, UINT64 count_and_start, INT base_vertex) {
    apply_context_state(self);
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->DrawIndexed(static_cast<UINT>(count_and_start >> 32), static_cast<UINT>(count_and_start),
                      base_vertex);
}

// The fast and placement setters bind one thing at one slot; a placement address inside
// the resource is not something the host can take, so the resource is bound whole
template <class T, void (STDMETHODCALLTYPE ID3D11DeviceContext::*Set)(UINT, UINT, T *const *)>
void STDMETHODCALLTYPE set_one(void *self, UINT slot, void *object) {
    auto *const host = static_cast<T *>(unwrap(object));
    if constexpr (std::is_same_v<T, ID3D11ShaderResourceView>)
        if constexpr (Set == &ID3D11DeviceContext::PSSetShaderResources)
            keep_pixel_view(self, slot, host);
    (static_cast<ID3D11DeviceContext *>(unwrap(self))->*Set)(slot, 1, &host);
}
template <class T, void (STDMETHODCALLTYPE ID3D11DeviceContext::*Set)(UINT, UINT, T *const *)>
void STDMETHODCALLTYPE set_placed(void *self, UINT slot, void *object, void *) {
    set_one<T, Set>(self, slot, object);
}
void STDMETHODCALLTYPE set_fast_vertex_buffer(void *self, UINT slot, void *buffer, UINT stride) {
    auto *const host = static_cast<ID3D11Buffer *>(unwrap(buffer));
    const UINT offset = 0;
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->IASetVertexBuffers(slot, 1, &host, &stride, &offset);
}
void STDMETHODCALLTYPE set_placed_vertex_buffer(void *self, UINT slot, void *buffer, void *,
                                                UINT stride) {
    set_fast_vertex_buffer(self, slot, buffer, stride);
}
void STDMETHODCALLTYPE set_fast_index_buffer(void *self, UINT hardware_format, void *buffer) {
    set_index_buffer(self, hardware_format, buffer, 0);
}
void STDMETHODCALLTYPE set_placed_index_buffer(void *self, UINT hardware_format, void *buffer,
                                               void *) {
    set_index_buffer(self, hardware_format, buffer, 0);
}

// SetFastResources_Debug(table, end): what titles call instead of writing descriptor packets
// once the context's out-of-line flag (0x390) is set. Each header: entry count in its low
// byte, first slot in the next, stage in bits 16-19 (VS, HS, DS, GS, PS, CS, then 6 for
// vertex streams), resource kind in bits 28-30 (0 views, 1 buffers, else samplers); each
// entry is 4 dwords: our object, then the stride (bits 48-63) and placement address.
constexpr std::size_t out_of_line_flags = 0x390;
void STDMETHODCALLTYPE set_fast_resources(void *self, const UINT *table, const UINT *end) {
    auto *const context = static_cast<ID3D11DeviceContext *>(unwrap(self));
    using View = ID3D11ShaderResourceView;
    using Buffer = ID3D11Buffer;
    using Sampler = ID3D11SamplerState;
    using Context = ID3D11DeviceContext;
    using SetViews = void (STDMETHODCALLTYPE Context::*)(UINT, UINT, View *const *);
    using SetBuffers = void (STDMETHODCALLTYPE Context::*)(UINT, UINT, Buffer *const *);
    using SetSamplers = void (STDMETHODCALLTYPE Context::*)(UINT, UINT, Sampler *const *);
    constexpr SetViews views[] = {&Context::VSSetShaderResources, &Context::HSSetShaderResources,
                                  &Context::DSSetShaderResources, &Context::GSSetShaderResources,
                                  &Context::PSSetShaderResources, &Context::CSSetShaderResources};
    constexpr SetBuffers constants[] = {
        &Context::VSSetConstantBuffers, &Context::HSSetConstantBuffers,
        &Context::DSSetConstantBuffers, &Context::GSSetConstantBuffers,
        &Context::PSSetConstantBuffers, &Context::CSSetConstantBuffers};
    constexpr SetSamplers samplers[] = {&Context::VSSetSamplers, &Context::HSSetSamplers,
                                        &Context::DSSetSamplers, &Context::GSSetSamplers,
                                        &Context::PSSetSamplers, &Context::CSSetSamplers};
    while (table && table < end) {
        const UINT header = *table++;
        const UINT count = header & 0xff, slot = (header >> 8) & 0xff, stage = (header >> 16) & 0xf,
                   kind = (header >> 28) & 7;
        for (UINT i = 0; i < count && table + 4 <= end; ++i, table += 4) {
            void *object;
            std::uint64_t extra;
            std::memcpy(&object, table, 8);
            std::memcpy(&extra, table + 2, 8);
            auto *const host = unwrap(object);
            if (kind == 0 && stage < 6) {
                auto *view = static_cast<View *>(host);
                if (stage == 4)
                    keep_pixel_view(self, slot + i, view);
                (context->*views[stage])(slot + i, 1, &view);
            } else if (kind == 1 && stage == 6) {
                auto *buffer = static_cast<Buffer *>(host);
                const UINT stride = static_cast<UINT>(extra >> 48);
                UINT offset = 0;
                buffer_offset(buffer, extra & 0xffffffffffffull, offset);
                context->IASetVertexBuffers(slot + i, 1, &buffer, &stride, &offset);
            } else if (kind == 1 && stage < 6) {
                auto *buffer = static_cast<Buffer *>(host);
                UINT offset = 0;
                ID3D11DeviceContext1 *latest = nullptr;
                const auto address = extra & 0xffffffffffffull;
                // constants the title wrote at an address of its own (a placement binding)
                if (buffer && address && !buffer_offset(buffer, address, offset)) {
                    D3D11_BUFFER_DESC desc;
                    buffer->GetDesc(&desc);
                    if (desc.Usage == D3D11_USAGE_DEFAULT &&
                        readable_memory(address, desc.ByteWidth))
                        context->UpdateSubresource(buffer, 0, nullptr,
                                                   reinterpret_cast<const void *>(address), 0, 0);
                }
                if (buffer && buffer_offset(buffer, extra & 0xffffffffffffull, offset) && offset &&
                    SUCCEEDED(context->QueryInterface(&latest))) {
                    // constants from the address the title gave, in 256-byte steps
                    D3D11_BUFFER_DESC desc;
                    buffer->GetDesc(&desc);
                    const UINT first = offset / 256 * 16,
                               count =
                                   std::min(4096u, (desc.ByteWidth / 16 - first + 15) / 16 * 16);
                    using Set1 = void (STDMETHODCALLTYPE ID3D11DeviceContext1::*)(
                        UINT, UINT, Buffer *const *, const UINT *, const UINT *);
                    constexpr Set1 constants1[] = {&ID3D11DeviceContext1::VSSetConstantBuffers1,
                                                   &ID3D11DeviceContext1::HSSetConstantBuffers1,
                                                   &ID3D11DeviceContext1::DSSetConstantBuffers1,
                                                   &ID3D11DeviceContext1::GSSetConstantBuffers1,
                                                   &ID3D11DeviceContext1::PSSetConstantBuffers1,
                                                   &ID3D11DeviceContext1::CSSetConstantBuffers1};
                    (latest->*constants1[stage])(slot + i, 1, &buffer, &first, &count);
                    latest->Release();
                } else {
                    (context->*constants[stage])(slot + i, 1, &buffer);
                }
            } else if (stage < 6) {
                auto *sampler = static_cast<Sampler *>(host);
                (context->*samplers[stage])(slot + i, 1, &sampler);
            }
        }
    }
}
// The same of the first kit, whose headers say what is bound another way: a number in bits
// 16-23 (0-5 the views of a stage, 7-12 its constant buffers, 13 the vertex streams) and 2
// in bits 24-27 for samplers. The stages and the entries are the same.
void STDMETHODCALLTYPE set_fast_resources_earliest(void *self, const UINT *table, const UINT *end) {
    std::vector<UINT> later(table, table && end > table ? end : table);
    for (std::size_t at = 0; at < later.size(); at += 1 + (later[at] & 0xff) * 4) {
        const UINT header = later[at], number = (header >> 16) & 0xff;
        const bool sampler = ((header >> 24) & 0xf) == 2;
        UINT stage = 15, kind = 0; // nothing known: skipped
        if (sampler || number < 6)
            stage = number, kind = sampler ? 2 : 0;
        else if (number >= 7 && number <= 13)
            stage = number - 7, kind = 1;
        later[at] = (header & 0xffff) | stage << 16 | kind << 28;
    }
    set_fast_resources(self, later.data(), later.data() + later.size());
}
// SetFastResourcesFromBatch_Debug (213): resources as the packets the title would have put
// in the console's ring itself. Each is a word that says how many words follow and where in
// the GPU's table of bindings they go, then the descriptors of the objects, which here name
// our objects (descriptors.cpp). The table has a part per stage: 20 constant buffers of 16
// bytes, 128 views of 32 and 16 samplers of 16; the vertex buffers come after the stages.
void STDMETHODCALLTYPE set_fast_resources_from_batch(void *self, const UINT *batch, UINT bytes) {
    auto *const context = static_cast<ID3D11DeviceContext *>(unwrap(self));
    using View = ID3D11ShaderResourceView;
    using Buffer = ID3D11Buffer;
    using Sampler = ID3D11SamplerState;
    using Context = ID3D11DeviceContext;
    using Latest = ID3D11DeviceContext1;
    using SetViews = void (STDMETHODCALLTYPE Context::*)(UINT, UINT, View *const *);
    using SetBuffers = void (STDMETHODCALLTYPE Context::*)(UINT, UINT, Buffer *const *);
    using SetBuffers1 =
        void (STDMETHODCALLTYPE Latest::*)(UINT, UINT, Buffer *const *, const UINT *, const UINT *);
    using SetSamplers = void (STDMETHODCALLTYPE Context::*)(UINT, UINT, Sampler *const *);
    struct Stage {
        UINT base;
        SetViews views;
        SetBuffers constants;
        SetBuffers1 constants1;
        SetSamplers samplers;
        bool pixel;
    };
    // by where their part of the table starts
    static const Stage stages[] = {
        {0x0, &Context::CSSetShaderResources, &Context::CSSetConstantBuffers,
         &Latest::CSSetConstantBuffers1, &Context::CSSetSamplers, false},
        {0x1c40, &Context::VSSetShaderResources, &Context::VSSetConstantBuffers,
         &Latest::VSSetConstantBuffers1, &Context::VSSetSamplers, false},
        {0x2fb0, &Context::HSSetShaderResources, &Context::HSSetConstantBuffers,
         &Latest::HSSetConstantBuffers1, &Context::HSSetSamplers, false},
        {0x4320, &Context::DSSetShaderResources, &Context::DSSetConstantBuffers,
         &Latest::DSSetConstantBuffers1, &Context::DSSetSamplers, false},
        {0x5690, &Context::GSSetShaderResources, &Context::GSSetConstantBuffers,
         &Latest::GSSetConstantBuffers1, &Context::GSSetSamplers, false},
        {0x6a00, &Context::PSSetShaderResources, &Context::PSSetConstantBuffers,
         &Latest::PSSetConstantBuffers1, &Context::PSSetSamplers, true}};
    constexpr UINT vertex_buffers = 0x7d70, views_at = 0x140, samplers_at = 0x1140,
                   samplers_end = 0x1240;
    Latest *latest = nullptr;
    context->QueryInterface(&latest);
    const auto *at = batch;
    const auto *const end = batch + bytes / 4;
    while (at && at < end) {
        const UINT header = *at++, count = (header >> 16) + 1, place = header & 0xffff;
        if (count > static_cast<UINT>(end - at))
            break;
        const auto *const words = at;
        at += count;
        if (place >= vertex_buffers) {
            for (UINT i = 0; i + 4 <= count; i += 4) {
                UINT offset = 0;
                auto *const buffer = described_buffer(words + i, offset);
                const UINT stride = (words[i + 1] >> 16) & 0x3fff;
                if (buffer)
                    context->IASetVertexBuffers((place - vertex_buffers + i * 4) / 16, 1, &buffer,
                                                &stride, &offset);
            }
            continue;
        }
        const Stage *stage = &stages[0];
        for (const auto &candidate : stages)
            if (place >= candidate.base)
                stage = &candidate;
        const UINT within = place - stage->base;
        if (within < views_at) {
            for (UINT i = 0; i + 4 <= count; i += 4) {
                const UINT slot = (within + i * 4) / 16;
                UINT offset = 0;
                auto *const buffer = described_buffer(words + i, offset);
                if (!buffer)
                    continue;
                if (offset && latest) {
                    // constants from where the descriptor points, in 256-byte steps
                    D3D11_BUFFER_DESC desc;
                    buffer->GetDesc(&desc);
                    const UINT first = offset / 256 * 16,
                               constants =
                                   std::min(4096u, (desc.ByteWidth / 16 - first + 15) / 16 * 16);
                    (latest->*stage->constants1)(slot, 1, &buffer, &first, &constants);
                } else {
                    (context->*stage->constants)(slot, 1, &buffer);
                }
            }
        } else if (within < samplers_at) {
            for (UINT i = 0; i + 8 <= count; i += 8) {
                const UINT slot = (within - views_at + i * 4) / 32;
                auto *view = described_view(words + i);
                if (!view)
                    continue;
                if (stage->pixel)
                    keep_pixel_view(self, slot, view);
                (context->*stage->views)(slot, 1, &view);
            }
        } else if (within < samplers_end) {
            for (UINT i = 0; i + 4 <= count; i += 4) {
                auto *sampler = described_sampler(words + i);
                if (sampler)
                    (context->*stage->samplers)((within - samplers_at + i * 4) / 16, 1, &sampler);
            }
        }
    }
    if (latest)
        latest->Release();
}

// The context's command buffer, which titles write GPU packets into themselves: the write
// pointer at 0x50 and its end at 0x58. There is no GPU to read them here, so they go to a
// scratch buffer of the context's that MakeCeSpace hands out again whenever it is full.
void *STDMETHODCALLTYPE make_ce_space(void *self) {
    std::byte *buffer;
    {
        const std::lock_guard lock(scratch_mutex);
        auto &owned = scratch[self];
        if (!owned)
            owned = std::make_unique<std::byte[]>(scratch_bytes);
        buffer = owned.get();
    }
    auto *const words = static_cast<std::byte **>(self);
    words[command_write] = buffer;
    words[command_end] = buffer + scratch_bytes - 4096; // the biased limit
    words[command_end + 1] = buffer + scratch_bytes;
    return buffer;
}
void STDMETHODCALLTYPE clear_render_target_x(void *self, void *view, UINT, const FLOAT *color) {
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->ClearRenderTargetView(static_cast<ID3D11RenderTargetView *>(unwrap(view)), color);
}
std::atomic<UINT64> context_fences{0};
UINT64 STDMETHODCALLTYPE insert_fence(void *, UINT) { return ++context_fences; }
// the GPU writes the value once the work before it is done, which here it already is
void STDMETHODCALLTYPE write_value_end_of_pipe(void *, void *destination, UINT value, UINT) {
    if (destination)
        std::memcpy(destination, &value, sizeof value);
}
void STDMETHODCALLTYPE context_copy_memory(void *, void *destination, const void *source,
                                           SIZE_T size) {
    std::memmove(destination, source, size);
}
void STDMETHODCALLTYPE context_fill_memory(void *, void *destination, SIZE_T size, UINT value) {
    auto *const words = static_cast<UINT *>(destination);
    for (SIZE_T i = 0; i < size / 4; ++i)
        words[i] = value;
}

// XXSetShader through the in-object table: the shader alone, no class instances
template <class Shader, void (STDMETHODCALLTYPE ID3D11DeviceContext::*Set)(
                            Shader *, ID3D11ClassInstance *const *, UINT)>
void STDMETHODCALLTYPE set_shader(void *self, void *shader) {
    (static_cast<ID3D11DeviceContext *>(unwrap(self))->*Set)(static_cast<Shader *>(unwrap(shader)),
                                                             nullptr, 0);
}

// Desktop slot of every ID3D11DeviceContext1 method, with what its arguments are.
std::array<Method, 134> context_methods() {
    std::array<Method, 134> methods{};
    const auto set = [&](std::size_t slot, std::initializer_list<Fix> fixes = {}) {
        methods[slot] = forward(slot, fixes);
    };
    set(3, {out(1, Kind::device)});
    for (std::size_t slot = 4; slot <= 6; ++slot)
        set(slot);
    // per stage: constant buffers, shader resources, samplers (start, count, array)
    for (std::size_t slot : {7, 16, 22, 62, 66, 71, 8, 25, 31, 59, 63, 67, 10, 26, 32, 61, 65, 70})
        set(slot, {in_array(3, 2)});
    for (std::size_t slot : {9, 11, 23, 60, 64, 69}) // shader, class instances
        set(slot, {in(1), in_array(2, 3)});
    for (std::size_t slot : {12, 13, 20, 21, 24, 38, 41, 44, 45, 110, 111, 112, 113})
        set(slot); // draws, topology, viewports, scissors, ClearState, Flush
    for (std::size_t slot : {12, 13, 20, 21, 38})
        methods[slot].draws = true;
    set(14, {in(1)}); // Map
    set(15, {in(1)}); // Unmap
    set(17, {in(1)}); // IASetInputLayout
    set(18, {in_array(3, 2)});
    set(19, {in(1)});
    for (std::size_t slot : {27, 28, 29, 30}) // Begin, End, GetData, SetPredication
        set(slot, {in(1)});
    set(33, {in_array(2, 1), in(3)}); // OMSetRenderTargets
    set(34, {in_array(2, 1), in(3), in_array(6, 5)});
    set(35, {in(1)});
    set(36, {in(1)});
    set(37, {in_array(2, 1)});
    for (std::size_t slot : {39, 40, 42, 43}) // indirect draws, RSSetState
        set(slot, {in(1)});
    methods[39].draws = methods[40].draws = true;
    set(46, {in(1), in(6)}); // CopySubresourceRegion
    set(47, {in(1), in(2)}); // CopyResource
    set(48, {in(1)});        // UpdateSubresource
    set(49, {in(1), in(3)});
    for (std::size_t slot : {50, 51, 52, 54}) // clears, GenerateMips
        set(slot, {in(1)});
    methods[53] = custom(&clear_depth_stencil);
    methods[55] = custom(&set_min_lod);
    methods[56] = custom(&get_min_lod);
    set(57, {in(1), in(3)}); // ResolveSubresource
    set(58, {in(1)});        // ExecuteCommandList
    set(68, {in_array(3, 2)});
    // what is bound, per stage: constant buffers, shader resources, samplers, shader
    for (std::size_t slot : {72, 77, 81, 100, 104, 109, 125, 126, 127, 128, 129, 130})
        set(slot, {out_array(3, 2, Kind::buffer)});
    for (std::size_t slot : {73, 84, 87, 97, 101, 105, 106})
        set(slot, {out_array(3, 2, Kind::view)});
    for (std::size_t slot : {75, 85, 88, 99, 103, 108})
        set(slot, {out_array(3, 2, Kind::state)});
    for (std::size_t slot : {74, 76, 82, 98, 102, 107})
        set(slot, {out(1, Kind::child)});
    set(78, {out(1, Kind::child)});                             // IAGetInputLayout
    set(79, {out_array(3, 2, Kind::buffer)});                   // IAGetVertexBuffers
    set(80, {out(1, Kind::buffer)});                            // IAGetIndexBuffer
    set(83);                                                    // IAGetPrimitiveTopology
    set(86, {out(1, Kind::state)});                             // GetPredication
    set(89, {out_array(2, 1, Kind::view), out(3, Kind::view)}); // OMGetRenderTargets
    set(90, {out_array(2, 1, Kind::view), out(3, Kind::view), out_array(6, 5, Kind::view)});
    set(91, {out(1, Kind::state)});
    set(92, {out(1, Kind::state)});
    set(93, {out_array(2, 1, Kind::buffer)}); // SOGetTargets
    set(94, {out(1, Kind::state)});
    set(95);
    set(96);
    set(114, {created(2, Kind::state)}); // FinishCommandList
    set(115, {in(1), in(6)});
    set(116, {in(1)});
    set(117, {in(1)});
    set(118, {in(1)});
    for (std::size_t slot = 119; slot <= 124; ++slot) // constant buffers with ranges
        set(slot, {in_array(3, 2)});
    set(132, {in(1)});
    set(133, {in(1)});
    return methods;
}
// The context has its methods twice: in the vtable, two slots later than on the desktop
// like every device child, and as pointers inside the object, which is how titles call
// most of them. That table is in desktop
// order with VSSetConstantBuffers (desktop 7) at 131, desktop + 124, and has no
// IASetPrimitiveTopology (24; the topology is state), so from 25 on it is desktop + 123.
// Its shader setters take the shader alone.
Layout context() {
    constexpr std::size_t object_table = 124;
    Layout result{"context", object_slots};
    device_child(result);
    const auto methods = context_methods();
    for (std::size_t host = 7; host < methods.size(); ++host)
        result.vtable[host + 2] = methods[host];
    for (std::size_t host = 3; host < methods.size(); ++host)
        if (host != 24)
            result.object[host + object_table - (host >= 25)] = methods[host];
    using Context = ID3D11DeviceContext;
    result.object[9 + object_table] = custom(&set_shader<ID3D11PixelShader, &Context::PSSetShader>);
    result.object[11 + object_table] =
        custom(&set_shader<ID3D11VertexShader, &Context::VSSetShader>);
    result.object[23 + object_table] =
        custom(&set_shader<ID3D11GeometryShader, &Context::GSSetShader>);
    result.object[60 + 123] = custom(&set_shader<ID3D11HullShader, &Context::HSSetShader>);
    result.object[64 + 123] = custom(&set_shader<ID3D11DomainShader, &Context::DSSetShader>);
    result.object[69 + 123] = custom(&set_shader<ID3D11ComputeShader, &Context::CSSetShader>);
    // ID3D11DeviceContextX's own methods (OS 6.2.11396 order), in the
    // vtable and in the object at the vtable slot + 124: PIX, counters, caches, fences,
    // limits and hints have no effect here
    const auto console = [&](std::size_t slot, Method method) {
        result.vtable[slot] = method;
        result.object[slot + 124] = method;
    };
    for (std::size_t slot = 141; slot <= 154; ++slot) // PIX..InsertWaitUntilIdle
        console(slot, {Method::ignore});
    console(155, custom(&insert_fence));
    console(156, {Method::ignore}); // InsertWaitOnFence
    // Remap...Inheritance: which bindings a command list takes over from the context that
    // runs it. Lists take over nothing here, so there is nothing to remap.
    for (std::size_t slot = 157; slot <= 160; ++slot)
        console(slot, {Method::ignore});
    // Set{Fast,Placement}{ConstantBuffer,ShaderResource,Sampler} per stage from 161, in the
    // order PS, VS, GS, CS, HS, DS
    using Buffer = ID3D11Buffer;
    using View = ID3D11ShaderResourceView;
    using Sampler = ID3D11SamplerState;
    const Method fast[] = {custom(&set_one<Buffer, &Context::PSSetConstantBuffers>),
                           custom(&set_one<View, &Context::PSSetShaderResources>),
                           custom(&set_one<Sampler, &Context::PSSetSamplers>),
                           custom(&set_one<Buffer, &Context::VSSetConstantBuffers>),
                           custom(&set_one<View, &Context::VSSetShaderResources>),
                           custom(&set_one<Sampler, &Context::VSSetSamplers>),
                           custom(&set_one<Buffer, &Context::GSSetConstantBuffers>),
                           custom(&set_one<View, &Context::GSSetShaderResources>),
                           custom(&set_one<Sampler, &Context::GSSetSamplers>),
                           custom(&set_one<Buffer, &Context::CSSetConstantBuffers>),
                           custom(&set_one<View, &Context::CSSetShaderResources>),
                           custom(&set_one<Sampler, &Context::CSSetSamplers>),
                           custom(&set_one<Buffer, &Context::HSSetConstantBuffers>),
                           custom(&set_one<View, &Context::HSSetShaderResources>),
                           custom(&set_one<Sampler, &Context::HSSetSamplers>),
                           custom(&set_one<Buffer, &Context::DSSetConstantBuffers>),
                           custom(&set_one<View, &Context::DSSetShaderResources>),
                           custom(&set_one<Sampler, &Context::DSSetSamplers>),
                           custom(&set_fast_vertex_buffer),
                           custom(&set_fast_index_buffer),
                           custom(&set_placed<Buffer, &Context::PSSetConstantBuffers>),
                           custom(&set_placed<View, &Context::PSSetShaderResources>),
                           custom(&set_placed<Buffer, &Context::VSSetConstantBuffers>),
                           custom(&set_placed<View, &Context::VSSetShaderResources>),
                           custom(&set_placed<Buffer, &Context::GSSetConstantBuffers>),
                           custom(&set_placed<View, &Context::GSSetShaderResources>),
                           custom(&set_placed<Buffer, &Context::CSSetConstantBuffers>),
                           custom(&set_placed<View, &Context::CSSetShaderResources>),
                           custom(&set_placed<Buffer, &Context::HSSetConstantBuffers>),
                           custom(&set_placed<View, &Context::HSSetShaderResources>),
                           custom(&set_placed<Buffer, &Context::DSSetConstantBuffers>),
                           custom(&set_placed<View, &Context::DSSetShaderResources>),
                           custom(&set_placed_vertex_buffer),
                           custom(&set_placed_index_buffer)};
    for (std::size_t i = 0; i < std::size(fast); ++i)
        console(161 + i, fast[i]);
    console(195, {Method::ignore});                   // HSSetTessellationParameters
    for (std::size_t slot = 196; slot <= 208; ++slot) // automatic flush..OMSetSampleMask
        console(slot, {Method::ignore});
    console(209, custom(&make_ce_space));
    console(210, custom(&set_fast_resources));
    console(213, custom(&set_fast_resources_from_batch));
    console(214, {Method::ignore}); // CSPlaceUnorderedAccessView
    console(215, custom(&write_value_end_of_pipe));
    console(216, custom(&context_copy_memory));
    console(217, custom(&context_fill_memory));
    for (std::size_t slot : {220, 221, 222, 224, 225}) // thread traces, wait on present
        console(slot, {Method::ignore});
    console(226, custom(&clear_render_target_x));
    // nothing is compressed here: GetResourceCompression(X) say none, decompressing is done
    for (std::size_t slot = 227; slot <= 230; ++slot)
        console(slot, {Method::ignore});
    result.object[8 + object_table] = custom(&set_shader_resource<&Context::PSSetShaderResources>);
    result.object[19 + object_table] = custom(&set_index_buffer);
    result.object[12 + object_table] = custom(&draw_indexed);
    result.apply_state = &apply_context_state;
    result.object[25 + 123] = custom(&set_shader_resource<&Context::VSSetShaderResources>);
    result.object[31 + 123] = custom(&set_shader_resource<&Context::GSSetShaderResources>);
    result.object[59 + 123] = custom(&set_shader_resource<&Context::HSSetShaderResources>);
    result.object[63 + 123] = custom(&set_shader_resource<&Context::DSSetShaderResources>);
    result.object[67 + 123] = custom(&set_shader_resource<&Context::CSSetShaderResources>);
    return result;
}
// The layouts of the first development kits (SDK versions below 2.0). Device
// children have one console method after ID3D11DeviceChild's, not two, so their own methods
// are one slot earlier; the context's table inside the object starts one pointer earlier;
// IDXGIObject has one more method, at 5, and the swap chain's own are one slot later.
Layout early_layout(const Layout &later, Kind kind) {
    auto result = later;
    if (kind == Kind::device)
        return result;
    if (kind == Kind::swap_chain) {
        for (std::size_t slot = 29; slot > 5; --slot)
            result.vtable[slot] = result.vtable[slot - 1];
        result.vtable[5] = Method{Method::ignore};
        return result;
    }
    for (std::size_t slot = 8; slot + 1 < result.vtable.size(); ++slot)
        result.vtable[slot] = result.vtable[slot + 1];
    // what a resource has at 11 there is not known to be GetDescriptor, which writes a lot
    if (kind == Kind::buffer || kind == Kind::texture1d || kind == Kind::texture2d ||
        kind == Kind::texture3d || kind == Kind::resource)
        result.vtable[11] = {};
    if (kind != Kind::context)
        return result;
    // the console's own methods (later slot 141 on) are three pointers earlier: two of them
    // are not there yet. IASetPlacementIndexBuffer and the command buffer's two methods are
    // where titles call them that way.
    constexpr std::size_t console = 141 + 124;
    result.table = later.table - 1;
    for (std::size_t index = 3; index + 3 < result.object.size(); ++index)
        result.object[index] = later.object[index + (index + 3 >= console ? 3 : 1)];
    // XXSetShaderResources takes the desktop's arguments there (start, count, views)
    const auto methods = context_methods();
    result.object[8 + 123] = methods[8];
    for (std::size_t host : {25, 31, 59, 63, 67})
        result.object[host + 122] = methods[host];
    return result;
}
// The first of those kits (SDK 1.5, seen in one title): the context's table inside the
// object is the desktop's vtable as it is, primitive topology included, from word 54, and
// its methods take the desktop's arguments.
// The kit before (SDK 1.4) has that table right after the object's first words, from word
// 5, and no flag is known for it.
Layout earliest_layout(const Layout &later, Kind kind, std::size_t table) {
    auto result = early_layout(later, kind);
    // no ID3D11Device2 yet: the console's own methods follow ID3D11Device1's
    if (kind == Kind::device)
        for (std::size_t slot = 51; slot + 4 < result.vtable.size(); ++slot)
            result.vtable[slot] = later.vtable[slot + 4];
    if (kind != Kind::context)
        return result;
    result.object = {};
    result.table = table;
    const auto methods = context_methods();
    for (std::size_t host = 3; host < methods.size(); ++host)
        result.object[host + result.table] = methods[host];
    // the console's own follow the desktop's last (133) at once: 77 words before the later
    // kits', going by OMSetDepthStencilStateX, MakeCeSpace and SetFastResources_Debug
    const std::size_t console = 134 + table, earlier = 131 - table;
    for (std::size_t index = console; index + earlier < later.object.size(); ++index)
        result.object[index] = later.object[index + earlier];
    result.object[210 + 124 - earlier] = custom(&set_fast_resources_earliest);
    // no state is left in the object by this kit's headers, and its flag is where the later
    // ones keep theirs of what changed
    result.out_of_line = table == 54 ? 0x168 : 0;
    result.apply_state = [](void *self) {
        upload_placements(static_cast<ID3D11DeviceContext *>(unwrap(self)));
        restore_pixel_views(self);
    };
    return result;
}
const Layout &layout_of_later(Kind kind);
// The launch kit, whose interfaces are still the desktop's: device children have no console
// method after ID3D11DeviceChild's, resources none before their own, and the device has no
// SetPrivateDataInterfaceGraphics. What the console added follows the desktop's methods.
Layout launch_layout(const Layout &later, Kind kind) {
    auto result = later;
    if (kind == Kind::swap_chain || kind == Kind::dma || kind == Kind::compute)
        return result;
    // the console's own methods alone, from the third: CreateDmaEngineContext is the device's
    // first there (GetImmediateContextX is not there yet), the PIX events the context's
    if (kind == Kind::device_x || kind == Kind::context_x) {
        const auto &whole = layout_of_later(kind == Kind::device_x ? Kind::device : Kind::context);
        const std::size_t first = kind == Kind::device_x ? 56 : 141;
        result = Layout{kind == Kind::device_x ? "device (console methods)"
                                               : "context (console methods)"};
        for (std::size_t slot = 3; slot + first - 3 < whole.vtable.size(); ++slot)
            result.vtable[slot] = whole.vtable[slot + first - 3];
        return result;
    }
    if (kind == Kind::device) {
        for (std::size_t slot = 37; slot + 1 < result.vtable.size(); ++slot)
            result.vtable[slot] = later.vtable[slot + 1];
        return result;
    }
    const bool memory = kind == Kind::buffer || kind == Kind::texture1d ||
                        kind == Kind::texture2d || kind == Kind::texture3d ||
                        kind == Kind::resource;
    for (std::size_t slot = 7; slot + 3 < result.vtable.size(); ++slot)
        result.vtable[slot] = later.vtable[slot + (memory && slot >= 10 ? 3 : 2)];
    return result;
}
// 0 later kits, 1 the first ones, 2 the launch kit, 3 the first of the first ones
std::atomic<int> kit_layout{0};
} // namespace

namespace {
const Layout *later_layouts = nullptr;
const Layout &layout_of_later(Kind kind) { return later_layouts[static_cast<std::size_t>(kind)]; }
} // namespace

const Layout &layout(Kind kind) {
    static const Layout layouts[] = {
        device(),
        context(),
        swap_chain(),
        resource("buffer", 10),
        resource("texture 1D", 10),
        resource("texture 2D", 10),
        resource("texture 3D", 10),
        resource("resource", 9),
        view(),
        [] {
            auto state = child("state", 8);
            state.vtable[10] = custom(&state_desc_x);
            state.words = child_words;
            return state;
        }(),
        child("device child", 6),
        dma(),
        compute(),
        Layout{"device (console methods)"},
        Layout{"context (console methods)"},
    };
    static const auto early = [] {
        std::array<Layout, std::size(layouts)> result;
        for (std::size_t kind = 0; kind < result.size(); ++kind)
            result[kind] = early_layout(layouts[kind], static_cast<Kind>(kind));
        return result;
    }();
    static const auto launch = [] {
        later_layouts = layouts;
        std::array<Layout, std::size(layouts)> result;
        for (std::size_t kind = 0; kind < result.size(); ++kind)
            result[kind] = launch_layout(layouts[kind], static_cast<Kind>(kind));
        return result;
    }();
    static const auto earliest = [] {
        std::array<Layout, std::size(layouts)> result;
        for (std::size_t kind = 0; kind < result.size(); ++kind)
            result[kind] = earliest_layout(layouts[kind], static_cast<Kind>(kind), 54);
        return result;
    }();
    static const auto before = [] {
        std::array<Layout, std::size(layouts)> result;
        for (std::size_t kind = 0; kind < result.size(); ++kind)
            result[kind] = earliest_layout(layouts[kind], static_cast<Kind>(kind), 5);
        return result;
    }();
    const auto *const kit = kit_layout == 2   ? launch.data()
                            : kit_layout == 3 ? earliest.data()
                            : kit_layout == 4 ? before.data()
                            : kit_layout == 1 ? early.data()
                                              : layouts;
    return kit[static_cast<std::size_t>(kind)];
}
void first_development_kits(bool yes, bool launch, int earliest) {
    kit_layout = !yes ? 0 : launch ? 2 : earliest ? 2 + earliest : 1;
}
bool first_development_kits() { return kit_layout != 0; }
bool launch_kit() { return kit_layout == 2; }

// Console-only interface IDs, as the system's d3d11_x.dll exports them. Titles use the
// "performance" ones as the device and the context themselves.
constexpr GUID device_x{
    0x177700f9, 0x876a, 0x4436, {0xb3, 0x68, 0x36, 0xa6, 0x04, 0xf8, 0x2c, 0xef}};
constexpr GUID performance_device_x{
    0x88671610, 0x712e, 0x4f1e, {0x84, 0xab, 0x01, 0xb5, 0x94, 0x8b, 0xd3, 0x73}};
constexpr GUID context_x{
    0x48800095, 0x7134, 0x4be7, {0x91, 0x86, 0xb8, 0x6b, 0xec, 0xb2, 0x64, 0x77}};
constexpr GUID performance_context_x{
    0x9458fe06, 0xc78d, 0x47f7, {0x96, 0xa0, 0xec, 0x7b, 0x72, 0x7b, 0xe1, 0xe9}};

bool kind_of(const GUID &iid, Kind &kind) {
    if (iid == __uuidof(ID3D11Texture2D))
        kind = Kind::texture2d;
    else if (iid == __uuidof(ID3D11Texture1D))
        kind = Kind::texture1d;
    else if (iid == __uuidof(ID3D11Texture3D))
        kind = Kind::texture3d;
    else if (iid == __uuidof(ID3D11Buffer))
        kind = Kind::buffer;
    else if (iid == __uuidof(ID3D11Resource))
        kind = Kind::resource;
    else if (iid == __uuidof(ID3D11Device) || iid == __uuidof(ID3D11Device1) || iid == device_x ||
             iid == performance_device_x)
        kind = Kind::device;
    else if (iid == __uuidof(ID3D11DeviceContext) || iid == __uuidof(ID3D11DeviceContext1) ||
             iid == context_x || iid == performance_context_x)
        kind = Kind::context;
    else if (iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IDXGISwapChain1))
        kind = Kind::swap_chain;
    else
        return false;
    return true;
}
} // namespace xv::runtime::durango::d3d11x
