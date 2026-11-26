#include "backends/graphics/backend.hpp"
#include "host/files.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace xv::graphics {
namespace {
std::atomic<Backend> selected{Backend::d3d11};
std::atomic<Fullscreen> fullscreen{Fullscreen::borderless};
std::atomic<std::uint64_t> frames{0};
// when the last frames were presented, by their number modulo the size
using Clock = std::chrono::steady_clock;
std::array<std::atomic<Clock::rep>, 512> presented_at{};
std::mutex driver_mutex;
std::string driver;

bool installed(std::string_view module) {
    std::error_code error;
    return std::filesystem::is_regular_file(host::executable_directory() / module, error);
}
} // namespace

std::string_view backend_name(Backend backend) {
    return backend == Backend::d3d11 ? "d3d11" : "vulkan";
}
std::optional<Backend> parse_backend(std::string_view name) {
    for (const auto backend : {Backend::d3d11, Backend::vulkan})
        if (name == backend_name(backend))
            return backend;
    return std::nullopt;
}
bool backend_renders_titles(Backend backend) {
    return backend == Backend::d3d11 ||
           (installed(vulkan_dxgi_module) && installed(vulkan_d3d11_module));
}
Backend selected_backend() { return selected; }
void select_backend(Backend backend) { selected = backend; }
std::string_view fullscreen_name(Fullscreen mode) {
    return mode == Fullscreen::native ? "native" : "borderless";
}
std::optional<Fullscreen> parse_fullscreen(std::string_view name) {
    for (const auto mode : {Fullscreen::borderless, Fullscreen::native})
        if (name == fullscreen_name(mode))
            return mode;
    return std::nullopt;
}
Fullscreen fullscreen_mode() { return fullscreen; }
void select_fullscreen_mode(Fullscreen mode) { fullscreen = mode; }
void frame_counted() {
    const auto frame = frames.fetch_add(1);
    presented_at[frame % presented_at.size()] = Clock::now().time_since_epoch().count();
}
// Over the last frames that span a second (or all that are kept), so that a slow rate is not
// rounded to whole frames in a fixed time. A frame later than they came on average lowers it
// as the wait goes on, so that a title that stops shows it.
double frame_rate() {
    const auto count = frames.load();
    if (count < 2)
        return 0;
    const auto at = [&](std::uint64_t frame) {
        return Clock::duration(presented_at[frame % presented_at.size()].load());
    };
    const auto last = at(count - 1);
    std::uint64_t spanned = 1;
    while (spanned + 1 < std::min<std::uint64_t>(count, presented_at.size()) &&
           last - at(count - 1 - spanned) < std::chrono::seconds(1))
        ++spanned;
    const double seconds = std::chrono::duration<double>(last - at(count - 1 - spanned)).count();
    if (seconds <= 0)
        return 0;
    const auto rate = static_cast<double>(spanned) / seconds;
    const double waited =
        std::chrono::duration<double>(Clock::now().time_since_epoch() - last).count();
    return waited * rate > 1 ? std::min(rate, 1 / waited) : rate;
}
std::uint64_t frames_presented() { return frames; }
void drawing_with(std::string_view name) {
    const std::lock_guard lock(driver_mutex);
    driver = name;
}
std::string video_driver() {
    const std::lock_guard lock(driver_mutex);
    return driver;
}
} // namespace xv::graphics
