#pragma once
#include "app/gui/theme.hpp"
#include "launcher/debug.hpp"
#include <array>
#include <map>
#include <string>

namespace xv::ui {
// what is typed into the number and text fields, by key, until it is taken
using DebugFields = std::map<std::string, std::array<char, 260>>;
// The debug settings of debug_options(), by group. With `defaults` they are a title's: each
// is either the one for all titles (and then not in `values`) or the title's own. True when
// one was changed.
bool debug_options_ui(const Fonts &fonts, DebugValues &values, DebugFields &fields,
                      const DebugValues *defaults = nullptr);
} // namespace xv::ui
