#include "runtime/modules/memory.hpp"
#include "runtime/common/runtime.hpp"
#include <catch2/catch.hpp>
#include <cstdint>
#include <windows.h>

// The title's physical pages as xmem.dll gives them: allocated, mapped at an address of the
// title's, and mapped again elsewhere from their numbers.
namespace {
using namespace xv::runtime;

using Allocate = BOOL WINAPI(ULONGLONG, ULONG_PTR *, ULONG_PTR *);
using Free = BOOL WINAPI(ULONG_PTR, ULONG_PTR *);
using Map = void *WINAPI(void *, ULONG_PTR, ULONG_PTR *);
using Scatter = BOOL WINAPI(void **, ULONG_PTR, ULONG_PTR *);
using Reserve = void *WINAPI(void *, SIZE_T, DWORD, ULONGLONG, DWORD);
using Release = BOOL WINAPI(void *, SIZE_T, DWORD);
constexpr std::size_t page = 1 << 16;
constexpr DWORD large_pages = 0x20000000;

Registry &xmem_registry() {
    static Registry registry;
    static const bool installed = (modules::install_xmem(registry), true);
    static_cast<void>(installed);
    return registry;
}
template <class Function> Function *xmem(const char *name) {
    auto *const found = reinterpret_cast<Function *>(xmem_registry().find("xmem.dll", name));
    REQUIRE(found);
    return found;
}
} // namespace

TEST_CASE("a physical page is numbered by its first 4 KiB frame") {
    ULONG_PTR count = 4, pages[4]{};
    REQUIRE(xmem<Allocate>("XMemAllocatePhysicalPages")(0, &count, pages));
    REQUIRE(count == 4);
    for (const auto frame : pages)
        CHECK(frame % 16 == 0);
    CHECK(pages[1] != pages[0]);
    CHECK(pages[2] != pages[1]);
    CHECK(pages[3] != pages[2]);

    // mapped where the title reserved them
    auto *const reserve = xmem<Reserve>("XMemVirtualAlloc");
    auto *const base = static_cast<std::uint8_t *>(
        reserve(nullptr, 4 * page, MEM_RESERVE | large_pages, 0, PAGE_READWRITE));
    REQUIRE(base);
    REQUIRE(xmem<Map>("XMemMapPhysicalPages")(base, 4, pages) == base);
    for (std::size_t i = 0; i < 4; ++i)
        base[i * page] = static_cast<std::uint8_t>(0x10 + i);

    // mapped again from the numbers, as a title that keeps them shifted right by 4 does: each
    // page is the one it was, not a neighbour's
    auto *const again = static_cast<std::uint8_t *>(
        reserve(nullptr, 2 * page, MEM_RESERVE | large_pages, 0, PAGE_READWRITE));
    REQUIRE(again);
    void *addresses[2]{again, again + page};
    ULONG_PTR kept[2]{pages[1] >> 4 << 4, pages[2] >> 4 << 4};
    auto *const scatter = xmem<Scatter>("XMemMapPhysicalPagesScatter");
    REQUIRE(scatter(addresses, 2, kept));
    CHECK(again[0] == 0x11);
    CHECK(again[page] == 0x12);

    // a frame within a page is no page
    ULONG_PTR within = pages[0] + 1;
    CHECK_FALSE(scatter(addresses, 1, &within));

    CHECK(scatter(addresses, 2, nullptr));
    auto *const release = reinterpret_cast<Release *>(xmem_registry().find_api("VirtualFree"));
    REQUIRE(release);
    CHECK(release(again, 0, MEM_RELEASE));
    CHECK(release(base, 0, MEM_RELEASE));
    CHECK(xmem<Free>("XMemFreePhysicalPages")(4, pages));
}
