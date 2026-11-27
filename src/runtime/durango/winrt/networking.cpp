#include "runtime/durango/winrt/networking.hpp"
#include "formats/appx/manifest.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/winrt/async.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/placeholder.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <eventtoken.h>
#include <filesystem>
#include <initializer_list>
#include <inspectable.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <objbase.h>
#include <utility>
#include <windows.h>
#include <winstring.h>

namespace xv::runtime::durango::winrt {
namespace {
constexpr wchar_t template_class[] = L"Windows.Xbox.Networking.SecureDeviceAssociationTemplate";
constexpr wchar_t association_class[] = L"Windows.Xbox.Networking.SecureDeviceAssociation";
constexpr wchar_t socket_class[] = L"Windows.Xbox.Networking.SecureDeviceSocketDescription";

// The interfaces, in the order of the console's Windows.Xbox.Networking metadata
constexpr GUID template_statics_iid{
    0x46745a18, 0x697a, 0x47e1, {0xbb, 0x62, 0xcc, 0x71, 0xde, 0x0f, 0xdd, 0x8c}};
constexpr GUID template_iid{
    0x5479611e, 0x35fa, 0x4241, {0x95, 0xba, 0xdf, 0x8e, 0x83, 0x38, 0xc7, 0x39}};
constexpr GUID association_statics_iid{
    0x15661441, 0xf13a, 0x46e4, {0x8a, 0xc2, 0xf4, 0x74, 0xd5, 0x9b, 0x1f, 0x0c}};
constexpr GUID association_iid{
    0xcce4fe47, 0x86fc, 0x467c, {0xa1, 0x21, 0x83, 0x06, 0x1f, 0x4b, 0x02, 0x86}};
constexpr GUID socket_iid{
    0x9835ed01, 0x7b1e, 0x41fc, {0xb6, 0x4d, 0x7d, 0x20, 0x44, 0x88, 0x92, 0xd6}};

// SecureDeviceAssociationState
constexpr std::int32_t state_ready = 3;
constexpr std::int32_t state_destroyed = 6;

struct ISocketDescription : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IpProtocol(std::int32_t *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BoundPortRangeLower(std::uint16_t *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BoundPortRangeUpper(std::uint16_t *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AllowedUsages(std::uint32_t *value) = 0;
};
struct IAssociation;
struct ITemplate : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE add_AssociationIncoming(IUnknown *handler,
                                                              EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_AssociationIncoming(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateAssociationAsync(IInspectable *address,
                                                             std::uint32_t behavior,
                                                             void **operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateAssociationForPortsAsync(IInspectable *address,
                                                                     std::uint32_t behavior,
                                                                     HSTRING initiator_port,
                                                                     HSTRING acceptor_port,
                                                                     void **operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCertificateRequestAsync(HSTRING subject,
                                                                    void **operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE InstallCertificateAsync(IInspectable *certificate,
                                                              void **action) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_InitiatorSocketDescription(ISocketDescription **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AcceptorSocketDescription(ISocketDescription **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AllowedUsages(std::uint32_t *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RelyingParty(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ProtocolParserDescriptions(void **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_QualityOfServiceMetricPathPriorities(void **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MultiplayerSessionRequirement(std::int32_t *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Associations(void **value) = 0;
};
struct ITemplateStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetTemplateByName(HSTRING name, ITemplate **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Templates(void **value) = 0;
};
struct IAssociation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(IUnknown *handler,
                                                       EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE DestroyAsync(void **action) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRemoteSocketAddressBytes(std::uint32_t *size,
                                                                  std::uint8_t **bytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLocalSocketAddressBytes(std::uint32_t *size,
                                                                 std::uint8_t **bytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(std::int32_t *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Template(ITemplate **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RemoteSecureDeviceAddress(IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RemoteHostName(IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RemotePort(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LocalHostName(IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LocalPort(HSTRING *value) = 0;
};
struct IAssociationStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAssociationBySocketAddressBytes(std::uint32_t remote_size,
                                                                         std::uint8_t *remote,
                                                                         std::uint32_t local_size,
                                                                         std::uint8_t *local,
                                                                         IAssociation **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAssociationByHostNamesAndPorts(IInspectable *remote_host,
                                                                        HSTRING remote_port,
                                                                        IInspectable *local_host,
                                                                        HSTRING local_port,
                                                                        IAssociation **value) = 0;
};

HRESULT put_string(HSTRING *value, std::wstring_view text) {
    if (!value)
        return E_POINTER;
    return WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), value);
}
template <class T> HRESULT put(T *value, T content) {
    if (!value)
        return E_POINTER;
    *value = content;
    return S_OK;
}
HRESULT put_bytes(std::uint32_t *size, std::uint8_t **bytes,
                  const std::vector<std::uint8_t> &data) {
    if (!size || !bytes)
        return E_POINTER;
    *size = 0;
    *bytes = nullptr;
    if (data.empty())
        return S_OK;
    *bytes = static_cast<std::uint8_t *>(CoTaskMemAlloc(data.size()));
    if (!*bytes)
        return E_OUTOFMEMORY;
    std::memcpy(*bytes, data.data(), data.size());
    *size = static_cast<std::uint32_t>(data.size());
    return S_OK;
}
std::wstring wide(const std::string &text) {
    if (text.empty())
        return {};
    const auto size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size);
    return result;
}
std::wstring_view view(HSTRING text) {
    UINT32 size = 0;
    const auto *const chars = WindowsGetStringRawBuffer(text, &size);
    return {chars, size};
}
std::uint32_t flags(const nlohmann::json &names,
                    std::initializer_list<std::pair<std::string_view, std::uint32_t>> table) {
    std::uint32_t result = 0;
    for (const auto &name : names)
        for (const auto &[key, bit] : table)
            if (name.is_string() && name.get<std::string>() == key)
                result |= bit;
    return result;
}
// A port in a string (CreateAssociationForPortsAsync, RemotePort), 0 if it is none
std::uint16_t port_of(std::wstring_view text) {
    if (text.empty() || text.size() > 5 || text.find_first_not_of(L"0123456789") != text.npos)
        return 0;
    const auto value = std::wcstoul(std::wstring(text).c_str(), nullptr, 10);
    return value <= 65535 ? static_cast<std::uint16_t>(value) : 0;
}
} // namespace

NetworkingManifest networking_manifest(const nlohmann::json &networking) {
    NetworkingManifest result;
    if (!networking.is_object())
        return result;
    for (const auto &socket : networking.value("sockets", nlohmann::json::array())) {
        SocketDescription description;
        description.name = wide(socket.at("name").get<std::string>());
        const auto protocol = socket.at("protocol");
        description.protocol = protocol == "Udp" ? 1 : protocol == "Tcp" ? 2 : 0;
        description.port_lower =
            static_cast<std::uint16_t>(socket.at("port_lower").get<unsigned>());
        description.port_upper =
            static_cast<std::uint16_t>(socket.at("port_upper").get<unsigned>());
        description.usages = flags(socket.at("usages"), {{"Initiate", 0x1},
                                                         {"Accept", 0x2},
                                                         {"SendChat", 0x100},
                                                         {"SendGameData", 0x200},
                                                         {"SendDebug", 0x400},
                                                         {"SendInsecure", 0x800},
                                                         {"ReceiveChat", 0x10000},
                                                         {"ReceiveGameData", 0x20000},
                                                         {"ReceiveDebug", 0x40000},
                                                         {"ReceiveInsecure", 0x80000}});
        result.sockets.push_back(std::move(description));
    }
    const auto find = [&](const nlohmann::json &name) -> std::size_t {
        if (name.is_string())
            for (std::size_t i = 0; i < result.sockets.size(); ++i)
                if (result.sockets[i].name == wide(name.get<std::string>()))
                    return i;
        return result.sockets.size();
    };
    for (const auto &entry : networking.value("templates", nlohmann::json::array())) {
        AssociationTemplate association;
        association.name = wide(entry.at("name").get<std::string>());
        association.initiator = find(entry.at("initiator"));
        association.acceptor = find(entry.at("acceptor"));
        if (association.initiator == result.sockets.size() ||
            association.acceptor == result.sockets.size()) {
            std::fprintf(stderr,
                         "[Network] association template %ls names a socket the manifest does "
                         "not describe; left out\n",
                         association.name.c_str());
            continue;
        }
        const auto requirement = entry.at("multiplayer_session_requirement");
        // an absent requirement is taken as None: nothing here has a multiplayer session
        association.requirement = requirement == "Required" ? 0 : requirement == "Optional" ? 1 : 2;
        association.usages = flags(entry.at("usages"), {{"Default", 0x0},
                                                        {"InitiateFromMicrosoftConsole", 0x1},
                                                        {"InitiateFromXboxLiveCompute", 0x2},
                                                        {"InitiateFromOtherDevice", 0x4},
                                                        {"InitiateFromWindowsDesktop", 0x8},
                                                        {"AcceptOnMicrosoftConsole", 0x100},
                                                        {"AcceptOnXboxLiveCompute", 0x200},
                                                        {"AcceptOnOtherDevice", 0x400},
                                                        {"AcceptOnWindowsDesktop", 0x800}});
        result.templates.push_back(std::move(association));
    }
    return result;
}

std::vector<std::uint8_t> peer_socket_address(std::uint16_t port, std::string_view host) {
    std::string text(host);
    if (text.empty()) {
        char configured[128]{};
        const auto size = GetEnvironmentVariableA("XV_SDA_PEER", configured, sizeof configured);
        text = size && size < sizeof configured ? configured : "::1";
    }
    std::vector<std::uint8_t> bytes;
    sockaddr_in6 v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(port);
        bytes.resize(sizeof v6);
        std::memcpy(bytes.data(), &v6, sizeof v6);
        return bytes;
    }
    sockaddr_in v4{};
    if (inet_pton(AF_INET, text.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        bytes.resize(sizeof v4);
        std::memcpy(bytes.data(), &v4, sizeof v4);
    }
    return bytes;
}

namespace {
// Reference counting and the IInspectable part, for an object that implements one interface
// of its own (`iid`) and answers the rest with placeholders.
template <class Interface> class Object : public Interface {
  public:
    Object(const wchar_t *name, const GUID &iid) : name_(name), iid_(iid) {}
    virtual ~Object() = default;
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) ||
            iid == __uuidof(IAgileObject) || iid == iid_) {
            *object = static_cast<Interface *>(this);
            this->AddRef();
            return S_OK;
        }
        return placeholder_interface(name_, iid, object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        if (!count || !iids)
            return E_POINTER;
        *iids = static_cast<IID *>(CoTaskMemAlloc(sizeof(IID)));
        if (!*iids)
            return E_OUTOFMEMORY;
        **iids = iid_;
        *count = 1;
        return S_OK;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override { return put_string(name, name_); }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override { return put(level, BaseTrust); }

  private:
    const wchar_t *name_;
    GUID iid_;
    std::atomic<ULONG> references_{1};
};

class SocketObject final : public Object<ISocketDescription> {
  public:
    explicit SocketObject(const SocketDescription &socket)
        : Object(socket_class, socket_iid), socket_(socket) {}
    STDMETHODIMP get_Name(HSTRING *value) override { return put_string(value, socket_.name); }
    STDMETHODIMP get_IpProtocol(std::int32_t *value) override {
        return put(value, static_cast<std::int32_t>(socket_.protocol));
    }
    STDMETHODIMP get_BoundPortRangeLower(std::uint16_t *value) override {
        return put(value, socket_.port_lower);
    }
    STDMETHODIMP get_BoundPortRangeUpper(std::uint16_t *value) override {
        return put(value, socket_.port_upper);
    }
    STDMETHODIMP get_AllowedUsages(std::uint32_t *value) override {
        return put(value, socket_.usages);
    }

  private:
    SocketDescription socket_;
};
} // namespace

// What the classes share: the manifest's templates, and the associations made from them.
class Network {
  public:
    explicit Network(NetworkingManifest manifest) : manifest_(std::move(manifest)) {}
    const NetworkingManifest &manifest() const { return manifest_; }
    std::mutex lock;
    std::vector<IAssociation *> associations; // no references: each removes itself

  private:
    NetworkingManifest manifest_;
};

std::shared_ptr<Network> make_network(NetworkingManifest manifest) {
    return std::make_shared<Network>(std::move(manifest));
}

namespace {
class TemplateObject;
ITemplate *template_object(const std::shared_ptr<Network> &network, std::size_t index);

class AssociationObject final : public Object<IAssociation> {
  public:
    AssociationObject(std::shared_ptr<Network> network, std::size_t index, IInspectable *address,
                      std::uint16_t local_port, std::uint16_t remote_port)
        : Object(association_class, association_iid), network_(std::move(network)), index_(index),
          address_(address), local_port_(local_port), remote_port_(remote_port),
          remote_(peer_socket_address(remote_port)), local_(local_address(remote_, local_port)) {
        if (address_)
            address_->AddRef();
        const std::lock_guard guard(network_->lock);
        network_->associations.push_back(this);
    }
    ~AssociationObject() override {
        forget();
        if (address_)
            address_->Release();
    }
    bool matches(const std::uint8_t *remote, std::uint32_t remote_size, const std::uint8_t *local,
                 std::uint32_t local_size) const {
        const auto same = [](const std::vector<std::uint8_t> &own, const std::uint8_t *bytes,
                             std::uint32_t size) {
            return own.size() == size && (!size || std::memcmp(own.data(), bytes, size) == 0);
        };
        return state_ == state_ready && same(remote_, remote, remote_size) &&
               (!local_size || same(local_, local, local_size));
    }
    std::size_t template_index() const { return index_; }

    STDMETHODIMP add_StateChanged(IUnknown *handler, EventRegistrationToken *token) override {
        // Ready from the start, and destroyed only when asked: nothing to raise
        if (!handler || !token)
            return E_POINTER;
        token->value = ++next_token_;
        return S_OK;
    }
    STDMETHODIMP remove_StateChanged(EventRegistrationToken) override { return S_OK; }
    STDMETHODIMP DestroyAsync(void **action) override {
        if (!action)
            return E_POINTER;
        state_ = state_destroyed;
        forget();
        *action = make_completed_action();
        return S_OK;
    }
    STDMETHODIMP GetRemoteSocketAddressBytes(std::uint32_t *size, std::uint8_t **bytes) override {
        return put_bytes(size, bytes, remote_);
    }
    STDMETHODIMP GetLocalSocketAddressBytes(std::uint32_t *size, std::uint8_t **bytes) override {
        return put_bytes(size, bytes, local_);
    }
    STDMETHODIMP get_State(std::int32_t *value) override { return put(value, state_.load()); }
    STDMETHODIMP get_Template(ITemplate **value) override {
        if (!value)
            return E_POINTER;
        *value = template_object(network_, index_);
        return S_OK;
    }
    STDMETHODIMP get_RemoteSecureDeviceAddress(IInspectable **value) override {
        if (!value)
            return E_POINTER;
        *value = address_;
        if (address_)
            address_->AddRef();
        return S_OK;
    }
    // Windows.Networking.HostName is the host's to make; no title has been seen to ask yet
    STDMETHODIMP get_RemoteHostName(IInspectable **value) override { return null_object(value); }
    STDMETHODIMP get_LocalHostName(IInspectable **value) override { return null_object(value); }
    STDMETHODIMP get_RemotePort(HSTRING *value) override {
        return put_string(value, std::to_wstring(remote_port_));
    }
    STDMETHODIMP get_LocalPort(HSTRING *value) override {
        return put_string(value, std::to_wstring(local_port_));
    }

  private:
    static HRESULT null_object(IInspectable **value) {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        return E_NOTIMPL;
    }
    // this machine's loopback in the family of the peer's address, at the initiator's port
    static std::vector<std::uint8_t> local_address(const std::vector<std::uint8_t> &peer,
                                                   std::uint16_t port) {
        const bool v4 = peer.size() == sizeof(sockaddr_in);
        return peer_socket_address(port, v4 ? "127.0.0.1" : "::1");
    }
    void forget() {
        const std::lock_guard guard(network_->lock);
        auto &all = network_->associations;
        all.erase(std::remove(all.begin(), all.end(), this), all.end());
    }

    std::shared_ptr<Network> network_;
    std::size_t index_;
    IInspectable *address_;
    std::uint16_t local_port_;
    std::uint16_t remote_port_;
    std::vector<std::uint8_t> remote_;
    std::vector<std::uint8_t> local_;
    std::atomic<std::int32_t> state_{state_ready};
    std::atomic<std::int64_t> next_token_{0};
};

class TemplateObject final : public Object<ITemplate> {
  public:
    TemplateObject(std::shared_ptr<Network> network, std::size_t index)
        : Object(template_class, template_iid), network_(std::move(network)), index_(index) {}

    STDMETHODIMP add_AssociationIncoming(IUnknown *handler,
                                         EventRegistrationToken *token) override {
        // Nothing initiates an association towards this console: never raised
        if (!handler || !token)
            return E_POINTER;
        token->value = ++next_token_;
        return S_OK;
    }
    STDMETHODIMP remove_AssociationIncoming(EventRegistrationToken) override { return S_OK; }
    STDMETHODIMP CreateAssociationAsync(IInspectable *address, std::uint32_t,
                                        void **operation) override {
        return create(address, own().port_lower, acceptor().port_lower, operation);
    }
    STDMETHODIMP CreateAssociationForPortsAsync(IInspectable *address, std::uint32_t,
                                                HSTRING initiator_port, HSTRING acceptor_port,
                                                void **operation) override {
        const auto local = port_of(view(initiator_port));
        const auto remote = port_of(view(acceptor_port));
        if (!in_range(own(), local) || !in_range(acceptor(), remote)) {
            if (operation)
                *operation = nullptr;
            return E_INVALIDARG;
        }
        return create(address, local, remote, operation);
    }
    STDMETHODIMP CreateCertificateRequestAsync(HSTRING, void **operation) override {
        if (!operation)
            return E_POINTER;
        *operation = make_failed_operation(E_NOTIMPL);
        return S_OK;
    }
    STDMETHODIMP InstallCertificateAsync(IInspectable *, void **action) override {
        if (!action)
            return E_POINTER;
        *action = make_failed_action(E_NOTIMPL);
        return S_OK;
    }
    STDMETHODIMP get_Name(HSTRING *value) override { return put_string(value, entry().name); }
    STDMETHODIMP get_InitiatorSocketDescription(ISocketDescription **value) override {
        return socket(value, entry().initiator);
    }
    STDMETHODIMP get_AcceptorSocketDescription(ISocketDescription **value) override {
        return socket(value, entry().acceptor);
    }
    STDMETHODIMP get_AllowedUsages(std::uint32_t *value) override {
        return put(value, entry().usages);
    }
    STDMETHODIMP get_RelyingParty(HSTRING *value) override { return put_string(value, L""); }
    STDMETHODIMP get_ProtocolParserDescriptions(void **value) override { return empty(value); }
    STDMETHODIMP get_QualityOfServiceMetricPathPriorities(void **value) override {
        return empty(value);
    }
    STDMETHODIMP get_MultiplayerSessionRequirement(std::int32_t *value) override {
        return put(value, static_cast<std::int32_t>(entry().requirement));
    }
    STDMETHODIMP get_Associations(void **value) override {
        if (!value)
            return E_POINTER;
        std::vector<IInspectable *> items;
        const std::lock_guard guard(network_->lock);
        for (auto *association : network_->associations)
            if (static_cast<AssociationObject *>(association)->template_index() == index_) {
                association->AddRef();
                items.push_back(association);
            }
        *value = make_vector_view(std::move(items));
        return S_OK;
    }

  private:
    const AssociationTemplate &entry() const { return network_->manifest().templates[index_]; }
    const SocketDescription &own() const { return network_->manifest().sockets[entry().initiator]; }
    const SocketDescription &acceptor() const {
        return network_->manifest().sockets[entry().acceptor];
    }
    static bool in_range(const SocketDescription &socket, std::uint16_t port) {
        return port && port >= socket.port_lower && port <= socket.port_upper;
    }
    HRESULT create(IInspectable *address, std::uint16_t local, std::uint16_t remote,
                   void **operation) {
        if (!operation)
            return E_POINTER;
        *operation = nullptr;
        if (!address)
            return E_INVALIDARG;
        if (peer_socket_address(remote).empty()) { // XV_SDA_PEER is not an IP address
            std::fputs("[Network] XV_SDA_PEER is not an IP address: no peer\n", stderr);
            *operation = make_failed_operation(HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE));
            return S_OK;
        }
        auto *const association = new AssociationObject(network_, index_, address, local, remote);
        *operation = make_completed_operation(static_cast<IAssociation *>(association));
        return S_OK;
    }
    HRESULT socket(ISocketDescription **value, std::size_t index) {
        if (!value)
            return E_POINTER;
        *value = new SocketObject(network_->manifest().sockets[index]);
        return S_OK;
    }
    static HRESULT empty(void **value) {
        if (!value)
            return E_POINTER;
        *value = make_vector_view({});
        return S_OK;
    }

    std::shared_ptr<Network> network_;
    std::size_t index_;
    std::atomic<std::int64_t> next_token_{0};
};

ITemplate *template_object(const std::shared_ptr<Network> &network, std::size_t index) {
    return new TemplateObject(network, index);
}

class TemplateStatics final : public Object<ITemplateStatics> {
  public:
    explicit TemplateStatics(std::shared_ptr<Network> network)
        : Object(template_class, template_statics_iid), network_(std::move(network)) {}
    STDMETHODIMP GetTemplateByName(HSTRING name, ITemplate **value) override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        const auto wanted = view(name);
        const auto &templates = network_->manifest().templates;
        for (std::size_t i = 0; i < templates.size(); ++i)
            if (templates[i].name == wanted) {
                *value = template_object(network_, i);
                return S_OK;
            }
        std::fprintf(stderr, "[Network] no association template %.*ls in the manifest\n",
                     static_cast<int>(wanted.size()), wanted.data());
        return E_INVALIDARG;
    }
    STDMETHODIMP get_Templates(void **value) override {
        if (!value)
            return E_POINTER;
        std::vector<IInspectable *> items;
        for (std::size_t i = 0; i < network_->manifest().templates.size(); ++i)
            items.push_back(template_object(network_, i));
        *value = make_vector_view(std::move(items));
        return S_OK;
    }

  private:
    std::shared_ptr<Network> network_;
};

class AssociationStatics final : public Object<IAssociationStatics> {
  public:
    explicit AssociationStatics(std::shared_ptr<Network> network)
        : Object(association_class, association_statics_iid), network_(std::move(network)) {}
    STDMETHODIMP GetAssociationBySocketAddressBytes(std::uint32_t remote_size, std::uint8_t *remote,
                                                    std::uint32_t local_size, std::uint8_t *local,
                                                    IAssociation **value) override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        if (!remote_size || !remote)
            return E_INVALIDARG;
        const std::lock_guard guard(network_->lock);
        for (auto *association : network_->associations)
            if (static_cast<AssociationObject *>(association)
                    ->matches(remote, remote_size, local, local ? local_size : 0)) {
                association->AddRef();
                *value = association;
                return S_OK;
            }
        // no association with that device: the console answers with none
        return S_OK;
    }
    STDMETHODIMP GetAssociationByHostNamesAndPorts(IInspectable *, HSTRING, IInspectable *, HSTRING,
                                                   IAssociation **value) override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        return E_NOTIMPL;
    }

  private:
    std::shared_ptr<Network> network_;
};

std::shared_ptr<Network> title_network() {
    static const auto network = [] {
        NetworkingManifest manifest;
        try {
            manifest = networking_manifest(
                appx::inspect_manifest(
                    host::read_snapshot(package_directory(std::filesystem::current_path()) /
                                            "AppxManifest.xml",
                                        appx::manifest_limit))
                    .at("networking"));
            std::fprintf(stderr, "[Network] %zu socket descriptions, %zu association templates\n",
                         manifest.sockets.size(), manifest.templates.size());
        } catch (const std::exception &e) {
            std::fprintf(stderr, "[Network] no networking manifest: %s\n", e.what());
        }
        return make_network(std::move(manifest));
    }();
    return network;
}
} // namespace

IInspectable *networking_class(std::wstring_view name, const std::shared_ptr<Network> &network) {
    if (name == template_class)
        return new TemplateStatics(network);
    if (name == association_class)
        return new AssociationStatics(network);
    return nullptr;
}

IInspectable *networking_class(std::wstring_view name) {
    if (name != template_class && name != association_class)
        return nullptr;
    // one of each for the process, kept alive by the reference held here
    static IInspectable *const templates = networking_class(template_class, title_network());
    static IInspectable *const associations = networking_class(association_class, title_network());
    auto *const factory = name == template_class ? templates : associations;
    factory->AddRef();
    return factory;
}
} // namespace xv::runtime::durango::winrt
