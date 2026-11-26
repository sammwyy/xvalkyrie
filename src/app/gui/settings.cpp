#include "app/gui/settings.hpp"
#include "app/gui/widgets.hpp"
#include "app/version.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <exception>

namespace xv::ui {
namespace {
struct PageEntry {
    Settings::Page page;
    const char *name;
};
constexpr std::array pages{PageEntry{Settings::Page::input, "Input"},
                           PageEntry{Settings::Page::graphics, "Graphics"},
                           PageEntry{Settings::Page::profiles, "Profiles"},
                           PageEntry{Settings::Page::console, "Console"},
                           PageEntry{Settings::Page::libraries, "Library"},
                           PageEntry{Settings::Page::debug, "Debug"},
                           PageEntry{Settings::Page::about, "About"}};

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
// an entry of the page list
bool page_entry(const char *name, bool shown) {
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0, .5f});
    const bool pressed = ImGui::Selectable((std::string("   ") + name).c_str(), shown, 0, {0, 34});
    ImGui::PopStyleVar();
    return pressed;
}
// a path that can be selected and copied
void path_field(const char *id, const std::filesystem::path &path) {
    auto text = utf8(path);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText(id, text.data(), text.size() + 1, ImGuiInputTextFlags_ReadOnly);
}

// The controls in the order of input::Control, as the groups a person looks for them in.
struct ControlGroup {
    const char *name;
    input::Control first, end;
};
constexpr std::array control_groups{
    ControlGroup{"Buttons", input::Control::a, input::Control::dpad_up},
    ControlGroup{"D-pad", input::Control::dpad_up, input::Control::left_shoulder},
    ControlGroup{"Bumpers, triggers and stick buttons", input::Control::left_shoulder,
                 input::Control::left_stick_up},
    ControlGroup{"Left stick", input::Control::left_stick_up, input::Control::right_stick_up},
    ControlGroup{"Right stick", input::Control::right_stick_up, input::Control::count},
};

// The languages and regions a console can be set to, by their locale names.
struct LanguageEntry {
    const char *tag, *name;
};
constexpr LanguageEntry languages[] = {
    {"ar-SA", "Arabic (Saudi Arabia)"},
    {"zh-CN", "Chinese (Simplified, China)"},
    {"zh-HK", "Chinese (Traditional, Hong Kong)"},
    {"zh-TW", "Chinese (Traditional, Taiwan)"},
    {"cs-CZ", "Czech (Czechia)"},
    {"da-DK", "Danish (Denmark)"},
    {"nl-NL", "Dutch (Netherlands)"},
    {"en-AU", "English (Australia)"},
    {"en-CA", "English (Canada)"},
    {"en-GB", "English (United Kingdom)"},
    {"en-US", "English (United States)"},
    {"fi-FI", "Finnish (Finland)"},
    {"fr-CA", "French (Canada)"},
    {"fr-FR", "French (France)"},
    {"de-DE", "German (Germany)"},
    {"el-GR", "Greek (Greece)"},
    {"he-IL", "Hebrew (Israel)"},
    {"hu-HU", "Hungarian (Hungary)"},
    {"it-IT", "Italian (Italy)"},
    {"ja-JP", "Japanese (Japan)"},
    {"ko-KR", "Korean (Korea)"},
    {"nb-NO", "Norwegian (Norway)"},
    {"pl-PL", "Polish (Poland)"},
    {"pt-BR", "Portuguese (Brazil)"},
    {"pt-PT", "Portuguese (Portugal)"},
    {"ru-RU", "Russian (Russia)"},
    {"sk-SK", "Slovak (Slovakia)"},
    {"es-AR", "Spanish (Argentina)"},
    {"es-CL", "Spanish (Chile)"},
    {"es-CO", "Spanish (Colombia)"},
    {"es-MX", "Spanish (Mexico)"},
    {"es-ES", "Spanish (Spain)"},
    {"sv-SE", "Swedish (Sweden)"},
    {"tr-TR", "Turkish (Turkey)"},
};

constexpr const char *age_groups[] = {"Unknown", "Child", "Teen", "Adult"};
constexpr float profile_field = 240;
// The profile in use at a glance: its initial on a disc, the gamertag, what tells it from
// the others and its gamerscore.
void profile_card(const Fonts &fonts, const ConsoleSettings &profile) {
    constexpr float height = 84, radius = 26;
    auto *const list = ImGui::GetWindowDrawList();
    const auto min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy({width, height});
    list->AddRectFilled(min, {min.x + width, min.y + height}, IM_COL32(255, 255, 255, 14), 8);
    list->AddRect(min, {min.x + width, min.y + height}, IM_COL32(255, 255, 255, 26), 8);
    const ImVec2 center{min.x + 18 + radius, min.y + height / 2};
    list->AddCircleFilled(center, radius, color::fade(color::scarlet, .9f), 48);
    // the first character of the gamertag, all the bytes of it
    std::size_t bytes = profile.gamertag.empty() ? 0 : 1;
    while (bytes < profile.gamertag.size() &&
           (static_cast<unsigned char>(profile.gamertag[bytes]) & 0xc0) == 0x80)
        ++bytes;
    auto initial = profile.gamertag.substr(0, bytes);
    if (bytes == 1)
        initial[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(initial[0])));
    auto *const heading = fonts.heading;
    const auto letter = heading->CalcTextSizeA(heading->FontSize, FLT_MAX, 0, initial.c_str());
    list->AddText(heading, heading->FontSize, {center.x - letter.x / 2, center.y - letter.y / 2},
                  IM_COL32_WHITE, initial.c_str());
    const auto score = "G  " + std::to_string(profile.gamerscore);
    const float pill =
        fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, 0, score.c_str()).x + 14;
    badge(list, fonts, {min.x + width - pill - 18, min.y + (height - badge_height) / 2},
          {score.c_str(), IM_COL32(255, 255, 255, 22), color::text});
    const float left = center.x + radius + 16, room = width - (left - min.x) - pill - 30;
    ImGui::PushFont(heading);
    const auto gamertag = fitted(profile.gamertag, room);
    ImGui::PopFont();
    list->AddText(heading, heading->FontSize, {left, min.y + 16}, color::text, gamertag.c_str());
    ImGui::PushFont(fonts.small);
    const auto line = fitted(profile.profile + "  \xc2\xb7  XUID " + profile.xbox_user_id +
                                 "  \xc2\xb7  " + age_groups[std::min(profile.age_group, 3u)],
                             room);
    ImGui::PopFont();
    list->AddText(fonts.small, fonts.small->FontSize, {left, min.y + 16 + heading->FontSize + 4},
                  color::muted, line.c_str());
}
} // namespace

void Settings::draw(const Fonts &fonts, const Host &host, Library library,
                    ConsoleSettings &profile) {
    if (!input_shown())
        binding_.reset();
    if (!open_)
        return;
    const auto *const screen = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(
        {std::min(860.f, screen->WorkSize.x - 40), std::min(580.f, screen->WorkSize.y - 60)},
        ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(screen->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    ImGui::SetNextWindowSizeConstraints({560, 360}, {FLT_MAX, FLT_MAX});
    if (raise_)
        ImGui::SetNextWindowFocus();
    raise_ = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    const bool visible = ImGui::Begin("Settings", &open_, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (visible) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 14});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, .28f));
        ImGui::BeginChild("Pages", {176, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        for (const auto &entry : pages)
            if (page_entry(entry.name, entry.page == page_))
                page_ = entry.page;
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {26, 14});
        ImGui::BeginChild("Page", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        switch (page_) {
        case Page::input:
            input(fonts, host);
            break;
        case Page::graphics:
            graphics(fonts, host);
            break;
        case Page::profiles:
            profiles(fonts, host, profile);
            break;
        case Page::console:
            console(fonts, host, profile);
            break;
        case Page::libraries:
            libraries(fonts, host, library);
            break;
        case Page::debug:
            debug(fonts);
            break;
        case Page::about:
            about(fonts, host);
            break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

// Where the gamepad's readings come from and which key is which control. Changes apply
// at once.
void Settings::input(const Fonts &fonts, const Host &host) {
    auto settings = input::settings();
    const auto changed = [&] {
        input::configure(settings);
        input_notice_.clear();
        try {
            if (host.input_saved)
                host.input_saved();
        } catch (const std::exception &e) {
            input_notice_ = std::string("Not saved: ") + e.what();
        }
    };
    section(fonts, "Capture");
    if (begin_settings("capture")) {
        setting(fonts, "Method",
                settings.capture == input::Capture::direct
                    ? "Direct: the gamepad (XInput) and the keyboard, read from the system."
                    : "Window: the keyboard, from this window's events.");
        static constexpr const char *methods[] = {"Window", "Direct"};
        int method = settings.capture == input::Capture::direct ? 1 : 0;
        if (segmented("method", methods, method)) {
            settings.capture = method ? input::Capture::direct : input::Capture::window;
            changed();
        }
        setting(fonts, "Host gamepad", "With direct capture, also read the gamepad plugged in.");
        ImGui::BeginDisabled(settings.capture != input::Capture::direct);
        if (ImGui::Checkbox("##gamepad", &settings.host_gamepad))
            changed();
        ImGui::EndDisabled();
        end_settings();
    }

    section(fonts, "Keyboard");
    note(fonts, "Click a key to change it, then press the new one. Escape leaves it as it was.");
    if (binding_ && pressed_) {
        if (pressed_ != 0x1b)
            settings.keys[*binding_] = pressed_;
        binding_.reset();
        pressed_ = 0;
        changed();
    }
    constexpr float key_width = 132;
    for (const auto &group : control_groups) {
        ImGui::Dummy({0, 4});
        ImGui::PushFont(fonts.small);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::muted), "%s", group.name);
        ImGui::PopFont();
        if (!ImGui::BeginTable(group.name, 2, ImGuiTableFlags_SizingStretchSame))
            continue;
        for (auto index = static_cast<std::size_t>(group.first);
             index < static_cast<std::size_t>(group.end); ++index) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(index));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input::control_name(static_cast<input::Control>(index)));
            const float clear = ImGui::GetFrameHeight();
            ImGui::SameLine(std::max(120.f, ImGui::GetContentRegionAvail().x - key_width - clear -
                                                ImGui::GetStyle().ItemSpacing.x - 12));
            const bool waiting = binding_ && *binding_ == index;
            const auto label =
                waiting ? std::string("Press a key...") : input::key_name(settings.keys[index]);
            if (waiting ? primary_button(label.c_str(), {key_width, 0})
                        : ImGui::Button(label.c_str(), {key_width, 0})) {
                binding_ = index;
                pressed_ = 0;
            }
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Button, 0);
            if (ImGui::Button("\xc3\x97", {clear, 0})) { // a multiplication sign as a cross
                settings.keys[index] = 0;
                binding_.reset();
                changed();
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Clear");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy({0, 8});
    if (ImGui::Button("Restore default keys")) {
        settings.keys = input::default_settings().keys;
        binding_.reset();
        changed();
    }
    if (!input_notice_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", input_notice_.c_str());
    }
}

void Settings::graphics(const Fonts &fonts, const Host &host) {
    const auto backend = graphics::selected_backend();
    section(fonts, "Rendering");
    if (begin_settings("rendering")) {
        setting(fonts, "Backend",
                backend == graphics::Backend::vulkan
                    ? "Vulkan: Direct3D 11 translated to Vulkan, shipped with xvalkyrie."
                    : "Direct3D 11: the host's own driver.");
        static constexpr const char *backends[] = {"Direct3D 11", "Vulkan"};
        int chosen = backend == graphics::Backend::vulkan ? 1 : 0;
        if (segmented("backend", backends, chosen)) {
            const auto picked = chosen ? graphics::Backend::vulkan : graphics::Backend::d3d11;
            graphics_notice_.clear();
            if (!graphics::backend_renders_titles(picked))
                graphics_notice_ = std::string("Vulkan needs ") +
                                   std::string(graphics::vulkan_d3d11_module) + " and " +
                                   std::string(graphics::vulkan_dxgi_module) +
                                   " next to xvalkyrie.exe. Build with -DXV_DXVK=ON.";
            else {
                graphics::select_backend(picked);
                try {
                    if (host.graphics_saved)
                        host.graphics_saved();
                } catch (const std::exception &e) {
                    graphics_notice_ = std::string("Not saved: ") + e.what();
                }
            }
        }
        const auto fullscreen = graphics::fullscreen_mode();
        setting(fonts, "Full screen",
                fullscreen == graphics::Fullscreen::native
                    ? "Native: F11 gives the display to the title, where the host can."
                    : "Borderless: F11 makes the title's window as large as the display.");
        static constexpr const char *modes[] = {"Borderless", "Native"};
        int mode = fullscreen == graphics::Fullscreen::native ? 1 : 0;
        if (segmented("fullscreen", modes, mode)) {
            graphics::select_fullscreen_mode(mode ? graphics::Fullscreen::native
                                                  : graphics::Fullscreen::borderless);
            graphics_notice_.clear();
            try {
                if (host.graphics_saved)
                    host.graphics_saved();
            } catch (const std::exception &e) {
                graphics_notice_ = std::string("Not saved: ") + e.what();
            }
        }
        end_settings();
    }
    note(fonts, "Applies to the next title started.");
    if (!graphics_notice_.empty())
        ImGui::TextWrapped("%s", graphics_notice_.c_str());
}

// The profiles, each a user of the console: which one is in use, what it says of itself,
// its folder, and making and deleting them. Changes apply at once.
void Settings::profiles(const Fonts &fonts, const Host &host, ConsoleSettings &settings) {
    if (!profile_loaded_) {
        profile_ = {};
        std::snprintf(profile_.name, sizeof profile_.name, "%s", settings.profile.c_str());
        std::snprintf(profile_.gamertag, sizeof profile_.gamertag, "%s", settings.gamertag.c_str());
        std::snprintf(profile_.user_id, sizeof profile_.user_id, "%s",
                      settings.xbox_user_id.c_str());
        profile_loaded_ = true;
    }
    const auto changed = [&] {
        profile_notice_.clear();
        try {
            if (host.console_saved)
                host.console_saved();
        } catch (const std::exception &e) {
            profile_notice_ = std::string("Not saved: ") + e.what();
        }
    };
    ImGui::Dummy({0, 6});
    profile_card(fonts, settings);
    ImGui::Dummy({0, 10});
    const auto names = profile_names();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Profile in use");
    ImGui::SameLine(0, 14);
    ImGui::SetNextItemWidth(profile_field);
    if (ImGui::BeginCombo("##profile", settings.profile.c_str())) {
        for (const auto &name : names)
            if (ImGui::Selectable(name.c_str(), name == settings.profile) &&
                name != settings.profile && use_profile(settings, name)) {
                profile_loaded_ = false;
                changed();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (primary_button("New profile...")) {
        new_ = {};
        std::snprintf(new_.user_id, sizeof new_.user_id, "%s", new_xbox_user_id().c_str());
        new_notice_.clear();
        ImGui::OpenPopup("New profile");
    }
    new_profile(fonts, host, settings);

    section(fonts, "Identity");
    if (begin_settings("identity")) {
        setting(fonts, "Name",
                "Of the profile and its folder. 1 to 32 letters, digits, spaces, - or _.");
        ImGui::SetNextItemWidth(profile_field);
        ImGui::InputText("##name", profile_.name, sizeof profile_.name);
        if (ImGui::IsItemDeactivatedAfterEdit() && settings.profile != profile_.name) {
            try {
                rename_profile(settings, profile_.name);
                changed();
            } catch (const std::exception &e) {
                profile_notice_ = e.what();
                profile_loaded_ = false;
            }
        }
        setting(fonts, "Gamertag", "1 to 15 characters, shown by titles as the player's name.");
        ImGui::SetNextItemWidth(profile_field);
        ImGui::InputText("##gamertag", profile_.gamertag, sizeof profile_.gamertag);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (valid_gamertag(profile_.gamertag)) {
                settings.gamertag = profile_.gamertag;
                changed();
            } else {
                profile_notice_ = "A gamertag has 1 to 15 characters.";
                profile_loaded_ = false;
            }
        }
        setting(fonts, "User ID",
                "The Xbox user ID (XUID). Some titles name their saves after it, so changing "
                "it hides those saves.");
        const float random = ImGui::CalcTextSize("New").x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SetNextItemWidth(profile_field - random - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##xuid", profile_.user_id, sizeof profile_.user_id,
                         ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (valid_xbox_user_id(profile_.user_id)) {
                settings.xbox_user_id = profile_.user_id;
                changed();
            } else {
                profile_notice_ = "A user ID has 1 to 20 digits.";
                profile_loaded_ = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("New")) {
            settings.xbox_user_id = new_xbox_user_id();
            profile_loaded_ = false;
            changed();
        }
        ImGui::SetItemTooltip("A random user ID");
        setting(fonts, "Gamerscore");
        ImGui::SetNextItemWidth(profile_field);
        ImGui::InputScalar("##gamerscore", ImGuiDataType_U32, &settings.gamerscore);
        if (ImGui::IsItemDeactivatedAfterEdit())
            changed();
        setting(fonts, "Age group", "Titles with age ratings or chat limits look at it.");
        int age = static_cast<int>(std::min(settings.age_group, 3u));
        if (segmented("age", age_groups, age)) {
            settings.age_group = static_cast<std::uint32_t>(age);
            changed();
        }
        end_settings();
    }

    section(fonts, "Files");
    const auto folder = profile_directory(settings.profile);
    bool remove = false;
    if (begin_settings("files")) {
        setting(fonts, "Folder", "The profile's settings and the saves made as this user.");
        if (ImGui::Button("Open folder"))
            show_folder(folder);
        ImGui::SameLine();
        if (ImGui::Button("Open saves"))
            show_folder(folder / "saves");
        setting(fonts, "Delete",
                names.size() > 1 ? "The profile and its saves. They cannot be brought back."
                                 : "The only profile there is cannot be deleted.");
        ImGui::BeginDisabled(names.size() <= 1);
        remove = ImGui::Button("Delete profile...");
        ImGui::EndDisabled();
        end_settings();
    }
    path_field("##folder", folder);
    if (remove)
        ImGui::OpenPopup("Delete profile");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    if (ImGui::BeginPopupModal("Delete profile", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("Delete the profile \"%s\" and all of its saves?", settings.profile.c_str());
        note(fonts, "They cannot be brought back.");
        ImGui::Dummy({0, 8});
        if (primary_button("Delete", {110, 0})) {
            delete_profile(settings.profile);
            for (const auto &name : names)
                if (name != settings.profile && use_profile(settings, name))
                    break;
            profile_loaded_ = false;
            changed();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {110, 0}))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Dummy({0, 4});
    note(fonts, "Applies to the next title started.");
    if (!profile_notice_.empty())
        ImGui::TextWrapped("%s", profile_notice_.c_str());
}

// Asks for what a new profile is to be, then makes it and puts it in use.
void Settings::new_profile(const Fonts &fonts, const Host &host, ConsoleSettings &settings) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    ImGui::SetNextWindowSize({500, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22, 18});
    const bool shown = ImGui::BeginPopupModal("New profile", nullptr, ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar();
    if (!shown)
        return;
    note(fonts, "A profile is a user of the console, with saves of its own.");
    if (begin_settings("new")) {
        setting(fonts, "Name", "1 to 32 letters, digits, spaces, - or _.");
        ImGui::SetNextItemWidth(profile_field);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##name", new_.name, sizeof new_.name);
        setting(fonts, "Gamertag", "1 to 15 characters. The name, if left empty.");
        ImGui::SetNextItemWidth(profile_field);
        ImGui::InputText("##gamertag", new_.gamertag, sizeof new_.gamertag);
        setting(fonts, "User ID", "The Xbox user ID (XUID), 1 to 20 digits.");
        const float random = ImGui::CalcTextSize("New").x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SetNextItemWidth(profile_field - random - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##xuid", new_.user_id, sizeof new_.user_id,
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::Button("New"))
            std::snprintf(new_.user_id, sizeof new_.user_id, "%s", new_xbox_user_id().c_str());
        ImGui::SetItemTooltip("A random user ID");
        setting(fonts, "Gamerscore");
        ImGui::SetNextItemWidth(profile_field);
        ImGui::InputScalar("##gamerscore", ImGuiDataType_U32, &new_.gamerscore);
        setting(fonts, "Age group");
        segmented("age", age_groups, new_.age);
        end_settings();
    }
    if (!new_notice_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, color::scarlet);
        ImGui::TextWrapped("%s", new_notice_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy({0, 8});
    if (primary_button("Create profile", {150, 0})) {
        auto made = settings;
        made.profile = new_.name;
        made.gamertag = *new_.gamertag ? new_.gamertag : made.profile.substr(0, 15);
        made.xbox_user_id = new_.user_id;
        made.gamerscore = new_.gamerscore;
        made.age_group = static_cast<std::uint32_t>(new_.age);
        try {
            create_profile(made);
            settings = made;
            profile_loaded_ = false;
            profile_notice_.clear();
            if (host.console_saved)
                host.console_saved();
            ImGui::CloseCurrentPopup();
        } catch (const std::exception &e) {
            new_notice_ = e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {110, 0}))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Settings::console(const Fonts &fonts, const Host &host, ConsoleSettings &settings) {
    const auto changed = [&] {
        console_notice_.clear();
        try {
            if (host.console_saved)
                host.console_saved();
        } catch (const std::exception &e) {
            console_notice_ = std::string("Not saved: ") + e.what();
        }
    };
    section(fonts, "Console model");
    if (begin_settings("model")) {
        // the models of one generation, as a row of choices
        const auto choose = [&](const char *id, Target target, ConsoleModel &model) {
            std::vector<ConsoleModel> models;
            std::vector<std::string> names;
            for (const auto each : console_models)
                if (model_target(each) == target) {
                    models.push_back(each);
                    names.emplace_back(model_name(each));
                }
            std::vector<const char *> labels;
            for (const auto &name : names)
                labels.push_back(name.c_str());
            int chosen =
                static_cast<int>(std::find(models.begin(), models.end(), model) - models.begin());
            if (segmented(id, labels, chosen) && models[chosen] != model) {
                model = models[chosen];
                changed();
            }
        };
        setting(fonts, "Xbox Series X|S titles",
                "The console they are told they run on, which some size their memory and "
                "picture by.");
        choose("series model", Target::scarlett, settings.series_model);
        setting(fonts, "Xbox One titles", "The console they are told they run on.");
        choose("one model", Target::durango, settings.one_model);
        end_settings();
    }
    section(fonts, "Language");
    if (begin_settings("language")) {
        setting(fonts, "Language and region",
                "What titles are told the console's are, whatever this PC's are.");
        const auto host_choice = "Windows (" + host_language() + ")";
        const auto shown = [&](const std::string &tag) {
            for (const auto &entry : languages)
                if (tag == entry.tag)
                    return std::string(entry.name);
            return tag;
        };
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##language", settings.language.empty()
                                                ? host_choice.c_str()
                                                : shown(settings.language).c_str())) {
            if (ImGui::Selectable(host_choice.c_str(), settings.language.empty()) &&
                !settings.language.empty()) {
                settings.language.clear();
                changed();
            }
            for (const auto &entry : languages)
                if (ImGui::Selectable(entry.name, settings.language == entry.tag) &&
                    settings.language != entry.tag) {
                    settings.language = entry.tag;
                    changed();
                }
            ImGui::EndCombo();
        }
        end_settings();
    }
    section(fonts, "Accessories");
    if (begin_settings("accessories")) {
        setting(fonts, "Headset",
                "The user has an audio device of their own: this PC's communications output.");
        if (ImGui::Checkbox("##headset", &settings.headset))
            changed();
        end_settings();
    }
    section(fonts, "System keyboard");
    if (begin_settings("keyboard")) {
        setting(fonts, "When a title asks for text",
                settings.keyboard_dialog ? "A window on this PC asks for it."
                                         : "The title gets the text it suggested, unchanged.");
        static constexpr const char *modes[] = {"Ask", "Default text"};
        int mode = settings.keyboard_dialog ? 0 : 1;
        if (segmented("keyboard", modes, mode)) {
            settings.keyboard_dialog = mode == 0;
            changed();
        }
        end_settings();
    }
    section(fonts, "System storage");
    if (begin_settings("system storage")) {
        const auto folder = settings.system_storage.u8string();
        setting(fonts, "Folder of the console's apps",
                "Optional. The dashboard and the system's packages, each unpacked in a folder "
                "of its own. Apps find the packages they depend on there, and File > Boot "
                "from Menu starts the dashboard.");
        ImGui::TextWrapped("%s", folder.empty() ? "Not set"
                                                : reinterpret_cast<const char *>(folder.c_str()));
        if (ImGui::Button("Choose...") && host.choose_system_storage)
            host.choose_system_storage();
        if (!folder.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                settings.system_storage.clear();
                changed();
            }
            if (system_menu(settings).empty())
                ImGui::TextDisabled("No dashboard (Xbox.Dashboard) in this folder.");
        }
        end_settings();
    }
    note(fonts, "Applies to the next title started.");
    if (!console_notice_.empty())
        ImGui::TextWrapped("%s", console_notice_.c_str());
}

void Settings::libraries(const Fonts &fonts, const Host &host, Library library) {
    auto &settings = library.settings;
    section(fonts, "Game folders");
    note(fonts, "Folders with unpacked titles or XVD/XVC packages. Packages are listed but "
                "cannot be started yet.");
    ImGui::Dummy({0, 4});
    if (settings.directories.empty())
        ImGui::TextDisabled("No folders yet.");
    const auto &style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Change...").x + ImGui::CalcTextSize("Remove").x +
                          4 * style.FramePadding.x + style.ItemSpacing.x;
    if (!settings.directories.empty() && begin_settings("folders")) {
        for (std::size_t index = 0; index < settings.directories.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const auto path = utf8(settings.directories[index]);
            ImGui::TextUnformatted(
                fitted(path, ImGui::GetContentRegionAvail().x - buttons - 24).c_str());
            ImGui::SetItemTooltip("%s", path.c_str());
            ImGui::TableNextColumn();
            if (ImGui::Button("Change...") && host.change_folder)
                host.change_folder(index);
            ImGui::SameLine();
            const bool remove = ImGui::Button("Remove");
            ImGui::PopID();
            if (remove) {
                settings.directories.erase(settings.directories.begin() +
                                           static_cast<std::ptrdiff_t>(index));
                if (host.libraries_saved)
                    host.libraries_saved(true);
                break;
            }
        }
        end_settings();
    }
    ImGui::Dummy({0, 6});
    if (primary_button("Add folder...") && host.add_folder)
        host.add_folder();
    ImGui::SameLine();
    if (ImGui::Button("Rescan") && host.rescan)
        host.rescan();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (library.scanning)
        ImGui::TextDisabled("Looking for titles...");
    else
        ImGui::TextDisabled("%zu %s found", library.titles,
                            library.titles == 1 ? "title" : "titles");
    if (!library.notice.empty())
        ImGui::TextWrapped("%s", library.notice.c_str());

    section(fonts, "View");
    if (begin_settings("view")) {
        setting(fonts, "Layout", "Cards with the logo of each title, or one title per line.");
        static constexpr const char *layouts[] = {"Grid", "List"};
        int layout = settings.grid ? 0 : 1;
        if (segmented("layout", layouts, layout)) {
            settings.grid = layout == 0;
            if (host.libraries_saved)
                host.libraries_saved(false);
        }
        end_settings();
    }
}

// What is left out or looked into in a run, for every title: the runtime's XV_* variables.
// A title's own, in its properties, take the place of these; the environment's of both.
void Settings::debug(const Fonts &fonts) {
    if (!debug_loaded_) {
        debug_ = load_debug_settings();
        debug_fields_.clear();
        debug_loaded_ = true;
    }
    note(fonts, "For finding out what a title needs and what it trips over. Some make titles "
                "slower or leave out what they draw. A title's properties can change them for "
                "that title alone.");
    ImGui::Dummy({0, 4});
    if (ImGui::Button("Reset all")) {
        debug_.clear();
        debug_fields_.clear();
        try {
            save_debug_settings(debug_);
            debug_notice_.clear();
        } catch (const std::exception &e) {
            debug_notice_ = std::string("Not saved: ") + e.what();
        }
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu set", debug_.size());
    if (debug_options_ui(fonts, debug_, debug_fields_)) {
        try {
            save_debug_settings(debug_);
            debug_notice_.clear();
        } catch (const std::exception &e) {
            debug_notice_ = std::string("Not saved: ") + e.what();
        }
    }
    ImGui::Dummy({0, 4});
    note(fonts, "Applies to the next title started. A variable already set in the environment "
                "is kept over these.");
    if (!debug_notice_.empty())
        ImGui::TextWrapped("%s", debug_notice_.c_str());
}

void Settings::about(const Fonts &fonts, const Host &host) {
    ImGui::Dummy({0, 6});
    const auto extent = wordmark_size(fonts, 52);
    wordmark(ImGui::GetWindowDrawList(), fonts, ImGui::GetCursorScreenPos(), 52);
    ImGui::Dummy(extent);
    ImGui::TextDisabled("%s", version);
    ImGui::Dummy({0, 6});
    ImGui::TextWrapped("Experimental Xbox One and Xbox Series X|S compatibility layer.");
    note(fonts, "Not affiliated with or endorsed by Microsoft. GPL-3.0 license.");
    section(fonts, "Updates");
    const auto updates = host.updates();
    ImGui::BeginDisabled(updates.checking || updates.installing);
    if (ImGui::Button(updates.checking ? "Checking..." : "Check for Updates"))
        host.check_updates();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(updates.available.empty() || updates.installing || updates.blocked);
    const auto label = updates.installing          ? "Updating..."
                       : updates.available.empty() ? std::string("Update")
                                                   : "Update to " + updates.available;
    if (primary_button(label.c_str()))
        host.install_update();
    ImGui::EndDisabled();
    if (!updates.available.empty() && updates.blocked)
        note(fonts, "Close the running title to update.");
    else if (!updates.message.empty())
        note(fonts, updates.message.c_str());
    section(fonts, "Files");
    note(fonts, "Configuration");
    path_field("##configuration", config_file());
    ImGui::Dummy({0, 2});
    note(fonts, "Session logs, the files to attach when reporting a problem");
    path_field("##logs", config_directory() / "logs");
}
} // namespace xv::ui
