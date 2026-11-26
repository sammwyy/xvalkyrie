#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xv::graphics {
// What a title's Direct3D calls end up on.
enum class Backend {
    d3d11, // the host's own Direct3D 11
    vulkan // Direct3D 11 translated to Vulkan by xv_d3d11.dll and xv_dxgi.dll
};
inline constexpr std::string_view vulkan_dxgi_module = "xv_dxgi.dll";
inline constexpr std::string_view vulkan_d3d11_module = "xv_d3d11.dll";
std::string_view backend_name(Backend backend);
std::optional<Backend> parse_backend(std::string_view name);
// can show what a title draws
bool backend_renders_titles(Backend backend);
// The backend console runtimes use. Process-wide; starts as d3d11.
Backend selected_backend();
void select_backend(Backend backend);

// How the title's window fills the screen (F11 in it).
enum class Fullscreen {
    borderless, // the window without its frame, as large as the display. The default.
    native      // the display given to the title's swap chain, where the host can
};
std::string_view fullscreen_name(Fullscreen mode);
std::optional<Fullscreen> parse_fullscreen(std::string_view name);
// Process-wide; starts as borderless.
Fullscreen fullscreen_mode();
void select_fullscreen_mode(Fullscreen mode);

// How many frames titles have presented in this process, whatever the backend. A backend
// calls frame_counted() once per Present.
void frame_counted();
std::uint64_t frames_presented();
// frames a second, over about the last second of them
double frame_rate();

// What the title's frames are drawn with in this process, for people: "Direct3D 11",
// "Vulkan", "Direct3D 12". Whoever creates the host device says so with drawing_with();
// empty before that.
void drawing_with(std::string_view driver);
std::string video_driver();
} // namespace xv::graphics
