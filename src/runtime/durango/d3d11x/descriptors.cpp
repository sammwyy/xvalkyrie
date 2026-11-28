#include "runtime/durango/d3d11x/descriptors.hpp"
#include "runtime/modules/placement.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <d3d11_1.h>
#include <map>
#include <mutex>
#include <tuple>

// Descriptor fields, as titles read them: a buffer's V# (48-bit address, size) at 0x20, a
// shader resource view's T# at 0x30 and a sampler's S# at 0x20, which here hold the id of
// the host object and a magic. Buffers get an address of their own when the title's memory
// is not theirs, so that what a packet points into names the buffer and the offset in it.
// Registers: textures from 0x6b40 in 0x20 steps, samplers from 0x7b40 in 0x10, vertex streams
// from 0x7d70 in 0x10, constant buffers from 0x1c40 (vertex shader) and 0x6a00 (pixel shader).
namespace xv::runtime::durango::d3d11x {
namespace {
constexpr std::uint32_t magic = 0xd3d1face, resource_magic = 0xd3d1fac0,
                        unordered_magic = 0xd3d1fac1;
constexpr std::uint32_t textures = 0x6b40, samplers = 0x7b40, vertices = 0x7d70,
                        vertex_constants = 0x1c40, pixel_constants = 0x6a00;
constexpr unsigned slots = 16;

std::mutex mutex;
std::map<std::uint32_t, IUnknown *> by_id; // host objects, not referenced
std::atomic<std::uint32_t> next_id{1};
struct Range {
    std::uintptr_t end;
    ID3D11Buffer *buffer;
};
std::map<std::uintptr_t, Range> buffers; // by start address
// what was said of each host object, to take back when it goes
std::multimap<IUnknown *, std::uint32_t> ids_of;
std::multimap<IUnknown *, std::uintptr_t> addresses_of;
std::atomic<std::uintptr_t> next_address{0x7e0000000000};

void put_id(void *object, std::size_t offset, IUnknown *host, std::uint32_t kind = magic,
            IUnknown *owner = nullptr) {
    const auto id = next_id++;
    auto *const at = static_cast<std::byte *>(object) + offset;
    std::memcpy(at, &id, 4);
    std::memcpy(at + 4, &kind, 4);
    const std::lock_guard lock(mutex);
    by_id[id] = host;
    ids_of.emplace(owner ? owner : host, id);
}
void put_buffer(void *object, ID3D11Buffer *host, std::uintptr_t address,
                IUnknown *owner = nullptr) {
    D3D11_BUFFER_DESC desc;
    host->GetDesc(&desc);
    auto *const at = static_cast<std::byte *>(object) + 0x20;
    std::memset(at, 0, 0x20);
    const std::uint64_t base = address & 0xffffffffffffull;
    std::memcpy(at, &base, 8);
    std::memcpy(at + 8, &desc.ByteWidth, 4);
    const std::lock_guard lock(mutex);
    buffers[address] = {address + desc.ByteWidth, host};
    addresses_of.emplace(owner ? owner : host, address);
}
template <class T> T *object_of(const std::uint32_t *descriptor, std::uint32_t kind = magic) {
    if (descriptor[1] != kind)
        return nullptr;
    const std::lock_guard lock(mutex);
    const auto found = by_id.find(descriptor[0]);
    return found == by_id.end() ? nullptr : static_cast<T *>(found->second);
}
// the buffer a V# points into, and how far into it
ID3D11Buffer *buffer_at(const std::uint32_t *descriptor, UINT &offset) {
    const auto address = (std::uintptr_t{descriptor[1] & 0xffff} << 32) | descriptor[0];
    const std::lock_guard lock(mutex);
    auto found = buffers.upper_bound(address);
    if (found == buffers.begin())
        return nullptr;
    --found;
    if (address >= found->second.end)
        return nullptr;
    offset = static_cast<UINT>(address - found->first);
    return found->second.buffer;
}

struct Register {
    std::array<std::uint32_t, 8> words{};
    bool set = false;
};
struct Stream {
    std::map<std::uint32_t, Register> registers;
    const std::byte *start = nullptr, *parsed = nullptr;
};
std::mutex streams_mutex;
std::map<ID3D11DeviceContext *, Stream> streams;

void read(Stream &stream, const std::byte *start, const std::byte *end) {
    if (stream.start != start || stream.parsed < start || stream.parsed > end) {
        stream.start = start;
        stream.parsed = start;
    }
    auto *at = stream.parsed;
    while (at + 4 <= end) {
        std::uint32_t header;
        std::memcpy(&header, at, 4);
        const auto count = (header >> 16) + 1;
        if (count > 8) { // not a descriptor packet: nothing further is either
            at = end;
            break;
        }
        if (at + 4 + 4 * count > end)
            break;
        auto &target = stream.registers[header & 0xffff];
        target.words = {};
        std::memcpy(target.words.data(), at + 4, 4 * count);
        target.set = true;
        at += 4 + 4 * count;
    }
    stream.parsed = at;
}
const std::uint32_t *find(const Stream &stream, std::uint32_t reg) {
    const auto found = stream.registers.find(reg);
    return found == stream.registers.end() || !found->second.set ? nullptr
                                                                 : found->second.words.data();
}
std::map<std::tuple<ID3D11DeviceContext *, bool, UINT>, std::pair<ID3D11Buffer *, UINT>>
    stream_slice_cbs;

void constants(ID3D11DeviceContext *context, const Stream &stream, std::uint32_t base, bool pixel) {
    ID3D11DeviceContext1 *latest = nullptr;
    context->QueryInterface(&latest);
    for (UINT slot = 0; slot < slots; ++slot) {
        const auto *descriptor = find(stream, base + slot * 0x10);
        if (!descriptor)
            continue;
        const auto address = (std::uintptr_t{descriptor[1] & 0xffff} << 32) | descriptor[0];
        UINT offset = 0;
        auto *const buffer = buffer_at(descriptor, offset);
        if (buffer && offset % 256 == 0) {
            if (latest && offset) {
                D3D11_BUFFER_DESC desc;
                buffer->GetDesc(&desc);
                const UINT first = offset / 16;
                const UINT count =
                    std::max(16u, (std::min(descriptor[2] ? descriptor[2] : desc.ByteWidth,
                                            desc.ByteWidth - offset) +
                                   255) /
                                      256 * 16);
                if (pixel)
                    latest->PSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
                else
                    latest->VSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
            } else if (pixel) {
                context->PSSetConstantBuffers(slot, 1, &buffer);
            } else {
                context->VSSetConstantBuffers(slot, 1, &buffer);
            }
            continue;
        }
        const UINT raw_bytes = descriptor[2] ? std::min(descriptor[2], 65536u) : 4096u;
        if (!address || !modules::readable_memory(address, 16))
            continue;
        UINT copy_len = raw_bytes;
        while (copy_len > 16 && !modules::readable_memory(address, copy_len))
            copy_len /= 2;
        const UINT aligned = (std::max(copy_len, 256u) + 255u) & ~255u;
        auto &entry = stream_slice_cbs[{context, pixel, slot}];
        if (!entry.first || entry.second < aligned) {
            if (entry.first)
                entry.first->Release();
            entry = {nullptr, 0};
            ID3D11Device *dev = nullptr;
            context->GetDevice(&dev);
            const D3D11_BUFFER_DESC bd{
                aligned, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0,
                0};
            if (SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &entry.first)))
                entry.second = aligned;
            dev->Release();
        }
        if (auto *slice = entry.first) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(slice, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memset(mapped.pData, 0, aligned);
                std::memcpy(mapped.pData, reinterpret_cast<const void *>(address), copy_len);
                context->Unmap(slice, 0);
            }
            if (pixel)
                context->PSSetConstantBuffers(slot, 1, &slice);
            else
                context->VSSetConstantBuffers(slot, 1, &slice);
        }
    }
    if (latest)
        latest->Release();
}
} // namespace

ID3D11ShaderResourceView *described_view(const std::uint32_t *descriptor) {
    return object_of<ID3D11ShaderResourceView>(descriptor);
}
ID3D11SamplerState *described_sampler(const std::uint32_t *descriptor) {
    return object_of<ID3D11SamplerState>(descriptor);
}
ID3D11Buffer *described_buffer(const std::uint32_t *descriptor, UINT &offset) {
    return buffer_at(descriptor, offset);
}

void describe_resource(std::uint32_t *descriptor, const void *object, ID3D11Resource *host) {
    std::memset(descriptor, 0, 640);
    put_id(descriptor, 0, host, resource_magic);
    // a buffer's address and size go with it: its views are made of those
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    host->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
        std::memcpy(descriptor + 2, static_cast<const std::byte *>(object) + 0x20, 16);
}
ID3D11Resource *described_resource(const std::uint32_t *descriptor) {
    return object_of<ID3D11Resource>(descriptor, resource_magic);
}
void describe_composed(std::uint32_t *descriptor, ID3D11ShaderResourceView *host) {
    std::memset(descriptor, 0, 32);
    put_id(descriptor, 0, host);
}
void describe_composed(std::uint32_t *descriptor, ID3D11UnorderedAccessView *host) {
    std::memset(descriptor, 0, 32);
    put_id(descriptor, 0, host, unordered_magic);
}
void describe_composed(std::uint32_t *descriptor, ID3D11SamplerState *host) {
    std::memset(descriptor, 0, 16);
    put_id(descriptor, 0, host);
}
ID3D11UnorderedAccessView *described_unordered(const std::uint32_t *descriptor) {
    return object_of<ID3D11UnorderedAccessView>(descriptor, unordered_magic);
}

void forget_described(IUnknown *host) {
    const std::lock_guard lock(mutex);
    const auto ids = ids_of.equal_range(host);
    for (auto id = ids.first; id != ids.second; ++id)
        by_id.erase(id->second);
    ids_of.erase(ids.first, ids.second);
    const auto addresses = addresses_of.equal_range(host);
    for (auto address = addresses.first; address != addresses.second; ++address)
        if (const auto found = buffers.find(address->second); found != buffers.end())
            buffers.erase(found);
    addresses_of.erase(addresses.first, addresses.second);
}

void describe(void *object, IUnknown *host) {
    if (ID3D11ShaderResourceView *view = nullptr; SUCCEEDED(host->QueryInterface(&view))) {
        put_id(object, 0x30, view, magic, host);
        view->Release();
    } else if (ID3D11SamplerState *sampler = nullptr; SUCCEEDED(host->QueryInterface(&sampler))) {
        put_id(object, 0x20, sampler, magic, host);
        sampler->Release();
    } else if (ID3D11Buffer *buffer = nullptr; SUCCEEDED(host->QueryInterface(&buffer))) {
        D3D11_BUFFER_DESC desc;
        buffer->GetDesc(&desc);
        put_buffer(object, buffer, next_address.fetch_add((desc.ByteWidth + 0xffff) & ~0xffffull),
                   host);
        buffer->Release();
    }
}

bool buffer_offset(ID3D11Buffer *host, std::uint64_t address, UINT &offset) {
    const std::lock_guard lock(mutex);
    auto found = buffers.upper_bound(static_cast<std::uintptr_t>(address));
    if (found == buffers.begin())
        return false;
    --found;
    if (found->second.buffer != host || address >= found->second.end)
        return false;
    offset = static_cast<UINT>(address - found->first);
    return true;
}

void describe_buffer(void *object, ID3D11Buffer *host, const void *address) {
    if (address)
        put_buffer(object, host, reinterpret_cast<std::uintptr_t>(address));
}

void apply_descriptors(ID3D11DeviceContext *context, const std::byte *start, const std::byte *end) {
    if (!start || !end || end < start)
        return;
    const std::lock_guard lock(streams_mutex);
    auto &stream = streams[context];
    read(stream, start, end);
    if (stream.registers.empty())
        return;
    for (UINT slot = 0; slot < slots; ++slot) {
        if (const auto *descriptor = find(stream, textures + slot * 0x20))
            if (auto *view = object_of<ID3D11ShaderResourceView>(descriptor))
                context->PSSetShaderResources(slot, 1, &view);
        if (const auto *descriptor = find(stream, samplers + slot * 0x10))
            if (auto *sampler = object_of<ID3D11SamplerState>(descriptor))
                context->PSSetSamplers(slot, 1, &sampler);
        if (const auto *descriptor = find(stream, vertices + slot * 0x10)) {
            UINT offset = 0;
            if (auto *buffer = buffer_at(descriptor, offset)) {
                const UINT stride = (descriptor[1] >> 16) & 0x3fff;
                context->IASetVertexBuffers(slot, 1, &buffer, &stride, &offset);
            }
        }
    }
    constants(context, stream, vertex_constants, false);
    constants(context, stream, pixel_constants, true);
}
} // namespace xv::runtime::durango::d3d11x
