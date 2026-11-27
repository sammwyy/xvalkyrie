#include "app/gui/properties.hpp"
#include "app/gui/widgets.hpp"
#include "launcher/debug.hpp"
#include "launcher/launch.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <imgui.h>
#include <string_view>
#include <tuple>
#include <vector>

namespace xv::ui {
namespace {
using json = nlohmann::json;
const json none;
// `value[key]`, null if it is not there
const json &at(const json &value, const char *key) {
    return value.is_object() && value.contains(key) ? value.at(key) : none;
}
std::string hex(const json &value) {
    if (!value.is_number_unsigned() && !value.is_number_integer())
        return "-";
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", value.get<unsigned long long>());
    return text;
}
// a value for people: a string as it is, a number in decimal, "-" for what is missing
std::string plain(const json &value) {
    if (value.is_null())
        return "-";
    if (value.is_string())
        return value.get<std::string>().empty() ? "-" : value.get<std::string>();
    return value.dump();
}
// One line of a two-column table: what it is, and its value, which a click copies.
void line(const char *name, const std::string &value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", name);
    ImGui::TableNextColumn();
    ImGui::PushID(name);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 18));
    if (ImGui::Selectable(value.c_str()))
        ImGui::SetClipboardText(value.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Click to copy");
    ImGui::PopID();
}
bool begin_lines(const char *id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit))
        return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
constexpr ImGuiTableFlags grid = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_SizingStretchProp;
bool contains(std::string_view text, std::string_view wanted) {
    return std::search(text.begin(), text.end(), wanted.begin(), wanted.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           }) != text.end();
}
} // namespace

void Properties::show(const std::filesystem::path &path, const std::string &name) {
    open_ = raise_ = true;
    if (loading_.valid())
        return; // one at a time; the one being read is shown when it is
    name_ = name;
    path_ = path;
    report_ = nullptr;
    raw_.clear();
    notice_.clear();
    fields_.clear();
    try {
        title_ = load_title_debug_settings(path);
        all_ = load_debug_settings();
    } catch (const std::exception &e) {
        notice_ = e.what();
    }
    shaders_.clear();
    shaders_read_ = false;
    selected_.clear();
    checked_.clear();
    source_text_.clear();
    source_of_.clear();
    loading_ = std::async(std::launch::async, [path] { return title_properties(path); });
}

void Properties::overview() const {
    const auto &manifest = at(report_, "manifest"), &image = at(report_, "image"),
               &bound = at(report_, "bound");
    for (const auto &[part, problem] : at(report_, "errors").items())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::scarlet), "%s: %s", part.c_str(),
                           plain(problem).c_str());
    if (begin_lines("title")) {
        line("Name", plain(at(manifest, "display_name")) +
                         (at(manifest, "display_name_reference").is_string()
                              ? "  (" + plain(at(manifest, "display_name_reference")) + ")"
                              : std::string()));
        line("Package", plain(at(manifest, "name")));
        line("Version", plain(at(manifest, "version")));
        if (!at(manifest, "publisher").is_null())
            line("Publisher", plain(at(manifest, "publisher")));
        line("Title ID", plain(at(manifest, "title_id")));
        if (!at(manifest, "store_id").is_null())
            line("Store ID", plain(at(manifest, "store_id")));
        line("Console", at(report_, "target").is_null()
                            ? "unknown (" + plain(at(report_, "target_indication")) + ")"
                            : plain(at(report_, "target")));
        line("Path", plain(at(report_, "path")));
        line("Executable", plain(at(report_, "executable")));
        line("Manifest folder", plain(at(report_, "manifest_directory")));
        line("Saved games", plain(at(report_, "save_directory")));
        line("Console system", plain(at(report_, "system_directory")));
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Image");
    if (begin_lines("image")) {
        line("Format", plain(at(image, "format")) + ", " + plain(at(image, "architecture")));
        line("File size", plain(at(image, "file_size")) + " bytes");
        line("Preferred base", hex(at(image, "image_base")));
        line("Size of image", hex(at(image, "size_of_image")));
        line("Entry point", "image+" + hex(at(image, "entry_rva")));
        line("Subsystem", plain(at(image, "subsystem")));
        line("DLL characteristics", hex(at(image, "dll_characteristics")));
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Imports");
    if (begin_lines("bound")) {
        line("Imports", std::to_string(at(report_, "imports").size()) + " in " +
                            std::to_string(at(report_, "modules").size()) + " modules");
        line("XValkyrie", plain(at(bound, "own")));
        line("Host APIs", plain(at(bound, "host")));
        line("Title's own DLLs", plain(at(bound, "title")));
        line("Console system", plain(at(bound, "system")));
        line("Not implemented", plain(at(bound, "missing")));
        ImGui::EndTable();
    }
}

void Properties::mapping() const {
    const auto &map = at(report_, "mapping"), &relocation = at(map, "relocation");
    if (map.is_null()) {
        ImGui::TextDisabled("The image could not be mapped: %s",
                            plain(at(at(report_, "errors"), "mapping")).c_str());
        return;
    }
    if (begin_lines("map")) {
        line("Preferred base", hex(at(map, "preferred_base")));
        line("Mapped at", hex(at(map, "guest_base")));
        line("Size", hex(at(map, "size_of_image")));
        line("Entry point", hex(at(map, "entry_va")));
        line("Relocation", at(relocation, "applied") == true
                               ? plain(at(relocation, "dir64_fixups")) + " fixups, moved by " +
                                     hex(at(relocation, "delta"))
                               : std::string("not applied"));
        line("TLS", plain(at(map, "tls")));
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Where a look at the image put it this time; a run maps it again.");
    ImGui::SeparatorText("Sections");
    if (!ImGui::BeginTable("sections", 5, grid))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const auto *title : {"Name", "Address", "End", "Size", "Access"})
        ImGui::TableSetupColumn(title);
    ImGui::TableHeadersRow();
    for (const auto &section : at(map, "sections")) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(plain(at(section, "name")).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(hex(at(section, "guest_va")).c_str());
        ImGui::TableNextColumn();
        if (at(section, "guest_va").is_number() && at(section, "size").is_number())
            ImGui::TextUnformatted(hex(json(at(section, "guest_va").get<unsigned long long>() +
                                            at(section, "size").get<unsigned long long>()))
                                       .c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(hex(at(section, "size")).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s%s", plain(at(section, "access")).c_str(),
                    at(section, "execute_requested") == true ? ", code" : "");
    }
    ImGui::EndTable();
}

void Properties::libraries() const {
    if (!ImGui::BeginTable("modules", 8, grid))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Module", 0, 3);
    ImGui::TableSetupColumn("Kind", 0, 1.6f);
    for (const auto *title : {"Imports", "XValkyrie", "Host", "Title", "System", "Missing"})
        ImGui::TableSetupColumn(title, 0, .9f);
    ImGui::TableHeadersRow();
    for (const auto &module : at(report_, "modules")) {
        const bool missing = at(module, "missing").is_number() && at(module, "missing") != 0;
        ImGui::TableNextRow();
        if (missing)
            ImGui::PushStyleColor(ImGuiCol_Text, color::scarlet);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(plain(at(module, "module")).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s%s", plain(at(module, "category")).c_str(),
                    at(module, "delay_load") == true ? ", delay" : "");
        for (const auto *key : {"imports", "own", "host", "title", "system", "missing"}) {
            ImGui::TableNextColumn();
            // a count of none says nothing: left blank, the ones that are there stand out
            if (at(module, key).is_number() && at(module, key) != 0)
                ImGui::TextUnformatted(plain(at(module, key)).c_str());
        }
        if (missing)
            ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

void Properties::imports() {
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##filter", "Module or symbol", filter_, sizeof filter_);
    ImGui::SameLine();
    ImGui::Checkbox("Not implemented only", &missing_only_);
    const auto &all = at(report_, "imports");
    std::vector<const json *> shown;
    const std::string_view wanted = filter_;
    for (const auto &import : all) {
        if (missing_only_ && at(import, "origin") != "missing")
            continue;
        if (!wanted.empty() && !contains(plain(at(import, "module")), wanted) &&
            !contains(plain(at(import, "symbol")), wanted))
            continue;
        shown.push_back(&import);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu", shown.size(), all.size());
    if (!ImGui::BeginTable("imports", 3, grid))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Module", 0, 2);
    ImGui::TableSetupColumn("Symbol", 0, 3);
    ImGui::TableSetupColumn("Binds to", 0, 1);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step())
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto &import = *shown[static_cast<std::size_t>(index)];
            const auto origin = plain(at(import, "origin"));
            ImGui::TableNextRow();
            if (origin == "missing")
                ImGui::PushStyleColor(ImGuiCol_Text, color::scarlet);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(plain(at(import, "module")).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(plain(at(import, "symbol")).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(origin == "own" ? "XValkyrie" : origin.c_str());
            if (origin == "missing")
                ImGui::PopStyleColor();
        }
    ImGui::EndTable();
}

void Properties::save_title() {
    try {
        save_title_debug_settings(path_, title_);
        notice_.clear();
    } catch (const std::exception &e) {
        notice_ = std::string("Not saved: ") + e.what();
    }
}

// The title's own settings of a page's groups, over those of the same page of Settings.
void Properties::options(const Fonts &fonts, OptionGroups groups, const char *page) {
    note(fonts, (std::string("This title's own settings. Default is what Settings > ") + page +
                 " has for every title. They apply from its next start.")
                    .c_str());
    if (!notice_.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::scarlet), "%s", notice_.c_str());
    ImGui::Dummy({0, 4});
    // the settings of this tab's groups, which All to default puts back and the count counts
    const auto here = [&](const DebugOption &option) {
        return groups.only ? std::string_view(option.group) == groups.only
                           : !user_group(option.group);
    };
    std::size_t own = 0;
    for (const auto &option : debug_options())
        own += here(option) && title_.debug.contains(option.key);
    if (ImGui::Button("All to default")) {
        for (const auto &option : debug_options())
            if (here(option)) {
                title_.debug.erase(option.key);
                fields_.erase(option.key);
            }
        save_title();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (groups.only)
        ImGui::TextDisabled("%zu of its own", own);
    else
        ImGui::TextDisabled("%zu of its own, %zu shaders with a stand-in", own,
                            title_.stand_ins.size());
    if (debug_options_ui(fonts, title_.debug, fields_, &all_, groups))
        save_title();
    ImGui::Dummy({0, 4});
    const auto file = title_settings_file(path_).u8string();
    note(fonts, ("Kept in " + std::string(file.begin(), file.end())).c_str());
}

namespace {
constexpr const char *stage_names[] = {"Vertex", "Pixel",    "Compute", "Hull",
                                       "Domain", "Geometry", "Mesh"};
std::string shader_key(const runtime::modules::JournalShader &shader) {
    return shader.hash + "-" + "vpc123m"[static_cast<int>(shader.stage)];
}
} // namespace

// The shaders the title's earlier runs asked for, from its journal in the shader cache: what
// became of each with this version of the translator, and a stand-in for the ones picked.
void Properties::shaders() {
    using State = Shader::State;
    if (reading_.valid() &&
        reading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto read = reading_.get();
        // of another title, if another was shown meanwhile: this one's is read again
        if (reading_for_ == path_) {
            shaders_ = std::move(read);
            std::sort(shaders_.begin(), shaders_.end(), [](const Shader &a, const Shader &b) {
                return std::tie(a.stage, a.hash) < std::tie(b.stage, b.hash);
            });
        } else {
            shaders_read_ = false;
        }
    }
    if (!shaders_read_ && !reading_.valid()) {
        shaders_read_ = true;
        reading_for_ = path_;
        reading_ = std::async(std::launch::async, [path = path_] {
            return runtime::modules::journal_shaders(shader_cache_folder(),
                                                     title_shader_journal(path));
        });
    }
    if (check_.valid() && check_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        for (auto &[file, why] : check_.get())
            checked_[file] = std::move(why);
    if (source_.valid() && source_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        source_text_ = source_.get();
    if (reading_.valid()) {
        const auto at = ImGui::GetCursorScreenPos();
        spinner(ImGui::GetWindowDrawList(), {at.x + 12, at.y + 12}, 8);
        ImGui::SetCursorScreenPos({at.x + 32, at.y + 4});
        ImGui::TextDisabled("Reading the shaders of earlier runs");
        return;
    }
    if (shaders_.empty()) {
        ImGui::TextDisabled("No shaders yet: a run of the title notes the ones it asks for.");
        return;
    }
    // what each has come to: made, not made, or not tried by this version; for the last,
    // what a translation here said
    const auto state_of = [&](const Shader &shader) {
        if (shader.state != State::untried)
            return shader.state;
        const auto found = checked_.find(shader.file.string());
        return found == checked_.end() ? State::untried
               : found->second.empty() ? State::made
                                       : State::failed;
    };
    const auto why_of = [&](const Shader &shader) -> std::string {
        if (shader.state == State::failed)
            return shader.why;
        const auto found = checked_.find(shader.file.string());
        return found == checked_.end() ? std::string() : found->second;
    };
    std::size_t made = 0, failed = 0, untried = 0;
    for (const auto &shader : shaders_) {
        const auto state = state_of(shader);
        ++(state == State::made ? made : state == State::failed ? failed : untried);
    }
    ImGui::Text("%zu shaders: %zu translated, %zu not, %zu not tried by translator v%d; %zu with "
                "a stand-in",
                shaders_.size(), made, failed, untried,
                runtime::modules::shader_translator_version(), title_.stand_ins.size());
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##shader filter", "Hash or reason", shader_filter_,
                             sizeof shader_filter_);
    ImGui::SameLine();
    static constexpr const char *stages[] = {"All stages", "Vertex", "Pixel", "Compute", "Mesh"};
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("##stage", &stage_shown_, stages, IM_ARRAYSIZE(stages));
    ImGui::SameLine();
    static constexpr const char *states[] = {"Any state", "Translated", "Not translated",
                                             "Not tried", "With a stand-in"};
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##state", &state_shown_, states, IM_ARRAYSIZE(states));
    ImGui::SameLine();
    if (check_.valid()) {
        ImGui::TextDisabled("Translating %zu of %zu", checking_->done.load(),
                            checking_->total.load());
    } else {
        if (ImGui::Button("Translate the rest")) {
            // the ones neither made nor noted, translated here for what stops them
            std::vector<Shader> wanted;
            for (const auto &shader : shaders_)
                if (shader.state == State::untried && !checked_.count(shader.file.string()))
                    wanted.push_back(shader);
            checking_ = std::make_shared<Checking>();
            checking_->total = wanted.size();
            check_ = std::async(std::launch::async, [wanted, checking = checking_] {
                std::map<std::string, std::string> results;
                for (const auto &shader : wanted) {
                    std::string why;
                    const auto source = runtime::modules::journal_shader_source(shader, &why);
                    results[shader.file.string()] = source        ? std::string()
                                                    : why.empty() ? std::string("?")
                                                                  : why;
                    ++checking->done;
                }
                return results;
            });
        }
        ImGui::SetItemTooltip("Translates those this version has not tried yet, without "
                              "compiling them, to tell what stops the ones that are not");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
        shaders_read_ = false;
    std::vector<const Shader *> shown;
    const std::string_view wanted = shader_filter_;
    static constexpr runtime::modules::ShaderStage by_choice[] = {
        runtime::modules::ShaderStage::vertex, runtime::modules::ShaderStage::pixel,
        runtime::modules::ShaderStage::compute, runtime::modules::ShaderStage::mesh};
    for (const auto &shader : shaders_) {
        const auto state = state_of(shader);
        const bool stood_in = title_.stand_ins.count(shader_key(shader)) != 0;
        if (stage_shown_ && shader.stage != by_choice[stage_shown_ - 1])
            continue;
        if ((state_shown_ == 1 && state != State::made) ||
            (state_shown_ == 2 && state != State::failed) ||
            (state_shown_ == 3 && state != State::untried) || (state_shown_ == 4 && !stood_in))
            continue;
        if (!wanted.empty() && !contains(shader.hash, wanted) && !contains(why_of(shader), wanted))
            continue;
        shown.push_back(&shader);
    }
    const Shader *picked = nullptr;
    for (const auto &shader : shaders_)
        if (shader.file.string() == selected_)
            picked = &shader;
    if (ImGui::BeginTable("shaders", 6, grid, {0, picked ? -220.f : 0.f})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Stand-in", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Stage", 0, .8f);
        ImGui::TableSetupColumn("Hash", 0, 1.6f);
        ImGui::TableSetupColumn("Heaps", 0, .6f);
        ImGui::TableSetupColumn("State", 0, 1.1f);
        ImGui::TableSetupColumn("What stops it", 0, 4);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step())
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto &shader = *shown[static_cast<std::size_t>(index)];
                const auto key = shader_key(shader), file = shader.file.string();
                const auto state = state_of(shader);
                ImGui::TableNextRow();
                ImGui::PushID(file.c_str());
                ImGui::TableNextColumn();
                bool stood_in = title_.stand_ins.count(key) != 0;
                if (ImGui::Checkbox("##stand-in", &stood_in)) {
                    if (stood_in)
                        title_.stand_ins.insert(key);
                    else
                        title_.stand_ins.erase(key);
                    save_title();
                }
                ImGui::SetItemTooltip("A stand-in that draws nothing in its place, from the "
                                      "title's next start");
                ImGui::TableNextColumn();
                if (ImGui::Selectable(
                        stage_names[static_cast<int>(shader.stage)], selected_ == file,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    selected_ = selected_ == file ? std::string() : file;
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(shader.hash.substr(0, 16).c_str());
                ImGui::SetItemTooltip("%s", key.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(shader.heaps ? "yes" : "");
                ImGui::TableNextColumn();
                if (state == State::failed)
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::scarlet),
                                       "Not translated");
                else
                    ImGui::TextUnformatted(state == State::made ? "Translated" : "Not tried");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(why_of(shader).c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    if (picked)
        shader_details(*picked);
}

// the one picked in the list: what it is, and the HLSL made for it
void Properties::shader_details(const Shader &shader) {
    ImGui::BeginChild("shader", {0, 0}, ImGuiChildFlags_Borders);
    const auto key = shader_key(shader), file = shader.file.string();
    const char *const model = shader.model == runtime::modules::HostShaderModel::sm6 ? "SM 6"
                              : shader.model == runtime::modules::HostShaderModel::automatic
                                  ? "SM 5 or 6"
                                  : "SM 5";
    ImGui::Text("%s shader %s, %s, %ju bytes%s%s%s", stage_names[static_cast<int>(shader.stage)],
                key.c_str(), model, shader.bytes, shader.heaps ? ", reads the heaps" : "",
                shader.root.empty() ? "" : ", root signature ", shader.root.c_str());
    if (ImGui::Button("Copy hash"))
        ImGui::SetClipboardText(key.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy entry path"))
        ImGui::SetClipboardText(file.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(source_.valid());
    if (ImGui::Button(source_.valid() ? "Translating..." : "Show HLSL")) {
        source_of_ = file;
        source_text_.clear();
        source_ = std::async(std::launch::async, [shader] {
            std::string why;
            const auto source = runtime::modules::journal_shader_source(shader, &why);
            return source ? *source : "// not translated: " + why;
        });
    }
    ImGui::EndDisabled();
    if (source_of_ == file && !source_text_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Copy HLSL"))
            ImGui::SetClipboardText(source_text_.c_str());
        ImGui::InputTextMultiline("##hlsl", source_text_.data(), source_text_.size() + 1,
                                  ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::EndChild();
}

void Properties::draw(const Fonts &fonts) {
    if (loading_.valid() &&
        loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            report_ = loading_.get();
        } catch (const std::exception &e) {
            report_ = json{{"errors", {{"title", e.what()}}}};
        }
        raw_ = report_.dump(2, ' ', false, json::error_handler_t::replace);
    }
    if (!open_)
        return;
    const auto *screen = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(
        {std::min(820.f, screen->WorkSize.x - 40), std::min(600.f, screen->WorkSize.y - 60)},
        ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(screen->GetWorkCenter(), ImGuiCond_Appearing, {.5f, .5f});
    if (raise_)
        ImGui::SetNextWindowFocus();
    raise_ = false;
    if (ImGui::Begin(("Properties - " + name_ + "###properties").c_str(), &open_,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        if (loading_.valid()) {
            const auto at = ImGui::GetCursorScreenPos();
            spinner(ImGui::GetWindowDrawList(), {at.x + 12, at.y + 12}, 8);
            ImGui::SetCursorScreenPos({at.x + 32, at.y + 4});
            ImGui::TextDisabled("Reading the title");
        } else if (ImGui::BeginTabBar("parts")) {
            if (ImGui::BeginTabItem("Overview")) {
                ImGui::BeginChild("overview");
                overview();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            // what is drawn and what the title reaches, for playing; then its make-up after
            const struct {
                const char *tab;
                OptionGroups groups;
            } pages[] = {{"Render", {render_group}}, {"Game", {game_group}}};
            for (const auto &page : pages)
                if (ImGui::BeginTabItem(page.tab)) {
                    ImGui::BeginChild(page.tab);
                    options(fonts, page.groups, page.tab);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            if (ImGui::BeginTabItem("Mapping")) {
                mapping();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Libraries")) {
                libraries();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Imports")) {
                imports();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Debug")) {
                ImGui::BeginChild("Debug");
                options(fonts, {}, "Debug");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Shaders")) {
                shaders();
                ImGui::EndTabItem();
            }
            // everything above and the manifest as it was read, to select and copy
            if (ImGui::BeginTabItem("Raw")) {
                ImGui::InputTextMultiline("##raw", raw_.data(), raw_.size() + 1,
                                          ImGui::GetContentRegionAvail(),
                                          ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}
} // namespace xv::ui
