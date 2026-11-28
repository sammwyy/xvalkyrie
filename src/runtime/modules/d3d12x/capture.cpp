#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// XV_CAPTURE=<frame>[,<frame>...]: every render target and depth buffer the title draws into
// during those frames, and what it presents, read back when it presents and kept as BMP files
// in XV_CAPTURE_DIR (the temporary folder without it), in the order the title first bound
// each, with a line of the log each: size, format, draws, average colour and how much of it
// is not black. What a frame is made of, pass by pass, to find the first that goes wrong.
// The textures the title writes through memory are kept too, and each draw of those frames
// is a line of the log (device.cpp's capture_call).
namespace xv::runtime::modules::d3d12x {
namespace {
struct Captured {
    ID3D12Resource *resource;
    bool depth;
    std::uint64_t draws = 0;
};
std::mutex mutex;
std::vector<Captured> captured; // this frame's, held
std::set<std::uint64_t> frames() {
    std::set<std::uint64_t> wanted;
    if (const auto *text = std::getenv("XV_CAPTURE"))
        for (const char *at = text; *at;) {
            char *end = nullptr;
            const auto frame = std::strtoull(at, &end, 10);
            if (end == at)
                break;
            wanted.insert(frame);
            at = *end == ',' ? end + 1 : end;
        }
    return wanted;
}
const std::set<std::uint64_t> &wanted() {
    static const auto frames_wanted = frames();
    return frames_wanted;
}
std::filesystem::path folder() {
    if (const auto *text = std::getenv("XV_CAPTURE_DIR"))
        return text;
    return std::filesystem::temp_directory_path();
}
// one channel of a pixel as 0 to 255, by the format's numbers
float half(std::uint16_t bits) {
    const std::uint32_t sign = (bits >> 15) & 1, exponent = (bits >> 10) & 31,
                        mantissa = bits & 1023;
    float value = exponent == 0    ? std::ldexp(static_cast<float>(mantissa), -24)
                  : exponent == 31 ? 65504.0f
                                   : std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
    return sign ? -value : value;
}
float small_float(std::uint32_t bits, int mantissa_bits) {
    const std::uint32_t exponent = bits >> mantissa_bits,
                        mantissa = bits & ((1u << mantissa_bits) - 1);
    if (exponent == 0)
        return std::ldexp(static_cast<float>(mantissa), -14 - mantissa_bits);
    if (exponent == 31)
        return 65000.0f;
    return std::ldexp(static_cast<float>(mantissa | (1u << mantissa_bits)),
                      static_cast<int>(exponent) - 15 - mantissa_bits);
}
std::uint8_t byte_of(float value) {
    // a linear value as the display shows it, with what is above one at full
    const auto shown = std::pow(std::clamp(value, 0.0f, 1.0f), 1.0f / 2.2f);
    return static_cast<std::uint8_t>(std::lround(shown * 255.0f));
}
// the pixel at `in` as blue, green, red, alpha; false for a format not read here
bool pixel(DXGI_FORMAT format, const std::uint8_t *in, std::uint8_t *out) {
    std::uint32_t word = 0;
    std::memcpy(&word, in, 4);
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        out[0] = in[2], out[1] = in[1], out[2] = in[0], out[3] = in[3];
        return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        std::memcpy(out, in, 4);
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        out[2] = static_cast<std::uint8_t>((word & 0x3ff) >> 2);
        out[1] = static_cast<std::uint8_t>(((word >> 10) & 0x3ff) >> 2);
        out[0] = static_cast<std::uint8_t>(((word >> 20) & 0x3ff) >> 2);
        out[3] = static_cast<std::uint8_t>((word >> 30) * 85);
        return true;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        out[2] = byte_of(small_float(word & 0x7ff, 6));
        out[1] = byte_of(small_float((word >> 11) & 0x7ff, 6));
        out[0] = byte_of(small_float(word >> 22, 5));
        out[3] = 255;
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
        std::uint16_t channels[4];
        std::memcpy(channels, in, 8);
        out[2] = byte_of(half(channels[0])), out[1] = byte_of(half(channels[1]));
        out[0] = byte_of(half(channels[2])), out[3] = byte_of(half(channels[3]));
        return true;
    }
    case DXGI_FORMAT_R16G16_FLOAT: {
        std::uint16_t channels[2];
        std::memcpy(channels, in, 4);
        out[2] = byte_of(half(channels[0])), out[1] = byte_of(half(channels[1]));
        out[0] = 0, out[3] = 255;
        return true;
    }
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS: {
        std::uint16_t channel;
        std::memcpy(&channel, in, 2);
        out[0] = out[1] = out[2] = byte_of(half(channel)), out[3] = 255;
        return true;
    }
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
        out[0] = out[1] = out[2] = in[0], out[3] = 255;
        return true;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: {
        float value;
        std::memcpy(&value, in, 4);
        // depth: near is white, as a reversed depth buffer has it bright
        out[0] = out[1] = out[2] =
            static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        out[3] = 255;
        return true;
    }
    case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        float channels[4];
        std::memcpy(channels, in, 16);
        out[2] = byte_of(channels[0]), out[1] = byte_of(channels[1]);
        out[0] = byte_of(channels[2]), out[3] = byte_of(channels[3]);
        return true;
    }
    default:
        return false;
    }
}
unsigned bytes_per_pixel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
        return 1;
    default:
        return 4;
    }
}
void save(ID3D12Resource *resource, D3D12_RESOURCE_STATES state, std::uint64_t frame,
          const std::string &name, std::uint64_t draws) {
    std::vector<std::uint8_t> data;
    unsigned pitch = 0, width = 0, height = 0;
    DXGI_FORMAT format{};
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        !read_back(resource, data, pitch, width, height, format, state)) {
        std::fprintf(stderr, "[Capture] frame %llu %s: not read back (dimension %d, %u samples)\n",
                     static_cast<unsigned long long>(frame), name.c_str(), desc.Dimension,
                     desc.SampleDesc.Count);
        return;
    }
    std::uint8_t probe[4];
    const auto size = bytes_per_pixel(format);
    if (!pixel(format, data.data(), probe)) {
        std::fprintf(stderr, "[Capture] frame %llu %s: %ux%u format %d, which is not read here\n",
                     static_cast<unsigned long long>(frame), name.c_str(), width, height, format);
        return;
    }
    std::vector<std::uint8_t> picture(std::size_t{width} * height * 4);
    double sum[3]{};
    std::uint64_t lit = 0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            auto *const out = picture.data() + (std::size_t{y} * width + x) * 4;
            pixel(format, data.data() + std::size_t{y} * pitch + std::size_t{x} * size, out);
            out[3] = 255;
            sum[0] += out[2], sum[1] += out[1], sum[2] += out[0];
            lit += (out[0] | out[1] | out[2]) > 2;
        }
    const auto file = folder() / ("frame" + std::to_string(frame) + "_" + name + "_" +
                                  std::to_string(width) + "x" + std::to_string(height) + "_f" +
                                  std::to_string(static_cast<int>(format)) + ".bmp");
    if (auto *const out = _wfopen(file.c_str(), L"wb")) {
        std::uint8_t header[54]{'B', 'M'};
        const std::uint32_t total = 54 + static_cast<std::uint32_t>(picture.size()), offset = 54,
                            info = 40, planes_bits = 1 | (32 << 16);
        const std::int32_t signed_width = static_cast<std::int32_t>(width),
                           signed_height = -static_cast<std::int32_t>(height);
        std::memcpy(header + 2, &total, 4);
        std::memcpy(header + 10, &offset, 4);
        std::memcpy(header + 14, &info, 4);
        std::memcpy(header + 18, &signed_width, 4);
        std::memcpy(header + 22, &signed_height, 4);
        std::memcpy(header + 26, &planes_bits, 4);
        std::fwrite(header, 1, sizeof header, out);
        std::fwrite(picture.data(), 1, picture.size(), out);
        std::fclose(out);
    }
    const double pixels = std::max<double>(1.0, double(width) * height);
    std::fprintf(stderr,
                 "[Capture] frame %llu %s: %ux%u format %d, %llu draws, average %.1f %.1f %.1f, "
                 "%.1f%% not black\n",
                 static_cast<unsigned long long>(frame), name.c_str(), width, height, format,
                 static_cast<unsigned long long>(draws), sum[0] / pixels, sum[1] / pixels,
                 sum[2] / pixels, 100.0 * static_cast<double>(lit) / pixels);
}
} // namespace

// XV_CAPTURE_AUTO=<draws>[,<frames>]: the frame after one with that many mesh draws (direct or
// indirect) is captured, as many times as asked (once without), a frame apart at least: a
// title's 3D shots, whose frame numbers differ from run to run.
struct AutoCapture {
    std::uint64_t draws = 0, left = 0, delay = 0;
    std::atomic<std::uint64_t> meshes{0}, frame{~0ull};
};
AutoCapture &auto_capture() {
    static AutoCapture state;
    static const bool read = [] {
        if (const auto *const text = std::getenv("XV_CAPTURE_AUTO")) {
            char *end = nullptr;
            state.draws = std::strtoull(text, &end, 10);
            if (end && *end == ',')
                state.left = std::strtoull(end + 1, &end, 10);
            else
                state.left = 1;
            if (end && *end == ',')
                state.delay = std::strtoull(end + 1, nullptr, 10);
        }
        return true;
    }();
    static_cast<void>(read);
    return state;
}
void count_mesh_draw() {
    if (auto_capture().draws)
        ++auto_capture().meshes;
}
bool capturing() {
    const auto next = frames_shown() + 1;
    return (!wanted().empty() && wanted().count(next)) || auto_capture().frame == next;
}
// at each present: whether the next frame is one to capture
void arm_auto_capture() {
    auto &state = auto_capture();
    if (!state.draws)
        return;
    const auto meshes = state.meshes.exchange(0);
    if (state.left && meshes >= state.draws && state.frame != frames_shown() + 1) {
        if (state.delay) {
            --state.delay;
            return;
        }
        --state.left;
        state.frame = frames_shown() + 2;
        std::fprintf(stderr, "[Capture] %llu mesh draws in frame %llu: the next is captured\n",
                     static_cast<unsigned long long>(meshes),
                     static_cast<unsigned long long>(frames_shown() + 1));
        std::fflush(stderr);
    }
}

void capture_bound(ID3D12Resource *resource, bool depth) {
    if (!resource || !capturing())
        return;
    const std::lock_guard lock(mutex);
    for (const auto &one : captured)
        if (one.resource == resource)
            return;
    resource->AddRef();
    captured.push_back({resource, depth});
}

void capture_draw(ID3D12Resource *target) {
    if (!target || !capturing())
        return;
    const std::lock_guard lock(mutex);
    for (auto &one : captured)
        if (one.resource == target)
            ++one.draws;
}

void capture_frame(ID3D12Resource *picture) {
    const bool now = capturing();
    arm_auto_capture();
    if (!now)
        return;
    const auto frame = frames_shown() + 1;
    std::vector<Captured> taken;
    {
        const std::lock_guard lock(mutex);
        taken.swap(captured);
    }
    std::fprintf(stderr, "[Capture] frame %llu: %zu targets, into %ls\n",
                 static_cast<unsigned long long>(frame), taken.size(), folder().c_str());
    const bool capture_all = std::getenv("XV_CAPTURE_ALL") != nullptr;
    int index = 0;
    for (const auto &one : taken) {
        char name[32];
        std::snprintf(name, sizeof name, "%02d_%s", index++, one.depth ? "depth" : "target");
        if (!one.depth || capture_all)
            save(one.resource,
                 tracked_state(one.resource, one.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                       : D3D12_RESOURCE_STATE_RENDER_TARGET),
                 frame, name, one.draws);
        one.resource->Release();
    }
    if (picture)
        save(picture, tracked_state(picture, D3D12_RESOURCE_STATE_PRESENT), frame, "presented", 0);
    // and the textures the title writes through memory, as the host has them now
    if (capture_all) {
        index = 0;
        for (auto *const texture : written_textures()) {
            char name[32];
            std::snprintf(name, sizeof name, "written%02d", index++);
            save(texture, tracked_state(texture, D3D12_RESOURCE_STATE_COMMON), frame, name, 0);
            texture->Release();
        }
    }
    std::fflush(stderr);
}
} // namespace xv::runtime::modules::d3d12x
