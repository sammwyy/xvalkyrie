#include "app/gui/desktop.hpp"
#include "app/gui/file_dialog.hpp"
#include "app/gui/keys.hpp"
#include "app/gui/logo.hpp"
#include "app/gui/properties.hpp"
#include "app/gui/settings.hpp"
#include "app/gui/theme.hpp"
#include "app/gui/widgets.hpp"
#include "app/version.hpp"
#include "host/files.hpp"
#include "launcher/launch.hpp"
#include "launcher/library.hpp"
#include "launcher/update.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <future>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace xv {
namespace {
struct Glfw {
    Glfw() {
        if (!glfwInit())
            throw std::runtime_error("GLFW initialization failed; check the display connection");
    }
    ~Glfw() { glfwTerminate(); }
};
struct Interface {
    explicit Interface(GLFWwindow *window) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
            ImGui::DestroyContext();
            throw std::runtime_error("ImGui window backend initialization failed");
        }
        if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            throw std::runtime_error("ImGui renderer initialization failed");
        }
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        fonts = ui::load_fonts();
        ui::apply_style();
    }
    ~Interface() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    ui::Fonts fonts;
};
struct Guest {
    std::thread thread;
    std::atomic<bool> done = false;
    std::uint32_t code = 0;
    std::exception_ptr error;
    // what its process printed and the window has not taken yet
    std::mutex mutex;
    std::string output;
    void printed(std::string_view piece) {
        const std::lock_guard lock(mutex);
        for (const char c : piece)
            if (c != '\r')
                output += c;
    }
};
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

class Desktop {
  public:
    Desktop(GLFWwindow *window, const Interface &ui, const DesktopHooks &hooks)
        : window_(window), ui_(ui), hooks_(hooks), libraries_(hooks.libraries),
          console_(hooks.console) {
        glfwSetWindowUserPointer(window, this);
        glfwSetDropCallback(window, [](GLFWwindow *target, int count, const char **paths) {
            if (count > 0) {
                const std::string text = paths[0];
                static_cast<Desktop *>(glfwGetWindowUserPointer(target))->dropped_ =
                    std::filesystem::path(std::u8string(text.begin(), text.end()));
            }
        });
        check_for_updates();
        settings_host_.add_folder = [this] { pick_folder(Dialog::add_library); };
        settings_host_.choose_system_storage = [this] { pick_folder(Dialog::system_storage); };
        settings_host_.change_folder = [this](std::size_t index) {
            pick_folder(Dialog::replace_library, index);
        };
        settings_host_.rescan = [this] { refresh_library(); };
        settings_host_.check_updates = [this] { check_for_updates(); };
        settings_host_.install_update = [this] {
            if (update_ && !busy())
                updating_ =
                    std::async(std::launch::async, [update = *update_] { install_update(update); });
        };
        settings_host_.updates = [this] {
            return ui::Settings::Updates{update_check_.valid(), updating_.valid(), busy(),
                                         update_ ? update_->version : std::string{},
                                         update_message_};
        };
        settings_host_.libraries_saved = [this](bool rescan) { save_libraries(rescan); };
        settings_host_.input_saved = hooks.input_changed;
        settings_host_.graphics_saved = hooks.graphics_changed;
        settings_host_.console_saved = [this] {
            if (hooks_.console_changed)
                hooks_.console_changed(console_);
        };
        // what the last scan found is on screen at once; the scan then brings it up to date
        titles_ = cached_library(libraries_.directories, library_cache_file());
        refresh_library();
    }
    ~Desktop() {
        // A guest that is still running cannot be stopped; it ends with the process.
        if (guest_)
            guest_->thread.detach();
    }
    int exit_code() const { return exit_code_; }
    // A key of the main window, before the interface sees it: with the options waiting
    // for one, the next key pressed becomes a binding.
    void key(int glfw_key, bool pressed) {
        const int code = ui::virtual_key(glfw_key);
        if (code && pressed && settings_.wants_key())
            settings_.key(code);
    }
    // something on screen moves without input: the loading bar. Not while a title runs:
    // it has a window of its own, and this one is at rest.
    bool animating() const {
        return loading_.valid() || discovery_.valid() || logos_.loading() || properties_.loading();
    }
    // the native dialog is modal: the window stays open until it is dismissed
    bool dialog_open() const { return dialog_.valid(); }

    // Reading and checking a title takes a while, so it happens off the window thread;
    // start() picks the result up.
    void open(const std::filesystem::path &path) {
        if (busy())
            return;
        notice_.clear();
        outcome_.clear();
        ran_ = false;
        title_.reset();
        name_ = utf8(path.filename());
        if (const auto it = std::find_if(titles_.begin(), titles_.end(),
                                         [&](const auto &title) { return title.path == path; });
            it != titles_.end()) {
            name_ = it->name;
            splash_ = it->splash;
        } else {
            splash_ = title_splash(path);
        }
        glfwSetWindowTitle(window_, ("XValkyrie - " + name_).c_str());
        path_ = path;
        loading_ = std::async(std::launch::async, hooks_.load, path);
    }
    void start() {
        const auto path = path_;
        try {
            title_ = loading_.get();
        } catch (const std::exception &e) {
            notice_ = std::string("Could not load the title: ") + e.what();
            exit_code_ = 1;
            return;
        }
        exit_code_ = title_->startable() ? 0 : 1;
        if (!title_->startable())
            return;
        // shared so that a guest outliving the window still has somewhere to report to
        log_.clear();
        log_extent_.reset();
        started_ = std::chrono::steady_clock::now();
        guest_ = std::make_shared<Guest>();
        guest_->thread = std::thread([guest = guest_, run = hooks_.run, path] {
            try {
                guest->code =
                    run(path, [&guest](std::string_view piece) { guest->printed(piece); });
            } catch (...) {
                guest->error = std::current_exception();
            }
            guest->done = true;
        });
    }

    void frame() {
        poll();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
            pick();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, ImGuiInputFlags_RouteGlobal))
            close_app();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
            glfwSetWindowShouldClose(window_, true);
        menu();
        status_bar();
        content();
        settings_.draw(ui_.fonts, settings_host_,
                       {libraries_, titles_.size(), discovery_.valid(), library_notice_}, console_);
        properties_.draw(ui_.fonts);
    }

  private:
    bool busy() const { return guest_ || dialog_.valid() || loading_.valid(); }
    void check_for_updates() {
        update_check_ = std::async(std::launch::async, [] { return find_update(release); });
    }
    enum class Dialog { file, folder, add_library, replace_library, system_storage };
    void pick() {
        if (!busy()) {
            dialog_kind_ = Dialog::file;
            dialog_ = pick_title_file(window_);
        }
    }
    void pick_folder(Dialog kind, std::size_t index = 0) {
        if (!busy()) {
            dialog_kind_ = kind;
            replace_index_ = index;
            dialog_ = pick_title_folder(window_);
        }
    }
    void refresh_library() {
        if (discovery_.valid()) {
            refresh_pending_ = true;
            return;
        }
        discovery_ = std::async(std::launch::async, [roots = libraries_.directories] {
            return discover_library(roots, library_cache_file());
        });
    }
    void save_libraries(bool rescan = true) {
        try {
            if (hooks_.libraries_changed)
                hooks_.libraries_changed(libraries_);
            library_notice_.clear();
        } catch (const std::exception &e) {
            library_notice_ = std::string("Not saved: ") + e.what();
        }
        if (rescan) {
            // a folder that was removed takes its titles with it before the scan is over
            if (!discovery_.valid())
                titles_ = cached_library(libraries_.directories, library_cache_file());
            refresh_library();
        }
    }
    // Close App: ends the title that runs, whose log stays on screen; with none running,
    // puts away the one that was opened, log and all, and goes back to the library.
    bool can_close_app() const {
        return guest_ ? static_cast<bool>(hooks_.close_running) : title_ && !busy();
    }
    void close_app() {
        if (!can_close_app())
            return;
        if (guest_)
            hooks_.close_running();
        else
            close_title();
    }
    void close_title() {
        ran_ = false;
        title_.reset();
        notice_.clear();
        exit_code_ = 0;
        glfwSetWindowTitle(window_, "XValkyrie");
    }
    void poll() {
        if (dialog_.valid() &&
            dialog_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                const auto path = dialog_.get();
                if (!path.empty()) {
                    if (dialog_kind_ == Dialog::file || dialog_kind_ == Dialog::folder) {
                        open(path);
                    } else if (dialog_kind_ == Dialog::system_storage) {
                        if (std::filesystem::is_directory(path)) {
                            console_.system_storage = std::filesystem::weakly_canonical(path);
                            if (hooks_.console_changed)
                                hooks_.console_changed(console_);
                        }
                    } else if (std::filesystem::is_directory(path)) {
                        const auto normalized = std::filesystem::weakly_canonical(path);
                        if (dialog_kind_ == Dialog::replace_library &&
                            replace_index_ < libraries_.directories.size())
                            libraries_.directories[replace_index_] = normalized;
                        else if (dialog_kind_ == Dialog::add_library &&
                                 std::find(libraries_.directories.begin(),
                                           libraries_.directories.end(),
                                           normalized) == libraries_.directories.end())
                            libraries_.directories.push_back(normalized);
                        save_libraries();
                    }
                }
            } catch (const std::exception &e) {
                notice_ = std::string("Folder dialog failed: ") + e.what();
            }
        }
        if (update_check_.valid() &&
            update_check_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto check = update_check_.get();
            update_ = std::move(check.update);
            update_message_ = update_ ? update_->version + " is available" : check.message;
        }
        if (updating_.valid() &&
            updating_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                updating_.get();
                restart();
                glfwSetWindowShouldClose(window_, true);
            } catch (const std::exception &e) {
                update_message_ = std::string("Not updated: ") + e.what();
            }
        }
        if (discovery_.valid() &&
            discovery_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                titles_ = discovery_.get();
            } catch (const std::exception &e) {
                library_notice_ = std::string("Scan failed: ") + e.what();
            }
            if (refresh_pending_) {
                refresh_pending_ = false;
                refresh_library();
            }
        }
        if (loading_.valid() &&
            loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            start();
        if (guest_) {
            const std::lock_guard lock(guest_->mutex);
            if (!guest_->output.empty()) {
                log_ += guest_->output;
                guest_->output.clear();
                // a title that prints without end: the oldest half goes, at a line
                if (log_.size() > log_limit)
                    log_.erase(0, log_.find('\n', log_.size() / 2) + 1);
                log_extent_.reset();
            }
        }
        if (guest_ && guest_->done) {
            guest_->thread.join();
            // The log stays on screen, and says how the title ended as its last line: the
            // exit code, what it means (the runtime's own codes, the host's name for an
            // exception or a status) and how long the title ran.
            const auto ran = duration(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count());
            if (!log_.empty() && log_.back() != '\n')
                log_ += '\n';
            try {
                if (guest_->error)
                    std::rethrow_exception(guest_->error);
                char code[16];
                std::snprintf(code, sizeof code, "0x%08X", guest_->code);
                outcome_ = std::string("Exited with code ") + code;
                ended_badly_ = false;
                exit_code_ = static_cast<int>(guest_->code);
                log_ += "[Launcher] " + exit_description(guest_->code) + ": exit code " + code +
                        ", after " + ran + "\n";
            } catch (const std::exception &e) {
                outcome_ = std::string("Stopped: ") + e.what();
                ended_badly_ = true;
                exit_code_ = 1;
                log_ += std::string("[Launcher] The title could not be run: ") + e.what() + "\n";
            }
            log_extent_.reset();
            ran_ = true;
            guest_.reset();
        }
        if (!dropped_.empty() && !busy())
            open(dropped_);
        dropped_.clear();
    }
    void menu() {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open File...", "Ctrl+O", false, !busy()))
                pick();
            if (ImGui::MenuItem("Open Folder...", nullptr, false, !busy()))
                pick_folder(Dialog::folder);
            // the console's dashboard, where Options > Console has a system storage with one
            if (const auto dashboard = system_menu(console_);
                ImGui::MenuItem("Boot from Menu", nullptr, false, !busy() && !dashboard.empty()))
                open(dashboard);
            if (ImGui::MenuItem("Close App", "Ctrl+W", false, can_close_app()))
                close_app();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Ctrl+Q"))
                glfwSetWindowShouldClose(window_, true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options")) {
            if (ImGui::MenuItem("Input..."))
                settings_.show(ui::Settings::Page::input);
            if (ImGui::MenuItem("Graphics..."))
                settings_.show(ui::Settings::Page::graphics);
            if (ImGui::MenuItem("Render..."))
                settings_.show(ui::Settings::Page::render);
            if (ImGui::MenuItem("Game..."))
                settings_.show(ui::Settings::Page::game);
            if (ImGui::MenuItem("Profiles..."))
                settings_.show(ui::Settings::Page::profiles);
            if (ImGui::MenuItem("Console..."))
                settings_.show(ui::Settings::Page::console);
            if (ImGui::MenuItem("Game Libraries..."))
                settings_.show(ui::Settings::Page::libraries);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About XValkyrie"))
                settings_.show(ui::Settings::Page::about);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    const char *state() const {
        if (!notice_.empty())
            return notice_.c_str();
        if (loading_.valid())
            return "Loading";
        if (!title_)
            return "Ready";
        if (guest_)
            return "Running";
        if (!outcome_.empty())
            return outcome_.c_str();
        return "Not started";
    }
    void status_bar() {
        const auto *screen = ImGui::GetMainViewport();
        const float height = ImGui::GetFrameHeight();
        ImGui::SetNextWindowPos(
            {screen->WorkPos.x, screen->WorkPos.y + screen->WorkSize.y - height});
        ImGui::SetNextWindowSize({screen->WorkSize.x, height});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        ImGui::Begin("Status", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (title_ || loading_.valid())
            ImGui::Text("%s  -  %s", name_.c_str(), state());
        else
            ImGui::TextUnformatted(state());
        // on the right, the version
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().FramePadding.x -
                        ImGui::CalcTextSize(version).x);
        ImGui::TextDisabled("%s", version);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
    static const ui::Badge &platform(const LibraryTitle &title) {
        return title.target == Target::durango    ? ui::badges::xbox_one
               : title.target == Target::scarlett ? ui::badges::series
                                                  : ui::badges::unknown;
    }
    const LogoImage &logo(const LibraryTitle &title) {
        return title.logo.empty() ? empty_logo_ : logos_.get(title.logo);
    }
    // the badges of a title and its version, in a line starting at `at` and no longer than
    // `width`
    void badges(ImDrawList *list, ImVec2 at, float width, const LibraryTitle &title) const {
        const float end = at.x + width;
        at.x += ui::badge(list, ui_.fonts, at, platform(title)) + 6;
        if (title.package)
            at.x += ui::badge(list, ui_.fonts, at, ui::badges::package) + 6;
        auto *const font = ui_.fonts.small;
        // the version and the title ID; the ID alone, then nothing, where they do not fit
        const auto both = title.title_id.empty() || title.version.empty()
                              ? title.version + title.title_id
                              : title.version + "  " + title.title_id;
        for (const auto *text : {&both, &title.title_id}) {
            const auto size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text->c_str());
            if (text->empty() || at.x + 2 + size.x > end)
                continue;
            list->AddText(font, font->FontSize, {at.x + 2, at.y + (ui::badge_height - size.y) / 2},
                          ui::color::muted, text->c_str());
            break;
        }
    }
    // The logo of a title scaled to fit inside the rectangle, or its initial where there
    // is none. `rounding` is that of the rectangle's corners in `corners`.
    void cover(ImDrawList *list, const LibraryTitle &title, ImVec2 min, ImVec2 max, float rounding,
               ImDrawFlags corners) {
        list->AddRectFilled(min, max, IM_COL32(10, 10, 13, 255), rounding, corners);
        const float width = max.x - min.x, height = max.y - min.y;
        const auto &image = logo(title);
        if (image.texture) {
            const float scale = std::min(width / static_cast<float>(image.width),
                                         height / static_cast<float>(image.height));
            const float w = static_cast<float>(image.width) * scale,
                        h = static_cast<float>(image.height) * scale;
            const ImVec2 from{min.x + (width - w) / 2, min.y + (height - h) / 2};
            // only an image that reaches the corners has to be rounded with them
            const bool fills = w > width - 1 && h > height - 1;
            list->AddImageRounded(static_cast<ImTextureID>(image.texture), from,
                                  {from.x + w, from.y + h}, {0, 0}, {1, 1}, IM_COL32_WHITE,
                                  fills ? rounding : 0, corners);
        } else {
            auto *const font = height >= 80 ? ui_.fonts.display : ui_.fonts.body;
            const auto *const begin = title.name.c_str();
            const char *end = begin + (title.name.empty() ? 0 : 1);
            while (*end && (static_cast<unsigned char>(*end) & 0xc0) == 0x80)
                ++end;
            const auto size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, begin, end);
            list->AddText(font, font->FontSize,
                          {min.x + (width - size.x) / 2, min.y + (height - size.y) / 2},
                          IM_COL32(255, 255, 255, 46), begin, end);
        }
        if (title.package)
            list->AddRectFilled(min, max, IM_COL32(10, 10, 13, 150), rounding, corners);
    }
    // What the item just submitted for a title does: a click or the keyboard focus selects
    // it, a double click or Enter starts it.
    void interact(const LibraryTitle &title) {
        if (ImGui::IsItemFocused() || ImGui::IsItemClicked())
            selected_ = title.path;
        const bool entered = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                                        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
        const bool twice =
            ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if ((entered || twice) && !busy())
            open(title.path);
    }
    // What a right click on the item just submitted for a title offers.
    void context_menu(const LibraryTitle &title) {
        if (!ImGui::BeginPopupContextItem("title"))
            return;
        selected_ = title.path;
        if (ImGui::MenuItem("Run", "Enter", false, !busy()))
            open(title.path);
        if (ImGui::MenuItem("Open Directory"))
            host::show_in_file_manager(title.path);
        // a shortcut that starts the title in xvboot, without this window
        if (ImGui::MenuItem("Create Desktop Shortcut")) {
            const auto name =
                std::filesystem::path(std::u8string(title.name.begin(), title.name.end()))
                    .wstring();
            const auto made = host::create_desktop_shortcut(
                name, L"xvboot.exe", L"exec \"" + title.path.wstring() + L"\" --allow-missing");
            library_notice_ = made.empty() ? "The shortcut could not be made."
                                           : "Shortcut on the desktop: " + utf8(made.filename());
        }
        // read again from its files, pictures included, whatever the cache says
        if (ImGui::MenuItem("Refresh Cache", nullptr, false, !discovery_.valid())) {
            forget_cached_title(library_cache_file(), title.path);
            for (const auto &picture : {title.logo, title.splash})
                logos_.forget(picture);
            refresh_library();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Properties"))
            properties_.show(title.path, title.name);
        ImGui::EndPopup();
    }
    // The console's way of showing where the focus is: a ring a little outside the item.
    // `focus` goes from 0 (none) to 1.
    static void focus_ring(ImDrawList *list, ImVec2 min, ImVec2 max, float rounding, float focus) {
        if (focus <= 0)
            return;
        list->AddRect({min.x - 8, min.y - 8}, {max.x + 8, max.y + 8},
                      ui::color::fade(ui::color::scarlet, .22f * focus), rounding + 8, 0, 4);
        list->AddRect({min.x - 4, min.y - 4}, {max.x + 4, max.y + 4},
                      ui::color::fade(ui::color::scarlet, focus), rounding + 4, 0, 2.5f);
    }
    // room an item needs around it for its ring and for growing when it is selected
    static constexpr float ring_room = 14;
    static ImU32 panel(bool lit) {
        return lit ? IM_COL32(46, 46, 56, 240) : IM_COL32(24, 24, 30, 205);
    }
    static ImU32 outline(bool hovered) {
        return hovered ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 20);
    }
    // a library entry as a card: square cover, name, badges
    static constexpr float card_text = 68;
    void card(const LibraryTitle &title, float width) {
        constexpr float rounding = 6, inset = 12;
        ImVec2 min = ImGui::GetCursorScreenPos(), max{min.x + width, min.y + width + card_text};
        ImGui::PushStyleColor(ImGuiCol_NavCursor, 0);
        ImGui::InvisibleButton("card", {width, width + card_text});
        ImGui::PopStyleColor();
        interact(title);
        context_menu(title);
        const bool hovered = ImGui::IsItemHovered(), selected = title.path == selected_;
        // the selected card comes forward a little
        const float focus = ui::animate(ImGui::GetItemID(), selected ? 1.f : 0.f);
        const float grow = 3 * focus;
        min = {min.x - grow, min.y - grow};
        max = {max.x + grow, max.y + grow};
        const float side = max.x - min.x;
        auto *const list = ImGui::GetWindowDrawList();
        list->AddRectFilled(min, max, panel(hovered || selected), rounding);
        cover(list, title, min, {max.x, min.y + side}, rounding, ImDrawFlags_RoundCornersTop);
        list->AddText({min.x + inset, min.y + side + 11},
                      title.package ? ui::color::muted : ui::color::text,
                      ui::fitted(title.name, side - 2 * inset).c_str());
        badges(list, {min.x + inset, min.y + side + 20 + ImGui::GetFontSize()}, side - 2 * inset,
               title);
        list->AddRect(min, max, outline(hovered), rounding);
        focus_ring(list, min, max, rounding, focus);
    }
    // a library entry as a row: thumbnail, name, badges
    static constexpr float row_height = 56;
    void row(const LibraryTitle &title, float width) {
        constexpr float rounding = 6, thumbnail = 40;
        const ImVec2 min = ImGui::GetCursorScreenPos(), max{min.x + width, min.y + row_height};
        ImGui::PushStyleColor(ImGuiCol_NavCursor, 0);
        ImGui::InvisibleButton("row", {width, row_height});
        ImGui::PopStyleColor();
        interact(title);
        context_menu(title);
        const bool hovered = ImGui::IsItemHovered(), selected = title.path == selected_;
        const float focus = ui::animate(ImGui::GetItemID(), selected ? 1.f : 0.f);
        auto *const list = ImGui::GetWindowDrawList();
        list->AddRectFilled(min, max, panel(hovered || selected), rounding);
        const float pad = (row_height - thumbnail) / 2;
        cover(list, title, {min.x + pad, min.y + pad},
              {min.x + pad + thumbnail, min.y + pad + thumbnail}, 4, 0);
        const float left = min.x + 2 * pad + thumbnail, room = max.x - 16 - left;
        list->AddText({left, min.y + 9}, title.package ? ui::color::muted : ui::color::text,
                      ui::fitted(title.name, room).c_str());
        badges(list, {left, min.y + 30}, room, title);
        list->AddRect(min, max, outline(hovered), rounding);
        focus_ring(list, min, max, rounding, focus);
    }
    // `image` scaled to cover the rectangle, cut where it does not fit
    static void fill(ImDrawList *list, const LogoImage &image, ImVec2 min, ImVec2 max, ImU32 tint) {
        const float room = (max.x - min.x) / (max.y - min.y),
                    picture = static_cast<float>(image.width) / static_cast<float>(image.height);
        ImVec2 from{0, 0}, to{1, 1};
        if (picture > room)
            from.x = (1 - room / picture) / 2, to.x = 1 - from.x;
        else
            from.y = (1 - picture / room) / 2, to.y = 1 - from.y;
        list->AddImage(static_cast<ImTextureID>(image.texture), min, max, from, to, tint);
    }
    // The splash picture of the opened title behind a screen about it, under `shade` of
    // black (0 to 1) so that what is written on it can be read. False if it has none.
    bool title_background(float shade) {
        const auto &image = splash_.empty() ? empty_logo_ : logos_.get(splash_);
        if (!image.texture)
            return false;
        auto *const list = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetWindowPos(),
                     max{min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight()};
        fill(list, image, min, max, IM_COL32_WHITE);
        const auto black = [](float alpha) {
            return IM_COL32(6, 6, 9, static_cast<int>(255 * alpha));
        };
        list->AddRectFilled(min, max, black(shade));
        // darker towards the bottom, where the text is
        const float middle = min.y + (max.y - min.y) * .45f;
        list->AddRectFilledMultiColor({min.x, middle}, max, black(0), black(0), black(.8f),
                                      black(.8f));
        return true;
    }
    // What stays on screen from the moment a title is opened until it starts: its splash
    // picture with the name and the progress at the bottom, or the logo in the middle for
    // a title without one.
    void loading_screen(const char *step) {
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        auto *const list = ImGui::GetWindowDrawList();
        const float center = origin.x + size.x / 2;
        auto *const heading = ui_.fonts.heading;
        float y;
        if (title_background(.35f)) {
            y = origin.y + size.y - heading->FontSize - 98;
        } else {
            const float mark = std::clamp(size.x * .1f, 52.f, 104.f);
            const auto extent = ui::wordmark_size(ui_.fonts, mark);
            y = origin.y + (size.y - extent.y - 110) / 2;
            ui::wordmark(list, ui_.fonts, {center - extent.x / 2, y}, mark);
            y += extent.y + 22;
        }
        ui::centered_text(list, heading, center, y, ui::color::text, name_.c_str());
        y += heading->FontSize + 22;
        // how long a title takes to read is not known, so the bar only shows activity
        const float width = std::min(360.f, size.x - 64), left = center - width / 2;
        const float phase = static_cast<float>(std::fmod(ImGui::GetTime() * .8, 1.0));
        const float head = left + (phase * 1.35f - .35f) * width;
        list->AddRectFilled({left, y}, {left + width, y + 4}, IM_COL32(255, 255, 255, 40), 2);
        list->PushClipRect({left, y}, {left + width, y + 4}, true);
        list->AddRectFilled({head, y}, {head + width * .35f, y + 4}, ui::color::scarlet, 2);
        list->PopClipRect();
        y += 4 + 16;
        ui::centered_text(list, ui_.fonts.small, center, y, ui::color::muted, step);
    }
    // Everything the title's process printed, in what is left of the window. It is text
    // that can be selected and copied (drag, Ctrl+A, Ctrl+C): a field that cannot be
    // written to, as large as the text, inside the part that scrolls.
    void log_view() {
        ImGui::PushFont(ui_.fonts.small);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
        if (ImGui::BeginChild("Log", {0, 0}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            // stays at the end while it is there; scrolled up, it stays where it was put
            const bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1;
            if (!log_extent_)
                log_extent_ = ImGui::CalcTextSize(log_.data(), log_.data() + log_.size());
            const auto &style = ImGui::GetStyle();
            const ImVec2 size{std::max(ImGui::GetContentRegionAvail().x,
                                       log_extent_->x + 2 * style.FramePadding.x + 16),
                              log_extent_->y + 2 * style.FramePadding.y +
                                  ImGui::GetTextLineHeight()};
            ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(205, 205, 214, 255));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            ImGui::InputTextMultiline("##log", log_.data(), log_.size() + 1, size,
                                      ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (at_end)
                ImGui::SetScrollHereY(1);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    // What is on screen while a title runs, which it does in a window of its own, and
    // after it has ended: everything its process printed, as it comes, over the title's
    // splash picture. Close App, or Escape once the title is gone, puts it away.
    void running_screen() {
        title_background(.82f);
        log_view();
        if (!guest_ && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())
            close_title();
    }
    static std::string duration(double seconds) {
        char text[48];
        const auto whole = static_cast<long long>(seconds);
        if (whole >= 3600)
            std::snprintf(text, sizeof text, "%lld h %lld min", whole / 3600, whole % 3600 / 60);
        else if (whole >= 60)
            std::snprintf(text, sizeof text, "%lld min %lld s", whole / 60, whole % 60);
        else
            std::snprintf(text, sizeof text, "%.1f s", seconds);
        return text;
    }
    // what went wrong with the opened title, in a few words
    const char *problem() const {
        if (title_->startable())
            return ended_badly_ ? "The title stopped" : "The title ended";
        switch (title_->status) {
        case LaunchStatus::unreadable:
            return "This file could not be read";
        case LaunchStatus::invalid_image:
            return "This is not an executable";
        case LaunchStatus::wrong_machine:
            return "This is not an x64 executable";
        case LaunchStatus::package_unsupported:
            return "Packages cannot be opened yet";
        case LaunchStatus::other_target:
            return "This is not an Xbox title";
        case LaunchStatus::runtime_unavailable:
            return "This title cannot start yet";
        case LaunchStatus::runnable:
            break;
        }
        return ended_badly_ ? "The title stopped" : "The title ended";
    }
    // A title that was opened and is not running: a sign, what happened in a few words,
    // the whole story, and the way back.
    void title_screen() {
        const bool failed = !title_->startable() || ended_badly_;
        title_background(.8f);
        // a title that ran says how it ended before anything else
        const auto story = outcome_.empty() ? title_->detail : outcome_ + ". " + title_->detail;
        constexpr float sign = 32, pad = 18;
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const float width = std::min(600.f, size.x - 48), center = origin.x + size.x / 2;
        const float text = ImGui::CalcTextSize(story.c_str(), nullptr, false, width - 2 * pad).y;
        const float box = std::min(text + 2 * pad, std::max(80.f, size.y - 330));
        auto *const heading = ui_.fonts.heading;
        const float height = 2 * sign + 20 + heading->FontSize + 8 + ui_.fonts.small->FontSize +
                             22 + box + 22 + ImGui::GetFrameHeight();
        float y = origin.y + std::max(24.f, (size.y - height) / 2);
        auto *const list = ImGui::GetWindowDrawList();

        const ImU32 ink = failed ? ui::color::scarlet : IM_COL32(134, 226, 134, 255);
        const ImVec2 middle{center, y + sign};
        list->AddCircleFilled(middle, sign, ui::color::fade(ink, .16f), 48);
        list->AddCircle(middle, sign, ink, 48, 2.5f);
        if (failed) { // an exclamation mark
            list->AddRectFilled({center - 2.5f, middle.y - 15}, {center + 2.5f, middle.y + 5}, ink,
                                2.5f);
            list->AddCircleFilled({center, middle.y + 13}, 3.2f, ink, 16);
        } else { // a check mark
            const ImVec2 points[] = {{center - 13, middle.y + 1},
                                     {center - 4, middle.y + 10},
                                     {center + 14, middle.y - 9}};
            list->AddPolyline(points, 3, ink, 0, 4);
        }
        y += 2 * sign + 20;
        ui::centered_text(list, heading, center, y, ui::color::text, problem());
        y += heading->FontSize + 8;
        ImGui::PushFont(ui_.fonts.small);
        const auto name = ui::fitted(name_, width);
        ImGui::PopFont();
        ui::centered_text(list, ui_.fonts.small, center, y, ui::color::muted, name.c_str());
        y += ui_.fonts.small->FontSize + 22;

        ImGui::SetCursorScreenPos({center - width / 2, y});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.07f, .07f, .09f, .86f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
        ImGui::BeginChild("Story", {width, box},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(205, 205, 214, 255));
        ImGui::TextWrapped("%s", story.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        y += box + 22;

        const float button = ImGui::CalcTextSize("Back to library").x + 44;
        ImGui::SetCursorScreenPos({center - button / 2, y});
        if (ui::primary_button("Back to library", {button, 0}) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            close_title();
    }
    // the library is on screen and has something in it
    bool library_shown() const {
        return !loading_.valid() && !guest_ && !title_ && !titles_.empty();
    }
    void content() {
        const auto *screen = ImGui::GetMainViewport();
        const ImVec2 size{screen->WorkSize.x, screen->WorkSize.y - ImGui::GetFrameHeight()};
        // the layer under everything: with a library on top, the wordmark is part of it
        ui::backdrop(ui_.fonts, screen->WorkPos,
                     {screen->WorkPos.x + size.x, screen->WorkPos.y + size.y},
                     library_shown() ? .09f : 0);
        ImGui::SetNextWindowPos(screen->WorkPos);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24, 18});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::Begin("Content", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (loading_.valid())
            loading_screen("Reading the title");
        else if (guest_)
            running_screen();
        else if (title_ && ran_)
            running_screen();
        else if (title_)
            title_screen();
        else if (titles_.empty())
            welcome();
        else
            library();
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    // an empty library: the logo, and the ways to get a title in
    void welcome() {
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        auto *const list = ImGui::GetWindowDrawList();
        const float mark = std::clamp(size.x * .14f, 52.f, 156.f);
        const auto extent = ui::wordmark_size(ui_.fonts, mark);
        const float center = origin.x + size.x / 2;
        float y = origin.y + (size.y - extent.y - 150) / 2;
        ui::wordmark(list, ui_.fonts, {center - extent.x / 2, y}, mark);
        y += extent.y + 8;
        ui::centered_text(list, ui_.fonts.small, center, y, ui::color::muted, version);
        y += 44;
        // the first scan, with nothing to show yet: only that something is going on
        if (discovery_.valid()) {
            ui::spinner(list, {center, y + 14}, 11);
            return;
        }
        ui::centered_text(list, ui_.fonts.body, center, y, ui::color::text,
                          !library_notice_.empty() ? library_notice_.c_str()
                                                   : "Your library is empty");
        y += 26;
        ui::centered_text(list, ui_.fonts.small, center, y, ui::color::muted,
                          "Add a folder with unpacked titles, open one, or drop it on this window");
        y += 34;
        const auto &style = ImGui::GetStyle();
        const float add = ImGui::CalcTextSize("Add game folder...").x + 2 * style.FramePadding.x,
                    file = ImGui::CalcTextSize("Open file...").x + 2 * style.FramePadding.x;
        ImGui::SetCursorScreenPos({center - (add + style.ItemSpacing.x + file) / 2, y});
        if (ui::primary_button("Add game folder..."))
            pick_folder(Dialog::add_library);
        ImGui::SameLine();
        if (ImGui::Button("Open file..."))
            pick();
    }
    // the line above the titles: logo, count, search, view, add
    void library_header(std::size_t shown) {
        constexpr float height = 32, mark = 26;
        static constexpr const char *layouts[] = {"Grid", "List"};
        auto *const list = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const auto extent = ui::wordmark_size(ui_.fonts, mark);
        ui::wordmark(list, ui_.fonts, {at.x, at.y + (height - extent.y) / 2}, mark);
        float x = at.x + extent.x + 14;
        list->AddLine({x, at.y + 7}, {x, at.y + height - 7}, IM_COL32(255, 255, 255, 40));
        x += 14;
        const float line = ImGui::GetFontSize();
        list->AddText({x, at.y + (height - line) / 2}, ui::color::text, "Library");
        x += ImGui::CalcTextSize("Library").x + 10;
        char count[64];
        if (shown != titles_.size())
            std::snprintf(count, sizeof count, "%zu of %zu", shown, titles_.size());
        else
            std::snprintf(count, sizeof count, "%zu %s", shown, shown == 1 ? "title" : "titles");
        auto *const small = ui_.fonts.small;
        list->AddText(small, small->FontSize, {x, at.y + (height - small->FontSize) / 2 + 1},
                      ui::color::muted, count);
        // the library on screen may still change: a scan is bringing it up to date
        if (discovery_.valid())
            ui::spinner(list,
                        {x + small->CalcTextSizeA(small->FontSize, FLT_MAX, 0, count).x + 14,
                         at.y + height / 2},
                        5, 1.5f);

        const auto &style = ImGui::GetStyle();
        const float room = ImGui::GetContentRegionAvail().x;
        const float view =
            2 * (std::max(ImGui::CalcTextSize("Grid").x, ImGui::CalcTextSize("List").x) +
                 2 * style.FramePadding.x + 6);
        const float buttons = view + style.ItemSpacing.x + ImGui::CalcTextSize("Add folder...").x +
                              2 * style.FramePadding.x;
        // the search box is the first thing to go in a narrow window
        const float search = std::min(240.f, room - (x - at.x) - 120 - buttons);
        ImGui::SetCursorScreenPos(
            {at.x + room - buttons - (search >= 100 ? search + style.ItemSpacing.x : 0),
             at.y + (height - ImGui::GetFrameHeight()) / 2});
        if (search >= 100) {
            ImGui::SetNextItemWidth(search);
            ImGui::InputTextWithHint("##search", "Search", search_, sizeof search_);
            ImGui::SameLine();
        }
        int layout = libraries_.grid ? 0 : 1;
        if (ui::segmented("view", layouts, layout)) {
            libraries_.grid = layout == 0;
            save_libraries(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add folder..."))
            pick_folder(Dialog::add_library);
        ImGui::SetCursorScreenPos({at.x, at.y + height + 14});
    }
    bool matches(const LibraryTitle &title) const {
        const auto lower = [](unsigned char c) { return std::tolower(c); };
        const std::string_view wanted = search_;
        return wanted.empty() || std::search(title.name.begin(), title.name.end(), wanted.begin(),
                                             wanted.end(), [&](char a, char b) {
                                                 return lower(a) == lower(b);
                                             }) != title.name.end();
    }
    void library() {
        std::vector<const LibraryTitle *> shown;
        for (const auto &title : titles_)
            if (matches(title))
                shown.push_back(&title);
        library_header(shown.size());
        if (!library_notice_.empty())
            ImGui::TextDisabled("%s", library_notice_.c_str());
        if (shown.empty()) {
            ImGui::TextDisabled("No title matches \"%s\"", search_);
            return;
        }
        // like on the console, something is always selected: the first title if nothing else
        auto chosen = std::find_if(shown.begin(), shown.end(),
                                   [&](const auto *title) { return title->path == selected_; });
        if (chosen == shown.end()) {
            chosen = shown.begin();
            selected_ = (*chosen)->path;
        }
        // the titles scroll under the header; the padding is the room for the rings, which
        // the header's own spacing already provides above
        ImGui::SetCursorPos({ImGui::GetCursorPosX() - ring_room, ImGui::GetCursorPosY() - 8});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ring_room, ring_room});
        const bool open = ImGui::BeginChild(
            "Games", {ImGui::GetContentRegionAvail().x + ring_room, 0},
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        if (open) {
            const float room = ImGui::GetContentRegionAvail().x;
            const float gap = libraries_.grid ? 20.f : 8.f;
            // as many columns as fit at the smallest width, then stretched to fill the row
            const int columns =
                libraries_.grid ? std::max(1, static_cast<int>((room + gap) / (168 + gap))) : 1;
            const float width = std::floor((room - gap * static_cast<float>(columns - 1)) /
                                           static_cast<float>(columns));
            const float height = libraries_.grid ? width + card_text : row_height;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {gap, gap});
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>((shown.size() + columns - 1) / columns), height + gap);
            while (clipper.Step())
                for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line)
                    for (int column = 0; column < columns; ++column) {
                        const auto index = static_cast<std::size_t>(line * columns + column);
                        if (index >= shown.size())
                            break;
                        ImGui::PushID(static_cast<int>(index));
                        if (column)
                            ImGui::SameLine();
                        if (libraries_.grid)
                            card(*shown[index], width);
                        else
                            row(*shown[index], width);
                        ImGui::PopID();
                    }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
    }

    GLFWwindow *window_;
    const Interface &ui_;
    const DesktopHooks &hooks_;
    std::optional<LaunchResult> title_;
    std::string name_;    // file name of title_
    std::string outcome_; // how the last run of title_ ended
    bool ended_badly_ = false;
    std::filesystem::path splash_; // the splash picture of what was opened, if it has one
    std::string notice_;           // frontend problem unrelated to title_
    std::shared_ptr<Guest> guest_;
    static constexpr std::size_t log_limit = 1 << 20;
    std::string log_;                  // what the process of the title that runs, or ran, printed
    std::optional<ImVec2> log_extent_; // how large it is as text; worked out again when it grows
    std::chrono::steady_clock::time_point started_; // when the title was
    bool ran_ = false; // the title opened ran and has ended: its log is what is on screen
    std::future<LaunchResult> loading_;
    std::future<UpdateCheck> update_check_;
    std::string update_message_; // for the about page
    std::optional<Update> update_;
    std::future<void> updating_;
    std::filesystem::path path_; // what loading_ is reading
    std::future<std::filesystem::path> dialog_;
    Dialog dialog_kind_ = Dialog::file;
    std::size_t replace_index_ = 0;
    std::filesystem::path dropped_;
    LibrarySettings libraries_;
    ConsoleSettings console_;
    std::vector<LibraryTitle> titles_;
    std::future<std::vector<LibraryTitle>> discovery_;
    bool refresh_pending_ = false;
    std::string library_notice_;
    std::filesystem::path selected_; // the library entry a click or the keyboard chose
    char search_[64] = "";
    Logos logos_;
    LogoImage empty_logo_;
    ui::Settings settings_;
    ui::Settings::Host settings_host_;
    ui::Properties properties_;
    int exit_code_ = 0;
};
} // namespace

int run_desktop(const DesktopHooks &hooks, const std::filesystem::path &initial) {
    Glfw glfw;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(1180, 740, "XValkyrie", nullptr, nullptr), glfwDestroyWindow);
    if (!window) {
        const char *message = nullptr;
        glfwGetError(&message);
        throw std::runtime_error(message ? message : "OpenGL 3.3 window creation failed");
    }
    glfwSetWindowSizeLimits(window.get(), 640, 420, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window.get());
    if (!gladLoadGL(glfwGetProcAddress))
        throw std::runtime_error("OpenGL function loading failed");
    glfwSwapInterval(1);
    // Set before the interface installs its own callbacks, which then call these first.
    glfwSetKeyCallback(window.get(), [](GLFWwindow *target, int key, int, int action, int) {
        if (auto *const desktop = static_cast<Desktop *>(glfwGetWindowUserPointer(target));
            desktop && action != GLFW_REPEAT)
            desktop->key(key, action == GLFW_PRESS);
    });
    Interface ui(window.get());
    Desktop desktop(window.get(), ui, hooks);
    if (!initial.empty())
        desktop.open(initial);
    for (;;) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window.get())) {
            if (!desktop.dialog_open())
                break;
            glfwSetWindowShouldClose(window.get(), false);
        }
        int w, h;
        glfwGetFramebufferSize(window.get(), &w, &h);
        if (w <= 0 || h <= 0) {
            glfwWaitEventsTimeout(.1);
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        desktop.frame();
        ImGui::Render();
        glViewport(0, 0, w, h);
        glClearColor(.035f, .035f, .047f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window.get());
        // redraw every vblank while in use so menus respond at once, otherwise idle
        if (!desktop.animating() && !glfwGetWindowAttrib(window.get(), GLFW_FOCUSED) &&
            !glfwGetWindowAttrib(window.get(), GLFW_HOVERED))
            glfwWaitEventsTimeout(.1);
    }
    return desktop.exit_code();
}
} // namespace xv
