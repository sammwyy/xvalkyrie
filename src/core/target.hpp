#pragma once
#include <array>
#include <optional>
#include <string_view>

namespace xv {
// Console generations by their Microsoft code names.
enum class Target {
    durango, // Xbox One, One S, One X
    scarlett // Xbox Series X|S
};
constexpr std::string_view target_id(Target target) {
    return target == Target::durango ? "durango" : "scarlett";
}
constexpr std::string_view target_name(Target target) {
    return target == Target::durango ? "Xbox One" : "Xbox Series X|S";
}
// The console a title is told it runs on, one of the models of its generation
enum class ConsoleModel { xbox_one, xbox_one_s, xbox_one_x, series_s, series_x };
constexpr std::array<ConsoleModel, 5> console_models{
    ConsoleModel::xbox_one, ConsoleModel::xbox_one_s, ConsoleModel::xbox_one_x,
    ConsoleModel::series_s, ConsoleModel::series_x};
constexpr Target model_target(ConsoleModel model) {
    return model == ConsoleModel::series_s || model == ConsoleModel::series_x ? Target::scarlett
                                                                              : Target::durango;
}
// the most capable of each generation, which is what a title is told without a choice
constexpr ConsoleModel default_model(Target target) {
    return target == Target::scarlett ? ConsoleModel::series_x : ConsoleModel::xbox_one_x;
}
// `wanted` where it is of the generation, the default one of it otherwise
constexpr ConsoleModel model_for(Target target, ConsoleModel wanted) {
    return model_target(wanted) == target ? wanted : default_model(target);
}
// as config.ini and the command line name it
constexpr std::string_view model_id(ConsoleModel model) {
    constexpr std::string_view ids[]{"xbox_one", "xbox_one_s", "xbox_one_x", "series_s",
                                     "series_x"};
    return ids[static_cast<int>(model)];
}
constexpr std::string_view model_name(ConsoleModel model) {
    constexpr std::string_view names[]{"Xbox One", "Xbox One S", "Xbox One X", "Xbox Series S",
                                       "Xbox Series X"};
    return names[static_cast<int>(model)];
}
// as a window's caption names it
constexpr std::string_view model_short_name(ConsoleModel model) {
    constexpr std::string_view names[]{"Xbox One", "XOne S", "XOne X", "Series S", "Series X"};
    return names[static_cast<int>(model)];
}
constexpr std::optional<ConsoleModel> parse_model(std::string_view id) {
    for (const auto model : console_models)
        if (model_id(model) == id)
            return model;
    return std::nullopt;
}
} // namespace xv
