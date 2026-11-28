#include "backends/graphics/backend.hpp"
#include "backends/graphics/d3d11/host.hpp"
#include "backends/graphics/dxvk/host.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/com_forward.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/layout.hpp"
#include "runtime/durango/d3d11x/wrap.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/durango/winrt/application.hpp"
#include "runtime/modules/acphal.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/memory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/implements.h>

// d3d11_x.dll on the host's Direct3D 11. Direct3D objects go to the title wrapped in the
// console layout (wrap.hpp). The DXGI device, adapter and factory are the same as on the
// desktop and are passed through, except for the way to the swap chain: a console swap
// chain is made for a CoreWindow, and ours is not one the host would accept.
namespace xv::runtime::durango {
namespace d3d11x {
HRESULT device_dxgi(IUnknown *host_device, REFIID iid, void **object);
}
namespace {
template <class... Interfaces> bool is_any(REFIID iid) {
    return ((iid == __uuidof(Interfaces)) || ...);
}
HRESULT keep(Forwarder *self, void **object) {
    forwarder_add_ref(self);
    *object = self;
    return S_OK;
}
void *slot(auto *function) { return reinterpret_cast<void *>(function); }
// Titles built with the first development kits (SDK versions 1.x, where later ones are 2.x)
// have one more method in IDXGIObject, before GetPrivateData: every DXGI method after it is
// one slot later than on the desktop. 0 for the others.
std::size_t dxgi_inserted = 0;

// IDXGIFactory2::CreateSwapChainForCoreWindow
// The title's first direct queue: a console Direct3D 12 swap chain is made for the device,
// the host's for the queue that presents
HRESULT make_swap_chain(IDXGIFactory2 *factory, IUnknown *device,
                        const DXGI_SWAP_CHAIN_DESC1 *description, IDXGISwapChain1 **swap_chain) {
    auto *const window = winrt::current_title_window();
    if (!window || !description || !swap_chain)
        return E_INVALIDARG;
    // what a CoreWindow takes and a window handle does not
    auto host = *description;
    // flip swap chains take no typeless formats, which the console's do
    if (host.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
        host.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    else if (host.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        host.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (host.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        host.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (host.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        host.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // nor sRGB ones: the buffers are made without, and a view of them may still be sRGB
    else if (host.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        host.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (host.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        host.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // The console's screen is the size of the buffers; the title window is whatever size it
    // is, and without scaling it shows the corner of the picture that fits in it.
    host.Scaling = DXGI_SCALING_STRETCH;
    if (host.AlphaMode != DXGI_ALPHA_MODE_UNSPECIFIED && host.AlphaMode != DXGI_ALPHA_MODE_IGNORE)
        host.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const bool flip = host.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                      host.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (flip) {
        host.BufferCount = std::max(host.BufferCount, 2u);
        host.SampleDesc = {1, 0};
    }
    host.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    host.Flags &= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE |
                  DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                  DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    // the title's object for the device, or one of the DXGI interfaces of it
    auto *presenter = unwrap_forwarder(static_cast<IUnknown *>(d3d11x::unwrap(device)));
    if (ID3D12Device *d3d12 = nullptr;
        presenter && SUCCEEDED(presenter->QueryInterface(IID_PPV_ARGS(&d3d12)))) {
        d3d12->Release();
        if (auto *const queue = modules::d3d12x::direct_queue())
            presenter = queue;
    }
    auto result = factory->CreateSwapChainForHwnd(presenter, static_cast<HWND>(window->native()),
                                                  &host, nullptr, nullptr, swap_chain);
    // A console window takes several swap chains (one per resolution and color space, of
    // which the title presents one); a host window takes one. The others get no window.
    if (result == E_ACCESSDENIED && flip) {
        host.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        host.Scaling = DXGI_SCALING_STRETCH;
        result = factory->CreateSwapChainForComposition(presenter, &host, nullptr, swap_chain);
    }
    if (SUCCEEDED(result))
        *swap_chain =
            static_cast<IDXGISwapChain1 *>(d3d11x::wrap(*swap_chain, d3d11x::Kind::swap_chain));
    std::fprintf(
        stderr,
        "[Runtime] swap chain %ux%u format %u buffers %u effect %u scaling %u alpha %u "
        "samples %u usage 0x%x flags 0x%x for the title window: 0x%08lx\n",
        description->Width, description->Height, static_cast<unsigned>(description->Format),
        description->BufferCount, static_cast<unsigned>(description->SwapEffect),
        static_cast<unsigned>(description->Scaling), static_cast<unsigned>(description->AlphaMode),
        description->SampleDesc.Count, static_cast<unsigned>(description->BufferUsage),
        description->Flags, static_cast<unsigned long>(result));
    std::fflush(stderr);
    return result;
}
HRESULT STDMETHODCALLTYPE create_swap_chain(Forwarder *self, IUnknown *device, IUnknown *,
                                            const DXGI_SWAP_CHAIN_DESC1 *description, IDXGIOutput *,
                                            IDXGISwapChain1 **swap_chain) {
    return make_swap_chain(static_cast<IDXGIFactory2 *>(forwarder_host(self)), device, description,
                           swap_chain);
}
// IDXGIFactory::CreateSwapChain (slot 10): converted to DXGI_SWAP_CHAIN_DESC1 on the title window
HRESULT STDMETHODCALLTYPE create_swap_chain_legacy(Forwarder *self, IUnknown *device,
                                                   DXGI_SWAP_CHAIN_DESC *description,
                                                   IDXGISwapChain **swap_chain) {
    if (!description || !swap_chain)
        return E_INVALIDARG;
    DXGI_SWAP_CHAIN_DESC1 desc1{};
    desc1.Width = description->BufferDesc.Width;
    desc1.Height = description->BufferDesc.Height;
    desc1.Format = description->BufferDesc.Format;
    desc1.SampleDesc = description->SampleDesc;
    desc1.BufferUsage = description->BufferUsage;
    desc1.BufferCount = description->BufferCount;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.SwapEffect = description->SwapEffect;
    desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc1.Flags = description->Flags;
    IDXGISwapChain1 *created = nullptr;
    const auto result = make_swap_chain(static_cast<IDXGIFactory2 *>(forwarder_host(self)), device,
                                        &desc1, &created);
    if (SUCCEEDED(result))
        *swap_chain = created;
    return result;
}
// the host's factory up to IDXGIFactory7 where it has it, 32 methods, else IDXGIFactory2
bool latest_factory = false;
HRESULT factory_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIFactory, IDXGIFactory1, IDXGIFactory2>(iid) ||
        (latest_factory &&
         is_any<IDXGIFactory3, IDXGIFactory4, IDXGIFactory5, IDXGIFactory6, IDXGIFactory7>(iid)))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}
void *factory_forwarder(IDXGIFactory2 *factory) {
    IDXGIFactory7 *latest = nullptr;
    latest_factory = SUCCEEDED(factory->QueryInterface(&latest));
    if (latest_factory)
        factory->Release();
    return make_forwarder(
        latest_factory ? latest : factory, latest_factory ? 32 : 25, &factory_query,
        {{10, slot(&create_swap_chain_legacy)}, {16, slot(&create_swap_chain)}}, dxgi_inserted);
}

// IDXGIObject::GetParent of the adapter
HRESULT STDMETHODCALLTYPE adapter_parent(Forwarder *self, REFIID iid, void **parent) {
    auto *const adapter = static_cast<IDXGIAdapter *>(forwarder_host(self));
    if (!is_any<IDXGIFactory, IDXGIFactory1, IDXGIFactory2, IDXGIFactory3, IDXGIFactory4,
                IDXGIFactory5, IDXGIFactory6, IDXGIFactory7>(iid))
        return adapter->GetParent(iid, parent);
    IDXGIFactory2 *factory = nullptr;
    const auto result =
        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    if (FAILED(result))
        return result;
    *parent = factory_forwarder(factory);
    return S_OK;
}
// IDXGIOutput::GetGammaControlCapabilities, which the host only answers for an output a
// swap chain owns full screen: a title that asks gives up on the failure. The console's
// display has a ramp of 1025 points from 0 to 1.
HRESULT STDMETHODCALLTYPE gamma_capabilities(Forwarder *,
                                             DXGI_GAMMA_CONTROL_CAPABILITIES *capabilities) {
    if (!capabilities)
        return E_POINTER;
    *capabilities = {};
    capabilities->MaxConvertedValue = 1.0f;
    capabilities->NumGammaControlPoints = 1025;
    for (UINT point = 0; point < 1025; ++point)
        capabilities->ControlPointPositions[point] = static_cast<float>(point) / 1024.0f;
    return S_OK;
}
// SetGammaControl: taken and dropped, the picture keeps the host's ramp
HRESULT STDMETHODCALLTYPE set_gamma(Forwarder *, const DXGI_GAMMA_CONTROL *) { return S_OK; }
HRESULT output_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIOutput>(iid))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}
// IDXGIAdapter::EnumOutputs
HRESULT STDMETHODCALLTYPE adapter_outputs(Forwarder *self, UINT index, IDXGIOutput **output) {
    IDXGIOutput *host = nullptr;
    const auto result =
        static_cast<IDXGIAdapter *>(forwarder_host(self))->EnumOutputs(index, &host);
    if (FAILED(result))
        return result;
    *output = reinterpret_cast<IDXGIOutput *>(
        make_forwarder(host, 19, &output_query,
                       {{13, slot(&gamma_capabilities)}, {14, slot(&set_gamma)}}, dxgi_inserted));
    return S_OK;
}
bool latest_adapter = false;
HRESULT adapter_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2>(iid) ||
        (latest_adapter && is_any<IDXGIAdapter3, IDXGIAdapter4>(iid)))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}

// The host's adapter, with GetParent leading to our factory; takes over `host`
HRESULT wrap_adapter(IDXGIAdapter *host, IDXGIAdapter **adapter) {
    HRESULT result = S_OK;
    // up to IDXGIAdapter4 where the host has it, 19 methods, else IDXGIAdapter2
    IDXGIAdapter4 *fourth = nullptr;
    latest_adapter = SUCCEEDED(host->QueryInterface(&fourth));
    IDXGIAdapter2 *latest = nullptr;
    if (latest_adapter)
        latest = fourth;
    else
        result = host->QueryInterface(&latest);
    host->Release();
    if (FAILED(result))
        return result;
    *adapter = reinterpret_cast<IDXGIAdapter *>(
        make_forwarder(latest, latest_adapter ? 19 : 12, &adapter_query,
                       {{6, slot(&adapter_parent)}, {7, slot(&adapter_outputs)}}, dxgi_inserted));
    return S_OK;
}
// IDXGIDevice::GetAdapter
HRESULT STDMETHODCALLTYPE device_adapter(Forwarder *self, IDXGIAdapter **adapter) {
    IDXGIAdapter *host = nullptr;
    const auto result = static_cast<IDXGIDevice *>(forwarder_host(self))->GetAdapter(&host);
    return FAILED(result) ? result : wrap_adapter(host, adapter);
}
// IDXGIObject::GetParent of the DXGI device, the other way to its adapter
HRESULT STDMETHODCALLTYPE device_parent(Forwarder *self, REFIID iid, void **parent) {
    if (!is_any<IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2, IDXGIAdapter3, IDXGIAdapter4>(iid))
        return static_cast<IDXGIDevice *>(forwarder_host(self))->GetParent(iid, parent);
    return device_adapter(self, reinterpret_cast<IDXGIAdapter **>(parent));
}
HRESULT dxgi_device_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIDevice, IDXGIDevice1>(iid))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}

HRESULT WINAPI create_device(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT flags,
                             const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
                             ID3D11Device **device, D3D_FEATURE_LEVEL *level,
                             ID3D11DeviceContext **context) {
    if (device)
        *device = nullptr;
    if (context)
        *context = nullptr;
    // the launch kit still passes the desktop's SDK version (7), and its DXGI is the desktop's
    dxgi_inserted = sdk >= 0x10000 && sdk < 0x20000 ? 5 : 0;
    d3d11x::first_development_kits(sdk < 0x20000, sdk < 0x10000,
                                   sdk < 0x10005   ? 2
                                   : sdk < 0x1000b ? 1
                                                   : 0);
    modules::early_audio_processor(sdk < 0x20000);
    const auto backend = graphics::selected_backend();
    if (!graphics::backend_renders_titles(backend)) {
        std::fprintf(stderr,
                     "[Runtime] the vulkan backend needs xv_d3d11.dll and xv_dxgi.dll next to "
                     "xvalkyrie.exe\n");
        std::fflush(stderr);
        return DXGI_ERROR_UNSUPPORTED;
    }
    // Console-only flag bits and the debug layer, which may not be installed, are dropped.
    const UINT host_flags =
        flags & (D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                 D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS |
                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    ID3D11Device1 *latest = nullptr;
    bool debug_layer = false;
    const bool vulkan = backend == graphics::Backend::vulkan;
    const auto result = vulkan ? graphics::dxvk::create_device(host_flags, levels, level_count,
                                                               &latest, level, context)
                               : graphics::d3d11::create_device(
                                     host_flags, std::getenv("XV_D3D_DEBUG") != nullptr, levels,
                                     level_count, &latest, level, context, debug_layer);
    if (debug_layer)
        d3d11x::watch_debug_messages(latest);
    std::fprintf(stderr, "[Runtime] Direct3D 11 device on %s (flags 0x%x, sdk 0x%x): 0x%08lx\n",
                 vulkan ? "Vulkan" : "the host", flags, sdk, static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result)) {
        if (context && *context) {
            (*context)->Release();
            *context = nullptr;
        }
        return result;
    }
    graphics::drawing_with(vulkan ? "Vulkan" : "Direct3D 11");
    if (context && *context)
        *context =
            static_cast<ID3D11DeviceContext *>(d3d11x::wrap(*context, d3d11x::Kind::context));
    if (device)
        *device = static_cast<ID3D11Device *>(d3d11x::wrap(latest, d3d11x::Kind::device));
    else
        latest->Release();
    return S_OK;
}

// The parameters are the
// console's ring and tessellation buffer sizes, which mean nothing to the host.
// They start with the SDK version of the kit the title was built with.
HRESULT WINAPI create_device_x(const UINT *parameters, ID3D11Device **device,
                               ID3D11DeviceContext **context) {
    return create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                         parameters ? parameters[0] : D3D11_SDK_VERSION, device, nullptr, context);
}
// D3D11XCreateDeviceXAndSwapChain1: the device, and a swap chain for the title's window
// from the factory of the device's adapter
HRESULT WINAPI create_device_and_swap_chain(const UINT *parameters,
                                            const DXGI_SWAP_CHAIN_DESC1 *description,
                                            IDXGISwapChain1 **swap_chain, ID3D11Device **device,
                                            ID3D11DeviceContext **context) {
    if (swap_chain)
        *swap_chain = nullptr;
    ID3D11Device *made = nullptr;
    auto result = create_device_x(parameters, &made, context);
    if (FAILED(result))
        return result;
    if (description && swap_chain) {
        auto *const host = static_cast<IUnknown *>(d3d11x::unwrap(made));
        IDXGIDevice *dxgi = nullptr;
        IDXGIAdapter *adapter = nullptr;
        IDXGIFactory2 *factory = nullptr;
        result = host->QueryInterface(&dxgi);
        if (SUCCEEDED(result))
            result = dxgi->GetAdapter(&adapter);
        if (SUCCEEDED(result))
            result = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result))
            result = make_swap_chain(factory, made, description, swap_chain);
        for (IUnknown *const object :
             {static_cast<IUnknown *>(factory), static_cast<IUnknown *>(adapter),
              static_cast<IUnknown *>(dxgi)})
            if (object)
                object->Release();
    }
    if (device && SUCCEEDED(result))
        *device = made;
    else
        made->Release();
    return result;
}
HRESULT WINAPI present_array(UINT interval, UINT, UINT, UINT count, void **swap_chains,
                             const void *) {
    auto result = S_OK;
    for (UINT i = 0; swap_chains && i < count; ++i)
        if (const auto presented = d3d11x::present_swap_chain(swap_chains[i], interval);
            FAILED(presented))
            result = presented;
    return result;
}
// DXGIX_FRAME_STATISTICS, 0x70 bytes: nothing measured
HRESULT WINAPI frame_statistics(UINT count, void *statistics) {
    if (statistics)
        std::memset(statistics, 0, std::size_t{count} * 0x70);
    return S_OK;
}
// ESRAM is ordinary memory here, 64 KiB pages committed where the title reserved them
HRESULT WINAPI map_esram(UINT, void *address, UINT pages, const UINT *) {
    return modules::commit_title_memory(address, std::size_t{pages} << 16) ? S_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI ignored() { return S_OK; }

// Events a title asks to have set by the display. Each is a copy of the title's handle, so
// that one the title closes does no harm.
std::mutex notification_mutex;
bool keep_event(HANDLE &kept, HANDLE event) {
    HANDLE copy = nullptr;
    if (event && !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &copy, 0, FALSE,
                                  DUPLICATE_SAME_ACCESS))
        return false;
    const std::lock_guard lock(notification_mutex);
    if (kept)
        CloseHandle(kept);
    kept = copy;
    return true;
}
// DXGIXSetVLineNotification(counter, line, event): the event is set each time the display's
// scan gets to that line, which is once per refresh; no event stops the counter. A title
// that paces its main loop on it waits for good if nothing sets it. The refresh here is the
// console's 60 a second, whatever the host's display does, and every line is the first.
HANDLE vline_events[2] = {};
HRESULT WINAPI set_vline_notification(UINT counter, UINT, HANDLE event) {
    if (counter >= std::size(vline_events))
        return E_INVALIDARG;
    if (!keep_event(vline_events[counter], event))
        return E_HANDLE;
    static const bool started = [] {
        std::thread([] {
            // a timer of the host's finer than its 15 ms tick, where it has one
            auto *const timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
            LARGE_INTEGER frequency, now;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&now);
            const auto refresh = frequency.QuadPart / 60;
            auto next = now.QuadPart;
            for (;;) {
                next += refresh;
                QueryPerformanceCounter(&now);
                if (now.QuadPart > next + 4 * refresh)
                    next = now.QuadPart; // far behind: no burst to catch up
                const auto left = (next - now.QuadPart) * 10'000'000 / frequency.QuadPart;
                LARGE_INTEGER due;
                due.QuadPart = -left;
                if (left > 0 && timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, INFINITE);
                else if (left > 0)
                    Sleep(static_cast<DWORD>(left / 10'000));
                const std::lock_guard lock(notification_mutex);
                for (auto *const event : vline_events)
                    if (event)
                        SetEvent(event);
            }
        }).detach();
        return true;
    }();
    static_cast<void>(started);
    return S_OK;
}
// DXGIXSetFrameNotification(kind, event): set when a frame is queued, completed or flipped.
// A frame is all three here by the time Present returns.
HANDLE frame_events[3] = {};
HRESULT WINAPI set_frame_notification(UINT kind, HANDLE event) {
    if (kind >= std::size(frame_events))
        return E_INVALIDARG;
    return keep_event(frame_events[kind], event) ? S_OK : E_HANDLE;
}

} // namespace

void d3d11x::signal_frame_notifications() {
    const std::lock_guard lock(notification_mutex);
    for (auto *const event : frame_events)
        if (event)
            SetEvent(event);
}

// IDXGIDevice of our device: the host's, with GetAdapter leading to our factory
HRESULT d3d11x::device_dxgi(IUnknown *host_device, REFIID, void **object) {
    IDXGIDevice1 *host = nullptr;
    const auto result = host_device->QueryInterface(&host);
    if (FAILED(result))
        return result;
    *object =
        make_forwarder(host, 14, &dxgi_device_query,
                       {{6, slot(&device_parent)}, {7, slot(&device_adapter)}}, dxgi_inserted);
    return S_OK;
}

void install_d3d11x(Registry &registry) {
    const std::pair<const char *, void *> functions[] = {
        {"D3D11CreateDevice", reinterpret_cast<void *>(&create_device)},
        {"D3D11XCreateDeviceX", reinterpret_cast<void *>(&create_device_x)},
        {"D3D11XCreateDeviceXAndSwapChain1",
         reinterpret_cast<void *>(&create_device_and_swap_chain)},
        {"DXGIXPresentArray", reinterpret_cast<void *>(&present_array)},
        {"DXGIXGetFrameStatistics", reinterpret_cast<void *>(&frame_statistics)},
        {"D3DMapEsramMemory", reinterpret_cast<void *>(&map_esram)},
        {"DXGIXSetVLineNotification", reinterpret_cast<void *>(&set_vline_notification)},
        {"DXGIXSetFrameNotification", reinterpret_cast<void *>(&set_frame_notification)}};
    for (const auto &[name, function] : functions)
        registry.add("d3d11_x.dll", name, function);
    // interface IDs the DLL exports as data, for titles built without their own copies
    const auto iid = []<class Interface>(const Interface *) {
        static const GUID value = __uuidof(Interface);
        return const_cast<GUID *>(&value);
    };
#define XV_IID(Interface) {"IID_" #Interface, iid(static_cast<const Interface *>(nullptr))}
    const std::pair<const char *, void *> ids[] = {XV_IID(IDXGIObject),
                                                   XV_IID(IDXGIDeviceSubObject),
                                                   XV_IID(IDXGIResource),
                                                   XV_IID(IDXGISurface),
                                                   XV_IID(IDXGISurface1),
                                                   XV_IID(IDXGIAdapter),
                                                   XV_IID(IDXGIAdapter1),
                                                   XV_IID(IDXGIAdapter2),
                                                   XV_IID(IDXGIOutput),
                                                   XV_IID(IDXGISwapChain),
                                                   XV_IID(IDXGISwapChain1),
                                                   XV_IID(IDXGIFactory),
                                                   XV_IID(IDXGIFactory1),
                                                   XV_IID(IDXGIFactory2),
                                                   XV_IID(IDXGIDevice),
                                                   XV_IID(IDXGIDevice1),
                                                   XV_IID(IDXGIDevice2),
                                                   XV_IID(ID3D11Device),
                                                   XV_IID(ID3D11Device1),
                                                   XV_IID(ID3D11DeviceContext),
                                                   XV_IID(ID3D11DeviceContext1),
                                                   XV_IID(ID3D11DeviceChild),
                                                   XV_IID(ID3D11Resource),
                                                   XV_IID(ID3D11Buffer),
                                                   XV_IID(ID3D11Texture1D),
                                                   XV_IID(ID3D11Texture2D),
                                                   XV_IID(ID3D11Texture3D),
                                                   XV_IID(ID3D11View),
                                                   XV_IID(ID3D11ShaderResourceView),
                                                   XV_IID(ID3D11RenderTargetView),
                                                   XV_IID(ID3D11DepthStencilView),
                                                   XV_IID(ID3D11UnorderedAccessView),
                                                   XV_IID(ID3D11VertexShader),
                                                   XV_IID(ID3D11PixelShader),
                                                   XV_IID(ID3D11GeometryShader),
                                                   XV_IID(ID3D11HullShader),
                                                   XV_IID(ID3D11DomainShader),
                                                   XV_IID(ID3D11ComputeShader),
                                                   XV_IID(ID3D11InputLayout),
                                                   XV_IID(ID3D11SamplerState),
                                                   XV_IID(ID3D11BlendState),
                                                   XV_IID(ID3D11DepthStencilState),
                                                   XV_IID(ID3D11RasterizerState),
                                                   XV_IID(ID3D11Query),
                                                   XV_IID(ID3D11Predicate),
                                                   XV_IID(ID3D11Counter),
                                                   XV_IID(ID3D11CommandList),
                                                   XV_IID(ID3D11ClassLinkage),
                                                   XV_IID(ID3D11ClassInstance),
                                                   XV_IID(ID3D11Asynchronous),
                                                   XV_IID(ID3D11Debug),
                                                   XV_IID(ID3D11InfoQueue)};
#undef XV_IID
    for (const auto &[name, value] : ids)
        registry.add("d3d11_x.dll", name, value);
    // d3d12_x.dll: the device is the shared Direct3D 12 layer's, the swap chains are ours
    modules::d3d12x::set_adapter_wrapper(&wrap_adapter);
    modules::d3d12x::install(registry, "d3d12_x.dll", Target::durango);
    for (const auto &[name, function] : functions)
        if (std::string_view(name).starts_with("DXGIX") ||
            std::string_view(name) == "D3DMapEsramMemory")
            registry.add("d3d12_x.dll", name, function);
}
} // namespace xv::runtime::durango
