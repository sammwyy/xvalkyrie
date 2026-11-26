#define CATCH_CONFIG_MAIN
#include "core/settings.hpp"
#include "formats/pe/inspect.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

// The Agility SDK's Direct3D 12 runtime, in D3D12 next to the tests (see xv_deploy_agility),
// which xvboot runs titles on: the tests see what titles get, on any host
extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = XV_AGILITY_VERSION;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace {
// Catch2's own handler of faults goes first at each test and ends the run at any access
// violation, also one the code that made it catches itself: the Direct3D runtime, its debug
// layer and WARP may read memory that is not there and catch the fault, and the module makes
// the title's memory read-only to see its writes. It is off (CATCH_CONFIG_NO_WINDOWS_SEH, in
// CMakeLists.txt). A fault that nothing catches still ends the run, and says in which test.
char running[256]; // the test that runs
struct Running : Catch::TestEventListenerBase {
    using TestEventListenerBase::TestEventListenerBase;
    void testCaseStarting(const Catch::TestCaseInfo &test) override {
        strncpy_s(running, test.name.c_str(), _TRUNCATE);
    }
};
LPTOP_LEVEL_EXCEPTION_FILTER earlier_filter = nullptr;
LONG WINAPI uncaught(EXCEPTION_POINTERS *exception) {
    const auto *const record = exception->ExceptionRecord;
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(record->ExceptionAddress), &module))
        GetModuleFileNameW(module, name, MAX_PATH);
    std::fprintf(
        stderr, "\nexception 0x%08lx at %ls+0x%llx (data %llx) in the test \"%s\"\n",
        record->ExceptionCode, name,
        static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)),
        static_cast<unsigned long long>(
            record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0),
        running);
    std::fflush(stderr);
    return earlier_filter ? earlier_filter(exception) : EXCEPTION_CONTINUE_SEARCH;
}
const struct Uncaught {
    Uncaught() { earlier_filter = SetUnhandledExceptionFilter(&uncaught); }
} uncaught_filter;
} // namespace
CATCH_REGISTER_LISTENER(Running)

TEST_CASE("pe headers") {
    std::ifstream f("synthetic.pe", std::ios::binary);
    std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(f), {}};
    REQUIRE(data.size() == 1024);
    SECTION("basic fields") {
        const auto result = xv::pe::inspect(data);
        CHECK(result.at("image_base") == 0x140000000ull);
        CHECK(result.at("architecture") == "x86_64");
        CHECK(result.at("sections")[0].at("name") == ".rdata");
        CHECK(result.at("directory_descriptors")[4].contains("file_offset"));
        CHECK(result.at("console_target_verified") == false);
        data[152 + 68] = 14;
        CHECK(xv::pe::inspect(data).at("platform_indicator") ==
              "Xbox subsystem; generation unknown");
    }
    SECTION("truncated file") {
        for (std::size_t n : {0, 63, 151, 391, 431, 511, 1023})
            CHECK_THROWS(xv::pe::inspect(std::span(data).first(n)));
    }
    SECTION("too many directories") {
        data[152 + 108] = 17;
        CHECK_THROWS(xv::pe::inspect(data));
    }
    SECTION("section offset out of range") {
        data[392 + 23] = 0xff;
        CHECK_THROWS(xv::pe::inspect(data));
    }
}
TEST_CASE("settings") {
    std::istringstream partial(R"({"vsync":false})");
    const auto settings = xv::read_settings(partial);
    CHECK_FALSE(settings.vsync);
    std::istringstream defaults("{}");
    CHECK(xv::read_settings(defaults).vsync);
    for (auto invalid : {"[]", R"({"vsync":"false"})", R"({"typo":true})", "{"}) {
        std::istringstream input(invalid);
        CHECK_THROWS(xv::read_settings(input));
    }
}
