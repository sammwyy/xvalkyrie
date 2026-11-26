#pragma once
#include "app/gui/debug_options.hpp"
#include "app/gui/theme.hpp"
#include "runtime/modules/console_shader.hpp"
#include <atomic>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace xv::ui {
// A window with everything that can be told about a title without running it, for looking
// into why it does or does not work: what it is, where its image is mapped, the modules it
// imports and where each import binds. What it shows is title_properties() of the launcher,
// worked out off the window thread. Then what the title has of its own for looking into a
// run: its debug settings, and the shaders earlier runs met, each of which can be given a
// stand-in.
class Properties {
  public:
    void show(const std::filesystem::path &path, const std::string &name);
    void draw(const Fonts &fonts);
    // the title, or something of it, is still being read
    bool loading() const {
        return loading_.valid() || reading_.valid() || check_.valid() || source_.valid();
    }

  private:
    void overview() const;
    void mapping() const;
    void libraries() const;
    void imports();
    void debug(const Fonts &fonts);
    void shaders();
    void shader_details(const runtime::modules::JournalShader &shader);
    void save_title();

    bool open_ = false, raise_ = false;
    std::filesystem::path path_;
    std::string name_;
    // the title's debug settings, and those for all titles they take the place of
    TitleDebugSettings title_;
    DebugValues all_;
    DebugFields fields_;
    std::string notice_;
    // its shaders, read when the tab is first shown
    using Shader = runtime::modules::JournalShader;
    std::future<std::vector<Shader>> reading_;
    std::filesystem::path reading_for_;
    std::vector<Shader> shaders_;
    bool shaders_read_ = false;
    char shader_filter_[64] = "";
    int stage_shown_ = 0, state_shown_ = 0;
    std::string selected_; // the file of the one shown below the list
    // what translating each says now, by file, for those that have not been made: "" when
    // it is translated, else what stops it
    struct Checking {
        std::atomic<std::size_t> done{0}, total{0};
    };
    std::shared_ptr<Checking> checking_;
    std::future<std::map<std::string, std::string>> check_;
    std::map<std::string, std::string> checked_;
    // the HLSL of the one shown, as it is made now
    std::future<std::string> source_;
    std::string source_text_, source_of_;
    std::future<nlohmann::json> loading_;
    nlohmann::json report_;
    std::string raw_; // the report as text, for the last tab
    char filter_[64] = "";
    bool missing_only_ = false;
};
} // namespace xv::ui
