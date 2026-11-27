#include "app/gui/debug_options.hpp"
#include "app/gui/widgets.hpp"
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <string>
#include <vector>

namespace xv::ui {
bool debug_options_ui(const Fonts &fonts, DebugValues &values, DebugFields &fields,
                      const DebugValues *defaults, OptionGroups groups) {
    bool changed = false;
    const char *group = nullptr;
    bool open = false;
    for (const auto &option : debug_options()) {
        if (groups.only ? std::strcmp(option.group, groups.only) != 0 : user_group(option.group))
            continue;
        if (!group || std::strcmp(group, option.group)) {
            if (open)
                end_settings();
            group = option.group;
            section(fonts, group);
            open = begin_settings(group);
        }
        if (!open)
            continue;
        ImGui::PushID(option.key);
        const auto found = values.find(option.key);
        const std::string *inherited = nullptr;
        if (defaults)
            if (const auto other = defaults->find(option.key); other != defaults->end())
                inherited = &other->second;
        setting(fonts, option.name, option.hint);
        if (option.kind == DebugOption::Kind::choice) {
            // one of its choices, or (a title's) the one for all titles
            const auto choices = option_choices(option);
            std::vector<std::string> names;
            if (defaults) {
                const std::size_t all = inherited ? std::stoul(*inherited) : 0;
                names.push_back("Default (" + (all < choices.size() ? choices[all] : "") + ")");
            }
            names.insert(names.end(), choices.begin(), choices.end());
            std::vector<const char *> labels;
            for (const auto &name : names)
                labels.push_back(name.c_str());
            const int first = defaults ? 1 : 0;
            int chosen = found != values.end() ? first + std::stoi(found->second) : 0;
            if (segmented("choice", labels, chosen)) {
                if (defaults && chosen == 0)
                    values.erase(option.key);
                else
                    values[option.key] = std::to_string(chosen - first);
                changed = true;
            }
        } else if (option.kind == DebugOption::Kind::flag) {
            const bool on = found != values.end() && found->second == "true";
            if (defaults) {
                // for all titles, on, or off
                const bool all_on = inherited && *inherited == "true";
                const char *labels[] = {all_on ? "Default (on)" : "Default (off)", "On", "Off"};
                int chosen = found == values.end() ? 0 : on ? 1 : 2;
                if (segmented("state", labels, chosen)) {
                    if (chosen == 0)
                        values.erase(option.key);
                    else
                        values[option.key] = chosen == 1 ? "true" : "false";
                    changed = true;
                }
            } else if (bool checked = on; ImGui::Checkbox("##on", &checked)) {
                if (checked)
                    values[option.key] = "true";
                else
                    values.erase(option.key);
                changed = true;
            }
        } else {
            // a number or a text, empty for none (or the one for all titles)
            auto [field, added] = fields.try_emplace(option.key);
            if (added)
                std::snprintf(field->second.data(), field->second.size(), "%s",
                              found != values.end() ? found->second.c_str() : "");
            const bool number = option.kind == DebugOption::Kind::number;
            const std::string hint =
                defaults ? (inherited ? "Default: " + *inherited : std::string("Default"))
                         : std::string("Not set");
            ImGui::SetNextItemWidth(number ? 140.f : 320.f);
            ImGui::InputTextWithHint("##value", hint.c_str(), field->second.data(),
                                     field->second.size(),
                                     number ? ImGuiInputTextFlags_CharsDecimal : 0);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string typed = field->second.data();
                if (typed.empty())
                    values.erase(option.key);
                else
                    values[option.key] = typed;
                changed = true;
            }
        }
        ImGui::PopID();
    }
    if (open)
        end_settings();
    return changed;
}
} // namespace xv::ui
