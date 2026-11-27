#include "formats/appx/manifest.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"

#include "runtime/durango/winrt/application.hpp"
#include "runtime/durango/winrt/gamepad.hpp"
#include "runtime/durango/winrt/networking.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include "runtime/durango/winrt/storage.hpp"
#include "runtime/durango/winrt/system_ui.hpp"
#include <activation.h>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <inspectable.h>
#include <map>
#include <nlohmann/json.hpp>
#include <roapi.h>
#include <string>
#include <string_view>
#include <windows.h>
#include <winstring.h>

namespace xv::runtime::durango {
namespace {
// Host classes that behave the same in any process. The rest of the host's classes want a
// packaged application around them, so the console versions are ours.
// An app of the console's system is a Windows app: every class the host has is the host's
// for it, and only the console's own are ours.
bool host_first = false;
bool host_class(std::wstring_view name) {
    if (host_first)
        return !name.starts_with(L"Windows.Xbox.");
    for (const std::wstring_view prefix :
         {L"Windows.Foundation.", L"Windows.Storage.Streams.", L"Windows.Data.",
          L"Windows.Globalization.", L"Windows.Security.Cryptography.", L"Windows.Media.Devices."})
        if (name.starts_with(prefix))
            return true;
    return false;
}

// in-process servers of the manifest: class -> DLL
const std::map<std::wstring, std::string, std::less<>> &title_classes() {
    static const auto classes = [] {
        std::map<std::wstring, std::string, std::less<>> result;
        try {
            const auto manifest = appx::inspect_manifest(host::read_snapshot(
                package_directory(std::filesystem::current_path()) / "AppxManifest.xml",
                appx::manifest_limit));
            for (const auto &[name, path] : manifest.at("activatable_classes").items())
                result.emplace(std::wstring(name.begin(), name.end()), path.get<std::string>());
        } catch (const std::exception &e) {
            std::fprintf(stderr, "[Runtime] no WinRT classes of the title's own: %s\n", e.what());
        }
        return result;
    }();
    return classes;
}
HRESULT title_factory(const wchar_t *name, const GUID &iid, void **factory) {
    const auto entry = title_classes().find(std::wstring_view(name));
    if (entry == title_classes().end())
        return REGDB_E_CLASSNOTREG;
    using Get = HRESULT(STDAPICALLTYPE *)(HSTRING, IActivationFactory **);
    const auto get =
        reinterpret_cast<Get>(detail::title_export(entry->second, "DllGetActivationFactory"));
    if (!get) {
        std::fprintf(stderr, "[Runtime] %ls: the title's %s has no DllGetActivationFactory\n", name,
                     entry->second.c_str());
        return REGDB_E_CLASSNOTREG;
    }
    HSTRING_HEADER header;
    HSTRING text = nullptr;
    auto result =
        WindowsCreateStringReference(name, static_cast<UINT32>(wcslen(name)), &header, &text);
    IActivationFactory *own = nullptr;
    if (SUCCEEDED(result))
        result = get(text, &own);
    if (SUCCEEDED(result)) {
        result = own->QueryInterface(iid, factory);
        own->Release();
    }
    if (FAILED(result))
        std::fprintf(stderr, "[Runtime] %ls: the title's %s answered 0x%08lx\n", name,
                     entry->second.c_str(), static_cast<unsigned long>(result));
    return result;
}

// vccorlib's GetActivationFactoryByPCWSTR: how C++/CX code reaches every WinRT class.
// Classes of the host are used as they are; the console ones have to be ours.
HRESULT host_factory(const wchar_t *name, const GUID &iid, void **factory) {
    HSTRING_HEADER header;
    HSTRING text = nullptr;
    auto result =
        WindowsCreateStringReference(name, static_cast<UINT32>(wcslen(name)), &header, &text);
    if (SUCCEEDED(result))
        result = RoGetActivationFactory(text, iid, factory);
    return result;
}
HRESULT __cdecl activation_factory(const wchar_t *name, const GUID &iid, void **factory) {
    if (host_first && host_class(name) && SUCCEEDED(host_factory(name, iid, factory)))
        return S_OK;
    if (auto *const own = winrt::application_class(name))
        return own->QueryInterface(iid, factory);
    if (auto *const own = winrt::input_class(name))
        return own->QueryInterface(iid, factory);
    if (auto *const own = winrt::networking_class(name)) {
        const auto result = own->QueryInterface(iid, factory);
        own->Release();
        return result;
    }
    if (auto *const own = winrt::storage_class(name))
        return own->QueryInterface(iid, factory);
    if (auto *const own = winrt::system_ui_class(name))
        return own->QueryInterface(iid, factory);
    if (const auto result = title_factory(name, iid, factory); result != REGDB_E_CLASSNOTREG)
        return result;
    if (host_class(name) && SUCCEEDED(host_factory(name, iid, factory)))
        return S_OK;
    *factory = winrt::make_placeholder(name, iid);
    return S_OK;
}
// what WRL code in the title's DLLs calls
HRESULT WINAPI ro_get_activation_factory(HSTRING name, const GUID &iid, void **factory) {
    if (!factory)
        return E_POINTER;
    *factory = nullptr;
    if (WindowsIsStringEmpty(name))
        return E_INVALIDARG;
    const std::wstring text(WindowsGetStringRawBuffer(name, nullptr));
    return activation_factory(text.c_str(), iid, factory);
}
HRESULT WINAPI ro_activate_instance(HSTRING name, IInspectable **instance) {
    if (!instance)
        return E_POINTER;
    *instance = nullptr;
    IActivationFactory *factory = nullptr;
    auto result = ro_get_activation_factory(name, __uuidof(IActivationFactory),
                                            reinterpret_cast<void **>(&factory));
    if (SUCCEEDED(result)) {
        result = factory->ActivateInstance(instance);
        factory->Release();
    }
    return result;
}
} // namespace

void install_winrt(Registry &registry, bool system_app) {
    host_first = system_app;
    // titles built with Visual Studio 2012 use the first, later ones the second
    for (const auto *module : {"vccorlib110.dll", "vccorlib140.dll"})
        registry.add(module, "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z",
                     reinterpret_cast<void *>(&activation_factory));
    registry.add("combase.dll", "RoGetActivationFactory",
                 reinterpret_cast<void *>(&ro_get_activation_factory));
    registry.add("combase.dll", "RoActivateInstance",
                 reinterpret_cast<void *>(&ro_activate_instance));
}
} // namespace xv::runtime::durango
