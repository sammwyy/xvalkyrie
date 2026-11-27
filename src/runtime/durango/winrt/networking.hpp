#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

struct IInspectable;

namespace xv::runtime::durango::winrt {
// Windows.Xbox.Networking's secure device association (SDA), the way every multiplayer title's
// game traffic is meant to flow: the title binds the sockets its manifest describes, asks a
// template for an association with a remote device, and sends to the socket address the
// association gives it.
//
// On the console that address leads through an authenticated tunnel to the other device. Here
// there is no other console: every association is ready at once and leads to one peer on this
// machine, at the acceptor socket's port, where a local server can answer. The peer is ::1, or
// the IP address in XV_SDA_PEER ("127.0.0.1" for a server that listens on IPv4 only).

// A socket description of the manifest. Protocol: SecureIpProtocol (1 UDP, 2 TCP); usages:
// SecureDeviceSocketUsage bits.
struct SocketDescription {
    std::wstring name;
    std::uint32_t protocol = 0;
    std::uint16_t port_lower = 0;
    std::uint16_t port_upper = 0;
    std::uint32_t usages = 0;
};
// An association template: two of the sockets by position, MultiplayerSessionRequirement
// (0 required, 1 optional, 2 none) and SecureDeviceAssociationUsage bits.
struct AssociationTemplate {
    std::wstring name;
    std::size_t initiator = 0;
    std::size_t acceptor = 0;
    std::uint32_t requirement = 2;
    std::uint32_t usages = 0;
};
struct NetworkingManifest {
    std::vector<SocketDescription> sockets;
    std::vector<AssociationTemplate> templates;
};
// From the `networking` part of appx::inspect_manifest. Names are matched exactly, as the
// console does; a template naming a socket that is not described is left out and reported.
NetworkingManifest networking_manifest(const nlohmann::json &networking);

// The socket address bytes an association hands a title for the peer at `port` (a
// SOCKADDR_IN6, or a SOCKADDR_IN for an IPv4 peer): the peer of XV_SDA_PEER, or `host` when
// it is given. Empty when the host is not an IP address.
std::vector<std::uint8_t> peer_socket_address(std::uint16_t port, std::string_view host = {});

// The state shared by the classes below: the templates and the associations made from them.
class Network;
std::shared_ptr<Network> make_network(NetworkingManifest manifest);

// The activation factory (with its statics) of Windows.Xbox.Networking's
// SecureDeviceAssociationTemplate or SecureDeviceAssociation, with a reference; null for
// other names. The first form serves the running title, from its manifest.
IInspectable *networking_class(std::wstring_view name);
IInspectable *networking_class(std::wstring_view name, const std::shared_ptr<Network> &network);
} // namespace xv::runtime::durango::winrt
