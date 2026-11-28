#include "loader/image.hpp"
#include "formats/pe/inspect.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

namespace xv::loader {
using json = nlohmann::json;
namespace {
constexpr std::uint32_t section_execute = 0x20000000, section_write = 0x80000000;
constexpr std::uint64_t user_space_end = 0x7fff'ffff'0000;

std::uint64_t image_size(const json &report) {
    if (report.at("machine") != 0x8664)
        throw std::runtime_error("Only x86-64 images can be mapped");
    const std::uint64_t size = report.at("size_of_image");
    if (!size || size > image_limit)
        throw std::runtime_error("SizeOfImage is zero or exceeds the 1 GiB mapping limit");
    return size;
}
std::uint64_t round_up(std::uint64_t value, std::uint64_t unit) {
    return (value + unit - 1) / unit * unit;
}
} // namespace

std::string import_label(const json &symbol) {
    return symbol.contains("name")
               ? symbol.at("name").get<std::string>()
               : "#" + std::to_string(symbol.at("ordinal").get<std::uint64_t>());
}

MappedImage::MappedImage(std::span<const std::uint8_t> file, const MapOptions &options)
    : MappedImage(file, pe::inspect(file, pe::Detail::light), options) {}

MappedImage::MappedImage(std::span<const std::uint8_t> file, const json &pe,
                         const MapOptions &options)
    : size_(image_size(pe)), memory_(static_cast<std::size_t>(size_)) {
    const std::uint64_t page = host::page_size(), preferred = pe.at("image_base"),
                        alignment = pe.at("section_alignment"), headers = pe.at("size_of_headers");
    if (!alignment || alignment % page || size_ % alignment)
        throw std::runtime_error("Section alignment is not a multiple of the host page size, or "
                                 "SizeOfImage is not a multiple of it");
    if (headers > alignment)
        throw std::runtime_error("Headers extend beyond the first section alignment unit");
    base_ = options.guest_base.value_or(reinterpret_cast<std::uint64_t>(memory_.address()));
    if (options.guest_base && (base_ % 0x10000 || !base_))
        throw std::runtime_error("Guest base must be a nonzero multiple of 64 KiB");
    if (base_ > user_space_end || size_ > user_space_end - base_)
        throw std::runtime_error("Guest image does not fit below the user address limit");

    memory_.commit();
    auto *const bytes = reinterpret_cast<std::uint8_t *>(memory_.address());
    std::memcpy(bytes, file.data(), static_cast<std::size_t>(headers));
    if (options.executable && options.guest_base)
        throw std::runtime_error("Executable mappings require the host-address guest base");
    struct Range {
        std::uint64_t start, end;
        bool write, execute = false;
    };
    std::vector<Range> ranges{{0, headers, false}};
    auto sections = json::array();
    for (const auto &section : pe.at("sections")) {
        const std::uint64_t rva = section.at("rva"), virtual_size = section.at("virtual_size"),
                            raw_size = section.at("raw_size"), raw = section.at("raw_offset"),
                            flags = section.at("characteristics");
        const auto extent = virtual_size ? virtual_size : raw_size;
        if (!extent)
            continue;
        if (rva % alignment || rva < alignment || rva > size_ || extent > size_ - rva)
            throw std::runtime_error("Section is misaligned or outside SizeOfImage");
        const auto copied = std::min(raw_size, extent);
        if (copied) // range already validated by pe::inspect
            std::memcpy(bytes + rva, file.data() + raw, static_cast<std::size_t>(copied));
        const bool execute = options.executable && (flags & section_execute);
        if (execute && (flags & section_write))
            throw std::runtime_error("Section is both writable and executable");
        ranges.push_back({rva, rva + extent, (flags & section_write) != 0, execute});
        sections.push_back({{"name", section.at("name")},
                            {"guest_va", base_ + rva},
                            {"size", extent},
                            {"access", flags & section_write ? "read_write" : "read"},
                            {"execute_requested", (flags & section_execute) != 0},
                            {"execute_granted", execute}});
    }
    const auto mapped = [&](std::uint64_t rva, std::uint64_t length) {
        for (const auto &range : ranges)
            if (rva >= range.start && rva < range.end && length <= range.end - rva)
                return true;
        return false;
    };
    const auto slot = [&](std::uint64_t rva) {
        if (!mapped(rva, 8))
            throw std::runtime_error("Address slot lies outside mapped headers and sections");
        return bytes + rva;
    };

    const auto &contents = pe.at("directory_contents");
    const auto delta = base_ - preferred;
    std::uint64_t fixups = 0;
    if (delta && (pe.at("characteristics").get<std::uint64_t>() & 1))
        throw std::runtime_error(
            "Relocations were stripped; image only loads at its preferred base");
    if (contents.contains("base_relocations")) {
        // the entries are read from the mapped .reloc data, which pe::inspect has checked
        for (const auto &block : contents.at("base_relocations")) {
            const std::uint64_t page = block.at("page_rva"), data = block.at("data_rva"),
                                count = block.at("count");
            if (data > size_ || count * 2 > size_ - data)
                throw std::runtime_error("Base relocations lie outside the image");
            for (std::uint64_t i = 0; i < count; ++i) {
                std::uint16_t word;
                std::memcpy(&word, bytes + data + 2 * i, 2);
                const auto type = word >> 12;
                if (type == 0) // padding entry
                    continue;
                if (type != 10)
                    throw std::runtime_error("Unsupported base relocation type " +
                                             std::to_string(type) + " for an x64 image");
                auto *const target = slot(page + (word & 0xfff));
                std::uint64_t value;
                std::memcpy(&value, target, 8);
                value += delta;
                std::memcpy(target, &value, 8);
                ++fixups;
            }
        }
    }

    auto unresolved = json::array();
    std::uint64_t slots = 0, resolved = 0;
    auto opaque = json::array();
    auto delay_unresolved = json::array();
    auto delay_opaque = json::array();
    std::uint64_t delay_slots = 0, delay_resolved = 0;
    // One binding pass for a list of import modules: every IAT slot gets the resolver's
    // answer, or the unresolved placeholder. Delay-load descriptors bind exactly the same
    // way; the mapper runs no code, so the lazy __delayLoadHelper2 path stays out of scope.
    // The delay name table (INT) keeps its entries, so a host delay helper could still
    // resolve. Descriptor layout follows the Microsoft PE Format documentation,
    // "Delay-Load Import Tables (Image Only)":
    // https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#delay-load-import-tables-image-only
    const auto bind = [&](const json &modules, bool delay_load, json &unresolved_out,
                          json &opaque_out, std::uint64_t &slots_out, std::uint64_t &resolved_out) {
        for (const auto &module : modules) {
            const auto &name = module.at("module").get_ref<const std::string &>();
            if (!module.contains("symbols")) {
                // delay descriptors without a name table keep the status pe::inspect gave them
                if (delay_load)
                    opaque_out.push_back({{"module", name}, {"status", module.at("status")}});
                else
                    opaque_out.push_back(name);
                continue;
            }
            auto rva = module.at("iat_rva").get<std::uint64_t>();
            for (const auto &symbol : module.at("symbols")) {
                const auto label = import_label(symbol);
                auto value = options.resolve ? options.resolve(name, label) : std::nullopt;
                if (value) {
                    ++resolved_out;
                } else {
                    value = unresolved_tag | slots_out;
                    unresolved_out.push_back({{"module", name},
                                              {"symbol", label},
                                              {"slot_va", base_ + rva},
                                              {"slot_value", *value}});
                }
                std::memcpy(slot(rva), &*value, 8);
                ++slots_out;
                rva += 8;
            }
        }
    };
    if (contents.contains("imports"))
        bind(contents.at("imports"), false, unresolved, opaque, slots, resolved);
    if (contents.contains("delay_imports"))
        bind(contents.at("delay_imports"), true, delay_unresolved, delay_opaque, delay_slots,
             delay_resolved);

    // sections are page aligned (checked above) so they never share a page
    memory_.protect(0, static_cast<std::size_t>(size_), host::Access::none);
    for (const auto &range : ranges)
        memory_.protect(static_cast<std::size_t>(range.start),
                        static_cast<std::size_t>(round_up(range.end - range.start, page)),
                        range.write     ? host::Access::read_write
                        : range.execute ? host::Access::read_execute
                                        : host::Access::read);

    for (const auto &range : ranges)
        readable_.emplace_back(range.start, range.end);
    const std::uint64_t entry = pe.at("entry_rva");
    report_ = {
        {"preferred_base", preferred},
        {"guest_base", base_},
        {"guest_base_is_host_address", !options.guest_base},
        {"size_of_image", size_},
        {"sections", std::move(sections)},
        {"relocation", {{"delta", delta}, {"dir64_fixups", fixups}, {"applied", delta != 0}}},
        {"imports",
         {{"slots", slots},
          {"resolved", resolved},
          {"unresolved", std::move(unresolved)},
          {"modules_without_symbols", std::move(opaque)}}},
        {"delay_imports",
         {{"slots", delay_slots},
          {"resolved", delay_resolved},
          {"unresolved", std::move(delay_unresolved)},
          {"modules_without_symbols", std::move(delay_opaque)}}},
        {"tls", contents.contains("tls") ? "declared; template not instantiated, callbacks not run"
                                         : "none declared"},
        {"entry_va", entry ? json(base_ + entry) : json(nullptr)},
        {"execution", options.executable ? "code sections are executable; the mapper runs nothing"
                                         : "not attempted; no page of this mapping is executable"}};
}

std::uint64_t MappedImage::read64(std::uint64_t rva) const {
    for (const auto &[start, end] : readable_) {
        if (rva >= start && rva < end && end - rva >= 8) {
            std::uint64_t value;
            std::memcpy(&value, memory_.address() + rva, 8);
            return value;
        }
    }
    throw std::out_of_range("Guest read outside mapped headers and sections");
}
} // namespace xv::loader
