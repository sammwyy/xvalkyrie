#include "runtime/durango/winrt/networking.hpp"
#include "formats/appx/manifest.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>

#include <catch2/catch.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <eventtoken.h>
#include <inspectable.h>
#include <nlohmann/json.hpp>
#include <objbase.h>
#include <string>
#include <vector>
#include <windows.h>
#include <winstring.h>

// Secure device association: the networking manifest as the parser reads it, the templates and
// associations a title gets from it, and a datagram over the socket address an association hands
// out. Synthetic manifests only.
namespace {
using namespace xv::runtime::durango::winrt;

// The console's interfaces, as a title calls them (the order of Windows.Xbox.Networking)
struct ISocket : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IpProtocol(std::int32_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BoundPortRangeLower(std::uint16_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BoundPortRangeUpper(std::uint16_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AllowedUsages(std::uint32_t *) = 0;
};
struct ITemplate : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE add_AssociationIncoming(IUnknown *,
                                                              EventRegistrationToken *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_AssociationIncoming(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateAssociationAsync(IInspectable *, std::uint32_t,
                                                             void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateAssociationForPortsAsync(IInspectable *, std::uint32_t,
                                                                     HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCertificateRequestAsync(HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE InstallCertificateAsync(IInspectable *, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InitiatorSocketDescription(ISocket **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AcceptorSocketDescription(ISocket **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AllowedUsages(std::uint32_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RelyingParty(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ProtocolParserDescriptions(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_QualityOfServiceMetricPathPriorities(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MultiplayerSessionRequirement(std::int32_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Associations(void **) = 0;
};
struct ITemplateStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetTemplateByName(HSTRING, ITemplate **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Templates(void **) = 0;
};
struct IAssociation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(IUnknown *, EventRegistrationToken *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE DestroyAsync(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRemoteSocketAddressBytes(std::uint32_t *,
                                                                  std::uint8_t **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLocalSocketAddressBytes(std::uint32_t *,
                                                                 std::uint8_t **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(std::int32_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Template(ITemplate **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RemoteSecureDeviceAddress(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RemoteHostName(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RemotePort(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LocalHostName(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LocalPort(HSTRING *) = 0;
};
struct IAssociationStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAssociationBySocketAddressBytes(
        std::uint32_t, std::uint8_t *, std::uint32_t, std::uint8_t *, IAssociation **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAssociationByHostNamesAndPorts(IInspectable *, HSTRING,
                                                                        IInspectable *, HSTRING,
                                                                        IAssociation **) = 0;
};
struct IOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void **) = 0;
};
struct IView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(std::uint64_t, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned *) = 0;
};
constexpr GUID template_statics_iid{
    0x46745a18, 0x697a, 0x47e1, {0xbb, 0x62, 0xcc, 0x71, 0xde, 0x0f, 0xdd, 0x8c}};
constexpr GUID association_statics_iid{
    0x15661441, 0xf13a, 0x46e4, {0x8a, 0xc2, 0xf4, 0x74, 0xd5, 0x9b, 0x1f, 0x0c}};
constexpr wchar_t template_class[] = L"Windows.Xbox.Networking.SecureDeviceAssociationTemplate";
constexpr wchar_t association_class[] = L"Windows.Xbox.Networking.SecureDeviceAssociation";

std::string manifest(unsigned game_port_lower, unsigned game_port_upper) {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/2010/manifest"
         xmlns:mx="http://schemas.microsoft.com/appx/2013/xbox/manifest">
  <Identity Name="Synthetic" Publisher="CN=Test" Version="1.0.0.0" />
  <Applications>
   <Application Id="App" Executable="game.exe" EntryPoint="game.App">
    <Extensions>
    <mx:Extension Category="windows.xbox.networking">
      <mx:XboxNetworkingManifest>
        <mx:SocketDescriptions>
          <mx:SocketDescription Name="GameSocketUdp" SecureIpProtocol="Udp" BoundPort=")" +
           std::to_string(game_port_lower) + "-" + std::to_string(game_port_upper) + R"(">
            <mx:AllowedUsages>
              <mx:SecureDeviceSocketUsage Type="Initiate" />
              <mx:SecureDeviceSocketUsage Type="Accept" />
              <mx:SecureDeviceSocketUsage Type="SendGameData" />
              <mx:SecureDeviceSocketUsage Type="ReceiveGameData" />
            </mx:AllowedUsages>
          </mx:SocketDescription>
          <mx:SocketDescription Name="HostSocketTcp" SecureIpProtocol="Tcp" BoundPort="2190">
            <mx:AllowedUsages><mx:SecureDeviceSocketUsage Type="Initiate" /></mx:AllowedUsages>
          </mx:SocketDescription>
        </mx:SocketDescriptions>
        <mx:SecureDeviceAssociationTemplates>
          <mx:SecureDeviceAssociationTemplate Name="GameTrafficUdp" InitiatorSocketDescription="GameSocketUdp" AcceptorSocketDescription="GameSocketUdp" MultiplayerSessionRequirement="None">
            <mx:AllowedUsages><mx:SecureDeviceAssociationUsage Type="Default" /></mx:AllowedUsages>
          </mx:SecureDeviceAssociationTemplate>
          <mx:SecureDeviceAssociationTemplate Name="HostTrafficTcp" InitiatorSocketDescription="HostSocketTcp" AcceptorSocketDescription="HostSocketTcp" MultiplayerSessionRequirement="Required">
            <mx:AllowedUsages>
              <mx:SecureDeviceAssociationUsage Type="InitiateFromMicrosoftConsole" />
              <mx:SecureDeviceAssociationUsage Type="AcceptOnMicrosoftConsole" />
            </mx:AllowedUsages>
          </mx:SecureDeviceAssociationTemplate>
          <mx:SecureDeviceAssociationTemplate Name="Dangling" InitiatorSocketDescription="Nowhere" AcceptorSocketDescription="GameSocketUdp" />
        </mx:SecureDeviceAssociationTemplates>
      </mx:XboxNetworkingManifest>
    </mx:Extension>
    </Extensions>
   </Application>
  </Applications>
</Package>)";
}
nlohmann::json parse(const std::string &xml) {
    return xv::appx::inspect_manifest(
        {reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()});
}
std::shared_ptr<Network> network(unsigned lower = 1200, unsigned upper = 1210) {
    return make_network(networking_manifest(parse(manifest(lower, upper)).at("networking")));
}
template <class T>
T *statics(const wchar_t *name, const GUID &iid, const std::shared_ptr<Network> &n) {
    auto *const factory = networking_class(name, n);
    REQUIRE(factory);
    void *result = nullptr;
    REQUIRE(factory->QueryInterface(iid, &result) == S_OK);
    factory->Release();
    return static_cast<T *>(result);
}
HSTRING text(const wchar_t *value) {
    HSTRING result = nullptr;
    REQUIRE(WindowsCreateString(value, static_cast<UINT32>(wcslen(value)), &result) == S_OK);
    return result;
}
std::wstring string(HSTRING value) {
    UINT32 size = 0;
    const auto *const chars = WindowsGetStringRawBuffer(value, &size);
    std::wstring result(chars, size);
    WindowsDeleteString(value);
    return result;
}
ITemplate *by_name(ITemplateStatics *templates, const wchar_t *name) {
    auto *const key = text(name);
    ITemplate *result = nullptr;
    const auto hr = templates->GetTemplateByName(key, &result);
    WindowsDeleteString(key);
    return hr == S_OK ? result : nullptr;
}
IAssociation *associate(ITemplate *entry, IInspectable *address) {
    void *operation = nullptr;
    REQUIRE(entry->CreateAssociationAsync(address, 0, &operation) == S_OK);
    REQUIRE(operation);
    void *association = nullptr;
    const auto hr = static_cast<IOperation *>(operation)->GetResults(&association);
    static_cast<IOperation *>(operation)->Release();
    REQUIRE(hr == S_OK);
    return static_cast<IAssociation *>(association);
}
std::vector<std::uint8_t> remote_bytes(IAssociation *association) {
    std::uint32_t size = 0;
    std::uint8_t *bytes = nullptr;
    REQUIRE(association->GetRemoteSocketAddressBytes(&size, &bytes) == S_OK);
    std::vector<std::uint8_t> result(bytes, bytes + size);
    CoTaskMemFree(bytes);
    return result;
}
IInspectable *device_address() {
    return static_cast<IInspectable *>(
        make_placeholder(L"Windows.Xbox.Networking.SecureDeviceAddress", __uuidof(IInspectable)));
}
struct Winsock {
    Winsock() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Winsock() {
        if (ok)
            WSACleanup();
    }
    bool ok = false;
};
} // namespace

TEST_CASE("The networking manifest gives its sockets and association templates", "[network]") {
    const auto networking = parse(manifest(1200, 2150)).at("networking");
    const auto &sockets = networking.at("sockets");
    REQUIRE(sockets.size() == 2);
    CHECK(sockets[0].at("name") == "GameSocketUdp");
    CHECK(sockets[0].at("protocol") == "Udp");
    CHECK(sockets[0].at("port_lower") == 1200);
    CHECK(sockets[0].at("port_upper") == 2150);
    CHECK(sockets[0].at("usages").size() == 4);
    CHECK(sockets[1].at("port_lower") == 2190);
    CHECK(sockets[1].at("port_upper") == 2190);
    const auto &templates = networking.at("templates");
    REQUIRE(templates.size() == 3);
    CHECK(templates[1].at("name") == "HostTrafficTcp");
    CHECK(templates[1].at("initiator") == "HostSocketTcp");
    CHECK(templates[1].at("multiplayer_session_requirement") == "Required");
    CHECK(templates[2].at("multiplayer_session_requirement").is_null());

    // the same extension among the package's own extensions is read as well
    auto package_level = manifest(1200, 2150);
    const auto open = package_level.find("    <Extensions>");
    const std::string closing = "    </Extensions>\n";
    const auto close = package_level.find(closing) + closing.size();
    const auto block = package_level.substr(open, close - open);
    package_level.erase(open, close - open);
    package_level.insert(package_level.find("</Package>"), block);
    CHECK(parse(package_level).at("networking").at("sockets").size() == 2);

    // a manifest without the extension has none of either
    const auto plain = parse(R"(<Package><Identity Name="A" /><Applications>)"
                             R"(<Application Executable="a.exe" /></Applications></Package>)");
    CHECK(plain.at("networking").at("sockets").empty());
    CHECK(plain.at("networking").at("templates").empty());
}

TEST_CASE("Malformed socket ports are refused", "[network]") {
    for (const auto *bad : {"0", "65536", "12a", "2000-1000", "", "-5", "1-2-3"}) {
        auto xml = manifest(1200, 1210);
        xml.replace(xml.find("1200-1210"), 9, bad);
        INFO(bad);
        CHECK_THROWS(parse(xml));
    }
}

TEST_CASE("Templates map their sockets, usages and session requirement", "[network]") {
    const auto model = networking_manifest(parse(manifest(1200, 1210)).at("networking"));
    REQUIRE(model.sockets.size() == 2);
    CHECK(model.sockets[0].protocol == 1);
    CHECK(model.sockets[1].protocol == 2);
    CHECK(model.sockets[0].usages == (0x1 | 0x2 | 0x200 | 0x20000));
    // the template naming an undescribed socket is left out
    REQUIRE(model.templates.size() == 2);
    CHECK(model.templates[0].name == L"GameTrafficUdp");
    CHECK(model.templates[0].requirement == 2);
    CHECK(model.templates[1].requirement == 0);
    CHECK(model.templates[1].initiator == 1);
    CHECK(model.templates[1].usages == (0x1 | 0x100));
}

TEST_CASE("A title finds the manifest's templates and their sockets", "[network]") {
    const auto shared = network();
    auto *const templates = statics<ITemplateStatics>(template_class, template_statics_iid, shared);
    void *view = nullptr;
    REQUIRE(templates->get_Templates(&view) == S_OK);
    unsigned size = 0;
    REQUIRE(static_cast<IView *>(view)->get_Size(&size) == S_OK);
    CHECK(size == 2);
    static_cast<IView *>(view)->Release();

    CHECK_FALSE(by_name(templates, L"Dangling"));
    CHECK_FALSE(by_name(templates, L"gametrafficudp")); // names are exact
    auto *const entry = by_name(templates, L"GameTrafficUdp");
    REQUIRE(entry);
    HSTRING name = nullptr;
    REQUIRE(entry->get_Name(&name) == S_OK);
    CHECK(string(name) == L"GameTrafficUdp");
    ISocket *acceptor = nullptr;
    REQUIRE(entry->get_AcceptorSocketDescription(&acceptor) == S_OK);
    std::uint16_t lower = 0, upper = 0;
    std::int32_t protocol = 0;
    CHECK(acceptor->get_BoundPortRangeLower(&lower) == S_OK);
    CHECK(acceptor->get_BoundPortRangeUpper(&upper) == S_OK);
    CHECK(acceptor->get_IpProtocol(&protocol) == S_OK);
    CHECK(lower == 1200);
    CHECK(upper == 1210);
    CHECK(protocol == 1);
    acceptor->Release();
    std::int32_t requirement = -1;
    CHECK(entry->get_MultiplayerSessionRequirement(&requirement) == S_OK);
    CHECK(requirement == 2);
    entry->Release();
    templates->Release();
}

TEST_CASE("An association is ready at once and leads to the local peer", "[network]") {
    const auto shared = network();
    auto *const templates = statics<ITemplateStatics>(template_class, template_statics_iid, shared);
    auto *const lookup =
        statics<IAssociationStatics>(association_class, association_statics_iid, shared);
    auto *const entry = by_name(templates, L"GameTrafficUdp");
    REQUIRE(entry);
    auto *const address = device_address();
    auto *const association = associate(entry, address);

    std::int32_t state = 0;
    REQUIRE(association->get_State(&state) == S_OK);
    CHECK(state == 3); // Ready
    const auto remote = remote_bytes(association);
    const auto expected = peer_socket_address(1200, "::1");
    CHECK(remote == expected);
    HSTRING port = nullptr;
    REQUIRE(association->get_RemotePort(&port) == S_OK);
    CHECK(string(port) == L"1200");
    IInspectable *same = nullptr;
    REQUIRE(association->get_RemoteSecureDeviceAddress(&same) == S_OK);
    CHECK(same == address);
    same->Release();

    // a received datagram's address finds the association again
    auto bytes = remote;
    IAssociation *found = nullptr;
    REQUIRE(lookup->GetAssociationBySocketAddressBytes(static_cast<std::uint32_t>(bytes.size()),
                                                       bytes.data(), 0, nullptr, &found) == S_OK);
    CHECK(found == association);
    found->Release();
    void *list = nullptr;
    REQUIRE(entry->get_Associations(&list) == S_OK);
    unsigned count = 0;
    static_cast<IView *>(list)->get_Size(&count);
    CHECK(count == 1);
    static_cast<IView *>(list)->Release();

    // a port outside the template's sockets is refused
    void *operation = nullptr;
    auto *const in_range = text(L"1205");
    auto *const outside = text(L"1300");
    CHECK(entry->CreateAssociationForPortsAsync(address, 0, in_range, outside, &operation) ==
          E_INVALIDARG);
    REQUIRE(entry->CreateAssociationForPortsAsync(address, 0, in_range, in_range, &operation) ==
            S_OK);
    void *chosen = nullptr;
    REQUIRE(static_cast<IOperation *>(operation)->GetResults(&chosen) == S_OK);
    static_cast<IOperation *>(operation)->Release();
    CHECK(remote_bytes(static_cast<IAssociation *>(chosen)) == peer_socket_address(1205, "::1"));
    static_cast<IAssociation *>(chosen)->Release();
    WindowsDeleteString(in_range);
    WindowsDeleteString(outside);

    // destroyed, it is no longer found
    void *action = nullptr;
    REQUIRE(association->DestroyAsync(&action) == S_OK);
    static_cast<IUnknown *>(action)->Release();
    REQUIRE(association->get_State(&state) == S_OK);
    CHECK(state == 6); // Destroyed
    found = nullptr;
    REQUIRE(lookup->GetAssociationBySocketAddressBytes(static_cast<std::uint32_t>(bytes.size()),
                                                       bytes.data(), 0, nullptr, &found) == S_OK);
    CHECK_FALSE(found);

    association->Release();
    address->Release();
    entry->Release();
    lookup->Release();
    templates->Release();
}

TEST_CASE("The peer address follows XV_SDA_PEER", "[network]") {
    CHECK(peer_socket_address(1000, "127.0.0.1").size() == sizeof(sockaddr_in));
    CHECK(peer_socket_address(1000, "::1").size() == sizeof(sockaddr_in6));
    CHECK(peer_socket_address(1000, "not-an-address").empty());
    _putenv_s("XV_SDA_PEER", "127.0.0.1");
    const auto v4 = peer_socket_address(1001);
    _putenv_s("XV_SDA_PEER", "");
    REQUIRE(v4.size() == sizeof(sockaddr_in));
    sockaddr_in address{};
    std::memcpy(&address, v4.data(), sizeof address);
    CHECK(address.sin_family == AF_INET);
    CHECK(ntohs(address.sin_port) == 1001);
    CHECK(address.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
}

TEST_CASE("A datagram sent to an association's address reaches a local server", "[network]") {
    const Winsock winsock;
    REQUIRE(winsock.ok);
    for (const auto *peer : {"::1", "127.0.0.1"}) {
        INFO(peer);
        const bool v6 = std::strchr(peer, ':') != nullptr;
        const auto family = v6 ? AF_INET6 : AF_INET;
        // the server: bound to any free loopback port, which the synthetic manifest then describes
        const SOCKET server = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(server != INVALID_SOCKET);
        auto bind_to = peer_socket_address(0, peer);
        if (bind(server, reinterpret_cast<const sockaddr *>(bind_to.data()),
                 static_cast<int>(bind_to.size())) != 0) {
            closesocket(server);
            WARN("no " << peer << " loopback on this machine");
            continue;
        }
        sockaddr_storage bound{};
        int bound_size = sizeof bound;
        REQUIRE(getsockname(server, reinterpret_cast<sockaddr *>(&bound), &bound_size) == 0);
        const auto port = ntohs(v6 ? reinterpret_cast<sockaddr_in6 &>(bound).sin6_port
                                   : reinterpret_cast<sockaddr_in &>(bound).sin_port);

        _putenv_s("XV_SDA_PEER", peer);
        const auto shared = network(port, port);
        auto *const templates =
            statics<ITemplateStatics>(template_class, template_statics_iid, shared);
        auto *const entry = by_name(templates, L"GameTrafficUdp");
        REQUIRE(entry);
        auto *const address = device_address();
        auto *const association = associate(entry, address);
        auto remote = remote_bytes(association);
        _putenv_s("XV_SDA_PEER", "");

        // the title's socket sends where the association says
        const SOCKET title = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(title != INVALID_SOCKET);
        DWORD timeout = 2000;
        setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof timeout);
        setsockopt(title, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof timeout);
        const char hello[] = "synthetic game datagram";
        REQUIRE(sendto(title, hello, sizeof hello, 0,
                       reinterpret_cast<const sockaddr *>(remote.data()),
                       static_cast<int>(remote.size())) == static_cast<int>(sizeof hello));
        char received[64]{};
        sockaddr_storage from{};
        int from_size = sizeof from;
        REQUIRE(recvfrom(server, received, sizeof received, 0, reinterpret_cast<sockaddr *>(&from),
                         &from_size) == static_cast<int>(sizeof hello));
        CHECK(std::memcmp(received, hello, sizeof hello) == 0);
        // and the server's answer comes back to the title's socket
        const char reply[] = "ack";
        REQUIRE(sendto(server, reply, sizeof reply, 0, reinterpret_cast<const sockaddr *>(&from),
                       from_size) == static_cast<int>(sizeof reply));
        char answer[8]{};
        REQUIRE(recv(title, answer, sizeof answer, 0) == static_cast<int>(sizeof reply));
        CHECK(std::memcmp(answer, reply, sizeof reply) == 0);

        closesocket(title);
        closesocket(server);
        association->Release();
        address->Release();
        entry->Release();
        templates->Release();
    }
}
