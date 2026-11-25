#include "backends/graphics/backend.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/console_shader.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include "runtime/modules/memory.hpp"
#include "runtime/modules/placement.hpp"
#include "runtime/modules/xg.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <deque>
#include <dxgi1_6.h>
#include <functional>
#include <intrin.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wrl/implements.h>

// d3d12_x.dll and d3d12_xs.dll on the host's Direct3D 12. Both consoles have the same
// interfaces up to the methods each added last; `console` says which the title was built for.
namespace xv::runtime::modules::d3d12x {
namespace {
Target console = Target::durango;
AdapterWrapper adapter_wrapper = nullptr;
template <class... Interfaces> bool is_any(REFIID iid) {
    return ((iid == __uuidof(Interfaces)) || ...);
}
// The title's first direct queue: the console presents for the device, the host for a queue
std::atomic<ID3D12CommandQueue *> first_direct_queue{nullptr};
// d3d12_x.dll: root signatures are the host's format as well; the device is not there yet
template <class Function> Function host_d3d12(const char *name) {
    static const auto library = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return library ? reinterpret_cast<Function>(GetProcAddress(library, name)) : nullptr;
}
HRESULT WINAPI serialize_root_signature(const void *desc, UINT version, void **blob,
                                        void **errors) {
    using Serialize = HRESULT(WINAPI *)(const void *, UINT, void **, void **);
    static const auto serialize = host_d3d12<Serialize>("D3D12SerializeRootSignature");
    return serialize ? serialize(desc, version, blob, errors) : E_NOTIMPL;
}
HRESULT WINAPI serialize_versioned_root_signature(const void *desc, void **blob, void **errors) {
    using Serialize = HRESULT(WINAPI *)(const void *, void **, void **);
    static const auto serialize = host_d3d12<Serialize>("D3D12SerializeVersionedRootSignature");
    // the console's own flags (0x1000000 and 0x2000000 seen), which the host turns down
    const auto *versioned = static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(desc);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC known{};
    if (versioned && versioned->Desc_1_1.Flags & ~0xfff) {
        known = *versioned;
        known.Desc_1_1.Flags &= static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0xfff);
        versioned = &known;
    }
    ID3DBlob *said = nullptr;
    auto result =
        serialize ? serialize(versioned, blob, reinterpret_cast<void **>(&said)) : E_NOTIMPL;
    // A host without the heaps a shader indexes itself (the flags 0x400 and 0x800, which
    // Windows 10's Direct3D 12 does not know): the signature is made without them, so that
    // the title has one, and shaders that index the heaps are not served by it.
    if (result == E_INVALIDARG && versioned && versioned->Desc_1_1.Flags & 0xc00) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
            std::fprintf(stderr,
                         "[Runtime] the host's Direct3D 12 has no directly indexed descriptor "
                         "heaps (root signature flags 0x%x): made without\n",
                         versioned->Desc_1_1.Flags);
            std::fflush(stderr);
        }
        if (said)
            said->Release();
        said = nullptr;
        known = *versioned;
        known.Desc_1_1.Flags &= static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x3ff);
        versioned = &known;
        result = serialize(versioned, blob, reinterpret_cast<void **>(&said));
    }
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "[Runtime] D3D12SerializeVersionedRootSignature version %d, %u parameters, "
                     "%u static samplers, flags 0x%x: 0x%08lx %s\n",
                     versioned ? versioned->Version : 0,
                     versioned ? versioned->Desc_1_1.NumParameters : 0,
                     versioned ? versioned->Desc_1_1.NumStaticSamplers : 0,
                     versioned ? versioned->Desc_1_1.Flags : 0, static_cast<unsigned long>(result),
                     said ? static_cast<const char *>(said->GetBufferPointer()) : "");
        std::fflush(stderr);
    }
    if (errors)
        *errors = said;
    else if (said)
        said->Release();
    return result;
}
HRESULT WINAPI root_signature_deserializer(const void *blob, SIZE_T size, REFIID iid,
                                           void **reader) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateRootSignatureDeserializer");
    return create ? create(blob, size, iid, reader) : E_NOTIMPL;
}
HRESULT WINAPI versioned_root_signature_deserializer(const void *blob, SIZE_T size, REFIID iid,
                                                     void **reader) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateVersionedRootSignatureDeserializer");
    return create ? create(blob, size, iid, reader) : E_NOTIMPL;
}
HRESULT WINAPI create_device_12(void *adapter, UINT level, REFIID iid, void **device) {
    if (device)
        *device = nullptr;
    std::fprintf(stderr, "[Runtime] D3D12CreateDevice(%p, 0x%x): Direct3D 12.X is not there yet\n",
                 adapter, level);
    std::fflush(stderr);
    return E_NOTIMPL;
}
// The Series console's IDXGIOutput goes on past the desktop's methods with three of its own,
// and a title asks it for the display's modes. The host's output answers the rest.
struct ConsoleMode {
    UINT width, height;
    DXGI_RATIONAL refresh;
    DXGI_FORMAT format;
    DXGI_MODE_SCANLINE_ORDER scanline;
    DXGI_MODE_SCALING scaling;
    UINT flags;
};
// (No virtual destructor: it would take the slot right after the desktop's methods.)
class ConsoleOutput final : public IDXGIOutput {
  public:
    explicit ConsoleOutput(IDXGIOutput *host) : host_(host) {}
    ~ConsoleOutput() { host_->Release(); }
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (!is_any<IUnknown, IDXGIObject, IDXGIOutput>(iid)) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *object = this;
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP SetPrivateData(REFGUID id, UINT size, const void *data) override {
        return host_->SetPrivateData(id, size, data);
    }
    STDMETHODIMP SetPrivateDataInterface(REFGUID id, const IUnknown *object) override {
        return host_->SetPrivateDataInterface(id, object);
    }
    STDMETHODIMP GetPrivateData(REFGUID id, UINT *size, void *data) override {
        return host_->GetPrivateData(id, size, data);
    }
    STDMETHODIMP GetParent(REFIID iid, void **parent) override {
        return host_->GetParent(iid, parent);
    }
    STDMETHODIMP GetDesc(DXGI_OUTPUT_DESC *desc) override { return host_->GetDesc(desc); }
    STDMETHODIMP GetDisplayModeList(DXGI_FORMAT format, UINT flags, UINT *count,
                                    DXGI_MODE_DESC *modes) override {
        return host_->GetDisplayModeList(format, flags, count, modes);
    }
    STDMETHODIMP FindClosestMatchingMode(const DXGI_MODE_DESC *wanted, DXGI_MODE_DESC *closest,
                                         IUnknown *device) override {
        return host_->FindClosestMatchingMode(wanted, closest, device);
    }
    STDMETHODIMP WaitForVBlank() override { return host_->WaitForVBlank(); }
    STDMETHODIMP TakeOwnership(IUnknown *, BOOL) override { return S_OK; }
    STDMETHODIMP_(void) ReleaseOwnership() override {}
    STDMETHODIMP GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES *caps) override {
        return host_->GetGammaControlCapabilities(caps);
    }
    STDMETHODIMP SetGammaControl(const DXGI_GAMMA_CONTROL *) override { return S_OK; }
    STDMETHODIMP GetGammaControl(DXGI_GAMMA_CONTROL *control) override {
        return host_->GetGammaControl(control);
    }
    STDMETHODIMP SetDisplaySurface(IDXGISurface *) override { return S_OK; }
    STDMETHODIMP GetDisplaySurfaceData(IDXGISurface *) override { return E_NOTIMPL; }
    STDMETHODIMP GetFrameStatistics(DXGI_FRAME_STATISTICS *statistics) override {
        return host_->GetFrameStatistics(statistics);
    }
    virtual HRESULT STDMETHODCALLTYPE GetRefreshRate(DXGI_RATIONAL *rate) {
        if (!rate)
            return E_POINTER;
        *rate = {60, 1};
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetAspectRatio(DXGI_RATIONAL *ratio) {
        if (!ratio)
            return E_POINTER;
        *ratio = {16, 9};
        return S_OK;
    }
    // GetDisplayModeListX: the modes a television gives the console, at 60 Hz
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeListX(DXGI_FORMAT format, UINT, UINT *count,
                                                          ConsoleMode *modes) {
        if (!count)
            return E_POINTER;
        static constexpr UINT sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
        if (!modes) {
            *count = static_cast<UINT>(std::size(sizes));
            return S_OK;
        }
        const auto given = std::min<UINT>(*count, static_cast<UINT>(std::size(sizes)));
        for (UINT i = 0; i < given; ++i)
            modes[i] = {sizes[i][0],
                        sizes[i][1],
                        {60, 1},
                        format,
                        DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE,
                        DXGI_MODE_SCALING_UNSPECIFIED,
                        0};
        const bool all = given == std::size(sizes);
        *count = given;
        return all ? S_OK : DXGI_ERROR_MORE_DATA;
    }

  private:
    IDXGIOutput *host_;
    std::atomic<ULONG> references_{1};
};
// the host's adapter, but for the outputs it gives
class ConsoleAdapter final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IDXGIAdapter2, IDXGIAdapter1, IDXGIAdapter>> {
  public:
    // takes over `host`
    static HRESULT make(IDXGIAdapter *host, IDXGIAdapter **adapter) {
        IDXGIAdapter2 *latest = nullptr;
        const auto result = host->QueryInterface(&latest);
        host->Release();
        if (SUCCEEDED(result))
            *adapter = Microsoft::WRL::Make<ConsoleAdapter>(latest).Detach();
        return result;
    }
    explicit ConsoleAdapter(IDXGIAdapter2 *host) : host_(host) {}
    ~ConsoleAdapter() override { host_->Release(); }
    STDMETHODIMP SetPrivateData(REFGUID id, UINT size, const void *data) override {
        return host_->SetPrivateData(id, size, data);
    }
    STDMETHODIMP SetPrivateDataInterface(REFGUID id, const IUnknown *object) override {
        return host_->SetPrivateDataInterface(id, object);
    }
    STDMETHODIMP GetPrivateData(REFGUID id, UINT *size, void *data) override {
        return host_->GetPrivateData(id, size, data);
    }
    STDMETHODIMP GetParent(REFIID iid, void **parent) override {
        return host_->GetParent(iid, parent);
    }
    STDMETHODIMP EnumOutputs(UINT index, IDXGIOutput **output) override {
        if (!output)
            return E_POINTER;
        IDXGIOutput *host = nullptr;
        const auto result = host_->EnumOutputs(index, &host);
        *output = SUCCEEDED(result) ? new ConsoleOutput(host) : nullptr;
        return result;
    }
    STDMETHODIMP GetDesc(DXGI_ADAPTER_DESC *desc) override { return host_->GetDesc(desc); }
    STDMETHODIMP CheckInterfaceSupport(REFGUID name, LARGE_INTEGER *version) override {
        return host_->CheckInterfaceSupport(name, version);
    }
    STDMETHODIMP GetDesc1(DXGI_ADAPTER_DESC1 *desc) override { return host_->GetDesc1(desc); }
    STDMETHODIMP GetDesc2(DXGI_ADAPTER_DESC2 *desc) override { return host_->GetDesc2(desc); }

  private:
    IDXGIAdapter2 *host_;
};
// The console's Direct3D 12 device answers for IDXGIDevice1, which the host's does not: this
// one leads to the adapter the device was made on, and from there to our factory.
class D3D12DxgiDevice final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDXGIDevice1> {
  public:
    explicit D3D12DxgiDevice(LUID luid) : luid_(luid) {}
    STDMETHODIMP GetAdapter(IDXGIAdapter **adapter) override {
        if (!adapter)
            return E_POINTER;
        *adapter = nullptr;
        IDXGIFactory4 *factory = nullptr;
        auto result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(result))
            return result;
        IDXGIAdapter *host = nullptr;
        result = factory->EnumAdapterByLuid(luid_, IID_PPV_ARGS(&host));
        factory->Release();
        if (SUCCEEDED(result) && !adapter_wrapper && console == Target::scarlett)
            return ConsoleAdapter::make(host, adapter);
        if (FAILED(result) || !adapter_wrapper) {
            *adapter = host;
            return result;
        }
        return adapter_wrapper(host, adapter);
    }
    STDMETHODIMP GetParent(REFIID iid, void **parent) override {
        if (!is_any<IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2, IDXGIAdapter3, IDXGIAdapter4>(iid))
            return E_NOINTERFACE;
        return GetAdapter(reinterpret_cast<IDXGIAdapter **>(parent));
    }
    STDMETHODIMP SetPrivateData(REFGUID, UINT, const void *) override { return S_OK; }
    STDMETHODIMP SetPrivateDataInterface(REFGUID, const IUnknown *) override { return S_OK; }
    STDMETHODIMP GetPrivateData(REFGUID, UINT *, void *) override { return DXGI_ERROR_NOT_FOUND; }
    STDMETHODIMP CreateSurface(const DXGI_SURFACE_DESC *, UINT, DXGI_USAGE,
                               const DXGI_SHARED_RESOURCE *, IDXGISurface **) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP QueryResourceResidency(IUnknown *const *, DXGI_RESIDENCY *, UINT) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetGPUThreadPriority(INT) override { return S_OK; }
    STDMETHODIMP GetGPUThreadPriority(INT *priority) override {
        *priority = 0;
        return S_OK;
    }
    STDMETHODIMP SetMaximumFrameLatency(UINT) override { return S_OK; }
    STDMETHODIMP GetMaximumFrameLatency(UINT *latency) override {
        *latency = 3;
        return S_OK;
    }

  private:
    LUID luid_;
};
// The console's Direct3D 12 objects are the host's with a vtable of their own, one slot
// before which is the host's: being the host's objects, they go back to the host's API as
// they are, whichever way the title got hold of them.
using Slot = void *;
Slot *original_vtable(void *object) {
    return static_cast<Slot *>((*static_cast<Slot **>(object))[-1]);
}
template <class Method> Method original(void *object, std::size_t index) {
    return reinterpret_cast<Method>(original_vtable(object)[index]);
}
// The host's runtime calls its own objects through their vtable too: a slot the console has
// differently runs the console's method only when the title calls it, and the host's
// otherwise (its callers pass the object first; the title's, for methods that return a
// structure, second). The console's methods are one table per kind of object.
enum class Family { device, list, heap, queue, resource, allocator, fence };
constexpr std::size_t max_slots = 160;
template <Family Kind> std::array<Slot, max_slots> console_table{};
using Raw = std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t);
// Code that calls as the title's own does, besides the title's: the tests' (see call_as_title),
// a few pieces at most. A free place has neither end; one is filled start first and emptied
// end first, so that a caller never sees a range that is not one.
struct CodeRange {
    std::atomic<const std::uint8_t *> start{nullptr}, end{nullptr};
};
std::array<CodeRange, 4> test_code;
std::mutex test_code_mutex;
bool title_code(const void *address) {
    if (detail::in_title(address))
        return true;
    const auto *const at = static_cast<const std::uint8_t *>(address);
    for (const auto &range : test_code)
        if (at >= range.start.load() && at < range.end.load())
            return true;
    return false;
}
// Whether the call that returns to `address` is the title's own call of this slot: the
// instruction before it is `call [reg + Index * 8]`. The host's runtime also gets here with
// the title's return address, when one of its methods jumps on to another slot.
bool title_called_slot(const void *address, std::size_t index) {
    if (!title_code(address))
        return false;
    const auto *code = static_cast<const std::uint8_t *>(address);
    const auto offset = static_cast<std::uint32_t>(index * 8);
    std::uint32_t disp32;
    std::memcpy(&disp32, code - 4, 4);
    // FF /2 with a 32-bit displacement: FF 90+r, or FF 94 and a SIB byte
    if (disp32 == offset &&
        ((code[-6] == 0xff && (code[-5] & 0xf8) == 0x90) || (code[-7] == 0xff && code[-6] == 0x94)))
        return true;
    // slot 0, no displacement: FF 10+r
    if (offset == 0 && code[-2] == 0xff && (code[-1] & 0xf8) == 0x10)
        return true;
    // with an 8-bit displacement: FF 50+r d8
    if (offset < 0x80 && code[-1] == offset && code[-3] == 0xff && (code[-2] & 0xf8) == 0x50)
        return true;
    // `call reg` (FF D0+r, with a prefix for r8 to r15): the slot was loaded into the register
    // first, with `mov reg, [reg + Index * 8]` a few instructions back. Titles built against
    // the GDK's header call this way.
    if (code[-2] == 0xff && (code[-1] & 0xf8) == 0xd0) {
        for (const auto *at = code - 3; at > code - 160; --at) {
            if (at[0] != 0x8b)
                continue;
            const auto mode = at[1] >> 6;
            const std::size_t skip = (at[1] & 7) == 4 ? 3 : 2; // a SIB byte follows
            std::uint32_t loaded = ~0u;
            if (mode == 2)
                std::memcpy(&loaded, at + skip, 4);
            else if (mode == 1)
                loaded = at[skip];
            else if (mode == 0)
                loaded = 0;
            if (loaded == offset && (mode != 0 || offset == 0))
                return true;
        }
        return false;
    }
    // Another slot's call is the host's runtime going on from one method to the next. A call
    // of a function of the title's own is the title's: the function ends in a jump through
    // the slot (the out-of-line side of the device's inline methods does).
    if ((code[-6] == 0xff && (code[-5] & 0xf8) == 0x90) || (code[-7] == 0xff && code[-6] == 0x94) ||
        (code[-3] == 0xff && (code[-2] & 0xf8) == 0x50) ||
        (code[-2] == 0xff && (code[-1] & 0xf8) == 0x10))
        return false;
    std::int32_t relative;
    std::memcpy(&relative, code - 4, 4);
    return code[-5] == 0xe8 && title_code(code + relative);
}
template <Family Kind, std::size_t Index>
std::uint64_t STDMETHODCALLTYPE by_caller(void *first, std::uint64_t a, std::uint64_t b,
                                          std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                          std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                          std::uint64_t i, std::uint64_t j) {
    const auto method = title_called_slot(_ReturnAddress(), Index) ? console_table<Kind>[Index]
                                                                   : original_vtable(first)[Index];
    return reinterpret_cast<Raw>(method)(first, a, b, c, d, e, f, g, h, i, j);
}
template <Family Kind, std::size_t... Index>
std::array<Slot, max_slots> by_caller_slots(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&by_caller<Kind, Index>)...};
}
template <Family Kind>
const auto dispatchers = by_caller_slots<Kind>(std::make_index_sequence<max_slots>{});
// the console vtables made below, which are memory of ours and not of an image
std::mutex console_vtables_mutex;
std::set<const void *> console_vtables;
bool console_vtable_at(const void *vtable) {
    const std::lock_guard lock(console_vtables_mutex);
    return console_vtables.contains(vtable);
}
// One console vtable per host vtable (one per class of the host's runtime); an object that
// has one already keeps it
template <Family Kind>
void console_vtable(void *object, std::size_t host_slots, std::size_t slots,
                    const std::vector<std::pair<std::size_t, Slot>> &overrides) {
    static std::mutex mutex;
    static std::map<Slot *, Slot *> blocks; // host vtable -> console vtable
    const std::lock_guard lock(mutex);
    auto *const host = *static_cast<Slot **>(object);
    for (const auto &[from, to] : blocks)
        if (to == host)
            return;
    auto &block = blocks[host];
    if (!block) {
        // The host's class has methods of its own after the interface's, which its runtime
        // calls on the object: they are copied as well, as far as its vtable's memory goes.
        std::size_t copied = host_slots;
        if (MEMORY_BASIC_INFORMATION region{}; VirtualQuery(host, &region, sizeof region))
            copied = std::min<std::size_t>(
                max_slots, (static_cast<const std::uint8_t *>(region.BaseAddress) +
                            region.RegionSize - reinterpret_cast<const std::uint8_t *>(host)) /
                               sizeof(Slot));
        copied = std::max(copied, host_slots);
        auto *const memory = new Slot[std::max(slots, copied) + 1]{};
        memory[0] = host;
        std::copy(host, host + copied, memory + 1);
        // a console method where the host has one of its own: by who calls
        for (std::size_t index = host_slots; index < slots; ++index)
            memory[1 + index] = dispatchers<Kind>[index];
        for (const auto &[index, function] : overrides) {
            console_table<Kind>[index] = function;
            memory[1 + index] = dispatchers<Kind>[index];
        }
        block = memory + 1;
        const std::lock_guard known(console_vtables_mutex);
        console_vtables.insert(block);
    }
    *static_cast<Slot **>(object) = block;
}
// Methods that return an 8-byte structure: the console's return it in RAX, where the
// desktop's C++ methods write it through a hidden pointer
template <std::size_t Index> std::uint64_t STDMETHODCALLTYPE returned_in_rax(void *self) {
    std::uint64_t value = 0;
    original<void *(STDMETHODCALLTYPE *)(void *, std::uint64_t *)>(self, Index)(self, &value);
    return value;
}

// The consoles' own DXGI formats, which the host has none of, by the names DirectXTex (MIT)
// gives them in DirectXTexP.h
constexpr auto XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT = static_cast<DXGI_FORMAT>(116);
constexpr auto XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT = static_cast<DXGI_FORMAT>(117);
constexpr auto XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT = static_cast<DXGI_FORMAT>(118);
constexpr auto XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS = static_cast<DXGI_FORMAT>(119);
constexpr auto XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT = static_cast<DXGI_FORMAT>(120);
constexpr auto XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM = static_cast<DXGI_FORMAT>(189);
constexpr auto XBOX_DXGI_FORMAT_R4G4_UNORM = static_cast<DXGI_FORMAT>(190);
// What a format is used as, which decides what the host is given for it
enum class FormatUse {
    resource,
    shader_view, // a shader resource view
    target_view, // a render target or unordered access view
    depth_view   // a depth stencil view
};
bool small_colour(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B5G6R5_UNORM || format == DXGI_FORMAT_B5G5R5A1_UNORM ||
           format == DXGI_FORMAT_B4G4R4A4_UNORM;
}
// Whether the host draws to a 16-bit colour and writes it from shaders, as the console does:
// D3D12_FEATURE_FORMAT_SUPPORT, asked once a format
bool host_draws(ID3D12Device *device, DXGI_FORMAT format) {
    static std::atomic<int> answers[3]{}; // 1 yes, -1 no, 0 not asked yet
    auto &answer = answers[format == DXGI_FORMAT_B5G6R5_UNORM     ? 0
                           : format == DXGI_FORMAT_B5G5R5A1_UNORM ? 1
                                                                  : 2];
    if (const int known = answer.load())
        return known > 0;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    if (!device ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)))
        return false;
    const bool draws = (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) &&
                       (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
                       (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    answer = draws ? 1 : -1;
    return draws;
}
// The host's format for one the title gives. The console's own are the host's nearest: its
// 10-bit floats 16-bit ones, its signed 10-bit colour 16 bits a channel, R4G4 8 bits a
// channel. Its 16-bit depth with 8-bit stencil is the host's 24-bit one, typeless for the
// resource so that views can read either plane. A 16-bit colour drawn to or written from
// shaders is 8 bits a channel where the host does neither with it (see host_draws): `flags`
// say whether a resource is.
DXGI_FORMAT host_format(ID3D12Device *device, DXGI_FORMAT format, FormatUse use,
                        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    // (as a number: the console's own are no values of the host's enumeration)
    switch (static_cast<int>(format)) {
    case XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT:
    case XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT:
    case XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
    case XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT:
        return use == FormatUse::resource     ? DXGI_FORMAT_R24G8_TYPELESS
               : use == FormatUse::depth_view ? DXGI_FORMAT_D24_UNORM_S8_UINT
               : format == XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT
                   ? DXGI_FORMAT_X24_TYPELESS_G8_UINT
                   : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM:
        return DXGI_FORMAT_R16G16B16A16_SNORM;
    case XBOX_DXGI_FORMAT_R4G4_UNORM:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM: {
        const bool drawn =
            use == FormatUse::target_view ||
            (use == FormatUse::resource && (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)));
        return drawn && !host_draws(device, format) ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
    }
    default:
        return format;
    }
}
// a resource's description with the host's format
template <class Desc> void host_resource_format(ID3D12Device *device, Desc &desc) {
    desc.Format = host_format(device, desc.Format, FormatUse::resource, desc.Flags);
}
// a clear value in the format of the host's view the resource is cleared through
const D3D12_CLEAR_VALUE *host_clear_value(ID3D12Device *device, const D3D12_CLEAR_VALUE *clear,
                                          D3D12_RESOURCE_FLAGS flags, D3D12_CLEAR_VALUE &host) {
    if (!clear)
        return nullptr;
    host = *clear;
    host.Format =
        host_format(device, clear->Format,
                    flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ? FormatUse::depth_view
                                                                    : FormatUse::target_view);
    return &host;
}
// Methods that return a larger structure: the console's take the place for it first and the
// object second, the desktop's C++ methods the other way round
D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
resource_allocation_info(D3D12_RESOURCE_ALLOCATION_INFO *info, ID3D12Device *self, UINT mask,
                         UINT count, const D3D12_RESOURCE_DESC *descs) {
    // A Series title's texture that is only read takes what the console's layout of it does,
    // in the swizzle mode it names or the one the console picks, which is how its archives
    // store it: a title may read a streamed texture in pieces of this size, and the console's
    // reader stops on a piece that does not end where one of the archive's does, smaller or
    // larger. What is drawn to the host lays out itself.
    std::uint64_t laid_size = 0, laid_alignment = 1;
    constexpr UINT drawn = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (console == Target::scarlett && descs && count == 1 && !(descs->Flags & drawn) &&
        !series_allocation(descs, laid_size, laid_alignment))
        laid_size = 0;
    const struct Laid {
        D3D12_RESOURCE_ALLOCATION_INFO *info;
        std::uint64_t size;
        ~Laid() {
            if (size && info->SizeInBytes != UINT64_MAX)
                info->SizeInBytes = size;
        }
    } laid{info, laid_size};
    std::vector<D3D12_RESOURCE_DESC> host(descs, descs + (descs ? count : 0));
    for (auto &desc : host) {
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc.Flags & 0xff);
        host_resource_format(self, desc);
        // the console's own layouts (0x100 and up) are the host's to choose
        if (desc.Layout >= 0x100)
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    using Get = D3D12_RESOURCE_ALLOCATION_INFO *(
        STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT,
                             const D3D12_RESOURCE_DESC *);
    const auto get = original<Get>(self, 25);
    get(self, info, mask, count, host.data());
    // an alignment the console takes and the host does not (4 KiB for any texture): the
    // host's own, and the size it has then
    if (info->SizeInBytes == UINT64_MAX) {
        for (auto &desc : host)
            desc.Alignment = 0;
        get(self, info, mask, count, host.data());
        // a texture the host has no such thing as (a format it does not draw to): the
        // size of its plain layout, for a title that sizes its memory by this
        if (info->SizeInBytes == UINT64_MAX && !host.empty()) {
            std::uint64_t total = 0;
            for (const auto &desc : host) {
                const auto bytes =
                    desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                        ? desc.Width
                        : modules::texture_bytes(static_cast<std::uint32_t>(desc.Width),
                                                 desc.Height, desc.MipLevels, desc.DepthOrArraySize,
                                                 static_cast<std::uint32_t>(desc.Format));
                total += (bytes + 0xffff) & ~std::uint64_t{0xffff};
            }
            if (static std::atomic<int> shown{0}; shown++ < 8)
                std::fprintf(stderr,
                             "[Runtime] GetResourceAllocationInfo: the host has no size for "
                             "dimension %d %llux%ux%u mips %u format %d samples %u layout %d "
                             "flags 0x%x: %llu bytes by the plain layout\n",
                             host[0].Dimension, host[0].Width, host[0].Height,
                             host[0].DepthOrArraySize, host[0].MipLevels, host[0].Format,
                             host[0].SampleDesc.Count, host[0].Layout, host[0].Flags, total);
            *info = {total, 0x10000};
        }
    }
    return info;
}
// GetResourceAllocationInfo2 (the Series console's slot 108), with the place for the result
// first as well: the descriptions are D3D12_RESOURCE_DESC1 (72 bytes each, beginning as the
// first kind does), and each resource's offset, alignment and size go to `each`
D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
resource_allocation_info_2(D3D12_RESOURCE_ALLOCATION_INFO *info, ID3D12Device *self, UINT mask,
                           UINT count, const std::uint8_t *descs, std::uint64_t *each) {
    std::vector<D3D12_RESOURCE_DESC> plain(descs ? count : 0);
    for (UINT i = 0; i < plain.size(); ++i)
        std::memcpy(&plain[i], descs + std::size_t{i} * 72, sizeof plain[i]);
    resource_allocation_info(info, self, mask, static_cast<UINT>(plain.size()), plain.data());
    std::uint64_t offset = 0;
    for (UINT i = 0; each && i < plain.size(); ++i) {
        D3D12_RESOURCE_ALLOCATION_INFO one{};
        resource_allocation_info(&one, self, mask, 1, &plain[i]);
        const auto alignment = std::max<std::uint64_t>(one.Alignment, 1);
        offset = (offset + alignment - 1) / alignment * alignment;
        each[3 * i] = offset;
        each[3 * i + 1] = one.Alignment;
        each[3 * i + 2] = one.SizeInBytes;
        offset += one.SizeInBytes;
    }
    return info;
}
D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE custom_heap_properties(D3D12_HEAP_PROPERTIES *properties,
                                                                ID3D12Device *self, UINT node,
                                                                D3D12_HEAP_TYPE type) {
    using Get = D3D12_HEAP_PROPERTIES *(STDMETHODCALLTYPE *)(void *, D3D12_HEAP_PROPERTIES *, UINT,
                                                             D3D12_HEAP_TYPE);
    return original<Get>(self, 26)(self, properties, node, type);
}
// a console method that is a host one at another slot, with the same arguments
template <std::size_t Host>
std::uint64_t STDMETHODCALLTYPE moved_method(void *self, std::uint64_t a, std::uint64_t b,
                                             std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                             std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                             std::uint64_t i, std::uint64_t j) {
    return reinterpret_cast<Raw>(original_vtable(self)[Host])(self, a, b, c, d, e, f, g, h, i, j);
}
// ID3D12Resource as the console has it: the desktop's methods, then GetPlacementDescX and
// GetOpaqueResourceTypeX, which nothing here has
HRESULT STDMETHODCALLTYPE no_console_method(void *) { return E_NOTIMPL; }
void console_resource(void *resource) {
    if (resource && std::getenv("XV_PATCH_RESOURCES"))
        console_vtable<Family::resource>(resource, 15, 17,
                                         {{15, reinterpret_cast<Slot>(&no_console_method)},
                                          {16, reinterpret_cast<Slot>(&no_console_method)}});
}
// Map (8) of a resource the host does not map, and the address of one that has none. All of the
// console's memory is the CPU's as well, and a title maps a texture of the default heap to fill it
// (a video player writes its frames that way, one clears its back buffers). The title gets
// memory of ours, laid out as the host copies the texture's subresources to a buffer, whose
// writes are watched: what it wrote since the last time goes to the texture before a direct
// queue runs the next lists it is given (upload_scratch). Such memory is never given back,
// as the title may go on writing where it was told to.
std::mutex scratch_mutex;
struct Scratch {
    void *memory = nullptr;
    std::uint64_t size = 0;
    ID3D12Resource *upload = nullptr; // of `size` bytes, mapped at `staged`
    void *staged = nullptr;
};
std::map<ID3D12Resource *, Scratch> scratch_maps;
void forget_scratch(void *resource) {
    const std::lock_guard lock(scratch_mutex);
    if (const auto found = scratch_maps.find(static_cast<ID3D12Resource *>(resource));
        found != scratch_maps.end()) {
        if (found->second.upload)
            found->second.upload->Release();
        scratch_maps.erase(found);
    }
}
// the bytes all of a texture's subresources take as the host copies them to a buffer
std::uint64_t copyable_bytes(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc) {
    const UINT count =
        std::max<UINT>(desc.MipLevels, 1) * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                 ? 1
                                                 : std::max<UINT>(desc.DepthOrArraySize, 1));
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, count, 0, nullptr, nullptr, nullptr, &total);
    return total;
}
void *scratch_memory(ID3D12Resource *self) {
    ID3D12Device *device = nullptr;
    if (FAILED(self->GetDevice(IID_PPV_ARGS(&device))))
        return nullptr;
    const auto desc = self->GetDesc();
    auto size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && size != ~0ull)
        size = std::max(size, copyable_bytes(device, desc));
    device->Release();
    if (size == ~0ull)
        return nullptr;
    const std::lock_guard lock(scratch_mutex);
    const bool known = scratch_maps.contains(self);
    auto &scratch = scratch_maps[self];
    if (scratch.size < size) {
        scratch = {VirtualAlloc(nullptr, static_cast<SIZE_T>(size),
                                MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE),
                   size};
        if (!known)
            if (ID3DDestructionNotifier *notifier = nullptr;
                SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&notifier))) && notifier) {
                UINT id = 0;
                notifier->RegisterDestructionCallback([](void *gone) { forget_scratch(gone); },
                                                      self, &id);
                notifier->Release();
            }
        if (static std::atomic<int> shown{0}; shown++ < 4) {
            std::fprintf(stderr,
                         "[Runtime] the title writes a %llux%u texture's memory: what it writes "
                         "there goes to the texture before the next lists a direct queue runs\n",
                         desc.Width, desc.Height);
            std::fflush(stderr);
        }
    }
    return scratch.memory;
}
} // namespace
std::vector<ID3D12Resource *> written_textures() {
    const std::lock_guard lock(scratch_mutex);
    std::vector<ID3D12Resource *> textures;
    for (const auto &[texture, scratch] : scratch_maps)
        if (scratch.memory && texture->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            texture->AddRef();
            textures.push_back(texture);
        }
    return textures;
}
namespace {
// Before a direct queue runs lists: each texture whose memory of ours (scratch_memory) the
// title wrote since the last time gets that memory copied in, on that queue, first.
void upload_scratch(ID3D12CommandQueue *queue) {
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    struct Written {
        ID3D12Resource *texture, *upload;
        std::uint64_t size;
    };
    std::vector<Written> written;
    ID3D12Device *device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return;
    {
        const std::lock_guard lock(scratch_mutex);
        for (auto &[texture, scratch] : scratch_maps) {
            if (!scratch.memory || texture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                continue;
            void *pages[1];
            ULONG_PTR count = 1;
            ULONG granularity = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, scratch.memory,
                              static_cast<SIZE_T>(scratch.size), pages, &count, &granularity) ||
                !count)
                continue;
            ResetWriteWatch(scratch.memory, static_cast<SIZE_T>(scratch.size));
            if (!scratch.upload) {
                const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
                const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                               0,
                                               scratch.size,
                                               1,
                                               1,
                                               1,
                                               DXGI_FORMAT_UNKNOWN,
                                               {1, 0},
                                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                               D3D12_RESOURCE_FLAG_NONE};
                if (FAILED(device->CreateCommittedResource(
                        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                        nullptr, IID_PPV_ARGS(&scratch.upload))) ||
                    FAILED(scratch.upload->Map(0, nullptr, &scratch.staged))) {
                    if (scratch.upload)
                        scratch.upload->Release();
                    scratch.upload = nullptr;
                    continue;
                }
            }
            // what the title has now, which the copy reads when the queue gets to it
            std::memcpy(scratch.staged, scratch.memory, static_cast<std::size_t>(scratch.size));
            texture->AddRef();
            written.push_back({texture, scratch.upload, scratch.size});
        }
    }
    if (written.empty()) {
        device->Release();
        return;
    }
    // one list a time, made again once the queue is done with the one before
    static std::mutex list_mutex;
    static ID3D12CommandAllocator *allocator = nullptr;
    static ID3D12GraphicsCommandList *list = nullptr;
    static ID3D12Fence *fence = nullptr;
    static UINT64 value = 0;
    const std::lock_guard lock(list_mutex);
    if (!list && (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&allocator))) ||
                  FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                   nullptr, IID_PPV_ARGS(&list))) ||
                  FAILED(list->Close()) ||
                  FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))) {
        list = nullptr;
        device->Release();
        return;
    }
    if (fence->GetCompletedValue() < value) {
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 2000);
        CloseHandle(event);
    }
    allocator->Reset();
    list->Reset(allocator, nullptr);
    for (const auto &one : written) {
        const auto desc = one.texture->GetDesc();
        const UINT count = std::max<UINT>(desc.MipLevels, 1) *
                           (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                ? 1
                                : std::max<UINT>(desc.DepthOrArraySize, 1));
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), nullptr, nullptr,
                                      &total);
        if (total > one.size)
            continue;
        const auto state = tracked_state(one.texture, D3D12_RESOURCE_STATE_COMMON);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition = {one.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state,
                              D3D12_RESOURCE_STATE_COPY_DEST};
        if (state != D3D12_RESOURCE_STATE_COPY_DEST)
            list->ResourceBarrier(1, &barrier);
        for (UINT n = 0; n < count; ++n) {
            const D3D12_TEXTURE_COPY_LOCATION to{
                one.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = n}};
            const D3D12_TEXTURE_COPY_LOCATION from{one.upload,
                                                   D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                   {.PlacedFootprint = footprints[n]}};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        if (state != D3D12_RESOURCE_STATE_COPY_DEST)
            list->ResourceBarrier(1, &barrier);
    }
    if (SUCCEEDED(list->Close())) {
        ID3D12CommandList *const lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++value);
    }
    for (const auto &one : written)
        one.texture->Release();
    if (static std::atomic<int> shown{0}; shown++ < 4) {
        std::fprintf(stderr,
                     "[Runtime] %zu textures the title wrote through its memory copied in\n",
                     written.size());
        std::fflush(stderr);
    }
    device->Release();
}
HRESULT STDMETHODCALLTYPE map_resource(ID3D12Resource *self, UINT subresource,
                                       const D3D12_RANGE *range, void **data) {
    using Map = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RANGE *, void **);
    const auto result = original<Map>(self, 8)(self, subresource, range, data);
    if (SUCCEEDED(result) || !data)
        return result;
    *data = scratch_memory(self);
    if (!*data) {
        const auto desc = self->GetDesc();
        std::fprintf(stderr,
                     "[Runtime] Map of subresource %u of a resource of dimension %d %llux%u "
                     "format %d flags 0x%x: 0x%08lx\n",
                     subresource, desc.Dimension, desc.Width, desc.Height, desc.Format, desc.Flags,
                     static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    return *data ? S_OK : result;
}
void STDMETHODCALLTYPE unmap_resource(ID3D12Resource *self, UINT subresource,
                                      const D3D12_RANGE *range) {
    {
        const std::lock_guard lock(scratch_mutex);
        if (scratch_maps.contains(self))
            return;
    }
    using Unmap = void(STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RANGE *);
    original<Unmap>(self, 9)(self, subresource, range);
}
// Textures placed in the title's memory (CreatePlacedResourceX), with the address each is at and
// the description the title gave. On the console a texture's GPU address is its memory: the
// title gives that address back to free the pages it placed a texture in, and views of that
// memory are views of that texture. An entry goes when its texture is destroyed
// (ID3DDestructionNotifier), so nothing here holds a texture.
struct PlacedTexture {
    std::uint64_t address;
    D3D12_RESOURCE_DESC desc;
};
std::mutex placed_texture_mutex;
std::map<const void *, PlacedTexture> placed_textures;
void forget_placed_texture(void *resource) {
    const std::lock_guard lock(placed_texture_mutex);
    placed_textures.erase(resource);
}
void note_placed_texture(ID3D12Resource *resource, std::uint64_t address,
                         const D3D12_RESOURCE_DESC &desc) {
    ID3DDestructionNotifier *notifier = nullptr;
    // without a way to tell when it goes, it is not followed: its pointer may be another's later
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&notifier))) || !notifier)
        return;
    UINT id = 0;
    const auto registered = notifier->RegisterDestructionCallback(
        [](void *gone) { forget_placed_texture(gone); }, resource, &id);
    notifier->Release();
    if (FAILED(registered))
        return;
    const std::lock_guard lock(placed_texture_mutex);
    placed_textures[resource] = {address, desc};
}
bool placed_texture_address(const void *resource, std::uint64_t &address) {
    const std::lock_guard lock(placed_texture_mutex);
    const auto found = placed_textures.find(resource);
    if (found == placed_textures.end())
        return false;
    address = found->second.address;
    return true;
}
// the texture placed at `address` that a title's description `of` describes, with a reference
ID3D12Resource *texture_placed_at(std::uint64_t address, const D3D12_RESOURCE_DESC &of) {
    const std::lock_guard lock(placed_texture_mutex);
    for (const auto &[resource, placed] : placed_textures) {
        const auto &d = placed.desc;
        if (placed.address == address && d.Dimension == of.Dimension && d.Width == of.Width &&
            d.Height == of.Height && d.DepthOrArraySize == of.DepthOrArraySize &&
            d.MipLevels == of.MipLevels && d.Format == of.Format) {
            auto *const texture = static_cast<ID3D12Resource *>(const_cast<void *>(resource));
            texture->AddRef();
            return texture;
        }
    }
    return nullptr;
}
// GetGPUVirtualAddress (11): see track_buffer, and a placed texture's is where it was placed
bool mapped_address(const void *resource, std::uint64_t &address);
std::uint64_t STDMETHODCALLTYPE resource_address(ID3D12Resource *self) {
    if (std::uint64_t address = 0; mapped_address(self, address))
        return address;
    if (std::uint64_t address = 0; placed_texture_address(self, address))
        return address;
    const auto address = original<std::uint64_t(STDMETHODCALLTYPE *)(void *)>(self, 11)(self);
    // the host has no address for a texture; the console's is its memory
    return address ? address : reinterpret_cast<std::uint64_t>(scratch_memory(self));
}
void mappable_resource(void *resource) {
    console_vtable<Family::resource>(resource, 15, 17,
                                     {{8, reinterpret_cast<Slot>(&map_resource)},
                                      {9, reinterpret_cast<Slot>(&unmap_resource)},
                                      {11, reinterpret_cast<Slot>(&resource_address)}});
}
// ID3D12Device::QueryInterface: the console's device is also its DXGI device
HRESULT STDMETHODCALLTYPE device_query(ID3D12Device *self, REFIID iid, void **object) {
    if (object && is_any<IDXGIObject, IDXGIDevice, IDXGIDevice1>(iid)) {
        *object = static_cast<IDXGIDevice1 *>(
            Microsoft::WRL::Make<D3D12DxgiDevice>(self->GetAdapterLuid()).Detach());
        return S_OK;
    }
    // ID3D12Device8 had another interface ID in the kits than the one the desktop shipped with
    static constexpr GUID kit_device8{
        0x728f1fa2, 0xea11, 0x4a9f, {0x9b, 0x52, 0x9b, 0x42, 0xa4, 0x84, 0xd5, 0xc9}};
    const auto query = original<HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **)>(self, 0);
    const auto result = query(self, iid == kit_device8 ? __uuidof(ID3D12Device8) : iid, object);
    // ID3D12Device9 to 11 of the later kits, where the host has no such device: the same
    // object, whose methods past ID3D12Device8's say which they are when called
    static constexpr GUID later_devices[]{
        {0x4c80e962, 0xf032, 0x4f60, {0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c}},
        {0x517f8718, 0xaa66, 0x49f9, {0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x31}},
        {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}}};
    if (result != E_NOINTERFACE || !object ||
        std::none_of(std::begin(later_devices), std::end(later_devices),
                     [&](const GUID &later) { return iid == later; }))
        return result;
    return query(self, __uuidof(ID3D12Device8), object);
}
// CheckFeatureSupport: some of the console's structures are larger than the desktop's
// (D3D12_OPTIONS is 64 bytes there): the host fills its part, the rest is zero
HRESULT STDMETHODCALLTYPE check_feature_support(ID3D12Device *self, D3D12_FEATURE feature,
                                                void *data, UINT size) {
    using Check = HRESULT(STDMETHODCALLTYPE *)(void *, D3D12_FEATURE, void *, UINT);
    const auto check = original<Check>(self, 13);
    // What a format of the console's own can do is what its stand-in on the host can (see
    // host_format): the structures that name one begin with it
    if ((feature == D3D12_FEATURE_FORMAT_SUPPORT || feature == D3D12_FEATURE_FORMAT_INFO ||
         feature == D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS) &&
        data && size >= sizeof(DXGI_FORMAT)) {
        DXGI_FORMAT format;
        std::memcpy(&format, data, sizeof format);
        const auto host =
            host_format(self, format,
                        format == XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT ? FormatUse::depth_view
                                                                     : FormatUse::shader_view);
        if (host != format) {
            std::memcpy(data, &host, sizeof host);
            const auto result = check(self, feature, data, size);
            std::memcpy(data, &format, sizeof format);
            return result;
        }
    }
    auto result = check(self, feature, data, size);
    if (result != E_INVALIDARG || !data)
        return result;
    std::vector<std::uint8_t> input(static_cast<std::uint8_t *>(data),
                                    static_cast<std::uint8_t *>(data) + size);
    for (UINT smaller = size - 4; smaller >= 4 && smaller + 32 >= size; smaller -= 4) {
        std::memcpy(data, input.data(), size);
        if (SUCCEEDED(result = check(self, feature, data, smaller))) {
            std::memset(static_cast<std::uint8_t *>(data) + smaller, 0, size - smaller);
            break;
        }
    }
    return result;
}
// Descriptor heaps as the console has them: a CPU handle is the address of the descriptor,
// below 4 GiB, and titles write descriptors there themselves. Each heap gets such memory of
// its own, a descriptor's worth per host descriptor; the host's descriptor at the same index
// is the one that counts. Descriptors made through the device are the host's already (the
// shadow holds a mark); those the title wrote are hardware descriptors, read when used.
//
// The host's CPU may only write a heap that shaders see: a copy from one is invalid
// (ID3D12Device::CopyDescriptors in Microsoft's documentation), and so is the CPU handle of
// ClearUnorderedAccessView*. Titles copy within the heaps they bind, and so does the decoding
// of what they wrote. So the host's descriptors of a heap shaders see are made in a twin
// heap shaders do not see, and the slots written there since the last submission are copied
// to the heap shaders see before a queue is given work (copy_twins).
struct Twin {
    ID3D12Device *device;
    ID3D12DescriptorHeap *heap; // the twin, which the CPU reads
    D3D12_CPU_DESCRIPTOR_HANDLE visible, start;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    UINT count, step;
    // The title's slots under `limit` are the same ones of the heap shaders see. One the host
    // makes shorter than the title's has a ring of `ring_count` slots from `ring_first` instead
    // for the rest (visible_slot).
    UINT limit, ring_first = 0, ring_count = 0;
    std::atomic<UINT> ring_next{0};
    // a bit for each slot written, and a bit for each word of those that may have one
    std::unique_ptr<std::atomic<std::uint64_t>[]> written, words;
    std::atomic<bool> any{false};
    Twin(ID3D12Device *device, ID3D12DescriptorHeap *heap, D3D12_CPU_DESCRIPTOR_HANDLE visible,
         D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, UINT step)
        : device(device), heap(heap), visible(visible),
          start(heap->GetCPUDescriptorHandleForHeapStart()), type(type), count(count), step(step),
          limit(count), written(new std::atomic<std::uint64_t>[(count + 63) / 64] {}),
          words(new std::atomic<std::uint64_t>[(count + 4095) / 4096] {}) {}
    ~Twin() { heap->Release(); }
    void wrote(std::size_t index, std::size_t slots = 1) {
        for (const auto end = std::min<std::size_t>(index + slots, count); index < end; ++index) {
            written[index / 64].fetch_or(1ull << (index % 64), std::memory_order_relaxed);
            words[index / 4096].fetch_or(1ull << (index / 64 % 64), std::memory_order_release);
        }
        any.store(true, std::memory_order_release);
    }
};
struct ShadowHeap {
    std::uintptr_t base, end;
    D3D12_CPU_DESCRIPTOR_HANDLE host;
    UINT stride;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    ID3D12Device *device;
    bool shader_visible;
    std::uint64_t gpu; // the host's GPU handle of the start, for a shader-visible heap
    UINT host_stride;
    void *owner;                // the heap the title holds
    bool borrowed = false;      // the memory is the title's own, from its allocation hooks
    UINT host_count = ~0u;      // the host's descriptors, where it has fewer than the title
    std::shared_ptr<Twin> twin; // for a heap shaders see: where `host` is
};
// The console's descriptor sizes, which titles also have compiled in: the device's inline
// methods copy descriptors of these sizes themselves
UINT console_stride(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? 32
           : type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV       ? 112
           : type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV       ? 128
                                                          : 16;
}
UINT STDMETHODCALLTYPE descriptor_increment(ID3D12Device *, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return console_stride(type);
}
std::mutex shadow_mutex;
std::map<std::uintptr_t, ShadowHeap> shadow_heaps;         // by base
std::map<void *, std::uintptr_t> shadow_of;                // heap -> base
constexpr std::uint64_t host_mark = 0x5853'4844'4553'4352; // "XSHDESCR"
void *low_memory(std::size_t bytes) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.HighestEndingAddress = reinterpret_cast<void *>(0xffff'ffffull);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return VirtualAlloc2(nullptr, nullptr, (bytes + 0xffff) & ~std::size_t{0xffff},
                         MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, &parameter, 1);
}
const ShadowHeap *shadow_heap(std::uintptr_t address) {
    auto found = shadow_heaps.upper_bound(address);
    if (found == shadow_heaps.begin())
        return nullptr;
    --found;
    return address < found->second.end ? &found->second : nullptr;
}
// A descriptor the host's heap is too short for lands on its last one, which is given up
// for that: the host drops the device over a handle outside its heap. A heap shaders see has
// a twin as long as the title's, where every one has its place.
D3D12_CPU_DESCRIPTOR_HANDLE host_slot(const ShadowHeap &heap, std::uintptr_t at) {
    const auto index = std::min<std::uintptr_t>(
        (at - heap.base) / heap.stride, (heap.twin ? heap.twin->count : heap.host_count) - 1);
    return {heap.host.ptr + index * heap.host_stride};
}
// the host's handle for a console one; others are the host's already
D3D12_CPU_DESCRIPTOR_HANDLE host_handle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr))
        return host_slot(*heap, handle.ptr);
    return handle;
}
// A view of memory of the title's that is in no buffer here is a view of a copy, and the
// title fills such memory after it has made the view (the constants and matrices of a
// draw). What each was made of is kept, by the host's slot, so that the copy can be
// made again when a draw reads it (refresh_memory_view).
struct MemoryView {
    D3D12_CPU_DESCRIPTOR_HANDLE handle{}; // the title's
    bool constants = false;
    D3D12_CONSTANT_BUFFER_VIEW_DESC buffer{};
    std::uint8_t resource[sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) + sizeof(std::uint64_t)]{};
};
std::mutex memory_view_mutex;
std::map<std::uint64_t, MemoryView> memory_views;
// The view of a buffer in each of the host's slots. A shader that reads the heaps itself
// reads a buffer as elements of the size its code has, whatever the view the title made
// says (the console's descriptor is an address): the view is made again as one of such
// elements for the draw (structured_view).
struct BufferView {
    ID3D12Resource *resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc;
};
std::map<std::uint64_t, BufferView> buffer_views; // under memory_view_mutex
// XV_D3D_LOST: what the shader resource view in each of the host's slots was made of
std::map<std::uint64_t, std::string> lost_views; // under memory_view_mutex
// the host's descriptors of `slots` slots from `at` were written: the twin's go to the heap
// shaders see before the next submission
void wrote(const ShadowHeap &heap, std::uintptr_t at, std::size_t slots = 1) {
    if (heap.twin)
        heap.twin->wrote((at - heap.base) / heap.stride, slots);
}
void wrote(D3D12_CPU_DESCRIPTOR_HANDLE handle, std::size_t slots = 1) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr))
        wrote(*heap, handle.ptr, slots);
}
// the same for a descriptor written by the host's own slot, as a view made again for a draw is
void wrote_slot(std::uint64_t slot) {
    const std::lock_guard lock(shadow_mutex);
    for (const auto &[base, heap] : shadow_heaps)
        if (auto *const twin = heap.twin.get();
            twin && slot >= twin->start.ptr &&
            slot < twin->start.ptr + std::uint64_t{twin->count} * twin->step) {
            twin->wrote((slot - twin->start.ptr) / twin->step);
            return;
        }
}
// The twins' slots written since the last time, to the heaps shaders see, in runs
void copy_twins() {
    std::vector<std::shared_ptr<Twin>> twins;
    {
        const std::lock_guard lock(shadow_mutex);
        for (const auto &[base, heap] : shadow_heaps)
            if (heap.twin && heap.twin->any.exchange(false, std::memory_order_acq_rel))
                twins.push_back(heap.twin);
    }
    using Copy = void(STDMETHODCALLTYPE *)(void *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
    for (const auto &twin : twins) {
        const auto copy = original<Copy>(twin->device, 24);
        for (std::size_t group = 0; group < (std::size_t{twin->count} + 4095) / 4096; ++group) {
            auto marked = twin->words[group].exchange(0, std::memory_order_acq_rel);
            while (marked) {
                const auto word = group * 64 + static_cast<unsigned>(std::countr_zero(marked));
                marked &= marked - 1;
                auto bits = twin->written[word].exchange(0, std::memory_order_acq_rel);
                while (bits) {
                    const auto first = static_cast<unsigned>(std::countr_zero(bits));
                    const auto run = static_cast<unsigned>(std::countr_one(bits >> first));
                    const auto index = word * 64 + first;
                    // past `limit` the slots go to the ring as lists read them
                    if (index < twin->limit)
                        copy(twin->device,
                             static_cast<UINT>(std::min<std::size_t>(run, twin->limit - index)),
                             {twin->visible.ptr + index * twin->step},
                             {twin->start.ptr + index * twin->step}, twin->type);
                    bits = run + first >= 64 ? 0 : bits & ~(((1ull << run) - 1) << first);
                }
            }
        }
    }
}
// The mark of a host descriptor, and the host's handle of the slot that holds it: a title
// that copies descriptors itself (the device's inline CopyDescriptorsSimple) carries both
// along, which tells where the host's descriptor of the copy is to come from.
void set_mark(const ShadowHeap &heap, std::uintptr_t at) {
    at = heap.base + (at - heap.base) / heap.stride * heap.stride;
    wrote(heap, at);
    const std::uint64_t words[2]{host_mark, host_slot(heap, at).ptr};
    std::memcpy(reinterpret_cast<void *>(at), words, sizeof words);
}
void mark_host(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    std::uint64_t slot = 0;
    {
        const std::lock_guard lock(shadow_mutex);
        if (const auto *heap = shadow_heap(handle.ptr)) {
            set_mark(*heap, handle.ptr);
            slot = host_slot(*heap, handle.ptr).ptr;
        }
    }
    const std::lock_guard lock(memory_view_mutex);
    memory_views.erase(slot);
    buffer_views.erase(slot);
}
void keep_memory_view(const MemoryView &view) {
    const auto slot = host_handle(view.handle).ptr;
    const std::lock_guard lock(memory_view_mutex);
    if (memory_views.size() > 200'000)
        memory_views.clear();
    memory_views[slot] = view;
}
// whether a host handle is a slot of a heap the title still has
bool live_slot(D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint64_t handle) {
    const std::lock_guard lock(shadow_mutex);
    for (const auto &[base, heap] : shadow_heaps) {
        const auto size = (heap.end - heap.base) / heap.stride * heap.host_stride;
        if (heap.type == type && handle >= heap.host.ptr && handle - heap.host.ptr < size)
            return (handle - heap.host.ptr) % heap.host_stride == 0;
    }
    return false;
}
// A marked descriptor that names another slot is a copy the title made itself: the host's
// descriptor follows it
void resolve(const ShadowHeap &heap, std::uintptr_t at) {
    at = heap.base + (at - heap.base) / heap.stride * heap.stride;
    std::uint64_t words[2];
    std::memcpy(words, reinterpret_cast<const void *>(at), sizeof words);
    const auto own = host_slot(heap, at);
    if (words[0] != host_mark || words[1] == own.ptr || words[1] == 0)
        return;
    // the slot it names may have gone with its heap, or the title may be writing here now
    if (!live_slot(heap.type, words[1]))
        return;
    using Copy = void(STDMETHODCALLTYPE *)(void *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(heap.device, 24)(heap.device, 1, own, D3D12_CPU_DESCRIPTOR_HANDLE{words[1]},
                                    heap.type);
    set_mark(heap, at);
    // what is known of the descriptor goes with it
    const std::lock_guard lock(memory_view_mutex);
    if (const auto found = memory_views.find(words[1]); found != memory_views.end()) {
        auto view = found->second;
        view.handle.ptr = at;
        memory_views[own.ptr] = view;
    } else {
        memory_views.erase(own.ptr);
    }
    if (const auto found = lost_views.find(words[1]); found != lost_views.end())
        lost_views[own.ptr] = found->second;
    if (const auto found = buffer_views.find(words[1]); found != buffer_views.end())
        buffer_views[own.ptr] = found->second;
    else
        buffer_views.erase(own.ptr);
}
// the host's handle for a console one whose descriptor is about to be read
D3D12_CPU_DESCRIPTOR_HANDLE host_view(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    ShadowHeap heap;
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *found = shadow_heap(handle.ptr);
        if (!found)
            return handle;
        heap = *found;
    }
    resolve(heap, handle.ptr);
    return host_slot(heap, handle.ptr);
}
// a constant buffer view of nothing, as the device's inline CreateConstantBufferView writes it
bool null_buffer(const std::uint32_t *words) { return words[2] == 1 && words[3] == 0x0002403c; }
std::uint64_t gpu_address(std::uint64_t address);
// A descriptor the title wrote: made on the host from what the hardware descriptor says.
// Buffers (V#: 48-bit address, stride in bits 48-61, record count in the third dword) become
// constant buffer views.
void decode_written(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    ShadowHeap found{};
    {
        const std::lock_guard lock(shadow_mutex);
        if (const auto *const live = shadow_heap(handle.ptr))
            found = *live;
    }
    const auto *const heap = found.base ? &found : nullptr;
    if (!heap || heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        return;
    std::uint32_t words[4];
    std::memcpy(words, reinterpret_cast<const void *>(handle.ptr), sizeof words);
    std::uint64_t first;
    std::memcpy(&first, words, sizeof first);
    if (first == host_mark)
        resolve(*heap, handle.ptr);
    if (first == host_mark || first == 0 || null_buffer(words))
        return;
    // what kinds of descriptors titles write, by the type in the last dword (buffers 0, images
    // 8 and up), once each
    if (static std::atomic<std::uint32_t> seen{0}; std::getenv("XV_D3D_TRACE")) {
        const std::uint32_t bit = 1u << (words[3] >> 28);
        if (!(seen.fetch_or(bit) & bit))
            std::fprintf(stderr, "[Runtime] descriptor written by the title: %08x %08x %08x %08x\n",
                         words[0], words[1], words[2], words[3]);
    }
    const std::uint64_t address = first & 0xffff'ffff'ffffull;
    const std::uint32_t stride = (words[1] >> 16) & 0x3fff;
    const std::uint64_t size = std::uint64_t{words[2]} * (stride ? stride : 1);
    D3D12_CONSTANT_BUFFER_VIEW_DESC view{
        gpu_address(address),
        static_cast<UINT>(std::min<std::uint64_t>((size + 255) & ~255ull, 65536))};
    // memory the host's GPU cannot see: a view of nothing
    if (!view.BufferLocation)
        view = {};
    using Create = void(STDMETHODCALLTYPE *)(void *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
    original<Create>(heap->device, 17)(heap->device, &view, host_handle(handle));
    wrote(*heap, handle.ptr);
}
// A title of the later kits exports __XMemAllocationHooks__ (allocate, free): the console's
// Direct3D takes a descriptor heap's memory through them, as an object of type 4 (the low
// 14 bits of the attributes) of the allocator 128, Direct3D's (bits 16 to 23), and such a
// title may answer with memory it laid out itself and expect the heap to be there.
struct AllocationHooks {
    void *(WINAPI *allocate)(SIZE_T, ULONGLONG);
    void(WINAPI *free)(void *, ULONGLONG);
};
constexpr ULONGLONG descriptor_heap_object = 4 | 128ull << 16;
const AllocationHooks *allocation_hooks(const void *caller) {
    static const AllocationHooks *const hooks = [caller]() -> const AllocationHooks * {
        const auto *const base =
            static_cast<const std::uint8_t *>(detail::title_image_base(caller));
        if (!base)
            return nullptr;
        const auto *const dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *const nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!directory.VirtualAddress || !directory.Size)
            return nullptr;
        const auto *const exports =
            reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + directory.VirtualAddress);
        const auto *const names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
        const auto *const ordinals =
            reinterpret_cast<const WORD *>(base + exports->AddressOfNameOrdinals);
        const auto *const functions =
            reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i)
            if (!std::strcmp(reinterpret_cast<const char *>(base + names[i]),
                             "__XMemAllocationHooks__"))
                return reinterpret_cast<const AllocationHooks *>(base + functions[ordinals[i]]);
        return nullptr;
    }();
    return hooks && hooks->allocate && hooks->free ? hooks : nullptr;
}
// the title's memory for a heap of `bytes`, if it has a place of its own for it: pages it
// mapped itself. Anything else it gives is given back.
void *title_heap_memory(const void *caller, std::size_t bytes) {
    const auto *const hooks = allocation_hooks(caller);
    if (!hooks)
        return nullptr;
    auto *const memory = hooks->allocate(bytes, descriptor_heap_object);
    MEMORY_BASIC_INFORMATION info{};
    if (std::getenv("XV_D3D_TRACE")) {
        VirtualQuery(memory, &info, sizeof info);
        std::fprintf(
            stderr,
            "[Runtime] descriptor heap of %zu bytes: the title gives %p (type 0x%lx state 0x%lx)%c",
            bytes, memory, info.Type, info.State, 10);
        std::fflush(stderr);
    }
    if (memory && VirtualQuery(memory, &info, sizeof info) && info.Type == MEM_MAPPED &&
        info.State == MEM_COMMIT)
        return memory;
    if (memory)
        hooks->free(memory, descriptor_heap_object);
    return nullptr;
}
void register_shadow(ID3D12Device *device, void *heap, const D3D12_DESCRIPTOR_HEAP_DESC &desc,
                     D3D12_CPU_DESCRIPTOR_HANDLE host, std::uint64_t gpu,
                     void *title_memory = nullptr, UINT host_count = ~0u) {
    const UINT stride = console_stride(desc.Type);
    auto *const memory =
        title_memory ? title_memory : low_memory(std::size_t{desc.NumDescriptors} * stride);
    if (!memory) {
        std::fprintf(stderr, "[Runtime] no memory below 4 GiB for a descriptor heap\n");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    const bool visible = (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
    const UINT host_stride = device->GetDescriptorHandleIncrementSize(desc.Type);
    // the twin of a heap shaders see; without one the host's descriptors are made in it
    std::shared_ptr<Twin> twin;
    if (visible) {
        D3D12_DESCRIPTOR_HEAP_DESC twin_desc = desc;
        twin_desc.Flags &= ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        void *made = nullptr;
        using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *,
                                                    REFIID, void **);
        if (SUCCEEDED(original<Create>(device, 14)(device, &twin_desc,
                                                   __uuidof(ID3D12DescriptorHeap), &made)) &&
            made) {
            twin = std::make_shared<Twin>(device, static_cast<ID3D12DescriptorHeap *>(made), host,
                                          desc.Type, twin_desc.NumDescriptors, host_stride);
            host = twin->start;
            // an eighth of the host's heap, 128 Ki slots at most, for the ring: a title that
            // goes round all of its heap in turn uses a few tables of the far part a frame
            if (host_count < desc.NumDescriptors) {
                twin->ring_count = std::min(host_count / 8, 131072u);
                twin->limit = twin->ring_first = host_count - twin->ring_count;
            }
        }
    }
    const std::lock_guard lock(shadow_mutex);
    shadow_heaps[base] = {base,
                          base + std::size_t{desc.NumDescriptors} * stride,
                          host,
                          stride,
                          desc.Type,
                          device,
                          visible,
                          gpu,
                          host_stride,
                          heap,
                          title_memory != nullptr,
                          host_count,
                          std::move(twin)};
    shadow_of[heap] = base;
}
// Before the GPU runs: what the title wrote straight into a shader-visible heap since last
// time, read from the pages written to
void decode_written_heaps() {
    std::vector<ShadowHeap> heaps;
    {
        const std::lock_guard lock(shadow_mutex);
        for (const auto &[base, heap] : shadow_heaps)
            if (heap.shader_visible && heap.type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                heaps.push_back(heap);
    }
    std::vector<void *> pages(4096);
    for (const auto &heap : heaps) {
        ULONG_PTR count = pages.size();
        ULONG size = 0;
        const auto bytes = ((heap.end - heap.base) + 0xffff) & ~std::uintptr_t{0xffff};
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void *>(heap.base), bytes,
                          pages.data(), &count, &size) != 0)
            continue;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const auto page = reinterpret_cast<std::uintptr_t>(pages[i]);
            const auto first = std::max(heap.base, page);
            const auto last = std::min(heap.end, page + size);
            for (auto at =
                     heap.base + (first - heap.base + heap.stride - 1) / heap.stride * heap.stride;
                 at + heap.stride <= last; at += heap.stride)
                decode_written({at});
        }
    }
}
// ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart: the shadow's start
// The title's descriptor heap: a proxy of the host's, which the host's runtime keeps as its
// own (it knows its objects by their vtable). The proxy's handles are the console's: the CPU
// one the shadow's start, both in RAX.
struct HeapProxy {
    Slot *vtable;
    ID3D12DescriptorHeap *host;
    std::atomic<ULONG> references;
};
std::mutex heap_proxy_mutex;
std::atomic<unsigned> shadows_released{0};
std::set<const void *> heap_proxies;
ID3D12DescriptorHeap *host_heap(void *heap) {
    const std::lock_guard lock(heap_proxy_mutex);
    return heap_proxies.contains(heap) ? static_cast<HeapProxy *>(heap)->host
                                       : static_cast<ID3D12DescriptorHeap *>(heap);
}
std::uint64_t STDMETHODCALLTYPE cpu_heap_start(void *self) {
    const std::lock_guard lock(shadow_mutex);
    const auto found = shadow_of.find(self);
    return found != shadow_of.end()
               ? found->second
               : static_cast<HeapProxy *>(self)->host->GetCPUDescriptorHandleForHeapStart().ptr;
}
// the console's GPU handles are its CPU ones: the shadow's start as well
std::uint64_t STDMETHODCALLTYPE gpu_heap_start(void *self) {
    {
        const std::lock_guard lock(shadow_mutex);
        const auto found = shadow_of.find(self);
        if (found != shadow_of.end() && shadow_heaps[found->second].shader_visible)
            return found->second;
    }
    return static_cast<HeapProxy *>(self)->host->GetGPUDescriptorHandleForHeapStart().ptr;
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE heap_method(void *self, std::uint64_t a, std::uint64_t b,
                                            std::uint64_t c, std::uint64_t d) {
    auto *const host = static_cast<HeapProxy *>(self)->host;
    return reinterpret_cast<std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t,
                                                               std::uint64_t, std::uint64_t)>(
        (*reinterpret_cast<Slot **>(host))[Index])(host, a, b, c, d);
}
HRESULT STDMETHODCALLTYPE heap_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    if (is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12DescriptorHeap>(
            iid)) {
        ++static_cast<HeapProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return static_cast<HeapProxy *>(self)->host->QueryInterface(iid, object);
}
ULONG STDMETHODCALLTYPE heap_add_ref(void *self) {
    return ++static_cast<HeapProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE heap_release(void *self) {
    auto *const proxy = static_cast<HeapProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        {
            const std::lock_guard lock(heap_proxy_mutex);
            heap_proxies.erase(proxy);
        }
        // its shadow goes with it: the host's descriptors it stood for are no more
        {
            const std::lock_guard lock(shadow_mutex);
            if (const auto found = shadow_of.find(proxy); found != shadow_of.end()) {
                if (!shadow_heaps[found->second].borrowed)
                    VirtualFree(reinterpret_cast<void *>(found->second), 0, MEM_RELEASE);
                shadow_heaps.erase(found->second);
                shadow_of.erase(found);
                ++shadows_released;
            }
        }
        proxy->host->Release();
        delete proxy;
    }
    return left;
}
template <std::size_t... Index> std::array<Slot, 11> heap_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, 11> vtable{reinterpret_cast<Slot>(&heap_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&heap_query);
    vtable[1] = reinterpret_cast<Slot>(&heap_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&heap_release);
    vtable[9] = reinterpret_cast<Slot>(&cpu_heap_start);
    vtable[10] = reinterpret_cast<Slot>(&gpu_heap_start);
    return vtable;
}
std::array<Slot, 11> heap_vtable = heap_vtable_of(std::make_index_sequence<11>{});
// whether the title has the later kits' layouts of queues and lists (see queue_by_layout)
std::atomic<bool> later_queue{false};
// The buffers the title has, by GPU address: a view of one names the resource, where a
// hardware descriptor names an address
struct BufferRange {
    std::uint64_t end;
    ID3D12Resource *resource;
};
std::mutex buffer_mutex;
std::map<std::uint64_t, BufferRange> buffers_at; // by start
// On the Series console a buffer's GPU address is where its memory is for the CPU as well,
// and a title writes there without mapping anything. Such a buffer is kept mapped here and
// the title is given that address as its GPU address; an address that comes back from the
// title is turned into the host's wherever the host takes one.
struct MappedBuffer {
    std::uint64_t end, gpu;
};
std::map<std::uint64_t, MappedBuffer> mapped_at;    // by the address the title has
std::map<const void *, std::uint64_t> mapped_start; // by resource
bool mapped_address(const void *resource, std::uint64_t &address) {
    const std::lock_guard lock(buffer_mutex);
    const auto found = mapped_start.find(resource);
    if (found == mapped_start.end())
        return false;
    address = found->second;
    return true;
}
// The host's GPU address for one the title gives: in a buffer here, the host's address there.
// Memory of the title's that no buffer here holds is 0, which callers turn into a view of
// nothing: the host's GPU cannot see that memory, and given its CPU address it removes the
// device. Any other address goes on as it is.
std::uint64_t gpu_address(std::uint64_t address) {
    if (!address)
        return 0;
    {
        const std::lock_guard lock(buffer_mutex);
        if (const auto found = mapped_at.upper_bound(address);
            found != mapped_at.begin() && address < std::prev(found)->second.end)
            return std::prev(found)->second.gpu + (address - std::prev(found)->first);
        if (const auto found = buffers_at.upper_bound(address);
            found != buffers_at.begin() && address < std::prev(found)->second.end)
            return address;
    }
    MEMORY_BASIC_INFORMATION info{};
    const bool title_memory = VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                              info.State == MEM_COMMIT;
    return title_memory ? 0 : address;
}
void track_buffer(ID3D12Resource *resource, bool mapped = false) {
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        // made where a released buffer was: that one's mapped address is not this one's
        const std::lock_guard lock(buffer_mutex);
        mapped_start.erase(resource);
        return;
    }
    auto start = resource->GetGPUVirtualAddress();
    void *memory = nullptr;
    const D3D12_RANGE nothing_read{0, 0};
    const bool kept = mapped && SUCCEEDED(resource->Map(0, &nothing_read, &memory)) && memory;
    const std::lock_guard lock(buffer_mutex);
    if (kept) {
        const auto address = reinterpret_cast<std::uint64_t>(memory);
        // what was mapped here before is gone
        for (auto old = mapped_at.lower_bound(address);
             old != mapped_at.end() && old->first < address + desc.Width;)
            old = mapped_at.erase(old);
        mapped_at[address] = {address + desc.Width, start};
        mapped_start[resource] = address;
        start = address;
    } else {
        mapped_start.erase(resource);
    }
    buffers_at[start] = {start + desc.Width, resource};
}
bool buffer_at(std::uint64_t address, ID3D12Resource *&resource, std::uint64_t &offset,
               std::uint64_t &size) {
    const std::lock_guard lock(buffer_mutex);
    auto found = buffers_at.upper_bound(address);
    if (found == buffers_at.begin())
        return false;
    --found;
    if (address >= found->second.end)
        return false;
    resource = found->second.resource;
    offset = address - found->first;
    size = found->second.end - address;
    return true;
}

// An address the title gives for a vertex or index buffer that no buffer here answers for:
// worth a line, since the host's GPU reads nothing of the title's memory (see gpu_address)
// and may fault on any other address
void check_address(const char *what, std::uint64_t address, std::uint64_t bytes) {
    if (!address)
        return;
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, size = 0;
    {
        const std::lock_guard lock(buffer_mutex);
        auto found = mapped_at.upper_bound(address);
        if (found != mapped_at.begin() && address < std::prev(found)->second.end)
            return;
    }
    if (buffer_at(address, resource, offset, size) && size >= bytes)
        return;
    if (static std::atomic<int> shown{0}; shown++ < 8) {
        MEMORY_BASIC_INFORMATION info{};
        const bool cpu = VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                         info.State == MEM_COMMIT;
        std::fprintf(stderr, "[Runtime] %s at %llx (%llu bytes) is in no buffer known here%s\n",
                     what, static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(bytes),
                     cpu ? ": it is memory of the title's" : "");
        std::fflush(stderr);
    }
}
// The descriptor tables of each root signature: the type of the descriptors at each offset
// from a table's start, for the descriptors the title writes itself
struct TableRange {
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    UINT offset, count;
};
std::mutex signature_mutex;
std::map<const void *, std::vector<std::vector<TableRange>>> tables_of; // per parameter
std::map<const void *, std::string> signature_text; // XV_D3D_LOST: its parameters, in words
void read_root_signature(const void *signature, const void *blob, SIZE_T size) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateVersionedRootSignatureDeserializer");
    ID3D12VersionedRootSignatureDeserializer *reader = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc = nullptr;
    if (!create || FAILED(create(blob, size, IID_PPV_ARGS(&reader))) ||
        FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc))) {
        if (reader)
            reader->Release();
        return;
    }
    std::vector<std::vector<TableRange>> tables(desc->Desc_1_1.NumParameters);
    for (UINT i = 0; i < desc->Desc_1_1.NumParameters; ++i) {
        const auto &parameter = desc->Desc_1_1.pParameters[i];
        if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;
        UINT next = 0;
        for (UINT r = 0; r < parameter.DescriptorTable.NumDescriptorRanges; ++r) {
            const auto &range = parameter.DescriptorTable.pDescriptorRanges[r];
            const UINT offset =
                range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                    ? next
                    : range.OffsetInDescriptorsFromTableStart;
            // an unbounded range: as many as a table here reads at most
            const UINT count = range.NumDescriptors == UINT_MAX ? 64 : range.NumDescriptors;
            tables[i].push_back({range.RangeType, offset, count});
            next = offset + count;
        }
    }
    std::string text = "flags " + std::to_string(desc->Desc_1_1.Flags) + ", " +
                       std::to_string(desc->Desc_1_1.NumStaticSamplers) + " static samplers";
    for (UINT i = 0; i < desc->Desc_1_1.NumParameters; ++i) {
        const auto &parameter = desc->Desc_1_1.pParameters[i];
        static const char *const kinds[] = {"table", "constants", "CBV", "SRV", "UAV"};
        text += std::string("; ") +
                (parameter.ParameterType < 5 ? kinds[parameter.ParameterType] : "?");
        if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            for (UINT r = 0; r < parameter.DescriptorTable.NumDescriptorRanges; ++r) {
                const auto &range = parameter.DescriptorTable.pDescriptorRanges[r];
                text += " [type " + std::to_string(range.RangeType) + " x" +
                        std::to_string(static_cast<int>(range.NumDescriptors)) + " r" +
                        std::to_string(range.BaseShaderRegister) + " space " +
                        std::to_string(range.RegisterSpace) + " flags " +
                        std::to_string(range.Flags) + "]";
            }
        else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
            text += " r" + std::to_string(parameter.Constants.ShaderRegister) + " x" +
                    std::to_string(parameter.Constants.Num32BitValues);
        else
            text += " r" + std::to_string(parameter.Descriptor.ShaderRegister) + " space " +
                    std::to_string(parameter.Descriptor.RegisterSpace) + " flags " +
                    std::to_string(parameter.Descriptor.Flags);
        text += " visibility " + std::to_string(parameter.ShaderVisibility);
    }
    reader->Release();
    const std::lock_guard lock(signature_mutex);
    signature_text[signature] = std::move(text);
    tables_of[signature] = std::move(tables);
}

UINT element_bytes(DXGI_FORMAT format);
// The bytes of an element of a buffer view: its structure's, a word for a raw one, its
// format's otherwise; 0 for a format a buffer has no elements of
template <class View> UINT view_element(const View &view) {
    // D3D12_BUFFER_SRV_FLAG_RAW and D3D12_BUFFER_UAV_FLAG_RAW
    return view.Buffer.StructureByteStride            ? view.Buffer.StructureByteStride
           : static_cast<UINT>(view.Buffer.Flags) & 1 ? 4
                                                      : element_bytes(view.Format);
}
// A buffer view reaches no further than the end of its buffer: the host drops the device over
// one that does, or over one of no elements, where the console's view goes on into the memory
// after the buffer. What is past the end reads 0 and takes no writes. False when nothing of
// the view is in the buffer.
template <class View> bool fit_buffer_view(ID3D12Resource *resource, View &view) {
    const std::uint64_t element = view_element(view);
    if (!resource || !element)
        return true;
    const auto elements = resource->GetDesc().Width / element;
    if (view.Buffer.FirstElement >= elements)
        return false;
    view.Buffer.NumElements = static_cast<UINT>(
        std::min<std::uint64_t>(view.Buffer.NumElements, elements - view.Buffer.FirstElement));
    return view.Buffer.NumElements != 0;
}
// The host takes a raw view only from a multiple of 16 bytes into its buffer (FirstElement a
// multiple of 4), and drops the device over one from anywhere else; the console's starts at
// any word.
template <class View> bool raw_off_16(const View &view) {
    return static_cast<UINT>(view.Buffer.Flags) & 1 && view.Buffer.FirstElement % 4;
}
std::uint64_t ring_copy(const void *source, std::uint64_t bytes, std::uint64_t most = 64 << 10,
                        std::uint64_t align = 0, ID3D12Resource **buffer = nullptr,
                        std::uint64_t *offset = nullptr);
// A raw view to read that the host cannot have where it is: what it is of is copied, as it
// is now, to 16 bytes into the ring of copies, and the view is of the copy, as constants that
// do not start on 256 bytes are (see create_cbv). `address` becomes the title's address of
// its first word, from which the copy is made again when the view is (refresh_memory_view).
// False when the CPU cannot read the buffer, or the view is larger than a copy made each time
// it is made may be (as for views of memory in no buffer, see create_srv_at).
bool copy_raw_view(ID3D12Resource *&resource, D3D12_SHADER_RESOURCE_VIEW_DESC &view,
                   std::uint64_t &address) {
    const auto start = view.Buffer.FirstElement * 4;
    const auto bytes = std::uint64_t{view.Buffer.NumElements} * 4;
    if (bytes > (16ull << 20))
        return false;
    // the buffer's memory as the CPU has it: where it is kept mapped, or mapped now
    std::uint64_t kept = 0;
    const bool mapped = mapped_address(resource, kept);
    void *memory = reinterpret_cast<void *>(kept);
    const D3D12_RANGE read{static_cast<SIZE_T>(start), static_cast<SIZE_T>(start + bytes)};
    if (!mapped && (FAILED(resource->Map(0, &read, &memory)) || !memory))
        return false;
    ID3D12Resource *ring = nullptr;
    std::uint64_t offset = 0;
    const auto copy = ring_copy(static_cast<const std::uint8_t *>(memory) + start, bytes, bytes, 16,
                                &ring, &offset);
    if (!mapped) {
        const D3D12_RANGE nothing_written{0, 0};
        resource->Unmap(0, &nothing_written);
    }
    if (!copy)
        return false;
    address = (mapped ? kept : resource->GetGPUVirtualAddress()) + start;
    resource = ring;
    view.Buffer.FirstElement = offset / 4;
    return true;
}
// what a raw view copied by copy_raw_view is of, kept to make the copy again
void keep_raw_copy(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_SHADER_RESOURCE_VIEW_DESC view,
                   std::uint64_t address) {
    MemoryView kept;
    kept.handle = handle;
    view.Buffer.FirstElement = 0;
    std::memcpy(kept.resource, &view, sizeof view);
    std::memcpy(kept.resource + sizeof view, &address, sizeof address);
    keep_memory_view(kept);
}
// A descriptor the title wrote, made on the host as what the table it is bound in says it is.
// A buffer descriptor (V#) has a 48-bit address, the stride in bits 48-61 and the record
// count in its third dword.
std::atomic<std::uint64_t> decoded[4];
// What decode_as made a view of nothing of, by where it is: its words and the type it was
// read as, so that a table bound again does not have it made again while it is the same
struct Unmarked {
    std::array<std::uint32_t, 4> words;
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    bool operator==(const Unmarked &) const = default;
};
std::mutex unmarked_mutex;
std::unordered_map<std::uintptr_t, Unmarked> unmarked;
void decode_as(const ShadowHeap &heap, std::uintptr_t at, D3D12_DESCRIPTOR_RANGE_TYPE type) {
    std::uint32_t words[4];
    std::memcpy(words, reinterpret_cast<const void *>(at), sizeof words);
    std::uint64_t first;
    std::memcpy(&first, words, sizeof first);
    if (first == host_mark) {
        resolve(heap, at);
        return;
    }
    // what was decoded to nothing here before and is still the same is that still
    const Unmarked seen{{words[0], words[1], words[2], words[3]}, type};
    {
        const std::lock_guard lock(unmarked_mutex);
        if (const auto found = unmarked.find(at); found != unmarked.end() && found->second == seen)
            return;
    }
    const auto left_unmarked = [&] {
        const std::lock_guard lock(unmarked_mutex);
        if (unmarked.size() > 1'000'000)
            unmarked.clear();
        unmarked[at] = seen;
    };
    ++decoded[type & 3];
    if (static std::atomic<std::uint32_t> shown{0};
        std::getenv("XV_D3D_TRACE") && first != 0 && !(shown.fetch_or(1u << type) & (1u << type))) {
        std::uint32_t more[4]{};
        if (heap.stride >= 32)
            std::memcpy(more, reinterpret_cast<const void *>(at + 16), sizeof more);
        std::fprintf(stderr,
                     "[Runtime] title %s descriptor: %08x %08x %08x %08x | %08x %08x %08x %08x"
                     "\n",
                     type == 0   ? "SRV"
                     : type == 1 ? "UAV"
                     : type == 2 ? "CBV"
                                 : "sampler",
                     words[0], words[1], words[2], words[3], more[0], more[1], more[2], more[3]);
        std::fflush(stderr);
    }
    // A sampler the title wrote (S#): the hardware's four words of sampler state. Clamp
    // modes in bits 0 to 8 of the first, the comparison in 12 to 14, the anisotropy in 9 to
    // 11; the LOD range in the second (4.8 fixed point each); the bias in the low 14 bits of
    // the third (signed 5.8), then the filters; the border colour's kind in the top of the
    // last. A sampler slot the host is left without hangs the GPU when it is sampled with.
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
        static constexpr D3D12_TEXTURE_ADDRESS_MODE modes[] = {
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,   D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE};
        const auto magnify = (words[2] >> 20) & 3, minify = (words[2] >> 22) & 3,
                   mip = (words[2] >> 26) & 3, compare = (words[0] >> 12) & 7,
                   ratio = (words[0] >> 9) & 7, border = words[3] >> 30;
        const bool anisotropic = magnify >= 2 || minify >= 2;
        const bool compares = compare != 0 && compare != 7;
        D3D12_SAMPLER_DESC desc{};
        desc.Filter = static_cast<D3D12_FILTER>(
            (anisotropic ? 0x55u : (minify & 1) << 4 | (magnify & 1) << 2 | (mip == 2 ? 1u : 0u)) |
            (compares ? 0x80u : 0u));
        desc.AddressU = modes[words[0] & 7];
        desc.AddressV = modes[(words[0] >> 3) & 7];
        desc.AddressW = modes[(words[0] >> 6) & 7];
        desc.MipLODBias =
            static_cast<float>(static_cast<std::int32_t>(words[2] << 18) >> 18) / 256.0f;
        desc.MaxAnisotropy = anisotropic ? 1u << ratio : 1;
        desc.ComparisonFunc = compares ? static_cast<D3D12_COMPARISON_FUNC>(compare + 1)
                                       : D3D12_COMPARISON_FUNC_NEVER;
        const float colour = border == 2 ? 1.0f : 0.0f;
        desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = colour;
        desc.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
        desc.MinLOD = static_cast<float>(words[1] & 0xfff) / 256.0f;
        desc.MaxLOD = mip ? static_cast<float>((words[1] >> 12) & 0xfff) / 256.0f : desc.MinLOD;
        auto *const device = heap.device;
        original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
            device, 22)(device, &desc, host_slot(heap, at));
        set_mark(heap, at);
        return;
    }
    const auto host = host_slot(heap, at);
    auto *const device = heap.device;
    // the title wrote another descriptor here: a copy kept for what was here is not made again
    {
        const std::lock_guard lock(memory_view_mutex);
        memory_views.erase(host.ptr);
    }
    // a view of nothing, of a buffer or a texture
    const auto view_nothing = [&](bool buffer) {
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
            const D3D12_CONSTANT_BUFFER_VIEW_DESC view{};
            original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
                device, 17)(device, &view, host);
        } else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = buffer ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension =
                buffer ? D3D12_SRV_DIMENSION_BUFFER : D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (!buffer)
                view.Texture2D.MipLevels = 1;
            original<void(STDMETHODCALLTYPE *)(void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 18)(
                device, nullptr, &view, host);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_UINT;
            view.ViewDimension =
                buffer ? D3D12_UAV_DIMENSION_BUFFER : D3D12_UAV_DIMENSION_TEXTURE2D;
            original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 19)(
                device, nullptr, nullptr, &view, host);
        }
    };
    // a descriptor the title cleared is a null one, whose reads give 0: the host's slot may
    // hold anything until it is made so. What decodes to nothing is left unmarked, as it may
    // be no descriptor at all: a range of the title's signature runs past the descriptors it
    // wrote, into what it keeps after them (a draw's constants right after its
    // table), which the mark would overwrite. It is decoded again when it is read again.
    if ((first == 0 && words[2] == 0 && words[3] == 0) || null_buffer(words)) {
        view_nothing(false);
        left_unmarked();
        return;
    }
    const std::uint64_t address = first & 0xffff'ffff'ffffull;
    const UINT stride = (words[1] >> 16) & 0x3fff;
    const std::uint64_t bytes = std::uint64_t{words[2]} * (stride ? stride : 1);
    // a buffer descriptor (type 0 in the last dword's top bits) of memory no buffer here
    // holds views nothing (see gpu_address)
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, room = 0;
    if (!buffer_at(address, resource, offset, room)) {
        if (type != D3D12_DESCRIPTOR_RANGE_TYPE_CBV && words[3] >> 28 != 0)
            return;
        view_nothing(true);
        left_unmarked();
        return;
    }
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
        // the address is the one the title has for the buffer, which on Series is where
        // its memory is: the host is given its own for it, and no more than the buffer has
        const auto wanted = std::max<std::uint64_t>((bytes + 255) & ~255ull, 256);
        const D3D12_CONSTANT_BUFFER_VIEW_DESC view{
            gpu_address(address),
            static_cast<UINT>(std::min<std::uint64_t>({wanted, 65536, room & ~255ull}))};
        if (!view.SizeInBytes)
            view_nothing(true);
        else
            original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
                device, 17)(device, &view, host);
    } else {
        const auto size = std::min(room, bytes ? bytes : room);
        // structured when the descriptor has a stride that is not a format's, raw otherwise
        const bool structured = stride > 4 && offset % stride == 0;
        const UINT element = structured ? stride : 4;
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer = {offset / element, static_cast<UINT>(size / element),
                           structured ? stride : 0,
                           structured ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW};
            std::uint64_t copied = 0;
            if (!fit_buffer_view(resource, view) ||
                (raw_off_16(view) && !copy_raw_view(resource, view, copied)))
                view_nothing(true);
            else
                original<void(STDMETHODCALLTYPE *)(void *, void *, const void *,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 18)(
                    device, resource, &view, host);
            if (copied)
                keep_raw_copy({at}, view, copied);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer = {offset / element, static_cast<UINT>(size / element),
                           structured ? stride : 0, 0,
                           structured ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW};
            // a raw view not on 16 bytes is of nothing: a copy would lose what the GPU writes
            if (!fit_buffer_view(resource, view) || raw_off_16(view))
                view_nothing(true);
            else
                original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 19)(
                    device, resource, nullptr, &view, host);
        }
    }
    set_mark(heap, at);
}
// A table bound at a GPU handle: what the title wrote in it, by the root signature's ranges
void decode_table(const void *signature, UINT parameter, std::uint64_t gpu) {
    std::vector<TableRange> ranges;
    {
        const std::lock_guard lock(signature_mutex);
        const auto found = tables_of.find(signature);
        if (found == tables_of.end() || parameter >= found->second.size())
            return;
        ranges = found->second[parameter];
    }
    ShadowHeap heap{};
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *found = shadow_heap(gpu);
        if (!found || !found->shader_visible)
            return;
        heap = *found;
    }
    const std::uintptr_t table = gpu;
    for (const auto &range : ranges)
        for (UINT i = 0; i < range.count; ++i) {
            const auto at = table + std::uintptr_t{range.offset + i} * heap.stride;
            if (at + heap.stride > heap.end)
                break;
            decode_as(heap, at, range.type);
        }
}

// The device's methods that take CPU handles: the host's handle, and the shadow marked
using Handle = D3D12_CPU_DESCRIPTOR_HANDLE;
// The console's 16-bit depth with stencil (D16_UNORM_S8_UINT 118, and 119 and 120 to read its
// depth and its stencil) is no format of the host's, whose nearest is 24-bit depth with
// stencil. `resource`: what a texture is made as, which is typeless so that it can be read.
DXGI_FORMAT host_format(DXGI_FORMAT format, bool resource = false) {
    switch (static_cast<int>(format)) {
    case 118:
        return resource ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 119:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case 120:
        return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    default:
        return format;
    }
}
// B5G5R5A1_UNORM (86) and B4G4R4A4_UNORM (115) are a texture's formats on the host and
// nothing it draws or computes into, as a title of the console does: such a target is made
// in R8G8B8A8_UNORM, which holds all they can, and its views and the pipelines that draw to
// it follow.
bool packed_16(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B5G5R5A1_UNORM || format == DXGI_FORMAT_B4G4R4A4_UNORM;
}
DXGI_FORMAT target_format(DXGI_FORMAT format) {
    return packed_16(format) ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
}
DXGI_FORMAT view_format(ID3D12Resource *resource, DXGI_FORMAT format) {
    format = host_format(format);
    return packed_16(format) && resource && resource->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM
               ? DXGI_FORMAT_R8G8B8A8_UNORM
               : format;
}
// the value a target is cleared to names its format as well
const D3D12_CLEAR_VALUE *host_clear(const D3D12_CLEAR_VALUE *clear, D3D12_CLEAR_VALUE &host) {
    if (!clear || target_format(host_format(clear->Format)) == clear->Format)
        return clear;
    host = *clear;
    host.Format = target_format(host_format(clear->Format));
    return &host;
}
std::uint64_t root_address(std::uint64_t address);
std::uint64_t root_uav_address(std::uint64_t address);
std::uint64_t root_constants(std::uint64_t address);
void when_run(void *self, std::function<void()> work);
// the title's list a method of this thread is recording into, whose copies of the title's
// memory (ring_copy) are made again when a queue is given it
thread_local void *copying_for = nullptr;
struct CopyingFor {
    void *before;
    explicit CopyingFor(void *list) : before(copying_for) { copying_for = list; }
    ~CopyingFor() { copying_for = before; }
    CopyingFor(const CopyingFor &) = delete;
    CopyingFor &operator=(const CopyingFor &) = delete;
};
// how much of `bytes` from `address` on is memory the process can read, over as many regions
// of it as there are
std::uint64_t readable_bytes(const void *address, std::uint64_t bytes) {
    const auto start = reinterpret_cast<std::uint64_t>(address);
    std::uint64_t readable = 0;
    while (readable < bytes) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(start + readable), &info, sizeof info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            break;
        readable = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize - start;
    }
    return std::min(readable, bytes);
}
void STDMETHODCALLTYPE create_cbv(ID3D12Device *self, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                  Handle handle) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC host{};
    bool copied = false;
    if (desc) {
        host = *desc;
        // the console takes a view of any size; the host wants multiples of 256 bytes, and
        // removes the device over one that is not. No more than the buffer has, as for the
        // other views (see fit_buffer_view), where it has 256 bytes.
        ID3D12Resource *buffer = nullptr;
        std::uint64_t offset = 0, room = 0;
        host.SizeInBytes = (host.SizeInBytes + 255u) & ~255u;
        if (buffer_at(host.BufferLocation, buffer, offset, room) && room < host.SizeInBytes &&
            room >= 256)
            host.SizeInBytes = static_cast<UINT>(room & ~255ull);
        // On the Series console the view may be of any memory the title has, a buffer or
        // not, and start anywhere: the host drops the device over an address that is in no
        // buffer of its own. Such constants are copied to where the host takes them, as
        // they are when the view is made (a title may make its views as it draws).
        const auto given = host.BufferLocation;
        host.BufferLocation =
            console == Target::scarlett ? root_address(given) : gpu_address(given);
        MEMORY_BASIC_INFORMATION info{};
        // root_address has copied what is memory of the title's and in no buffer
        copied = console == Target::scarlett && host.BufferLocation != given &&
                 VirtualQuery(reinterpret_cast<void *>(given), &info, sizeof info) &&
                 info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (console == Target::scarlett && host.BufferLocation & 255 &&
            VirtualQuery(reinterpret_cast<void *>(given), &info, sizeof info) &&
            info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const auto room =
                reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize - given;
            if (const auto copy = ring_copy(reinterpret_cast<const void *>(given),
                                            std::min<std::uint64_t>(room, host.SizeInBytes))) {
                host.BufferLocation = copy;
                copied = true;
            }
        }
        if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 12) {
            std::fprintf(stderr,
                         "[Lost] CreateConstantBufferView(%llx, %u bytes) -> %llx, %u bytes, "
                         "from %s%c",
                         static_cast<unsigned long long>(given), desc->SizeInBytes,
                         static_cast<unsigned long long>(host.BufferLocation), host.SizeInBytes,
                         detail::guest_location(_ReturnAddress()).c_str(), 10);
            float words[16]{};
            if (desc->SizeInBytes >= 0x1a0 && info.State == MEM_COMMIT)
                std::memcpy(words, reinterpret_cast<const void *>(given + 0x160), sizeof words);
            std::fprintf(stderr,
                         "[Lost]   at 0x160: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g "
                         "%g%c",
                         words[0], words[1], words[2], words[3], words[4], words[5], words[6],
                         words[7], words[8], words[9], words[10], words[11], words[12], words[13],
                         words[14], words[15], 10);
            std::fflush(stderr);
        }
        // Memory the title has reserved and not yet committed is in no buffer and cannot be
        // copied: the view is of nothing for now, and made again when a draw reads it, by
        // which time the title has the memory and what it put there.
        if (console == Target::scarlett && given && host.BufferLocation == given && !copied &&
            VirtualQuery(reinterpret_cast<void *>(given), &info, sizeof info) &&
            info.State == MEM_RESERVE) {
            host.BufferLocation = 0;
            copied = true;
        }
        // memory the host's GPU cannot see and has no copy of: a view of nothing, whose
        // reads give 0
        if (!host.BufferLocation)
            host = {};
    }
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 17)(
        self, desc ? &host : nullptr, host_handle(handle));
    mark_host(handle);
    if (copied) {
        MemoryView view;
        view.handle = handle;
        view.constants = true;
        view.buffer = *desc;
        keep_memory_view(view);
    }
}
// What a view without a format reads a typeless texture as: the console takes the number
// format from elsewhere, the host wants it in the view. `srgb` picks the gamma twin.
DXGI_FORMAT typed_format(DXGI_FORMAT format, bool srgb = false) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_BC1_TYPELESS:
        return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS:
        return srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS:
        return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS:
        return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS:
        return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_BC6H_TYPELESS:
        return DXGI_FORMAT_BC6H_UF16;
    case DXGI_FORMAT_BC7_TYPELESS:
        return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    default:
        return format;
    }
}
// A view of all of a typeless texture (as the console's 16-bit depth with stencil is here) in
// `format`, for a title that gives none: the host has no format for it otherwise
D3D12_SHADER_RESOURCE_VIEW_DESC whole_shader_view(const D3D12_RESOURCE_DESC &of,
                                                  DXGI_FORMAT format) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const UINT slices = of.DepthOrArraySize;
    if (of.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipLevels = UINT_MAX;
    } else if (of.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        view.ViewDimension =
            slices > 1 ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE1D;
        view.Texture1DArray = {0, UINT_MAX, 0, slices, 0};
    } else if (of.SampleDesc.Count > 1) {
        view.ViewDimension =
            slices > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        view.Texture2DMSArray = {0, slices};
    } else if (slices > 1) {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray = {0, UINT_MAX, 0, slices, 0, 0};
    } else {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = UINT_MAX;
    }
    return view;
}
template <class View> const View *null_view(View &view, const D3D12_RESOURCE_DESC *of);
void STDMETHODCALLTYPE create_srv(ID3D12Device *self, ID3D12Resource *resource, const void *desc,
                                  Handle handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    const auto of = resource ? resource->GetDesc() : D3D12_RESOURCE_DESC{};
    std::uint64_t copied = 0; // the title's address of a raw view copied (copy_raw_view)
    if (desc) {
        std::memcpy(&view, desc, sizeof view);
        // the console's own formats as the host has them (see host_format): the stencil of its
        // depth is the second plane of the host's
        if (const auto format = host_format(self, view.Format, FormatUse::shader_view);
            format != view.Format) {
            const UINT plane = format == DXGI_FORMAT_X24_TYPELESS_G8_UINT ? 1 : 0;
            if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
                view.Texture2D.PlaneSlice = plane;
            else if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY)
                view.Texture2DArray.PlaneSlice = plane;
            view.Format = format;
            desc = &view;
        }
        // a 16-bit colour of a resource 8 bits a channel stand for: B5G6R5 has no alpha to
        // read, which is 1
        if (resource && small_colour(view.Format) && of.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            if (view.Format == DXGI_FORMAT_B5G6R5_UNORM) {
                const auto mapping = view.Shader4ComponentMapping;
                const auto channel = [&](UINT component) {
                    const auto from = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(component, mapping);
                    return from == D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3
                               ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
                               : from;
                };
                view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                    channel(0), channel(1), channel(2), channel(3));
            }
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc = &view;
        }
    } else if (resource && of.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
               typed_format(of.Format) != of.Format) {
        view = whole_shader_view(of, typed_format(of.Format));
        desc = &view;
    }
    // The console views a multisampled texture as a plain one and a typeless one without a
    // format; the host removes the device over either
    if (resource && desc) {
        if (of.SampleDesc.Count > 1 && view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            desc = &view;
        } else if (of.SampleDesc.Count > 1 &&
                   view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
            const auto slices = view.Texture2DArray;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {slices.FirstArraySlice, slices.ArraySize};
            desc = &view;
        }
        if (view.Format == DXGI_FORMAT_UNKNOWN && of.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            typed_format(of.Format) != of.Format) {
            view.Format = typed_format(of.Format);
            desc = &view;
        }
        if (view.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
            desc = &view;
            if (!fit_buffer_view(resource, view) ||
                (raw_off_16(view) && !copy_raw_view(resource, view, copied))) {
                resource = nullptr;
                desc = null_view(view, nullptr);
            }
        }
    }
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(self, 18)(
        self, resource, desc, host_handle(handle));
    mark_host(handle);
    if (copied)
        keep_raw_copy(handle, view, copied);
    if (resource && desc && console == Target::scarlett) {
        D3D12_SHADER_RESOURCE_VIEW_DESC made;
        std::memcpy(&made, desc, sizeof made);
        if (made.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
            const auto slot = host_handle(handle).ptr;
            const std::lock_guard lock(memory_view_mutex);
            if (buffer_views.size() > 400'000)
                buffer_views.clear();
            buffer_views[slot] = {resource, made};
        }
    }
    if (static const bool checking = std::getenv("XV_D3D_LOST") != nullptr;
        checking && resource && desc) {
        D3D12_SHADER_RESOURCE_VIEW_DESC made;
        std::memcpy(&made, desc, sizeof made);
        const auto of = resource->GetDesc();
        char text[200];
        std::snprintf(text, sizeof text,
                      "a view (dimension %d format %d, first %llu, %u of %u bytes) of resource %p "
                      "dimension %d %llux%u at %llx",
                      made.ViewDimension, made.Format,
                      static_cast<unsigned long long>(made.Buffer.FirstElement),
                      made.Buffer.NumElements, made.Buffer.StructureByteStride,
                      static_cast<void *>(resource), of.Dimension, of.Width, of.Height,
                      of.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                          ? static_cast<unsigned long long>(resource->GetGPUVirtualAddress())
                          : 0ull);
        const auto slot = host_handle(handle).ptr;
        const std::lock_guard lock(memory_view_mutex);
        if (lost_views.size() > 400'000)
            lost_views.clear();
        lost_views[slot] = text;
    }
}
// XV_D3D_LOST: the last view each thread made, said when the host drops the device there
thread_local char lost_last[640];
void STDMETHODCALLTYPE create_uav(ID3D12Device *self, ID3D12Resource *resource,
                                  ID3D12Resource *counter, const void *desc, Handle handle) {
    // A resource the host could not make writable this way (a depth buffer that is one on
    // the console, see host_rules): the host drops the device over a view of it. The
    // descriptor is one of nothing, which reads zeros and takes writes.
    if (resource && !(resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
            std::fprintf(stderr, "[Runtime] an unordered access view of a resource the host "
                                 "does not write that way is a view of nothing\n");
            std::fflush(stderr);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC nothing{};
        if (desc)
            std::memcpy(&nothing, desc, sizeof nothing);
        const auto of = resource->GetDesc();
        if (!desc || nothing.ViewDimension == D3D12_UAV_DIMENSION_UNKNOWN)
            nothing.ViewDimension =
                of.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER      ? D3D12_UAV_DIMENSION_BUFFER
                : of.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_UAV_DIMENSION_TEXTURE3D
                : of.DepthOrArraySize > 1 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                          : D3D12_UAV_DIMENSION_TEXTURE2D;
        nothing.Format = DXGI_FORMAT_R32_FLOAT;
        // the plane a view of the stencil names is none of this format's
        if (nothing.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D)
            nothing.Texture2D.PlaneSlice = 0;
        else if (nothing.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY)
            nothing.Texture2DArray.PlaneSlice = 0;
        if (static const bool checking = std::getenv("XV_D3D_LOST") != nullptr; checking) {
            std::uint32_t words[10]{};
            if (desc)
                std::memcpy(words, desc, sizeof words);
            std::snprintf(lost_last, sizeof lost_last,
                          "[Lost] thread %lu a view of nothing for a resource of format %d "
                          "flags 0x%x %llux%ux%u; the title's view%s: %u %u %x %x %x %x%c",
                          GetCurrentThreadId(), of.Format, of.Flags, of.Width, of.Height,
                          of.DepthOrArraySize, desc ? "" : " (none)", words[0], words[1], words[2],
                          words[3], words[4], words[5], 10);
        }
        original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *, Handle)>(self, 19)(
            self, nullptr, nullptr, &nothing, host_handle(handle));
        mark_host(handle);
        return;
    }
    // the console's own formats as the host has them (see host_format)
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    if (desc) {
        std::memcpy(&view, desc, sizeof view);
        if (const auto format =
                view_format(resource, host_format(self, view.Format, FormatUse::target_view));
            format != view.Format) {
            view.Format = format;
            desc = &view;
        }
    }
    if (resource && desc && view.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
        desc = &view;
        // a raw view not on 16 bytes (see raw_off_16) is of nothing: a copy would lose what
        // the GPU writes
        if (!fit_buffer_view(resource, view) || raw_off_16(view)) {
            resource = counter = nullptr;
            desc = null_view(view, nullptr);
        }
    }
    if (static const bool checking = std::getenv("XV_D3D_LOST") != nullptr; checking) {
        D3D12_RESOURCE_DESC of{};
        if (resource)
            of = resource->GetDesc();
        std::uint32_t words[10]{};
        if (desc)
            std::memcpy(words, desc, sizeof words);
        std::snprintf(lost_last, sizeof lost_last,
                      "[Lost] thread %lu CreateUnorderedAccessView: resource %p (dimension %d "
                      "%llux%ux%u mips %u format %d samples %u flags 0x%x), counter %p, view%s "
                      "format %u dimension %u: %x %x %x %x %x %x %x %x%c",
                      GetCurrentThreadId(), static_cast<void *>(resource), of.Dimension, of.Width,
                      of.Height, of.DepthOrArraySize, of.MipLevels, of.Format, of.SampleDesc.Count,
                      of.Flags, static_cast<void *>(counter), desc ? "" : " (none)", words[0],
                      words[1], words[2], words[3], words[4], words[5], words[6], words[7],
                      words[8], words[9], 10);
        if (resource) {
            resource->AddRef();
            const auto references = resource->Release();
            const auto so_far = std::strlen(lost_last);
            std::snprintf(lost_last + so_far, sizeof lost_last - so_far,
                          "[Lost]   the resource has %lu references, vtable %p%c", references,
                          *reinterpret_cast<void **>(resource), 10);
        }
        const std::lock_guard lock(shadow_mutex);
        const auto *const heap = shadow_heap(handle.ptr);
        const auto length = std::strlen(lost_last);
        std::snprintf(
            lost_last + length, sizeof lost_last - length,
            "[Lost]   handle %llx: %s, type %d, descriptor %llu of %llu (the host has "
            "%u), host handle %llx%c",
            static_cast<unsigned long long>(handle.ptr),
            heap ? "in a heap of the title's" : "in NO heap known here",
            heap ? static_cast<int>(heap->type) : -1,
            heap ? static_cast<unsigned long long>((handle.ptr - heap->base) / heap->stride) : 0ull,
            heap ? static_cast<unsigned long long>((heap->end - heap->base) / heap->stride) : 0ull,
            heap ? heap->host_count : 0u,
            heap ? static_cast<unsigned long long>(host_slot(*heap, handle.ptr).ptr) : 0ull, 10);
    }
    original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *, Handle)>(self, 19)(
        self, resource, counter, desc, host_handle(handle));
    mark_host(handle);
}
// Xbox One's CreatePlacedShaderResourceViewX (46) and CreatePlacedUnorderedAccessViewX (47):
// the console's descriptions begin as the host's do and go on with where the memory is and
// how the hardware reads it, which the host's resource already says
void STDMETHODCALLTYPE create_placed_srv(ID3D12Device *self, ID3D12Resource *resource,
                                         const void *desc, Handle handle) {
    if (!resource)
        return;
    // the number format after the host's fields and the memory's address: 9 is sRGB
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    if (desc) {
        std::memcpy(&view, desc, sizeof view);
        std::uint32_t number = 0;
        std::memcpy(&number, static_cast<const std::uint8_t *>(desc) + sizeof view + 12,
                    sizeof number);
        const auto of = resource->GetDesc();
        if (view.Format == DXGI_FORMAT_UNKNOWN && of.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
            view.Format = typed_format(of.Format, number == 9);
    }
    create_srv(self, resource, desc ? &view : nullptr, handle);
}
void STDMETHODCALLTYPE create_placed_uav(ID3D12Device *self, ID3D12Resource *resource,
                                         const void *desc, Handle handle) {
    if (resource)
        create_uav(self, resource, nullptr, desc, handle);
}
// GetDefaultMSAAParametersX (59): the standard pattern of the sample count, in sixteenths of
// a pixel from its centre, the same for the four pixels of a quad
HRESULT STDMETHODCALLTYPE default_msaa_parameters(void *, const DXGI_SAMPLE_DESC *samples,
                                                  std::int8_t (*locations)[16][2],
                                                  std::uint8_t *priorities,
                                                  std::uint32_t *parameters) {
    if (!samples || !std::has_single_bit(samples->Count) || samples->Count > 16)
        return E_INVALIDARG;
    static constexpr std::int8_t one[][2]{{0, 0}};
    static constexpr std::int8_t two[][2]{{4, 4}, {-4, -4}};
    static constexpr std::int8_t four[][2]{{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
    static constexpr std::int8_t eight[][2]{{1, -3}, {-1, 3},  {5, 1}, {-3, -5},
                                            {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
    static constexpr std::int8_t sixteen[][2]{
        {1, 1},  {-1, -3}, {-3, 2},  {4, -1}, {-5, -2}, {2, 5},  {5, 3}, {3, -5},
        {-2, 6}, {0, -7},  {-4, -6}, {-6, 4}, {-8, 0},  {7, -4}, {6, 7}, {-7, -8}};
    const std::int8_t(*const pattern)[2] = samples->Count == 1   ? one
                                           : samples->Count == 2 ? two
                                           : samples->Count == 4 ? four
                                           : samples->Count == 8 ? eight
                                                                 : sixteen;
    if (locations)
        for (int pixel = 0; pixel < 4; ++pixel) {
            std::memset(locations[pixel], 0, sizeof locations[pixel]);
            std::memcpy(locations[pixel], pattern, samples->Count * 2);
        }
    if (priorities)
        for (std::uint8_t i = 0; i < 16; ++i)
            priorities[i] = static_cast<std::uint8_t>(i % samples->Count);
    if (parameters) {
        // every sample is a colour sample the pixel shader sees
        const auto log2 = static_cast<std::uint32_t>(std::countr_zero(samples->Count));
        parameters[0] = log2 | 8u << 3 | log2 << 7;
        parameters[1] = log2 | log2 << 3 | log2 << 6 | log2 << 9;
    }
    return S_OK;
}
// the resource behind each render target view, for XV_D3D_PROBE
std::mutex target_mutex;
std::map<std::uint64_t, ID3D12Resource *> target_of;
std::atomic<ID3D12Resource *> last_target{nullptr};
std::map<std::uint64_t, std::string> view_text; // XV_D3D_LOST, under target_mutex
std::vector<ID3D12Resource *> bound_targets;    // the first of each size, under target_mutex
// the depth format a view of a typeless depth buffer reads it as, unknown for any other
DXGI_FORMAT depth_view_format(DXGI_FORMAT resource) {
    switch (resource) {
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_D16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
// CreateRenderTargetView (20) and CreateDepthStencilView (21): the console's own formats as
// the host has them (see host_format). A depth view of a typeless depth buffer (as the
// console's 16-bit depth with stencil is here) without a format, or without a description,
// views all of it in the depth format it stands for.
template <std::size_t Index>
void STDMETHODCALLTYPE create_target_view(ID3D12Device *self, ID3D12Resource *resource,
                                          const void *desc, Handle handle) {
    using View = std::conditional_t<Index == 20, D3D12_RENDER_TARGET_VIEW_DESC,
                                    D3D12_DEPTH_STENCIL_VIEW_DESC>;
    View view{};
    const auto depth = Index == 21 && resource ? depth_view_format(resource->GetDesc().Format)
                                               : DXGI_FORMAT_UNKNOWN;
    if (desc) {
        std::memcpy(&view, desc, sizeof view);
        view.Format = view_format(
            resource, host_format(self, view.Format,
                                  Index == 20 ? FormatUse::target_view : FormatUse::depth_view));
        if (view.Format == DXGI_FORMAT_UNKNOWN)
            view.Format = depth;
        desc = &view;
    } else if constexpr (Index == 21) {
        if (depth != DXGI_FORMAT_UNKNOWN) {
            const auto of = resource->GetDesc();
            const bool many = of.SampleDesc.Count > 1;
            view.Format = depth;
            if (of.DepthOrArraySize > 1) {
                view.ViewDimension = many ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY
                                          : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                if (many)
                    view.Texture2DMSArray = {0, of.DepthOrArraySize};
                else
                    view.Texture2DArray = {0, 0, of.DepthOrArraySize};
            } else {
                view.ViewDimension =
                    many ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
            }
            desc = &view;
        }
    }
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(self, Index)(
        self, resource, desc, host_handle(handle));
    mark_host(handle);
    if (Index == 21) { // XV_CAPTURE: what a depth view is of
        const std::lock_guard lock(target_mutex);
        target_of[host_handle(handle).ptr] = resource;
    }
    if (Index == 20) {
        const std::lock_guard lock(target_mutex);
        target_of[host_handle(handle).ptr] = resource;
        // XV_D3D_PROBE reads the targets back: each is held, so that one the title lets go
        // of is still there to read
        static const bool probing = std::getenv("XV_D3D_PROBE") != nullptr;
        static std::set<ID3D12Resource *> held;
        if (probing && resource && held.insert(resource).second)
            resource->AddRef();
    }
    // XV_D3D_LOST: what each view is of, for the draw the host drops the device at
    if (static const bool checking = std::getenv("XV_D3D_LOST") != nullptr; checking) {
        char text[200] = "no resource";
        if (resource) {
            const auto of = resource->GetDesc();
            std::snprintf(text, sizeof text,
                          "resource %p dimension %d %llux%ux%u mips %u format %d samples %u flags "
                          "0x%x",
                          static_cast<void *>(resource), of.Dimension, of.Width, of.Height,
                          of.DepthOrArraySize, of.MipLevels, of.Format, of.SampleDesc.Count,
                          of.Flags);
        }
        std::uint32_t words[4]{};
        if (desc)
            std::memcpy(words, desc, sizeof words);
        char more[120];
        std::snprintf(more, sizeof more, "; view %s format %u dimension %u, %u %u",
                      desc ? "with" : "without a description:", words[0], words[1], words[2],
                      words[3]);
        const std::lock_guard lock(target_mutex);
        view_text[host_handle(handle).ptr] =
            std::string(Index == 20 ? "RTV of " : "DSV of ") + text + more;
    }
}
void STDMETHODCALLTYPE create_sampler(ID3D12Device *self, const D3D12_SAMPLER_DESC *desc,
                                      Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 22)(
        self, desc, host_handle(handle));
    mark_host(handle);
}
// CreateSamplerX (61): a sampler as the console's GPU has it, with each filter on its own.
// What the host has no word for (the filter along Z, the second bias, the anisotropy bias
// and threshold, the performance settings) is left out.
struct ConsoleSampler {
    UINT flags, filter_mag, filter_min, filter_mip, filter_z, filter_mode;
    D3D12_TEXTURE_ADDRESS_MODE address_u, address_v, address_w;
    FLOAT min_lod, max_lod, lod_bias, lod_bias_secondary;
    UINT max_anisotropy;
    FLOAT anisotropy_bias;
    UINT anisotropy_threshold;
    D3D12_COMPARISON_FUNC comparison;
    FLOAT border[4];
};
void STDMETHODCALLTYPE create_sampler_x(ID3D12Device *self, const ConsoleSampler *desc,
                                        Handle handle) {
    if (!desc)
        return;
    // filters: 0 point, 1 bilinear, 2 and 3 the same with anisotropy; mips: 0 none, 1 point,
    // 2 linear; mode: 0 a blend, 1 the minimum, 2 the maximum
    const bool compares = desc->comparison > D3D12_COMPARISON_FUNC_NEVER &&
                          desc->comparison < D3D12_COMPARISON_FUNC_ALWAYS;
    const UINT reduction = compares                 ? 1
                           : desc->filter_mode == 1 ? 2
                           : desc->filter_mode == 2 ? 3
                                                    : 0;
    const bool anisotropic = desc->filter_mag >= 2 || desc->filter_min >= 2;
    const UINT basic = (desc->filter_min & 1) << 4 | (desc->filter_mag & 1) << 2 |
                       (desc->filter_mip == 2 ? 1u : 0u);
    const auto address = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
        return mode >= 1 && mode <= 5 ? mode : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    };
    D3D12_SAMPLER_DESC host{};
    host.Filter = static_cast<D3D12_FILTER>((anisotropic ? 0x55u : basic) | reduction << 7);
    host.AddressU = address(desc->address_u);
    host.AddressV = address(desc->address_v);
    host.AddressW = address(desc->address_w);
    host.MipLODBias = desc->lod_bias;
    host.MaxAnisotropy = anisotropic ? std::clamp(desc->max_anisotropy, 1u, 16u) : 1;
    host.ComparisonFunc = compares ? desc->comparison : D3D12_COMPARISON_FUNC_NEVER;
    std::copy(desc->border, desc->border + 4, host.BorderColor);
    host.MinLOD = desc->min_lod;
    host.MaxLOD = desc->filter_mip ? desc->max_lod : desc->min_lod;
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 22)(
        self, &host, host_handle(handle));
    mark_host(handle);
}
// A copied descriptor: the host's goes along with the host's copy; one the title wrote goes
// along as the title wrote it, read where it is bound
void carry(Handle destination, Handle source, UINT stride) {
    host_view(source);
    const std::lock_guard lock(shadow_mutex);
    const auto *const heap = shadow_heap(destination.ptr);
    if (!heap)
        return;
    std::uint64_t first = host_mark;
    if (shadow_heap(source.ptr))
        std::memcpy(&first, reinterpret_cast<const void *>(source.ptr), sizeof first);
    if (first == host_mark || first == 0)
        set_mark(*heap, destination.ptr);
    else
        std::memcpy(reinterpret_cast<void *>(destination.ptr),
                    reinterpret_cast<const void *>(source.ptr), stride);
}
void STDMETHODCALLTYPE copy_descriptors(ID3D12Device *self, UINT destination_ranges,
                                        const Handle *destinations, const UINT *destination_sizes,
                                        UINT source_ranges, const Handle *sources,
                                        const UINT *source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    std::vector<Handle> host_destinations(destinations, destinations + destination_ranges);
    std::vector<Handle> host_sources(sources, sources + source_ranges);
    const UINT stride = console_stride(type);
    // the descriptors one by one, in the order both lists of ranges give them
    UINT source_range = 0, source_at = 0;
    for (UINT range = 0; range < destination_ranges; ++range) {
        const UINT count = destination_sizes ? destination_sizes[range] : 1;
        for (UINT i = 0; i < count && source_range < source_ranges; ++i) {
            carry({destinations[range].ptr + std::size_t{i} * stride},
                  {sources[source_range].ptr + std::size_t{source_at} * stride}, stride);
            if (++source_at >= (source_sizes ? source_sizes[source_range] : 1))
                ++source_range, source_at = 0;
        }
        host_destinations[range] = host_handle(destinations[range]);
    }
    for (UINT range = 0; range < source_ranges; ++range)
        host_sources[range] = host_handle(sources[range]);
    using Copy =
        void(STDMETHODCALLTYPE *)(void *, UINT, const Handle *, const UINT *, UINT, const Handle *,
                                  const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(self, 23)(self, destination_ranges, host_destinations.data(), destination_sizes,
                             source_ranges, host_sources.data(), source_sizes, type);
    for (UINT range = 0; range < destination_ranges; ++range)
        wrote(destinations[range], destination_sizes ? destination_sizes[range] : 1);
}
void STDMETHODCALLTYPE copy_descriptors_simple(ID3D12Device *self, UINT count, Handle destination,
                                               Handle source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const UINT stride = console_stride(type);
    for (UINT i = 0; i < count; ++i)
        carry({destination.ptr + std::size_t{i} * stride}, {source.ptr + std::size_t{i} * stride},
              stride);
    using Copy =
        void(STDMETHODCALLTYPE *)(void *, UINT, Handle, Handle, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(self, 24)(self, count, host_handle(destination), host_handle(source), type);
    wrote(destination, count);
}
// CreateDescriptorHeap: the heap's CPU handles are the console's (shadow memory), its GPU
// handles the host's, returned in RAX (9, 10)
HRESULT STDMETHODCALLTYPE create_descriptor_heap(ID3D12Device *self,
                                                 const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID iid,
                                                 void **heap) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);
    // The Series console's sampler heap without a bound (type 4) is a sampler heap here, as
    // large as the host makes one: 2048 samplers, which is more than titles keep
    D3D12_DESCRIPTOR_HEAP_DESC changed{};
    const auto *const caller = _ReturnAddress();
    const std::size_t title_bytes =
        desc ? std::size_t{desc->NumDescriptors} * console_stride(desc->Type) : 0;
    if (desc && static_cast<int>(desc->Type) == 4) {
        changed = *desc;
        changed.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        changed.NumDescriptors = std::min(changed.NumDescriptors, 2048u);
        desc = &changed;
    }
    // and a heap of target views is never one shaders see, whatever the title says of it
    if (desc &&
        (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
         desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
        desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        changed = *desc;
        changed.Flags &= ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc = &changed;
    }
    // The host shows its shaders a million descriptors at most: a larger heap is made that
    // large, and what the title keeps past that is not seen
    // (a host that makes a larger one is given the title's: that is tried first)
    constexpr UINT default_visible_limit = 1'000'000;
    const UINT title_count = desc ? desc->NumDescriptors : 0;
    auto result = E_FAIL;
    if (desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE &&
        desc->NumDescriptors > default_visible_limit &&
        FAILED(result = original<Create>(self, 14)(self, desc, iid, heap))) {
        // The adapter's own limit where it tells one (OPTIONS19) that is under what the title
        // asked for. Binding tier alone cannot determine it: WARP and hardware can report
        // different limits at the same tier.
        UINT host_visible_limit = default_visible_limit;
#if defined(__ID3D12Device12_INTERFACE_DEFINED__)
        D3D12_FEATURE_DATA_D3D12_OPTIONS19 options{};
        using CheckFeature = HRESULT(STDMETHODCALLTYPE *)(void *, D3D12_FEATURE, void *, UINT);
        if (SUCCEEDED(original<CheckFeature>(self, 13)(self, D3D12_FEATURE_D3D12_OPTIONS19,
                                                       &options, sizeof(options))) &&
            options.MaxViewDescriptorHeapSize &&
            options.MaxViewDescriptorHeapSize < desc->NumDescriptors) {
            host_visible_limit = options.MaxViewDescriptorHeapSize;
        }
#endif
        std::fprintf(stderr,
                     "[Runtime] a descriptor heap of %u descriptors for shaders is made with "
                     "%u, the host's limit (0x%08lx for all of them): the views past that are "
                     "not seen\n",
                     desc->NumDescriptors, host_visible_limit, static_cast<unsigned long>(result));
        std::fflush(stderr);
        changed = *desc;
        changed.NumDescriptors = host_visible_limit;
        desc = &changed;
    }
    if (FAILED(result))
        result = original<Create>(self, 14)(self, desc, iid, heap);
    if (FAILED(result) && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateDescriptorHeap type %d, %u descriptors, flags 0x%x, node "
                     "0x%x: 0x%08lx%c",
                     desc->Type, desc->NumDescriptors, desc->Flags, desc->NodeMask,
                     static_cast<unsigned long>(result), 10);
        std::fflush(stderr);
    }
    if (FAILED(result) || !heap || !*heap || !desc)
        return result;
    auto *const host_heap_object = static_cast<ID3D12DescriptorHeap *>(*heap);
    const auto host = host_heap_object->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                         ? host_heap_object->GetGPUDescriptorHandleForHeapStart().ptr
                         : 0;
    auto *const proxy = new HeapProxy{heap_vtable.data(), host_heap_object, 1};
    {
        const std::lock_guard lock(heap_proxy_mutex);
        heap_proxies.insert(proxy);
    }
    *heap = proxy;
    // the shadow is as long as the title's heap; the host's may be shorter
    auto shadowed = *desc;
    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        shadowed.NumDescriptors = title_count;
    register_shadow(self, proxy, shadowed, host, gpu,
                    title_bytes ? title_heap_memory(caller, title_bytes) : nullptr,
                    desc->NumDescriptors);
    return result;
}
HRESULT STDMETHODCALLTYPE create_committed_resource(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
                                                    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                                    D3D12_RESOURCE_STATES,
                                                    const D3D12_CLEAR_VALUE *, REFIID, void **);
void report_messages();
// CreateRootSignature (16): its descriptor tables are kept, to know what the descriptors the
// title writes are
// The root signatures the title made, newest last, each with a reference of ours and
// whether it allows an input layout (which tells one for drawing from one for computing)
struct TitleSignature {
    ID3D12RootSignature *signature;
    bool input_layout;
};
std::mutex title_signature_mutex;
std::vector<TitleSignature> title_signatures;
// the RTS0 part each was made from: a shader of the later kits has no root signature in it,
// and what its registers hold is read from the one its pipeline is made with
std::map<const void *, std::vector<std::uint8_t>> signature_parts;
std::span<const std::uint8_t> signature_part(const void *signature) {
    const std::lock_guard lock(title_signature_mutex);
    const auto found = signature_parts.find(signature);
    return found == signature_parts.end() ? std::span<const std::uint8_t>{}
                                          : std::span<const std::uint8_t>{found->second};
}
void remember_signature(ID3D12RootSignature *signature, const void *blob, SIZE_T size) {
    const auto *const bytes = static_cast<const std::uint8_t *>(blob);
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> rts0;
    if (size >= 32 && !std::memcmp(bytes, "DXBC", 4)) {
        std::uint32_t count = 0;
        std::memcpy(&count, bytes + 28, 4);
        for (std::uint32_t i = 0; i < count && 36 + 4 * i <= size; ++i) {
            std::uint32_t offset = 0;
            std::memcpy(&offset, bytes + 32 + 4 * i, 4);
            if (offset + 8 + 24 <= size && !std::memcmp(bytes + offset, "RTS0", 4)) {
                std::memcpy(&flags, bytes + offset + 8 + 20, 4);
                std::uint32_t length = 0;
                std::memcpy(&length, bytes + offset + 4, 4);
                if (offset + 8 + std::size_t{length} <= size)
                    rts0.assign(bytes + offset + 8, bytes + offset + 8 + length);
            }
        }
    }
    const std::lock_guard lock(title_signature_mutex);
    if (!rts0.empty())
        signature_parts[signature] = std::move(rts0);
    for (const auto &known : title_signatures)
        if (known.signature == signature)
            return;
    signature->AddRef();
    title_signatures.push_back(
        {signature, (flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0});
}
HRESULT STDMETHODCALLTYPE create_root_signature(ID3D12Device *self, UINT node, const void *blob,
                                                SIZE_T size, REFIID iid, void **signature) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, UINT, const void *, SIZE_T, REFIID, void **);
    // A shader container says how long it is (the dword at 24). The host turns down a size
    // that differs from it; titles get away with a wrong one on the console (a sample passes
    // one shader with the size of another), so the container's own is what the host is given.
    std::uint32_t contained = 0;
    if (blob && size >= 32 && !std::memcmp(blob, "DXBC", 4)) {
        std::memcpy(&contained, static_cast<const std::uint8_t *>(blob) + 24, 4);
        MEMORY_BASIC_INFORMATION info{};
        const auto *const end = static_cast<const std::uint8_t *>(blob) + contained;
        if (contained >= 32 && contained != size && VirtualQuery(blob, &info, sizeof info) &&
            info.State == MEM_COMMIT &&
            end <= static_cast<const std::uint8_t *>(info.BaseAddress) + info.RegionSize)
            size = contained;
    }
    const auto result = original<Create>(self, 16)(self, node, blob, size, iid, signature);
    if (SUCCEEDED(result) && signature && *signature) {
        read_root_signature(*signature, blob, size);
        remember_signature(static_cast<ID3D12RootSignature *>(*signature), blob, size);
    }
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreateRootSignature from %zu bytes: 0x%08lx\n",
                     static_cast<std::size_t>(size), static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
D3D12_RESOURCE_DESC host_resource_desc(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc);
D3D12_RESOURCE_STATES host_state(D3D12_RESOURCE_STATES state);
// A title places textures in memory it maps pages into later, many more than it has pages
// for (slots of what it streams): on the console they take nothing until then. Here such a
// texture is a reserved resource, which has no memory either, and is given a heap of its own
// when it is first uploaded to (back_texture).
std::mutex unbacked_mutex;
std::set<const void *> unbacked;
HRESULT unbacked_texture(ID3D12Device *self, std::uint64_t address, const D3D12_RESOURCE_DESC *desc,
                         D3D12_RESOURCE_STATES state, REFIID iid, void **resource) {
    if (console != Target::scarlett || !desc || !address || !resource ||
        desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc->SampleDesc.Count > 1 ||
        desc->Flags &
            (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        readable_memory(address, 1))
        return E_NOTIMPL;
    auto host = host_resource_desc(self, *desc);
    host.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    host.Alignment = 0;
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                     const D3D12_CLEAR_VALUE *, REFIID, void **);
    const auto result =
        original<Create>(self, 30)(self, &host, host_state(state), nullptr, iid, resource);
    if (FAILED(result))
        return result;
    // its address is where it was placed, as a committed one's
    console_resource(*resource);
    mappable_resource(*resource);
    ID3DDestructionNotifier *notifier = nullptr;
    UINT id = 0;
    if (SUCCEEDED(static_cast<IUnknown *>(*resource)->QueryInterface(IID_PPV_ARGS(&notifier)))) {
        notifier->RegisterDestructionCallback(
            [](void *gone) {
                const std::lock_guard lock(unbacked_mutex);
                unbacked.erase(gone);
            },
            *resource, &id);
        notifier->Release();
    }
    const std::lock_guard lock(unbacked_mutex);
    unbacked.insert(*resource);
    return result;
}
// the heap for one of those, its tiles mapped on `queue` before the work given it next; the
// resource keeps the heap
void back_texture(ID3D12CommandQueue *queue, ID3D12Device *device, ID3D12Resource *texture) {
    {
        const std::lock_guard lock(unbacked_mutex);
        if (!unbacked.erase(texture))
            return;
    }
    UINT tiles = 0;
    D3D12_PACKED_MIP_INFO packed{};
    device->GetResourceTiling(texture, &tiles, &packed, nullptr, nullptr, 0, nullptr);
    if (!tiles)
        return;
    const D3D12_HEAP_DESC heap_desc{std::uint64_t{tiles} * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
                                    {D3D12_HEAP_TYPE_DEFAULT},
                                    0,
                                    D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
    ID3D12Heap *heap = nullptr;
    if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap))))
        return;
    // the standard mips' tiles in order, then the packed ones
    const UINT standard = tiles - packed.NumTilesForPackedMips;
    D3D12_TILED_RESOURCE_COORDINATE starts[2]{};
    D3D12_TILE_REGION_SIZE sizes[2]{};
    UINT regions = 0;
    if (standard)
        sizes[regions++].NumTiles = standard;
    if (packed.NumTilesForPackedMips) {
        starts[regions].Subresource = packed.NumStandardMips;
        sizes[regions++].NumTiles = packed.NumTilesForPackedMips;
    }
    const UINT offset = 0;
    queue->UpdateTileMappings(texture, regions, starts, sizes, heap, 1, nullptr, &offset, &tiles,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    static constexpr GUID backing{
        0x6f2b1c4e, 0x58d3, 0x4a7f, {0x9e, 0x21, 0x3c, 0x5a, 0x7d, 0x80, 0x14, 0xb6}};
    texture->SetPrivateDataInterface(backing, heap);
    heap->Release();
}
// slot 44: CreatePlacedResourceX(address, desc, state, clear, iid, resource), a resource on
// memory of the title's (Unreal's XboxOneD3D12Texture.cpp names it). The host cannot place a
// resource at an address: a committed one in GPU memory stands for it.
HRESULT STDMETHODCALLTYPE create_placed_resource_x(ID3D12Device *self, std::uint64_t address,
                                                   const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                   void **resource) {
    if (static std::atomic<int> shown{0};
        desc && std::getenv("XV_D3D_TRACE") &&
        (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || shown < 4) && shown++ < 40) {
        std::uint64_t first = 0;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
            info.State == MEM_COMMIT)
            std::memcpy(&first, reinterpret_cast<const void *>(address), sizeof first);
        std::fprintf(stderr,
                     "[Runtime] CreatePlacedResourceX at %llx: dimension %d %llux%ux%u mips %u "
                     "format %d layout %d flags 0x%x state 0x%x, memory %s %llx\n",
                     static_cast<unsigned long long>(address), desc->Dimension, desc->Width,
                     desc->Height, desc->DepthOrArraySize, desc->MipLevels, desc->Format,
                     desc->Layout, desc->Flags, state,
                     info.State == MEM_COMMIT ? "committed" : "not committed",
                     static_cast<unsigned long long>(first));
        std::fflush(stderr);
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto result = unbacked_texture(self, address, desc, state, iid, resource);
    if (FAILED(result))
        result = create_committed_resource(self, &heap, D3D12_HEAP_FLAG_NONE, desc, state, clear,
                                           iid, resource);
    if (SUCCEEDED(result) && resource && *resource && desc && address &&
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        note_placed_texture(static_cast<ID3D12Resource *>(*resource), address, *desc);
    // a texture the title fills in its memory: the host's copy follows it
    if (SUCCEEDED(result) && resource && *resource && desc &&
        !(desc->Flags &
          (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
        track_placement(static_cast<ID3D12Resource *>(*resource),
                        reinterpret_cast<const void *>(address),
                        console == Target::scarlett ? desc : nullptr, desc->Format);
    return result;
}
// The state a D3D12_BARRIER_LAYOUT stands for, by the table of the two in the Enhanced
// Barriers spec (Microsoft's DirectX-Specs); the layouts of one kind of queue as the general
// ones, the compute queue's shader resource without pixel shaders. 30 has no name in later
// headers, and 31 is DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE, which earlier ones
// do not have. UNDEFINED (-1) and what no header names are COMMON.
D3D12_RESOURCE_STATES legacy_state(UINT layout) {
    constexpr auto shader_resource =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES states[]{
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_DEPTH_READ,
        shader_resource,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
        D3D12_RESOURCE_STATE_RESOLVE_DEST,
        D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,
        D3D12_RESOURCE_STATE_VIDEO_DECODE_READ,
        D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE,
        D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ,
        D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE,
        D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ,
        D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
        // the direct queue's
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        shader_resource,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST,
        // the compute queue's
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST,
        // 30, then 31
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_GENERIC_READ,
    };
    return layout < std::size(states) ? states[layout] : D3D12_RESOURCE_STATE_COMMON;
}
// The later kits' CreatePlacedResourceX (device slot 133) and CreateCommittedResource (127)
// take a D3D12_RESOURCE_DESC1 and a D3D12_BARRIER_LAYOUT where the first took a state: -1
// (undefined) for a buffer. The other layouts are taken as the states they stand for. A
// title that calls them has the later layouts of queues and lists as well, and may record
// lists before any call of a queue shows it.
D3D12_RESOURCE_STATES layout_state(UINT layout, const void *caller) {
    later_queue = true;
    if (static std::atomic<int> shown{0}; layout != ~0u && shown++ < 8) {
        std::fprintf(stderr, "[Runtime] a resource created with layout %u from %s\n", layout,
                     detail::guest_location(caller).c_str());
        std::fflush(stderr);
    }
    return legacy_state(layout);
}
// A texture made in a layout is in the state it stands for until a barrier moves it: an
// enhanced barrier from UNDEFINED starts there (see enhanced_barriers).
void remember_state(ID3D12Resource *resource, D3D12_RESOURCE_STATES state);
HRESULT made_in_layout(HRESULT result, const D3D12_RESOURCE_DESC *desc, UINT layout,
                       void **resource) {
    if (SUCCEEDED(result) && desc && desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        resource && *resource)
        remember_state(static_cast<ID3D12Resource *>(*resource), legacy_state(layout));
    return result;
}
HRESULT STDMETHODCALLTYPE create_placed_resource_x_layout(ID3D12Device *self, std::uint64_t address,
                                                          const D3D12_RESOURCE_DESC *desc,
                                                          UINT layout,
                                                          const D3D12_CLEAR_VALUE *clear,
                                                          REFIID iid, void **resource) {
    return made_in_layout(create_placed_resource_x(self, address, desc,
                                                   layout_state(layout, _ReturnAddress()), clear,
                                                   iid, resource),
                          desc, layout, resource);
}
// the arguments of the desktop's CreateCommittedResource3
HRESULT STDMETHODCALLTYPE create_committed_resource_layout(
    ID3D12Device *self, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC *desc, UINT layout, const D3D12_CLEAR_VALUE *clear, void *, UINT,
    const DXGI_FORMAT *, REFIID iid, void **resource) {
    const auto result = made_in_layout(
        create_committed_resource(self, heap, flags, desc, layout_state(layout, _ReturnAddress()),
                                  clear, iid, resource),
        desc, layout, resource);
    // A display target's memory is taken through the title's allocation hooks on the
    // console, in 64 KiB pages (bits 14 and 15 of the attributes: 2); a title that keeps a
    // pool for them checks how much of it went. The host's resource has no use for it.
    if (const auto *const hooks = allocation_hooks(_ReturnAddress());
        hooks && SUCCEEDED(result) && flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY && resource &&
        *resource) {
        const auto made = static_cast<ID3D12Resource *>(*resource)->GetDesc();
        const auto info = self->GetResourceAllocationInfo(0, 1, &made);
        if (info.SizeInBytes != UINT64_MAX)
            hooks->allocate(static_cast<SIZE_T>(info.SizeInBytes), 0x8000 | 128ull << 16);
    }
    return result;
}
// The later kits' views of memory at an address, with no resource object (device slots 70
// and 71): the description of what is there, the host's description of the view followed
// by the memory's GPU address (and the counter's), and where the descriptor goes. The
// buffer here that holds the address is the one viewed.
UINT element_bytes(DXGI_FORMAT format) {
    // the 16-bit colours, and the 32-bit ones blue first (B8G8R8A8_UNORM to B8G8R8X8_UNORM_SRGB)
    if (format == DXGI_FORMAT_B5G6R5_UNORM || format == DXGI_FORMAT_B5G5R5A1_UNORM ||
        format == DXGI_FORMAT_B4G4R4A4_UNORM)
        return 2;
    const int f = format;
    return f >= 1 && f <= 4     ? 16
           : f >= 5 && f <= 8   ? 12
           : f >= 9 && f <= 18  ? 8
           : f >= 23 && f <= 43 ? 4
           : f >= 48 && f <= 59 ? 2
           : f >= 60 && f <= 64 ? 1
           : f >= 87 && f <= 93 ? 4
                                : 0;
}
void view_at(const char *what, const D3D12_RESOURCE_DESC *of, UINT dimension, std::uint64_t address,
             const void *caller) {
    later_queue = true;
    if (static std::atomic<int> shown{0}; shown++ < 24) {
        std::fprintf(stderr,
                     "[Runtime] %s at %llx: resource dimension %d %llux%u format %d, view "
                     "dimension %u, from %s\n",
                     what, static_cast<unsigned long long>(address), of ? of->Dimension : -1,
                     of ? of->Width : 0, of ? of->Height : 0, of ? of->Format : 0, dimension,
                     detail::guest_location(caller).c_str());
        std::fflush(stderr);
    }
}
// A texture that is only memory to the title, with no resource object: the host's texture
// for that memory, made when the first view of it is, and kept for as long as the same
// kind of texture is viewed there. What the title writes there is followed as for a placed
// resource.
struct MemoryTexture {
    ID3D12Resource *resource;
    D3D12_RESOURCE_DESC desc;
};
std::mutex memory_texture_mutex;
std::map<std::uint64_t, MemoryTexture> memory_textures;
ID3D12Resource *texture_at(ID3D12Device *self, std::uint64_t address,
                           const D3D12_RESOURCE_DESC *of) {
    if (!address || !of || of->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return nullptr;
    const std::lock_guard lock(memory_texture_mutex);
    const auto same = [&](const D3D12_RESOURCE_DESC &other) {
        return other.Dimension == of->Dimension && other.Width == of->Width &&
               other.Height == of->Height && other.DepthOrArraySize == of->DepthOrArraySize &&
               other.MipLevels == of->MipLevels && other.Format == of->Format;
    };
    if (const auto found = memory_textures.find(address); found != memory_textures.end()) {
        if (same(found->second.desc))
            return found->second.resource;
        found->second.resource->Release();
        memory_textures.erase(found);
    }
    // the texture the title placed there, if the view is of one like it
    if (auto *const placed = texture_placed_at(address, *of)) {
        memory_textures[address] = {placed, *of};
        return placed;
    }
    ID3D12Resource *resource = nullptr;
    if (const auto result = create_placed_resource_x(self, address, of, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&resource));
        FAILED(result) || !resource) {
        if (static std::atomic<int> shown{0}; shown++ < 8) {
            std::fprintf(stderr,
                         "[Runtime] no texture for the memory at %llx: dimension %d %llux%ux%u "
                         "mips %u format %d flags 0x%x: 0x%08lx\n",
                         static_cast<unsigned long long>(address), of->Dimension, of->Width,
                         of->Height, of->DepthOrArraySize, of->MipLevels, of->Format, of->Flags,
                         static_cast<unsigned long>(result));
            std::fflush(stderr);
        }
        return nullptr;
    }
    memory_textures[address] = {resource, *of};
    return resource;
}
// A view of nothing, whose reads give 0, for memory no resource here holds (see gpu_address):
// the host wants a format for it unless it is of structures, and a raw one to start on 16
// bytes even then (see raw_off_16): it starts at the first
template <class View> const View *null_view(View &view, const D3D12_RESOURCE_DESC *of) {
    // D3D12_SRV_DIMENSION_BUFFER and D3D12_UAV_DIMENSION_BUFFER
    const bool buffer = static_cast<int>(view.ViewDimension) == 1;
    if (view.Format == DXGI_FORMAT_UNKNOWN && !(buffer && view.Buffer.StructureByteStride)) {
        const auto format = of && !buffer ? typed_format(of->Format) : DXGI_FORMAT_UNKNOWN;
        view.Format = format != DXGI_FORMAT_UNKNOWN ? format : DXGI_FORMAT_R32_UINT;
    }
    if (buffer)
        view.Buffer.FirstElement = 0;
    return &view;
}
void STDMETHODCALLTYPE create_srv_at(ID3D12Device *self, const D3D12_RESOURCE_DESC *of,
                                     const void *desc, Handle handle) {
    if (!desc)
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC view;
    std::uint64_t address = 0;
    std::memcpy(&view, desc, sizeof view);
    std::memcpy(&address, static_cast<const std::uint8_t *>(desc) + sizeof view, sizeof address);
    view_at("a shader resource view of memory", of, view.ViewDimension, address, _ReturnAddress());
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, size = 0;
    if (view.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
        auto *const texture = texture_at(self, address, of);
        create_srv(self, texture, texture ? &view : null_view(view, of), handle);
        return;
    }
    const UINT element = view.Buffer.StructureByteStride ? view.Buffer.StructureByteStride
                         : view.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW
                             ? 4
                             : element_bytes(view.Format);
    const bool known = buffer_at(address, resource, offset, size);
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 6) {
        float first[16]{};
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
            info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            std::memcpy(first, reinterpret_cast<const void *>(address), sizeof first);
        std::fprintf(stderr,
                     "[Lost] a view of a buffer of memory at %llx (%s): first %llu, %u of %u "
                     "bytes, flags %u, format %d, of %llu bytes: %g %g %g %g | %g %g %g %g | %g %g "
                     "%g %g | %g %g %g %g%c",
                     static_cast<unsigned long long>(address), known ? "a buffer" : "no buffer",
                     static_cast<unsigned long long>(view.Buffer.FirstElement),
                     view.Buffer.NumElements, view.Buffer.StructureByteStride, view.Buffer.Flags,
                     view.Format, static_cast<unsigned long long>(of ? of->Width : 0), first[0],
                     first[1], first[2], first[3], first[4], first[5], first[6], first[7], first[8],
                     first[9], first[10], first[11], first[12], first[13], first[14], first[15],
                     10);
    }
    if (!known) {
        // Memory of the title's that is in no buffer here: what the view is of is copied,
        // as it is now, to where the host can have a view (the matrices a title draws its
        // first screens with may be in such views, made as it draws).
        // The copy is of the elements viewed, and starts on 256 bytes, which a raw view takes.
        const auto skipped = view.Buffer.FirstElement * element;
        const auto bytes = std::uint64_t{view.Buffer.NumElements} * element;
        MEMORY_BASIC_INFORMATION info{};
        if (console != Target::scarlett || !element || !bytes || bytes > (16ull << 20) ||
            !VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize - address <
                skipped + bytes ||
            !ring_copy(reinterpret_cast<const void *>(address + skipped), bytes, bytes,
                       256 % element ? element : 256, &resource, &offset)) {
            // nothing the host can have a view of: a view of nothing, whose reads give 0
            create_srv(self, nullptr, null_view(view, of), handle);
            return;
        }
        view.Buffer.FirstElement = 0;
    }
    if (element)
        view.Buffer.FirstElement += offset / element;
    create_srv(self, resource, &view, handle);
    if (!known) {
        MemoryView kept;
        kept.handle = handle;
        std::memcpy(kept.resource, desc, sizeof kept.resource);
        keep_memory_view(kept);
    }
}
// the view of a buffer in the host's `slot`, as one of elements of `stride` bytes
void structured_view(ID3D12Device *device, std::uint64_t slot, std::uint32_t stride) {
    BufferView view;
    {
        const std::lock_guard lock(memory_view_mutex);
        const auto found = buffer_views.find(slot);
        if (found == buffer_views.end())
            return;
        view = found->second;
    }
    const std::uint64_t element =
        view.desc.Buffer.StructureByteStride                 ? view.desc.Buffer.StructureByteStride
        : view.desc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW ? 4
                                                             : element_bytes(view.desc.Format);
    const auto start = view.desc.Buffer.FirstElement * element;
    const auto bytes = view.desc.Buffer.NumElements * element;
    if (!element || element == stride || start % stride || bytes < stride)
        return;
    auto made = view.desc;
    made.Format = DXGI_FORMAT_UNKNOWN;
    made.Buffer.FirstElement = start / stride;
    made.Buffer.NumElements = static_cast<UINT>(bytes / stride);
    made.Buffer.StructureByteStride = stride;
    made.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    if (!fit_buffer_view(view.resource, made))
        return;
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(device, 18)(
        device, view.resource, &made, Handle{slot});
    wrote_slot(slot);
    const std::lock_guard lock(memory_view_mutex);
    buffer_views[slot] = {view.resource, made};
}
// the copy a view of the title's memory is of, made again: `slot` is the host's
extern std::atomic<ID3D12Device *> d3d12_device;
extern std::atomic<bool> lost_again;
void refresh_memory_view(std::uint64_t slot) {
    MemoryView view;
    {
        const std::lock_guard lock(memory_view_mutex);
        const auto found = memory_views.find(slot);
        if (found == memory_views.end())
            return;
        view = found->second;
    }
    auto *const device = d3d12_device.load();
    if (!device)
        return;
    if (static std::atomic<int> shown{0};
        std::getenv("XV_D3D_LOST") && (shown++ < 12 || lost_again)) {
        std::uint64_t address = view.buffer.BufferLocation;
        if (!view.constants)
            std::memcpy(&address, view.resource + sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC),
                        sizeof address);
        else if (view.buffer.SizeInBytes >= 0x1a0)
            address += 0x160;
        float first[16]{};
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
            info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            std::memcpy(first, reinterpret_cast<const void *>(address), sizeof first);
        std::fprintf(stderr,
                     "[Lost] a view of memory at %llx (%s) made again: %g %g %g %g | %g %g %g %g "
                     "| %g %g %g %g | %g %g %g %g%c",
                     static_cast<unsigned long long>(address),
                     view.constants ? "constants" : "a buffer", first[0], first[1], first[2],
                     first[3], first[4], first[5], first[6], first[7], first[8], first[9],
                     first[10], first[11], first[12], first[13], first[14], first[15], 10);
    }
    if (view.constants)
        create_cbv(device, &view.buffer, view.handle);
    else
        create_srv_at(device, nullptr, view.resource, view.handle);
}
void STDMETHODCALLTYPE create_uav_at(ID3D12Device *self, const D3D12_RESOURCE_DESC *of,
                                     const void *desc, Handle handle) {
    if (!desc)
        return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC view;
    std::uint64_t address = 0;
    std::memcpy(&view, desc, sizeof view);
    std::memcpy(&address, static_cast<const std::uint8_t *>(desc) + sizeof view, sizeof address);
    view_at("an unordered access view of memory", of, view.ViewDimension, address,
            _ReturnAddress());
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, size = 0;
    if (view.ViewDimension != D3D12_UAV_DIMENSION_BUFFER ||
        !buffer_at(address, resource, offset, size)) {
        create_uav(self, nullptr, nullptr, null_view(view, of), handle);
        return;
    }
    const UINT element = view.Buffer.StructureByteStride ? view.Buffer.StructureByteStride
                         : view.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW
                             ? 4
                             : element_bytes(view.Format);
    if (element)
        view.Buffer.FirstElement += offset / element;
    create_uav(self, resource, nullptr, &view, handle);
}
// The console's GPU draws rectangle lists and quad lists (topology types 19 and 32, the
// topologies 17 and 19). Here both are triangles: a quad list through indices of ours (see
// draw_instanced), a rectangle list through a geometry shader of ours (see
// rectangle_pipeline).
constexpr D3D12_PRIMITIVE_TOPOLOGY_TYPE rectangle_type =
    static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(19);
D3D12_PRIMITIVE_TOPOLOGY_TYPE host_topology_type(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
    return type == 19 || type == 32 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : type;
}
// What a shader a pipeline was turned down with is made of: its container's parts by name
// and size. A console build may carry only code for the console's GPU. XV_SHADER_DUMP=<file>
// writes the first one there.
void describe_shader(const char *stage, const D3D12_SHADER_BYTECODE &code) {
    const auto *const bytes = static_cast<const std::uint8_t *>(code.pShaderBytecode);
    if (!bytes || code.BytecodeLength < 32)
        return;
    std::string parts;
    std::uint32_t count = 0;
    if (!std::memcmp(bytes, "DXBC", 4)) {
        std::memcpy(&count, bytes + 28, 4);
        for (std::uint32_t i = 0; i < std::min(count, 32u); ++i) {
            std::uint32_t offset = 0, size = 0;
            std::memcpy(&offset, bytes + 32 + 4 * i, 4);
            if (offset + 8 > code.BytecodeLength)
                break;
            std::memcpy(&size, bytes + offset + 4, 4);
            parts += ' ' + std::string(reinterpret_cast<const char *>(bytes + offset), 4) + ':' +
                     std::to_string(size);
        }
    }
    std::fprintf(stderr, "[Runtime]   %s shader, %zu bytes, starts %.4s, parts:%s\n", stage,
                 static_cast<std::size_t>(code.BytecodeLength), bytes, parts.c_str());
    if (static std::atomic<bool> dumped{false};
        std::getenv("XV_SHADER_DUMP") && !dumped.exchange(true))
        if (auto *const file = std::fopen(std::getenv("XV_SHADER_DUMP"), "wb")) {
            std::fwrite(bytes, 1, code.BytecodeLength, file);
            std::fclose(file);
        }
}
// A shader of the title's with only console GPU code is given to the host as the shader
// made for it (see console_shader.hpp)
std::span<const std::uint8_t> shader_bytes(const D3D12_SHADER_BYTECODE &code) {
    if (!code.pShaderBytecode)
        return {};
    return {static_cast<const std::uint8_t *>(code.pShaderBytecode), code.BytecodeLength};
}
bool host_bytecode(modules::ShaderStage stage, D3D12_SHADER_BYTECODE &code,
                   modules::HostShaderModel model = modules::HostShaderModel::sm5,
                   std::span<const modules::PackedInput> packed = {},
                   const void *signature = nullptr) {
    const auto container = shader_bytes(code);
    if (!code.pShaderBytecode || !modules::console_only_shader(container))
        return false;
    const auto made =
        modules::host_shader(stage, container, model, packed, signature_part(signature));
    if (made.empty())
        return false;
    code = {made.data(), made.size()};
    return true;
}

bool host_graphics_pair(ID3D12Device *device, D3D12_SHADER_BYTECODE &vs, D3D12_SHADER_BYTECODE &ps,
                        bool allow_sm6, std::span<const modules::PackedInput> packed = {},
                        const void *signature = nullptr) {
    using modules::HostShaderModel;
    using modules::ShaderStage;
    if (allow_sm6 && vs.pShaderBytecode && ps.pShaderBytecode) {
        auto pixel = ps;
        if (host_bytecode(ShaderStage::pixel, pixel, HostShaderModel::automatic, {}, signature)) {
            if (!modules::shader_has_dxil(shader_bytes(pixel))) {
                ps = pixel;
                host_bytecode(ShaderStage::vertex, vs, HostShaderModel::sm5, packed, signature);
                return true;
            }
            // A graphics pipeline cannot mix DXBC and DXIL on all supported drivers.
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
            auto vertex = vs;
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options,
                                                      sizeof options)) &&
                options.WaveOps &&
                SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                      sizeof model)) &&
                model.HighestShaderModel >= D3D_SHADER_MODEL_6_6 &&
                (modules::shader_has_dxil(shader_bytes(vertex)) ||
                 host_bytecode(ShaderStage::vertex, vertex, HostShaderModel::sm6, packed,
                               signature))) {
                vs = vertex;
                ps = pixel;
                return true;
            }
        }
    }
    const bool vertex =
        host_bytecode(ShaderStage::vertex, vs, HostShaderModel::sm5, packed, signature);
    const bool pixel = host_bytecode(ShaderStage::pixel, ps, HostShaderModel::sm5, {}, signature);
    return vertex || pixel;
}
// A pipeline without a root signature has it in its shaders, and the shader made for the
// host does not carry it. On the host a pipeline is bound to the root signature it was made
// with, and the title sets its own on its lists: the pipeline gets the newest one the title
// made of its kind (for drawing or for computing), or failing that the one in the shader.
ID3D12RootSignature *shader_root_signature(ID3D12Device *self, bool drawing,
                                           std::initializer_list<D3D12_SHADER_BYTECODE> shaders) {
    {
        const std::lock_guard lock(title_signature_mutex);
        for (auto known = title_signatures.rbegin(); known != title_signatures.rend(); ++known)
            if (known->input_layout == drawing) {
                known->signature->AddRef();
                return known->signature;
            }
    }
    for (const auto &shader : shaders) {
        ID3D12RootSignature *signature = nullptr;
        if (shader.pShaderBytecode &&
            SUCCEEDED(self->CreateRootSignature(0, shader.pShaderBytecode, shader.BytecodeLength,
                                                IID_PPV_ARGS(&signature))))
            return signature;
    }
    return nullptr;
}
// The host's pipeline of a title's, made now if it was not yet (see DeferredPipeline); null
// when the host turns it down
ID3D12PipelineState *host_pipeline(ID3D12PipelineState *state);
// the same for a list, which goes on without it while the host's driver is long making it
// (null then, as for one the host turns down)
ID3D12PipelineState *list_pipeline(ID3D12PipelineState *state);
HRESULT STDMETHODCALLTYPE deferred_graphics_pipeline(ID3D12Device *self,
                                                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                     REFIID iid, void **state);
HRESULT STDMETHODCALLTYPE deferred_compute_pipeline(ID3D12Device *self,
                                                    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                    REFIID iid, void **state);
// What each graphics pipeline was made from, as the host was given it: the console derives
// a pipeline from another with some of its state changed, and the host makes one whole.
// Shaders are kept once each, however many pipelines use them.
struct KeptPipeline {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::deque<std::string> names;
};
std::mutex kept_mutex;
std::map<const void *, KeptPipeline> kept_pipelines;
// A pipeline made of the programs of the title's cache (see deserialize_pipeline) is under
// a root signature of ours: its shaders read their descriptors from the heaps themselves,
// and are given what the programs have in their user registers as root constants (see
// rdna::Shader::heaps). `parameters`: the root parameters of the signature the pipeline was
// cached with, which is what those registers hold, in order. `uses`: the descriptors the
// shaders read, by the register their table is in and their place in it, and what each is;
// `samplers`: the registers that have the number of a sampler in the sampler heap. Those
// descriptors that the title wrote itself are made the host's before a draw.
struct HeapUses {
    // per stage (vertex, pixel): what its user registers hold - `kind` 1 a table, 2 a root
    // view, 5 constants; the first register, how many, the title's root parameter - then the
    // descriptors read and the registers that have a sampler's number
    struct Entry {
        int kind = 0;
        std::uint32_t reg = 8, count = 1, slot = 0;
    };
    struct Use {
        std::uint32_t reg, place;
        D3D12_DESCRIPTOR_RANGE_TYPE type;
        std::uint32_t stride = 0; // of a buffer's elements, as the shader reads them
    };
    struct Stage {
        std::vector<Entry> entries;
        std::vector<Use> uses;
        std::vector<std::uint32_t> samplers;
    };
    Stage stages[2];
    bool compute = false; // a compute pipeline's, whose one stage is the first
    std::string made;     // XV_D3D_LOST: what its shaders are kept as in the shader cache
};
// what a shader of a container says of its registers (modules::shader_heap_layout)
HeapUses::Stage heap_stage(const D3D12_SHADER_BYTECODE &code) {
    HeapUses::Stage stage;
    if (!code.pShaderBytecode)
        return stage;
    const auto layout = modules::shader_heap_layout(
        {static_cast<const std::uint8_t *>(code.pShaderBytecode), code.BytecodeLength});
    for (const auto &entry : layout.entries)
        stage.entries.push_back({entry.type == 3   ? 1
                                 : entry.type == 1 ? 2
                                                   : 5,
                                 entry.reg, entry.count, entry.slot});
    for (const auto &read : layout.reads)
        if (read.type == 3)
            stage.samplers.push_back(read.reg);
        else
            stage.uses.push_back({read.reg, read.place,
                                  static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(read.type),
                                  read.stride});
    return stage;
}
std::map<const void *, std::shared_ptr<const HeapUses>> heap_pipelines; // under kept_mutex
std::shared_ptr<const HeapUses> heap_uses(const void *pipeline) {
    const std::lock_guard lock(kept_mutex);
    const auto found = heap_pipelines.find(pipeline);
    return found == heap_pipelines.end() ? nullptr : found->second;
}
// that root signature: 24 constants for the vertex or mesh shader (b0) and as many for the
// pixel shader (b1), both in space 200, the sampler the shaders sample with, and the heaps indexed
// by the shaders; then a root view of each kind (t0, b0 and u0 of space 201) for the one a
// shader may read. Null where the host has no such heaps.
ID3D12RootSignature *heap_signature(ID3D12Device *device);
// set while a pipeline the host turned down with such shaders is made again the title's way
thread_local bool heap_shaders_refused = false;
// What the host turns down with shaders that read the heaps (NVIDIA: E_OUTOFMEMORY for some)
// is made again with the title's signature and its ranges: true when the caller should
// make it once more, as it does then.
bool retry_without_heaps(HRESULT result, bool heaps) {
    if (SUCCEEDED(result) || !heaps || heap_shaders_refused)
        return false;
    static std::atomic<unsigned> refused{0};
    if (const auto count = ++refused; std::has_single_bit(count)) {
        std::fprintf(stderr,
                     "[Runtime] %u pipelines whose shaders read the heaps themselves the host "
                     "turns down (0x%08lx): they are made the title's way%c",
                     count, static_cast<unsigned long>(result), 10);
        std::fflush(stderr);
    }
    return true;
}
bool heap_shaders_wanted(ID3D12Device *device) {
    if (heap_shaders_refused)
        return false;
    // what the host turns down this way is made again the title's way (retry_without_heaps);
    // XV_HEAP_SHADERS=0 keeps every pipeline the title's way
    static const bool on = [] {
        const auto *const text = std::getenv("XV_HEAP_SHADERS");
        return !text || *text != '0';
    }();
    return on && console == Target::scarlett && heap_signature(device);
}
// A shader of the Series console is made to read the heaps itself where its file says what
// its user registers hold and what it reads at each place of a table; one of the earlier
// kits, whose file has neither (Fortnite's), keeps the title's signature.
bool heap_layout_known(const D3D12_SHADER_BYTECODE &code) {
    return code.pShaderBytecode && modules::console_only_shader(shader_bytes(code)) &&
           !modules::shader_heap_layout(shader_bytes(code)).entries.empty();
}
bool heap_compute_shader(ID3D12Device *device, const D3D12_SHADER_BYTECODE &code) {
    return heap_shaders_wanted(device) && heap_layout_known(code);
}
std::shared_ptr<const HeapUses> compute_heap_uses(const D3D12_SHADER_BYTECODE &code) {
    auto uses = std::make_shared<HeapUses>();
    uses->stages[0] = heap_stage(code);
    uses->compute = true;
    uses->made = "a container's";
    return uses;
}
ID3D12RootSignature *heap_signature(ID3D12Device *device) {
    static std::once_flag once;
    static ID3D12RootSignature *signature = nullptr;
    std::call_once(once, [&] {
        using Serialize = HRESULT(WINAPI *)(const void *, void **, void **);
        const auto serialize = host_d3d12<Serialize>("D3D12SerializeVersionedRootSignature");
        D3D12_ROOT_PARAMETER1 parameters[5]{};
        for (UINT n = 0; n < 2; ++n) {
            parameters[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[n].Constants = {n, 200, 24};
            parameters[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        for (UINT n = 2; n < 5; ++n) {
            parameters[n].Descriptor = {0, 201, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
            parameters[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        // s0: the one shaders sample with when they make a sampler of words in their code;
        // s1: the same, for a shader that compares with it (a shadow map's)
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (UINT n = 0; n < 2; ++n) {
            auto &sampler = samplers[n];
            sampler.Filter = n ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                               : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW =
                n ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.ComparisonFunc =
                n ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_NEVER;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = n;
            sampler.RegisterSpace = 200;
        }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = 5;
        desc.Desc_1_1.pParameters = parameters;
        desc.Desc_1_1.NumStaticSamplers = 2;
        desc.Desc_1_1.pStaticSamplers = samplers;
        // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED and
        // SAMPLER_HEAP_DIRECTLY_INDEXED
        desc.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x1 | 0x400 | 0x800);
        ID3DBlob *blob = nullptr, *said = nullptr;
        auto result = serialize ? serialize(&desc, reinterpret_cast<void **>(&blob),
                                            reinterpret_cast<void **>(&said))
                                : E_NOTIMPL;
        if (SUCCEEDED(result)) {
            using Create =
                HRESULT(STDMETHODCALLTYPE *)(void *, UINT, const void *, SIZE_T, REFIID, void **);
            result = original<Create>(device, 16)(device, 0, blob->GetBufferPointer(),
                                                  blob->GetBufferSize(), IID_PPV_ARGS(&signature));
        }
        if (FAILED(result)) {
            std::fprintf(stderr,
                         "[Runtime] no root signature with heaps the shaders index (0x%08lx): "
                         "the pipelines of the title's cache draw nothing%c",
                         static_cast<unsigned long>(result), 10);
            std::fflush(stderr);
        }
        if (blob)
            blob->Release();
        if (said)
            said->Release();
    });
    return signature;
}
std::map<std::uint64_t, std::vector<std::uint8_t>> kept_shaders;
D3D12_SHADER_BYTECODE keep_shader(const D3D12_SHADER_BYTECODE &code) {
    if (!code.pShaderBytecode || !code.BytecodeLength)
        return {};
    const auto *const bytes = static_cast<const std::uint8_t *>(code.pShaderBytecode);
    std::uint64_t hash = 0xcbf29ce484222325ull ^ code.BytecodeLength;
    for (SIZE_T i = 0; i < code.BytecodeLength; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    auto &kept = kept_shaders[hash];
    if (kept.empty())
        kept.assign(bytes, bytes + code.BytecodeLength);
    return {kept.data(), kept.size()};
}
// XV_D3D_LOST: how each pipeline was made, for the draw that the host drops the device at
std::map<const void *, std::string> made_pipelines;
// XV_D3D_LOST_AT=<n>: what is said of the first draws is said again of the twelve from draw
// n on, when the title has got further
std::atomic<bool> lost_again{false};
void lost_pipeline(const char *how, void *const *pipeline, const void *root) {
    static const bool checking = std::getenv("XV_D3D_LOST") != nullptr;
    if (!checking || !pipeline || !*pipeline)
        return;
    char text[40];
    std::snprintf(text, sizeof text, ", root signature %p", root);
    const std::lock_guard lock(kept_mutex);
    made_pipelines[*pipeline] = std::string(how) + text;
}
void keep_pipeline(const void *pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
    const std::lock_guard lock(kept_mutex);
    auto &kept = kept_pipelines[pipeline];
    kept = {};
    kept.desc = desc;
    for (auto *const shader :
         {&kept.desc.VS, &kept.desc.PS, &kept.desc.DS, &kept.desc.HS, &kept.desc.GS})
        *shader = keep_shader(*shader);
    kept.desc.StreamOutput = {};
    kept.desc.CachedPSO = {};
    for (UINT i = 0; i < desc.InputLayout.NumElements && desc.InputLayout.pInputElementDescs; ++i) {
        auto element = desc.InputLayout.pInputElementDescs[i];
        element.SemanticName = kept.names.emplace_back(element.SemanticName).c_str();
        kept.elements.push_back(element);
    }
    kept.desc.InputLayout.pInputElementDescs = kept.elements.data();
    if (kept.desc.pRootSignature)
        kept.desc.pRootSignature->AddRef(); // for as long as it may be derived from
}
// CreateDerivedGraphicsPipelineStateX (55)
struct DerivedState {
    UINT type; // 0 depth and stencil, 1 rasterizer, 2 blend, 3 sample mask, 4 and up: what
               // only the console's GPU has a use for
    union {
        D3D12_DEPTH_STENCIL_DESC depth_stencil;
        D3D12_RASTERIZER_DESC rasterizer;
        D3D12_BLEND_DESC blend;
        UINT sample_mask;
    };
};
HRESULT STDMETHODCALLTYPE create_derived_graphics_pipeline(ID3D12Device *self,
                                                           ID3D12PipelineState *source, UINT count,
                                                           const DerivedState *states, REFIID iid,
                                                           void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    if (!source || !state || (count && !states))
        return E_INVALIDARG;
    source = host_pipeline(source);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    {
        const std::lock_guard lock(kept_mutex);
        const auto found = kept_pipelines.find(source);
        if (found == kept_pipelines.end()) {
            std::fprintf(stderr, "[Runtime] CreateDerivedGraphicsPipelineStateX from a pipeline "
                                 "that was not made here\n");
            return E_INVALIDARG;
        }
        desc = found->second.desc;
    }
    for (UINT i = 0; i < count; ++i)
        if (states[i].type == 0)
            desc.DepthStencilState = states[i].depth_stencil;
        else if (states[i].type == 1)
            desc.RasterizerState = states[i].rasterizer;
        else if (states[i].type == 2)
            desc.BlendState = states[i].blend;
        else if (states[i].type == 3)
            desc.SampleMask = states[i].sample_mask;
    const auto result = original<Create>(self, 10)(self, &desc, iid, state);
    if (SUCCEEDED(result))
        lost_pipeline("derived", state, desc.pRootSignature);
    if (SUCCEEDED(result) && *state) {
        keep_pipeline(*state, desc);
        if (const auto uses = heap_uses(source)) {
            const std::lock_guard lock(kept_mutex);
            heap_pipelines[*state] = uses;
        }
    } else {
        std::fprintf(stderr, "[Runtime] CreateDerivedGraphicsPipelineStateX: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// A pipeline that draws rectangle lists gets the geometry shader that makes the fourth
// corner of each, unless it has one of the title's. A rectangle faces no way: it is not
// culled. Without the shader the triangle of the three corners is drawn, half the rectangle.
bool rectangle_pipeline(D3D12_GRAPHICS_PIPELINE_STATE_DESC &host) {
    if (!host.GS.pShaderBytecode && host.VS.pShaderBytecode) {
        const auto corner = modules::rectangle_shader(
            {static_cast<const std::uint8_t *>(host.VS.pShaderBytecode), host.VS.BytecodeLength});
        if (!corner.empty()) {
            host.GS = {corner.data(), corner.size()};
            host.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            return true;
        }
    }
    return false;
}
void report_half_rectangles() {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a rectangle list is drawn as the triangles of its three "
                             "corners: half of each rectangle is missing\n");
        std::fflush(stderr);
    }
}
// XV_PSO_TIME: how many pipelines of each kind the title has made, how long that took in
// all and in the host's driver, said every five seconds with the frame it is at
struct PipelineTime {
    std::atomic<std::uint64_t> count{0}, total{0}, host{0};
};
PipelineTime pipeline_times[3]; // graphics, compute, stream
std::uint64_t microseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
struct PipelineClock {
    int kind;
    std::uint64_t started = microseconds();
    static thread_local int depth;
    PipelineClock(int which) : kind(which) { ++depth; }
    ~PipelineClock() {
        if (--depth)
            return;
        static const bool timing = std::getenv("XV_PSO_TIME") != nullptr;
        if (!timing)
            return;
        auto &time = pipeline_times[kind];
        ++time.count;
        time.total += microseconds() - started;
        static std::atomic<std::uint64_t> said{0};
        const auto now = microseconds();
        if (auto last = said.load();
            now - last > 5'000'000 && said.compare_exchange_strong(last, now)) {
            std::fprintf(stderr, "[Runtime] pipelines at frame %llu:",
                         static_cast<unsigned long long>(frames_shown()));
            const char *const names[] = {"graphics", "compute", "stream"};
            for (int i = 0; i < 3; ++i)
                std::fprintf(stderr, " %s %llu in %.1f s (host %.1f s);", names[i],
                             static_cast<unsigned long long>(pipeline_times[i].count.load()),
                             pipeline_times[i].total / 1e6, pipeline_times[i].host / 1e6);
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
    }
};
thread_local int PipelineClock::depth = 0;
struct HostClock {
    int kind;
    std::uint64_t started = microseconds();
    ~HostClock() { pipeline_times[kind].host += microseconds() - started; }
};
// CreateGraphicsPipelineState (10): the console's flags, as for compute pipelines
HRESULT STDMETHODCALLTYPE create_graphics_pipeline(ID3D12Device *self,
                                                   const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                   REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    const PipelineClock clock{0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC host{};
    ID3D12RootSignature *from_shader = nullptr;
    std::shared_ptr<const HeapUses> uses;
    bool tried_heaps = false;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<modules::PackedInput> packed;
    if (desc) {
        host = *desc;
        host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        host.DSVFormat = host_format(host.DSVFormat);
        for (auto &target : host.RTVFormats)
            target = target_format(target);
        // The console's R10G10B10_SNORM_A2_UNORM (189), what packed normals are in, is no
        // vertex format of the host's: its bits are read unsigned there, and a vertex shader
        // made here makes them signed again
        if (host.InputLayout.pInputElementDescs) {
            elements.assign(host.InputLayout.pInputElementDescs,
                            host.InputLayout.pInputElementDescs + host.InputLayout.NumElements);
            for (auto &element : elements)
                if (element.Format == XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM) {
                    element.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                    if (element.SemanticName)
                        packed.push_back({element.SemanticName, element.SemanticIndex});
                }
            host.InputLayout.pInputElementDescs = elements.data();
        }
        host.PrimitiveTopologyType = host_topology_type(host.PrimitiveTopologyType);
        // the formats it draws to, the host's (see host_format)
        for (auto &format : host.RTVFormats)
            format = host_format(self, format, FormatUse::target_view);
        host.DSVFormat = host_format(self, host.DSVFormat, FormatUse::depth_view);
        const auto geometry = host.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                  ? modules::ShaderStage::geometry_points
                              : host.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                  ? modules::ShaderStage::geometry_lines
                                  : modules::ShaderStage::geometry_triangles;
        const bool simple = desc->PrimitiveTopologyType != rectangle_type &&
                            !host.GS.pShaderBytecode && !host.HS.pShaderBytecode &&
                            !host.DS.pShaderBytecode;
        // On the Series console the two are translated to read the heaps themselves, and
        // the pipeline is under a root signature of ours (see HeapUses), with
        // XV_HEAP_SHADERS=1; else the title's signature and its ranges are kept.
        const bool heaps = simple && heap_shaders_wanted(self) && host.VS.pShaderBytecode &&
                           host.PS.pShaderBytecode && heap_layout_known(host.VS) &&
                           heap_layout_known(host.PS);
        tried_heaps = heaps;
        modules::use_heap_shaders(heaps);
        const bool pair =
            host_graphics_pair(self, host.VS, host.PS, simple, packed, host.pRootSignature);
        modules::use_heap_shaders(false);
        if (heaps && pair) {
            auto reads = std::make_shared<HeapUses>();
            reads->stages[0] = heap_stage(desc->VS);
            reads->stages[1] = heap_stage(desc->PS);
            reads->made = "a container's";
            uses = std::move(reads);
            host.pRootSignature = heap_signature(self);
        }
        const bool made = host_bytecode(geometry, host.GS) || pair;
        if (made && !host.pRootSignature)
            host.pRootSignature = from_shader =
                shader_root_signature(self, true, {desc->VS, desc->PS, desc->GS});
    }
    const bool rectangles = desc && desc->PrimitiveTopologyType == rectangle_type;
    const auto cull = host.RasterizerState.CullMode;
    const bool cornered = rectangles && rectangle_pipeline(host);
    HRESULT result;
    {
        const HostClock timed{0};
        result = original<Create>(self, 10)(self, desc ? &host : nullptr, iid, state);
    }
    // the host's compiler may lay the shader's inputs out another way than the title's
    // vertex shader its outputs: the pipeline is then made without it
    if (FAILED(result) && cornered) {
        host.GS = {};
        host.RasterizerState.CullMode = cull;
        result = original<Create>(self, 10)(self, &host, iid, state);
    }
    if (rectangles && (!cornered || !host.GS.pShaderBytecode))
        report_half_rectangles();
    if (retry_without_heaps(result, tried_heaps)) {
        if (from_shader)
            from_shader->Release();
        heap_shaders_refused = true;
        result = create_graphics_pipeline(self, desc, iid, state);
        heap_shaders_refused = false;
        return result;
    }
    if (SUCCEEDED(result) && state)
        lost_pipeline(from_shader ? "graphics, signature from its shaders" : "graphics", state,
                      host.pRootSignature);
    if (SUCCEEDED(result) && desc && state && *state && console == Target::scarlett)
        keep_pipeline(*state, host);
    if (SUCCEEDED(result) && state && *state && uses) {
        const std::lock_guard lock(kept_mutex);
        heap_pipelines[*state] = uses;
    }
    if (from_shader)
        from_shader->Release();
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreateGraphicsPipelineState: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        // the device went before this: what the host knows of why
        if (static std::atomic<bool> told{false};
            result == DXGI_ERROR_DEVICE_REMOVED && !told.exchange(true))
            report_failure();
        if (static std::atomic<int> shown{0}; desc && shown++ < 2) {
            describe_shader("vertex", desc->VS);
            describe_shader("pixel", desc->PS);
            const auto bytes = [](const D3D12_SHADER_BYTECODE &code) {
                return std::span<const std::uint8_t>(
                    static_cast<const std::uint8_t *>(code.pShaderBytecode), code.BytecodeLength);
            };
            std::fprintf(stderr, "[Runtime]   vertex %s\n[Runtime]   pixel %s\n",
                         modules::shader_signature(bytes(desc->VS)).c_str(),
                         modules::shader_signature(bytes(desc->PS)).c_str());
            if (desc->GS.pShaderBytecode)
                std::fprintf(stderr, "[Runtime]   geometry %s\n",
                             modules::shader_signature(bytes(desc->GS)).c_str());
            std::fprintf(stderr,
                         "[Runtime]   topology %d, %u targets of format %d, depth %d, "
                         "samples %u, layout:",
                         host.PrimitiveTopologyType, host.NumRenderTargets, host.RTVFormats[0],
                         host.DSVFormat, host.SampleDesc.Count);
            for (UINT i = 0; i < host.InputLayout.NumElements; ++i)
                std::fprintf(stderr, " %s%u/%d",
                             host.InputLayout.pInputElementDescs[i].SemanticName,
                             host.InputLayout.pInputElementDescs[i].SemanticIndex,
                             host.InputLayout.pInputElementDescs[i].Format);
            std::fprintf(stderr, "\n");
            describe_shader("geometry", desc->GS);
            describe_shader("hull", desc->HS);
            describe_shader("domain", desc->DS);
        }
        report_messages();
    }
    return result;
}
// slot 74: RegisterPagePoolX(address, page count, handle): the pages are the title's memory,
// mapped for the GPU by later page mapping calls; here the pool is only a handle
HRESULT STDMETHODCALLTYPE register_page_pool(ID3D12Device *, std::uint64_t address, UINT,
                                             HANDLE *handle) {
    if (!handle)
        return E_POINTER;
    *handle = reinterpret_cast<HANDLE>(address);
    return S_OK;
}
// Shaders that end the process inside the host's driver when a pipeline is made of them (a
// fail-fast, which nothing here can catch), by the digest of their container:
// XboxBvhBuilder's LeafRehydration on NVIDIA 581.15.
bool ends_host_driver(const D3D12_SHADER_BYTECODE &code) {
    static constexpr std::uint8_t digests[][16]{{0x79, 0x8a, 0x9e, 0xfc, 0xf0, 0xc2, 0x44, 0x93,
                                                 0x71, 0xee, 0xde, 0xf4, 0xa4, 0xb8, 0x35, 0xf0}};
    const auto *const bytes = static_cast<const std::uint8_t *>(code.pShaderBytecode);
    if (!bytes || code.BytecodeLength < 20 || std::memcmp(bytes, "DXBC", 4))
        return false;
    for (const auto &digest : digests)
        if (!std::memcmp(bytes + 4, digest, sizeof digest))
            return true;
    return false;
}
// CreateComputePipelineState (11)
HRESULT STDMETHODCALLTYPE create_compute_pipeline(ID3D12Device *self,
                                                  const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                  REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    const PipelineClock clock{1};
    // the console's pipeline flags mean nothing to the host, which takes none of them
    D3D12_COMPUTE_PIPELINE_STATE_DESC host{};
    ID3D12RootSignature *from_shader = nullptr;
    std::shared_ptr<const HeapUses> uses;
    bool heaps = false;
    if (desc) {
        host = *desc;
        host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        heaps = heap_compute_shader(self, desc->CS);
        modules::use_heap_shaders(heaps);
        const bool made = host_bytecode(modules::ShaderStage::compute, host.CS,
                                        modules::HostShaderModel::sm5, {}, host.pRootSignature);
        modules::use_heap_shaders(false);
        if (made && heaps) {
            uses = compute_heap_uses(desc->CS);
            host.pRootSignature = heap_signature(self);
        } else if (made && !host.pRootSignature) {
            host.pRootSignature = from_shader = shader_root_signature(self, false, {desc->CS});
        }
    }
    // a shader of the title's own that the host's driver cannot make gets an empty one in
    // its place, with the title's root signature: the title goes on, and what the shader
    // computes is missing
    bool emptied = false;
    const auto empty = [&] {
        const auto shader = modules::empty_compute_shader();
        if (shader.empty() || from_shader || !desc || !desc->CS.pShaderBytecode)
            return false;
        host.CS = {shader.data(), shader.size()};
        return emptied = true;
    };
    if (desc && ends_host_driver(desc->CS))
        empty();
    HRESULT result;
    {
        const HostClock timed{1};
        result = original<Create>(self, 11)(self, desc ? &host : nullptr, iid, state);
    }
    if (retry_without_heaps(result, heaps)) {
        heap_shaders_refused = true;
        result = create_compute_pipeline(self, desc, iid, state);
        heap_shaders_refused = false;
        return result;
    }
    if (FAILED(result) && !emptied && desc && host.CS.pShaderBytecode == desc->CS.pShaderBytecode &&
        empty())
        result = original<Create>(self, 11)(self, &host, iid, state);
    if (SUCCEEDED(result) && state)
        lost_pipeline("compute", state, host.pRootSignature);
    if (SUCCEEDED(result) && state && *state && uses) {
        const std::lock_guard lock(kept_mutex);
        heap_pipelines[*state] = uses;
    }
    if (emptied) {
        std::fprintf(stderr,
                     "[Runtime] a compute shader of %zu bytes that the host's driver cannot "
                     "make has an empty one in its place: 0x%08lx\n",
                     desc->CS.BytecodeLength, static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    if (from_shader)
        from_shader->Release();
    if (FAILED(result) && desc) {
        const auto *bytes = static_cast<const std::uint8_t *>(desc->CS.pShaderBytecode);
        std::fprintf(stderr,
                     "[Runtime] CreateComputePipelineState: 0x%08lx, shader of %zu bytes "
                     "starting %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     static_cast<unsigned long>(result), desc->CS.BytecodeLength,
                     bytes ? bytes[0] : 0, bytes ? bytes[1] : 0, bytes ? bytes[2] : 0,
                     bytes ? bytes[3] : 0, bytes ? bytes[4] : 0, bytes ? bytes[5] : 0,
                     bytes ? bytes[6] : 0, bytes ? bytes[7] : 0);
        // DXBC: magic, digest, version, size, chunk count, chunk offsets; each chunk a tag
        if (bytes && desc->CS.BytecodeLength >= 32) {
            std::uint32_t chunks = 0;
            std::memcpy(&chunks, bytes + 28, 4);
            std::string tags;
            for (std::uint32_t i = 0; i < chunks && 32 + i * 4 + 4 <= desc->CS.BytecodeLength;
                 ++i) {
                std::uint32_t offset = 0;
                std::memcpy(&offset, bytes + 32 + i * 4, 4);
                if (offset + 8 <= desc->CS.BytecodeLength) {
                    std::uint32_t size = 0;
                    std::memcpy(&size, bytes + offset + 4, 4);
                    tags += ' ';
                    tags.append(reinterpret_cast<const char *>(bytes + offset), 4);
                    tags += ':' + std::to_string(size);
                }
            }
            std::fprintf(stderr, "[Runtime] its chunks:%s\n", tags.c_str());
        }
        // XV_D3D_TRACE: the first shaders the host turned down, to look at
        if (static std::atomic<int> dumped{0};
            bytes && std::getenv("XV_D3D_TRACE") && std::getenv("TEMP") && dumped < 4)
            if (auto *const file = std::fopen((std::string(std::getenv("TEMP")) + "\\xv_shader_" +
                                               std::to_string(dumped++) + ".bin")
                                                  .c_str(),
                                              "wb")) {
                std::fwrite(bytes, 1, desc->CS.BytecodeLength, file);
                std::fclose(file);
            }
        std::fflush(stderr);
        report_messages();
    }
    return result;
}
// XV_D3D_LOST: the first call after which the host has dropped the device, and the calls
// before it
void lost_check(const char *kind, std::size_t index, void *from, void *object = nullptr);
// ID3D12CommandQueue goes on past the desktop's 19 methods with the console's own
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE unknown_queue_method(void *, std::uint64_t a, std::uint64_t b,
                                                     std::uint64_t c) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: command queue slot %zu (%llx, "
                     "%llx, %llx) from %s\n",
                     Index, a, b, c, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue *self, UINT count,
                                             ID3D12CommandList *const *lists);
HRESULT STDMETHODCALLTYPE ignored_queue_method(void *) { return S_OK; }
// slot 23: SignalX(fence, value, flush flags), by Unreal's message; the host's caches need no
// flushing asked for
HRESULT STDMETHODCALLTYPE signal_x(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value,
                                   UINT) {
    return self->Signal(fence, value);
}
constexpr std::size_t host_queue_slots = 19, queue_slots = 48;
template <std::size_t... Index>
std::vector<std::pair<std::size_t, Slot>> unknown_queue_methods(std::index_sequence<Index...>) {
    return {{host_queue_slots + Index,
             reinterpret_cast<Slot>(&unknown_queue_method<host_queue_slots + Index>)}...};
}
// slot 62: CreateComputePipelineState as the console has it (Unreal's message names it): two
// more arguments, 0 at its call site, before the interface and the state
HRESULT STDMETHODCALLTYPE create_compute_pipeline_x(ID3D12Device *self,
                                                    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                    void *, void *, REFIID iid, void **state) {
    return deferred_compute_pipeline(self, desc, iid, state);
}
// The title's command queue: a proxy of the host's, which the host's runtime and DXGI keep as
// their own. The console's queue has the desktop's methods to Wait (15), then PIX captures,
// SuspendX, ResumeX, SignalX (23), ..., GetDesc (26) and its page mapping methods.
struct QueueProxy {
    Slot *vtable;
    ID3D12CommandQueue *host;
    std::atomic<ULONG> references;
};
ID3D12CommandQueue *queue_host(void *self) { return static_cast<QueueProxy *>(self)->host; }
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE queue_method(void *self, std::uint64_t a, std::uint64_t b,
                                             std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                             std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                             std::uint64_t i, std::uint64_t j) {
    auto *const host = queue_host(self);
    return reinterpret_cast<Raw>((*reinterpret_cast<Slot **>(host))[Index])(host, a, b, c, d, e, f,
                                                                            g, h, i, j);
}
HRESULT STDMETHODCALLTYPE queue_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    if (is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandQueue>(
            iid)) {
        ++static_cast<QueueProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return queue_host(self)->QueryInterface(iid, object);
}
ULONG STDMETHODCALLTYPE queue_add_ref(void *self) {
    return ++static_cast<QueueProxy *>(self)->references;
}
void note_queue_wait(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value);
void forget_queue(ID3D12CommandQueue *queue);
ULONG STDMETHODCALLTYPE queue_release(void *self) {
    auto *const proxy = static_cast<QueueProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        forget_queue(proxy->host);
        proxy->host->Release();
        delete proxy;
    }
    return left;
}
// Wait (15): the host's, noted for order_queues
HRESULT STDMETHODCALLTYPE queue_wait(void *self, ID3D12Fence *fence, UINT64 value) {
    const auto result = queue_host(self)->Wait(fence, value);
    if (SUCCEEDED(result))
        note_queue_wait(queue_host(self), fence, value);
    return result;
}
void STDMETHODCALLTYPE queue_execute(void *self, UINT count, ID3D12CommandList *const *lists) {
    execute_command_lists(queue_host(self), count, lists);
    // what the debug layer has to say of the draws, which no call's result shows
    report_messages();
}
HRESULT STDMETHODCALLTYPE queue_signal_x(void *self, ID3D12Fence *fence, UINT64 value, UINT) {
    return queue_host(self)->Signal(fence, value);
}
D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE queue_get_desc(void *self,
                                                           D3D12_COMMAND_QUEUE_DESC *desc) {
    using Get = D3D12_COMMAND_QUEUE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_COMMAND_QUEUE_DESC *);
    auto *const host = queue_host(self);
    return reinterpret_cast<Get>((*reinterpret_cast<Slot **>(host))[18])(host, desc);
}
// slot 29: CopyPageMappingsBatchX(count, batches, pool, pool pages, flags). A title maps
// physical pages into a pool of its memory and has the GPU map them into a heap it reserved:
// each batch is the runs of pages from a destination on, each run of the pool's pages from
// `start` on (0), of its page `start` over and over (1), unmapped (2) or left as it is (3).
// Done at once, which work submitted before it has seen to here.
struct PageRun {
    UINT kind, start, count;
};
struct PageBatch {
    std::uint64_t destination;
    UINT count;
    const PageRun *runs;
};
HRESULT STDMETHODCALLTYPE queue_copy_page_mappings(void *, UINT count, const PageBatch *batches,
                                                   std::uint64_t pool, UINT pool_pages, UINT) {
    constexpr std::uint64_t page = 1 << 16;
    if (count && !batches)
        return E_INVALIDARG;
    bool mapped = true;
    for (const auto &batch : std::span(batches, count)) {
        auto at = batch.destination;
        for (const auto &run : std::span(batch.runs, batch.runs ? batch.count : 0)) {
            const auto bytes = std::uint64_t{run.count} * page;
            const auto source = pool + std::uint64_t{run.start} * page;
            const auto inside = [&](std::uint64_t pages) {
                return std::uint64_t{run.start} + pages <= pool_pages;
            };
            switch (run.kind) {
            case 0:
                mapped = inside(run.count) && map_alias(at, source, bytes) && mapped;
                break;
            case 1:
                for (UINT i = 0; i < run.count; ++i)
                    mapped = inside(1) && map_alias(at + i * page, source, page) && mapped;
                break;
            case 2:
                map_alias(at, 0, bytes);
                break;
            default:
                break;
            }
            at += bytes;
        }
    }
    if (static std::atomic<bool> shown{false}; !mapped && !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a GPU page mapping from the pool at %llx failed\n",
                     static_cast<unsigned long long>(pool));
        std::fflush(stderr);
    }
    return S_OK;
}
// slot 30: PresentX. The console shows the title's own resources; there is no swap chain.
HRESULT STDMETHODCALLTYPE queue_present_x(void *self, UINT planes, const void *plane_parameters,
                                          const void *present_parameters) {
    return present_x(queue_host(self), planes, plane_parameters, present_parameters);
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE unknown_queue_slot(void *, std::uint64_t a, std::uint64_t b) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: command queue slot %zu (%llx, "
                     "%llx) from %s\n",
                     Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
template <std::size_t... Index>
std::array<Slot, 48> queue_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, 48> vtable{(Index < 18
                                     ? reinterpret_cast<Slot>(&queue_method<Index>)
                                     : reinterpret_cast<Slot>(&unknown_queue_slot<Index>))...};
    vtable[0] = reinterpret_cast<Slot>(&queue_query);
    vtable[1] = reinterpret_cast<Slot>(&queue_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&queue_release);
    vtable[10] = reinterpret_cast<Slot>(&queue_execute);
    vtable[15] = reinterpret_cast<Slot>(&queue_wait);
    // The later layout's 35 (33 here, see queue_by_layout): ExecuteCommandListsX(count,
    // lists, a value, flags), which a title of the later kits submits all its lists with
    vtable[33] = reinterpret_cast<Slot>(&queue_execute);
    for (const std::size_t ignored : {18, 19, 20, 21, 22, 24, 25})
        vtable[ignored] = reinterpret_cast<Slot>(&ignored_queue_method);
    vtable[23] = reinterpret_cast<Slot>(&queue_signal_x);
    vtable[26] = reinterpret_cast<Slot>(&queue_get_desc);
    vtable[29] = reinterpret_cast<Slot>(&queue_copy_page_mappings);
    vtable[30] = reinterpret_cast<Slot>(&queue_present_x);
    // SetHDRToneMapperX: the picture goes to the host's display as the title made it
    vtable[31] = reinterpret_cast<Slot>(&ignored_queue_method);
    return vtable;
}
std::array<Slot, 48> queue_vtable = queue_vtable_of(std::make_index_sequence<48>{});
// Titles built with later headers have two more methods before Signal: it is at 16 there
// and PresentX at 32. Which two is not known; they lie among the event markers, which do
// nothing here. Neither the version a title creates its device with nor its system DLLs
// tell the two layouts apart, so the calls do: Signal and Wait are given a fence, which
// nothing else at their places is, and only the later layout has a method at 32.
bool is_object(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto vtable = *reinterpret_cast<const void *const *>(address);
    // a host object, or one whose vtable is a console one of ours (a fence's: see console_fence)
    return vtable && VirtualQuery(vtable, &region, sizeof region) && region.State == MEM_COMMIT &&
           (region.Type == MEM_IMAGE || console_vtable_at(vtable));
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE queue_by_layout(void *self, std::uint64_t a, std::uint64_t b,
                                                std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                                std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                                std::uint64_t i, std::uint64_t j) {
    const bool fence = (Index >= 14 && Index <= 17) && is_object(a);
    if (Index >= 32 || (Index >= 16 && fence))
        later_queue = true;
    std::size_t slot = Index;
    if (later_queue) {
        if (Index < 16)
            return 0; // markers, and the two methods that are not known
        slot = Index - 2;
    } else if (Index < 16 && !fence) {
        return 0; // a marker of a later title that has not signalled yet
    }
    const auto result =
        reinterpret_cast<Raw>(queue_vtable[slot])(self, a, b, c, d, e, f, g, h, i, j);
    lost_check("queue", slot, _ReturnAddress(), self);
    return result;
}
template <std::size_t... Index>
std::array<Slot, 48> layout_queue_vtable_of(std::index_sequence<Index...>) {
    auto vtable = queue_vtable;
    ((vtable[14 + Index] = reinterpret_cast<Slot>(&queue_by_layout<14 + Index>)), ...);
    return vtable;
}
std::array<Slot, 48> layout_queue_vtable = layout_queue_vtable_of(std::make_index_sequence<34>{});
void *make_queue_proxy(ID3D12CommandQueue *host) {
    return new QueueProxy{layout_queue_vtable.data(), host, 1};
}
// slot 48: CreateCommandQueue(desc, iid, queue), by its arguments at Unreal's call site
HRESULT STDMETHODCALLTYPE create_command_queue(ID3D12Device *self,
                                               const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid,
                                               void **queue) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
    const auto result = original<Create>(self, 8)(self, desc, iid, queue);
    if (FAILED(result) && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommandQueue type %d priority %d flags 0x%x node 0x%x: "
                     "0x%08lx%c",
                     desc->Type, desc->Priority, desc->Flags, desc->NodeMask,
                     static_cast<unsigned long>(result), 10);
        std::fflush(stderr);
    }
    auto *const host =
        SUCCEEDED(result) && queue ? static_cast<ID3D12CommandQueue *>(*queue) : nullptr;
    if (host)
        *queue = make_queue_proxy(host);
    if (SUCCEEDED(result) && queue && *queue && desc &&
        desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12CommandQueue *expected = nullptr;
        if (first_direct_queue.compare_exchange_strong(expected, host))
            first_direct_queue.load()->AddRef();
    }
    return result;
}
// CreateCommandQueueX (48): the type, the flags, and which engine and queue of the hardware.
// The copy engine's queue (type 16) is a direct queue here, whose lists take any transition.
HRESULT STDMETHODCALLTYPE create_command_queue_x(ID3D12Device *self, const UINT *desc, REFIID iid,
                                                 void **queue) {
    if (!desc)
        return E_INVALIDARG;
    const D3D12_COMMAND_QUEUE_DESC host{desc[0] == 16
                                            ? D3D12_COMMAND_LIST_TYPE_DIRECT
                                            : static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]),
                                        0, static_cast<D3D12_COMMAND_QUEUE_FLAGS>(desc[1] & 1), 0};
    return create_command_queue(self, &host, iid, queue);
}
// What the host insists on where the console takes anything: a resource of an upload heap
// starts as one to read from and one of a readback heap as one to copy to, and only a depth
// buffer is kept from shaders.
void host_rules(const D3D12_HEAP_PROPERTIES *heap, D3D12_RESOURCE_FLAGS &flags,
                D3D12_RESOURCE_STATES &state) {
    // 0x100000, which later kits give a display target and the host has no state for
    state &= ~static_cast<D3D12_RESOURCE_STATES>(0x100000);
    if (heap && heap->Type == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap && heap->Type == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    // A depth buffer of the console's may be written by compute shaders and drawn to as a
    // colour as well; the host makes none that is (E_INVALIDARG). It stays a depth buffer:
    // what the title writes to it any other way is lost.
    constexpr auto others = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL && flags & others) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
            std::fprintf(stderr,
                         "[Runtime] a depth buffer that is also written without being drawn to "
                         "(flags 0x%x) is only a depth buffer on the host\n",
                         static_cast<unsigned>(flags));
            std::fflush(stderr);
        }
        flags &= ~others;
        if (state & (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET))
            state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
}
// CreateCommittedResource, reported when the host turns it down
HRESULT STDMETHODCALLTYPE create_committed_resource(
    ID3D12Device *self, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
    REFIID iid, void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(
        void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
    // the console's D3D12_RESOURCE_STATE_GENERIC_READ (0x8c3) has no indirect argument bit, the
    // desktop's (0xac3), which upload heaps require, does
    constexpr auto console_generic_read = static_cast<D3D12_RESOURCE_STATES>(0x8c3);
    if (state == console_generic_read)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    // resource flags above the desktop's (0x100000, 0x200000 on Unreal's render targets) are
    // the console's own
    D3D12_RESOURCE_DESC host_desc{};
    if (desc) {
        host_desc = *desc;
        host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host_desc.Flags & 0xff);
        host_desc.Format = host_format(host_desc.Format, true);
        if (host_desc.Flags &
            (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            host_desc.Format = target_format(host_desc.Format);
        // a console layout names a tile mode (0x100 | mode): the host picks its own
        if (host_desc.Layout >= 0x100)
            host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // the console takes any alignment for a buffer (a title asks for 64 bytes); the
        // host only its own
        // and a constant buffer view of 256 bytes on a smaller buffer, which the host drops
        // the device for: the buffer is made to hold it
        if (host_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            host_desc.Alignment = 0;
            host_desc.Width = (host_desc.Width + 255) & ~std::uint64_t{255};
        }
        host_rules(heap, host_desc.Flags, state);
        host_resource_format(self, host_desc);
    }
    D3D12_CLEAR_VALUE cleared{}, cleared_host{};
    clear = host_clear_value(self, host_clear(clear, cleared), host_desc.Flags, cleared_host);
    // All of the console's memory is the CPU's as well: a title maps a buffer of the default
    // heap and writes to it. The host only maps what is in a heap the CPU can reach, so such
    // a buffer goes in one (unless it is written by the GPU, which those heaps do not take).
    D3D12_HEAP_PROPERTIES mappable{};
    const auto *const asked = heap;
    if (console == Target::scarlett && heap && desc && heap->Type == D3D12_HEAP_TYPE_DEFAULT &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        mappable = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
                    D3D12_MEMORY_POOL_L0, heap->CreationNodeMask, heap->VisibleNodeMask};
        heap = &mappable;
    }
    if (static std::atomic<int> shown{0};
        std::getenv("XV_D3D_TRACE") && heap && desc && shown++ < 40)
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource heap %d flags 0x%x dimension %d %llux%u "
                     "format %d flags 0x%x state 0x%x\n",
                     heap->Type, flags, desc->Dimension, desc->Width, desc->Height, desc->Format,
                     desc->Flags, state);
    auto result = original<Create>(self, 27)(self, heap, flags, desc ? &host_desc : nullptr, state,
                                             clear, iid, resource);
    // a buffer the host does not make there is made where the title asked
    if (FAILED(result) && heap != asked) {
        heap = asked;
        result =
            original<Create>(self, 27)(self, heap, flags, &host_desc, state, clear, iid, resource);
    }
    if (SUCCEEDED(result) && resource && *resource) {
        console_resource(*resource);
        track_buffer(static_cast<ID3D12Resource *>(*resource), console == Target::scarlett);
        if (console == Target::scarlett)
            mappable_resource(*resource);
    }
    if (FAILED(result) && heap && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource heap %d/%d/%d flags 0x%x dimension %d "
                     "alignment %llu %llux%ux%u mips %u format %d samples %u layout %d flags "
                     "0x%x state 0x%x: 0x%08lx\n",
                     heap->Type, heap->CPUPageProperty, heap->MemoryPoolPreference, flags,
                     desc->Dimension, desc->Alignment, desc->Width, desc->Height,
                     desc->DepthOrArraySize, desc->MipLevels, desc->Format, desc->SampleDesc.Count,
                     desc->Layout, desc->Flags, state, static_cast<unsigned long>(result));
        std::fflush(stderr);
        // the first thing to be turned down for a device the host has dropped says why it did
        if (static std::atomic<bool> told{false};
            result == DXGI_ERROR_DEVICE_REMOVED && !told.exchange(true))
            report_failure();
    }
    return result;
}
// XV_D3D_DEBUG: the host's debug layer, whose messages go to the log when something the title
// asked for is turned down
std::atomic<ID3D12InfoQueue *> debug_messages{nullptr};
void report_messages() {
    auto *const queue = debug_messages.load();
    if (!queue)
        return;
    // one thread at a time: another one's clearing would take away the messages this one counted
    static std::mutex reporting;
    const std::lock_guard lock(reporting);
    for (UINT64 i = 0, count = queue->GetNumStoredMessages(); i < count; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        // nothing there to read: the size is left at 0 when the call fails
        if (size < sizeof(D3D12_MESSAGE))
            continue;
        std::vector<std::uint8_t> buffer(size);
        auto *const message = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            std::fprintf(stderr, "[Runtime] Direct3D 12 debug layer: %.*s\n",
                         static_cast<int>(message->DescriptionByteLength), message->pDescription);
    }
    queue->ClearStoredMessages();
    std::fflush(stderr);
}

// ID3D12GraphicsCommandList as the console has it. The title reads the object itself: a
// write pointer (at +0x38 on Xbox One, +0x18 on Xbox Series) and its limits after it, between
// which it records commands inline when there is room. The title gets a proxy of the
// console's size whose inline space is always full, so that every command comes through a
// method, and the proxy hands those to the host's list. On Xbox Series the inline
// SetPipelineState also copies 32 bytes of the pipeline state into the list at +0x540, before
// it looks at the write pointer: the proxy has the room, 0x560 bytes being the list's size
// there.
struct ListProxy {
    Slot *vtable;
    std::uint8_t fields[0x600 - sizeof(Slot *)];
    ID3D12GraphicsCommandList *host;
    std::atomic<ULONG> references;
    ID3D12Resource *target = nullptr; // bound last, for XV_D3D_PROBE's draw counts
    const void *graphics_root = nullptr, *compute_root = nullptr;
    ID3D12DescriptorHeap *heaps[2]{};        // the host's, as bound: views, then samplers
    UINT topology = 0;                       // as the title set it, the console's own included
    D3D12_INDEX_BUFFER_VIEW indices{};       // the title's, to put back after a quad draw
    ID3D12PipelineState *pipeline = nullptr; // set last, the host's
    // the title's pipeline set last has none of the host's: what it draws or computes is left out
    bool no_pipeline = false;
    // The title's root arguments for drawing, by parameter, kept so that they can be set
    // again: a draw with a pipeline that reads the heap itself (HeapUses) puts a root
    // signature of ours on the host's list, which takes none of them.
    struct RootArgument {
        int kind = 0; // 1 table, 2 constant buffer, 3 shader resource, 4 unordered access,
                      // 5 constants
        std::uint64_t value = 0;
        std::vector<std::uint32_t> words;
    };
    std::vector<RootArgument> graphics_arguments, compute_arguments;
    std::shared_ptr<const HeapUses> heap_uses; // of `pipeline`
    bool ours_bound = false;                   // the host's list has heap_signature
    bool compute_ours_bound = false;           // and for computing
    D3D12_VERTEX_BUFFER_VIEW vertices{};       // XV_D3D_LOST: the title's first stream, set last
    UINT vertex_calls = 0;
    bool legacy_barriers = false; // Barrier as on a host without enhanced barriers
    bool waits_memory = false;    // it has a Wait32BitValueX or Wait64BitValueX
    // the host's driver faulted on what it was given: the list is not executed, as the host
    // drops the device for it
    bool broken = false;
    // what it does to memory by address, in order, done when a queue is given it (when_run)
    std::vector<std::function<void()>> on_memory;
    RootArgument &argument(UINT parameter, bool compute = false) {
        auto &arguments = compute ? compute_arguments : graphics_arguments;
        if (arguments.size() <= parameter)
            arguments.resize(std::min<std::size_t>(parameter, 63) + 1);
        return arguments[std::min<std::size_t>(parameter, 63)];
    }
};
std::mutex proxy_mutex;
std::set<const void *> list_proxies;
// the host's list for a title's one, which may be either
ID3D12CommandList *host_list(void *list) {
    const std::lock_guard lock(proxy_mutex);
    return list_proxies.contains(list) ? static_cast<ListProxy *>(list)->host
                                       : static_cast<ID3D12CommandList *>(list);
}
using List = ID3D12GraphicsCommandList;
List *host_of(void *self) { return static_cast<ListProxy *>(self)->host; }

// Each console slot calls this host slot, the arguments going on as they came (integer
// registers and stack). The console has the desktop's methods in another order: the queries
// (BeginQuery, EndQuery, ResolveQueryData, SetPredication; the desktop's 52 to 55) come right
// after ExecuteBundle, at 28 to 31, and push SetDescriptorHeaps to ClearUnorderedAccessView-
// Float 4 slots later; from SetMarker (56) on they agree again. Unreal's call sites show it:
// Close, Reset, CopyBufferRegion and ResourceBarrier in place, SetDescriptorHeaps at 32, its
// timestamps' EndQuery and ResolveQueryData at 29 and 30.
//
// Past the first interface the two consoles differ. Xbox One has ID3D12GraphicsCommandList1
// and 2 at 104 to 110. Xbox Series has two more methods of its own first (to 104), then
// those at 105 to 111 and ID3D12GraphicsCommandList3 to 6 at 112 to 128, with four console
// methods among them (122, 123, 126, 128). The host has all of them from 60 on, in the
// desktop's order. A title built with a later kit has console methods past 128, and
// ID3D12GraphicsCommandList7's Barrier among them at 144 (see enhanced_barriers).
constexpr std::size_t host_list_slots = 60, list_slots = 160, unmapped = ~std::size_t{0};
std::array<std::size_t, list_slots> list_map{};
void map_list_slots() {
    for (std::size_t slot = 0; slot < list_slots; ++slot)
        list_map[slot] = slot <= 27               ? slot
                         : slot < 32              ? slot + 24
                         : slot < 56              ? slot - 4
                         : slot < host_list_slots ? slot
                                                  : unmapped;
    // CopyTextureRegion with one more argument, which the host's has not
    list_map[83] = 16;
    // DispatchX and CopyResourceX likewise: Dispatch and CopyResource, and flags
    list_map[78] = 14;
    list_map[84] = 17;
    if (console == Target::durango) {
        for (std::size_t slot = 104; slot <= 110; ++slot)
            list_map[slot] = slot - 44;
        return;
    }
    for (std::size_t slot = 105; slot <= 121; ++slot)
        list_map[slot] = slot - 45;
    list_map[124] = 77; // RSSetShadingRate
    list_map[125] = 78; // RSSetShadingRateImage
    list_map[127] = 79; // DispatchMesh
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE list_method(void *self, std::uint64_t a, std::uint64_t b,
                                            std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                            std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                            std::uint64_t i, std::uint64_t j) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && calls++ < 2) {
        std::fprintf(
            stderr,
            "[Runtime] Direct3D 12 command list slot %zu (%llx %llx %llx %llx %llx) from %s\n",
            Index, a, b, c, d, e, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    const auto target = list_map[Index];
    if (target == unmapped) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
            std::fprintf(stderr,
                         "[Runtime] unimplemented Direct3D 12 call: command list slot %zu (%llx, "
                         "%llx) from %s\n",
                         Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
            std::fflush(stderr);
        }
        return E_NOTIMPL;
    }
    auto *const host = host_of(self);
    const auto result = reinterpret_cast<Raw>((*reinterpret_cast<Slot **>(host))[target])(
        host, a, b, c, d, e, f, g, h, i, j);
    lost_check("list", Index, _ReturnAddress(), self);
    return result;
}
HRESULT STDMETHODCALLTYPE list_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    // The later interfaces are the same object on the console, and on the host: the proxy
    // answers for those the host's list has. ID3D12GraphicsCommandList5 and 6 had other
    // interface IDs in the kits than the ones the desktop shipped with.
    static constexpr GUID kit_list5{
        0x55050859, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    static constexpr GUID kit_list6{
        0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    const auto &host_iid = iid == kit_list5   ? __uuidof(ID3D12GraphicsCommandList5)
                           : iid == kit_list6 ? __uuidof(ID3D12GraphicsCommandList6)
                                              : iid;
    bool ours = is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList,
                       ID3D12GraphicsCommandList>(host_iid);
    if (!ours &&
        is_any<ID3D12GraphicsCommandList1, ID3D12GraphicsCommandList2, ID3D12GraphicsCommandList3,
               ID3D12GraphicsCommandList4, ID3D12GraphicsCommandList5, ID3D12GraphicsCommandList6,
               ID3D12GraphicsCommandList7>(host_iid)) {
        IUnknown *later = nullptr;
        if (SUCCEEDED(host_of(self)->QueryInterface(host_iid, reinterpret_cast<void **>(&later)))) {
            later->Release();
            ours = true;
        }
    }
    if (ours) {
        ++static_cast<ListProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return host_of(self)->QueryInterface(host_iid, object);
}
ULONG STDMETHODCALLTYPE list_add_ref(void *self) {
    return ++static_cast<ListProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE list_release(void *self) {
    auto *const proxy = static_cast<ListProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        {
            const std::lock_guard lock(proxy_mutex);
            list_proxies.erase(proxy);
        }
        proxy->host->Release();
        delete proxy;
    }
    return left;
}

// The methods that take CPU handles, at their console slots (the desktop's + 4): the host's
// handles. The depth clear has a float in a register the generic path does not carry.
using Handle = D3D12_CPU_DESCRIPTOR_HANDLE;
std::atomic<ID3D12Device *> d3d12_device{nullptr};
void lost_check(const char *kind, std::size_t index, void *from, void *object) {
    static const bool checking = std::getenv("XV_D3D_LOST") != nullptr;
    static std::atomic<bool> told{false};
    if (!checking || told.load(std::memory_order_relaxed))
        return;
    struct Call {
        const char *kind;
        std::size_t index;
        void *from, *object;
        DWORD thread;
    };
    // a thread of its own that does nothing but look, and says where every thread is when
    // the device goes
    static const bool watching = [] {
        // what the host's runtime and driver say for a debugger (OutputDebugString raises
        // an exception to hand its text over), and from where
        AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS *exception) -> LONG {
            const auto *const record = exception->ExceptionRecord;
            if ((record->ExceptionCode != 0x40010006 && record->ExceptionCode != 0x4001000a) ||
                record->NumberParameters < 2)
                return EXCEPTION_CONTINUE_SEARCH;
            const bool wide = record->ExceptionCode == 0x4001000a;
            const auto text = record->ExceptionInformation[1];
            const bool removing =
                wide ? std::wcsstr(reinterpret_cast<const wchar_t *>(text), L"Removing") != nullptr
                     : std::strstr(reinterpret_cast<const char *>(text), "Removing") != nullptr;
            if (wide)
                std::fprintf(stderr, "[Said] thread %lu: %ls", GetCurrentThreadId(),
                             reinterpret_cast<const wchar_t *>(text));
            else
                std::fprintf(stderr, "[Said] thread %lu: %s", GetCurrentThreadId(),
                             reinterpret_cast<const char *>(text));
            std::fflush(stderr);
            // the thread that says so is the one whose call the host drops the device for
            if (removing) {
                std::fputs(lost_last, stderr);
                std::fflush(stderr);
            }
            if (removing)
                detail::report_thread_stacks(exception->ContextRecord->Rsp);
            return EXCEPTION_CONTINUE_SEARCH;
        });
        std::thread([] {
            for (const auto *name : {L"D3D12Core.dll", L"d3d12.dll", L"dxgi.dll", L"nvwgf2umx.dll",
                                     L"xvboot.exe", L"d3d12SDKLayers.dll"})
                std::fprintf(stderr, "[Stacks] %ls is at %llx%c", name,
                             static_cast<unsigned long long>(
                                 reinterpret_cast<std::uintptr_t>(GetModuleHandleW(name))),
                             10);
            std::fflush(stderr);
            for (;;) {
                auto *const watched = d3d12_device.load();
                if (watched && FAILED(watched->GetDeviceRemovedReason()))
                    break;
                YieldProcessor();
            }
            std::fprintf(stderr, "[Runtime] the host has dropped the device; the threads:%c", 10);
            detail::report_thread_stacks();
        }).detach();
        return true;
    }();
    static_cast<void>(watching);
    static Call ring[64];
    static std::atomic<std::uint64_t> next{0};
    const auto at = next++;
    ring[at % 64] = {kind, index, from, object, GetCurrentThreadId()};
    auto *const device = d3d12_device.load();
    if (!device || SUCCEEDED(device->GetDeviceRemovedReason()) || told.exchange(true))
        return;
    std::fprintf(stderr, "[Runtime] the host has dropped the device (0x%08lx) by call %llu:%c",
                 static_cast<unsigned long>(device->GetDeviceRemovedReason()),
                 static_cast<unsigned long long>(at), 10);
    for (std::uint64_t i = at >= 63 ? at - 63 : 0; i <= at; ++i) {
        const auto &call = ring[i % 64];
        std::fprintf(stderr, "[Runtime]   thread %lu %s %p slot %zu from %s%c", call.thread,
                     call.kind, call.object, call.index, detail::guest_location(call.from).c_str(),
                     10);
    }
    std::fflush(stderr);
}
HRESULT STDMETHODCALLTYPE close_list(void *self) {
    const auto result = host_of(self)->Close();
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 4000) {
        std::fprintf(stderr, "[Lost] thread %lu list %p (type %d) closed: 0x%08lx%c",
                     GetCurrentThreadId(), self, static_cast<int>(host_of(self)->GetType()),
                     static_cast<unsigned long>(result), 10);
        std::fflush(stderr);
    }
    if (FAILED(result)) {
        // the host drops the device when such a list is executed: what was wrong in it is
        // the debug layer's to say
        if (static std::atomic<int> shown{0}; shown++ < 8) {
            std::fprintf(
                stderr, "[Runtime] a command list (type %d) closes with 0x%08lx, from %s\n",
                static_cast<int>(host_of(self)->GetType()), static_cast<unsigned long>(result),
                detail::guest_location(_ReturnAddress()).c_str());
            std::fflush(stderr);
        }
        report_messages();
    }
    return result;
}
// ResourceBarrier: transitions name states the console's way. GENERIC_READ is 0x8c3 there,
// and bit 0x400000 goes with the states shaders read in; on the host that bit is a ray
// tracing acceleration structure, which is no state to be combined with another. Xbox One
// has more of its own from there up (what stays compressed or expanded across the
// transition), where the host has video and shading rate states.
D3D12_RESOURCE_STATES host_state(D3D12_RESOURCE_STATES state) {
    if (state == static_cast<D3D12_RESOURCE_STATES>(0x8c3))
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    if (console == Target::durango)
        return state & ~static_cast<D3D12_RESOURCE_STATES>(0x0fd00000);
    constexpr auto structure = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    state &= ~static_cast<D3D12_RESOURCE_STATES>(0x100000);
    return state & structure && state != structure ? state & ~structure : state;
}
// The state the title left each render target in, by its last transition (and each texture
// its enhanced barriers moved): the console shows a resource whatever its state, the host
// copies it out of one it has to be told
std::mutex state_mutex;
std::map<ID3D12Resource *, D3D12_RESOURCE_STATES> target_states;
void STDMETHODCALLTYPE resource_barrier(void *self, UINT count,
                                        const D3D12_RESOURCE_BARRIER *barriers) {
    std::vector<D3D12_RESOURCE_BARRIER> host(barriers, barriers + (barriers ? count : 0));
    for (auto &barrier : host)
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            barrier.Transition.StateBefore = host_state(barrier.Transition.StateBefore);
            barrier.Transition.StateAfter = host_state(barrier.Transition.StateAfter);
            // a depth buffer the console also writes otherwise is only one here (host_rules):
            // those states are its writing one
            if (barrier.Transition.pResource && barrier.Transition.pResource->GetDesc().Flags &
                                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
                for (auto *const state :
                     {&barrier.Transition.StateBefore, &barrier.Transition.StateAfter}) {
                    constexpr auto others =
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET;
                    if (*state & others)
                        *state = (*state & ~others) | D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    if (*state & D3D12_RESOURCE_STATE_DEPTH_WRITE)
                        *state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                }
            if (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET ||
                barrier.Transition.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) {
                const std::lock_guard lock(state_mutex);
                target_states[barrier.Transition.pResource] = barrier.Transition.StateAfter;
            }
        }
    // The title's placed resources are committed ones here, each with memory of its own:
    // none aliases another, and the host turns a list down for saying so.
    std::erase_if(host, [](const D3D12_RESOURCE_BARRIER &barrier) {
        return barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING ||
               (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                barrier.Transition.StateBefore == barrier.Transition.StateAfter);
    });
    // The console takes two transitions of one subresource in a call, one after the other;
    // the host takes a subresource once per call, so the call is split where one comes again.
    const auto again = [&](std::size_t first, std::size_t at) {
        if (host[at].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            return false;
        const auto &now = host[at].Transition;
        for (auto i = first; i < at; ++i)
            if (host[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                host[i].Transition.pResource == now.pResource &&
                (host[i].Transition.Subresource == now.Subresource ||
                 host[i].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
                 now.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
                return true;
        return false;
    };
    for (std::size_t first = 0, at = 0; at <= host.size(); ++at)
        if (at == host.size() || again(first, at)) {
            if (at > first)
                host_of(self)->ResourceBarrier(static_cast<UINT>(at - first), host.data() + first);
            first = at;
        }
}
void remember_state(ID3D12Resource *resource, D3D12_RESOURCE_STATES state) {
    const std::lock_guard lock(state_mutex);
    target_states[resource] = state;
}
// ID3D12GraphicsCommandList7::Barrier(group count, groups), the enhanced barriers of
// Microsoft's d3d12.h and of the Enhanced Barriers spec (DirectX-Specs), which a title built
// with a later kit makes its transitions with. A group is global, texture or buffer barriers;
// each says what work waits for what (sync), how memory was used and will be (access), and
// for a texture its layout before and after, of which subresources.
//
// The title's textures are the host's in the states their layouts stand for (layout_state),
// and the host takes no enhanced barrier on a texture in a state other than COMMON (the
// spec's rule for the two kinds together). So a texture's change of layout is a transition
// as ResourceBarrier makes them, between the states the two layouts stand for, with no pixel
// shader state on a list of another queue than the direct one. Where the layout before is
// UNDEFINED, the state is the one the texture's last transition left it in. The halves of a
// split barrier are its begin and its end, and DISCARD is the texture's discard once there.
//
// What only orders work (a global or a buffer barrier, a texture's with no change of layout)
// is the host's enhanced barrier where it has them (D3D12_FEATURE_D3D12_OPTIONS12), global
// for a texture, with only the bits the desktop's headers name, for the whole of a buffer
// (the only range the host takes). Elsewhere it is an unordered access barrier, on the
// resource or on all, which the spec gives as the legacy meaning of a global barrier.
bool host_has_enhanced_barriers(List *host) {
    static const bool has = [host] {
        ID3D12Device *device = nullptr;
        if (FAILED(host->GetDevice(IID_PPV_ARGS(&device))))
            return false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
        const bool found = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,
                                                                 &options, sizeof options)) &&
                           options.EnhancedBarriersSupported;
        device->Release();
        return found;
    }();
    return has;
}
constexpr auto known_sync = static_cast<D3D12_BARRIER_SYNC>(0x81f0ffff);
constexpr auto known_access = static_cast<D3D12_BARRIER_ACCESS>(0x807fffff);
// The subresources of a range as legacy barriers name them: each index, or
// D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES alone for all. A range of no mip levels is one
// index already (all of them at 0xffffffff, as in a legacy barrier).
std::vector<UINT> range_subresources(ID3D12Resource *resource,
                                     const D3D12_BARRIER_SUBRESOURCE_RANGE &range) {
    if (!range.NumMipLevels)
        return {range.IndexOrFirstMipLevel};
    const auto desc = resource->GetDesc();
    const UINT mips = std::max<UINT>(desc.MipLevels, 1);
    const UINT slices = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                            ? 1
                            : std::max<UINT>(desc.DepthOrArraySize, 1);
    UINT planes = 1;
    if (ID3D12Device *device = nullptr; SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device)))) {
        D3D12_FEATURE_DATA_FORMAT_INFO info{desc.Format};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof info)))
            planes = std::max<UINT>(info.PlaneCount, 1);
        device->Release();
    }
    const auto end = [](UINT first, UINT count, UINT limit) {
        return static_cast<UINT>(std::min<std::uint64_t>(std::uint64_t{first} + count, limit));
    };
    const UINT mip_end = end(range.IndexOrFirstMipLevel, range.NumMipLevels, mips);
    const UINT slice_end = end(range.FirstArraySlice, range.NumArraySlices, slices);
    const UINT plane_end = end(range.FirstPlane, range.NumPlanes, planes);
    if (!range.IndexOrFirstMipLevel && mip_end == mips && !range.FirstArraySlice &&
        slice_end == slices && !range.FirstPlane && plane_end == planes)
        return {D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES};
    std::vector<UINT> indices;
    for (UINT plane = range.FirstPlane; plane < plane_end; ++plane)
        for (UINT slice = range.FirstArraySlice; slice < slice_end; ++slice)
            for (UINT mip = range.IndexOrFirstMipLevel; mip < mip_end; ++mip)
                indices.push_back(mip + (slice + plane * slices) * mips);
    return indices;
}
void STDMETHODCALLTYPE enhanced_barriers(void *self, UINT32 count,
                                         const D3D12_BARRIER_GROUP *groups) {
    auto *const host = host_of(self);
    ID3D12GraphicsCommandList7 *enhanced = nullptr;
    if (!static_cast<ListProxy *>(self)->legacy_barriers && host_has_enhanced_barriers(host))
        host->QueryInterface(IID_PPV_ARGS(&enhanced));
    std::vector<D3D12_RESOURCE_BARRIER> legacy;
    std::vector<std::pair<ID3D12Resource *, UINT>> discards;
    std::vector<D3D12_GLOBAL_BARRIER> globals;
    std::vector<D3D12_BUFFER_BARRIER> buffers;
    // work before and work after in order, for one resource or for all
    const auto order = [&](ID3D12Resource *resource, D3D12_BARRIER_SYNC sync_before,
                           D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS access_before,
                           D3D12_BARRIER_ACCESS access_after) {
        sync_before &= known_sync;
        sync_after &= known_sync;
        if (sync_before == D3D12_BARRIER_SYNC_NONE || sync_after == D3D12_BARRIER_SYNC_NONE)
            return;
        if (enhanced) {
            const D3D12_GLOBAL_BARRIER known{sync_before, sync_after, access_before & known_access,
                                             access_after & known_access};
            if (resource && resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                buffers.push_back({known.SyncBefore, known.SyncAfter, known.AccessBefore,
                                   known.AccessAfter, resource, 0, UINT64_MAX});
            else
                globals.push_back(known);
            return;
        }
        if (sync_after == D3D12_BARRIER_SYNC_SPLIT)
            return;
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        if (resource && resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
            barrier.UAV.pResource = resource;
        legacy.push_back(barrier);
    };
    const bool direct = host->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
    const auto state = [direct](D3D12_RESOURCE_STATES of) {
        return direct ? of : of & ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    };
    const auto texture = [&](const D3D12_TEXTURE_BARRIER &of) {
        constexpr auto undefined = D3D12_BARRIER_LAYOUT_UNDEFINED;
        // with no layout after, what was there is not wanted: the texture stays as it is
        if (of.LayoutAfter == undefined) {
            if (of.LayoutBefore == undefined)
                order(of.pResource, of.SyncBefore, of.SyncAfter, of.AccessBefore, of.AccessAfter);
            return;
        }
        const auto before = state(of.LayoutBefore == undefined
                                      ? tracked_state(of.pResource, D3D12_RESOURCE_STATE_COMMON)
                                      : legacy_state(of.LayoutBefore));
        const auto after = state(legacy_state(of.LayoutAfter));
        const bool begins = of.SyncAfter == D3D12_BARRIER_SYNC_SPLIT;
        constexpr auto discardable = D3D12_RESOURCE_STATE_RENDER_TARGET |
                                     D3D12_RESOURCE_STATE_DEPTH_WRITE |
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const bool discard = !begins && of.Flags & D3D12_TEXTURE_BARRIER_FLAG_DISCARD &&
                             of.LayoutBefore == undefined &&
                             after & (direct ? discardable : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // the same state before and after: only work to order, unordered access with no
        // enhanced barriers
        if (before == after && (enhanced || after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            order(of.pResource, of.SyncBefore, of.SyncAfter, of.AccessBefore, of.AccessAfter);
        if (before == after && !discard)
            return;
        const auto subresources = range_subresources(of.pResource, of.Subresources);
        for (const auto subresource : subresources) {
            if (before == after)
                break;
            D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
            barrier.Flags = begins ? D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY
                            : of.SyncBefore == D3D12_BARRIER_SYNC_SPLIT
                                ? D3D12_RESOURCE_BARRIER_FLAG_END_ONLY
                                : D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition = {of.pResource, subresource, before, after};
            legacy.push_back(barrier);
        }
        if (!begins)
            remember_state(of.pResource, after);
        if (discard)
            for (const auto subresource : subresources)
                discards.emplace_back(of.pResource, subresource);
    };
    for (UINT32 group = 0; groups && group < count; ++group) {
        const auto &of = groups[group];
        for (UINT32 i = 0; of.pGlobalBarriers && i < of.NumBarriers; ++i)
            if (of.Type == D3D12_BARRIER_TYPE_GLOBAL) {
                const auto &barrier = of.pGlobalBarriers[i];
                order(nullptr, barrier.SyncBefore, barrier.SyncAfter, barrier.AccessBefore,
                      barrier.AccessAfter);
            } else if (of.Type == D3D12_BARRIER_TYPE_TEXTURE && of.pTextureBarriers[i].pResource) {
                texture(of.pTextureBarriers[i]);
            } else if (of.Type == D3D12_BARRIER_TYPE_BUFFER && of.pBufferBarriers[i].pResource) {
                const auto &barrier = of.pBufferBarriers[i];
                order(barrier.pResource, barrier.SyncBefore, barrier.SyncAfter,
                      barrier.AccessBefore, barrier.AccessAfter);
            }
    }
    resource_barrier(self, static_cast<UINT>(legacy.size()), legacy.data());
    for (const auto &[resource, subresource] : discards) {
        D3D12_DISCARD_REGION region{0, nullptr, subresource, 1};
        host->DiscardResource(
            resource, subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? nullptr : &region);
    }
    if (!enhanced)
        return;
    D3D12_BARRIER_GROUP made[2]{{D3D12_BARRIER_TYPE_GLOBAL}, {D3D12_BARRIER_TYPE_BUFFER}};
    made[0].NumBarriers = static_cast<UINT32>(globals.size());
    made[0].pGlobalBarriers = globals.data();
    made[1].NumBarriers = static_cast<UINT32>(buffers.size());
    made[1].pBufferBarriers = buffers.data();
    const auto *const first = globals.empty() ? made + 1 : made;
    if (const auto groups_made = static_cast<UINT32>(!globals.empty() + !buffers.empty()))
        enhanced->Barrier(groups_made, first);
    enhanced->Release();
}
// CopyTextureRegion from a buffer at an offset the host does not take: the console reads a
// texture's data from anywhere in a buffer, the host from multiples of 512 bytes. The data
// goes through a buffer of ours first, where it starts at 0. Those buffers are let go a
// while after they were used, when the GPU is long past them.
struct Staging {
    ID3D12Resource *buffer;
    ULONGLONG made;
};
std::mutex staging_mutex;
std::deque<Staging> staging;
// CopyTextureRegion from a sampler feedback map to a buffer: the console decodes in the copy,
// the host needs ResolveSubresourceRegion(DECODE) into an R8_UINT texture first. Bytes are
// whole mip levels (255: none); the console's 5.3 fixed point for R8_TYPELESS is not done.
struct DecodedFeedback {
    ID3D12Resource *texture;
    UINT64 width;
    UINT height;
};
std::mutex decoded_feedback_mutex;
std::map<ID3D12Resource *, DecodedFeedback> decoded_feedback; // by map
bool copy_feedback_map(List *host, const D3D12_TEXTURE_COPY_LOCATION *target,
                       const D3D12_TEXTURE_COPY_LOCATION *source) {
    if (!target || !source || !target->pResource || !source->pResource ||
        target->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
        source->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        target->PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)
        return false;
    ID3D12Resource2 *map = nullptr;
    if (FAILED(source->pResource->QueryInterface(IID_PPV_ARGS(&map))))
        return false;
    const auto desc = map->GetDesc1();
    map->Release();
    if (desc.Format != DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE)
        return false;
    const auto &region = desc.SamplerFeedbackMipRegion;
    const auto width = std::max<UINT64>(desc.Width / std::max<UINT>(region.Width, 1), 1);
    const auto height = std::max<UINT>(desc.Height / std::max<UINT>(region.Height, 1), 1);
    ID3D12GraphicsCommandList1 *list = nullptr;
    if (FAILED(host->QueryInterface(IID_PPV_ARGS(&list))))
        return false;
    ID3D12Resource *texture = nullptr;
    {
        const std::lock_guard lock(decoded_feedback_mutex);
        auto &decoded = decoded_feedback[source->pResource];
        if (decoded.texture && (decoded.width != width || decoded.height != height)) {
            decoded.texture->Release();
            decoded.texture = nullptr;
        }
        ID3D12Device *device = nullptr;
        if (!decoded.texture && SUCCEEDED(host->GetDevice(IID_PPV_ARGS(&device)))) {
            const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC bytes{};
            bytes.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            bytes.Width = width;
            bytes.Height = height;
            bytes.DepthOrArraySize = desc.DepthOrArraySize;
            bytes.MipLevels = 1;
            bytes.Format = DXGI_FORMAT_R8_UINT;
            bytes.SampleDesc.Count = 1;
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes,
                                            D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                            IID_PPV_ARGS(&decoded.texture));
            device->Release();
            decoded.width = width;
            decoded.height = height;
        }
        texture = decoded.texture;
    }
    if (!texture) {
        list->Release();
        return false;
    }
    const auto transition = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES from,
                                D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        host->ResourceBarrier(1, &barrier);
    };
    transition(source->pResource, D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list->ResolveSubresourceRegion(texture, ~0u, 0, 0, source->pResource, ~0u, nullptr,
                                   DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
    list->Release();
    transition(source->pResource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(texture, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto placed = *target;
    auto &footprint = placed.PlacedFootprint.Footprint;
    footprint.Format = DXGI_FORMAT_R8_UINT;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex =
        source->SubresourceIndex < desc.DepthOrArraySize ? source->SubresourceIndex : 0;
    const D3D12_BOX part{0,
                         0,
                         0,
                         static_cast<UINT>(std::min<UINT64>(width, footprint.Width)),
                         std::min(height, footprint.Height),
                         1};
    host->CopyTextureRegion(&placed, 0, 0, 0, &from, &part);
    transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    if (static std::atomic<bool> shown{false};
        target->PlacedFootprint.Footprint.Format == DXGI_FORMAT_R8_TYPELESS &&
        !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a sampler feedback map is read back as whole mip levels, "
                             "where the title asks for the console's 5.3 fixed point\n");
        std::fflush(stderr);
    }
    return true;
}
// A copy between a buffer and a texture whose format's stand-in on the host lays its texels
// out otherwise (see host_format): the host has no such footprint, and the bytes would not be
// its texels. The copy is not made.
bool foreign_footprint(const D3D12_TEXTURE_COPY_LOCATION *footprint,
                       const D3D12_TEXTURE_COPY_LOCATION *texture) {
    if (!footprint || footprint->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        return false;
    const auto format = footprint->PlacedFootprint.Footprint.Format;
    return host_format(nullptr, format, FormatUse::shader_view) != format ||
           (small_colour(format) && texture && texture->pResource &&
            texture->pResource->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM);
}
void STDMETHODCALLTYPE copy_texture_region(void *self, const D3D12_TEXTURE_COPY_LOCATION *target,
                                           UINT x, UINT y, UINT z,
                                           const D3D12_TEXTURE_COPY_LOCATION *source,
                                           const D3D12_BOX *box) {
    if (foreign_footprint(target, source) || foreign_footprint(source, target))
        return;
    auto *const host = host_of(self);
    if (!x && !y && !z && copy_feedback_map(host, target, source))
        return;
    ID3D12Device *device = nullptr;
    // the other way, a texture read into a buffer at such an offset: into a buffer of ours
    // at 0, and from there to where the title wants it
    if (target && source && target->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
        target->PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT &&
        target->pResource && !x && !y && !z && SUCCEEDED(host->GetDevice(IID_PPV_ARGS(&device)))) {
        const auto &footprint = target->PlacedFootprint.Footprint;
        const auto offset = target->PlacedFootprint.Offset;
        const auto room = target->pResource->GetDesc().Width;
        const auto rows = footprint.Height; // block formats have fewer; the room caps it
        const auto bytes = std::min<UINT64>(UINT64{footprint.RowPitch} * rows * footprint.Depth,
                                            room > offset ? room - offset : 0);
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc{
            D3D12_RESOURCE_DIMENSION_BUFFER,
            0,
            std::max<UINT64>(UINT64{footprint.RowPitch} * rows * footprint.Depth, 1),
            1,
            1,
            1,
            DXGI_FORMAT_UNKNOWN,
            {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource *buffer = nullptr;
        const auto made = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&buffer));
        device->Release();
        if (SUCCEEDED(made) && bytes) {
            auto staged = *target;
            staged.pResource = buffer;
            staged.PlacedFootprint.Offset = 0;
            host->CopyTextureRegion(&staged, 0, 0, 0, source, box);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Transition = {buffer, 0, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE};
            host->ResourceBarrier(1, &barrier);
            host->CopyBufferRegion(target->pResource, offset, buffer, 0, bytes);
            const std::lock_guard lock(staging_mutex);
            staging.push_back({buffer, GetTickCount64()});
            return;
        }
        if (buffer)
            buffer->Release();
        device = nullptr;
    }
    if (!source || source->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
        source->PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0 ||
        !source->pResource || FAILED(host->GetDevice(IID_PPV_ARGS(&device)))) {
        host->CopyTextureRegion(target, x, y, z, source, box);
        return;
    }
    const auto &footprint = source->PlacedFootprint.Footprint;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = footprint.Depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = footprint.Width;
    texture.Height = footprint.Height;
    texture.DepthOrArraySize = static_cast<UINT16>(footprint.Depth);
    texture.MipLevels = 1;
    texture.Format = footprint.Format;
    texture.SampleDesc.Count = 1;
    UINT rows = footprint.Height;
    device->GetCopyableFootprints(&texture, 0, 1, 0, nullptr, &rows, nullptr, nullptr);
    const auto offset = source->PlacedFootprint.Offset;
    const auto available = source->pResource->GetDesc().Width;
    const auto bytes = std::min<UINT64>(UINT64{footprint.RowPitch} * rows * footprint.Depth,
                                        available > offset ? available - offset : 0);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                   0,
                                   std::max<UINT64>(bytes, 1),
                                   1,
                                   1,
                                   1,
                                   DXGI_FORMAT_UNKNOWN,
                                   {1, 0},
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                   D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *buffer = nullptr;
    const auto made = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&buffer));
    device->Release();
    if (FAILED(made) || !bytes) {
        std::fprintf(stderr,
                     "[Runtime] CopyTextureRegion from offset %llu of a buffer of %llu bytes, "
                     "%ux%ux%u format %d pitch %u: no way round the offset (0x%08lx)\n",
                     offset, available, footprint.Width, footprint.Height, footprint.Depth,
                     footprint.Format, footprint.RowPitch, static_cast<unsigned long>(made));
        host->CopyTextureRegion(target, x, y, z, source, box);
        return;
    }
    host->CopyBufferRegion(buffer, 0, source->pResource, offset, bytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {buffer, 0, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    host->ResourceBarrier(1, &barrier);
    auto staged = *source;
    staged.pResource = buffer;
    staged.PlacedFootprint.Offset = 0;
    host->CopyTextureRegion(target, x, y, z, &staged, box);
    const std::lock_guard lock(staging_mutex);
    const auto now = GetTickCount64();
    staging.push_back({buffer, now});
    while (!staging.empty() && now - staging.front().made > 20000) {
        staging.front().buffer->Release();
        staging.pop_front();
    }
}
// CopyTextureRegionX (83): the same copy with flags for the console's compressed surfaces
void STDMETHODCALLTYPE copy_texture_region_x(void *self, const D3D12_TEXTURE_COPY_LOCATION *target,
                                             UINT x, UINT y, UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION *source,
                                             const D3D12_BOX *box, UINT) {
    copy_texture_region(self, target, x, y, z, source, box);
}
void STDMETHODCALLTYPE execute_bundle(void *self, ID3D12GraphicsCommandList *bundle) {
    host_of(self)->ExecuteBundle(static_cast<List *>(host_list(bundle)));
    // the bundle's work on memory, there in the list
    {
        const std::lock_guard lock(proxy_mutex);
        if (!list_proxies.contains(bundle))
            return;
    }
    auto &into = static_cast<ListProxy *>(self)->on_memory;
    const auto &from = static_cast<ListProxy *>(static_cast<void *>(bundle))->on_memory;
    into.insert(into.end(), from.begin(), from.end());
}
// XV_D3D_TRACE: the first calls of a method, with a handle and the host's for it
void trace_handle(const char *method, Handle handle) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    static std::atomic<int> calls{0};
    if (tracing && calls++ < 24) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 %s(%llx -> %llx)\n", method,
                     static_cast<unsigned long long>(handle.ptr),
                     static_cast<unsigned long long>(host_handle(handle).ptr));
        std::fflush(stderr);
    }
}
// SetDescriptorHeaps: the host's heaps for the title's proxies
void STDMETHODCALLTYPE set_descriptor_heaps(void *self, UINT count,
                                            ID3D12DescriptorHeap *const *heaps) {
    auto *const list = static_cast<ListProxy *>(self);
    ID3D12DescriptorHeap *host[2]{};
    list->heaps[0] = list->heaps[1] = nullptr;
    for (UINT i = 0; i < count && i < 2; ++i) {
        host[i] = host_heap(heaps[i]);
        list->heaps[host[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] = host[i];
    }
    host_of(self)->SetDescriptorHeaps(std::min(count, 2u), host);
}
// A console GPU handle is the descriptor's address, good whatever heaps are set, and titles
// use tables of heaps they never set. The host reads a table from the heap that is bound,
// so the heap a handle is in gets bound first. It replaces the one of its kind: tables of
// two view heaps cannot be bound for one draw here.
void bind_heap_of(void *self, std::uint64_t handle) {
    void *owner = nullptr;
    bool samplers = false;
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *heap = shadow_heap(handle);
        if (!heap || !heap->shader_visible)
            return;
        owner = heap->owner;
        samplers = heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    }
    auto *const list = static_cast<ListProxy *>(self);
    auto *const host = host_heap(owner);
    if (list->heaps[samplers] == host)
        return;
    list->heaps[samplers] = host;
    ID3D12DescriptorHeap *bound[2]{};
    UINT count = 0;
    for (auto *heap : list->heaps)
        if (heap)
            bound[count++] = heap;
    host_of(self)->SetDescriptorHeaps(count, bound);
}
// Reset: the host's list starts again with no heaps
HRESULT STDMETHODCALLTYPE reset_list(void *self, ID3D12CommandAllocator *allocator,
                                     ID3D12PipelineState *state) {
    auto *const list = static_cast<ListProxy *>(self);
    list->heaps[0] = list->heaps[1] = nullptr;
    list->topology = 0;
    list->indices = {};
    list->no_pipeline = state && !(state = list_pipeline(state));
    list->pipeline = state;
    list->heap_uses = state ? heap_uses(state) : nullptr;
    list->graphics_arguments.clear();
    list->compute_arguments.clear();
    list->ours_bound = false;
    list->compute_ours_bound = false;
    list->waits_memory = false;
    list->broken = false;
    list->on_memory.clear();
    const auto result = host_of(self)->Reset(allocator, state);
    if (static std::atomic<int> shown{0};
        std::getenv("XV_D3D_LOST") && (FAILED(result) || shown < 80) && shown++ < 200) {
        std::fprintf(stderr, "[Lost] thread %lu list %p reset (allocator %p, state %p): 0x%08lx%c",
                     GetCurrentThreadId(), self, static_cast<void *>(allocator),
                     static_cast<void *>(state), static_cast<unsigned long>(result), 10);
        std::fflush(stderr);
    }
    return result;
}
// The host's slot in a heap shaders see for `span` descriptors from the title's slot `index`.
// Those in the part of the host's heap that is the title's are where the title has them. A
// heap the host makes shorter than the title's keeps a ring at its end instead: what lies
// past the title's part is copied there from the twin, which has all of it, when the list
// runs, after what the title wrote for that submission is in the twin.
UINT visible_slot(ListProxy *list, const ShadowHeap &heap, std::uint64_t index, UINT span) {
    auto *const twin = heap.twin.get();
    span = std::max(span, 1u);
    if (!twin || !twin->ring_count || index + span <= twin->limit || index >= twin->count)
        return static_cast<UINT>(std::min<std::uint64_t>(index, heap.host_count - 1));
    span =
        static_cast<UINT>(std::min<std::uint64_t>({span, twin->ring_count, twin->count - index}));
    UINT start = twin->ring_next.load(std::memory_order_relaxed), next;
    do {
        next = start + span <= twin->ring_count ? start : 0;
    } while (!twin->ring_next.compare_exchange_weak(start, next + span, std::memory_order_relaxed));
    const auto slot = twin->ring_first + next;
    list->on_memory.push_back([shared = heap.twin, slot, index, span] {
        using Copy =
            void(STDMETHODCALLTYPE *)(void *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                      D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
        original<Copy>(shared->device,
                       24)(shared->device, span, {shared->visible.ptr + slot * shared->step},
                           {shared->start.ptr + index * shared->step}, shared->type);
    });
    return slot;
}
// the host's GPU handle for a console one of `span` descriptors, which is the descriptor's
// address in the shadow
D3D12_GPU_DESCRIPTOR_HANDLE host_gpu(ListProxy *list, D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                     UINT span = 1) {
    ShadowHeap heap{};
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *const found = shadow_heap(handle.ptr);
        if (!found || !found->shader_visible)
            return handle;
        heap = *found;
    }
    return {heap.gpu +
            std::uint64_t{visible_slot(list, heap, (handle.ptr - heap.base) / heap.stride, span)} *
                heap.host_stride};
}
// how many descriptors a table of a root signature's parameter takes, by its ranges
UINT table_span(const void *signature, UINT parameter) {
    const std::lock_guard lock(signature_mutex);
    const auto found = tables_of.find(signature);
    if (found == tables_of.end() || parameter >= found->second.size())
        return 1;
    UINT span = 1;
    for (const auto &range : found->second[parameter])
        span = std::max(span, range.offset + range.count);
    return span;
}
// The root signatures and descriptor tables: the title's own descriptors in a table are read
// as the table says, once bound
void STDMETHODCALLTYPE set_compute_root_signature(void *self, ID3D12RootSignature *signature) {
    auto *const list = static_cast<ListProxy *>(self);
    list->compute_root = signature;
    list->compute_arguments.clear();
    list->compute_ours_bound = false;
    host_of(self)->SetComputeRootSignature(signature);
}
void STDMETHODCALLTYPE set_graphics_root_signature(void *self, ID3D12RootSignature *signature) {
    auto *const list = static_cast<ListProxy *>(self);
    list->graphics_root = signature;
    list->graphics_arguments.clear();
    list->ours_bound = false;
    host_of(self)->SetGraphicsRootSignature(signature);
}
void STDMETHODCALLTYPE set_compute_root_table(void *self, UINT parameter,
                                              D3D12_GPU_DESCRIPTOR_HANDLE table) {
    auto *const list = static_cast<ListProxy *>(self);
    list->argument(parameter, true) = {1, table.ptr, {}};
    decode_table(list->compute_root, parameter, table.ptr);
    bind_heap_of(self, table.ptr);
    if (!list->compute_ours_bound)
        host_of(self)->SetComputeRootDescriptorTable(
            parameter, host_gpu(list, table, table_span(list->compute_root, parameter)));
}
void STDMETHODCALLTYPE set_graphics_root_table(void *self, UINT parameter,
                                               D3D12_GPU_DESCRIPTOR_HANDLE table) {
    auto *const list = static_cast<ListProxy *>(self);
    list->argument(parameter) = {1, table.ptr, {}};
    decode_table(list->graphics_root, parameter, table.ptr);
    bind_heap_of(self, table.ptr);
    if (!list->ours_bound)
        host_of(self)->SetGraphicsRootDescriptorTable(
            parameter, host_gpu(list, table, table_span(list->graphics_root, parameter)));
}
// SetGraphicsRoot32BitConstant (38) and SetGraphicsRoot32BitConstants (40), and the compute
// ones before each (37, 39)
void keep_constants(ListProxy *list, bool compute, UINT parameter, UINT count, const void *data,
                    UINT offset) {
    auto &argument = list->argument(parameter, compute);
    argument.kind = 5;
    if (data && offset + count <= 256) {
        if (argument.words.size() < offset + count)
            argument.words.resize(offset + count);
        std::memcpy(argument.words.data() + offset, data, 4 * std::size_t{count});
    }
}
void STDMETHODCALLTYPE set_graphics_root_constant(void *self, UINT parameter, UINT value,
                                                  UINT offset) {
    auto *const list = static_cast<ListProxy *>(self);
    keep_constants(list, false, parameter, 1, &value, offset);
    if (!list->ours_bound)
        host_of(self)->SetGraphicsRoot32BitConstant(parameter, value, offset);
}
void STDMETHODCALLTYPE set_graphics_root_constants(void *self, UINT parameter, UINT count,
                                                   const void *data, UINT offset) {
    auto *const list = static_cast<ListProxy *>(self);
    keep_constants(list, false, parameter, count, data, offset);
    if (!list->ours_bound)
        host_of(self)->SetGraphicsRoot32BitConstants(parameter, count, data, offset);
}
void STDMETHODCALLTYPE set_compute_root_constant(void *self, UINT parameter, UINT value,
                                                 UINT offset) {
    auto *const list = static_cast<ListProxy *>(self);
    keep_constants(list, true, parameter, 1, &value, offset);
    if (!list->compute_ours_bound)
        host_of(self)->SetComputeRoot32BitConstant(parameter, value, offset);
}
void STDMETHODCALLTYPE set_compute_root_constants(void *self, UINT parameter, UINT count,
                                                  const void *data, UINT offset) {
    auto *const list = static_cast<ListProxy *>(self);
    keep_constants(list, true, parameter, count, data, offset);
    if (!list->compute_ours_bound)
        host_of(self)->SetComputeRoot32BitConstants(parameter, count, data, offset);
}
void STDMETHODCALLTYPE set_graphics_root_cbv(void *self, UINT index, std::uint64_t address);
void STDMETHODCALLTYPE set_graphics_root_srv(void *self, UINT index, std::uint64_t address);
void STDMETHODCALLTYPE set_graphics_root_uav(void *self, UINT index, std::uint64_t address);
void STDMETHODCALLTYPE set_compute_root_cbv(void *self, UINT index, std::uint64_t address);
void STDMETHODCALLTYPE set_compute_root_srv(void *self, UINT index, std::uint64_t address);
void STDMETHODCALLTYPE set_compute_root_uav(void *self, UINT index, std::uint64_t address);
// What one stage of a pipeline that reads the heaps itself has in its user registers, from
// `first` on: a table's first descriptor, constants as they are, from the title's root
// arguments; the descriptors it reads are made the host's, and a root view it reads is set
// with `view` (2 shader resource, 3 constant buffer, 4 unordered access: the root
// parameter of heap_signature, and the host's address)
template <class View>
void fill_user(ListProxy *list, const HeapUses::Stage &stage,
               const std::vector<ListProxy::RootArgument> &arguments, std::uint32_t first,
               std::uint32_t (&user)[24], View view) {
    std::uintptr_t tables[24]{};
    ShadowHeap heaps[24]{};
    std::memset(user, 0, sizeof user);
    for (const auto &entry : stage.entries) {
        const auto at = entry.reg - first;
        const auto *const argument =
            entry.slot < arguments.size() ? &arguments[entry.slot] : nullptr;
        if (entry.reg < first || at >= 24 || !argument)
            continue;
        if (entry.kind == 1 && argument->kind == 1) {
            {
                const std::lock_guard lock(shadow_mutex);
                if (const auto *const in = shadow_heap(argument->value); in && in->shader_visible) {
                    heaps[at] = *in;
                    tables[at] = argument->value;
                }
            }
            // the table's first descriptor in the host's heap, for as many as the shader reads
            if (tables[at]) {
                UINT span = 1;
                for (const auto &use : stage.uses)
                    if (use.reg == entry.reg)
                        span = std::max(span, use.place + 1);
                user[at] = visible_slot(list, heaps[at],
                                        (tables[at] - heaps[at].base) / heaps[at].stride, span);
            }
        } else if (entry.kind == 5 && argument->kind == 5) {
            for (UINT n = 0; n < entry.count && at + n < 24; ++n)
                if (n < argument->words.size())
                    user[at + n] = argument->words[n];
        } else if (entry.kind == 2 && argument->kind == 3) {
            view(2, root_address(argument->value));
        } else if (entry.kind == 2 && argument->kind == 2) {
            view(3, root_constants(argument->value));
        } else if (entry.kind == 2 && argument->kind == 4) {
            view(4, root_uav_address(argument->value));
        }
    }
    for (const auto &use : stage.uses)
        if (const auto at = use.reg - first; use.reg >= first && at < 24 && tables[at]) {
            const auto where = tables[at] + std::uintptr_t{use.place} * heaps[at].stride;
            if (where + heaps[at].stride > heaps[at].end)
                continue;
            // a view of the title's memory is of that memory as it is now
            std::uint64_t words[2];
            std::memcpy(words, reinterpret_cast<const void *>(where), sizeof words);
            if (words[0] == host_mark)
                refresh_memory_view(words[1]);
            decode_as(heaps[at], where, use.type);
            if (use.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && use.stride > 4)
                structured_view(heaps[at].device, host_slot(heaps[at], where).ptr, use.stride);
        }
    if (!stage.samplers.empty()) {
        ShadowHeap samplers{};
        {
            const std::lock_guard lock(shadow_mutex);
            for (const auto &[base, heap] : shadow_heaps)
                if (heap.type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && heap.shader_visible)
                    samplers = heap;
        }
        for (const auto in : stage.samplers)
            if (const auto at = in - first; in >= first && at < 24 && samplers.base) {
                const auto where = samplers.base + std::uintptr_t{user[at]} * samplers.stride;
                if (where + samplers.stride <= samplers.end)
                    decode_as(samplers, where, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
            }
    }
}
// Before a dispatch, as prepare_draw before a draw: a compute pipeline that reads the heaps
// itself gets heap_signature and its user registers (from s0) in the first root constants;
// any other gets the title's signature and arguments back if ours was on.
void prepare_dispatch(ListProxy *list) {
    const CopyingFor copying(list);
    const bool ours = list->heap_uses && list->heap_uses->compute;
    if (!ours) {
        if (!list->compute_ours_bound)
            return;
        list->compute_ours_bound = false;
        list->host->SetComputeRootSignature(const_cast<ID3D12RootSignature *>(
            static_cast<const ID3D12RootSignature *>(list->compute_root)));
        const auto arguments = list->compute_arguments;
        for (UINT parameter = 0; parameter < arguments.size(); ++parameter) {
            const auto &argument = arguments[parameter];
            if (argument.kind == 1)
                set_compute_root_table(list, parameter, {argument.value});
            else if (argument.kind == 2)
                set_compute_root_cbv(list, parameter, argument.value);
            else if (argument.kind == 3)
                set_compute_root_srv(list, parameter, argument.value);
            else if (argument.kind == 4)
                set_compute_root_uav(list, parameter, argument.value);
            else if (argument.kind == 5 && !argument.words.empty())
                list->host->SetComputeRoot32BitConstants(
                    parameter, static_cast<UINT>(argument.words.size()), argument.words.data(), 0);
        }
        return;
    }
    auto *const signature = heap_signature(d3d12_device.load());
    if (!signature)
        return;
    if (!list->compute_ours_bound) {
        list->host->SetComputeRootSignature(signature);
        list->compute_ours_bound = true;
    }
    std::uint32_t user[24];
    fill_user(list, list->heap_uses->stages[0], list->compute_arguments, 0, user,
              [&](UINT parameter, std::uint64_t address) {
                  if (parameter == 2)
                      list->host->SetComputeRootShaderResourceView(parameter, address);
                  else if (parameter == 3)
                      list->host->SetComputeRootConstantBufferView(parameter, address);
                  else
                      list->host->SetComputeRootUnorderedAccessView(parameter, address);
              });
    list->host->SetComputeRoot32BitConstants(0, 24, user, 0);
}
// Before a draw: the host's list gets the root signature the pipeline is under. One of the
// title's cache that reads the heap is told where the table the title draws with starts
// (its first root parameter, as the programs have it in s8), and what the shaders read
// there is made the host's; any other pipeline gets the title's signature and arguments
// back if ours was on.
void prepare_draw(ListProxy *list) {
    const CopyingFor copying(list);
    if (!list->heap_uses || list->heap_uses->compute) {
        if (!list->ours_bound)
            return;
        list->ours_bound = false;
        list->host->SetGraphicsRootSignature(const_cast<ID3D12RootSignature *>(
            static_cast<const ID3D12RootSignature *>(list->graphics_root)));
        const auto arguments = list->graphics_arguments;
        for (UINT parameter = 0; parameter < arguments.size(); ++parameter) {
            const auto &argument = arguments[parameter];
            if (argument.kind == 1)
                set_graphics_root_table(list, parameter, {argument.value});
            else if (argument.kind == 2)
                set_graphics_root_cbv(list, parameter, argument.value);
            else if (argument.kind == 3)
                set_graphics_root_srv(list, parameter, argument.value);
            else if (argument.kind == 4)
                set_graphics_root_uav(list, parameter, argument.value);
            else if (argument.kind == 5 && !argument.words.empty())
                list->host->SetGraphicsRoot32BitConstants(
                    parameter, static_cast<UINT>(argument.words.size()), argument.words.data(), 0);
        }
        return;
    }
    auto *const ours = heap_signature(d3d12_device.load());
    if (!ours)
        return;
    if (!list->ours_bound) {
        list->host->SetGraphicsRootSignature(ours);
        list->ours_bound = true;
    }
    // the user registers from s8 on: a table's first descriptor, constants as they are
    std::uint32_t user[24]{};
    const auto &arguments = list->graphics_arguments;
    for (UINT index = 0; index < 2; ++index) {
        fill_user(list, list->heap_uses->stages[index], arguments, 8, user,
                  [&](UINT parameter, std::uint64_t address) {
                      if (parameter == 2)
                          list->host->SetGraphicsRootShaderResourceView(parameter, address);
                      else if (parameter == 3)
                          list->host->SetGraphicsRootConstantBufferView(parameter, address);
                      else
                          list->host->SetGraphicsRootUnorderedAccessView(parameter, address);
                  });
        list->host->SetGraphicsRoot32BitConstants(index, 24, user, 0);
    }
    // XV_D3D_LOST: what the first of these draws are given
    if (static std::atomic<int> shown{0};
        std::getenv("XV_D3D_LOST") && (shown++ < 24 || lost_again)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC made{};
        {
            const std::lock_guard lock(kept_mutex);
            if (const auto kept = kept_pipelines.find(list->pipeline); kept != kept_pipelines.end())
                made = kept->second.desc;
        }
        D3D12_RESOURCE_DESC target{};
        if (list->target)
            target = list->target->GetDesc();
        std::string text;
        for (std::size_t p = 0; p < arguments.size(); ++p) {
            char one[64];
            std::snprintf(one, sizeof one, " %zu:%d/%llx", p, arguments[p].kind,
                          arguments[p].kind == 5 && !arguments[p].words.empty()
                              ? static_cast<unsigned long long>(arguments[p].words[0])
                              : static_cast<unsigned long long>(arguments[p].value));
            text += one;
        }
        std::fprintf(stderr,
                     "[Lost] a draw with the title's programs: pipeline %p, %u targets of format "
                     "%d, depth %d, %u inputs; bound target %llux%u format %d; topology %u; "
                     "registers %x %x %x %x %x %x; %zu parameters, arguments%s; shaders %s%c",
                     static_cast<void *>(list->pipeline), made.NumRenderTargets, made.RTVFormats[0],
                     made.DSVFormat, made.InputLayout.NumElements, target.Width, target.Height,
                     target.Format, list->topology, user[0], user[1], user[2], user[3], user[4],
                     user[5], list->heap_uses->stages[1].entries.size(), text.c_str(),
                     list->heap_uses->made.c_str(), 10);
        {
            float first[18]{};
            MEMORY_BASIC_INFORMATION info{};
            const auto at = list->vertices.BufferLocation;
            const bool memory =
                at && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof info) &&
                info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (memory)
                std::memcpy(first, reinterpret_cast<const void *>(at), sizeof first);
            std::fprintf(stderr,
                         "[Lost]   vertices set %u times on this list; stream 0 at %llx, %u bytes, "
                         "stride %u, %s: %g %g %g %g %g %g %g %g; then %g %g; %g %g %g; %g %g%c",
                         list->vertex_calls, static_cast<unsigned long long>(at),
                         list->vertices.SizeInBytes, list->vertices.StrideInBytes,
                         memory ? "memory" : "not memory here", first[0], first[1], first[2],
                         first[3], first[4], first[5], first[6], first[7], first[10], first[11],
                         first[12], first[13], first[14], first[16], first[17], 10);
        }
        // what each stage reads, by the table its register has
        for (UINT index = 0; index < 2; ++index) {
            const auto &stage = list->heap_uses->stages[index];
            for (const auto &entry : stage.entries) {
                const auto *const argument =
                    entry.slot < arguments.size() ? &arguments[entry.slot] : nullptr;
                std::fprintf(stderr,
                             "[Lost]   %s s%u: kind %d x%u of parameter %u, given %d/%llx%c",
                             index ? "pixel" : "vertex", entry.reg, entry.kind, entry.count,
                             entry.slot, argument ? argument->kind : 0,
                             static_cast<unsigned long long>(argument ? argument->value : 0), 10);
            }
            for (const auto &use : stage.uses) {
                std::uintptr_t table = 0;
                ShadowHeap heap{};
                for (const auto &entry : stage.entries)
                    if (entry.kind == 1 && entry.reg == use.reg && entry.slot < arguments.size() &&
                        arguments[entry.slot].kind == 1) {
                        const std::lock_guard lock(shadow_mutex);
                        if (const auto *const in = shadow_heap(arguments[entry.slot].value)) {
                            heap = *in;
                            table = arguments[entry.slot].value;
                        }
                    }
                if (!table)
                    continue;
                std::uint64_t words[4]{};
                std::memcpy(
                    words,
                    reinterpret_cast<const void *>(table + std::uintptr_t{use.place} * heap.stride),
                    std::min<std::size_t>(sizeof words, heap.stride));
                ID3D12Resource *resource = nullptr;
                std::uint64_t offset = 0, room = 0;
                const bool marked = words[0] == host_mark;
                const bool buffer =
                    !marked && buffer_at(words[0] & 0xffff'ffff'ffffull, resource, offset, room);
                std::fprintf(
                    stderr,
                    "[Lost]   %s s%u place %u type %d (heap slot %llu): %016llx %016llx "
                    "%016llx %016llx, %s%c",
                    index ? "pixel" : "vertex", use.reg, use.place, use.type,
                    static_cast<unsigned long long>((table - heap.base) / heap.stride + use.place),
                    static_cast<unsigned long long>(words[0]),
                    static_cast<unsigned long long>(words[1]),
                    static_cast<unsigned long long>(words[2]),
                    static_cast<unsigned long long>(words[3]),
                    marked   ? "the host's"
                    : buffer ? "the title's, of a buffer known here"
                             : "the title's, of NO buffer known here",
                    10);
                // the memory a view of the title's memory is of, as it is now
                MemoryView view;
                bool kept = false;
                {
                    const std::lock_guard lock(memory_view_mutex);
                    if (const auto found = lost_views.find(words[1]);
                        marked && found != lost_views.end())
                        std::fprintf(stderr, "[Lost]     %s%c", found->second.c_str(), 10);
                    if (const auto found = memory_views.find(words[1]);
                        marked && found != memory_views.end()) {
                        view = found->second;
                        kept = true;
                    }
                }
                if (kept) {
                    std::uint64_t address = view.buffer.BufferLocation;
                    if (!view.constants)
                        std::memcpy(&address,
                                    view.resource + sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC),
                                    sizeof address);
                    else if (view.buffer.SizeInBytes >= 0x1a0)
                        address += 0x160;
                    float first[16]{};
                    MEMORY_BASIC_INFORMATION info{};
                    if (VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                        info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                        std::memcpy(first, reinterpret_cast<const void *>(address), sizeof first);
                    std::fprintf(stderr,
                                 "[Lost]     of memory at %llx (%s): %g %g %g %g | %g %g %g %g | "
                                 "%g %g %g %g | %g %g %g %g%c",
                                 static_cast<unsigned long long>(address),
                                 view.constants ? "constants" : "a buffer", first[0], first[1],
                                 first[2], first[3], first[4], first[5], first[6], first[7],
                                 first[8], first[9], first[10], first[11], first[12], first[13],
                                 first[14], first[15], 10);
                }
            }
        }
        std::fflush(stderr);
    }
}
// XV_D3D_PROBE: draws per render target
std::mutex draw_mutex;
std::map<ID3D12Resource *, std::uint64_t> draws_into;
// XV_CAPTURE: each draw of the frame kept, with what it draws
void capture_call(void *self, const char *method, UINT count, UINT instances, UINT first,
                  INT base) {
    if (!capturing())
        return;
    const auto *const list = static_cast<ListProxy *>(self);
    std::fprintf(
        stderr,
        "[Capture] %s %u x%u from %u base %d, topology %u, pipeline %p%s, target %p, "
        "vertices %llx (%u bytes, stride %u), indices %llx (%u bytes, format %d)\n",
        method, count, instances, first, base, list->topology, static_cast<void *>(list->pipeline),
        list->heap_uses ? " (heaps)" : "", static_cast<void *>(list->target),
        static_cast<unsigned long long>(list->vertices.BufferLocation), list->vertices.SizeInBytes,
        list->vertices.StrideInBytes, static_cast<unsigned long long>(list->indices.BufferLocation),
        list->indices.SizeInBytes, list->indices.Format);
}
void count_draw(void *self) {
    capture_draw(static_cast<ListProxy *>(self)->target);
    static const bool probing = std::getenv("XV_D3D_PROBE") != nullptr;
    if (!probing)
        return;
    const std::lock_guard lock(draw_mutex);
    ++draws_into[static_cast<ListProxy *>(self)->target];
}
constexpr UINT rectangle_list = 17, quad_list = 19;
void STDMETHODCALLTYPE set_topology(void *self, UINT topology) {
    static_cast<ListProxy *>(self)->topology = topology;
    host_of(self)->IASetPrimitiveTopology(topology == rectangle_list || topology == quad_list
                                              ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                              : static_cast<D3D_PRIMITIVE_TOPOLOGY>(topology));
}
// Indices that draw quads as two triangles each, for at least `quads` of them: 0 1 2, 0 2 3,
// then the same four vertices on. One buffer that only grows; the ones it outgrew stay
// alive, since lists already recorded may still use them.
D3D12_INDEX_BUFFER_VIEW quad_indices(UINT quads) {
    static std::mutex mutex;
    static ID3D12Resource *buffer = nullptr;
    static UINT capacity = 0;
    const std::lock_guard lock(mutex);
    auto *const device = d3d12_device.load();
    if (quads > capacity && device) {
        const UINT wanted = std::max(quads, std::max(capacity * 2, 4096u));
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                       0,
                                       std::uint64_t{wanted} * 6 * sizeof(std::uint32_t),
                                       1,
                                       1,
                                       1,
                                       DXGI_FORMAT_UNKNOWN,
                                       {1, 0},
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource *larger = nullptr;
        void *mapped = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&larger))) &&
            SUCCEEDED(larger->Map(0, nullptr, &mapped))) {
            auto *const index = static_cast<std::uint32_t *>(mapped);
            constexpr UINT corners[] = {0, 1, 2, 0, 2, 3};
            for (UINT quad = 0; quad < wanted; ++quad)
                for (UINT corner = 0; corner < 6; ++corner)
                    index[quad * 6 + corner] = quad * 4 + corners[corner];
            larger->Unmap(0, nullptr);
            buffer = larger;
            capacity = wanted;
        }
    }
    if (!buffer || quads > capacity)
        return {};
    return {buffer->GetGPUVirtualAddress(), quads * 6 * static_cast<UINT>(sizeof(std::uint32_t)),
            DXGI_FORMAT_R32_UINT};
}
// A quad list is drawn with indices of ours over the title's vertices. A rectangle list
// gives three corners of each rectangle and the GPU makes the fourth: here the pipeline's
// geometry shader does (see rectangle_pipeline).
// The host's driver faults on the pipelines a library of the console's that builds ray
// tracing structures makes; the list is left unexecuted then (ListProxy::broken).
bool host_set_pipeline(ID3D12GraphicsCommandList *list, ID3D12PipelineState *state) {
    __try {
        list->SetPipelineState(state);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void STDMETHODCALLTYPE set_pipeline_state(void *self, ID3D12PipelineState *state) {
    auto *const list = static_cast<ListProxy *>(self);
    list->no_pipeline = state && !(state = list_pipeline(state));
    list->pipeline = state;
    list->heap_uses = state ? heap_uses(state) : nullptr;
    if (state && !host_set_pipeline(list->host, state)) {
        if (static std::atomic<bool> told{false}; !told.exchange(true)) {
            std::fprintf(stderr,
                         "[Runtime] the host's driver faults on a pipeline set from %s: its list "
                         "is not executed\n",
                         detail::guest_location(_ReturnAddress()).c_str());
            std::fflush(stderr);
        }
        list->no_pipeline = true;
        list->broken = true;
    }
}
// XV_D3D_LOST: what the first draws are made with
void lost_draw(void *self, const char *method, UINT count) {
    // how the pipelines the title draws with were made, every so many draws
    if (static const bool checking = std::getenv("XV_D3D_LOST") != nullptr; checking) {
        static std::mutex mutex;
        static std::map<std::string, std::uint64_t> kinds;
        static std::uint64_t draws = 0;
        std::string kind = "not known";
        {
            const std::lock_guard lock(kept_mutex);
            if (const auto made = made_pipelines.find(static_cast<ListProxy *>(self)->pipeline);
                made != made_pipelines.end())
                kind = made->second.rfind("stream", 0) == 0
                           ? "stream" +
                                 made->second.substr(
                                     made->second.rfind(", ", made->second.rfind(", root") - 1),
                                     made->second.rfind(", root") -
                                         made->second.rfind(", ", made->second.rfind(", root") - 1))
                           : made->second.substr(0, made->second.find(", root"));
        }
        const std::lock_guard lock(mutex);
        if (static_cast<ListProxy *>(self)->heap_uses)
            kind += ", of the title's programs";
        ++kinds[kind];
        static const std::uint64_t again = [] {
            const auto *const text = std::getenv("XV_D3D_LOST_AT");
            return text ? std::strtoull(text, nullptr, 10) : 0ull;
        }();
        lost_again = again && draws >= again && draws < again + 12;
        if (++draws % 500 == 0) {
            std::fprintf(stderr, "[Lost] %llu draws:", static_cast<unsigned long long>(draws));
            for (const auto &[name, number] : kinds)
                std::fprintf(stderr, " %llu %s;", static_cast<unsigned long long>(number),
                             name.c_str());
            std::fprintf(stderr, "%c", 10);
            std::fflush(stderr);
        }
    }
    static std::atomic<int> shown{0};
    if (!std::getenv("XV_D3D_LOST") || (shown++ >= 16 && !lost_again))
        return;
    auto *const list = static_cast<ListProxy *>(self);
    const void *root = nullptr;
    bool known = false;
    std::string how = "?";
    {
        const std::lock_guard lock(kept_mutex);
        if (const auto made = made_pipelines.find(list->pipeline); made != made_pipelines.end())
            how = made->second;
        if (const auto found = kept_pipelines.find(list->pipeline); found != kept_pipelines.end()) {
            known = true;
            root = found->second.desc.pRootSignature;
        }
    }
    {
        const std::lock_guard lock(signature_mutex);
        const auto found = signature_text.find(list->graphics_root);
        std::fprintf(stderr, "[Lost] root signature %p: %s%c", list->graphics_root,
                     found == signature_text.end() ? "not read" : found->second.c_str(), 10);
    }
    auto *const device = d3d12_device.load();
    std::fprintf(stderr,
                 "[Lost] thread %lu list %p %s(%u): pipeline %p (%s; %s, its root signature %p), "
                 "root signature set %p, topology %u, device 0x%08lx%c",
                 GetCurrentThreadId(), self, method, count, static_cast<void *>(list->pipeline),
                 how.c_str(), known ? "kept" : "not kept", root, list->graphics_root,
                 list->topology,
                 device ? static_cast<unsigned long>(device->GetDeviceRemovedReason()) : 0ul, 10);
    std::fflush(stderr);
}
void STDMETHODCALLTYPE draw_instanced(void *self, UINT vertices, UINT instances, UINT first,
                                      UINT first_instance) {
    if (static_cast<ListProxy *>(self)->no_pipeline)
        return;
    count_draw(self);
    capture_call(self, "DrawInstanced", vertices, instances, first, 0);
    lost_draw(self, "DrawInstanced", vertices);
    auto *const list = static_cast<ListProxy *>(self);
    prepare_draw(list);
    // XV_D3D_LOST_PSO: the draw with a pipeline of ours that draws nothing, to tell whether
    // it is the title's pipeline the host drops the device for
    if (std::getenv("XV_D3D_LOST_PSO") && list->graphics_root) {
        const auto vertex = modules::empty_vertex_shader(), pixel = modules::empty_pixel_shader();
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = const_cast<ID3D12RootSignature *>(
            static_cast<const ID3D12RootSignature *>(list->graphics_root));
        desc.VS = {vertex.data(), vertex.size()};
        desc.PS = {pixel.data(), pixel.size()};
        desc.SampleMask = ~0u;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.SampleDesc.Count = 1;
        ID3D12PipelineState *ours = nullptr;
        const auto made =
            d3d12_device.load()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&ours));
        std::fprintf(stderr, "[Lost] a pipeline of ours for the draw: 0x%08lx%c",
                     static_cast<unsigned long>(made), 10);
        std::fflush(stderr);
        if (ours)
            list->host->SetPipelineState(ours);
    }
    if (list->topology == quad_list) {
        const auto view = quad_indices(vertices / 4);
        if (!view.BufferLocation)
            return;
        list->host->IASetIndexBuffer(&view);
        list->host->DrawIndexedInstanced(vertices / 4 * 6, instances, 0, static_cast<INT>(first),
                                         first_instance);
        list->host->IASetIndexBuffer(list->indices.BufferLocation ? &list->indices : nullptr);
        return;
    }
    host_of(self)->DrawInstanced(vertices, instances, first, first_instance);
}
void STDMETHODCALLTYPE draw_indexed_instanced(void *self, UINT indices, UINT instances, UINT first,
                                              INT base, UINT first_instance) {
    if (static_cast<ListProxy *>(self)->no_pipeline)
        return;
    count_draw(self);
    capture_call(self, "DrawIndexedInstanced", indices, instances, first, base);
    lost_draw(self, "DrawIndexedInstanced", indices);
    prepare_draw(static_cast<ListProxy *>(self));
    if (static std::atomic<bool> shown{false};
        static_cast<ListProxy *>(self)->topology == quad_list && !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] an indexed quad list is drawn as a triangle list, which "
                             "is not the same picture\n");
        std::fflush(stderr);
    }
    host_of(self)->DrawIndexedInstanced(indices, instances, first, base, first_instance);
}
// XV_D3D_TRACE: a GPU address the title gives, and whether it is CPU memory of the process
void trace_address(const char *method, std::uint64_t address) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    static std::atomic<int> calls{0};
    if (!tracing || calls++ >= 24)
        return;
    MEMORY_BASIC_INFORMATION info{};
    const bool cpu = VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                     info.State == MEM_COMMIT;
    std::fprintf(stderr, "[Runtime] Direct3D 12 %s(%llx)%s\n", method,
                 static_cast<unsigned long long>(address), cpu ? " in the title's memory" : "");
    std::fflush(stderr);
}
// On the Series console vertices and indices may lie in any memory the title has, as its
// root descriptors may (see ring_copy): what is in no buffer here is copied into the ring as
// it is when it is bound, all of it. `bytes` becomes no more than the buffer here or the copy
// has: the host's GPU reads past a buffer's end into whatever follows it, or faults.
std::uint64_t within_buffer(std::uint64_t address, std::uint64_t bytes) {
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, room = 0;
    return buffer_at(address, resource, offset, room) ? std::min(bytes, room) : bytes;
}
std::uint64_t geometry_address(const char *what, std::uint64_t address, UINT &bytes) {
    bytes = static_cast<UINT>(within_buffer(address, bytes));
    if (!address || console != Target::scarlett) {
        check_address(what, address, bytes);
        return gpu_address(address);
    }
    {
        const std::lock_guard lock(buffer_mutex);
        auto found = mapped_at.upper_bound(address);
        if (found != mapped_at.begin() && address < std::prev(found)->second.end)
            return std::prev(found)->second.gpu + (address - std::prev(found)->first);
        auto buffer = buffers_at.upper_bound(address);
        if (buffer != buffers_at.begin() && address < std::prev(buffer)->second.end)
            return address;
    }
    // as much of it as is memory
    const auto readable = readable_bytes(reinterpret_cast<const void *>(address), bytes);
    const auto copy =
        readable ? ring_copy(reinterpret_cast<const void *>(address), readable, readable) : 0;
    if (copy)
        bytes = static_cast<UINT>(readable);
    else
        check_address(what, address, bytes);
    // memory the host's GPU cannot see and has no copy of is 0: no buffer, whose reads give 0
    return copy ? copy : gpu_address(address);
}
void STDMETHODCALLTYPE set_vertex_buffers(void *self, UINT start, UINT count,
                                          const D3D12_VERTEX_BUFFER_VIEW *views) {
    const CopyingFor copying(self);
    if (views && count)
        trace_address("IASetVertexBuffers", views->BufferLocation);
    std::vector<D3D12_VERTEX_BUFFER_VIEW> host;
    if (views && count && start == 0)
        static_cast<ListProxy *>(self)->vertices = views[0];
    ++static_cast<ListProxy *>(self)->vertex_calls;
    if (views) {
        host.assign(views, views + count);
        for (auto &view : host) {
            view.BufferLocation =
                geometry_address("a vertex buffer", view.BufferLocation, view.SizeInBytes);
            if (!view.BufferLocation)
                view.SizeInBytes = 0;
        }
    }
    host_of(self)->IASetVertexBuffers(start, count, views ? host.data() : nullptr);
}
void STDMETHODCALLTYPE set_index_buffer(void *self, const D3D12_INDEX_BUFFER_VIEW *view) {
    const CopyingFor copying(self);
    if (view)
        trace_address("IASetIndexBuffer", view->BufferLocation);
    auto host = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
    host.BufferLocation =
        geometry_address("an index buffer", host.BufferLocation, host.SizeInBytes);
    if (!host.BufferLocation)
        host.SizeInBytes = 0;
    static_cast<ListProxy *>(self)->indices = host;
    host_of(self)->IASetIndexBuffer(view ? &host : nullptr);
}
// On the Series console any memory the GPU can see may be given as a root descriptor, a
// buffer or not. An address that is memory of the title's and in no buffer here is copied
// into a buffer of ours, which is what the host is given: 64 KiB of it, or as much as is
// there. The copies go round a ring, which a frame does not come near filling.
constexpr std::uint64_t most_copied = 64 << 10;
// the GPU address of a copy of `bytes` (most_copied at most) at `source`, 0 if there is no ring;
// with `align`, the copy starts on a multiple of it in the ring, which `buffer` and `offset`
// are given
std::uint64_t ring_copy(const void *source, std::uint64_t bytes, std::uint64_t most,
                        std::uint64_t align, ID3D12Resource **buffer, std::uint64_t *offset) {
    constexpr std::uint64_t ring_size = 256ull << 20;
    most = std::min(most, ring_size);
    static std::mutex mutex;
    static ID3D12Resource *ring = nullptr;
    static std::uint8_t *memory = nullptr;
    static std::uint64_t next = 0;
    const std::lock_guard lock(mutex);
    if (!ring) {
        auto *const device = d3d12_device.load();
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = ring_size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void *mapped = nullptr;
        if (!device ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&ring))) ||
            FAILED(ring->Map(0, nullptr, &mapped))) {
            ring = nullptr;
            return 0;
        }
        memory = static_cast<std::uint8_t *>(mapped);
        std::fprintf(stderr, "[Runtime] root descriptors the host cannot take where they are are "
                             "copied into a buffer of its own\n");
        std::fflush(stderr);
    }
    bytes = std::min(bytes, most);
    if (next + most + align > ring_size)
        next = 0;
    const auto at = align ? (next + align - 1) / align * align : next;
    next = (at + bytes + 255) & ~255ull;
    // A list's copy is made when a queue is given the list, each time it is, as the title
    // has the memory then: titles record their lists first and fill what they draw with
    // after. Memory the title has let go of by then is left as it was; a copy may span many
    // regions of memory, as the title's vertices do.
    if (copying_for) {
        const auto *const from = static_cast<const std::uint8_t *>(source);
        auto *const to = memory + at;
        when_run(copying_for, [from, to, bytes] {
            std::memcpy(to, from, static_cast<std::size_t>(readable_bytes(from, bytes)));
        });
    }
    std::memcpy(memory + at, source, bytes);
    if (buffer)
        *buffer = ring;
    if (offset)
        *offset = at;
    return ring->GetGPUVirtualAddress() + at;
}
// Buffers of ours for a root descriptor of memory the host's GPU cannot see and cannot have a
// copy of (see gpu_address): one of zeros to read, and one to write that nothing reads. A
// root descriptor has no null form, and the GPU faults at address 0.
std::uint64_t unseen_root_address(bool written) {
    static std::mutex mutex;
    static ID3D12Resource *buffers[2]{};
    const std::lock_guard lock(mutex);
    auto *&buffer = buffers[written ? 1 : 0];
    if (auto *const device = d3d12_device.load(); !buffer && device) {
        const D3D12_HEAP_PROPERTIES heap{written ? D3D12_HEAP_TYPE_DEFAULT
                                                 : D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = most_copied;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags =
            written ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        // the one to write starts common, which a list takes it out of for its writes itself
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   written ? D3D12_RESOURCE_STATE_COMMON
                                                           : D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&buffer))))
            buffer = nullptr;
    }
    return buffer ? buffer->GetGPUVirtualAddress() : 0;
}
std::uint64_t root_address(std::uint64_t address) {
    if (const auto host = gpu_address(address); host || !address)
        return host;
    if (const auto readable = readable_bytes(reinterpret_cast<const void *>(address), most_copied))
        if (const auto copy = ring_copy(reinterpret_cast<const void *>(address), readable))
            return copy;
    return unseen_root_address(false);
}
// A root UAV of memory the host's GPU cannot see writes where nothing reads
std::uint64_t root_uav_address(std::uint64_t address) {
    const auto host = gpu_address(address);
    return host || !address ? host : unseen_root_address(true);
}
// A constant buffer starts anywhere on the console and on a multiple of 256 bytes on the
// host, which turns the whole list down for one that does not. What a buffer the CPU can
// read holds from such an address on is copied to where the host takes it.
std::uint64_t root_constants(std::uint64_t address) {
    const auto host = root_address(address);
    // only Xbox One titles have been seen to need it, and a Series title that binds its
    // constants this way for every draw would have 64 KiB copied each time
    if (!(host & 255) || console != Target::durango)
        return host;
    ID3D12Resource *buffer = nullptr;
    std::uint64_t start = 0, end = 0;
    {
        const std::lock_guard lock(buffer_mutex);
        const auto found = buffers_at.upper_bound(host);
        if (found == buffers_at.begin() || host >= std::prev(found)->second.end)
            return host;
        start = std::prev(found)->first;
        end = std::prev(found)->second.end;
        buffer = std::prev(found)->second.resource;
    }
    void *memory = nullptr;
    const D3D12_RANGE nothing_read{0, 0};
    if (FAILED(buffer->Map(0, &nothing_read, &memory)) || !memory)
        return host;
    const auto copy =
        ring_copy(static_cast<const std::uint8_t *>(memory) + (host - start), end - host);
    buffer->Unmap(0, &nothing_read);
    return copy ? copy : host;
}
void STDMETHODCALLTYPE set_graphics_root_cbv(void *self, UINT index, std::uint64_t address) {
    const CopyingFor copying(self);
    static_cast<ListProxy *>(self)->argument(index) = {2, address, {}};
    if (static_cast<ListProxy *>(self)->ours_bound)
        return;
    trace_address("SetGraphicsRootConstantBufferView", address);
    const auto host = root_constants(address);
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 16) {
        std::fprintf(stderr,
                     "[Lost] thread %lu list %p SetGraphicsRootConstantBufferView(%u, %llx) -> "
                     "%llx%c",
                     GetCurrentThreadId(), self, index, static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(host), 10);
        std::fflush(stderr);
    }
    host_of(self)->SetGraphicsRootConstantBufferView(index, host);
}
void STDMETHODCALLTYPE set_compute_root_cbv(void *self, UINT index, std::uint64_t address) {
    const CopyingFor copying(self);
    auto *const list = static_cast<ListProxy *>(self);
    list->argument(index, true) = {2, address, {}};
    if (!list->compute_ours_bound)
        host_of(self)->SetComputeRootConstantBufferView(index, root_constants(address));
}
void STDMETHODCALLTYPE set_compute_root_srv(void *self, UINT index, std::uint64_t address) {
    const CopyingFor copying(self);
    auto *const list = static_cast<ListProxy *>(self);
    list->argument(index, true) = {3, address, {}};
    if (!list->compute_ours_bound)
        host_of(self)->SetComputeRootShaderResourceView(index, root_address(address));
}
void STDMETHODCALLTYPE set_graphics_root_srv(void *self, UINT index, std::uint64_t address) {
    const CopyingFor copying(self);
    static_cast<ListProxy *>(self)->argument(index) = {3, address, {}};
    if (static_cast<ListProxy *>(self)->ours_bound)
        return;
    const auto host = root_address(address);
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 16) {
        MEMORY_BASIC_INFORMATION info{};
        const bool cpu = VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                         info.State == MEM_COMMIT;
        std::fprintf(stderr,
                     "[Lost] thread %lu list %p SetGraphicsRootShaderResourceView(%u, %llx%s) "
                     "-> %llx, from %s%c",
                     GetCurrentThreadId(), self, index, static_cast<unsigned long long>(address),
                     cpu ? " = memory of the title's" : "", static_cast<unsigned long long>(host),
                     detail::guest_location(_ReturnAddress()).c_str(), 10);
        std::fflush(stderr);
    }
    host_of(self)->SetGraphicsRootShaderResourceView(index, host);
}
void STDMETHODCALLTYPE set_compute_root_uav(void *self, UINT index, std::uint64_t address) {
    auto *const list = static_cast<ListProxy *>(self);
    list->argument(index, true) = {4, address, {}};
    if (!list->compute_ours_bound)
        host_of(self)->SetComputeRootUnorderedAccessView(index, root_uav_address(address));
}
void STDMETHODCALLTYPE dispatch(void *self, UINT x, UINT y, UINT z) {
    if (static_cast<ListProxy *>(self)->no_pipeline)
        return;
    prepare_dispatch(static_cast<ListProxy *>(self));
    host_of(self)->Dispatch(x, y, z);
}
void STDMETHODCALLTYPE set_graphics_root_uav(void *self, UINT index, std::uint64_t address) {
    static_cast<ListProxy *>(self)->argument(index) = {4, address, {}};
    if (static_cast<ListProxy *>(self)->ours_bound)
        return;
    host_of(self)->SetGraphicsRootUnorderedAccessView(index, root_uav_address(address));
}
void STDMETHODCALLTYPE so_set_targets(void *self, UINT start, UINT count,
                                      const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) {
    std::vector<D3D12_STREAM_OUTPUT_BUFFER_VIEW> host;
    if (views) {
        host.assign(views, views + count);
        // a target in memory the host's GPU cannot see is none: what goes there is lost
        for (auto &view : host) {
            const auto location = gpu_address(view.BufferLocation),
                       filled = gpu_address(view.BufferFilledSizeLocation);
            view = (location || !view.BufferLocation) && (filled || !view.BufferFilledSizeLocation)
                       ? D3D12_STREAM_OUTPUT_BUFFER_VIEW{location,
                                                         within_buffer(view.BufferLocation,
                                                                       view.SizeInBytes),
                                                         filled}
                       : D3D12_STREAM_OUTPUT_BUFFER_VIEW{};
        }
    }
    host_of(self)->SOSetTargets(start, count, views ? host.data() : nullptr);
}
void STDMETHODCALLTYPE set_viewports(void *self, UINT count, const D3D12_VIEWPORT *viewports) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && viewports && count && calls++ < 6) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 RSSetViewports(%u: %g %g %g %g %g %g)\n", count,
                     viewports->TopLeftX, viewports->TopLeftY, viewports->Width, viewports->Height,
                     viewports->MinDepth, viewports->MaxDepth);
        std::fflush(stderr);
    }
    host_of(self)->RSSetViewports(count, viewports);
}
void STDMETHODCALLTYPE set_scissors(void *self, UINT count, const D3D12_RECT *rects) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && rects && count && calls++ < 6) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 RSSetScissorRects(%u: %ld %ld %ld %ld)\n",
                     count, rects->left, rects->top, rects->right, rects->bottom);
        std::fflush(stderr);
    }
    host_of(self)->RSSetScissorRects(count, rects);
}
void STDMETHODCALLTYPE om_set_render_targets(void *self, UINT count, const Handle *targets,
                                             BOOL single, const Handle *depth) {
    if (targets && count)
        trace_handle("OMSetRenderTargets", targets[0]);
    std::vector<Handle> host(targets ? (single ? 1 : count) : 0);
    for (std::size_t i = 0; i < host.size(); ++i)
        host[i] = host_view(targets[i]);
    // one handle for a run of descriptors: the rest follow it in the heap
    const auto stride = console_stride(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 1; targets && single && i < count; ++i)
        host_view({targets[0].ptr + std::size_t{i} * stride});
    // only for XV_D3D_PROBE: the title may have let go of a resource its view is still
    // listed for
    static const bool probing = std::getenv("XV_D3D_PROBE") != nullptr;
    if (probing && !host.empty()) {
        const std::lock_guard lock(target_mutex);
        if (const auto found = target_of.find(host[0].ptr); found != target_of.end()) {
            last_target = found->second;
            static_cast<ListProxy *>(self)->target = found->second;
            const auto desc = found->second->GetDesc();
            if (bound_targets.size() < 12 &&
                std::none_of(bound_targets.begin(), bound_targets.end(), [&](auto *other) {
                    const auto known = other->GetDesc();
                    return known.Width == desc.Width && known.Height == desc.Height &&
                           known.Format == desc.Format;
                }))
                bound_targets.push_back(found->second);
        }
    }
    Handle host_depth{};
    if (depth)
        host_depth = host_view(*depth);
    // XV_CAPTURE: what this frame draws into, and the first target for its draws
    if (capturing()) {
        ID3D12Resource *first = nullptr;
        {
            const std::lock_guard lock(target_mutex);
            for (const auto &view : host)
                if (const auto found = target_of.find(view.ptr); found != target_of.end()) {
                    capture_bound(found->second, false);
                    first = first ? first : found->second;
                }
            if (depth)
                if (const auto found = target_of.find(host_depth.ptr); found != target_of.end())
                    capture_bound(found->second, true);
        }
        static_cast<ListProxy *>(self)->target = first;
    }
    if (static std::atomic<int> shown{0};
        std::getenv("XV_D3D_LOST") && (shown++ < 16 || lost_again)) {
        const std::lock_guard lock(target_mutex);
        const auto say = [](std::uint64_t handle) {
            const auto found = view_text.find(handle);
            return found == view_text.end() ? std::string("a view never made") : found->second;
        };
        std::fprintf(stderr, "[Lost] thread %lu list %p OMSetRenderTargets(%u, single %d)%c",
                     GetCurrentThreadId(), self, count, single, 10);
        for (const auto &view : host)
            std::fprintf(stderr, "[Lost]   target %llx: %s%c",
                         static_cast<unsigned long long>(view.ptr), say(view.ptr).c_str(), 10);
        if (depth)
            std::fprintf(stderr, "[Lost]   depth %llx -> %llx: %s%c",
                         static_cast<unsigned long long>(depth->ptr),
                         static_cast<unsigned long long>(host_depth.ptr),
                         say(host_depth.ptr).c_str(), 10);
        std::fflush(stderr);
    }
    host_of(self)->OMSetRenderTargets(count, targets ? host.data() : nullptr, single,
                                      depth ? &host_depth : nullptr);
}
void STDMETHODCALLTYPE clear_depth_stencil(void *self, Handle view, D3D12_CLEAR_FLAGS flags,
                                           FLOAT depth, UINT8 stencil, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearDepthStencilView(host_view(view), flags, depth, stencil, count, rects);
}
void STDMETHODCALLTYPE clear_render_target(void *self, Handle view, const FLOAT *color, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearRenderTargetView(host_view(view), color, count, rects);
}
// ClearUnorderedAccessView needs its CPU handle in a non-shader-visible heap; a title passes
// one of its shader-visible heap. The feedback map's view is remade in a heap of ours.
std::mutex feedback_mutex;
std::map<ID3D12Resource *, ID3D12Resource *> feedback_texture; // by map: what it is paired with
struct FeedbackViews {
    ID3D12DescriptorHeap *heap = nullptr;
    UINT stride = 0, next = 0;
} feedback_views;
Handle feedback_clear_view(List *host, Handle view, ID3D12Resource *map) {
    constexpr UINT slots = 64;
    const std::lock_guard lock(feedback_mutex);
    const auto paired = feedback_texture.find(map);
    if (paired == feedback_texture.end())
        return view;
    ID3D12Device *device = nullptr;
    if (FAILED(host->GetDevice(IID_PPV_ARGS(&device))))
        return view;
    auto &views = feedback_views;
    if (!views.heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots};
        using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *,
                                                    REFIID, void **);
        original<Create>(device, 14)(device, &desc, IID_PPV_ARGS(&views.heap));
        views.stride = device->GetDescriptorHandleIncrementSize(desc.Type);
    }
    if (views.heap) {
        view = {views.heap->GetCPUDescriptorHandleForHeapStart().ptr +
                std::size_t{views.next++ % slots} * views.stride};
        using Create = void(STDMETHODCALLTYPE *)(void *, ID3D12Resource *, ID3D12Resource *,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE);
        original<Create>(device, 71)(device, paired->second, map, view);
    }
    device->Release();
    return view;
}
void STDMETHODCALLTYPE clear_uav_uint(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                      ID3D12Resource *resource, const UINT *values, UINT count,
                                      const D3D12_RECT *rects) {
    bind_heap_of(self, gpu.ptr);
    host_of(self)->ClearUnorderedAccessViewUint(
        host_gpu(static_cast<ListProxy *>(self), gpu),
        feedback_clear_view(host_of(self), host_view(cpu), resource), resource, values, count,
        rects);
}
void STDMETHODCALLTYPE clear_uav_float(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                       ID3D12Resource *resource, const FLOAT *values, UINT count,
                                       const D3D12_RECT *rects) {
    bind_heap_of(self, gpu.ptr);
    host_of(self)->ClearUnorderedAccessViewFloat(host_gpu(static_cast<ListProxy *>(self), gpu),
                                                 host_view(cpu), resource, values, count, rects);
}
void STDMETHODCALLTYPE ignored_list_method(void *) {}
void STDMETHODCALLTYPE flush_pipeline_x(void *self, UINT, std::uint64_t, std::uint64_t);
template <std::size_t... Index>
std::array<Slot, list_slots> list_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, list_slots> vtable{reinterpret_cast<Slot>(&list_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&list_query);
    vtable[1] = reinterpret_cast<Slot>(&list_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&list_release);
    vtable[9] = reinterpret_cast<Slot>(&close_list);
    vtable[10] = reinterpret_cast<Slot>(&reset_list);
    // the later layout's 139 (137 here, see list_by_layout): Reset with one argument more
    vtable[137] = reinterpret_cast<Slot>(&reset_list);
    vtable[21] = reinterpret_cast<Slot>(&set_viewports);
    vtable[25] = reinterpret_cast<Slot>(&set_pipeline_state);
    vtable[22] = reinterpret_cast<Slot>(&set_scissors);
    vtable[26] = reinterpret_cast<Slot>(&resource_barrier);
    vtable[27] = reinterpret_cast<Slot>(&execute_bundle);
    vtable[20] = reinterpret_cast<Slot>(&set_topology);
    vtable[16] = reinterpret_cast<Slot>(&copy_texture_region);
    vtable[12] = reinterpret_cast<Slot>(&draw_instanced);
    vtable[13] = reinterpret_cast<Slot>(&draw_indexed_instanced);
    vtable[32] = reinterpret_cast<Slot>(&set_descriptor_heaps);
    vtable[33] = reinterpret_cast<Slot>(&set_compute_root_signature);
    vtable[34] = reinterpret_cast<Slot>(&set_graphics_root_signature);
    vtable[35] = reinterpret_cast<Slot>(&set_compute_root_table);
    vtable[36] = reinterpret_cast<Slot>(&set_graphics_root_table);
    vtable[37] = reinterpret_cast<Slot>(&set_compute_root_constant);
    vtable[38] = reinterpret_cast<Slot>(&set_graphics_root_constant);
    vtable[39] = reinterpret_cast<Slot>(&set_compute_root_constants);
    vtable[40] = reinterpret_cast<Slot>(&set_graphics_root_constants);
    vtable[14] = reinterpret_cast<Slot>(&dispatch);
    vtable[41] = reinterpret_cast<Slot>(&set_compute_root_cbv);
    vtable[42] = reinterpret_cast<Slot>(&set_graphics_root_cbv);
    vtable[43] = reinterpret_cast<Slot>(&set_compute_root_srv);
    vtable[44] = reinterpret_cast<Slot>(&set_graphics_root_srv);
    vtable[45] = reinterpret_cast<Slot>(&set_compute_root_uav);
    vtable[46] = reinterpret_cast<Slot>(&set_graphics_root_uav);
    vtable[49] = reinterpret_cast<Slot>(&so_set_targets);
    vtable[47] = reinterpret_cast<Slot>(&set_index_buffer);
    vtable[48] = reinterpret_cast<Slot>(&set_vertex_buffers);
    vtable[50] = reinterpret_cast<Slot>(&om_set_render_targets);
    vtable[51] = reinterpret_cast<Slot>(&clear_depth_stencil);
    vtable[52] = reinterpret_cast<Slot>(&clear_render_target);
    vtable[53] = reinterpret_cast<Slot>(&clear_uav_uint);
    vtable[54] = reinterpret_cast<Slot>(&clear_uav_float);
    vtable[67] = reinterpret_cast<Slot>(&flush_pipeline_x);
    // OMSetDepthBoundsX: a depth bounds test no host pipeline here enables
    vtable[68] = reinterpret_cast<Slot>(&ignored_list_method);
    // what tunes the console's GPU and changes no picture: occlusion query and hierarchical
    // stencil controls, NopX, KickoffX, shader limits and compression state
    for (const std::size_t hint : {64, 65, 66, 73, 79, 82, 93, 95, 96, 97})
        vtable[hint] = reinterpret_cast<Slot>(&ignored_list_method);
    return vtable;
}
std::array<Slot, list_slots> list_vtable = list_vtable_of(std::make_index_sequence<list_slots>{});
// Titles with the later queue layout (see later_queue) have two more methods in their lists
// as well, at 59 and 60: every method from there on is two slots further. What the two are
// is not known. A list is made before the layout shows, so each call looks.
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE list_by_layout(void *self, std::uint64_t a, std::uint64_t b,
                                               std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                               std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                               std::uint64_t i, std::uint64_t j) {
    if (later_queue && Index < 61)
        return 0;
    const auto result = reinterpret_cast<Raw>(list_vtable[later_queue ? Index - 2 : Index])(
        self, a, b, c, d, e, f, g, h, i, j);
    lost_check("list", later_queue ? Index - 2 : Index, _ReturnAddress(), self);
    return result;
}
template <std::size_t... Index>
std::array<Slot, list_slots> layout_list_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, list_slots> vtable{};
    ((vtable[59 + Index] = reinterpret_cast<Slot>(&list_by_layout<59 + Index>)), ...);
    return vtable;
}
std::array<Slot, list_slots> layout_list_vtable =
    layout_list_vtable_of(std::make_index_sequence<list_slots - 59>{});
// BeginRenderPass: the views in its descriptions are console handles
void STDMETHODCALLTYPE begin_render_pass(void *self, UINT count,
                                         const D3D12_RENDER_PASS_RENDER_TARGET_DESC *targets,
                                         const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth,
                                         D3D12_RENDER_PASS_FLAGS flags) {
    ID3D12GraphicsCommandList4 *host = nullptr;
    if (FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&host))))
        return;
    std::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> host_targets(targets,
                                                                   targets + (targets ? count : 0));
    for (auto &target : host_targets)
        target.cpuDescriptor = host_view(target.cpuDescriptor);
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC host_depth{};
    if (depth) {
        host_depth = *depth;
        host_depth.cpuDescriptor = host_view(depth->cpuDescriptor);
    }
    host->BeginRenderPass(count, host_targets.data(), depth ? &host_depth : nullptr, flags);
    host->Release();
}
// DispatchMeshX (group count, flags, indirect arguments, payload): only the plain form, with
// a count and nothing else, has a host method
void STDMETHODCALLTYPE dispatch_mesh_x(void *self, UINT groups, UINT64 flags, UINT64 arguments,
                                       UINT64 payload) {
    if (static_cast<ListProxy *>(self)->no_pipeline)
        return;
    ID3D12GraphicsCommandList6 *host = nullptr;
    if (flags || arguments || payload ||
        FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&host)))) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] DispatchMeshX(%u, flags 0x%llx, arguments %llx, payload "
                         "%llx) has no host form\n",
                         groups, flags, arguments, payload);
        return;
    }
    count_draw(self);
    prepare_draw(static_cast<ListProxy *>(self));
    host->DispatchMesh(groups, 1, 1);
    host->Release();
}
// ExecuteIndirect (59): left out with the rest of what the list draws or computes while its
// pipeline is not there, as the host turns down the list with another pipeline set
void STDMETHODCALLTYPE execute_indirect(void *self, ID3D12CommandSignature *signature, UINT count,
                                        ID3D12Resource *arguments, UINT64 offset,
                                        ID3D12Resource *counts, UINT64 counts_offset) {
    if (static_cast<ListProxy *>(self)->no_pipeline)
        return;
    host_of(self)->ExecuteIndirect(signature, count, arguments, offset, counts, counts_offset);
}
// DispatchMesh (127), the desktop's: what it draws with is made ready as for a draw
void STDMETHODCALLTYPE dispatch_mesh(void *self, UINT x, UINT y, UINT z) {
    if (static_cast<ListProxy *>(self)->no_pipeline)
        return;
    ID3D12GraphicsCommandList6 *host = nullptr;
    if (FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&host))))
        return;
    count_draw(self);
    prepare_draw(static_cast<ListProxy *>(self));
    host->DispatchMesh(x, y, z);
    host->Release();
}
// The ray tracing methods (117 to 121): building acceleration structures, copying them and
// tracing rays. What they are given is GPU addresses of the title's memory (geometry, the
// structures and their scratch), which are none of the host's GPU: done as they come, the
// host's GPU reads where nothing is and the host drops the device, or its driver faults.
// Until those addresses are the host's, nothing is done, and nothing is traced.
void STDMETHODCALLTYPE ray_tracing_method(void *) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] the title traces rays, which is not done here: its "
                             "acceleration structures are not built\n");
        std::fflush(stderr);
    }
}
// The methods of a list that work on memory by its address (the console header's slots 60
// to 63, 69, 70 and 88 to 90). On the console a GPU address is the title's own memory and
// the GPU does the work when it gets there; titles time their frames with the values it
// writes. The host's GPU cannot reach that memory, so the CPU does the work, in the list's
// order, when a queue is given the list (each time it is), not when it is recorded.
bool title_memory(std::uint64_t address, std::uint64_t size, bool write);
void when_run(void *self, std::function<void()> work) {
    {
        const std::lock_guard lock(proxy_mutex);
        if (!list_proxies.contains(self)) {
            work();
            return;
        }
    }
    static_cast<ListProxy *>(self)->on_memory.push_back(std::move(work));
}
template <class Value> void write_value(std::uint64_t destination, Value value) {
    if (title_memory(destination, sizeof value, true))
        std::memcpy(reinterpret_cast<void *>(destination), &value, sizeof value);
}
std::uint64_t host_clock() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}
// D3D12XBOX_WRITE_VALUE_BOP_FLAGS: 1 the GPU's timestamp, 2 the system's
void STDMETHODCALLTYPE write_64_bottom_of_pipe(void *self, std::uint64_t destination,
                                               std::uint64_t value, UINT, UINT flags) {
    when_run(self, [=] { write_value(destination, flags & 3 ? host_clock() : value); });
}
void STDMETHODCALLTYPE write_32_bottom_of_pipe(void *self, std::uint64_t destination, UINT value,
                                               UINT, UINT) {
    when_run(self, [=] { write_value(destination, value); });
}
// D3D12XBOX_WRITE_VALUE_TOP_FLAGS: 4 the GPU's timestamp, 8 the system's
void STDMETHODCALLTYPE write_64_top_of_pipe(void *self, std::uint64_t destination,
                                            std::uint64_t value, UINT flags) {
    when_run(self, [=] { write_value(destination, flags & 12 ? host_clock() : value); });
}
// Write32BitValueTopOfPipeX and Write32BitValueBottomOfShaderX
void STDMETHODCALLTYPE write_32(void *self, std::uint64_t destination, UINT value, UINT) {
    when_run(self, [=] { write_value(destination, value); });
}
void STDMETHODCALLTYPE copy_memory(void *self, std::uint64_t destination, std::uint64_t source,
                                   std::uint64_t size, UINT) {
    when_run(self, [=] {
        if (title_memory(destination, size, true) && title_memory(source, size, false))
            std::memmove(reinterpret_cast<void *>(destination),
                         reinterpret_cast<const void *>(source), static_cast<std::size_t>(size));
    });
}
// FillMemoryWith32BitValueX, 64 and 128: the value over and over, for `size` bytes
void fill_memory(std::uint64_t destination, std::uint64_t size, const void *value,
                 std::size_t width) {
    if (!title_memory(destination, size, true))
        return;
    auto *const bytes = reinterpret_cast<std::uint8_t *>(destination);
    for (std::uint64_t at = 0; at + width <= size; at += width)
        std::memcpy(bytes + at, value, width);
}
void STDMETHODCALLTYPE fill_memory_32(void *self, std::uint64_t destination, std::uint64_t size,
                                      UINT value, UINT) {
    when_run(self, [=] { fill_memory(destination, size, &value, sizeof value); });
}
void STDMETHODCALLTYPE fill_memory_64(void *self, std::uint64_t destination, std::uint64_t size,
                                      std::uint64_t value, UINT) {
    when_run(self, [=] { fill_memory(destination, size, &value, sizeof value); });
}
// the value is read when the list is recorded, as the other methods' values are
void STDMETHODCALLTYPE fill_memory_128(void *self, std::uint64_t destination, std::uint64_t size,
                                       const UINT *value, UINT) {
    if (!value)
        return;
    const std::array<UINT, 4> words{value[0], value[1], value[2], value[3]};
    when_run(self, [=] { fill_memory(destination, size, words.data(), sizeof words); });
}
// ID3D12GraphicsCommandList2::WriteBufferImmediate (Xbox One's list slot 110, the Series
// console's 111). A destination the host's GPU cannot see is the title's own memory, which the
// CPU writes when the list is run, as for the console's write methods above.
void STDMETHODCALLTYPE write_buffer_immediate(
    void *self, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
    const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) {
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> host;
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_MODE> host_modes;
    for (UINT i = 0; parameters && i < count; ++i) {
        if (const auto address = gpu_address(parameters[i].Dest)) {
            host.push_back({address, parameters[i].Value});
            if (modes)
                host_modes.push_back(modes[i]);
        } else {
            when_run(self, [destination = parameters[i].Dest, value = parameters[i].Value] {
                write_value(destination, value);
            });
        }
    }
    ID3D12GraphicsCommandList2 *list = nullptr;
    if (host.empty() || FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    list->WriteBufferImmediate(static_cast<UINT>(host.size()), host.data(),
                               modes ? host_modes.data() : nullptr);
    list->Release();
}
// FlushPipelineX(flags, address, size): on the console the GPU finishes the work before it and
// flushes the caches the flags name, which a title may use where a desktop title has a
// barrier. Here it is a global barrier from all work and its writes to all work after
// (enhanced_barriers), whatever the flags: their values are the console header's own, and a
// flush of everything is never less than the one asked for. A global barrier has only the
// accesses no texture layout stands for (the Enhanced Barriers spec): what a texture's layout
// changes for is the title's own texture barrier.
void STDMETHODCALLTYPE flush_pipeline_x(void *self, UINT, std::uint64_t, std::uint64_t) {
    constexpr auto written = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS |
                             D3D12_BARRIER_ACCESS_STREAM_OUTPUT | D3D12_BARRIER_ACCESS_COPY_DEST;
    constexpr auto read = written | D3D12_BARRIER_ACCESS_VERTEX_BUFFER |
                          D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_INDEX_BUFFER |
                          D3D12_BARRIER_ACCESS_SHADER_RESOURCE |
                          D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT | D3D12_BARRIER_ACCESS_COPY_SOURCE;
    const D3D12_GLOBAL_BARRIER all{D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL, written, read};
    D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_GLOBAL, 1};
    group.pGlobalBarriers = &all;
    enhanced_barriers(self, 1, &group);
}
// Barrier is at 144 in the lists of titles of some kits and at 146 in later ones (a title's
// libraries may be of another kit than the title itself), with console methods of their own
// at the other: the call tells them apart, a barrier being given groups of barriers.
bool barrier_groups(std::uint64_t count, std::uint64_t address) {
    if (!count || count > 256 || !address)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) ||
        info.State != MEM_COMMIT || info.Protect & (PAGE_NOACCESS | PAGE_GUARD) ||
        address + count * sizeof(D3D12_BARRIER_GROUP) >
            reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize)
        return false;
    const auto *const groups = reinterpret_cast<const D3D12_BARRIER_GROUP *>(address);
    return std::all_of(groups, groups + count, [](const D3D12_BARRIER_GROUP &group) {
        return group.Type <= D3D12_BARRIER_TYPE_BUFFER && group.NumBarriers <= 4096 &&
               (!group.NumBarriers || group.pGlobalBarriers);
    });
}
// The console's lists hand out memory for the GPU to read from among their commands (size,
// alignment), at 146 here (148 in a list of the later layout, see list_by_layout), which a
// library of the console's fills: here a ring of memory of the title's kind, gone round only
// long after the GPU has read what was in it.
void *list_memory(std::uint64_t size, std::uint64_t alignment) {
    constexpr std::uint64_t ring_size = 256ull << 20;
    static auto *const ring =
        static_cast<std::uint8_t *>(modules::allocate_aligned(ring_size, 1 << 16, nullptr));
    static std::mutex ring_mutex;
    static std::uint64_t at = 0;
    if (!ring || !size || size > ring_size / 4 || alignment > (1 << 16) ||
        alignment & (alignment - 1))
        return nullptr;
    const std::lock_guard lock(ring_mutex);
    at = (at + std::max<std::uint64_t>(alignment, 1) - 1) &
         ~(std::max<std::uint64_t>(alignment, 1) - 1);
    if (at + size > ring_size)
        at = 0;
    auto *const memory = ring + at;
    at += size;
    return memory;
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE barrier_by_layout(void *self, std::uint64_t count,
                                                  std::uint64_t groups) {
    if (barrier_groups(count, groups)) {
        enhanced_barriers(self, static_cast<UINT32>(count),
                          reinterpret_cast<const D3D12_BARRIER_GROUP *>(groups));
        return 0;
    }
    // memory among the commands
    if (groups && groups <= (1 << 16) && !(groups & (groups - 1)))
        return reinterpret_cast<std::uint64_t>(list_memory(count, groups));
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(
            stderr,
            "[Runtime] unimplemented Direct3D 12 call: command list slot %zu (%llx, %llx), "
            "which is no barrier\n",
            Index, static_cast<unsigned long long>(count), static_cast<unsigned long long>(groups));
        std::fflush(stderr);
    }
    return 0;
}
// Wait32BitValueX and Wait64BitValueX (address, value, ...): on the console the GPU waits until
// the memory at the address compares as asked with the value, which work of another queue
// writes. The host's GPU has no such wait: a submission with one waits instead for the work
// the other queues were given before it (see order_queues).
void STDMETHODCALLTYPE wait_memory(void *self) {
    static_cast<ListProxy *>(self)->waits_memory = true;
}
// the slots where the two consoles' lists differ
void configure_lists() {
    map_list_slots();
    list_vtable[60] = reinterpret_cast<Slot>(&copy_memory);
    list_vtable[61] = reinterpret_cast<Slot>(&fill_memory_32);
    list_vtable[62] = reinterpret_cast<Slot>(&fill_memory_64);
    list_vtable[63] = reinterpret_cast<Slot>(&fill_memory_128);
    list_vtable[69] = reinterpret_cast<Slot>(&write_32_bottom_of_pipe);
    list_vtable[70] = reinterpret_cast<Slot>(&write_64_bottom_of_pipe);
    list_vtable[71] = list_vtable[72] = reinterpret_cast<Slot>(&wait_memory);
    list_vtable[88] = list_vtable[90] = reinterpret_cast<Slot>(&write_32);
    list_vtable[89] = reinterpret_cast<Slot>(&write_64_top_of_pipe);
    const auto ignore = [](std::initializer_list<std::size_t> slots) {
        for (const auto slot : slots)
            list_vtable[slot] = reinterpret_cast<Slot>(&ignored_list_method);
    };
    list_vtable[console == Target::durango ? 110 : 111] =
        reinterpret_cast<Slot>(&write_buffer_immediate);
    if (console == Target::durango) {
        // PrefetchMemoryX and PrefetchPipelineStateX
        ignore({98, 99});
        return;
    }
    list_vtable[83] = reinterpret_cast<Slot>(&copy_texture_region_x);
    // XV_D3D_NO_QUERIES: no queries (BeginQuery, EndQuery, ResolveQueryData), which the
    // debug layer does not get through with some titles
    if (std::getenv("XV_D3D_NO_QUERIES"))
        ignore({28, 29, 30});
    list_vtable[98] = reinterpret_cast<Slot>(&dispatch_mesh_x);
    list_vtable[127] = reinterpret_cast<Slot>(&dispatch_mesh);
    for (std::size_t slot = 117; slot <= 121; ++slot)
        list_vtable[slot] = reinterpret_cast<Slot>(&ray_tracing_method);
    // DriverHintX, the counters, and the binning of primitive batches
    ignore({99, 100, 101, 102, 103, 104});
    list_vtable[113] = reinterpret_cast<Slot>(&begin_render_pass);
    // SetPredicationBufferX: nothing is drawn by a predicate here
    ignore({85});
    // ID3D12GraphicsCommandList7::Barrier, in the lists of a title built with a later kit
    list_vtable[144] = reinterpret_cast<Slot>(&barrier_by_layout<144>);
    list_vtable[146] = reinterpret_cast<Slot>(&barrier_by_layout<146>);
    list_vtable[59] = reinterpret_cast<Slot>(&execute_indirect);
}
// XV_D3D_LOST: every method of a list looks whether the host still has the device
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE checked_list_method(void *self, std::uint64_t a, std::uint64_t b,
                                                    std::uint64_t c, std::uint64_t d,
                                                    std::uint64_t e, std::uint64_t f,
                                                    std::uint64_t g, std::uint64_t h,
                                                    std::uint64_t i, std::uint64_t j) {
    // XV_D3D_SKIP=<slot>[,<slot>...]: those methods of a list do nothing, to tell which of
    // them the host drops the device for
    static const bool skipped = [] {
        const char *text = std::getenv("XV_D3D_SKIP");
        for (char *end = nullptr; text && *text; text = *end ? end + 1 : end) {
            const auto slot = std::strtoull(text, &end, 10);
            if (end == text)
                break;
            if (slot == Index)
                return true;
        }
        return false;
    }();
    if (skipped)
        return 0;
    const auto result =
        reinterpret_cast<Raw>(layout_list_vtable[Index])(self, a, b, c, d, e, f, g, h, i, j);
    lost_check("list method", Index, _ReturnAddress(), self);
    return result;
}
template <std::size_t... Index>
std::array<Slot, list_slots> checked_list_vtable_of(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&checked_list_method<Index>)...};
}
std::array<Slot, list_slots> checked_list_vtable =
    checked_list_vtable_of(std::make_index_sequence<list_slots>{});
void *make_list_proxy(List *host) {
    auto *const proxy = new ListProxy{};
    // the slots both layouts share, as they are now
    static const bool filled = [] {
        std::copy_n(list_vtable.begin(), 59, layout_list_vtable.begin());
        return true;
    }();
    static_cast<void>(filled);
    proxy->vtable = console == Target::scarlett ? layout_list_vtable.data() : list_vtable.data();
    if (console == Target::scarlett && std::getenv("XV_D3D_LOST")) {
        for (std::size_t slot = 0; slot < 3; ++slot)
            checked_list_vtable[slot] = layout_list_vtable[slot];
        proxy->vtable = checked_list_vtable.data();
    }
    proxy->host = host;
    proxy->references = 1;
    const std::lock_guard lock(proxy_mutex);
    list_proxies.insert(proxy);
    return proxy;
}

// ID3D12XboxDmaCommandList (a list of type 16): the console's hardware copies, fills and
// (de)compresses memory by itself, in the order of a queue. The slots are the ones of the
// console header's table. Resources are copied on a host list of ours; what works on
// memory by address is done by the CPU when the list is run (when_run), since on the console
// a GPU address is the title's own memory. The compression formats are not known: those say
// so.
constexpr UINT dma_list_type = 16;
constexpr std::size_t dma_slots = 32;
struct DmaProxy : ListProxy {
    ID3D12CommandAllocator *allocator = nullptr;
};
bool title_memory(std::uint64_t address, std::uint64_t size, bool write) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) ||
        info.State != MEM_COMMIT)
        return false;
    const auto end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY;
    const DWORD readable = writable | PAGE_READONLY | PAGE_EXECUTE_READ;
    return address + size <= end && (info.Protect & (write ? writable : readable));
}
template <std::size_t Index>
HRESULT STDMETHODCALLTYPE unknown_dma_method(void *, std::uint64_t a, std::uint64_t b) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: DMA list slot %zu (%llx, %llx) "
                     "from %s\n",
                     Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE dma_ok(void *) { return S_OK; }
UINT STDMETHODCALLTYPE dma_type(void *) { return dma_list_type; }
HRESULT STDMETHODCALLTYPE dma_device(void *self, REFIID iid, void **device) {
    return host_of(self)->GetDevice(iid, device);
}
HRESULT STDMETHODCALLTYPE dma_reset(void *self, ID3D12CommandAllocator *, ID3D12PipelineState *) {
    auto *const list = static_cast<DmaProxy *>(self);
    list->on_memory.clear();
    return list->host->Reset(list->allocator, nullptr);
}
void STDMETHODCALLTYPE dma_copy_buffer(void *self, ID3D12Resource *target, UINT64 target_offset,
                                       ID3D12Resource *source, UINT64 source_offset, UINT64 bytes) {
    host_of(self)->CopyBufferRegion(target, target_offset, source, source_offset, bytes);
}
void STDMETHODCALLTYPE dma_copy_texture(void *self, const D3D12_TEXTURE_COPY_LOCATION *target,
                                        UINT x, UINT y, UINT z,
                                        const D3D12_TEXTURE_COPY_LOCATION *source,
                                        const D3D12_BOX *box) {
    host_of(self)->CopyTextureRegion(target, x, y, z, source, box);
}
void STDMETHODCALLTYPE dma_copy_resource(void *self, ID3D12Resource *target,
                                         ID3D12Resource *source) {
    host_of(self)->CopyResource(target, source);
}
// the copy engine's memory methods are the list's: done when the list is run
void STDMETHODCALLTYPE dma_last_error(void *self, std::uint64_t target) {
    write_32(self, target, 0, 0);
}
void STDMETHODCALLTYPE dma_nothing(void *) {}
const std::array<Slot, dma_slots> dma_vtable = [] {
    std::array<Slot, dma_slots> vtable{};
    const auto fill = [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((vtable[Index] = reinterpret_cast<Slot>(&unknown_dma_method<Index>)), ...);
    };
    fill(std::make_index_sequence<dma_slots>{});
    const auto set = [&](std::size_t slot, auto *function) {
        vtable[slot] = reinterpret_cast<Slot>(function);
    };
    set(0, &list_query);
    set(1, &list_add_ref);
    set(2, &list_release);
    for (const std::size_t named : {3, 4, 5, 6}) // private data and the name
        set(named, &dma_ok);
    set(7, &dma_device);
    set(8, &dma_type);
    set(9, &close_list);
    set(10, &dma_reset);
    set(11, &dma_copy_buffer);
    set(12, &copy_texture_region);
    set(13, &dma_copy_resource);
    set(15, &resource_barrier);
    // queries and markers count nothing here; a wait is over when the list is run, as the
    // value it waits for was written by then (when_run); packets for the hardware and the
    // kickoff have nothing to go to
    for (const std::size_t ignored : {16, 17, 18, 19, 20, 21, 25, 30, 31})
        set(ignored, &dma_nothing);
    set(22, &copy_memory);
    set(23, &fill_memory_32);
    set(24, &write_64_bottom_of_pipe);
    set(26, &dma_last_error);
    return vtable;
}();
HRESULT make_dma_list(ID3D12Device *device, void **list) {
    auto *const proxy = new DmaProxy{};
    auto result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&proxy->allocator));
    if (SUCCEEDED(result))
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, proxy->allocator,
                                           nullptr, IID_PPV_ARGS(&proxy->host));
    if (FAILED(result)) {
        delete proxy;
        return result;
    }
    proxy->vtable = const_cast<Slot *>(dma_vtable.data());
    proxy->references = 1;
    const std::lock_guard lock(proxy_mutex);
    list_proxies.insert(proxy);
    *list = proxy;
    return S_OK;
}

// slot 73: a command list from a description (Unreal's call site: the type, 0, the node mask,
// 0), an allocator, a pipeline state, then the interface and the list
HRESULT STDMETHODCALLTYPE create_command_list(ID3D12Device *self, const UINT *desc,
                                              ID3D12CommandAllocator *allocator,
                                              ID3D12PipelineState *state, REFIID, void **list) {
    if (!desc || !list)
        return E_INVALIDARG;
    if (desc[0] == dma_list_type)
        return make_dma_list(self, list);
    List *host = nullptr;
    const auto result =
        self->CreateCommandList(desc[2], static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]), allocator,
                                host_pipeline(state), IID_PPV_ARGS(&host));
    *list = SUCCEEDED(result) ? make_list_proxy(host) : nullptr;
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommandListX type %u flags 0x%x nodes 0x%x allocator %p: "
                     "0x%08lx\n",
                     desc[0], desc[1], desc[2], static_cast<void *>(allocator),
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// The title's texels in a format of the console's own whose stand-in lays them out otherwise
// (see host_format), as the host's: a row of `width` of them. False for a format there is no
// such way for.
float small_float(std::uint32_t bits, int mantissa, int bias) {
    // no sign, no infinity and no NaN; an exponent of 0 is a denormal
    const auto fraction =
        static_cast<float>(bits & ((1u << mantissa) - 1)) / static_cast<float>(1u << mantissa);
    const auto exponent = static_cast<int>(bits >> mantissa);
    return exponent ? std::ldexp(1.0f + fraction, exponent - bias) : std::ldexp(fraction, 1 - bias);
}
std::uint16_t half(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    const std::uint32_t sign = bits >> 16 & 0x8000;
    const int exponent = static_cast<int>(bits >> 23 & 0xff) - 112;
    // nothing these formats hold is below the smallest normal half but 0
    if (exponent <= 0)
        return static_cast<std::uint16_t>(sign);
    const std::uint32_t rounded =
        (static_cast<std::uint32_t>(exponent) << 10 | (bits >> 13 & 0x3ff)) + (bits >> 12 & 1);
    return static_cast<std::uint16_t>(sign | std::min<std::uint32_t>(rounded, 0x7bff));
}
bool convert_row(DXGI_FORMAT format, const std::uint8_t *source, std::uint8_t *target, UINT width) {
    std::uint32_t texel = 0;
    switch (static_cast<int>(format)) {
    case XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT:
    case XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT: {
        // 7 bits of mantissa and 3 of exponent, or 6 and 4
        const int mantissa = format == XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT ? 7 : 6;
        const int bias = format == XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT ? 3 : 7;
        for (UINT x = 0; x < width; ++x) {
            std::memcpy(&texel, source + x * 4, 4);
            const std::uint16_t texels[]{half(small_float(texel & 0x3ff, mantissa, bias)),
                                         half(small_float(texel >> 10 & 0x3ff, mantissa, bias)),
                                         half(small_float(texel >> 20 & 0x3ff, mantissa, bias)),
                                         half(static_cast<float>(texel >> 30) / 3)};
            std::memcpy(target + x * 8, texels, 8);
        }
        return true;
    }
    case XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM:
        for (UINT x = 0; x < width; ++x) {
            std::memcpy(&texel, source + x * 4, 4);
            const auto channel = [&](int shift) {
                const auto value = static_cast<std::int32_t>(texel << (22 - shift)) >> 22;
                return static_cast<std::int16_t>(
                    std::lround(std::max(static_cast<float>(value) / 511, -1.0f) * 32767));
            };
            const std::int16_t texels[]{channel(0), channel(10), channel(20),
                                        static_cast<std::int16_t>(std::lround(
                                            static_cast<float>(texel >> 30) / 3 * 32767))};
            std::memcpy(target + x * 8, texels, 8);
        }
        return true;
    case XBOX_DXGI_FORMAT_R4G4_UNORM:
        for (UINT x = 0; x < width; ++x) {
            target[x * 2] = static_cast<std::uint8_t>((source[x] & 15) * 17);
            target[x * 2 + 1] = static_cast<std::uint8_t>((source[x] >> 4) * 17);
        }
        return true;
    default:
        return false;
    }
}
// The uploads of placement textures the title wrote, on a list of ours that runs before the
// title's on the same queue. Textures are where the title keeps them between its own lists:
// shader resources.
struct Uploader {
    // the lists take turns, so that a submission waits for the uploads of one some
    // submissions back, which the GPU is past, and not for the last ones
    struct Turn {
        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        UINT64 value = 0; // the fence's once the GPU is past this turn's uploads
        // the texels, in a buffer kept mapped and grown as needed
        ID3D12Resource *staging = nullptr;
        UINT64 size = 0;
        std::uint8_t *mapped = nullptr;
        std::vector<ID3D12Resource *> textures; // until the GPU is past them
    };
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
    std::array<Turn, 4> turns;
    std::size_t next = 0;
};
std::mutex uploader_mutex;
void upload_placements_d3d12(ID3D12CommandQueue *queue) {
    // shader resource states are a direct queue's: the uploads wait for one
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    auto uploads = take_d3d12_uploads();
    if (uploads.empty())
        return;
    // XV_D3D_NO_UPLOADS: what the title wrote into its textures is dropped, to tell whether
    // the host loses the device over it
    if (std::getenv("XV_D3D_NO_UPLOADS")) {
        for (auto &upload : uploads)
            upload.resource->Release();
        return;
    }
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_TRACE") && shown++ < 5) {
        std::fprintf(stderr,
                     "[Runtime] uploading %zu placement textures, the first with %zu "
                     "subresources\n",
                     uploads.size(), uploads[0].subresources.size());
        std::fflush(stderr);
    }
    const std::lock_guard lock(uploader_mutex);
    static Uploader uploader;
    ID3D12Device *device = nullptr;
    queue->GetDevice(IID_PPV_ARGS(&device));
    const auto give_up = [&] {
        for (auto &upload : uploads)
            upload.resource->Release();
        device->Release();
    };
    if (!uploader.fence &&
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploader.fence))))
        return give_up();
    auto &turn = uploader.turns[uploader.next++ % uploader.turns.size()];
    if (!turn.list &&
        (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&turn.allocator))) ||
         FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, turn.allocator,
                                          nullptr, IID_PPV_ARGS(&turn.list)))))
        return give_up();
    if (turn.value) {
        if (uploader.fence->GetCompletedValue() < turn.value) {
            auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            uploader.fence->SetEventOnCompletion(turn.value, event);
            const auto waited = WaitForSingleObject(event, 5000);
            CloseHandle(event);
            if (waited != WAIT_OBJECT_0) {
                std::fprintf(stderr,
                             "[Runtime] the uploads before these are not done after 5 s (the "
                             "queue is at %llu of %llu, device 0x%08lx)%c",
                             static_cast<unsigned long long>(uploader.fence->GetCompletedValue()),
                             static_cast<unsigned long long>(turn.value),
                             static_cast<unsigned long>(device->GetDeviceRemovedReason()), 10);
                std::fflush(stderr);
            }
        }
        for (auto *texture : turn.textures)
            texture->Release();
        turn.textures.clear();
        turn.allocator->Reset();
        turn.list->Reset(turn.allocator, nullptr);
    }
    // the staging buffer is grown to hold every texture's texels
    const auto convertible = [](const PlacementUpload &upload, const D3D12_RESOURCE_DESC &desc) {
        return upload.format == desc.Format || convert_row(upload.format, nullptr, nullptr, 0);
    };
    const auto aligned = [](UINT64 bytes) {
        return (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
               ~UINT64{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1};
    };
    UINT64 total = 0;
    for (auto &upload : uploads) {
        const auto desc = upload.resource->GetDesc();
        UINT64 bytes = 0;
        if (convertible(upload, desc))
            device->GetCopyableFootprints(&desc, 0, static_cast<UINT>(upload.subresources.size()),
                                          0, nullptr, nullptr, nullptr, &bytes);
        total += aligned(bytes);
    }
    if (total > turn.size) {
        if (turn.staging)
            turn.staging->Release();
        turn.staging = nullptr;
        turn.size = std::bit_ceil(std::max<UINT64>(total, 16 << 20));
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                              0,
                                              turn.size,
                                              1,
                                              1,
                                              1,
                                              DXGI_FORMAT_UNKNOWN,
                                              {1, 0},
                                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                              D3D12_RESOURCE_FLAG_NONE};
        void *mapped = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&turn.staging))) ||
            FAILED(turn.staging->Map(0, nullptr, &mapped))) {
            if (turn.staging)
                turn.staging->Release();
            turn.staging = nullptr;
            turn.size = 0;
            // closed, and reset when its turn comes again
            turn.list->Close();
            queue->Signal(uploader.fence, ++uploader.value);
            turn.value = uploader.value;
            return give_up();
        }
        turn.mapped = static_cast<std::uint8_t *>(mapped);
    }
    std::string what; // of each texture, should the host turn the list down
    UINT64 offset = 0;
    for (auto &upload : uploads) {
        const auto desc = upload.resource->GetDesc();
        // texels in a format of the console's own whose stand-in has another layout are
        // converted; where there is no way to, the host's texture keeps what it has
        if (!convertible(upload, desc)) {
            upload.resource->Release();
            continue;
        }
        const auto count = static_cast<UINT>(upload.subresources.size());
        back_texture(queue, device, upload.resource);
        what += " [dimension " + std::to_string(desc.Dimension) + " " + std::to_string(desc.Width) +
                "x" + std::to_string(desc.Height) + "x" + std::to_string(desc.DepthOrArraySize) +
                " mips " + std::to_string(desc.MipLevels) + " format " +
                std::to_string(desc.Format) + " samples " + std::to_string(desc.SampleDesc.Count) +
                " flags " + std::to_string(desc.Flags) + ", " + std::to_string(count) +
                " subresources]";
        if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 40) {
            UINT64 total = 0;
            device->GetCopyableFootprints(&desc, 0, count, 0, nullptr, nullptr, nullptr, &total);
            std::fprintf(stderr, "[Lost] an upload of%s: %llu bytes%c",
                         what.substr(what.rfind('[') - 1).c_str(),
                         static_cast<unsigned long long>(total), 10);
            std::fflush(stderr);
        }
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, count, offset, footprints.data(), rows.data(),
                                      nullptr, &bytes);
        offset += aligned(bytes);
        auto *const buffer = turn.staging;
        auto *const mapped = turn.mapped;
        {
            for (UINT i = 0; i < count; ++i) {
                const auto &source = upload.subresources[i];
                const auto pitch = upload.pitches[i];
                // each slice of a volume after the one before it, in both
                const auto plane = std::size_t{pitch} * rows[i];
                for (UINT slice = 0; slice < footprints[i].Footprint.Depth; ++slice)
                    for (UINT line = 0;
                         line < rows[i] &&
                         slice * plane + std::size_t{line + 1} * pitch <= source.size();
                         ++line) {
                        auto *const to = mapped + footprints[i].Offset +
                                         (std::size_t{slice} * rows[i] + line) *
                                             footprints[i].Footprint.RowPitch;
                        const auto *const from = reinterpret_cast<const std::uint8_t *>(
                            source.data() + slice * plane + std::size_t{line} * pitch);
                        if (upload.format != desc.Format)
                            convert_row(upload.format, from, to, footprints[i].Footprint.Width);
                        else
                            std::memcpy(to, from, pitch);
                    }
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Transition = {upload.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_DEST};
            turn.list->ResourceBarrier(1, &barrier);
            for (UINT i = 0; i < count; ++i) {
                const D3D12_TEXTURE_COPY_LOCATION target{upload.resource,
                                                         D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                         {.SubresourceIndex = i}};
                const D3D12_TEXTURE_COPY_LOCATION source{buffer,
                                                         D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                         {.PlacedFootprint = footprints[i]}};
                turn.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            turn.list->ResourceBarrier(1, &barrier);
            // the title may let go of the texture before the GPU copies into it
            turn.textures.push_back(upload.resource);
        }
    }
    // a list the host turned down is not executed: the host drops the device for that
    if (const auto closed = turn.list->Close(); FAILED(closed)) {
        std::fprintf(stderr,
                     "[Runtime] the upload of %zu textures the title wrote is turned down "
                     "(0x%08lx) and what it wrote is lost:%s%c",
                     uploads.size(), static_cast<unsigned long>(closed), what.c_str(), 10);
        report_messages();
    } else {
        const bool had = SUCCEEDED(device->GetDeviceRemovedReason());
        ID3D12CommandList *const lists[] = {turn.list};
        queue->ExecuteCommandLists(1, lists);
        if (const auto reason = device->GetDeviceRemovedReason(); had && FAILED(reason)) {
            std::fprintf(stderr,
                         "[Runtime] the host drops the device (0x%08lx) at the upload of %zu "
                         "textures the title wrote:%s%c",
                         static_cast<unsigned long>(reason), uploads.size(), what.c_str(), 10);
            std::fflush(stderr);
        }
    }
    queue->Signal(uploader.fence, ++uploader.value);
    turn.value = uploader.value;
    device->Release();
}
// The order of the queues' work for the console's waits on memory (wait_memory). Each queue
// signals a fence of ours after each submission. A submission with a list that waits on
// memory first waits, on the GPU, for each other queue's work up to its last submission, or
// up to the first wait of its own (Wait, or one made here) that is not over yet: work after
// such a wait may itself wait for this queue, and the two would wait for each other forever.
struct QueueOrder {
    ID3D12Fence *fence = nullptr;
    UINT64 submitted = 0; // the fence's value after the last submission
    struct Pending {
        UINT64 after; // the submissions before it
        ID3D12Fence *fence;
        UINT64 value;
    };
    std::deque<Pending> waits;
};
std::mutex order_mutex;
std::map<ID3D12CommandQueue *, QueueOrder> queue_orders;
// how far the queue's work goes with nothing left to wait for (order_mutex held)
UINT64 free_until(QueueOrder &order) {
    while (!order.waits.empty() &&
           order.waits.front().fence->GetCompletedValue() >= order.waits.front().value) {
        order.waits.front().fence->Release();
        order.waits.pop_front();
    }
    return order.waits.empty() ? order.submitted : order.waits.front().after;
}
void note_wait(QueueOrder &order, ID3D12Fence *fence, UINT64 value) {
    if (fence->GetCompletedValue() >= value)
        return;
    fence->AddRef();
    order.waits.push_back({order.submitted, fence, value});
}
void note_queue_wait(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value) {
    if (!fence)
        return;
    const std::lock_guard lock(order_mutex);
    note_wait(queue_orders[queue], fence, value);
}
void forget_queue(ID3D12CommandQueue *queue) {
    const std::lock_guard lock(order_mutex);
    const auto found = queue_orders.find(queue);
    if (found == queue_orders.end())
        return;
    for (const auto &pending : found->second.waits)
        pending.fence->Release();
    if (found->second.fence)
        found->second.fence->Release();
    queue_orders.erase(found);
}
// before the queue is given the lists: the other queues' work first, if one waits on memory
void order_before(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
    bool waits = false;
    {
        const std::lock_guard lock(proxy_mutex);
        for (UINT i = 0; i < count && !waits; ++i)
            waits =
                list_proxies.contains(lists[i]) &&
                static_cast<const ListProxy *>(static_cast<const void *>(lists[i]))->waits_memory;
    }
    if (!waits)
        return;
    const std::lock_guard lock(order_mutex);
    auto &own = queue_orders[queue];
    for (auto &[other, order] : queue_orders) {
        if (other == queue || !order.fence)
            continue;
        if (const auto until = free_until(order);
            until && SUCCEEDED(queue->Wait(order.fence, until)))
            note_wait(own, order.fence, until);
    }
}
// after: the queue's fence of ours moves on
void order_after(ID3D12CommandQueue *queue) {
    const std::lock_guard lock(order_mutex);
    auto &order = queue_orders[queue];
    if (!order.fence) {
        ID3D12Device *device = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
            return;
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&order.fence));
        device->Release();
        if (!order.fence)
            return;
    }
    if (SUCCEEDED(queue->Signal(order.fence, order.submitted + 1)))
        ++order.submitted;
}
// ID3D12CommandQueue::ExecuteCommandLists: the host's lists for the title's proxies
void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue *self, UINT count,
                                             ID3D12CommandList *const *lists) {
    if (const auto *text = std::getenv("XV_D3D_SLOW"))
        Sleep(std::atoi(text));
    decode_written_heaps();
    upload_scratch(self);
    copy_twins();
    upload_placements_d3d12(self);
    std::vector<ID3D12CommandList *> host(count);
    for (UINT i = 0; i < count; ++i)
        host[i] = host_list(lists[i]);
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 80) {
        std::fprintf(
            stderr, "[Lost] thread %lu queue %p (type %d) executes %u lists:", GetCurrentThreadId(),
            static_cast<void *>(self), static_cast<int>(self->GetDesc().Type), count);
        for (UINT i = 0; i < count; ++i)
            std::fprintf(stderr, " %p (type %d)", static_cast<void *>(lists[i]),
                         host[i] ? static_cast<int>(host[i]->GetType()) : -1);
        std::fprintf(stderr, "%c", 10);
        std::fflush(stderr);
    }
    order_before(self, count, lists);
    // the lists' work on memory, in their order (when_run)
    for (UINT i = 0; i < count; ++i) {
        {
            const std::lock_guard lock(proxy_mutex);
            if (!list_proxies.contains(lists[i]))
                continue;
        }
        for (const auto &work : static_cast<ListProxy *>(static_cast<void *>(lists[i]))->on_memory)
            work();
    }
    {
        const std::lock_guard lock(proxy_mutex);
        for (UINT i = 0; i < count; ++i)
            if (list_proxies.contains(lists[i]) &&
                static_cast<ListProxy *>(static_cast<void *>(lists[i]))->broken)
                host[i] = nullptr;
        std::erase(host, nullptr);
    }
    if (!host.empty())
        self->ExecuteCommandLists(static_cast<UINT>(host.size()), host.data());
    order_after(self);
    // the submission the host drops the device at
    if (static std::atomic<bool> told{false}; !told.load())
        if (auto *const device = d3d12_device.load();
            device && FAILED(device->GetDeviceRemovedReason()) && !told.exchange(true)) {
            std::fprintf(stderr,
                         "[Runtime] the host drops the device at a submission of %u lists:", count);
            for (UINT i = 0; i < count; ++i)
                std::fprintf(stderr, " %p (type %d)", static_cast<void *>(lists[i]),
                             host[i] ? static_cast<int>(host[i]->GetType()) : -1);
            std::fprintf(stderr, "\n");
            report_failure();
        }
    // XV_D3D_SYNC: the GPU done with this work before the title goes on, to tell its waits
    // from ours
    if (std::getenv("XV_D3D_SYNC")) {
        static ID3D12Fence *fence = nullptr;
        static UINT64 value = 0;
        if (!fence) {
            ID3D12Device *device = nullptr;
            self->GetDevice(IID_PPV_ARGS(&device));
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            device->Release();
        }
        self->Signal(fence, ++value);
        auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
    }
}
// ID3D12CommandAllocator as the console has it: the desktop's methods to Reset (8), then
// SetAllocatorCallbackX (9) and two more of its own. The host's class has methods of its own
// at those slots, which a title's call must not reach: the debug layer's at 9 takes the
// title's arguments for data of its own.
// SetAllocatorCallbackX(callback, context): the console's runtime asks the title's callback for
// the memory the allocator's commands go in. The host's allocator keeps its commands in memory
// of its own, which is all the host's GPU reads, so the callback is never called.
HRESULT STDMETHODCALLTYPE set_allocator_callback_x(void *, void *, void *) { return S_OK; }
// the other two: no title has been seen calling them, and nothing here needs what they set
HRESULT STDMETHODCALLTYPE ignored_allocator_method(void *) { return S_OK; }
// CreateCommandAllocator (9): the console's copy engine has a list type of its own (16),
// which is a direct list here, as its queue is
HRESULT STDMETHODCALLTYPE create_command_allocator(ID3D12Device *self, UINT type, REFIID iid,
                                                   void **allocator) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, REFIID, void **);
    const auto result = original<Create>(self, 9)(
        self, type == 16 ? D3D12_COMMAND_LIST_TYPE_DIRECT : type, iid, allocator);
    if (SUCCEEDED(result) && allocator && *allocator)
        console_command_allocator(*allocator);
    return result;
}
// ID3D12Fence as the console has it: the desktop's methods to GetCreationFlags (11), then
// GetGpuAddressX (12), where the fence's completed value is in memory. The console's GPU writes
// it there and its CPU reads it at the same address; titles wait on it with MONITORX and
// MWAITX. Here the title reads a copy, which follows the host's fence: whatever moves the
// fence, the CPU or a queue, sets an event (SetEventOnCompletion) for a thread of ours, which
// copies the new value.
struct alignas(64) CompletedValue { // a cache line of its own, which the title watches
    std::atomic<std::uint64_t> value{0};
};
struct FollowedFence {
    std::unique_ptr<CompletedValue> completed = std::make_unique<CompletedValue>();
    std::uint64_t armed = 0; // the value the event is set for
};
std::mutex fence_mutex;
// each with a reference of ours, which goes when nothing else has one
std::map<ID3D12Fence *, FollowedFence> followed_fences;
HANDLE fence_moved = nullptr;
// the fence's completed value to its copy, and the event for the next one
void copy_completed_value(ID3D12Fence *fence, FollowedFence &followed) {
    const auto completed = fence->GetCompletedValue();
    // UINT64_MAX: the device was removed, and the value stays. A title that waits on the
    // fence's memory sees it before it submits again, and may stop then: this tells of it
    // before the title has the value.
    if (static std::atomic<bool> told{false}; completed == UINT64_MAX && !told.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a fence the title watches reads as the device removed\n");
        report_failure();
    }
    followed.completed->value.store(completed, std::memory_order_release);
    if (fence_moved && completed != UINT64_MAX && followed.armed != completed + 1 &&
        SUCCEEDED(fence->SetEventOnCompletion(completed + 1, fence_moved)))
        followed.armed = completed + 1;
}
void follow_fences() {
    for (;;) {
        WaitForSingleObject(fence_moved, INFINITE);
        std::vector<ID3D12Fence *> unused;
        {
            const std::lock_guard lock(fence_mutex);
            for (auto at = followed_fences.begin(); at != followed_fences.end();) {
                // ours is the only reference left: the title's last went through the host
                at->first->AddRef();
                if (at->first->Release() == 1) {
                    unused.push_back(at->first);
                    at = followed_fences.erase(at);
                    continue;
                }
                copy_completed_value(at->first, at->second);
                ++at;
            }
        }
        for (auto *const fence : unused)
            fence->Release();
    }
}
D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE fence_gpu_address(ID3D12Fence *self) {
    static std::once_flag started;
    std::call_once(started, [] {
        fence_moved = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_moved)
            std::thread(follow_fences).detach();
    });
    const std::lock_guard lock(fence_mutex);
    const auto [at, added] = followed_fences.try_emplace(self);
    if (added)
        self->AddRef();
    copy_completed_value(self, at->second);
    return reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(&at->second.completed->value);
}
// Signal (10) from the title: the copy has the value at once, as the fence's memory does
HRESULT STDMETHODCALLTYPE signal_fence(ID3D12Fence *self, UINT64 value) {
    const auto result =
        original<HRESULT(STDMETHODCALLTYPE *)(void *, UINT64)>(self, 10)(self, value);
    const std::lock_guard lock(fence_mutex);
    if (const auto at = followed_fences.find(self); at != followed_fences.end())
        copy_completed_value(self, at->second);
    return result;
}
// Release (2) from the title: our reference goes with its last
ULONG STDMETHODCALLTYPE release_fence(ID3D12Fence *self) {
    using Release = ULONG(STDMETHODCALLTYPE *)(void *);
    // The follower counts the references under the same lock: in between the two releases it
    // would see ours alone, let it go itself, and the title would be told 1 for its last.
    const std::lock_guard lock(fence_mutex);
    const auto left = original<Release>(self, 2)(self);
    if (left != 1 || !followed_fences.erase(self))
        return left;
    return original<Release>(self, 2)(self);
}
// CreateFence (36): the fence with the console's methods
HRESULT STDMETHODCALLTYPE create_fence(ID3D12Device *self, UINT64 initial, D3D12_FENCE_FLAGS flags,
                                       REFIID iid, void **fence) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, UINT64, D3D12_FENCE_FLAGS, REFIID, void **);
    const auto result = original<Create>(self, 36)(self, initial, flags, iid, fence);
    if (SUCCEEDED(result) && fence && *fence)
        console_fence(*fence);
    return result;
}
// CreateCommandList (12), the desktop's way: a proxy as well
HRESULT STDMETHODCALLTYPE create_command_list_desktop(ID3D12Device *self, UINT node,
                                                      D3D12_COMMAND_LIST_TYPE type,
                                                      ID3D12CommandAllocator *allocator,
                                                      ID3D12PipelineState *state, REFIID,
                                                      void **list) {
    if (!list)
        return E_POINTER;
    List *host = nullptr;
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, D3D12_COMMAND_LIST_TYPE,
                                                ID3D12CommandAllocator *, ID3D12PipelineState *,
                                                REFIID, void **);
    const auto result = original<Create>(self, 12)(self, node, type, allocator,
                                                   host_pipeline(state), IID_PPV_ARGS(&host));
    *list = SUCCEEDED(result) ? make_list_proxy(host) : nullptr;
    return result;
}
// The console's ID3D12Device goes on past the desktop's 44 methods with its own. Those not
// known yet say which they are and fail.
template <std::size_t Index>
HRESULT STDMETHODCALLTYPE unknown_device_method(void *, void *first, void *second) {
    std::fprintf(stderr,
                 "[Runtime] unimplemented Direct3D 12 call: device slot %zu (%p, %p) from %s\n",
                 Index, first, second, detail::guest_location(_ReturnAddress()).c_str());
    std::fflush(stderr);
    return E_NOTIMPL;
}
// CreatePlacedResource (29) and CreateReservedResource (30): the console's states, flags and
// formats
D3D12_RESOURCE_DESC host_resource_desc(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc) {
    auto host = desc;
    host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & 0xff);
    host.Format = host_format(host.Format, true);
    if (host.Flags &
        (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        host.Format = target_format(host.Format);
    if (host.Layout >= 0x100)
        host.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    host_resource_format(device, host);
    return host;
}
HRESULT STDMETHODCALLTYPE create_placed_resource(ID3D12Device *self, ID3D12Heap *heap,
                                                 UINT64 offset, const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES state,
                                                 const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                 void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, ID3D12Heap *, UINT64,
                                                const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                                const D3D12_CLEAR_VALUE *, REFIID, void **);
    D3D12_RESOURCE_DESC host{};
    if (desc)
        host = host_resource_desc(self, *desc);
    D3D12_CLEAR_VALUE cleared{}, cleared_host{};
    clear = host_clear_value(self, host_clear(clear, cleared), host.Flags, cleared_host);
    const auto result = original<Create>(self, 29)(self, heap, offset, desc ? &host : nullptr,
                                                   host_state(state), clear, iid, resource);
    if (SUCCEEDED(result) && resource && *resource)
        track_buffer(static_cast<ID3D12Resource *>(*resource));
    return result;
}
HRESULT STDMETHODCALLTYPE create_reserved_resource(ID3D12Device *self,
                                                   const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                   void **resource) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                     const D3D12_CLEAR_VALUE *, REFIID, void **);
    D3D12_RESOURCE_DESC host{};
    if (desc)
        host = host_resource_desc(self, *desc);
    D3D12_CLEAR_VALUE cleared_host{};
    clear = host_clear_value(self, clear, host.Flags, cleared_host);
    const auto result = original<Create>(self, 30)(self, desc ? &host : nullptr, host_state(state),
                                                   clear, iid, resource);
    if (SUCCEEDED(result) && resource && *resource)
        track_buffer(static_cast<ID3D12Resource *>(*resource));
    return result;
}
// slot 60: CreateGraphicsPipelineStateX, the pipeline and the console's extensions of it
HRESULT STDMETHODCALLTYPE create_graphics_pipeline_x(ID3D12Device *self,
                                                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                     UINT, const void *, REFIID iid, void **state) {
    return deferred_graphics_pipeline(self, desc, iid, state);
}
// what only the console's driver and debugger have a use for: driver hints, hang and debug
// callbacks, debug flags, memory priority, UnregisterPagePoolX
HRESULT STDMETHODCALLTYPE ignored_device_method(void *) { return S_OK; }
// Deserialize{Graphics,Compute}PipelineStateX (device slots 57 and 67): a pipeline from the
// console driver's own serialised form, which a title ships a cache of. What is in it (the
// state, the input layout, each stage's program and its records) is not read yet: the title
// gets a pipeline with its root signature that draws or computes nothing, since it keeps no
// other way to make these and reads through the ones it did not get.
// XV_SHADER_CODE: what the first ones are given, to study (pso_<kind>_<n>.bin: the state's
// first 0x100 bytes, then each thing a pointer in it leads to).
template <int Kind>
HRESULT STDMETHODCALLTYPE deserialize_pipeline(ID3D12Device *self, const std::uint64_t *state,
                                               REFIID iid, void **pipeline) {
    if (pipeline)
        *pipeline = nullptr;
    static std::atomic<int> seen{0};
    const int index = seen++;
    // A Series title is told the cache is not there, as on a console that has none: it then
    // makes its pipelines from the shaders it ships, which are translated, where the cache's
    // would draw nothing. XV_PSO_CACHE takes the cache's all the same, XV_PSO_NOTIMPL refuses
    // them for an Xbox One title too.
    if (static const bool refused = std::getenv("XV_PSO_NOTIMPL") != nullptr ||
                                    (console == Target::scarlett && !std::getenv("XV_PSO_CACHE"));
        refused)
        return E_NOTIMPL;
    const auto readable = [](std::uint64_t address, std::size_t bytes) {
        MEMORY_BASIC_INFORMATION info{};
        return address > 0x10000 &&
               VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
               info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
               address + bytes <=
                   reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    };
    if (index == 0) {
        std::fprintf(stderr,
                     "[Runtime] Deserialize%sPipelineStateX from %s: console pipeline caches are "
                     "not read yet, such pipelines do nothing\n",
                     Kind ? "Compute" : "Graphics",
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    // XV_DS_TIME: when each of the first ones comes, and on which thread
    if (std::getenv("XV_DS_TIME") && index < 400) {
        std::fprintf(stderr, "[Time] at %llu ms thread %lu Deserialize%s %d\n", GetTickCount64(),
                     GetCurrentThreadId(), Kind ? "Compute" : "Graphics", index);
        std::fflush(stderr);
    }
    // XV_SHADER_CODE=<folder>: the state and each of its parts, under `name`
    const auto dump_parts = [&](const std::string &name) {
        if (!state || !readable(reinterpret_cast<std::uint64_t>(state), 0x100))
            return;
        if (auto *const file = std::fopen((name + ".bin").c_str(), "wb")) {
            std::fwrite(state, 1, 0x100, file);
            std::fclose(file);
        }
        // the root signature its programs are read with
        if (const auto part = signature_part(reinterpret_cast<const void *>(state[0]));
            !part.empty())
            if (auto *const file = std::fopen((name + ".rts0").c_str(), "wb")) {
                std::fwrite(part.data(), 1, part.size(), file);
                std::fclose(file);
            }
        for (int word = 0; word < 32; ++word)
            if (readable(state[word], 16)) {
                MEMORY_BASIC_INFORMATION info{};
                VirtualQuery(reinterpret_cast<void *>(state[word]), &info, sizeof info);
                const auto room = reinterpret_cast<std::uint64_t>(info.BaseAddress) +
                                  info.RegionSize - state[word];
                const auto length = word + 1 < 32 && state[word + 1] && state[word + 1] <= room &&
                                            state[word + 1] < (64u << 20)
                                        ? state[word + 1]
                                        : std::min<std::uint64_t>(room, 0x1000);
                if (auto *const file =
                        std::fopen((name + "_" + std::to_string(word) + ".bin").c_str(), "wb")) {
                    std::fwrite(reinterpret_cast<const void *>(state[word]), 1, length, file);
                    std::fclose(file);
                }
            }
    };
    const auto *const dump = std::getenv("XV_SHADER_CODE");
    if (dump && index < 6)
        dump_parts(std::string(dump) + (Kind ? "/pso_c_" : "/pso_g_") + std::to_string(index));
    if (!state || !pipeline || !readable(reinterpret_cast<std::uint64_t>(state), 8) ||
        signature_part(reinterpret_cast<const void *>(state[0])).empty())
        return E_NOTIMPL;
    auto *const signature = reinterpret_cast<ID3D12RootSignature *>(state[0]);
    if (Kind) {
        const auto shader = modules::empty_compute_shader();
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = signature;
        desc.CS = {shader.data(), shader.size()};
        using Create = HRESULT(STDMETHODCALLTYPE *)(
            void *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
        const auto made =
            shader.empty() ? E_NOTIMPL : original<Create>(self, 11)(self, &desc, iid, pipeline);
        if (SUCCEEDED(made))
            lost_pipeline("deserialized compute", pipeline, signature);
        return made;
    }
    auto vertex = modules::empty_vertex_shader(), pixel = modules::empty_pixel_shader();
    if (vertex.empty() || pixel.empty())
        return E_NOTIMPL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    // The state's parts, each a pointer and its length: the programs begin with hashes
    // (0xd580....) and the vertex program is the one that exports a position. The part
    // with 0x1000 at 4 and ~0 at 0x1c has the formats of the targets as bytes from 0x10,
    // the depth format at 0x24, how many targets at 0x25, and the input layout from 0x4c.
    // Where the programs are translated and the host's compiler takes them the pipeline is
    // made of them (one without a pixel program draws depth alone); its blend, depth and
    // rasterizer states are not read yet (XV_PSO_STAND_IN: never).
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> names;
    std::shared_ptr<const HeapUses> uses; // set: the pipeline is made of the title's programs
    std::string stopped_by;               // XV_D3D_LOST: what stops each of its programs
    if (!std::getenv("XV_PSO_STAND_IN")) {
        using Words = std::span<const std::uint32_t>;
        Words programs[2]{}; // vertex, pixel
        // what each binds, which is the part after it: records of eight words under a
        // header of four that begins with 2
        std::span<const std::uint8_t> records[2];
        std::span<const std::uint8_t> layout;
        for (int word = 1;
             word + 1 < 32 && readable(reinterpret_cast<std::uint64_t>(state), 8 * (word + 2));
             ++word) {
            const auto length = state[word + 1];
            if (length < 0x40 || length > (1u << 20) || !readable(state[word], length))
                continue;
            const auto *const words = reinterpret_cast<const std::uint32_t *>(state[word]);
            if (words[0] >> 16 == 0xd580) {
                bool position = false;
                for (std::size_t n = 0; n + 1 < length / 4 && words[n] != 0xbf810000; ++n)
                    position |= words[n] >> 26 == 0x3e && (words[n] >> 4 & 0x3f) == 12;
                programs[position ? 0 : 1] = {words, static_cast<std::size_t>(length / 4)};
                if (word + 3 < 32 && state[word + 3] >= 16 && state[word + 3] < (1u << 20) &&
                    readable(state[word + 2], state[word + 3]) &&
                    *reinterpret_cast<const std::uint32_t *>(state[word + 2]) == 2)
                    records[position ? 0 : 1] = {
                        reinterpret_cast<const std::uint8_t *>(state[word + 2]),
                        static_cast<std::size_t>(state[word + 3])};
            } else if (words[1] == 0x1000 && words[7] == ~0u) {
                layout = {reinterpret_cast<const std::uint8_t *>(state[word]),
                          static_cast<std::size_t>(length)};
            }
        }
        static std::mutex mutex;
        static std::map<std::uint64_t, std::vector<std::uint8_t>> made;
        static std::atomic<unsigned> real{0}, stood{0};
        // XV_PSO_ALL: of every pipeline, its root signature's address and the records of
        // its two programs, in <XV_SHADER_CODE>/pso_all.bin; each signature once, as
        // sig_<address>.rts0
        if (dump && std::getenv("XV_PSO_ALL")) {
            static std::set<const void *> written;
            const std::lock_guard lock(mutex);
            if (auto *const file = std::fopen((std::string(dump) + "/pso_all.bin").c_str(), "ab")) {
                const auto address = reinterpret_cast<std::uint64_t>(signature);
                std::fwrite(&address, 8, 1, file);
                for (const auto &part : records) {
                    const auto size = static_cast<std::uint32_t>(part.size());
                    std::fwrite(&size, 4, 1, file);
                    std::fwrite(part.data(), 1, part.size(), file);
                }
                std::fclose(file);
            }
            if (written.insert(signature).second) {
                char name[64];
                std::snprintf(name, sizeof name, "/sig_%llx.rts0",
                              reinterpret_cast<unsigned long long>(signature));
                const auto part = signature_part(signature);
                if (auto *const file = std::fopen((std::string(dump) + name).c_str(), "wb")) {
                    std::fwrite(part.data(), 1, part.size(), file);
                    std::fclose(file);
                }
            }
        }
        // what stops each program, and how many pipelines each of those reasons stops
        static std::map<std::uint64_t, std::string> stopped;
        static std::map<std::string, unsigned> reasons;
        const auto root = signature_part(signature);
        const auto shader = [&](modules::ShaderStage stage, Words program,
                                std::string &reason) -> std::span<const std::uint8_t> {
            reason = "no program found";
            if (program.empty())
                return {};
            std::uint64_t hash = 0xcbf29ce484222325ull ^ static_cast<std::uint64_t>(stage);
            const auto mix = [&](const void *data, std::size_t bytes) {
                for (std::size_t i = 0; i < bytes; ++i)
                    hash = (hash ^ static_cast<const std::uint8_t *>(data)[i]) * 0x100000001b3ull;
            };
            mix(program.data(), program.size_bytes());
            mix(root.data(), root.size());
            mix(layout.data(), layout.size());
            const auto &bound = records[stage == modules::ShaderStage::vertex ? 0 : 1];
            mix(bound.data(), bound.size());
            {
                const std::lock_guard lock(mutex);
                if (const auto found = made.find(hash); found != made.end()) {
                    reason = stopped[hash];
                    return found->second;
                }
            }
            auto compiled =
                modules::console_program_shader(stage, program, root, layout, &reason, bound);
            // a compiler's complaint is long and has the line in it: its first words tell
            if (reason.size() > 90)
                reason.resize(90);
            const std::lock_guard lock(mutex);
            stopped[hash] = reason;
            return made.emplace(hash, std::move(compiled)).first->second;
        };
        std::string why_vertex, why_pixel;
        const auto real_vertex = shader(modules::ShaderStage::vertex, programs[0], why_vertex);
        const auto real_pixel = shader(modules::ShaderStage::pixel, programs[1], why_pixel);
        {
            const std::lock_guard lock(mutex);
            if (real_vertex.empty())
                ++reasons["vertex: " + why_vertex];
            if (real_pixel.empty())
                ++reasons["pixel: " + why_pixel];
        }
        stopped_by =
            (real_vertex.empty() ? " vertex: " + why_vertex + ";" : "") +
            (real_pixel.empty() && !programs[1].empty() ? " pixel: " + why_pixel + ";" : "");
        // XV_PSO_WHY=<text>: the parts of the first few pipelines that such a reason stops,
        // as pso_w_<n>, with the reasons in pso_w_<n>.why
        if (const auto *const wanted = std::getenv("XV_PSO_WHY"); wanted && dump) {
            static std::atomic<int> dumped{0};
            const auto both = "vertex: " + (real_vertex.empty() ? why_vertex : "made") +
                              "; pixel: " + (real_pixel.empty() ? why_pixel : "made");
            if (both.find(wanted) != std::string::npos && dumped < 400) {
                // one of each pair of programs is enough
                static std::set<std::pair<const void *, const void *>> seen;
                bool fresh = false;
                {
                    const std::lock_guard lock(mutex);
                    fresh = seen.insert({programs[0].data(), programs[1].data()}).second;
                }
                if (const int number = fresh ? dumped++ : 8; number < 8) {
                    const auto name = std::string(dump) + "/pso_w_" + std::to_string(number);
                    dump_parts(name);
                    if (auto *const file = std::fopen((name + ".why").c_str(), "wb")) {
                        std::fputs(both.c_str(), file);
                        std::fclose(file);
                    }
                }
            }
        }
        const auto at32 = [&](std::size_t at) {
            std::uint32_t value = 0;
            if (at + 4 <= layout.size())
                std::memcpy(&value, layout.data() + at, 4);
            return value;
        };
        if (!real_vertex.empty() && (!real_pixel.empty() || programs[1].empty()) &&
            layout.size() >= 0x50 && heap_signature(self)) {
            vertex = real_vertex, pixel = real_pixel;
            // what the two programs' records name, and the parameters their registers hold
            auto reads = std::make_shared<HeapUses>();
            reads->made = why_vertex + " " + why_pixel;
            for (std::size_t index = 0; index < 2; ++index)
                for (std::size_t at = 16; at + 32 <= records[index].size(); at += 32) {
                    std::uint32_t words[6];
                    std::memcpy(words, records[index].data() + at, sizeof words);
                    if (words[0] != 0x41)
                        continue;
                    const auto in = words[5] >> 8 & 0xff;
                    if (words[1] == 2)
                        reads->stages[index].samplers.push_back(in);
                    else if (words[1] == 0x300 || words[1] == 0x302 || words[1] == 0x303)
                        reads->stages[index].uses.push_back(
                            {in, words[2] / 8,
                             words[1] == 0x300   ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                             : words[1] == 0x303 ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                                 : D3D12_DESCRIPTOR_RANGE_TYPE_CBV});
                }
            const auto at_root = [&](std::size_t at) {
                std::uint32_t value = 0;
                if (at + 4 <= root.size())
                    std::memcpy(&value, root.data() + at, 4);
                return value;
            };
            std::uint32_t reg = 8;
            for (std::uint32_t p = 0, count = std::min(at_root(4), 64u); p < count; ++p) {
                const auto entry = at_root(8) + 12 * p;
                const auto type = at_root(entry);
                const auto words = type == 0   ? 1u
                                   : type == 1 ? at_root(at_root(entry + 8) + 8)
                                               : 2u;
                if (type <= 1)
                    for (auto &stage : reads->stages)
                        stage.entries.push_back({type == 0 ? 1 : 5, reg, words, p});
                reg += words;
            }
            uses = std::move(reads);
            std::size_t at = 0x50;
            const auto count = std::min(at32(0x4c), 32u);
            names.reserve(count);
            for (std::uint32_t n = 0; n < count; ++n) {
                const auto length = at32(at);
                if (!length || length > 64 || at + 4 + length + 24 > layout.size())
                    break;
                names.emplace_back(reinterpret_cast<const char *>(layout.data()) + at + 4,
                                   length - 1);
                at += 4 + length;
                elements.push_back(
                    {names.back().c_str(), at32(at), static_cast<DXGI_FORMAT>(at32(at + 4)),
                     at32(at + 8), at32(at + 12),
                     static_cast<D3D12_INPUT_CLASSIFICATION>(at32(at + 16)), at32(at + 20)});
                at += 24;
            }
            desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
            desc.NumRenderTargets = std::min<UINT>(layout[0x25], 8);
            for (UINT target = 0; target < desc.NumRenderTargets; ++target)
                desc.RTVFormats[target] =
                    target_format(host_format(static_cast<DXGI_FORMAT>(layout[0x10 + target])));
            desc.DSVFormat = host_format(static_cast<DXGI_FORMAT>(layout[0x24]));
            for (auto &target : desc.BlendState.RenderTarget)
                target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            ++real;
        } else {
            ++stood;
        }
        if (const auto all = real + stood; (all & (all - 1)) == 0) {
            std::fprintf(stderr,
                         "[Runtime] pipelines of the title's cache so far: %u made of its "
                         "programs, %u that draw nothing%c",
                         real.load(), stood.load(), 10);
            const std::lock_guard lock(mutex);
            for (const auto &[reason, count] : reasons)
                std::fprintf(stderr, "[Runtime]   %u: %s%c", count, reason.c_str(), 10);
            std::fflush(stderr);
        }
    }
    desc.pRootSignature = uses ? heap_signature(self) : signature;
    desc.VS = {vertex.data(), vertex.size()};
    desc.PS = {pixel.data(), pixel.size()};
    desc.SampleMask = ~0u;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc.Count = 1;
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    auto result = original<Create>(self, 10)(self, &desc, iid, pipeline);
    // one of the title's programs that the host turns down: the one that draws nothing
    if (FAILED(result) && desc.InputLayout.NumElements) {
        static std::atomic<unsigned> refused{0};
        if (const auto count = ++refused; (count & (count - 1)) == 0) {
            std::fprintf(stderr,
                         "[Runtime] %u pipelines of the title's cache made of its programs "
                         "are turned down by the host (0x%08lx): they draw nothing%c",
                         count, static_cast<unsigned long>(result), 10);
            report_messages();
            std::fflush(stderr);
        }
        const auto empty_vertex = modules::empty_vertex_shader(),
                   empty_pixel = modules::empty_pixel_shader();
        desc.VS = {empty_vertex.data(), empty_vertex.size()};
        desc.PS = {empty_pixel.data(), empty_pixel.size()};
        desc.InputLayout = {};
        desc.pRootSignature = signature;
        uses.reset();
        desc.NumRenderTargets = 0;
        for (auto &format : desc.RTVFormats)
            format = DXGI_FORMAT_UNKNOWN;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        result = original<Create>(self, 10)(self, &desc, iid, pipeline);
    }
    // the title derives its pipelines from this one (CreateDerivedGraphicsPipelineStateX)
    if (SUCCEEDED(result))
        lost_pipeline(("deserialized graphics" + (uses ? std::string() : ":" + stopped_by)).c_str(),
                      pipeline, signature);
    if (SUCCEEDED(result) && *pipeline)
        keep_pipeline(*pipeline, desc);
    if (SUCCEEDED(result) && *pipeline && uses) {
        const std::lock_guard lock(kept_mutex);
        heap_pipelines[*pipeline] = uses;
    }
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] a pipeline that draws nothing: 0x%08lx%c",
                     static_cast<unsigned long>(result), 10);
        report_messages();
    }
    return result;
}
// GetGpuHardwareConfigurationX (68): the GPU's clock and which console it is: the first
// Xbox One, or the Series X
void STDMETHODCALLTYPE gpu_hardware_configuration(void *, std::uint64_t *configuration) {
    if (!configuration)
        return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    configuration[0] = static_cast<std::uint64_t>(frequency.QuadPart);
    configuration[1] = console == Target::scarlett ? 5 : 0;
}
constexpr std::size_t host_device_slots = 44, device_slots = 160;
template <std::size_t... Index>
std::vector<std::pair<std::size_t, Slot>> unknown_device_methods(std::index_sequence<Index...>) {
    return {{host_device_slots + Index,
             reinterpret_cast<Slot>(&unknown_device_method<host_device_slots + Index>)}...};
}

// ID3D12Device8::CreateCommittedResource2 and CreateSamplerFeedbackUnorderedAccessView (the
// host's 69 and 71). The first Series kit numbers the two sampler feedback formats 187 and
// 188, which the desktop has at 189 and 190.
HRESULT STDMETHODCALLTYPE create_committed_resource_2(
    ID3D12Device *self, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
    void *session, REFIID iid, void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(
        void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1 *,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, void *, REFIID, void **);
    D3D12_RESOURCE_DESC1 host_desc{};
    if (desc) {
        host_desc = *desc;
        host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host_desc.Flags & 0xff);
        host_desc.Format = host_format(host_desc.Format, true);
        if (host_desc.Flags &
            (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            host_desc.Format = target_format(host_desc.Format);
        if (host_desc.Layout >= 0x100)
            host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        host_resource_format(self, host_desc);
        if (host_desc.Format == 187 || host_desc.Format == 188) {
            host_desc.Format = static_cast<DXGI_FORMAT>(host_desc.Format + 2);
            host_desc.Alignment = 0; // the host has no small alignment for these
            // The console takes a mip region as large as the texture, one value for all of
            // it. The host stops at half: its map is then 2x2 where the title expects 1x1.
            auto &region = host_desc.SamplerFeedbackMipRegion;
            region.Width = std::min<UINT>(region.Width, static_cast<UINT>(host_desc.Width / 2));
            region.Height = std::min(region.Height, host_desc.Height / 2);
            if (host_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                region.Depth = 0;
            // The host wants as many mips as the texture the map is paired with later, where
            // the console takes one: a full chain, which is what a texture worth feedback has
            if (host_desc.MipLevels == 1)
                host_desc.MipLevels = static_cast<UINT16>(
                    std::bit_width(std::max<std::uint64_t>(host_desc.Width, host_desc.Height)));
        }
    }
    if (state == static_cast<D3D12_RESOURCE_STATES>(0x8c3))
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (desc)
        host_rules(heap, host_desc.Flags, state);
    D3D12_CLEAR_VALUE cleared{}, cleared_host{};
    clear = host_clear_value(self, host_clear(clear, cleared), host_desc.Flags, cleared_host);
    const auto result = original<Create>(self, 69)(self, heap, flags, desc ? &host_desc : nullptr,
                                                   state, clear, session, iid, resource);
    if (SUCCEEDED(result) && resource && *resource) {
        console_resource(*resource);
        track_buffer(static_cast<ID3D12Resource *>(*resource));
    }
    if (FAILED(result) && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource2 dimension %d %llux%ux%u mips %u format "
                     "%d flags 0x%x state 0x%x: 0x%08lx\n",
                     desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize,
                     desc->MipLevels, desc->Format, desc->Flags, state,
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// The console's takes a view description as well, null for the whole map
void STDMETHODCALLTYPE create_feedback_view(ID3D12Device *self, ID3D12Resource *target,
                                            ID3D12Resource *feedback,
                                            const D3D12_UNORDERED_ACCESS_VIEW_DESC *,
                                            D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    using Create = void(STDMETHODCALLTYPE *)(void *, ID3D12Resource *, ID3D12Resource *,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
    original<Create>(self, 71)(self, target, feedback, host_handle(handle));
    mark_host(handle);
    const std::lock_guard lock(feedback_mutex);
    feedback_texture[feedback] = target;
}
// GetCopyableFootprints (38): the description is a console one, with the console's flags
// and layouts, which the host answers with offsets of -1. A title adds those to the address
// it copies to.
void STDMETHODCALLTYPE copyable_footprints(ID3D12Device *self, const D3D12_RESOURCE_DESC *desc,
                                           UINT first, UINT count, UINT64 offset,
                                           D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows,
                                           UINT64 *row_sizes, UINT64 *total) {
    using Get =
        void(STDMETHODCALLTYPE *)(void *, const D3D12_RESOURCE_DESC *, UINT, UINT, UINT64,
                                  D3D12_PLACED_SUBRESOURCE_FOOTPRINT *, UINT *, UINT64 *, UINT64 *);
    D3D12_RESOURCE_DESC host{};
    if (desc) {
        host = *desc;
        host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & 0xff);
        if (host.Layout >= 0x100)
            host.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        host.Alignment = 0;
        host_resource_format(self, host);
    }
    if (console == Target::durango && desc && count && count <= 16384) {
        // The Xbox One has rows and subresources at multiples of 1024 bytes
        // (D3D12XBOX_TEXTURE_DATA_PITCH_ALIGNMENT), which titles also write by without asking.
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> placed(count);
        std::vector<UINT> lines(count);
        std::vector<UINT64> sizes(count);
        UINT64 bytes = 0;
        original<Get>(self, 38)(self, &host, first, count, 0, placed.data(), lines.data(),
                                sizes.data(), &bytes);
        if (bytes != ~0ull) {
            UINT64 end = 0;
            for (UINT i = 0; i < count; ++i) {
                const auto pitch = (sizes[i] + 1023) & ~1023ull;
                const auto start = (end + 1023) & ~1023ull;
                placed[i].Offset = offset + start;
                placed[i].Footprint.RowPitch = static_cast<UINT>(pitch);
                const auto slices = std::max(placed[i].Footprint.Depth, 1u);
                end = start + pitch * lines[i] * (slices - 1) +
                      pitch * (std::max(lines[i], 1u) - 1) + sizes[i];
            }
            if (layouts)
                std::copy(placed.begin(), placed.end(), layouts);
            if (rows)
                std::copy(lines.begin(), lines.end(), rows);
            if (row_sizes)
                std::copy(sizes.begin(), sizes.end(), row_sizes);
            if (total)
                *total = end;
            return;
        }
    }
    original<Get>(self, 38)(self, desc ? &host : nullptr, first, count, offset, layouts, rows,
                            row_sizes, total);
    if (static std::atomic<bool> shown{false};
        desc && total && *total == ~0ull && !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] GetCopyableFootprints: the host has no layout for dimension %d "
                     "%llux%ux%u mips %u format %d samples %u layout %d flags 0x%x\n",
                     desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize,
                     desc->MipLevels, desc->Format, desc->SampleDesc.Count, desc->Layout,
                     desc->Flags);
        std::fflush(stderr);
    }
}
// GetDeviceRemovedReason (37): a title asks once the host has dropped the device; why it did
// is worth a line
HRESULT STDMETHODCALLTYPE device_removed_reason(ID3D12Device *self) {
    const auto reason = original<HRESULT(STDMETHODCALLTYPE *)(void *)>(self, 37)(self);
    if (static std::atomic<bool> shown{false}; FAILED(reason) && !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] the host removed the Direct3D 12 device: 0x%08lx\n",
                     static_cast<unsigned long>(reason));
        report_failure();
        std::fflush(stderr);
    }
    return reason;
}
// CreateHeap (28): the console's heap flags and alignments. The host knows two alignments
// (64 KiB and 4 MiB) and its own flags; a heap is given the nearest of what it has.
HRESULT STDMETHODCALLTYPE create_heap(ID3D12Device *self, const D3D12_HEAP_DESC *desc, REFIID iid,
                                      void **heap) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_HEAP_DESC *, REFIID, void **);
    D3D12_HEAP_DESC host{};
    if (desc) {
        host = *desc;
        host.Flags = static_cast<D3D12_HEAP_FLAGS>(host.Flags & 0xfff);
        if (host.Alignment != 0 && host.Alignment != 0x10000 && host.Alignment != 0x400000)
            host.Alignment = host.Alignment > 0x10000 ? 0x400000 : 0x10000;
        host.SizeInBytes = (host.SizeInBytes + 0xffff) & ~0xffffull;
    }
    auto result = original<Create>(self, 28)(self, desc ? &host : nullptr, iid, heap);
    if (FAILED(result) && desc) {
        // once more with nothing but the size and the kind of heap
        D3D12_HEAP_DESC plain{};
        plain.SizeInBytes = host.SizeInBytes;
        plain.Properties.Type = host.Properties.Type >= 1 && host.Properties.Type <= 3
                                    ? host.Properties.Type
                                    : D3D12_HEAP_TYPE_DEFAULT;
        result = original<Create>(self, 28)(self, &plain, iid, heap);
        std::fprintf(stderr,
                     "[Runtime] CreateHeap of %llu bytes, type %d page %d pool %d, alignment "
                     "0x%llx, flags 0x%x: as a plain heap 0x%08lx\n",
                     desc->SizeInBytes, desc->Properties.Type, desc->Properties.CPUPageProperty,
                     desc->Properties.MemoryPoolPreference, desc->Alignment, desc->Flags,
                     static_cast<unsigned long>(result));
        if (FAILED(result))
            report_messages();
    }
    return result;
}
// CreateCommandSignature (41): which arguments the host turned down. The console has
// argument types of its own from 100 on.
HRESULT STDMETHODCALLTYPE create_command_signature(ID3D12Device *self,
                                                   const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                   ID3D12RootSignature *root, REFIID iid,
                                                   void **signature) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMMAND_SIGNATURE_DESC *,
                                                ID3D12RootSignature *, REFIID, void **);
    // 104 and 105 are the console's own dispatches (DISPATCHX, DISPATCH_L2): three counts,
    // as the desktop's dispatch has, which is what the host is given. What else sets them
    // apart on the console is not known here.
    D3D12_COMMAND_SIGNATURE_DESC host{};
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
    if (desc && desc->pArgumentDescs) {
        arguments.assign(desc->pArgumentDescs, desc->pArgumentDescs + desc->NumArgumentDescs);
        for (auto &argument : arguments)
            if (argument.Type == 104 || argument.Type == 105)
                argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        host = *desc;
        host.pArgumentDescs = arguments.data();
        desc = &host;
    }
    const auto result = original<Create>(self, 41)(self, desc, root, iid, signature);
    if (FAILED(result) && desc) {
        std::string types;
        for (UINT i = 0; i < desc->NumArgumentDescs && desc->pArgumentDescs; ++i)
            types += ' ' + std::to_string(desc->pArgumentDescs[i].Type);
        std::fprintf(stderr,
                     "[Runtime] CreateCommandSignature stride %u, %u arguments of types%s, root "
                     "signature %p: 0x%08lx\n",
                     desc->ByteStride, desc->NumArgumentDescs, types.c_str(),
                     static_cast<void *>(root), static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// A pipeline's stream is subobjects one after another, each a type and its description,
// aligned to 8 bytes; their sizes by type. The Series kit numbers the amplification and mesh
// shaders 23 and 24, where the desktop has them at 24 and 25, and has subobjects of its own
// from 25 on.
constexpr std::size_t subobject_sizes[] = {16, 24, 24, 24, 24, 24, 24, 40, 336, 8,  48, 56, 24,
                                           8,  8,  40, 8,  16, 8,  24, 8,  64,  32, 24, 24};
// ID3D12Device2::CreatePipelineState, from a stream of subobjects: the host's (47), and why
// when it turns the pipeline down
HRESULT STDMETHODCALLTYPE create_pipeline_state(ID3D12Device *self,
                                                const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                                REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_PIPELINE_STATE_STREAM_DESC *,
                                                REFIID, void **);
    const auto *const title_desc = desc;
    const PipelineClock clock{2};
    const auto &sizes = subobject_sizes;
    std::vector<std::uint8_t> stream;
    D3D12_PIPELINE_STATE_STREAM_DESC host{};
    // the title's vertex, pixel and compute shaders, and whether any had to be made for the
    // host: such a pipeline needs a root signature of its own, as in create_graphics_pipeline
    D3D12_SHADER_BYTECODE shaders[4] = {}; // and the mesh shader
    std::size_t shader_at[4] = {};
    bool simple_graphics = true;
    bool made = false;
    // a rectangle list: where the stream has its geometry shader and its rasterizer
    bool rectangles = false;
    std::size_t geometry_at = 0, rasterizer_at = 0;
    std::size_t signature_at = 0; // where in the stream the root signature's pointer is
    ID3D12RootSignature *from_shader = nullptr;
    std::shared_ptr<const HeapUses> uses;
    bool tried_heaps = false;
    std::string types = "stream of"; // XV_D3D_LOST: the subobjects, as the title numbers them
    std::string notes;               // and how its shaders were made for the host
    if (desc && desc->pPipelineStateSubobjectStream && console == Target::scarlett) {
        const auto *bytes = static_cast<const std::uint8_t *>(desc->pPipelineStateSubobjectStream);
        for (std::size_t at = 0; at + 8 <= desc->SizeInBytes;) {
            std::uint32_t type;
            std::memcpy(&type, bytes + at, 4);
            if (type >= std::size(sizes)) {
                if (static std::atomic<bool> shown{false}; !shown.exchange(true))
                    std::fprintf(stderr,
                                 "[Runtime] CreatePipelineState: console subobject %u, which "
                                 "ends what the host is given of the stream\n",
                                 type);
                break;
            }
            const auto size = std::min(sizes[type], desc->SizeInBytes - at);
            stream.insert(stream.end(), bytes + at, bytes + at + size);
            {
                std::uint64_t value = 0;
                if (size >= 16)
                    std::memcpy(&value, bytes + at + 8, 8);
                else if (size >= 8)
                    std::memcpy(&value, bytes + at + 4, 4);
                char text[40];
                std::snprintf(text, sizeof text, " %u:%llx", type,
                              static_cast<unsigned long long>(value));
                types += text;
            }
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY && size >= 8) {
                D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
                std::memcpy(&topology, stream.data() + stream.size() - size + 4, 4);
                rectangles |= topology == rectangle_type;
                topology = host_topology_type(topology);
                std::memcpy(stream.data() + stream.size() - size + 4, &topology, 4);
            }
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER && size >= 12)
                rasterizer_at = stream.size() - size + 4;
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS &&
                size >= 8 + sizeof(D3D12_SHADER_BYTECODE))
                geometry_at = stream.size() - size + 8;
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT && size >= 8) {
                DXGI_FORMAT depth;
                std::memcpy(&depth, stream.data() + stream.size() - size + 4, 4);
                depth = host_format(depth);
                std::memcpy(stream.data() + stream.size() - size + 4, &depth, 4);
            }
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS && size >= 36)
                for (std::size_t target = 0; target < 8; ++target) {
                    DXGI_FORMAT format;
                    const auto at_format = stream.size() - size + 4 + 4 * target;
                    std::memcpy(&format, stream.data() + at_format, 4);
                    format = target_format(format);
                    std::memcpy(stream.data() + at_format, &format, 4);
                }
            if (type == 23 || type == 24) {
                ++type;
                std::memcpy(stream.data() + stream.size() - size, &type, 4);
            }
            const auto value_at = stream.size() - size + 8;
            const int shader = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS   ? 0
                               : type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ? 1
                               : type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ? 2
                               : type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS ? 3
                                                                                : -1;
            if (shader >= 0 && size >= 8 + sizeof(D3D12_SHADER_BYTECODE)) {
                shader_at[shader] = value_at;
                std::memcpy(&shaders[shader], stream.data() + value_at, sizeof shaders[shader]);
            }
            if ((type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) &&
                size >= 8 + sizeof(D3D12_SHADER_BYTECODE)) {
                D3D12_SHADER_BYTECODE other{};
                std::memcpy(&other, stream.data() + value_at, sizeof other);
                simple_graphics &= !other.pShaderBytecode;
            }
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE && size >= 16)
                signature_at = stream.size() - size + 8;
            // the console's flags
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS && size >= 8)
                std::memset(stream.data() + stream.size() - size + 4, 0, 4);
            at += size;
        }
        D3D12_SHADER_BYTECODE vertex = shaders[0], pixel = shaders[1], compute = shaders[2];
        void *signature = nullptr;
        if (signature_at)
            std::memcpy(&signature, stream.data() + signature_at, sizeof signature);
        const bool heaps = simple_graphics && !compute.pShaderBytecode &&
                           heap_shaders_wanted(self) && vertex.pShaderBytecode &&
                           pixel.pShaderBytecode && heap_layout_known(vertex) &&
                           heap_layout_known(pixel);
        tried_heaps = heaps;
        auto mesh = shaders[3];
        // a mesh shader and its pixel shader read the heaps themselves, as a vertex and pixel
        // pair does, where the mesh shader's file says what its registers hold
        const bool mesh_heaps =
            mesh.pShaderBytecode && heap_shaders_wanted(self) &&
            modules::console_only_shader(shader_bytes(mesh)) &&
            !modules::shader_heap_layout(shader_bytes(mesh)).entries.empty() &&
            (!pixel.pShaderBytecode || modules::console_only_shader(shader_bytes(pixel)));
        if (mesh.pShaderBytecode) {
            tried_heaps = mesh_heaps;
            // a mesh shader is made for Shader Model 6, and the pixel shader with it as well:
            // a pipeline cannot have both kinds of bytecode
            modules::use_heap_shaders(mesh_heaps);
            made = host_bytecode(modules::ShaderStage::mesh, mesh, modules::HostShaderModel::sm6,
                                 {}, signature);
            if (static std::atomic<bool> shown{false}; !made && !shown.exchange(true)) {
                describe_shader("mesh", shaders[3]);
                std::fprintf(stderr, "[Runtime]   console code only: %d\n",
                             modules::console_only_shader(shader_bytes(shaders[3])));
            }
            made |= host_bytecode(modules::ShaderStage::pixel, pixel, modules::HostShaderModel::sm6,
                                  {}, signature);
            modules::use_heap_shaders(false);
            if (shader_at[3])
                std::memcpy(stream.data() + shader_at[3], &mesh, sizeof mesh);
            if (mesh_heaps && made) {
                auto reads = std::make_shared<HeapUses>();
                reads->stages[0] = heap_stage(shaders[3]);
                reads->stages[1] = heap_stage(shaders[1]);
                reads->made = "a container's mesh";
                uses = std::move(reads);
                auto *const ours = heap_signature(self);
                if (!signature_at) {
                    stream.resize(stream.size() + 16);
                    const UINT type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                    std::memcpy(stream.data() + stream.size() - 16, &type, 4);
                    signature_at = stream.size() - 8;
                }
                std::memcpy(stream.data() + signature_at, &ours, sizeof ours);
                signature = ours;
            }
        } else {
            modules::use_heap_shaders(heaps);
            made = host_graphics_pair(self, vertex, pixel,
                                      simple_graphics && !compute.pShaderBytecode, {}, signature);
            modules::use_heap_shaders(false);
        }
        if (heaps && made) {
            auto reads = std::make_shared<HeapUses>();
            reads->stages[0] = heap_stage(shaders[0]);
            reads->stages[1] = heap_stage(shaders[1]);
            reads->made = "a container's";
            uses = std::move(reads);
            auto *const ours = heap_signature(self);
            if (!signature_at) {
                stream.resize(stream.size() + 16);
                const UINT type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                std::memcpy(stream.data() + stream.size() - 16, &type, 4);
                signature_at = stream.size() - 8;
            }
            std::memcpy(stream.data() + signature_at, &ours, sizeof ours);
            signature = ours;
        }
        const bool compute_heaps = heap_compute_shader(self, compute);
        tried_heaps |= compute_heaps;
        modules::use_heap_shaders(compute_heaps);
        const bool computes = host_bytecode(modules::ShaderStage::compute, compute,
                                            modules::HostShaderModel::sm5, {}, signature);
        modules::use_heap_shaders(false);
        if (computes && compute_heaps) {
            uses = compute_heap_uses(shaders[2]);
            auto *const ours = heap_signature(self);
            if (!signature_at) {
                stream.resize(stream.size() + 16);
                const UINT type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                std::memcpy(stream.data() + stream.size() - 16, &type, 4);
                signature_at = stream.size() - 8;
            }
            std::memcpy(stream.data() + signature_at, &ours, sizeof ours);
            signature = ours;
        }
        made |= computes;
        if (made && std::getenv("XV_D3D_LOST")) {
            notes = ":";
            for (int index = 0; index < 3; ++index)
                if (shaders[index].pShaderBytecode &&
                    modules::console_only_shader(shader_bytes(shaders[index])))
                    notes += " " + modules::host_shader_note(
                                       static_cast<modules::ShaderStage>(index),
                                       shader_bytes(shaders[index]), uses != nullptr);
        }
        const D3D12_SHADER_BYTECODE translated[] = {vertex, pixel, compute};
        for (std::size_t index = 0; index < std::size(translated); ++index)
            if (shader_at[index])
                std::memcpy(stream.data() + shader_at[index], &translated[index], sizeof vertex);
        // the fourth corner of each rectangle, as in create_graphics_pipeline
        if (rectangles && simple_graphics && vertex.pShaderBytecode) {
            const auto corner = modules::rectangle_shader(shader_bytes(vertex));
            if (corner.empty()) {
                report_half_rectangles();
            } else {
                if (!geometry_at) {
                    stream.resize(stream.size() + 24);
                    const UINT type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS;
                    std::memcpy(stream.data() + stream.size() - 24, &type, 4);
                    geometry_at = stream.size() - 16;
                }
                const D3D12_SHADER_BYTECODE geometry{corner.data(), corner.size()};
                std::memcpy(stream.data() + geometry_at, &geometry, sizeof geometry);
                if (rasterizer_at) {
                    const auto cull = D3D12_CULL_MODE_NONE;
                    std::memcpy(stream.data() + rasterizer_at + 4, &cull, 4);
                }
            }
        }
        if (made && !signature &&
            (from_shader = shader_root_signature(self, !shaders[2].pShaderBytecode,
                                                 {shaders[0], shaders[1], shaders[2]}))) {
            if (!signature_at) {
                stream.resize(stream.size() + 16);
                signature_at = stream.size() - 8;
            }
            std::memcpy(stream.data() + signature_at, &from_shader, sizeof from_shader);
        }
        host = {stream.size(), stream.data()};
        desc = &host;
    } else if (desc && desc->pPipelineStateSubobjectStream) {
        // an Xbox One stream is the host's as it is, but for its formats
        const auto *bytes = static_cast<const std::uint8_t *>(desc->pPipelineStateSubobjectStream);
        stream.assign(bytes, bytes + desc->SizeInBytes);
        host = {stream.size(), stream.data()};
        desc = &host;
    }
    // the formats it draws to, the host's (see host_format), in the host's numbering of the
    // subobjects: D3D12_RT_FORMAT_ARRAY's eight formats, and the depth stencil format
    for (std::size_t at = 0; desc == &host && at + 8 <= stream.size();) {
        std::uint32_t type;
        std::memcpy(&type, stream.data() + at, 4);
        const std::size_t size = type < std::size(sizes)                          ? sizes[type]
                                 : type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS ? 24
                                                                                  : 0;
        if (!size || at + size > stream.size())
            break;
        const bool depth = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
        if (depth || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS)
            for (std::size_t slot = 0; slot < (depth ? 1u : 8u); ++slot) {
                DXGI_FORMAT format;
                std::memcpy(&format, stream.data() + at + 4 + slot * 4, sizeof format);
                format = host_format(self, format,
                                     depth ? FormatUse::depth_view : FormatUse::target_view);
                std::memcpy(stream.data() + at + 4 + slot * 4, &format, sizeof format);
            }
        at += size;
    }
    HRESULT result;
    {
        const HostClock timed{2};
        result = original<Create>(self, 47)(self, desc, iid, state);
    }
    if (retry_without_heaps(result, tried_heaps)) {
        if (from_shader)
            from_shader->Release();
        heap_shaders_refused = true;
        const auto again = create_pipeline_state(self, title_desc, iid, state);
        heap_shaders_refused = false;
        return again;
    }
    if (SUCCEEDED(result) && state && *state && uses) {
        const std::lock_guard lock(kept_mutex);
        heap_pipelines[*state] = uses;
    }
    if (SUCCEEDED(result) && state) {
        void *signature = nullptr;
        if (signature_at)
            std::memcpy(&signature, stream.data() + signature_at, sizeof signature);
        if (std::getenv("XV_D3D_LOST")) {
            types += "; as the host has it:";
            for (std::size_t at = 0; at < stream.size(); ++at) {
                char byte[4];
                std::snprintf(byte, sizeof byte, at % 4 ? "%02x" : " %02x", stream[at]);
                types += byte;
            }
        }
        lost_pipeline((types + (from_shader ? std::string(", signature from its shaders")
                                : shaders[2].pShaderBytecode ? ", compute" + notes
                                : made                       ? ", shaders made for the host" + notes
                                                             : std::string()))
                          .c_str(),
                      state, signature);
    }
    if (from_shader)
        from_shader->Release();
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreatePipelineState from a stream of %zu bytes: 0x%08lx\n",
                     desc ? desc->SizeInBytes : 0, static_cast<unsigned long>(result));
        // what the stream has, by the title's numbers, for the first few
        if (static std::atomic<int> told{0}; told++ < 3) {
            std::fprintf(stderr, "[Runtime]   %s%s\n", types.substr(0, 400).c_str(),
                         (made ? ", shaders made for the host" + notes : std::string()).c_str());
            if (debug_messages)
                report_messages();
        }
        if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_LOST") && shown++ < 40)
            std::fprintf(stderr, "[Lost]   %s%s\n", types.c_str(),
                         (made ? ", shaders made for the host" + notes : std::string()).c_str());
        report_messages();
    }
    return result;
}

// A title makes far more pipelines than it draws with, and makes them as it starts: a Series
// title tens of thousands in its first seconds, on worker threads it waits for before it
// shows anything, of which a few dozen draw its first screens. Making each for the host
// there (translating its shaders, the host's driver compiling them) holds the title at its
// start for minutes. So the title gets a pipeline of ours with what it was made of, and the
// host's pipeline is made the first time a list is given it. What it is made of may be the
// title's memory only for the call: the description is copied, the shaders kept once each
// for as long as a pipeline not made yet has them. A description with what is not copied
// here (stream output, a cached blob, view instancing, subobjects of unknown size) is made
// at once, as before. XV_PSO_DEFER=0 makes every pipeline at once.
struct DeferredPipeline {
    Slot *vtable;
    // the title's SetPipelineState copies 32 bytes from +0x10 of the pipeline into the list
    std::uint8_t packet[0x38]{};
    std::atomic<ULONG> references{1};
    ID3D12Device *device = nullptr;
    int kind = 0; // 0 graphics, 1 compute, 2 a stream
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    std::vector<std::uint8_t> stream;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::deque<std::string> names;
    std::vector<ID3D12RootSignature *> signatures;                         // held until made
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> shaders; // likewise
    std::wstring name;
    std::once_flag once;
    ID3D12PipelineState *host = nullptr;
    std::atomic<bool> ready{false};  // `host` is made, or turned down
    std::atomic<bool> queued{false}; // given to the threads that make pipelines
};
// for a title that asks for an interface a pipeline of ours has
bool deferrable(REFIID iid) {
    static const bool on = [] {
        const auto *const text = std::getenv("XV_PSO_DEFER");
        return !text || *text != '0';
    }();
    return on &&
           is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12PipelineState>(
               iid);
}
// the shaders of pipelines not made yet, by where the title had them: a title passes the
// same shader for many pipelines, and what is at an address is compared before it is shared
std::mutex held_mutex;
std::map<std::pair<const void *, SIZE_T>, std::weak_ptr<const std::vector<std::uint8_t>>>
    held_shaders;
D3D12_SHADER_BYTECODE hold_shader(DeferredPipeline &pipeline, const D3D12_SHADER_BYTECODE &code) {
    if (!code.pShaderBytecode || !code.BytecodeLength)
        return {};
    const auto *const bytes = static_cast<const std::uint8_t *>(code.pShaderBytecode);
    const std::pair key{code.pShaderBytecode, code.BytecodeLength};
    std::shared_ptr<const std::vector<std::uint8_t>> held;
    {
        const std::lock_guard lock(held_mutex);
        if (const auto found = held_shaders.find(key); found != held_shaders.end())
            held = found->second.lock();
    }
    if (!held || std::memcmp(held->data(), bytes, code.BytecodeLength)) {
        held =
            std::make_shared<const std::vector<std::uint8_t>>(bytes, bytes + code.BytecodeLength);
        const std::lock_guard lock(held_mutex);
        held_shaders[key] = held;
    }
    pipeline.shaders.push_back(held);
    return {held->data(), held->size()};
}
void hold_signature(DeferredPipeline &pipeline, ID3D12RootSignature *signature) {
    if (!signature)
        return;
    signature->AddRef();
    pipeline.signatures.push_back(signature);
}
D3D12_INPUT_LAYOUT_DESC hold_layout(DeferredPipeline &pipeline,
                                    const D3D12_INPUT_LAYOUT_DESC &layout) {
    if (!layout.pInputElementDescs || !layout.NumElements)
        return {};
    for (UINT i = 0; i < layout.NumElements; ++i) {
        auto element = layout.pInputElementDescs[i];
        if (element.SemanticName)
            element.SemanticName = pipeline.names.emplace_back(element.SemanticName).c_str();
        pipeline.elements.push_back(element);
    }
    return {pipeline.elements.data(), layout.NumElements};
}
void let_go(DeferredPipeline &pipeline) {
    for (auto *const signature : pipeline.signatures)
        signature->Release();
    pipeline.signatures.clear();
    pipeline.shaders.clear();
}
DeferredPipeline *deferred_of(const void *pipeline);
// the host's pipeline of one of ours, made now if it was not; null if the host turns it down
ID3D12PipelineState *made_pipeline(DeferredPipeline &pipeline) {
    std::call_once(pipeline.once, [&] {
        void *made = nullptr;
        const auto iid = __uuidof(ID3D12PipelineState);
        const D3D12_PIPELINE_STATE_STREAM_DESC stream{pipeline.stream.size(),
                                                      pipeline.stream.data()};
        const auto result =
            pipeline.kind == 0
                ? create_graphics_pipeline(pipeline.device, &pipeline.graphics, iid, &made)
            : pipeline.kind == 1
                ? create_compute_pipeline(pipeline.device, &pipeline.compute, iid, &made)
                : create_pipeline_state(pipeline.device, &stream, iid, &made);
        pipeline.host = SUCCEEDED(result) ? static_cast<ID3D12PipelineState *>(made) : nullptr;
        if (pipeline.host && !pipeline.name.empty())
            pipeline.host->SetName(pipeline.name.c_str());
        if (static std::atomic<int> told{0}; !pipeline.host && told++ < 8) {
            std::fprintf(stderr,
                         "[Runtime] a pipeline the title made earlier is turned down by the host "
                         "when a list is first given it (0x%08lx): what is drawn or computed "
                         "with it is left out\n",
                         static_cast<unsigned long>(result));
            std::fflush(stderr);
        }
        let_go(pipeline);
    });
    pipeline.ready.store(true, std::memory_order_release);
    return pipeline.host;
}
ID3D12PipelineState *host_pipeline(ID3D12PipelineState *state) {
    auto *const deferred = deferred_of(state);
    return deferred ? made_pipeline(*deferred) : state;
}
HRESULT STDMETHODCALLTYPE deferred_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (!is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12PipelineState>(
            iid))
        return E_NOINTERFACE;
    ++static_cast<DeferredPipeline *>(self)->references;
    *object = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE deferred_add_ref(void *self) {
    return ++static_cast<DeferredPipeline *>(self)->references;
}
ULONG STDMETHODCALLTYPE deferred_release(void *self) {
    auto *const pipeline = static_cast<DeferredPipeline *>(self);
    const auto left = --pipeline->references;
    if (left == 0) {
        let_go(*pipeline);
        if (pipeline->host)
            pipeline->host->Release();
        delete pipeline;
    }
    return left;
}
// The pipelines lists are given before they are made are made on threads of ours. The host's
// driver may take many seconds over one, and a title stops itself when its render thread has
// been held that long: a list waits for its pipeline at most a second of each frame all
// told, and what it would draw or compute with one that is not made by then is left out.
std::mutex making_mutex;
std::condition_variable to_make_added, pipeline_made;
std::deque<DeferredPipeline *> to_make; // each with a reference of ours
void make_pipelines() {
    for (;;) {
        DeferredPipeline *pipeline = nullptr;
        {
            std::unique_lock lock(making_mutex);
            to_make_added.wait(lock, [] { return !to_make.empty(); });
            pipeline = to_make.front();
            to_make.pop_front();
        }
        made_pipeline(*pipeline);
        // taken, so that a list that has just found it not made is waiting when told
        { const std::lock_guard lock(making_mutex); }
        pipeline_made.notify_all();
        deferred_release(pipeline);
    }
}
ID3D12PipelineState *list_pipeline(ID3D12PipelineState *state) {
    auto *const deferred = deferred_of(state);
    if (!deferred)
        return state;
    if (deferred->ready.load(std::memory_order_acquire))
        return deferred->host;
    // One thread: the host's driver holds a lock of its own while it compiles, which the
    // title's threads that make resources wait on, and more threads would hold it longer.
    static const bool started = [] {
        std::thread(make_pipelines).detach();
        return true;
    }();
    static_cast<void>(started);
    using Clock = std::chrono::steady_clock;
    static std::uint64_t frame = ~0ull;
    static Clock::duration left{};
    std::unique_lock lock(making_mutex);
    if (!deferred->queued.exchange(true)) {
        deferred_add_ref(deferred);
        to_make.push_back(deferred);
        to_make_added.notify_one();
    }
    if (const auto now = frames_shown(); now != frame) {
        frame = now;
        left = std::chrono::seconds(1);
    }
    const auto waited_from = Clock::now();
    pipeline_made.wait_for(lock, left, [&] { return deferred->ready.load(); });
    left -= std::min(left, Clock::now() - waited_from);
    if (deferred->ready.load(std::memory_order_acquire))
        return deferred->host;
    if (static std::atomic<bool> told{false}; !told.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a pipeline the host's driver takes long to make is left "
                             "out of the frame until it is made\n");
        std::fflush(stderr);
    }
    return nullptr;
}
// private data: what the host's pipeline would keep, which nothing here reads back
HRESULT STDMETHODCALLTYPE deferred_get_data(void *, REFGUID, UINT *size, void *) {
    if (size)
        *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}
HRESULT STDMETHODCALLTYPE deferred_set_data(void *, REFGUID, UINT, const void *) { return S_OK; }
HRESULT STDMETHODCALLTYPE deferred_set_name(void *self, const wchar_t *name) {
    auto *const pipeline = static_cast<DeferredPipeline *>(self);
    pipeline->name = name ? name : L"";
    if (pipeline->host)
        pipeline->host->SetName(pipeline->name.c_str());
    return S_OK;
}
HRESULT STDMETHODCALLTYPE deferred_get_device(void *self, REFIID iid, void **device) {
    return static_cast<DeferredPipeline *>(self)->device->QueryInterface(iid, device);
}
HRESULT STDMETHODCALLTYPE deferred_cached_blob(void *self, ID3DBlob **blob) {
    auto *const host = made_pipeline(*static_cast<DeferredPipeline *>(self));
    return host ? host->GetCachedBlob(blob) : E_FAIL;
}
template <std::size_t Index> HRESULT STDMETHODCALLTYPE unknown_deferred_method(void *) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: pipeline slot %zu from %s\n", Index,
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
template <std::size_t... Index>
std::array<Slot, 24> deferred_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, 24> vtable{reinterpret_cast<Slot>(&unknown_deferred_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&deferred_query);
    vtable[1] = reinterpret_cast<Slot>(&deferred_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&deferred_release);
    vtable[3] = reinterpret_cast<Slot>(&deferred_get_data);
    vtable[4] = vtable[5] = reinterpret_cast<Slot>(&deferred_set_data);
    vtable[6] = reinterpret_cast<Slot>(&deferred_set_name);
    vtable[7] = reinterpret_cast<Slot>(&deferred_get_device);
    vtable[8] = reinterpret_cast<Slot>(&deferred_cached_blob);
    return vtable;
}
std::array<Slot, 24> deferred_vtable = deferred_vtable_of(std::make_index_sequence<24>{});
DeferredPipeline *deferred_of(const void *pipeline) {
    return pipeline && *static_cast<Slot *const *>(pipeline) == deferred_vtable.data()
               ? static_cast<DeferredPipeline *>(const_cast<void *>(pipeline))
               : nullptr;
}
HRESULT hand_out_deferred(std::unique_ptr<DeferredPipeline> pipeline, REFIID iid, void **state) {
    pipeline->vtable = deferred_vtable.data();
    auto *const made = pipeline.release();
    const auto result = deferred_query(made, iid, state);
    deferred_release(made);
    return result;
}
HRESULT STDMETHODCALLTYPE deferred_graphics_pipeline(ID3D12Device *self,
                                                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                     REFIID iid, void **state) {
    if (!desc || !state || !deferrable(iid) || desc->StreamOutput.NumEntries ||
        desc->CachedPSO.pCachedBlob)
        return create_graphics_pipeline(self, desc, iid, state);
    auto pipeline = std::make_unique<DeferredPipeline>();
    pipeline->device = self;
    auto &kept = pipeline->graphics;
    kept = *desc;
    for (auto *const shader : {&kept.VS, &kept.PS, &kept.DS, &kept.HS, &kept.GS})
        *shader = hold_shader(*pipeline, *shader);
    kept.StreamOutput = {};
    kept.InputLayout = hold_layout(*pipeline, desc->InputLayout);
    hold_signature(*pipeline, desc->pRootSignature);
    return hand_out_deferred(std::move(pipeline), iid, state);
}
HRESULT STDMETHODCALLTYPE deferred_compute_pipeline(ID3D12Device *self,
                                                    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                    REFIID iid, void **state) {
    if (!desc || !state || !deferrable(iid) || desc->CachedPSO.pCachedBlob)
        return create_compute_pipeline(self, desc, iid, state);
    auto pipeline = std::make_unique<DeferredPipeline>();
    pipeline->device = self;
    pipeline->kind = 1;
    pipeline->compute = *desc;
    pipeline->compute.CS = hold_shader(*pipeline, desc->CS);
    hold_signature(*pipeline, desc->pRootSignature);
    return hand_out_deferred(std::move(pipeline), iid, state);
}
HRESULT STDMETHODCALLTYPE deferred_pipeline_state(ID3D12Device *self,
                                                  const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                                  REFIID iid, void **state) {
    if (!desc || !state || !deferrable(iid) || !desc->pPipelineStateSubobjectStream)
        return create_pipeline_state(self, desc, iid, state);
    auto pipeline = std::make_unique<DeferredPipeline>();
    pipeline->device = self;
    pipeline->kind = 2;
    const auto *const bytes =
        static_cast<const std::uint8_t *>(desc->pPipelineStateSubobjectStream);
    auto &stream = pipeline->stream;
    stream.assign(bytes, bytes + desc->SizeInBytes);
    // what each subobject points to, copied: shaders (amplification and mesh at the Series
    // kit's 23 and 24), the root signature and the input layout
    for (std::size_t at = 0; at + 8 <= stream.size();) {
        std::uint32_t type;
        std::memcpy(&type, stream.data() + at, 4);
        if (type >= std::size(subobject_sizes) || at + subobject_sizes[type] > stream.size())
            return create_pipeline_state(self, desc, iid, state);
        auto *const value = stream.data() + at + 8;
        if ((type >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS &&
             type <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) ||
            type == 23 || type == 24) {
            D3D12_SHADER_BYTECODE code;
            std::memcpy(&code, value, sizeof code);
            code = hold_shader(*pipeline, code);
            std::memcpy(value, &code, sizeof code);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            ID3D12RootSignature *signature;
            std::memcpy(&signature, value, sizeof signature);
            hold_signature(*pipeline, signature);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
            D3D12_INPUT_LAYOUT_DESC layout;
            std::memcpy(&layout, value, sizeof layout);
            layout = hold_layout(*pipeline, layout);
            std::memcpy(value, &layout, sizeof layout);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
            // what is not copied here: the declarations, the blob or the views' places, the
            // first of each a pointer to them or (view instancing) how many there are
            std::uint64_t first;
            std::memcpy(&first, value, sizeof first);
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING)
                first &= 0xffffffffu;
            if (first)
                return create_pipeline_state(self, desc, iid, state);
        }
        at += subobject_sizes[type];
    }
    return hand_out_deferred(std::move(pipeline), iid, state);
}
// the host's method `Index` after its first 44, at whatever slot the console has it
template <std::size_t... Index>
std::array<Slot, sizeof...(Index)> later_device_methods(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&moved_method<host_device_slots + Index>)...};
}

// D3D12XBOX_CREATE_DEVICE_PARAMETERS sizes the console's rings, which mean nothing here
HRESULT WINAPI xbox_create_device(void *, const UINT *parameters, REFIID iid, void **device) {
    if (!device)
        return E_POINTER;
    *device = nullptr;
    // the first of the parameters is the version of the kit the title was built with
    if (parameters && console == Target::scarlett) {
        std::fprintf(stderr, "[Runtime] the title asks for Direct3D 12 of kit version 0x%x\n",
                     *parameters);
    }
    configure_lists();
    using Create = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateDevice");
    // XV_D3D_DRED: what the GPU was doing when the host drops the device, without the debug
    // layer, which runs the queues one after the other and so hides what they do at once
    if (std::getenv("XV_D3D_DEBUG") || std::getenv("XV_D3D_DRED")) {
        using GetDebug = HRESULT(WINAPI *)(REFIID, void **);
        static const auto get_debug = host_d3d12<GetDebug>("D3D12GetDebugInterface");
        if (ID3D12Debug *debug = nullptr; std::getenv("XV_D3D_DEBUG") && get_debug &&
                                          SUCCEEDED(get_debug(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            // XV_D3D_DEBUG=2: also what each draw reads, checked on the GPU (slow)
            if (ID3D12Debug1 *gpu = nullptr; std::atoi(std::getenv("XV_D3D_DEBUG")) >= 2 &&
                                             SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&gpu)))) {
                // 3: the queues not serialized, as they are without the debug layer
                if (std::atoi(std::getenv("XV_D3D_DEBUG")) == 3)
                    gpu->SetEnableSynchronizedCommandQueueValidation(FALSE);
                else
                    gpu->SetEnableGPUBasedValidation(TRUE);
                gpu->Release();
            }
            debug->Release();
        }
        // The debug layer raises an exception of its own (0x87d) for some messages, for a
        // debugger to see. With none attached and a title's frames below, nobody takes it
        // and the process ends: it is passed over.
        static const auto passed_over =
            AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS *exception) -> LONG {
                return exception->ExceptionRecord->ExceptionCode == 0x87d
                           ? EXCEPTION_CONTINUE_EXECUTION
                           : EXCEPTION_CONTINUE_SEARCH;
            });
        static_cast<void>(passed_over);
        // what the GPU was doing when the device goes, and where it faulted
        if (ID3D12DeviceRemovedExtendedDataSettings *dred = nullptr;
            get_debug && SUCCEEDED(get_debug(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
        }
    }
    ID3D12Device *host = nullptr;
    const auto result =
        create ? create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host)) : E_NOTIMPL;
    if (SUCCEEDED(result)) {
        d3d12_device = host;
        graphics::drawing_with("Direct3D 12");
    }
    if (ID3D12InfoQueue *queue = nullptr;
        SUCCEEDED(result) && SUCCEEDED(host->QueryInterface(IID_PPV_ARGS(&queue))))
        debug_messages = queue;
    std::fprintf(stderr, "[Runtime] Direct3D 12 device for an %s title on the host: 0x%08lx\n",
                 std::string(target_name(console)).c_str(), static_cast<unsigned long>(result));
    // which runtime the host's d3d12.dll took: the system's, or the newer one next to xvboot
    if (wchar_t path[MAX_PATH];
        SUCCEEDED(result) &&
        GetModuleFileNameW(GetModuleHandleW(L"D3D12Core.dll"), path, MAX_PATH)) {
        ID3D12Device *later = nullptr;
        static constexpr GUID device10{
            0x517f8718, 0xaa66, 0x49f9, {0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x31}};
        const bool has =
            SUCCEEDED(host->QueryInterface(device10, reinterpret_cast<void **>(&later)));
        if (later)
            later->Release();
        std::fprintf(stderr,
                     "[Runtime] the host's Direct3D 12 runtime is %ls (%s ID3D12Device10)\n", path,
                     has ? "with" : "without");
    }
    std::fflush(stderr);
    if (FAILED(result))
        return result;
    auto overrides =
        unknown_device_methods(std::make_index_sequence<device_slots - host_device_slots>{});
    overrides.push_back({0, reinterpret_cast<Slot>(&device_query)});
    overrides.push_back({13, reinterpret_cast<Slot>(&check_feature_support)});
    overrides.push_back({14, reinterpret_cast<Slot>(&create_descriptor_heap)});
    overrides.push_back({15, reinterpret_cast<Slot>(&descriptor_increment)});
    overrides.push_back({16, reinterpret_cast<Slot>(&create_root_signature)});
    overrides.push_back({41, reinterpret_cast<Slot>(&create_command_signature)});
    overrides.push_back({38, reinterpret_cast<Slot>(&copyable_footprints)});
    overrides.push_back({37, reinterpret_cast<Slot>(&device_removed_reason)});
    if (console == Target::scarlett)
        overrides.push_back({28, reinterpret_cast<Slot>(&create_heap)});
    overrides.push_back({17, reinterpret_cast<Slot>(&create_cbv)});
    overrides.push_back({18, reinterpret_cast<Slot>(&create_srv)});
    overrides.push_back({19, reinterpret_cast<Slot>(&create_uav)});
    overrides.push_back({20, reinterpret_cast<Slot>(&create_target_view<20>)});
    overrides.push_back({21, reinterpret_cast<Slot>(&create_target_view<21>)});
    overrides.push_back({22, reinterpret_cast<Slot>(&create_sampler)});
    if (console == Target::scarlett)
        overrides.push_back({61, reinterpret_cast<Slot>(&create_sampler_x)});
    overrides.push_back({55, reinterpret_cast<Slot>(&create_derived_graphics_pipeline)});
    overrides.push_back({23, reinterpret_cast<Slot>(&copy_descriptors)});
    overrides.push_back({24, reinterpret_cast<Slot>(&copy_descriptors_simple)});
    overrides.push_back({25, reinterpret_cast<Slot>(&resource_allocation_info)});
    // GetAdapterLuid returns its 8 bytes in RAX
    overrides.push_back({43, reinterpret_cast<Slot>(&returned_in_rax<43>)});
    overrides.push_back({26, reinterpret_cast<Slot>(&custom_heap_properties)});
    overrides.push_back({27, reinterpret_cast<Slot>(&create_committed_resource)});
    overrides.push_back({44, reinterpret_cast<Slot>(&create_placed_resource_x)});
    if (console == Target::scarlett)
        overrides.push_back({133, reinterpret_cast<Slot>(&create_placed_resource_x_layout)});
    if (console == Target::scarlett) {
        overrides.push_back({127, reinterpret_cast<Slot>(&create_committed_resource_layout)});
        overrides.push_back({70, reinterpret_cast<Slot>(&create_srv_at)});
        overrides.push_back({57, reinterpret_cast<Slot>(&deserialize_pipeline<0>)});
        overrides.push_back({67, reinterpret_cast<Slot>(&deserialize_pipeline<1>)});
        overrides.push_back({71, reinterpret_cast<Slot>(&create_uav_at)});
    }
    overrides.push_back({8, reinterpret_cast<Slot>(&create_command_queue)});
    overrides.push_back({62, reinterpret_cast<Slot>(&create_compute_pipeline_x)});
    overrides.push_back({74, reinterpret_cast<Slot>(&register_page_pool)});
    overrides.push_back({10, reinterpret_cast<Slot>(&deferred_graphics_pipeline)});
    overrides.push_back({11, reinterpret_cast<Slot>(&deferred_compute_pipeline)});
    overrides.push_back({48, reinterpret_cast<Slot>(&create_command_queue_x)});
    overrides.push_back({73, reinterpret_cast<Slot>(&create_command_list)});
    overrides.push_back({12, reinterpret_cast<Slot>(&create_command_list_desktop)});
    overrides.push_back({9, reinterpret_cast<Slot>(&create_command_allocator)});
    overrides.push_back({36, reinterpret_cast<Slot>(&create_fence)});
    overrides.push_back({29, reinterpret_cast<Slot>(&create_placed_resource)});
    overrides.push_back({30, reinterpret_cast<Slot>(&create_reserved_resource)});
    overrides.push_back({60, reinterpret_cast<Slot>(&create_graphics_pipeline_x)});
    for (const std::size_t ignored : {49, 50, 51, 52, 53, 54, 58, 69, 72, 75})
        overrides.push_back({ignored, reinterpret_cast<Slot>(&ignored_device_method)});
    overrides.push_back({68, reinterpret_cast<Slot>(&gpu_hardware_configuration)});
    // how a GDK title paces its frames: there is no swap chain to wait for
    overrides.push_back({78, reinterpret_cast<Slot>(&set_frame_interval)});
    overrides.push_back({79, reinterpret_cast<Slot>(&schedule_frame_event)});
    overrides.push_back({80, reinterpret_cast<Slot>(&wait_frame_event)});
    overrides.push_back({81, reinterpret_cast<Slot>(&frame_statistics)});
    // The later device interfaces follow the console's own methods, where the host has them
    // right after the first interface's 44: ID3D12Device1 to 3 on Xbox One (87 to 92), and up
    // to ID3D12Device8 on Xbox Series (83 to 112, with a console method at 104).
    const auto later = later_device_methods(std::make_index_sequence<32>{});
    if (console == Target::durango) {
        for (std::size_t slot = 87; slot <= 92; ++slot)
            overrides.push_back({slot, later[slot - 87]});
        overrides.push_back({90, reinterpret_cast<Slot>(&deferred_pipeline_state)});
        overrides.push_back({46, reinterpret_cast<Slot>(&create_placed_srv)});
        overrides.push_back({47, reinterpret_cast<Slot>(&create_placed_uav)});
        overrides.push_back({59, reinterpret_cast<Slot>(&default_msaa_parameters)});
    } else {
        for (std::size_t slot = 83; slot <= 103; ++slot)
            overrides.push_back({slot, later[slot - 83]});
        for (std::size_t slot = 105; slot <= 112; ++slot)
            overrides.push_back({slot, later[slot - 84]});
        overrides.push_back({86, reinterpret_cast<Slot>(&deferred_pipeline_state)});
        overrides.push_back({108, reinterpret_cast<Slot>(&resource_allocation_info_2)});
        overrides.push_back({109, reinterpret_cast<Slot>(&create_committed_resource_2)});
        overrides.push_back({111, reinterpret_cast<Slot>(&create_feedback_view)});
    }
    // all of the host's vtable, which its runtime calls past the desktop's first 44 too
    console_vtable<Family::device>(host, 96, device_slots, overrides);
    // through the console's QueryInterface, which knows the kits' interface IDs
    const auto queried = device_query(host, iid, device);
    host->Release();
    return queried;
}
} // namespace

void console_d3d12_resource(void *resource) { console_resource(resource); }
HRESULT made_pipeline(void *pipeline) {
    return !pipeline                                                     ? E_POINTER
           : host_pipeline(static_cast<ID3D12PipelineState *>(pipeline)) ? S_OK
                                                                         : E_FAIL;
}

void *console_command_list(ID3D12GraphicsCommandList *host, bool enhanced_barriers) {
    static const bool configured = [] {
        configure_lists();
        return true;
    }();
    static_cast<void>(configured);
    host->AddRef();
    auto *const list = static_cast<ListProxy *>(make_list_proxy(host));
    list->legacy_barriers = !enhanced_barriers;
    return list;
}
void console_command_allocator(void *allocator) {
    console_vtable<Family::allocator>(allocator, 9, 12,
                                      {{9, reinterpret_cast<Slot>(&set_allocator_callback_x)},
                                       {10, reinterpret_cast<Slot>(&ignored_allocator_method)},
                                       {11, reinterpret_cast<Slot>(&ignored_allocator_method)}});
}
void console_fence(void *fence) {
    console_vtable<Family::fence>(fence, 12, 13,
                                  {{2, reinterpret_cast<Slot>(&release_fence)},
                                   {10, reinterpret_cast<Slot>(&signal_fence)},
                                   {12, reinterpret_cast<Slot>(&fence_gpu_address)}});
}
void call_as_title(const void *code, std::size_t size) {
    const std::lock_guard lock(test_code_mutex);
    for (auto &range : test_code)
        if (!range.start.load()) {
            range.start = static_cast<const std::uint8_t *>(code);
            range.end = static_cast<const std::uint8_t *>(code) + size;
            return;
        }
    throw std::length_error("Too many pieces of code call as the title");
}
void stop_calling_as_title(const void *code) {
    const std::lock_guard lock(test_code_mutex);
    for (auto &range : test_code)
        if (range.start.load() == code) {
            range.end = nullptr;
            range.start = nullptr;
        }
}
void forget_layouts() { later_queue = false; }
void report_failure() {
    auto *const device = d3d12_device.load();
    if (!device)
        return;
    {
        const std::lock_guard lock(signature_mutex);
        std::fprintf(stderr,
                     "[Runtime] root signatures %zu; title descriptors read by their tables: SRV "
                     "%llu, UAV %llu, CBV %llu, sampler %llu\n",
                     tables_of.size(), static_cast<unsigned long long>(decoded[0].load()),
                     static_cast<unsigned long long>(decoded[1].load()),
                     static_cast<unsigned long long>(decoded[2].load()),
                     static_cast<unsigned long long>(decoded[3].load()));
    }
    std::fprintf(stderr,
                 "[Runtime] Direct3D 12 device removed: 0x%08lx; descriptor heaps the title "
                 "let go of: %u\n",
                 static_cast<unsigned long>(device->GetDeviceRemovedReason()),
                 shadows_released.load());
    report_messages();
    ID3D12DeviceRemovedExtendedData *dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;
    if (D3D12_DRED_PAGE_FAULT_OUTPUT fault{}; SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)))
        std::fprintf(stderr, "[Runtime] GPU page fault at 0x%llx\n",
                     static_cast<unsigned long long>(fault.PageFaultVA));
    if (D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
        SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
        for (auto *node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
            const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount)
                continue;
            std::fprintf(stderr, "[Runtime] GPU stopped after %u of %u operations:", done,
                         node->BreadcrumbCount);
            for (UINT i = done > 3 ? done - 3 : 0; i < std::min(done + 3, node->BreadcrumbCount);
                 ++i)
                std::fprintf(stderr, " %s%d", i == done ? ">" : "",
                             static_cast<int>(node->pCommandHistory[i]));
            std::fprintf(stderr, "\n");
        }
    dred->Release();
    std::fflush(stderr);
}
ID3D12Resource *last_render_target() { return last_target.load(); }
std::uint64_t draws_into_target(ID3D12Resource *target) {
    const std::lock_guard lock(draw_mutex);
    const auto found = draws_into.find(target);
    return found == draws_into.end() ? 0 : found->second;
}
std::vector<ID3D12Resource *> bound_render_targets() {
    const std::lock_guard lock(target_mutex);
    return bound_targets;
}
bool read_back(ID3D12Resource *resource, std::vector<std::uint8_t> &data, unsigned &pitch,
               unsigned &width, unsigned &height, DXGI_FORMAT &format,
               D3D12_RESOURCE_STATES state) {
    auto *const queue = first_direct_queue.load();
    ID3D12Device *device = nullptr;
    if (!queue || FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    const auto desc = resource->GetDesc();
    // a multisampled texture cannot be copied to a buffer
    if (desc.SampleDesc.Count > 1) {
        device->Release();
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                    0,
                                    bytes,
                                    1,
                                    1,
                                    1,
                                    DXGI_FORMAT_UNKNOWN,
                                    {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                    D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *buffer = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    bool done = false;
    if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&buffer))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                            IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition = {resource, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        const D3D12_TEXTURE_COPY_LOCATION target{
            buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        const D3D12_TEXTURE_COPY_LOCATION source{
            resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList *const lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
        void *mapped = nullptr;
        if (SUCCEEDED(buffer->Map(0, nullptr, &mapped))) {
            const auto *start = static_cast<const std::uint8_t *>(mapped) + footprint.Offset;
            data.assign(start, start + std::size_t{footprint.Footprint.RowPitch} *
                                           footprint.Footprint.Height);
            buffer->Unmap(0, nullptr);
            pitch = footprint.Footprint.RowPitch;
            width = footprint.Footprint.Width;
            height = footprint.Footprint.Height;
            format = desc.Format;
            done = true;
        }
    }
    for (IUnknown *object : {static_cast<IUnknown *>(fence), static_cast<IUnknown *>(list),
                             static_cast<IUnknown *>(allocator), static_cast<IUnknown *>(buffer),
                             static_cast<IUnknown *>(device)})
        if (object)
            object->Release();
    return done;
}
ID3D12CommandQueue *direct_queue() { return first_direct_queue.load(); }
D3D12_RESOURCE_STATES tracked_state(ID3D12Resource *resource, D3D12_RESOURCE_STATES unknown) {
    const std::lock_guard lock(state_mutex);
    const auto found = target_states.find(resource);
    return found == target_states.end() ? unknown : found->second;
}
void set_adapter_wrapper(AdapterWrapper wrapper) { adapter_wrapper = wrapper; }

namespace {
HRESULT WINAPI succeeded() { return S_OK; }
// the console's debug interface is its own; titles go on without one
HRESULT WINAPI no_debug_interface(REFIID, void **debug) {
    if (debug)
        *debug = nullptr;
    return E_NOINTERFACE;
}
// Interface IDs the DLL exports as data, for titles built without their own copies
constexpr GUID graphics_unknown{
    0xaceeea63, 0xe0a9, 0x4a1c, {0xbb, 0xec, 0x71, 0xb2, 0xf4, 0x85, 0xf7, 0x58}};
constexpr GUID dma_command_list{
    0x30bc77b9, 0x8379, 0x4b25, {0x95, 0xde, 0xb8, 0x58, 0x8c, 0x42, 0x27, 0x5d}};
constexpr GUID debug_object_name{
    0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
template <class Interface> void *iid_of() {
    static const GUID iid = __uuidof(Interface);
    return const_cast<GUID *>(&iid);
}
} // namespace

void install(Registry &registry, std::string_view module, Target target) {
    console = target;
    const std::pair<const char *, void *> functions[] = {
        {"D3D12XboxCreateDevice", reinterpret_cast<void *>(&xbox_create_device)},
        {"D3D12CreateDevice", reinterpret_cast<void *>(&create_device_12)},
        {"D3D12GetDebugInterface", reinterpret_cast<void *>(&no_debug_interface)},
        {"D3D12XboxSetProcessDebugFlags", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxGetProcessDebugFlags", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxEnableMemoryProfiling", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxUnloadDriverModule", reinterpret_cast<void *>(&succeeded)},
        {"D3D12ConfigureVirtualMemoryX", reinterpret_cast<void *>(&succeeded)},
        {"D3DConfigureVirtualMemory", reinterpret_cast<void *>(&succeeded)},
        {"D3DFlushEntireCpuCache", reinterpret_cast<void *>(&succeeded)},
        {"D3D12CreateRootSignatureDeserializer",
         reinterpret_cast<void *>(&root_signature_deserializer)},
        {"D3D12CreateVersionedRootSignatureDeserializer",
         reinterpret_cast<void *>(&versioned_root_signature_deserializer)},
        {"D3D12SerializeRootSignature", reinterpret_cast<void *>(&serialize_root_signature)},
        {"D3D12SerializeVersionedRootSignature",
         reinterpret_cast<void *>(&serialize_versioned_root_signature)},
        {"IID_IGraphicsUnknown", const_cast<GUID *>(&graphics_unknown)},
        {"IID_ID3D12XboxDmaCommandList", const_cast<GUID *>(&dma_command_list)},
        {"WKPDID_D3DDebugObjectNameW", const_cast<GUID *>(&debug_object_name)},
        {"IID_ID3D12CommandAllocator", iid_of<ID3D12CommandAllocator>()},
        {"IID_ID3D12CommandList", iid_of<ID3D12CommandList>()},
        {"IID_ID3D12CommandQueue", iid_of<ID3D12CommandQueue>()},
        {"IID_ID3D12Debug", iid_of<ID3D12Debug>()},
        {"IID_ID3D12DescriptorHeap", iid_of<ID3D12DescriptorHeap>()},
        {"IID_ID3D12Device", iid_of<ID3D12Device>()},
        {"IID_ID3D12DeviceChild", iid_of<ID3D12DeviceChild>()},
        {"IID_ID3D12Fence", iid_of<ID3D12Fence>()},
        {"IID_ID3D12GraphicsCommandList", iid_of<ID3D12GraphicsCommandList>()},
        {"IID_ID3D12Heap", iid_of<ID3D12Heap>()},
        {"IID_ID3D12Object", iid_of<ID3D12Object>()},
        {"IID_ID3D12Pageable", iid_of<ID3D12Pageable>()},
        {"IID_ID3D12PipelineState", iid_of<ID3D12PipelineState>()},
        {"IID_ID3D12Resource", iid_of<ID3D12Resource>()},
        {"IID_ID3D12RootSignature", iid_of<ID3D12RootSignature>()},
        {"IID_ID3D12RootSignatureDeserializer", iid_of<ID3D12RootSignatureDeserializer>()},
        {"IID_IDXGIAdapter", iid_of<IDXGIAdapter>()},
        {"IID_IDXGIAdapter1", iid_of<IDXGIAdapter1>()},
        {"IID_IDXGIAdapter2", iid_of<IDXGIAdapter2>()},
        {"IID_IDXGIDevice", iid_of<IDXGIDevice>()},
        {"IID_IDXGIDevice1", iid_of<IDXGIDevice1>()},
        {"IID_IDXGIDevice2", iid_of<IDXGIDevice2>()},
        {"IID_IDXGIDeviceSubObject", iid_of<IDXGIDeviceSubObject>()},
        {"IID_IDXGIFactory", iid_of<IDXGIFactory>()},
        {"IID_IDXGIFactory1", iid_of<IDXGIFactory1>()},
        {"IID_IDXGIFactory2", iid_of<IDXGIFactory2>()},
        {"IID_IDXGIObject", iid_of<IDXGIObject>()},
        {"IID_IDXGISwapChain", iid_of<IDXGISwapChain>()},
        {"IID_IDXGISwapChain1", iid_of<IDXGISwapChain1>()}};
    for (const auto &[name, function] : functions)
        registry.add(module, name, function);
}
} // namespace xv::runtime::modules::d3d12x
