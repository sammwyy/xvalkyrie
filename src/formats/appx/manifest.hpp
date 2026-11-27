#pragma once
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace xv::appx {
inline constexpr std::size_t manifest_limit = 1024 * 1024;
// Identity, OS name and applications of an AppxManifest.xml, the description of an
// unpacked Xbox One (ERA) title. Executable paths are reported as written, and so are
// the DLLs of `activatable_classes`, the WinRT classes the package implements itself.
// `networking` holds the windows.xbox.networking extension: `sockets` (name, protocol,
// port_lower, port_upper, usages) and association `templates` (name, initiator, acceptor,
// multiplayer_session_requirement, usages), with names and usages as written.
nlohmann::json inspect_manifest(std::span<const std::uint8_t> bytes);
} // namespace xv::appx
