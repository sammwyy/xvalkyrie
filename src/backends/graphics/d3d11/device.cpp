#include "backends/graphics/d3d11/host.hpp"
#include <cstdio>
#include <dxgi1_6.h>

namespace xv::graphics::d3d11 {
HRESULT create_device(UINT flags, bool debug, const D3D_FEATURE_LEVEL *levels, UINT level_count,
                      ID3D11Device1 **device, D3D_FEATURE_LEVEL *level,
                      ID3D11DeviceContext **context, bool &debug_layer) {
    *device = nullptr;
    debug_layer = false;
    IDXGIAdapter1 *adapter = nullptr;
    if (IDXGIFactory6 *factory = nullptr;
        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        IDXGIAdapter1 *candidate = nullptr;
        for (UINT idx = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(
                 idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)));
             ++idx, candidate = nullptr) {
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                break;
            }
            candidate->Release();
        }
        factory->Release();
    }
    const auto driver = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    ID3D11Device *host = nullptr;
    auto result = E_FAIL;
    if (debug) {
        result = D3D11CreateDevice(adapter, driver, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG,
                                   levels, level_count, D3D11_SDK_VERSION, &host, level, context);
        debug_layer = SUCCEEDED(result);
        if (!debug_layer)
            std::fprintf(stderr, "[Runtime] no Direct3D debug layer on this host (0x%08lx)\n",
                         static_cast<unsigned long>(result));
    }
    if (FAILED(result))
        result = D3D11CreateDevice(adapter, driver, nullptr, flags, levels, level_count,
                                   D3D11_SDK_VERSION, &host, level, context);
    if (adapter)
        adapter->Release();
    if (SUCCEEDED(result)) {
        result = host->QueryInterface(device);
        host->Release();
    }
    if (FAILED(result))
        debug_layer = false;
    return result;
}
} // namespace xv::graphics::d3d11
