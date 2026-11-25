#pragma once
#include "core/target.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdint>
#include <optional>
#include <span>

namespace xv::runtime {
// Our modules for one console: what both share plus its own runtime.
Registry console_registry(Target target);
// Registry for the console the image's imports point to, empty if that is unknown.
Registry title_registry(std::span<const std::uint8_t> file);
// the console the image's imports point to, none for a build of another kind
std::optional<Target> title_target(std::span<const std::uint8_t> file);
// Whether the executable is an app of the console's Windows side (the dashboard and the
// system apps): built for an app container, with no console graphics import. Such an app
// gets the host's own APIs; what it needs of the console's shell is not here.
bool windows_app(std::span<const std::uint8_t> file);
} // namespace xv::runtime
