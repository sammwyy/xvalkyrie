#include "formats/pe/inspect.hpp"
#include "loader/image.hpp"
#include <catch2/catch.hpp>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
using xv::loader::MappedImage;
constexpr std::uint64_t preferred = 0x140000000;
std::vector<std::uint8_t> fixture(const char *name = "synthetic.pe") {
    std::ifstream input(name, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    REQUIRE(!bytes.empty());
    return bytes;
}
void put(std::vector<std::uint8_t> &bytes, std::size_t pos, std::uint64_t value,
         std::size_t width = 4) {
    for (std::size_t i = 0; i < width; ++i)
        bytes.at(pos + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t get32(const std::vector<std::uint8_t> &bytes, std::size_t pos) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(bytes.at(pos + i)) << (8 * i);
    return value;
}
} // namespace

TEST_CASE("map at preferred base") {
    const MappedImage image(fixture(), {.guest_base = preferred, .resolve = {}});
    const auto &report = image.report();
    CHECK(report.at("relocation").at("applied") == false);
    CHECK(report.at("relocation").at("dir64_fixups") == 1);
    CHECK(image.read64(0x1140) == preferred + 0x1180); // TLS callback pointer, a DIR64 target
    CHECK(image.read64(0) % 0x10000 == 0x5a4d);        // headers are mapped at RVA 0
    CHECK(report.at("sections")[0].at("guest_va") == preferred + 0x1000);
    CHECK(report.at("sections")[0].at("access") == "read");
    CHECK(report.at("sections")[0].at("execute_granted") == false);
    CHECK(report.at("entry_va").is_null());
    // outside of any section
    CHECK_THROWS_AS(image.read64(0x11fc), std::out_of_range);
    CHECK_THROWS_AS(image.read64(0x200), std::out_of_range);
    CHECK_THROWS_AS(image.read64(0x2000), std::out_of_range);
}

TEST_CASE("rebase") {
    SECTION("explicit base") {
        const MappedImage image(fixture(), {.guest_base = 0x7ff600000000, .resolve = {}});
        CHECK(image.read64(0x1140) == 0x7ff600001180);
        CHECK(image.report().at("relocation").at("delta") == 0x7ff600000000 - preferred);
    }
    SECTION("host address base") {
        const MappedImage image(fixture());
        CHECK(image.report().at("guest_base_is_host_address") == true);
        CHECK(image.read64(0x1140) == image.guest_base() + 0x1180);
    }
    SECTION("lower base") {
        const MappedImage image(fixture(), {.guest_base = 0x10000, .resolve = {}});
        CHECK(image.read64(0x1140) == 0x11180);
    }
}

TEST_CASE("import slots") {
    SECTION("no resolver") {
        const MappedImage image(fixture(), {.guest_base = preferred, .resolve = {}});
        const auto &imports = image.report().at("imports");
        CHECK(imports.at("slots") == 2);
        CHECK(imports.at("resolved") == 0);
        CHECK(imports.at("unresolved")[0].at("symbol") == "Probe");
        CHECK(imports.at("unresolved")[1].at("symbol") == "#17");
        CHECK(imports.at("unresolved")[1].at("slot_va") == preferred + 0x1058);
        for (std::uint64_t i = 0; i < 2; ++i) {
            const auto value = image.read64(0x1050 + 8 * i);
            CHECK(value == (xv::loader::unresolved_tag | i));
            // non-canonical with 48 and 57 bit addresses
            CHECK((value >> 47) != 0);
            CHECK((value >> 56) != 0);
            CHECK((value >> 47) != 0x1ffff);
        }
        CHECK(image.read64(0x1038) == 0x1068); // the lookup table keeps its name RVA
    }
    SECTION("with resolver") {
        const MappedImage image(
            fixture(), {.guest_base = preferred,
                        .resolve = [](std::string_view module,
                                      std::string_view symbol) -> std::optional<std::uint64_t> {
                            if (module == "fixture.dll" && symbol == "#17")
                                return 0x7000'0000'1000;
                            return std::nullopt;
                        }});
        CHECK(image.read64(0x1058) == 0x7000'0000'1000);
        CHECK(image.read64(0x1050) == xv::loader::unresolved_tag);
        CHECK(image.report().at("imports").at("resolved") == 1);
        CHECK(image.report().at("imports").at("unresolved").size() == 1);
    }
}

TEST_CASE("delay import slots") {
    // corpus "delay": KERNEL32.dll imports regularly; dxcompiler_xs.dll (#1) and
    // title-local.dll (Probe) delayed
    const auto pe = xv::pe::inspect(fixture("delay.pe"));
    const auto &delayed = pe.at("directory_contents").at("delay_imports");
    REQUIRE(delayed.size() == 2);
    const auto iat = [&](std::size_t index) {
        return delayed[index].at("iat_rva").get<std::uint64_t>();
    };
    SECTION("no resolver: placeholders and per-symbol entries") {
        const MappedImage image(fixture("delay.pe"), {.guest_base = preferred, .resolve = {}});
        const auto &report = image.report().at("delay_imports");
        CHECK(report.at("slots") == 2);
        CHECK(report.at("resolved") == 0);
        CHECK(report.at("unresolved")[0].at("module") == "dxcompiler_xs.dll");
        CHECK(report.at("unresolved")[0].at("symbol") == "#1");
        CHECK(report.at("unresolved")[0].at("slot_va") == preferred + iat(0));
        CHECK(report.at("unresolved")[1].at("module") == "title-local.dll");
        CHECK(report.at("unresolved")[1].at("symbol") == "Probe");
        CHECK(report.at("unresolved")[1].at("slot_va") == preferred + iat(1));
        for (std::uint64_t i = 0; i < 2; ++i) {
            const auto value = image.read64(iat(i));
            CHECK(value == (xv::loader::unresolved_tag | i));
            CHECK((value >> 47) != 0); // non-canonical, so calling one faults
        }
        CHECK(image.report().at("imports").at("slots") == 2); // regular imports as before
    }
    SECTION("with resolver: bound slot, missing symbol keeps the placeholder") {
        const MappedImage image(fixture("delay.pe"),
                                {.guest_base = preferred,
                                 .resolve = [](std::string_view module, std::string_view symbol)
                                     -> std::optional<std::uint64_t> {
                                     if (module == "title-local.dll" && symbol == "Probe")
                                         return 0x7000'0000'2000;
                                     return std::nullopt;
                                 }});
        CHECK(image.read64(iat(1)) == 0x7000'0000'2000);
        CHECK(image.read64(iat(0)) == xv::loader::unresolved_tag);
        const auto &report = image.report().at("delay_imports");
        CHECK(report.at("slots") == 2);
        CHECK(report.at("resolved") == 1);
        CHECK(report.at("unresolved").size() == 1);
        CHECK(report.at("unresolved")[0].at("module") == "dxcompiler_xs.dll");
    }
    SECTION("delay descriptor without a name table keeps its status") {
        auto bytes = fixture("delay.pe");
        const auto directory = get32(bytes, 152 + 112 + 8 * 13); // delay import directory RVA
        put(bytes, 512 + directory - 0x1000 + 32 + 16, 0);       // second descriptor's name table
        const MappedImage image(bytes, {.guest_base = preferred, .resolve = {}});
        const auto &report = image.report().at("delay_imports");
        CHECK(report.at("slots") == 1); // only dxcompiler_xs.dll has symbols to bind
        REQUIRE(report.at("modules_without_symbols").size() == 1);
        CHECK(report.at("modules_without_symbols")[0].at("module") == "title-local.dll");
        CHECK(report.at("modules_without_symbols")[0].at("status") ==
              "delay descriptor without name table; symbols unknown");
    }
}

TEST_CASE("an image that has no relocations, and did not have them stripped, loads anywhere") {
    auto bytes = fixture();
    put(bytes, 264 + 5 * 8, 0, 8);
    CHECK_NOTHROW(MappedImage(bytes, {.guest_base = 0x150000000, .resolve = {}}));
}

TEST_CASE("unmappable images") {
    auto bytes = fixture();
    std::optional<std::uint64_t> base = 0x150000000;
    SECTION("unaligned base") { base = 0x150001000; }
    SECTION("base out of range") { base = 0x7fffffff0000; }
    SECTION("relocations stripped") { put(bytes, 150, 0x23, 2); }
    SECTION("unsupported relocation type") { put(bytes, 512 + 0x108, 0x3140, 2); }
    SECTION("relocation target unmapped") { put(bytes, 512 + 0x100, 0x1800); }
    SECTION("relocation target past section end") { put(bytes, 512 + 0x108, 0xa1fc, 2); }
    SECTION("IAT outside section") { put(bytes, 512 + 16, 0x11f8); }
    SECTION("section alignment below page size") { put(bytes, 152 + 32, 512); }
    SECTION("SizeOfImage too small") { put(bytes, 152 + 56, 4096); }
    SECTION("SizeOfImage too big") { put(bytes, 152 + 56, 0x80000000); }
    SECTION("unaligned section") { put(bytes, 392 + 12, 0x1200); }
    SECTION("wrong machine") { put(bytes, 132, 0xaa64, 2); }
    CHECK_THROWS(MappedImage(bytes, {.guest_base = base, .resolve = {}}));
}
