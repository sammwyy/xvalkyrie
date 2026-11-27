#include "formats/appx/manifest.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xv::appx {
namespace {
using json = nlohmann::json;
json attribute(pugi::xml_node node, const char *name) {
    const auto attr = node.attribute(name);
    return attr ? json(attr.value()) : json(nullptr);
}
json text(pugi::xml_node node) { return node ? json(node.text().get()) : json(nullptr); }

bool named(pugi::xml_node node, std::string_view local) {
    const std::string_view name = node.name();
    const auto colon = name.rfind(':');
    return (colon == std::string_view::npos ? name : name.substr(colon + 1)) == local;
}
pugi::xml_node child_named(pugi::xml_node node, std::string_view local) {
    for (auto child : node.children())
        if (named(child, local))
            return child;
    return {};
}
// <AllowedUsages><SecureDevice...Usage Type="..."/>...</AllowedUsages>, as written
json usages(pugi::xml_node node) {
    auto result = json::array();
    for (auto usage : child_named(node, "AllowedUsages").children())
        if (*usage.attribute("Type").value() && result.size() < 32)
            result.push_back(usage.attribute("Type").value());
    return result;
}
// BoundPort="N" or "N-M", 1 to 65535 and N <= M
std::pair<unsigned, unsigned> port_range(std::string_view text) {
    const auto number = [](std::string_view part) {
        if (part.empty() || part.size() > 5 ||
            part.find_first_not_of("0123456789") != std::string_view::npos)
            throw std::runtime_error("Package manifest socket has a malformed BoundPort");
        const auto value = static_cast<unsigned>(std::stoul(std::string(part)));
        if (value < 1 || value > 65535)
            throw std::runtime_error("Package manifest socket BoundPort is out of range");
        return value;
    };
    const auto dash = text.find('-');
    const auto lower = number(text.substr(0, dash));
    const auto upper = dash == std::string_view::npos ? lower : number(text.substr(dash + 1));
    if (upper < lower)
        throw std::runtime_error("Package manifest socket BoundPort range is reversed");
    return {lower, upper};
}
// <Extension Category="windows.xbox.networking"><XboxNetworkingManifest>: the sockets a title
// may use for secure device traffic and the association templates between them
// It is one of an application's extensions in the titles seen, and may be the package's.
json networking(pugi::xml_node root) {
    auto sockets = json::array();
    auto templates = json::array();
    std::vector<pugi::xml_node> extensions;
    for (auto application : root.child("Applications").children("Application"))
        for (auto extension : application.child("Extensions").children())
            extensions.push_back(extension);
    for (auto extension : root.child("Extensions").children())
        extensions.push_back(extension);
    for (auto extension : extensions) {
        if (std::string_view(extension.attribute("Category").value()) != "windows.xbox.networking")
            continue;
        const auto manifest = child_named(extension, "XboxNetworkingManifest");
        for (auto socket : child_named(manifest, "SocketDescriptions").children()) {
            if (!named(socket, "SocketDescription"))
                continue;
            if (sockets.size() >= 64)
                throw std::runtime_error("Package manifest exceeds 64 socket descriptions");
            if (!*socket.attribute("Name").value())
                throw std::runtime_error("Package manifest socket description has no Name");
            const auto [lower, upper] = port_range(socket.attribute("BoundPort").value());
            sockets.push_back({{"name", socket.attribute("Name").value()},
                               {"protocol", attribute(socket, "SecureIpProtocol")},
                               {"port_lower", lower},
                               {"port_upper", upper},
                               {"usages", usages(socket)}});
        }
        for (auto node : child_named(manifest, "SecureDeviceAssociationTemplates").children()) {
            if (!named(node, "SecureDeviceAssociationTemplate"))
                continue;
            if (templates.size() >= 64)
                throw std::runtime_error("Package manifest exceeds 64 association templates");
            if (!*node.attribute("Name").value())
                throw std::runtime_error("Package manifest association template has no Name");
            templates.push_back({{"name", node.attribute("Name").value()},
                                 {"initiator", attribute(node, "InitiatorSocketDescription")},
                                 {"acceptor", attribute(node, "AcceptorSocketDescription")},
                                 {"multiplayer_session_requirement",
                                  attribute(node, "MultiplayerSessionRequirement")},
                                 {"usages", usages(node)}});
        }
    }
    return {{"sockets", std::move(sockets)}, {"templates", std::move(templates)}};
}
} // namespace

nlohmann::json inspect_manifest(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > manifest_limit)
        throw std::runtime_error("Expected a package manifest of 1 byte to 1 MiB");
    const std::string_view xml(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (xml.find("<!DOCTYPE") != std::string_view::npos)
        throw std::runtime_error("DTD declarations are not supported in package manifests");
    pugi::xml_document doc;
    const auto result = doc.load_buffer(bytes.data(), bytes.size());
    if (!result)
        throw std::runtime_error(std::string("Package manifest XML: ") + result.description());
    const auto root = doc.document_element();
    if (std::string_view(root.name()) != "Package")
        throw std::runtime_error("Expected Package root element");
    const auto identity = root.child("Identity");
    if (!identity)
        throw std::runtime_error("Package manifest has no Identity");
    auto applications = json::array();
    for (auto entry : root.child("Applications").children("Application")) {
        if (applications.size() >= 64)
            throw std::runtime_error("Package manifest exceeds 64 applications");
        if (!*entry.attribute("Executable").value())
            throw std::runtime_error("Package manifest application has no Executable");
        applications.push_back({{"id", attribute(entry, "Id")},
                                {"executable", entry.attribute("Executable").value()},
                                {"entry_point", attribute(entry, "EntryPoint")}});
    }
    if (applications.empty())
        throw std::runtime_error("Package manifest has no applications");
    const auto prerequisites = root.child("Prerequisites");
    // Every picture the visual elements of the applications name (Square150x150Logo, Logo,
    // SmallLogo, those of the tiles...), in the order written, whatever namespace prefix
    // the manifest version gives the elements. <Properties><Logo> is not one of them: it is
    // the small mark of the store's lists, and is kept apart.
    auto logos = json::array();
    const auto collect = [&](pugi::xml_node node) {
        for (auto attr : node.attributes())
            if (std::string_view(attr.name()).ends_with("Logo") && *attr.value() &&
                logos.size() < 32 &&
                std::find(logos.begin(), logos.end(), json(attr.value())) == logos.end())
                logos.push_back(attr.value());
    };
    for (auto application : root.child("Applications").children("Application"))
        for (auto visual : application.children()) {
            collect(visual);
            for (auto child : visual.children())
                collect(child);
        }
    auto store_logo = text(root.child("Properties").child("Logo"));
    auto logo = logos.empty() ? store_logo : logos[0];
    // <SplashScreen Image="..."/> inside the visual elements of an application, whatever
    // namespace prefix the manifest version gives either of them
    auto splash = json(nullptr);
    for (auto application : root.child("Applications").children("Application"))
        for (auto visual : application.children())
            for (auto child : visual.children())
                if (splash.is_null() && std::string_view(child.name()).ends_with("SplashScreen") &&
                    child.attribute("Image"))
                    splash = child.attribute("Image").value();
    // <mx:XboxLive TitleId="..." PrimaryServiceConfigId="..."/>, wherever it is
    const auto live = root.find_node([](pugi::xml_node node) {
        return std::string_view(node.name()).ends_with("XboxLive") && node.attribute("TitleId");
    });
    // in-process servers: class -> DLL
    auto classes = json::object();
    for (auto extension : root.child("Extensions").children()) {
        if (std::string_view(extension.attribute("Category").value()) !=
            "windows.activatableClass.inProcessServer")
            continue;
        const auto server = extension.child("InProcessServer");
        const std::string_view path = server.child("Path").text().get();
        if (path.empty())
            throw std::runtime_error("Package manifest in-process server has no Path");
        for (auto activatable : server.children("ActivatableClass")) {
            if (classes.size() >= 4096)
                throw std::runtime_error("Package manifest exceeds 4096 activatable classes");
            const std::string_view id = activatable.attribute("ActivatableClassId").value();
            if (id.empty())
                throw std::runtime_error("Package manifest activatable class has no id");
            classes[std::string(id)] = path;
        }
    }
    auto dependencies = json::array();
    for (auto dependency : root.child("Dependencies").children("PackageDependency")) {
        if (dependencies.size() >= 256)
            throw std::runtime_error("Package manifest exceeds 256 package dependencies");
        dependencies.push_back(dependency.attribute("Name").value());
    }
    return {{"name", attribute(identity, "Name")},
            {"dependencies", std::move(dependencies)},
            {"version", attribute(identity, "Version")},
            {"publisher", attribute(identity, "Publisher")},
            {"architecture", attribute(identity, "ProcessorArchitecture")},
            {"display_name", text(root.child("Properties").child("DisplayName"))},
            {"logo", std::move(logo)},
            {"logos", std::move(logos)},
            {"store_logo", std::move(store_logo)},
            {"splash", std::move(splash)},
            {"os_name", text(prerequisites.child("mx:OSName"))},
            {"environment", text(prerequisites.child("mx:ApplicationEnvironment"))},
            {"applications", std::move(applications)},
            {"xbox_live_title_id", attribute(live, "TitleId")},
            {"service_config_id", attribute(live, "PrimaryServiceConfigId")},
            {"activatable_classes", std::move(classes)},
            {"networking", networking(root)}};
}
} // namespace xv::appx
