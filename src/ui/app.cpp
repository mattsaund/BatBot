// SPDX-License-Identifier: MIT
//
// The application shell: layout, key handling, and the animation clock.
//
// This file owns the thread boundary. The engine runs elsewhere and pokes the
// screen through PostEvent, which is the only FTXUI call safe to make from off
// the UI thread; everything else here runs on the loop.
//
// Slash commands live in commands.cpp and the conversation in transcript.cpp,
// so what remains is the shell itself.
#include "batbot/ui/app.hpp"

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>

#include "batbot/config/paths.hpp"
#include "batbot/session/usage.hpp"
#include "batbot/ui/widgets/roundtable.hpp"
#include "batbot/ui/settings/settings_view.hpp"
#include "batbot/ui/theme.hpp"
#include "batbot/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {
namespace {

}  // namespace

App::App(Config config, const std::vector<std::string>& warnings)
    : config_(std::move(config)),
      bat_(config_.ui.unicode),
      settings_(config_),
      // The runtime panel builds on a thread of its own and pokes the screen
      // when progress moves, exactly as the engine does.
      runtimes_([this] { screen_.PostEvent(Event::Custom); }),
      store_(Project::current()),
      screen_(ScreenInteractive::Fullscreen()) {
    show_roundtable_ = config_.ui.show_roundtable;

    for (const std::string& warning : warnings) {
        state_.add_notice(warning);
    }
    state_.configure_seats(config_);

    // What this project has spent before today, so the counter continues
    // rather than restarting at zero each time BatBot opens.
    state_.set_project_usage(store_.project_usage());

    // The engine runs on its own thread and pokes the screen when the state
    // changes. PostEvent is the only FTXUI call safe to make from off-thread.
    engine_ = std::make_unique<Engine>(config_, state_, [this] {
        screen_.PostEvent(Event::Custom);
    });

    InputOption option;
    option.multiline = false;  // FTXUI 7 defaults this on; we want Enter to send
    option.on_enter  = [this] { on_submit(); };
    input_ = Input(&input_text_, "ask BatBot anything, or /help", option);
}

App::~App() {
    // Every thread that can call back into this object has to be gone before
    // any member is destroyed. The screen is declared after the runtime panel
    // and so dies first, and a build thread still calling PostEvent to report
    // progress would be reaching into freed memory.
    stop_ticker();
    runtimes_.shutdown();
    if (engine_) {
        engine_->stop();
    }
}

// ---------------------------------------------------------------------------
// Animation clock
// ---------------------------------------------------------------------------

void App::start_ticker() {
    ticking_.store(true, std::memory_order_relaxed);
    ticker_ = std::thread([this] {
        while (ticking_.load(std::memory_order_relaxed)) {
            // Animate briskly while BatBot is working, and slowly when he is
            // idle -- an idle bat still blinks, but should not cost a redraw
            // ten times a second forever.
            const bool busy = state_.busy();
            const auto interval = std::chrono::milliseconds(
                busy ? std::max(30, config_.ui.animation_ms) : 400);
            std::this_thread::sleep_for(interval);

            if (!ticking_.load(std::memory_order_relaxed)) {
                break;
            }
            tick_.fetch_add(1, std::memory_order_relaxed);
            screen_.PostEvent(Event::Custom);
        }
    });
}

void App::stop_ticker() {
    if (!ticking_.exchange(false)) {
        return;
    }
    if (ticker_.joinable()) {
        ticker_.join();
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

App::TableView App::table_view() const {
    if (!show_roundtable_) {
        return TableView::Hidden;
    }
    // Leave room for a usable transcript: the ring only earns its space when
    // there is enough terminal left over to still read the conversation.
    const int rows = Terminal::Size().dimy;
    if (rows >= 34) { return TableView::Full; }
    if (rows >= 24) { return TableView::Compact; }
    return TableView::Strip;
}

Element App::render_status(const Snapshot& snapshot) const {
    std::string left = "● " + std::string(mood_label(snapshot.mood));
    if (snapshot.resident) {
        left += "  │  resident: " + std::string(subject_name(*snapshot.resident));
    } else {
        left += "  │  no expert loaded";
    }

    const std::string right = snapshot.busy
        ? "ctrl-c cancel  ·  ctrl-e settings  ·  /help"
        : "ctrl-c quit  ·  ctrl-e settings  ·  /resume  ·  /help";

    return hbox({
        text(left) | color(mood_color(snapshot.mood)),
        filler(),
        render_usage(snapshot),
        text("   " + right) | color(theme::kMeta) | dim,
    });
}

Element App::render_usage(const Snapshot& snapshot) const {
    // Session first, because it is the number that changes as you work. The
    // project total is the quieter one and sits behind a separator.
    const bool unicode = config_.ui.unicode;
    std::string text_out = usage_readout(snapshot.session_usage,
                                         snapshot.live_tokens_per_second, unicode);

    const TokenUsage& project = snapshot.project_usage;
    if (project.total_tokens() > snapshot.session_usage.total_tokens()) {
        text_out += unicode ? "   │  project " : "   |  project ";
        text_out += format_tokens(project.total_tokens()) + " tok";
    }

    return text(text_out) |
           color(snapshot.live_tokens_per_second > 0.0 ? Color(theme::kSeatActive)
                                                       : Color(theme::kMeta));
}

Element App::render() {
    // Panels stack: the runtime panel is opened from settings and drawn over
    // it, so leaving one returns to the other rather than to the transcript.
    if (runtimes_.active()) {
        return dbox({settings_.render(), runtimes_.render() | center});
    }
    if (in_settings_) {
        return settings_.render();
    }

    const Snapshot snapshot = state_.snapshot();
    const std::size_t tick  = tick_.load(std::memory_order_relaxed);

    Elements rows;

    switch (table_view()) {
        case TableView::Full:
            rows.push_back(roundtable(snapshot, bat_, tick, /*compact=*/false));
            rows.push_back(separator());
            break;
        case TableView::Compact:
            rows.push_back(roundtable(snapshot, bat_, tick, /*compact=*/true));
            rows.push_back(separator());
            break;
        case TableView::Strip:
            rows.push_back(roundtable_strip(snapshot, tick));
            rows.push_back(separator());
            break;
        case TableView::Hidden:
            break;
    }

    rows.push_back(render_transcript(snapshot) | flex);
    rows.push_back(separator());
    rows.push_back(render_status(snapshot));
    rows.push_back(hbox({
        text(" › ") | color(theme::kAccent) | bold,
        input_->Render() | flex,
    }));

    Element screen = window(text(" BatBot " BATBOT_VERSION " ") | bold | color(theme::kBat),
                            vbox(std::move(rows)));

    if (sessions_.active()) {
        screen = dbox({std::move(screen), sessions_.render() | center});
    }
    return screen;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void App::open_settings() {
    // Start from what the engine is actually running, not from the copy this
    // object was constructed with, so the screen never shows stale values.
    settings_.set_config(engine_->config());
    settings_.set_status({});
    settings_.refresh();
    in_settings_ = true;
}

void App::save_settings() {
    Config edited = settings_.config();
    edited.resolve_models();

    if (!save_config(edited)) {
        settings_.set_status("could not write " + paths::config_file().string());
        return;
    }

    settings_.mark_saved();
    settings_.set_status("saved");

    // The engine applies it between requests; the UI-side copy is updated here
    // so the roundtable and the bat pick up cosmetic changes immediately.
    config_ = edited;
    bat_ = BatSprite(config_.ui.unicode);
    show_roundtable_ = config_.ui.show_roundtable;
    state_.configure_seats(config_);
    engine_->apply_config(std::move(edited));
}

// ---------------------------------------------------------------------------
// Session history
// ---------------------------------------------------------------------------

void App::persist_session() {
    const Snapshot snapshot = state_.snapshot();

    // Only write when a turn has actually finished. The engine wakes the
    // screen for every token, and rewriting the file that often would be a
    // disk write per token for no gain.
    std::size_t finished = 0;
    for (const Turn& turn : snapshot.turns) {
        if (!turn.streaming && !turn.reply.empty()) {
            ++finished;
        }
    }
    if (finished == persisted_turns_) {
        return;
    }
    persisted_turns_ = finished;

    std::string error;
    if (!store_.save(snapshot.turns, snapshot.session_usage, error)) {
        say(error);
    }
}

void App::resume_session(const std::string& id) {
    std::vector<Turn> turns;
    TokenUsage usage;
    std::string error;
    if (!store_.load(id, turns, usage, error)) {
        say(error);
        return;
    }

    // Continue the stored conversation rather than forking it: further turns
    // append to the same file, which is what "resume" means.
    store_.adopt(id);
    state_.clear_turns();
    state_.clear_notices();

    // Hand the exchanges back to the engine too, or the expert would answer
    // the next question with no memory of what is on screen.
    engine_->reset_history();
    std::vector<ChatMessage> history;
    for (const Turn& turn : turns) {
        state_.restore_turn(turn);
        if (!turn.failed && !turn.cancelled && !turn.reply.empty()) {
            history.push_back({"user", turn.prompt});
            history.push_back({"assistant", turn.reply});
        }
    }
    engine_->restore_history(std::move(history));

    persisted_turns_ = turns.size();
    follow_ = true;
    say("resumed " + id + " -- " + std::to_string(turns.size()) +
        (turns.size() == 1 ? " turn restored" : " turns restored"));
}

void App::on_submit() {
    const std::string text = format::trim(input_text_);
    input_text_.clear();
    if (text.empty()) {
        return;
    }

    if (handle_command(text)) {
        return;
    }

    state_.clear_notices();
    engine_->submit(text);
    follow_ = true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int App::run() {
    // FTXUI handles Ctrl-C itself by default *even when a component catches the
    // event*, and does so by re-raising SIGINT -- which would kill BatBot the
    // moment the user tried to interrupt a long answer. Taking ownership of the
    // key is what makes "Ctrl-C cancels, Ctrl-C again quits" possible.
    screen_.ForceHandleCtrlC(false);

    engine_->start();
    start_ticker();

    Component root = Renderer(input_, [this] { return render(); });

    root = CatchEvent(root, [this](const Event& event) {
        // The engine wakes the screen with a Custom event after every change.
        // A finished turn is the moment the conversation is worth writing, and
        // persist_session() returns immediately when nothing has finished.
        if (event == Event::Custom && !state_.busy()) {
            persist_session();
        }

        // Modals are checked outermost-first, so the topmost one gets the key.

        // The runtime panel sits above settings.
        if (runtimes_.active()) {
            if (event == Event::CtrlC) {
                runtimes_.close();
                return true;
            }
            if (runtimes_.handle(event) == RuntimeAction::Close) {
                runtimes_.close();
            }
            return true;
        }

        if (sessions_.active()) {
            switch (sessions_.handle(event)) {
                case SessionPickerAction::Resume: {
                    const std::string chosen = sessions_.chosen();
                    sessions_.close();
                    resume_session(chosen);
                    return true;
                }
                case SessionPickerAction::Delete: {
                    std::string error;
                    if (!store_.remove(sessions_.chosen(), error)) {
                        say(error);
                    }
                    sessions_.refresh(store_);
                    return true;
                }
                case SessionPickerAction::Close:
                case SessionPickerAction::None:
                    break;
            }
            return true;
        }

        // The settings screen takes the keyboard while it is open, apart from
        // Ctrl-C, so there is always a way out.
        if (in_settings_) {
            if (event == Event::CtrlC) {
                in_settings_ = false;
                return true;
            }
            bool consumed = false;
            switch (settings_.handle(event, consumed)) {
                case SettingsAction::Close:
                    in_settings_ = false;
                    return true;
                case SettingsAction::Apply:
                    save_settings();
                    return true;
                case SettingsAction::OpenRuntimes:
                    runtimes_.open();
                    return true;
                case SettingsAction::None:
                    break;
            }
            return consumed;
        }

        if (event == Event::CtrlE) {
            open_settings();
            return true;
        }

        if (event == Event::CtrlC) {
            // While BatBot is working, Ctrl-C stops the work rather than the
            // program -- the same contract every other REPL offers.
            if (state_.busy()) {
                engine_->cancel();
                return true;
            }
            should_exit_ = true;
            screen_.Exit();
            return true;
        }

        if (event == Event::CtrlT) {
            show_roundtable_ = !show_roundtable_;
            return true;
        }

        if (event == Event::PageUp) {
            const std::size_t turns = state_.snapshot().turns.size();
            if (turns > 0) {
                if (follow_) {
                    follow_     = false;
                    focus_turn_ = static_cast<int>(turns) - 1;
                }
                focus_turn_ = std::max(0, focus_turn_ - 1);
            }
            return true;
        }

        if (event == Event::PageDown) {
            const auto turns = static_cast<int>(state_.snapshot().turns.size());
            if (!follow_) {
                ++focus_turn_;
                if (focus_turn_ >= turns - 1) {
                    follow_ = true;  // back at the bottom: resume following
                }
            }
            return true;
        }

        return false;
    });

    screen_.Loop(root);

    stop_ticker();
    // Join the worker before any member is destroyed: its wake callback holds
    // a reference to the screen.
    engine_->stop();
    return 0;
}

}  // namespace batbot::ui
