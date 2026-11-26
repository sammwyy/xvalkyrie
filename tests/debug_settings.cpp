#include "launcher/debug.hpp"
#include <catch2/catch.hpp>
#include <map>
#include <set>

using namespace xv;

namespace {
std::map<std::string, std::string> environment(const DebugValues &all,
                                               const TitleDebugSettings &title = {}) {
    std::map<std::string, std::string> variables;
    for (const auto &[name, value] : debug_environment(all, title))
        variables[name] = value;
    return variables;
}
} // namespace

TEST_CASE("debug settings become the runtime's variables") {
    CHECK(environment({}).empty());
    // a flag, and one whose variable is set to turn something off
    auto made = environment({{"dred", "true"}, {"no_heap_shaders", "true"}, {"mute", "false"}});
    CHECK(made ==
          std::map<std::string, std::string>{{"XV_D3D_DRED", "1"}, {"XV_HEAP_SHADERS", "0"}});
    // numbers and text as typed
    made = environment({{"capture_frame", "2600"}, {"capture_folder", "E:\\caps"}});
    CHECK(made.at("XV_CAPTURE") == "2600");
    CHECK(made.at("XV_CAPTURE_DIR") == "E:\\caps");
}

TEST_CASE("debug flags that share a variable make a list of letters") {
    // the kinds of work left out, by the letters of those on
    auto made = environment({{"skip_mesh", "true"}, {"skip_indirect", "true"}});
    CHECK(made.at("XV_D3D_NO_WORK") == "mi");
    // the stages still translated, by the letters of those off
    made = environment({{"stand_in_mesh", "true"}});
    CHECK(made.at("XV_SHADER_STAND_INS") == "vpc");
    made = environment({{"stand_in_vertex", "true"},
                        {"stand_in_pixel", "true"},
                        {"stand_in_compute", "true"},
                        {"stand_in_mesh", "true"}});
    CHECK(made.at("XV_SHADER_STAND_INS") == "-");
    CHECK(!environment({{"stand_in_mesh", "false"}}).count("XV_SHADER_STAND_INS"));
}

TEST_CASE("a title's debug settings take the place of those for all titles") {
    TitleDebugSettings title;
    title.debug = {{"dred", "false"}, {"capture_frame", "10"}};
    title.stand_ins = {"0123abcd-m", "ff1bddd6-p"};
    const auto made = environment({{"dred", "true"}, {"capture_frame", "2600"}}, title);
    CHECK(!made.count("XV_D3D_DRED"));
    CHECK(made.at("XV_CAPTURE") == "10");
    CHECK(made.at("XV_SHADER_FORCE") == "0123abcd-m,ff1bddd6-p");
}
