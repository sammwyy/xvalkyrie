#include "runtime/modules/placement.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/memory.hpp"
#include "runtime/modules/xg.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <map>
#include <mutex>
#include <numeric>
#include <vector>
#include <windows.h>

namespace xv::runtime::modules {
namespace {
struct Placement {
    ID3D11Resource *resource; // one reference of ours, or null for a Direct3D 12 one
    std::uintptr_t start, end;
    bool dirty;
    UINT tile_mode, pitch;
    ID3D12Resource *d3d12 = nullptr; // one reference of ours
    D3D12_RESOURCE_DESC desc{};
    bool series = false;          // laid out as the Series console's xg_xs.dll says, from `layout`
    D3D12_RESOURCE_DESC layout{}; // the description the title gave, with its swizzle mode
    // its memory is read-only, so that a write is seen; one already writable is not made so
    // again: the title's storage queues write a placement's memory a page at a time
    mutable bool guarded = false;
    std::uint64_t uploaded_at = 0, uploads = 0;
    bool looked = false;   // waiting for its memory, and looked at once since (take_d3d12_uploads)
    bool streamed = false; // written since its upload by the host for the title, not by the title
};
void release(Placement &placement) {
    if (placement.resource)
        placement.resource->Release();
    if (placement.d3d12)
        placement.d3d12->Release();
}
// Taken again by the fault handler when code of ours writes to a placement's memory while
// it holds it
std::recursive_mutex mutex;
std::map<std::uintptr_t, Placement> placements; // by start
std::vector<std::uintptr_t> dirty;              // starts of the ones written since uploaded
std::uintptr_t largest = 0;
PVOID handler = nullptr;

constexpr std::uintptr_t page = 4096;
std::uintptr_t page_down(std::uintptr_t address) { return address & ~(page - 1); }
std::uintptr_t page_up(std::uintptr_t address) { return (address + page - 1) & ~(page - 1); }
// region by region: VirtualProtect fails for a range over more than one allocation, and the
// title's physical memory is mapped as many views
void protect(std::uintptr_t start, std::uintptr_t end, DWORD protection) {
    for (auto at = page_down(start); at < page_up(end);) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof info))
            return;
        const auto region_end = std::min(
            page_up(end), reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
        DWORD previous;
        if (info.State == MEM_COMMIT)
            VirtualProtect(reinterpret_cast<void *>(at), region_end - at, protection, &previous);
        at = region_end;
    }
}
// the placement's memory at every address the title sees it at: a physical page may be mapped
// at several, and a write through any of them is one to the placement
void protect(const Placement &placement, DWORD protection) {
    placement.guarded = protection == PAGE_READONLY;
    protect(placement.start, placement.end, protection);
    for (const auto &[start, end] : other_views(placement.start, placement.end))
        protect(start, end, protection);
}
// memory the host is writing for the title (see TitleWrite), at each address it is seen at:
// its placements are guarded again when it is done
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> writing;
bool being_written(const Placement &placement) {
    const auto first = page_down(placement.start), last = page_up(placement.end);
    return std::any_of(writing.begin(), writing.end(), [&](const auto &range) {
        return range.first < last && range.second > first;
    });
}
void guard(const Placement &placement) {
    if (!being_written(placement))
        protect(placement, PAGE_READONLY);
}
bool writable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
           (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
}

// the first write to a placement's memory since its last upload
LONG CALLBACK written(EXCEPTION_POINTERS *exception) {
    const auto *record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    const std::lock_guard lock(mutex);
    bool ours = false;
    // the ones whose pages hold the address: they start at most `largest` before it
    const auto mark = [&](std::uintptr_t written) {
        for (auto at = placements.upper_bound(page_up(written + 1)); at != placements.begin();) {
            auto &placement = (--at)->second;
            if (page_down(placement.start) + largest + page < written)
                break;
            if (written >= page_down(placement.start) && written < page_up(placement.end)) {
                ours = true;
                if (!placement.dirty)
                    dirty.push_back(placement.start);
                placement.dirty = true;
                placement.streamed = false;
                protect(placement, PAGE_READWRITE);
            }
        }
    };
    mark(address);
    // or a placement's memory seen at another address of its physical page
    if (!ours)
        for (const auto &[start, end] : other_views(address, address + 1))
            mark(start);
    // going on is only right if the write can now happen; otherwise it faults again forever
    if (ours && !writable(address))
        protect(address, address + 1, PAGE_READWRITE);
    return ours && writable(address) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

UINT row_bytes(UINT width, DXGI_FORMAT format) {
    const auto size = format_size(static_cast<std::uint32_t>(format));
    width = std::max(width, 1u);
    return size.blocks ? (width + 3) / 4 * size.bytes : width * size.bytes;
}
UINT rows(UINT height, DXGI_FORMAT format) {
    height = std::max(height, 1u);
    return format_size(static_cast<std::uint32_t>(format)).blocks ? (height + 3) / 4 : height;
}
// the subresources of a 2D texture in XG's linear layout: per array slice, mips in order
std::size_t texture_layout(const D3D11_TEXTURE2D_DESC &desc, std::uintptr_t address,
                           D3D11_SUBRESOURCE_DATA *data, UINT count) {
    const UINT mips = std::max(desc.MipLevels, 1u);
    std::size_t offset = 0;
    for (UINT slice = 0; slice < std::max(desc.ArraySize, 1u); ++slice)
        for (UINT mip = 0; mip < mips; ++mip) {
            const auto pitch = row_bytes(desc.Width >> mip, desc.Format);
            const auto bytes = std::size_t{pitch} * rows(desc.Height >> mip, desc.Format);
            if (const auto index = slice * mips + mip; data && index < count)
                data[index] = {reinterpret_cast<const void *>(address + offset), pitch,
                               static_cast<UINT>(bytes)};
            offset += bytes;
        }
    return offset;
}
std::size_t resource_bytes(ID3D11Resource *resource) {
    D3D11_RESOURCE_DIMENSION dimension;
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc;
        static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
        return desc.ByteWidth;
    }
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        return texture_layout(desc, 0, nullptr, 0);
    }
    return 0;
}
bool readable(std::uintptr_t start, std::size_t bytes) {
    for (auto at = page_down(start); at < start + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}
// XG_TEXTURE2D_DESC and the two XGTextureAddressComputer vtables of the console's xg_x.dll:
// CopyFromSubresource is at 6 in the older one, at 9 in the newer, which has the resource
// size at 3 where the older has the offset of a mip (0 for the first)
struct XgTexture2D {
    UINT width, height, mips, array_size, format, sample_count, sample_quality, usage, bind_flags,
        cpu_access, misc_flags, esram_offset, esram_usage, tile_mode, pitch;
};
using CreateComputer = HRESULT(WINAPI *)(const XgTexture2D *, void **);
using Copy = HRESULT(WINAPI *)(void *, void *, UINT, UINT, const void *, UINT, UINT);
CreateComputer console_xg() {
    static const auto create = reinterpret_cast<CreateComputer>(
        detail::system_export("xg_x.dll", "XGCreateTexture2DComputer"));
    return create;
}
// The console's computer for a texture. The first kits' description has one word about
// ESRAM where later ones have two, so the tile mode and the pitch are a word earlier there:
// which it is shows in whether the library takes the description.
HRESULT make_computer(const XgTexture2D &xg, void **computer) {
    const auto create = console_xg();
    if (!create)
        return E_NOTIMPL;
    static std::atomic<int> layout{0}; // 1 the later one, 2 the first kits', 0 not known yet
    auto earlier = xg;
    earlier.esram_usage = xg.tile_mode;
    earlier.tile_mode = xg.pitch;
    auto made = create(layout == 2 ? &earlier : &xg, computer);
    if (SUCCEEDED(made) && layout == 0)
        layout = 1;
    else if (FAILED(made) && layout == 0 && SUCCEEDED(made = create(&earlier, computer)))
        layout = 2;
    return made;
}
bool is_code(const void *address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof info) && info.State == MEM_COMMIT &&
           (info.Protect &
            (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
// One subresource out of its tiling, row after row. Later computers copy it themselves. The
// first kits' one ends with the method that says where an element (a pixel, or a block of
// a compressed format) is: the elements of a row lie together eight at a time, the width of
// the smallest tile. SEH only, no objects to unwind here.
bool copy_out(void *computer, void *destination, UINT mip, UINT slice, UINT mips,
              const void *source, UINT row_pitch, UINT slice_pitch, UINT element) {
    using Where = std::int64_t(WINAPI *)(void *, UINT, UINT, UINT64, UINT, UINT, UINT);
    auto *const vtable = *static_cast<void ***>(computer);
    __try {
        if (is_code(vtable[6])) {
            const auto first =
                reinterpret_cast<std::uint64_t(WINAPI *)(void *, UINT, UINT)>(vtable[3]);
            const auto copy = reinterpret_cast<Copy>(vtable[first(computer, 0, 0) ? 9 : 6]);
            return SUCCEEDED(
                copy(computer, destination, 0, slice * mips + mip, source, row_pitch, slice_pitch));
        }
        const auto where = reinterpret_cast<Where>(vtable[4]);
        const UINT width = row_pitch / element, height = slice_pitch / row_pitch;
        const auto start = where(computer, 0, mip, 0, 0, slice, 0);
        const UINT run =
            width >= 8 && where(computer, 0, mip, 7, 0, slice, 0) == start + 7 * element ? 8 : 1;
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; x += run) {
                const auto at = where(computer, 0, mip, x, y, slice, 0);
                if (at < 0)
                    return false;
                std::memcpy(static_cast<std::uint8_t *>(destination) + std::size_t{y} * row_pitch +
                                std::size_t{x} * element,
                            static_cast<const std::uint8_t *>(source) + at,
                            std::size_t{std::min(run, width - x)} * element);
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool detile(ID3D11DeviceContext *context, ID3D11Texture2D *texture,
            const D3D11_TEXTURE2D_DESC &desc, const Placement &placement) {
    const XgTexture2D xg{desc.Width,
                         desc.Height,
                         desc.MipLevels,
                         desc.ArraySize,
                         static_cast<UINT>(desc.Format),
                         desc.SampleDesc.Count,
                         desc.SampleDesc.Quality,
                         static_cast<UINT>(desc.Usage),
                         desc.BindFlags,
                         desc.CPUAccessFlags,
                         desc.MiscFlags,
                         0,
                         0,
                         placement.tile_mode,
                         placement.pitch};
    void *computer = nullptr;
    if (const auto made = make_computer(xg, &computer); FAILED(made) || !computer) {
        if (static std::atomic<bool> told{false}; made != E_NOTIMPL && !told.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] the console's XG makes nothing of %ux%u mips %u array %u "
                         "format %u tile mode %u pitch %u: 0x%08lx\n",
                         xg.width, xg.height, xg.mips, xg.array_size, xg.format, xg.tile_mode,
                         xg.pitch, static_cast<unsigned long>(made));
        return false;
    }
    auto *const vtable = *static_cast<void ***>(computer);
    const UINT mips = std::max(desc.MipLevels, 1u);
    const UINT element = format_size(static_cast<std::uint32_t>(desc.Format)).bytes;
    std::vector<std::byte> linear;
    bool ok = true;
    for (UINT slice = 0; slice < std::max(desc.ArraySize, 1u) && ok; ++slice)
        for (UINT mip = 0; mip < mips && ok; ++mip) {
            const auto pitch = row_bytes(desc.Width >> mip, desc.Format);
            const auto bytes = pitch * rows(desc.Height >> mip, desc.Format);
            linear.resize(bytes);
            const auto index = slice * mips + mip;
            ok = copy_out(computer, linear.data(), mip, slice, mips,
                          reinterpret_cast<const void *>(placement.start), pitch, bytes, element);
            if (ok)
                context->UpdateSubresource(texture, index, nullptr, linear.data(), pitch, bytes);
        }
    reinterpret_cast<ULONG(WINAPI *)(void *)>(vtable[1])(computer);
    return ok;
}
void upload(ID3D11DeviceContext *context, const Placement &placement) {
    auto *const resource = placement.resource;
    D3D11_RESOURCE_DIMENSION dimension;
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc;
        static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
        if (desc.Usage == D3D11_USAGE_DYNAMIC) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(resource, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, reinterpret_cast<const void *>(placement.start),
                            desc.ByteWidth);
                context->Unmap(resource, 0);
            }
        } else if (desc.Usage == D3D11_USAGE_DEFAULT) {
            context->UpdateSubresource(resource, 0, nullptr,
                                       reinterpret_cast<const void *>(placement.start), 0, 0);
        }
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
    if (desc.Usage != D3D11_USAGE_DEFAULT || desc.SampleDesc.Count > 1)
        return;
    if (placement.tile_mode > 1) {
        static std::atomic<bool> reported{false};
        if (!detile(context, static_cast<ID3D11Texture2D *>(resource), desc, placement) &&
            !reported.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] a texture in tile mode %u, and no xg_x.dll of the console "
                         "to read it: it stays as it is\n",
                         placement.tile_mode);
        return;
    }
    std::vector<D3D11_SUBRESOURCE_DATA> data(std::size_t{std::max(desc.MipLevels, 1u)} *
                                             std::max(desc.ArraySize, 1u));
    texture_layout(desc, placement.start, data.data(), static_cast<UINT>(data.size()));
    for (UINT index = 0; index < data.size(); ++index)
        context->UpdateSubresource(resource, index, nullptr, data[index].pSysMem,
                                   data[index].SysMemPitch, data[index].SysMemSlicePitch);
}
} // namespace

void track_placement(ID3D11Resource *resource, const void *address, UINT tile_mode, UINT pitch) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto bytes = resource ? resource_bytes(resource) : 0;
    if (!address || !bytes || !readable(start, bytes))
        return;
    const std::lock_guard lock(mutex);
    if (!handler)
        handler = AddVectoredExceptionHandler(1, &written);
    // a resource placed where an older one was replaces it
    if (const auto old = placements.find(start); old != placements.end()) {
        protect(old->second, PAGE_READWRITE); // the new one may not cover all of it
        release(old->second);
        placements.erase(old);
    }
    resource->AddRef();
    largest = std::max<std::uintptr_t>(largest, bytes);
    // a tiled texture was created from its memory as if it were linear: read it properly
    // before its first use
    const auto &placement =
        placements[start] = {resource, start, start + bytes, tile_mode > 1, tile_mode, pitch};
    if (placement.dirty)
        dirty.push_back(start);
    guard(placement);
}

bool placement_data(const D3D11_TEXTURE2D_DESC &desc, const void *address,
                    D3D11_SUBRESOURCE_DATA *data, UINT count) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    return address && desc.SampleDesc.Count <= 1 &&
           readable(start, texture_layout(desc, start, data, count));
}

namespace {
// [address, address + bytes) and where else its physical pages are seen
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> seen_at(const void *address,
                                                               std::size_t bytes) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    auto ranges = other_views(start, start + bytes);
    ranges.emplace_back(start, start + bytes);
    return ranges;
}
// the placements in those ranges, dirty and unguarded
void unguard(const std::vector<std::pair<std::uintptr_t, std::uintptr_t>> &ranges) {
    for (const auto &[first, last] : ranges)
        for (auto at = placements.lower_bound(last); at != placements.begin();) {
            auto &placement = (--at)->second;
            if (placement.start + largest < first)
                break;
            if (placement.end <= first || placement.start >= last)
                continue;
            if (!placement.dirty)
                dirty.push_back(placement.start), placement.streamed = true;
            placement.dirty = true;
            if (placement.guarded)
                protect(placement, PAGE_READWRITE);
        }
}
} // namespace

void prepare_title_write(const void *address, std::size_t bytes) {
    if (!address || !bytes)
        return;
    const auto ranges = seen_at(address, bytes);
    const std::lock_guard lock(mutex);
    unguard(ranges);
}
TitleWrite::TitleWrite(const void *address, std::size_t bytes) {
    if (!address || !bytes)
        return;
    ranges_ = seen_at(address, bytes);
    const std::lock_guard lock(mutex);
    writing.insert(writing.end(), ranges_.begin(), ranges_.end());
    unguard(ranges_);
}
TitleWrite::~TitleWrite() {
    if (ranges_.empty())
        return;
    const std::lock_guard lock(mutex);
    for (const auto &range : ranges_)
        if (const auto found = std::find(writing.begin(), writing.end(), range);
            found != writing.end())
            writing.erase(found);
    // uploaded from what was written, all of it
    unguard(ranges_);
}

bool readable_memory(std::uint64_t address, std::size_t bytes) {
    return readable(static_cast<std::uintptr_t>(address), bytes);
}

void upload_placements(ID3D11DeviceContext *context) {
    const std::lock_guard lock(mutex);
    std::vector<std::uintptr_t> kept;
    for (const auto start : dirty) {
        const auto found = placements.find(start);
        if (found == placements.end() || !found->second.dirty)
            continue;
        // uploaded by whoever submits Direct3D 12 work, or when the host is done writing it
        if (found->second.d3d12 || being_written(found->second)) {
            kept.push_back(start);
            continue;
        }
        // memory the title has let go of: what was in it is all the resource gets
        if (!readable(found->second.start, found->second.end - found->second.start)) {
            release(found->second);
            placements.erase(found);
            continue;
        }
        // watched again before it is read: a write while it is read is not missed, it makes
        // the placement dirty for the next upload
        found->second.dirty = false;
        guard(found->second);
        upload(context, found->second);
    }
    dirty = std::move(kept);
}
} // namespace xv::runtime::modules

namespace xv::runtime::modules {
namespace {
// XGComputeOptimalTileMode(dimension, format, width, height, array size, samples, bind flags,
// misc flags), by its code in the console's xg_x.dll: the tiling the console gives a texture
// whose description does not name one
using OptimalTileMode = UINT(WINAPI *)(UINT, UINT, UINT, UINT, UINT, UINT, UINT, UINT);
constexpr UINT bind_shader_resource = 8;
XgTexture2D xg_desc(const D3D12_RESOURCE_DESC &desc, UINT tile_mode) {
    return {static_cast<UINT>(desc.Width),
            desc.Height,
            desc.MipLevels,
            desc.DepthOrArraySize,
            static_cast<UINT>(desc.Format),
            desc.SampleDesc.Count,
            desc.SampleDesc.Quality,
            0,
            bind_shader_resource,
            0,
            0,
            0,
            0,
            tile_mode,
            0};
}
// The Series console's XG: a computer made from the resource description the title gave
// (XGCreateTextureComputer), whose methods are AddRef, Release, GetResourceLayout,
// GetResourceSizeBytes (3), ..., CopyIntoSubresource (8), CopyFromSubresource (9)
using CreateSeriesComputer = HRESULT(WINAPI *)(const D3D12_RESOURCE_DESC *, void **);
using SeriesCopy = HRESULT(WINAPI *)(void *, void *, UINT, UINT, UINT, const void *, UINT, UINT);
CreateSeriesComputer series_xg() {
    static const auto create = reinterpret_cast<CreateSeriesComputer>(
        detail::system_export("xg_xs.dll", "XGCreateTextureComputer"));
    return create;
}
bool series_copy_out(SeriesCopy copy, void *computer, void *destination, UINT subresource,
                     const void *source, UINT row_pitch, UINT slice_pitch) {
    __try {
        return SUCCEEDED(
            copy(computer, destination, 0, subresource, 0, source, row_pitch, slice_pitch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// the console's XG where the title's system folder has it, the layouts of ours otherwise
void *series_computer(const D3D12_RESOURCE_DESC &layout) {
    void *computer = nullptr;
    const auto create = series_xg();
    if (!create)
        return series_texture_computer(&layout);
    __try {
        if (!create || FAILED(create(&layout, &computer)))
            computer = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        computer = nullptr;
    }
    return computer;
}
std::uint64_t series_size(void *computer) {
    __try {
        return reinterpret_cast<std::uint64_t(WINAPI *)(void *)>(
            (*static_cast<void ***>(computer))[3])(computer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void series_release(void *computer) {
    reinterpret_cast<ULONG(WINAPI *)(void *)>((*static_cast<void ***>(computer))[1])(computer);
}
bool series_subresources(const Placement &placement, PlacementUpload &upload) {
    const auto &desc = placement.desc;
    auto *const computer = series_computer(placement.layout);
    if (!computer)
        return false;
    const auto copy = reinterpret_cast<SeriesCopy>((*static_cast<void ***>(computer))[9]);
    const UINT mips = std::max<UINT>(desc.MipLevels, 1);
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const UINT slices = volume ? 1 : std::max<UINT>(desc.DepthOrArraySize, 1);
    bool ok = true;
    for (UINT slice = 0; slice < slices && ok; ++slice)
        for (UINT mip = 0; mip < mips && ok; ++mip) {
            const auto pitch = row_bytes(static_cast<UINT>(desc.Width >> mip), desc.Format);
            const auto plane = std::size_t{pitch} * rows(desc.Height >> mip, desc.Format);
            const auto depth = volume ? std::max<UINT>(desc.DepthOrArraySize >> mip, 1) : 1;
            std::vector<std::byte> linear(plane * depth);
            ok = series_copy_out(copy, computer, linear.data(), slice * mips + mip,
                                 reinterpret_cast<const void *>(placement.start), pitch,
                                 static_cast<UINT>(plane));
            upload.subresources.push_back(std::move(linear));
            upload.pitches.push_back(pitch);
        }
    series_release(computer);
    return ok;
}
// what a texture's subresources hold, linear: through xg_x.dll for a tiling, as XG's linear
// layout otherwise
bool linear_subresources(const Placement &placement, PlacementUpload &upload) {
    if (placement.series)
        return series_subresources(placement, upload);
    const auto &desc = placement.desc;
    const UINT mips = std::max<UINT>(desc.MipLevels, 1),
               slices = std::max<UINT>(desc.DepthOrArraySize, 1);
    void *computer = nullptr;
    if (placement.tile_mode > 1 &&
        (FAILED(make_computer(xg_desc(desc, placement.tile_mode), &computer)) || !computer))
        return false;
    const UINT element = format_size(static_cast<std::uint32_t>(desc.Format)).bytes;
    bool ok = true;
    std::size_t offset = 0;
    for (UINT slice = 0; slice < slices && ok; ++slice)
        for (UINT mip = 0; mip < mips && ok; ++mip) {
            const auto pitch = row_bytes(static_cast<UINT>(desc.Width >> mip), desc.Format);
            const auto bytes = std::size_t{pitch} * rows(desc.Height >> mip, desc.Format);
            std::vector<std::byte> linear(bytes);
            if (computer)
                ok = copy_out(computer, linear.data(), mip, slice, mips,
                              reinterpret_cast<const void *>(placement.start), pitch,
                              static_cast<UINT>(bytes), element);
            else
                std::memcpy(linear.data(), reinterpret_cast<const void *>(placement.start + offset),
                            bytes);
            offset += bytes;
            upload.subresources.push_back(std::move(linear));
            upload.pitches.push_back(pitch);
        }
    if (computer)
        reinterpret_cast<ULONG(WINAPI *)(void *)>((*static_cast<void ***>(computer))[1])(computer);
    return ok;
}
// how many bytes of the title's memory a texture takes in its tiling
std::size_t placed_bytes(const D3D12_RESOURCE_DESC &desc, UINT tile_mode) {
    std::size_t linear = 0;
    const UINT mips = std::max<UINT>(desc.MipLevels, 1);
    for (UINT slice = 0; slice < std::max<UINT>(desc.DepthOrArraySize, 1); ++slice)
        for (UINT mip = 0; mip < mips; ++mip)
            linear += std::size_t{row_bytes(static_cast<UINT>(desc.Width >> mip), desc.Format)} *
                      rows(desc.Height >> mip, desc.Format);
    if (tile_mode <= 1)
        return linear;
    // a tiling pads to its tiles: the size XG computes for it, where the console's xg_x.dll
    // is the newer one that says (its first method after Release), the linear one otherwise
    void *computer = nullptr;
    if (FAILED(make_computer(xg_desc(desc, tile_mode), &computer)) || !computer)
        return linear;
    auto *const vtable = *static_cast<void ***>(computer);
    const auto size =
        reinterpret_cast<std::uint64_t(WINAPI *)(void *, UINT, UINT)>(vtable[3])(computer, 0, 0);
    reinterpret_cast<ULONG(WINAPI *)(void *)>(vtable[1])(computer);
    return size ? static_cast<std::size_t>(size) : linear;
}
} // namespace

std::vector<Placement> pending; // placed where there is no memory yet
// XV_D3D_TRACE, read once: placements are made all the time
bool tracing() {
    static const bool on = std::getenv("XV_D3D_TRACE") != nullptr;
    return on;
}
void track_placement(ID3D12Resource *resource, const void *address,
                     const D3D12_RESOURCE_DESC *layout, DXGI_FORMAT format) {
    if (!resource || !address)
        return;
    auto desc = resource->GetDesc();
    // the texels are laid out in the title's memory as its format has them
    if (format != DXGI_FORMAT_UNKNOWN)
        desc.Format = format;
    // a Series title: the console's XG says how the texture lies in its memory, the one of
    // ours where the title's system folder has none (for a layout that names a swizzle mode)
    void *const own = layout && !series_xg() ? series_texture_computer(layout) : nullptr;
    if (own)
        series_release(own);
    if (layout && (series_xg() || own) && desc.SampleDesc.Count == 1 &&
        (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
         desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
        auto *const computer = series_computer(*layout);
        const auto bytes = computer ? static_cast<std::size_t>(series_size(computer)) : 0;
        if (computer)
            series_release(computer);
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        if (static std::atomic<int> shown{0}; tracing() && shown++ < 5)
            std::fprintf(stderr, "[Runtime] placement texture at %llx, layout 0x%x, %zu bytes%s\n",
                         static_cast<unsigned long long>(start), layout->Layout, bytes,
                         bytes && readable(start, bytes) ? "" : ", not readable");
        if (!bytes)
            return;
        const std::lock_guard lock(mutex);
        // a texture placed where one waited is in its place
        std::erase_if(pending, [&](Placement &waiting) {
            if (waiting.start != start)
                return false;
            release(waiting);
            return true;
        });
        // memory the title has not committed yet: looked at again whenever work is
        // submitted, and followed from when it is there
        if (!readable(start, bytes)) {
            resource->AddRef();
            Placement waiting{nullptr, start, start + bytes, true, 0, 0};
            waiting.d3d12 = resource;
            waiting.desc = desc;
            waiting.series = true;
            waiting.layout = *layout;
            pending.push_back(waiting);
            return;
        }
        if (!handler)
            handler = AddVectoredExceptionHandler(1, &written);
        if (const auto old = placements.find(start); old != placements.end()) {
            protect(old->second, PAGE_READWRITE);
            release(old->second);
            placements.erase(old);
        }
        resource->AddRef();
        largest = std::max<std::uintptr_t>(largest, bytes);
        auto &placement = placements[start] = {nullptr, start, start + bytes, true, 0, 0};
        placement.d3d12 = resource;
        placement.desc = desc;
        placement.series = true;
        placement.layout = *layout;
        dirty.push_back(start);
        guard(placement);
        return;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count > 1)
        return;
    static const auto optimal = reinterpret_cast<OptimalTileMode>(
        detail::system_export("xg_x.dll", "XGComputeOptimalTileMode"));
    const UINT tile_mode =
        optimal
            ? optimal(3, static_cast<UINT>(desc.Format), static_cast<UINT>(desc.Width), desc.Height,
                      desc.DepthOrArraySize, desc.SampleDesc.Count, bind_shader_resource, 0)
            : 0;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto bytes = placed_bytes(desc, tile_mode);
    if (static std::atomic<int> shown{0}; tracing() && shown++ < 5)
        std::fprintf(stderr, "[Runtime] placement texture at %llx, tile mode %u, %zu bytes%s\n",
                     static_cast<unsigned long long>(start), tile_mode, bytes,
                     readable(start, bytes) ? "" : ", not all readable");
    if (!readable(start, bytes))
        return;
    const std::lock_guard lock(mutex);
    if (!handler)
        handler = AddVectoredExceptionHandler(1, &written);
    if (const auto old = placements.find(start); old != placements.end()) {
        protect(old->second, PAGE_READWRITE);
        release(old->second);
        placements.erase(old);
    }
    resource->AddRef();
    largest = std::max<std::uintptr_t>(largest, bytes);
    auto &placement = placements[start] = {nullptr, start, start + bytes, true, tile_mode, 0};
    placement.d3d12 = resource;
    placement.desc = desc;
    dirty.push_back(start);
    guard(placement);
}

namespace {
// A Direct3D 12 placement whose resource nothing but this has a reference to, and whose memory
// is gone or another placement's: the title has let go of it, and keeping it would keep the
// host's memory for it for as long as the title runs. While its memory is there, a view the
// title made of it may still be read, which on the console is of the memory.
bool let_go_of(const Placement &placement, bool taken) {
    if (!placement.d3d12)
        return false;
    placement.d3d12->AddRef();
    return placement.d3d12->Release() == 1 &&
           (taken || !readable(placement.start, placement.end - placement.start));
}
} // namespace

// The placements to upload are taken under the lock, and read out of their tiling after it,
// side by side: that takes long for many large textures, and the threads that write the title's
// memory (a title's storage queues) would all wait on it meanwhile.
std::vector<Placement> take_d3d12_placements();
std::vector<PlacementUpload> take_d3d12_uploads() {
    std::vector<PlacementUpload> uploads;
    auto taken = take_d3d12_placements();
    std::vector<PlacementUpload> read(taken.size());
    std::vector<char> ok(taken.size());
    std::vector<std::size_t> order(taken.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::for_each(std::execution::par, order.begin(), order.end(), [&](std::size_t n) {
        read[n] = {taken[n].d3d12, {}, {}, taken[n].desc.Format};
        ok[n] = linear_subresources(taken[n], read[n]);
    });
    for (std::size_t n = 0; n < read.size(); ++n)
        if (ok[n])
            uploads.push_back(std::move(read[n]));
        else
            taken[n].d3d12->Release();
    return uploads;
}
std::vector<Placement> take_d3d12_placements() {
    std::vector<Placement> uploads;
    const std::lock_guard lock(mutex);
    // every so many submissions, the placements the title has let go of
    if (static unsigned submissions = 0; ++submissions % 64 == 0) {
        std::uintptr_t before = 0; // the end of those before
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> freed;
        for (auto at = placements.begin(); at != placements.end();) {
            const auto next = std::next(at);
            const bool taken = before > at->second.start ||
                               (next != placements.end() && next->first < at->second.end);
            before = std::max(before, at->second.end);
            if (!let_go_of(at->second, taken)) {
                at = next;
                continue;
            }
            protect(at->second, PAGE_READWRITE);
            if (taken)
                freed.emplace_back(at->second.start, at->second.end);
            release(at->second);
            at = placements.erase(at);
        }
        // memory another placement has is guarded again for it, unless written since
        for (const auto &[first, last] : freed)
            for (auto at = placements.lower_bound(last); at != placements.begin();) {
                auto &placement = (--at)->second;
                if (placement.start + largest < first)
                    break;
                if (placement.end > first && !placement.dirty)
                    guard(placement);
            }
        std::erase_if(pending, [](Placement &waiting) {
            if (!let_go_of(waiting, false))
                return false;
            release(waiting);
            return true;
        });
    }
    // Those waiting for memory are looked at again where the title has mapped some since, and
    // once when they are new: there are thousands, and a look at memory is a call to the host.
    auto mapped = take_newly_mapped();
    std::sort(mapped.begin(), mapped.end());
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> merged;
    for (const auto &range : mapped)
        if (!merged.empty() && range.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, range.second);
        else
            merged.push_back(range);
    const auto touched = [&](const Placement &waiting) {
        const auto after = std::upper_bound(merged.begin(), merged.end(),
                                            std::pair{waiting.start, ~std::uintptr_t{0}});
        return (after != merged.begin() && std::prev(after)->second > waiting.start) ||
               (after != merged.end() && after->first < waiting.end);
    };
    std::erase_if(pending, [&](Placement &waiting) {
        const bool look = !waiting.looked || touched(waiting);
        waiting.looked = true;
        if (!look || !readable(waiting.start, waiting.end - waiting.start))
            return false;
        if (!handler)
            handler = AddVectoredExceptionHandler(1, &written);
        if (const auto old = placements.find(waiting.start); old != placements.end()) {
            protect(old->second, PAGE_READWRITE);
            release(old->second);
            placements.erase(old);
        }
        largest = std::max<std::uintptr_t>(largest, waiting.end - waiting.start);
        placements[waiting.start] = waiting;
        dirty.push_back(waiting.start);
        return true;
    });
    std::vector<std::uintptr_t> kept;
    for (const auto start : dirty) {
        const auto found = placements.find(start);
        if (found == placements.end() || !found->second.dirty)
            continue;
        auto &placement = found->second;
        // One uploaded a moment ago waits a little: the title's storage queues write a texture
        // a piece at a time over several submissions, and each upload is of all of it.
        // What the title writes itself (a video's frames) is not held back.
        constexpr std::uint64_t again_after = 250; // ms
        if (!placement.d3d12 || being_written(placement) ||
            (placement.streamed && placement.uploads &&
             GetTickCount64() - placement.uploaded_at < again_after)) {
            kept.push_back(start);
            continue;
        }
        // memory whose pages the title has unmapped: a texture of a heap it maps pages into
        // waits for them again, one of memory it has let go of is done with
        if (!readable(placement.start, placement.end - placement.start)) {
            if (placement.series)
                pending.push_back(placement);
            else
                release(placement);
            placements.erase(found);
            continue;
        }
        // watched again before it is read: a write while it is read is not missed, it makes
        // the placement dirty for the next upload
        placement.dirty = false;
        guard(placement);
        ++placement.uploads;
        placement.uploaded_at = GetTickCount64();
        placement.d3d12->AddRef();
        uploads.push_back(placement);
    }
    dirty = std::move(kept);
    return uploads;
}
void forget_placements() {
    const std::lock_guard lock(mutex);
    for (auto &[start, placement] : placements) {
        protect(placement, PAGE_READWRITE);
        release(placement);
    }
    placements.clear();
    for (auto &waiting : pending)
        release(waiting);
    pending.clear();
    dirty.clear();
    largest = 0;
    if (handler)
        RemoveVectoredExceptionHandler(handler);
    handler = nullptr;
}
} // namespace xv::runtime::modules
