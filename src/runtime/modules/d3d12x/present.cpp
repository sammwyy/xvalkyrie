#include "backends/graphics/backend.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include "runtime/modules/watchdog.hpp"
#include "runtime/modules/window.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dxgi1_6.h>
#include <intrin.h>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

// How a GDK title shows its frames. The console has no swap chain: the title renders into
// resources of its own and hands one to ID3D12CommandQueue::PresentX, and paces itself with
// the device's frame events. The host shows pictures through a swap chain, so there is one
// here, on the title surface, of the size and format of what the title presents: each
// PresentX copies the title's resource into its back buffer and presents that.
namespace xv::runtime::modules::d3d12x {
namespace {
// D3D12XBOX_PRESENT_PLANE_PARAMETERS
struct PlaneParameters {
    UINT64 token;
    UINT resource_count;
    ID3D12Resource **resources;
    D3D12_RECT *source_rectangles;
    void *placement;
    UINT color_space, scale_filter, extended_count;
    void *extended;
    UINT flags;
};
static_assert(sizeof(PlaneParameters) == 0x48);

struct Presenter {
    IDXGISwapChain3 *chain = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool fullscreen = false; // the display is the chain's
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
};
std::mutex mutex;
Presenter presenter;
std::atomic<std::uint64_t> presents{0};
// SetFrameIntervalX: the length of a frame, 60 Hz until the title says otherwise
// (D3D12XBOX_FRAME_INTERVAL_60_HZ), and how many intervals a frame has to reach the display
std::atomic<UINT> interval_microseconds{16667};
std::atomic<UINT> period_intervals{1};

// a format a flip swap chain takes, for what the title presents; UNKNOWN if there is none
DXGI_FORMAT chain_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
void release(Presenter &state) {
    // a chain that has the display cannot be let go of
    if (state.chain && state.fullscreen)
        state.chain->SetFullscreenState(FALSE, nullptr);
    for (IUnknown *object :
         {static_cast<IUnknown *>(state.chain), static_cast<IUnknown *>(state.list),
          static_cast<IUnknown *>(state.allocator), static_cast<IUnknown *>(state.fence)})
        if (object)
            object->Release();
    state = {};
}
void wait_for(ID3D12Fence *fence, UINT64 value) {
    if (fence->GetCompletedValue() >= value)
        return;
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(value, event);
    WaitForSingleObject(event, 5000);
    CloseHandle(event);
}
// the swap chain for pictures like `desc`, made again when the title changes what it presents
HRESULT prepare(ID3D12CommandQueue *queue, const D3D12_RESOURCE_DESC &desc) {
    const auto format = chain_format(desc.Format);
    const auto width = static_cast<UINT>(desc.Width);
    if (presenter.chain && presenter.width == width && presenter.height == desc.Height &&
        presenter.format == format)
        return S_OK;
    if (presenter.fence)
        wait_for(presenter.fence, presenter.value);
    release(presenter);
    ID3D12Device *device = nullptr;
    IDXGIFactory4 *factory = nullptr;
    IDXGISwapChain1 *chain = nullptr;
    auto result = queue->GetDevice(IID_PPV_ARGS(&device));
    if (SUCCEEDED(result))
        result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    auto *const window = static_cast<HWND>(title_window().native());
    DXGI_SWAP_CHAIN_DESC1 chain_desc{};
    chain_desc.Width = width;
    chain_desc.Height = desc.Height;
    chain_desc.Format = format;
    chain_desc.SampleDesc = {1, 0};
    chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain_desc.BufferCount = 2;
    chain_desc.Scaling = DXGI_SCALING_STRETCH;
    chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(result))
        result =
            factory->CreateSwapChainForHwnd(queue, window, &chain_desc, nullptr, nullptr, &chain);
    if (SUCCEEDED(result)) {
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        result = chain->QueryInterface(IID_PPV_ARGS(&presenter.chain));
    }
    if (SUCCEEDED(result))
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&presenter.allocator));
    if (SUCCEEDED(result))
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presenter.allocator,
                                           nullptr, IID_PPV_ARGS(&presenter.list));
    if (SUCCEEDED(result))
        result = presenter.list->Close();
    if (SUCCEEDED(result))
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presenter.fence));
    for (IUnknown *object : {static_cast<IUnknown *>(chain), static_cast<IUnknown *>(factory),
                             static_cast<IUnknown *>(device)})
        if (object)
            object->Release();
    std::fprintf(stderr,
                 "[Runtime] PresentX: a swap chain %ux%u format %u for the title's format %u on "
                 "the title surface: 0x%08lx\n",
                 width, desc.Height, static_cast<unsigned>(format),
                 static_cast<unsigned>(desc.Format), static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result)) {
        release(presenter);
        return result;
    }
    presenter.width = width;
    presenter.height = desc.Height;
    presenter.format = format;
    surface::offer_exclusive_fullscreen();
    return S_OK;
}
// Native full screen (F11 with Options > Graphics > Full screen: Native): the display is
// given to the chain, or taken back, when the window asks for the other of the two. A flip
// chain needs its buffers made again after either; nothing holds one between frames.
void match_fullscreen(Presenter &state) {
    const bool wanted = surface::exclusive_fullscreen_wanted();
    if (wanted == state.fullscreen)
        return;
    auto result = state.chain->SetFullscreenState(wanted, nullptr);
    const auto resized =
        state.chain->ResizeBuffers(0, state.width, state.height, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(result))
        result = resized;
    std::fprintf(stderr, "[Runtime] native full screen %s: 0x%08lx\n", wanted ? "on" : "off",
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
    state.fullscreen = wanted && SUCCEEDED(result);
    if (wanted && FAILED(result)) {
        state.chain->SetFullscreenState(FALSE, nullptr);
        state.chain->ResizeBuffers(0, state.width, state.height, DXGI_FORMAT_UNKNOWN, 0);
        surface::exclusive_fullscreen_failed();
    }
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
// Several planes: the title's picture and an overlay above it (its interface), which the
// console's display puts together. Here they are drawn into the back buffer, the first
// stretched over all of it as it is and each one after over what is below by its alpha,
// taken as premultiplied.
struct Compositor {
    ID3D12RootSignature *signature = nullptr;
    ID3D12PipelineState *opaque = nullptr, *over = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap *views = nullptr, *target = nullptr;
};
Compositor compositor;
constexpr UINT most_planes = 4;
constexpr char compositor_source[] = R"(
Texture2D plane : register(t0);
SamplerState linear_clamp : register(s0);
void vs(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD) {
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    return plane.Sample(linear_clamp, uv);
}
)";
std::vector<std::uint8_t> compile_compositor(const char *entry, const char *target) {
    using Compile =
        HRESULT(WINAPI *)(const void *, SIZE_T, const char *, const void *, void *, const char *,
                          const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
    static const auto compile = [] {
        const auto library =
            LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library ? reinterpret_cast<Compile>(GetProcAddress(library, "D3DCompile")) : nullptr;
    }();
    ID3DBlob *code = nullptr, *errors = nullptr;
    std::vector<std::uint8_t> bytes;
    if (compile && SUCCEEDED(compile(compositor_source, sizeof compositor_source - 1, "compositor",
                                     nullptr, nullptr, entry, target, 0, 0, &code, &errors))) {
        const auto *const data = static_cast<const std::uint8_t *>(code->GetBufferPointer());
        bytes.assign(data, data + code->GetBufferSize());
    }
    for (auto *const blob : {code, errors})
        if (blob)
            blob->Release();
    return bytes;
}
// the compositor for back buffers of `format`, made the first time; false if it cannot be
bool prepare_compositor(ID3D12Device *device, DXGI_FORMAT format) {
    auto &c = compositor;
    if (c.opaque && c.format == format)
        return true;
    for (IUnknown *object : {static_cast<IUnknown *>(c.opaque), static_cast<IUnknown *>(c.over)})
        if (object)
            object->Release();
    c.opaque = c.over = nullptr;
    if (!c.signature) {
        const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 1, &sampler,
                                             D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *blob = nullptr;
        if (FAILED(
                D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)))
            return false;
        const auto result = device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&c.signature));
        blob->Release();
        const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, most_planes,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC target{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(result) ||
            FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&c.views))) ||
            FAILED(device->CreateDescriptorHeap(&target, IID_PPV_ARGS(&c.target))))
            return false;
    }
    static const auto vertex = compile_compositor("vs", "vs_5_0");
    static const auto pixel = compile_compositor("ps", "ps_5_0");
    if (vertex.empty() || pixel.empty())
        return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = c.signature;
    desc.VS = {vertex.data(), vertex.size()};
    desc.PS = {pixel.data(), pixel.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&c.opaque))))
        return false;
    auto &blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&c.over))))
        return false;
    c.format = format;
    return true;
}
// the planes drawn into `back`, recorded on `list`
void compose(ID3D12Device *device, ID3D12GraphicsCommandList *list, ID3D12Resource *back,
             std::span<ID3D12Resource *const> pictures) {
    auto &c = compositor;
    const auto size =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto target = c.target->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(back, nullptr, target);
    std::vector<D3D12_RESOURCE_BARRIER> barriers{
        transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    for (UINT i = 0; i < pictures.size(); ++i) {
        const auto left = tracked_state(pictures[i], D3D12_RESOURCE_STATE_PRESENT);
        if (left != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            barriers.push_back(
                transition(pictures[i], left, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = chain_format(pictures[i]->GetDesc().Format);
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        auto at = c.views->GetCPUDescriptorHandleForHeapStart();
        at.ptr += std::size_t{i} * size;
        device->CreateShaderResourceView(pictures[i], &view, at);
    }
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    const auto desc = back->GetDesc();
    const D3D12_VIEWPORT viewport{
        0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    list->SetGraphicsRootSignature(c.signature);
    list->SetDescriptorHeaps(1, &c.views);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT i = 0; i < pictures.size(); ++i) {
        auto at = c.views->GetGPUDescriptorHandleForHeapStart();
        at.ptr += std::uint64_t{i} * size;
        list->SetPipelineState(i ? c.over : c.opaque);
        list->SetGraphicsRootDescriptorTable(0, at);
        list->DrawInstanced(3, 1, 0, 0);
    }
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}
// XV_D3D_PROBE=<frame>: what the title presents at that frame, and its render targets
void probe(ID3D12Resource *picture) {
    static const int at = [] {
        const auto *const text = std::getenv("XV_D3D_PROBE");
        const int frame = text ? std::atoi(text) : 0;
        return !text ? 0 : frame > 1 ? frame : 300;
    }();
    if (!at || presents.load() + 1 != static_cast<std::uint64_t>(at))
        return;
    std::vector<std::uint8_t> data;
    unsigned pitch = 0, width = 0, height = 0;
    DXGI_FORMAT format{};
    if (read_back(picture, data, pitch, width, height, format,
                  tracked_state(picture, D3D12_RESOURCE_STATE_PRESENT)))
        report_picture(
            data.data(), pitch, width, height, format,
            ("back buffer (" + std::to_string(draws_into_target(picture)) + " draws)").c_str());
    // XV_D3D_PROBE_TARGETS: the render targets as well. Not by default: a title may have let
    // go of one that is still listed, and reading that back takes the process down.
    int index = 0;
    if (std::getenv("XV_D3D_PROBE_TARGETS"))
        for (auto *const target : bound_render_targets())
            if (target != picture &&
                read_back(target, data, pitch, width, height, format,
                          tracked_state(target, D3D12_RESOURCE_STATE_RENDER_TARGET)))
                report_picture(data.data(), pitch, width, height, format,
                               ("render target " + std::to_string(index++) + " (" +
                                std::to_string(draws_into_target(target)) + " draws)")
                                   .c_str());
    report_failure();
}
} // namespace

std::uint64_t frames_shown() { return presents.load(); }

// What became of each frame, by the token WaitFrameEventX gave for it: a title asks with
// GetFrameStatisticsX, and an engine holds its next frames until it is told that the ones
// before were shown. Times are the performance counter's.
struct FrameRecord {
    UINT64 token = 0;
    UINT64 wait_call = 0, wait_return = 0;
    UINT64 present_call = 0, present_return = 0;
    bool shown = false;
};
std::mutex record_mutex;
FrameRecord records[256];
std::atomic<bool> flipped{false}; // a frame was shown since the flip objects were signalled
UINT64 counter() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<UINT64>(value.QuadPart);
}
FrameRecord &record_of(UINT64 token) {
    auto &record = records[token % std::size(records)];
    if (record.token != token)
        record = {token};
    return record;
}
// the end of a PresentX, however it ends: the frame of its token counts as shown
struct Shown {
    UINT64 token, called = counter();
    ~Shown() {
        const std::lock_guard lock(record_mutex);
        auto &record = record_of(token);
        record.present_call = called;
        record.present_return = counter();
        record.shown = true;
        flipped = true;
    }
};

// XV_FRAME_TRACE: how the title paces its frames, the first calls of each kind
bool frame_trace(std::atomic<int> &shown, int most = 12) {
    static const bool enabled = std::getenv("XV_FRAME_TRACE") != nullptr;
    return enabled && shown++ < most;
}

HRESULT present_x(ID3D12CommandQueue *queue, UINT planes, const void *plane_parameters,
                  const void *present_parameters) {
    if (static std::atomic<int> shown{0}; frame_trace(shown, 30)) {
        const auto *const first = static_cast<const PlaneParameters *>(plane_parameters);
        const auto *const whole = static_cast<const UINT *>(present_parameters);
        std::fprintf(
            stderr,
            "[Runtime] PresentX: %u planes, token %llu, %u resources, flags 0x%x, from %s\n",
            planes, first ? first->token : 0, first ? first->resource_count : 0,
            whole ? whole[6] : 0, detail::guest_location(_ReturnAddress()).c_str());
    }
    const Shown shown{
        plane_parameters ? static_cast<const PlaneParameters *>(plane_parameters)->token : 0};
    // no planes: the title shows nothing, which it does while it starts and suspends
    const auto *const plane = static_cast<const PlaneParameters *>(plane_parameters);
    if (!planes || !plane || !plane->resource_count || !plane->resources || !plane->resources[0])
        return S_OK;
    auto *const picture = plane->resources[0];
    auto desc = picture->GetDesc();
    // the planes past the first that can be shown, and the largest, which the chain is made for
    std::vector<ID3D12Resource *> pictures{picture};
    for (UINT i = 1; i < std::min(planes, most_planes); ++i) {
        const auto &above = plane[i];
        if (!above.resource_count || !above.resources || !above.resources[0])
            continue;
        const auto of = above.resources[0]->GetDesc();
        if (chain_format(of.Format) == DXGI_FORMAT_UNKNOWN || of.SampleDesc.Count > 1)
            continue;
        pictures.push_back(above.resources[0]);
        if (of.Width * of.Height > desc.Width * desc.Height)
            desc = of;
    }
    if (chain_format(desc.Format) == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count > 1) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] PresentX of a resource in format %u with %u samples, which "
                         "no swap chain of the host takes\n",
                         static_cast<unsigned>(desc.Format), desc.SampleDesc.Count);
        return S_OK;
    }
    const std::lock_guard lock(mutex);
    if (const auto result = prepare(queue, desc); FAILED(result))
        return result;
    probe(picture);
    capture_frame(picture);
    auto &state = presenter;
    wait_for(state.fence, state.value);
    match_fullscreen(state);
    ID3D12Resource *back = nullptr;
    auto result =
        state.chain->GetBuffer(state.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    if (SUCCEEDED(result))
        result = state.allocator->Reset();
    if (SUCCEEDED(result))
        result = state.list->Reset(state.allocator, nullptr);
    ID3D12Device *device = nullptr;
    if (SUCCEEDED(result) && pictures.size() > 1 &&
        SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
        prepare_compositor(device, state.format)) {
        compose(device, state.list, back, pictures);
        result = state.list->Close();
    } else if (SUCCEEDED(result)) {
        // whatever state the title left its picture in is where it stays
        const auto left = tracked_state(picture, D3D12_RESOURCE_STATE_PRESENT);
        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(picture, left, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
        state.list->ResourceBarrier(2, barriers);
        state.list->CopyResource(back, picture);
        std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
        state.list->ResourceBarrier(2, barriers);
        result = state.list->Close();
    }
    if (SUCCEEDED(result)) {
        ID3D12CommandList *const lists[] = {state.list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(state.fence, ++state.value);
        // one display refresh per frame interval of the title's
        const UINT refreshes = std::clamp((interval_microseconds.load() + 8333) / 16667, 1u, 4u);
        result = state.chain->Present(refreshes, 0);
    }
    if (back)
        back->Release();
    if (device)
        device->Release();
    graphics::frame_counted();
    ++presents;
    frame_watchdog();
    title_window().show();
    // a hidden window does not wait for the display, and a title that sees a status stops
    if (result == DXGI_STATUS_OCCLUDED)
        Sleep(16);
    if (FAILED(result)) {
        if (static std::atomic<bool> reported{false}; !reported.exchange(true)) {
            std::fprintf(stderr, "[Runtime] PresentX failed on the host: 0x%08lx\n",
                         static_cast<unsigned long>(result));
            report_failure();
        }
        return result;
    }
    return S_OK;
}

// The frame events: how a GDK title paces itself with no swap chain to wait for (names as
// Microsoft's Xbox GDK samples, FramePacing above all, and public bindings of the GDK's
// headers have them). A clock ticks once per frame interval. Each kind of event comes once
// per tick, at its offset into the interval, and a wait for it ends on the next one no wait
// has ended on yet: at once when one came while nobody waited, which lets a late title go on
// at its own pace instead of losing an interval. Ticks are counted from the clock's start, so
// lateness does not add up.
namespace {
using Clock = std::chrono::steady_clock;
// D3D12XBOX_FRAME_EVENT_TYPE: the origin (D3D12XBOX_FRAME_EVENT_ORIGIN, where a frame
// begins), the vertical blank and the flip
constexpr UINT origin_event = 0, flip_event = 2;
// ScheduleFrameEventX's DISABLE_SIGNAL: the title's objects are not signalled
constexpr UINT disable_signal = 0x1;
// WaitFrameEventX's flags: a wait for the origin that times out begins the frame all the same.
// What any other flag means is not known here, so they change nothing.
constexpr UINT begin_on_timeout = 0x2;
// D3D12XBOX_SCHEDULE_FRAME_OBJECT_LIST: objects of the title's to signal with an event
struct ScheduleObjectList {
    UINT count;
    HANDLE *objects;
};
// D3D12XBOX_WAIT_FRAME_OBJECT_LIST: objects of the title's that end a wait as well, and where
// the index of the one that did goes
struct WaitObjectList {
    UINT count;
    HANDLE *objects;
    UINT *signaled_index;
};
struct FrameEvent {
    Clock::duration offset{};    // into each interval
    std::vector<HANDLE> objects; // duplicates of the title's, so that they outlive its own
    bool signalled = false;      // the objects are signalled
    std::uint64_t waited = 0;    // the last tick a wait ended on
    std::uint64_t sent = 0;      // the last tick the objects were signalled on
};
struct FrameClock {
    bool started = false;
    std::uint64_t first = 0;         // the tick from which on `interval` counts
    Clock::time_point start, before; // when tick `first` comes, and the one before it came
    Clock::duration interval = std::chrono::microseconds(16667);
    FrameEvent events[3];
};
std::mutex clock_mutex;
FrameClock frame_clock;
// frames begun, each one's number its token (D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL is none's)
std::atomic<UINT64> frames_begun{0};

// the clock, started at its first use (all of these with clock_mutex held)
FrameClock &clock_at(Clock::time_point now) {
    auto &clock = frame_clock;
    if (!clock.started) {
        clock.started = true;
        clock.start = now;
        clock.before = now - clock.interval;
    }
    return clock;
}
// when the event of `kind` of a tick comes, from the one before `first` on: at its offset into
// the tick's interval, or at the interval's end for an offset longer than that
Clock::time_point event_time(const FrameClock &clock, UINT kind, std::uint64_t tick) {
    const auto offset = clock.events[kind].offset;
    if (tick < clock.first)
        return clock.before + std::min(offset, clock.start - clock.before);
    return clock.start + clock.interval * static_cast<Clock::rep>(tick - clock.first) +
           std::min(offset, clock.interval);
}
// the last tick whose event of `kind` has come by `now`
std::uint64_t last_tick(const FrameClock &clock, UINT kind, Clock::time_point now) {
    if (const auto at = event_time(clock, kind, clock.first); now >= at)
        return clock.first + static_cast<std::uint64_t>((now - at) / clock.interval);
    if (clock.first && now >= event_time(clock, kind, clock.first - 1))
        return clock.first - 1;
    return clock.first > 1 ? clock.first - 2 : 0;
}

// Until `until`, or until one of `objects` is signalled, whichever is first: to the
// microsecond with a timer of the host's finer than its 15 ms tick, where it has one. The
// index of the object, `count` when the time came, WAIT_FAILED for an object that cannot be
// waited on.
DWORD wait_until(Clock::time_point until, const HANDLE *objects, DWORD count) {
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    std::copy_n(objects, count, handles);
    const auto left =
        std::chrono::duration_cast<std::chrono::nanoseconds>(until - Clock::now()).count();
    HANDLE timer = nullptr;
    if (left > 0) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(left / 100, 1); // from now, in 100 ns
        if (timer && !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            CloseHandle(timer);
            timer = nullptr;
        }
    }
    const auto milliseconds =
        static_cast<DWORD>(std::max<LONGLONG>((left + 999'999) / 1'000'000, 0));
    DWORD ended = WAIT_TIMEOUT;
    if (timer) {
        handles[count] = timer;
        ended = WaitForMultipleObjects(count + 1, handles, FALSE, INFINITE);
        CloseHandle(timer);
    } else if (count) {
        ended = WaitForMultipleObjects(count, handles, FALSE, milliseconds);
    } else if (milliseconds) {
        Sleep(milliseconds);
    }
    if (ended < WAIT_OBJECT_0 + count)
        return ended - WAIT_OBJECT_0;
    if (ended >= WAIT_ABANDONED_0 && ended < WAIT_ABANDONED_0 + count)
        return ended - WAIT_ABANDONED_0;
    return ended == WAIT_FAILED ? WAIT_FAILED : count;
}

// Signals the title's objects at each event of theirs. The flip's only when a frame was shown
// since the last flip: a flip is of a frame, and an engine counts its frames shown by them.
void signal_events() {
    for (;;) {
        Clock::time_point wake;
        {
            const std::lock_guard lock(clock_mutex);
            const auto now = Clock::now();
            auto &clock = clock_at(now);
            wake = now + clock.interval; // nothing to signal: a look again an interval later
            for (UINT kind = 0; kind < std::size(clock.events); ++kind) {
                auto &event = clock.events[kind];
                if (!event.signalled || event.objects.empty())
                    continue;
                if (const auto tick = last_tick(clock, kind, now); tick > event.sent) {
                    event.sent = tick; // only the last of those missed (a stall): no burst
                    if (kind != flip_event || flipped.exchange(false))
                        for (const auto object : event.objects)
                            if (!SetEvent(object))
                                ReleaseSemaphore(object, 1, nullptr);
                }
                wake = std::min(wake, event_time(clock, kind, event.sent + 1));
            }
        }
        wait_until(wake, nullptr, 0);
    }
}
} // namespace

// SetFrameIntervalX(output, LengthInMicroseconds, PeriodInIntervals, flags). The period is
// how many intervals a frame has to reach the display (the FramePacing sample's frame budget
// is the length times the period, less the origin's offset); frames are shown here as they
// are presented, so it changes no wait, and GetFrameStatisticsX gives it back. There is one
// display to keep to, whatever the output, and no flag but FLAG_NONE is known.
HRESULT STDMETHODCALLTYPE set_frame_interval(ID3D12Device *, void *, UINT microseconds, UINT period,
                                             UINT flags) {
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(stderr,
                     "[Runtime] SetFrameIntervalX: %u microseconds, period %u, flags 0x%x\n",
                     microseconds, period, flags);
    // what a length of 0 does on the console is not known here, so the interval stays
    if (!microseconds)
        return S_OK;
    {
        const std::lock_guard lock(clock_mutex);
        const auto now = Clock::now();
        auto &clock = frame_clock;
        // the new length counts from the next tick, which comes when it would have: a wait
        // for it ends as it would have, and a title that sets the same length again keeps
        // its ticks
        if (clock.started && now >= clock.start) {
            const auto passed = static_cast<std::uint64_t>((now - clock.start) / clock.interval);
            clock.before = clock.start + clock.interval * static_cast<Clock::rep>(passed);
            clock.start = clock.before + clock.interval;
            clock.first += passed + 1;
        }
        clock.interval = std::chrono::microseconds(microseconds);
        clock_at(now);
    }
    interval_microseconds = microseconds;
    period_intervals = period;
    return S_OK;
}
// ScheduleFrameEventX(type, IntervalOffsetInMicroseconds, objects, flags): where in each
// interval an event comes, and the title's objects to signal with it (none given: those it
// had stay), from the next one on
HRESULT STDMETHODCALLTYPE schedule_frame_event(ID3D12Device *, UINT type, UINT offset,
                                               void *signal_list, UINT flags) {
    const auto *const list = static_cast<const ScheduleObjectList *>(signal_list);
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(
            stderr,
            "[Runtime] ScheduleFrameEventX: type %u, offset %u, %u objects, flags 0x%x, from %s\n",
            type, offset, list ? list->count : 0, flags,
            detail::guest_location(_ReturnAddress()).c_str());
    if (type >= std::size(frame_clock.events))
        return E_INVALIDARG;
    std::vector<HANDLE> objects;
    for (UINT i = 0; list && list->objects && i < list->count; ++i) {
        HANDLE copy = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), list->objects[i], GetCurrentProcess(), &copy, 0,
                            FALSE, DUPLICATE_SAME_ACCESS))
            objects.push_back(copy);
    }
    {
        const std::lock_guard lock(clock_mutex);
        const auto now = Clock::now();
        auto &clock = clock_at(now);
        auto &event = clock.events[type];
        if (list) {
            for (const auto old : event.objects)
                CloseHandle(old);
            event.objects = std::move(objects);
        }
        event.offset = std::chrono::microseconds(offset);
        event.signalled = !(flags & disable_signal);
        event.sent = last_tick(clock, type, now);
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread(signal_events).detach(); });
    return S_OK;
}
// WaitFrameEventX(type, TimeOutInMs, objects, flags, token): S_FALSE when the time is up first,
// S_OK when the event or one of the title's objects came, the object's index written then.
// Only a wait for the origin begins a frame and writes its token; the other kinds of event
// come whether a title has scheduled them or not, and leave the token as it was.
HRESULT STDMETHODCALLTYPE wait_frame_event(ID3D12Device *, UINT type, UINT timeout, void *wait_list,
                                           UINT flags, UINT64 *token) {
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(
            stderr,
            "[Runtime] WaitFrameEventX: type %u, timeout %u, list %p, flags 0x%x, from %s\n", type,
            timeout, wait_list, flags, detail::guest_location(_ReturnAddress()).c_str());
    const auto *const list = static_cast<const WaitObjectList *>(wait_list);
    const UINT count = list && list->objects ? list->count : 0;
    // one place of the host's wait is the clock's
    if (type >= std::size(frame_clock.events) || count >= MAXIMUM_WAIT_OBJECTS)
        return E_INVALIDARG;
    const auto called = counter();
    const auto now = Clock::now();
    std::uint64_t tick = 0;
    Clock::time_point comes;
    {
        const std::lock_guard lock(clock_mutex);
        const auto &clock = clock_at(now);
        const auto last = last_tick(clock, type, now);
        const auto waited = clock.events[type].waited;
        tick = last > waited ? last : waited + 1;
        comes = last > waited ? now : event_time(clock, type, tick);
    }
    auto result = S_OK;
    if (comes > now) {
        const auto until =
            timeout == INFINITE ? comes : std::min(comes, now + std::chrono::milliseconds(timeout));
        const auto ended = wait_until(until, count ? list->objects : nullptr, count);
        if (ended == WAIT_FAILED)
            return E_INVALIDARG;
        if (ended < count) {
            if (list->signaled_index)
                *list->signaled_index = ended;
            return S_OK;
        }
        if (until < comes)
            result = S_FALSE;
    }
    if (result == S_OK) {
        const std::lock_guard lock(clock_mutex);
        auto &waited = frame_clock.events[type].waited;
        waited = std::max(waited, tick);
    }
    // a frame begun on a time-out takes no event: the next wait still ends on it
    if (type == origin_event && (result == S_OK || flags & begin_on_timeout)) {
        const auto given = ++frames_begun;
        {
            const std::lock_guard records_lock(record_mutex);
            auto &record = record_of(given);
            record.wait_call = called;
            record.wait_return = counter();
        }
        if (token)
            *token = given;
    }
    return result;
}
HRESULT STDMETHODCALLTYPE frame_statistics(ID3D12Device *, UINT64 token, UINT types, UINT *count,
                                           void *statistics) {
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(
            stderr, "[Runtime] GetFrameStatisticsX: token %llu, types 0x%x, room for %u, from %s\n",
            token, types, count ? *count : 0, detail::guest_location(_ReturnAddress()).c_str());
    // D3D12XBOX_FRAME_STATISTICS: a type and 48 bytes of what goes with it; one entry for
    // each type asked for that there is something to say about
    struct Entry {
        UINT type;
        UINT padding;
        std::uint8_t data[48];
    };
    static_assert(sizeof(Entry) == 56);
    if (!count)
        return E_POINTER;
    const UINT room = statistics ? *count : 0;
    *count = 0;
    const std::lock_guard lock(record_mutex);
    const auto &record = records[token % std::size(records)];
    if (record.token != token)
        return S_OK;
    auto *const entries = static_cast<Entry *>(statistics);
    const auto put = [](Entry &entry, std::size_t at, auto value) {
        std::memcpy(entry.data + at, &value, sizeof value);
    };
    for (const UINT type : {0x1u, 0x2u, 0x100u, 0x1000u, 0x10000u}) {
        if (!(types & type) || *count >= room)
            continue;
        if (type >= 0x100 && !record.shown)
            continue; // not rendered, presented or displayed yet
        Entry entry{type, 0, {}};
        if (type == 0x1) { // the frame interval: LengthInMicroseconds, PeriodInIntervals
            put(entry, 0, static_cast<UINT32>(interval_microseconds.load()));
            put(entry, 4, static_cast<UINT32>(period_intervals.load()));
        } else if (type == 0x2) { // the frame event: when it was waited for and came
            put(entry, 8, record.wait_call);
            put(entry, 16, record.wait_return);
            put(entry, 24, record.wait_return);
        } else if (type == 0x100) { // rendering: when the GPU was done, and how long it took
            put(entry, 8, record.present_return);
            put(entry, 16, record.present_return - record.present_call);
        } else if (type == 0x1000) { // presenting: one plane, one view, nothing queued
            entry.data[1] = 1;
            entry.data[2] = 1;
            put(entry, 8, record.present_call);
            put(entry, 16, record.present_call);
            put(entry, 24, record.present_return);
            put(entry, 32, record.present_return - record.present_call);
            put(entry, 42, UINT16{1});
        } else { // the display: the vertical blank and the flip, at the top of the screen
            put(entry, 8, record.present_return);
            put(entry, 16, record.present_return);
        }
        entries[(*count)++] = entry;
    }
    return S_OK;
}

void report_picture(const std::uint8_t *data, unsigned pitch, unsigned width, unsigned height,
                    DXGI_FORMAT format, const char *what) {
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool ten_bits =
        format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    // one pixel as blue, green, red, alpha
    const auto pixel = [&](UINT x, UINT y, std::uint8_t *out) {
        const auto *in = data + std::size_t{y} * pitch + std::size_t{x} * 4;
        if (ten_bits) {
            std::uint32_t value;
            std::memcpy(&value, in, 4);
            out[2] = static_cast<std::uint8_t>((value & 0x3ff) >> 2);
            out[1] = static_cast<std::uint8_t>(((value >> 10) & 0x3ff) >> 2);
            out[0] = static_cast<std::uint8_t>(((value >> 20) & 0x3ff) >> 2);
            out[3] = static_cast<std::uint8_t>((value >> 30) * 85);
        } else {
            std::memcpy(out, in, 4);
            if (rgba)
                std::swap(out[0], out[2]);
        }
    };
    double sum[4]{};
    std::uint8_t bgra[4];
    for (UINT y = 0; y < height; y += 8)
        for (UINT x = 0; x < width; x += 8) {
            pixel(x, y, bgra);
            sum[0] += bgra[2], sum[1] += bgra[1], sum[2] += bgra[0], sum[3] += bgra[3];
        }
    if (const auto *folder = std::getenv("TEMP"); folder && format != DXGI_FORMAT_D32_FLOAT)
        if (auto *file =
                std::fopen((std::string(folder) + "\\xv_" + what + ".bmp").c_str(), "wb")) {
            const std::uint32_t pixels = width * height * 4;
            std::uint8_t header[54]{'B', 'M'};
            const std::uint32_t size = 54 + pixels, offset = 54, info = 40,
                                planes_bits = 1 | (32 << 16);
            const std::int32_t signed_width = width,
                               signed_height = -static_cast<std::int32_t>(height);
            std::memcpy(header + 2, &size, 4);
            std::memcpy(header + 10, &offset, 4);
            std::memcpy(header + 14, &info, 4);
            std::memcpy(header + 18, &signed_width, 4);
            std::memcpy(header + 22, &signed_height, 4);
            std::memcpy(header + 26, &planes_bits, 4);
            std::fwrite(header, 1, sizeof header, file);
            std::vector<std::uint8_t> row(std::size_t{width} * 4);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x)
                    pixel(x, y, row.data() + std::size_t{x} * 4);
                std::fwrite(row.data(), 1, row.size(), file);
            }
            std::fclose(file);
        }
    const double count = (width / 8.0) * (height / 8.0);
    std::fprintf(stderr, "[Runtime] %s %ux%u format %u average %.1f %.1f %.1f %.1f\n", what, width,
                 height, static_cast<unsigned>(format), sum[0] / count, sum[1] / count,
                 sum[2] / count, sum[3] / count);
}
} // namespace xv::runtime::modules::d3d12x
