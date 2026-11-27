#pragma once
#include "app/gui/theme.hpp"
#include "launcher/debug.hpp"
#include <array>
#include <map>
#include <string>

namespace xv::ui {
// what is typed into the number and text fields, by key, until it is taken
using DebugFields = std::map<std::string, std::array<char, 260>>;
// Which of debug_options() a page shows: the debug settings, or one group of those for playing
// (render_group, game_group).
struct OptionGroups {
    const char *only = nullptr; // that group alone; the debug groups without it
};
// The settings of debug_options(), by group. With `defaults` they are a title's: each is
// either the one for all titles (and then not in `values`) or the title's own. True when one
// was changed.
bool debug_options_ui(const Fonts &fonts, DebugValues &values, DebugFields &fields,
                      const DebugValues *defaults = nullptr, OptionGroups groups = {});
} // namespace xv::ui
