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
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "crucible/config/config.hpp"
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
    enum class View { Chat, Cook, Experts, Settings, History };

    void draw();                       ///< one frame, everything
    void draw_sidebar(const Snapshot& snapshot);
    void draw_header(const Snapshot& snapshot);
    void draw_chat(const Snapshot& snapshot);
    void draw_cook_pane(const Snapshot& snapshot);
    void draw_experts();
    void draw_settings();
    void draw_history();
    void draw_composer(const Snapshot& snapshot);
    void draw_new_expert_modal();

    /// Draw one model reply, with its markdown rendered rather than shown.
    void draw_markdown(const std::string& text, ImU32 colour);

    void submit_prompt();
    void begin_cook();
    void update_config(const std::function<void(Config&)>& change);
    void persist_session();
    void absorb_written_examples();
    void refresh_models();
    void say(std::string message);

    Config                  config_;
    AppState                state_;
    SessionStore            store_;
    std::unique_ptr<Engine> engine_;
    GLFWwindow*             window_ = nullptr;

    View        view_ = View::Chat;
    std::string prompt_;
    std::string cook_goal_;
    int         cook_minutes_ = 30;
    bool        cook_untimed_ = false;

    /// Set when the transcript should jump to the bottom on the next frame.
    ///
    /// A flag rather than an unconditional scroll: a user reading back through
    /// an hour-old cook while a new one streams must not be yanked to the end
    /// every time a token arrives.
    bool follow_ = true;

    /// The new-expert dialog's two boxes, and what to say when it is refused.
    bool        expert_modal_open_ = false;
    std::string new_expert_name_;
    std::string new_expert_blurb_;
    std::string expert_error_;

    std::vector<ModelFile> models_;      ///< the models directory, rescanned on demand
    std::vector<std::string> notices_;
    std::size_t persisted_turns_ = 0;

    /// A free-running clock for anything that animates, in seconds.
    float phase_ = 0.0F;
};

}  // namespace crucible::gui
