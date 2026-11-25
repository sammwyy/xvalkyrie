#include "runtime/console.hpp"
#include "analysis/modules.hpp"
#include "formats/pe/inspect.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/scarlett/runtime.hpp"
#include <nlohmann/json.hpp>

namespace xv::runtime {
Registry console_registry(Target target) {
    Registry registry;
    if (target == Target::durango)
        durango::install(registry);
    else
        scarlett::install(registry);
    return registry;
}
bool windows_app(std::span<const std::uint8_t> file) {
    const auto report = pe::inspect(file, pe::Detail::light);
    constexpr std::uint64_t app_container = 0x1000;
    return !analysis::console_target(analysis::import_profile(report)) &&
           (report.at("dll_characteristics").get<std::uint64_t>() & app_container);
}
std::optional<Target> title_target(std::span<const std::uint8_t> file) {
    return analysis::console_target(analysis::import_profile(pe::inspect(file, pe::Detail::light)));
}
Registry title_registry(std::span<const std::uint8_t> file) {
    const auto target = title_target(file);
    if (target)
        return console_registry(*target);
    // Not a console build: nothing of ours to bind it to. It gets the host's own functions,
    // so that it runs as far as it can.
    Registry registry;
    registry.open_to_host();
    if (windows_app(file)) {
        // An app of the console's own system (its dashboard and the like) is a Windows app
        // that asks for the console's WinRT classes: those are answered as for a title,
        // whichever API set the activation functions are imported from.
        durango::install_winrt(registry, true);
        for (const auto *name : {"RoGetActivationFactory", "RoActivateInstance"})
            registry.add_api(name, registry.find("combase.dll", name));
    }
    return registry;
}
} // namespace xv::runtime
