#include "runtime/modules/placement.hpp"
#include <atomic>
#include <catch2/catch.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>
#include <windows.h>

// A placed texture whose memory the title writes while its uploads are taken. Catch2 reports
// every access violation its own handler sees first, and puts its handler first as each test
// case starts, so this is one test case: the module's handler, installed by its first
// track_placement, comes before Catch2's in it. When it ends, the module lets go of the
// placement and of its handler, which the next test's first placement installs again.
namespace {
using namespace xv::runtime;

// the host's resource: only its description and its references are looked at
class Texture final : public ID3D12Resource {
  public:
    explicit Texture(const D3D12_RESOURCE_DESC &desc) : desc(desc) {}
    std::atomic<ULONG> references{1};
    std::function<void()> referenced; // when a reference is taken
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        if (referenced)
            referenced();
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **device) override {
        *device = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE *, void **) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) override {}
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return 0; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT,
                                                 UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT,
                                                  const D3D12_BOX *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *,
                                                D3D12_HEAP_FLAGS *) override {
        return E_NOTIMPL;
    }

  private:
    D3D12_RESOURCE_DESC desc;
};

// the module's placements and handler let go of when a test ends
struct ForgetPlacements {
    ForgetPlacements() = default;
    ~ForgetPlacements() { modules::forget_placements(); }
    ForgetPlacements(const ForgetPlacements &) = delete;
    ForgetPlacements &operator=(const ForgetPlacements &) = delete;
};
// the first byte of each upload taken, which gives back the taker's references
std::vector<int> take_first_bytes() {
    std::vector<int> first;
    for (auto &upload : modules::take_d3d12_uploads()) {
        if (!upload.subresources.empty() && !upload.subresources[0].empty())
            first.push_back(std::to_integer<int>(upload.subresources[0][0]));
        upload.resource->Release();
    }
    return first;
}
} // namespace

TEST_CASE("a placement is uploaded again after every write, one made while it is read too") {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 256;
    desc.Height = 256;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    constexpr std::size_t bytes = 256 * 256 * 4;
    auto *const memory = static_cast<volatile std::uint8_t *>(
        VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(memory);
    memory[0] = 1;
    Texture texture(desc);
    const ForgetPlacements forget;
    modules::track_placement(&texture, const_cast<std::uint8_t *>(memory));

    // uploaded once when placed, then not until it is written
    CHECK(take_first_bytes() == std::vector{1});
    CHECK(take_first_bytes().empty());
    memory[0] = 2;
    CHECK(take_first_bytes() == std::vector{2});
    CHECK(take_first_bytes().empty());

    // another thread writes it once it has been read, while the upload is being taken (when
    // the taker's reference is added): what was read goes now, and the write next time
    memory[0] = 3;
    const HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr),
                 writing = CreateEventW(nullptr, TRUE, FALSE, nullptr),
                 written = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread writer([&] {
        WaitForSingleObject(go, INFINITE);
        SetEvent(writing);
        memory[0] = 4;
        SetEvent(written);
    });
    texture.referenced = [&] {
        SetEvent(go);
        WaitForSingleObject(writing, 10000);
        // the memory is watched again before it is read, so the write waits for the upload
        // to be taken: no longer than this
        WaitForSingleObject(written, 100);
    };
    // the texture is read out of its tiling after the lock is let go of, so the write may
    // already be there: the upload has the one value or the other, and the next has the write
    const auto first = take_first_bytes();
    CHECK((first == std::vector{3} || first == std::vector{4}));
    texture.referenced = nullptr;
    writer.join();
    CHECK(take_first_bytes() == std::vector{4});
    CHECK(take_first_bytes().empty());
    for (const auto event : {go, writing, written})
        CloseHandle(event);

    // memory the title let go of after writing it: so does the module, with its reference
    memory[0] = 5;
    REQUIRE(VirtualFree(const_cast<std::uint8_t *>(memory), 0, MEM_RELEASE));
    CHECK(take_first_bytes().empty());
    CHECK(texture.references.load() == 1);
}
