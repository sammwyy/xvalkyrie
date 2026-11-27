#pragma once
#include "app/gui/debug_options.hpp"
#include "app/gui/theme.hpp"
#include "launcher/config.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace xv::ui {
// The settings window: a list of pages on the left, the chosen one on the right. A new
// page is an entry in `pages` (settings.cpp) and a function that draws it.
class Settings {
  public:
    enum class Page { input, graphics, render, game, profiles, console, libraries, debug, about };
    // What the settings need done by whoever owns the window. All of them are called
    // from draw().
    // what the about page shows of updates
    struct Updates {
        bool checking = false, installing = false, blocked = false; // blocked: a title runs
        std::string available;                                      // the later release, if any
        std::string message; // what the last check or update said
    };
    struct Host {
        std::function<void()> add_folder;                 // ask for a folder to add
        std::function<void(std::size_t)> change_folder;   // ask for one to put in its place
        std::function<void()> rescan;                     // look for titles again
        std::function<void(bool rescan)> libraries_saved; // `libraries` was changed
        std::function<void()> input_saved;                // input::settings() was; may throw
        std::function<void()> graphics_saved;
        std::function<void()> console_saved;
        std::function<void()> choose_system_storage; // ask for the folder of the system's apps
        std::function<void()> check_updates, install_update;
        std::function<Updates()> updates;
    };
    // what the library page shows about the library itself
    struct Library {
        LibrarySettings &settings;
        std::size_t titles;
        bool scanning;
        const std::string &notice;
    };

    void show(Page page) {
        page_ = page;
        open_ = true;
        raise_ = true;
        profile_loaded_ = false;
        debug_loaded_ = false;
    }
    bool shown() const { return open_; }
    // While the input page is up the keyboard is for choosing keys, not for the title.
    bool input_shown() const { return open_ && page_ == Page::input; }
    // true while a control waits for the key to bind to it: give the next one to key()
    bool wants_key() const { return binding_.has_value(); }
    void key(int virtual_key) { pressed_ = virtual_key; }
    void draw(const Fonts &fonts, const Host &host, Library library, ConsoleSettings &profile);

  private:
    void input(const Fonts &fonts, const Host &host);
    void graphics(const Fonts &fonts, const Host &host);
    void profiles(const Fonts &fonts, const Host &host, ConsoleSettings &settings);
    void new_profile(const Fonts &fonts, const Host &host, ConsoleSettings &settings);
    void console(const Fonts &fonts, const Host &host, ConsoleSettings &settings);
    void libraries(const Fonts &fonts, const Host &host, Library library);
    void options(const Fonts &fonts, OptionGroups groups, const char *intro);
    void about(const Fonts &fonts, const Host &host);

    bool open_ = false, raise_ = false;
    Page page_ = Page::input;
    std::optional<std::size_t> binding_; // control waiting for a key
    int pressed_ = 0;                    // the key it got
    std::string input_notice_, graphics_notice_, console_notice_, profile_notice_, new_notice_;
    // what is typed of a profile: the one in use, and the one being made
    struct ProfileForm {
        char name[40] = "", gamertag[64] = "", user_id[32] = "";
        std::uint32_t gamerscore = 0;
        int age = 3;
    };
    ProfileForm profile_, new_;
    bool profile_loaded_ = false; // profile_ is of the profile in use
    // the debug settings for all titles, as debug.ini has them when the page is shown
    DebugValues debug_;
    DebugFields debug_fields_;
    bool debug_loaded_ = false;
    std::string debug_notice_;
};
} // namespace xv::ui
