// SPDX-License-Identifier: MIT
//
// The desktop face of Crucible.
//
// The same program as `crucible` in a terminal, and the word "same" is meant
// literally: this owns a Config, an AppState, an Engine, a SessionStore and a
// CookLog, exactly as the TUI does, and does not know anything about routing,
// cooking or models that the terminal does not. Everything below this file is
// shared. If the two ever disagree about what an expert is or how a cook
// finishes, that is a bug in one of the faces and not a difference of opinion.
//
// It follows that a feature added to the engine appears in both, and a feature
// added here is a drawing decision only.
//
// The one thing this has that the terminal program does not is a project
// picker. `crucible` is told where it is by being run there -- you cd, then you
// type it -- and a window has no cd, so it has to offer the list instead.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "crucible/config/config.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/session/store.hpp"

struct GLFWwindow;

namespace crucible::gui {

class App {
public:
    App(Config config, std::vector<std::string> warnings);
    ~App();
    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    /// Open the window and run until it is closed. Returns a process exit code.
    int run();

private:
    /// Which pane the main area is showing.
    enum class View { Chat, Cook, Experts, History, Settings };

    /// Which page of the settings. One list down the left and one page on the
    /// right, which is the shape every desktop application settles on because
    /// a single scrolling wall of switches cannot be navigated.
    enum class SettingsPage { General, Experts, Hardware, Tools, About };

    // --- frame ------------------------------------------------------------
    void draw();
    void draw_sidebar(const Snapshot& snapshot);
    void draw_splitter();
    void draw_chat(const Snapshot& snapshot);
    void draw_cook(const Snapshot& snapshot);
    void draw_expert_list();
    void draw_history();
    void draw_settings();
    void draw_composer(const Snapshot& snapshot);

    void draw_new_expert_modal();
    void draw_project_modal();
    void draw_trust_modal();

    /// One cook step: its verb, its summary, and the diff or output it expands
    /// into.
    void draw_cook_step(const CookStep& step, std::size_t index);

    // --- actions ----------------------------------------------------------
    void submit_prompt();
    void begin_cook();
    void update_config(const std::function<void(Config&)>& change);
    void persist_session();
    void absorb_written_examples();
    void refresh_models();
    void say(std::string message);

    /// Point Crucible at another directory: new history, new cook journal, new
    /// workshop root. Refused while a cook is running, because the cook is
    /// about the directory it started in.
    ///
    /// Goes through the same folder-trust store the terminal program uses. A
    /// directory trusted in one face is trusted in the other.
    void open_project(const std::filesystem::path& root);

    Config                  config_;
    AppState                state_;
    std::unique_ptr<SessionStore> store_;
    std::unique_ptr<Engine> engine_;
    TrustStore              trust_;
    GLFWwindow*             window_ = nullptr;

    View         view_          = View::Chat;
    SettingsPage settings_page_ = SettingsPage::General;

    std::string prompt_;
    std::string cook_goal_;
    int         cook_minutes_ = 30;
    bool        cook_untimed_ = false;

    /// Width of the left column in pixels, dragged by the splitter, and whether
    /// it is showing at all. Both are per-session: they are how the window is
    /// arranged right now, not a preference worth writing to the config file.
    float sidebar_width_ = 0.0F;   ///< 0 until the first frame sizes it
    bool  sidebar_open_  = true;

    /// Set when the transcript should jump to the bottom on the next frame.
    ///
    /// A flag rather than an unconditional scroll: a user reading back through
    /// an hour-old cook while a new one streams must not be yanked to the end
    /// every time a token arrives.
    bool follow_ = true;

    /// Which cook steps are expanded to show their diff or output. By index
    /// into the journal, which is stable for the life of a cook.
    std::vector<bool> expanded_;

    /// The new-expert dialog's two boxes, and what to say when it is refused.
    bool        expert_modal_open_ = false;
    std::string new_expert_name_;
    std::string new_expert_blurb_;
    std::string expert_error_;

    /// The project picker: where it is browsing, and the name of a folder to
    /// create.
    bool                  project_modal_open_ = false;
    std::filesystem::path browse_;
    std::string           browse_text_;
    std::string           new_folder_;
    std::string           project_error_;

    /// A directory waiting on the trust question, and the answer to it.
    std::optional<std::filesystem::path> pending_trust_;

    std::vector<ModelFile>   models_;   ///< the models directory, rescanned on demand
    std::vector<std::string> notices_;
    std::size_t persisted_turns_ = 0;
};

}  // namespace crucible::gui
