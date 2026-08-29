// SPDX-License-Identifier: MIT
// The terminal application: layout, input handling, slash commands, and the
// animation clock that keeps BatBot moving while an expert loads.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "batbot/config/config.hpp"
#include "batbot/engine/engine.hpp"
#include "batbot/engine/state.hpp"
#include "batbot/ui/widgets/bat_sprite.hpp"
#include "batbot/ui/settings/settings_view.hpp"

namespace batbot::ui {

class App {
public:
    App(Config config, const std::vector<std::string>& warnings);
    ~App();
    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    /// Run the TUI until the user quits. Returns a process exit code.
    int run();

private:
    /// How much of the roundtable the terminal has room for.
    enum class TableView { Full, Compact, Strip, Hidden };

    ftxui::Element render();
    void open_settings();
    void save_settings();
    ftxui::Element render_transcript(const Snapshot& snapshot) const;
    ftxui::Element render_turn(const Turn& turn) const;
    ftxui::Element render_welcome() const;
    ftxui::Element render_status(const Snapshot& snapshot) const;

    void on_submit();
    /// Returns true if the text was a slash command and has been dealt with.
    bool handle_command(const std::string& text);
    void say(std::string message);

    TableView table_view() const;
    void start_ticker();
    void stop_ticker();

    Config                  config_;
    AppState                state_;
    BatSprite               bat_;
    SettingsView            settings_;
    bool                    in_settings_ = false;
    std::unique_ptr<Engine> engine_;

    ftxui::ScreenInteractive screen_;
    ftxui::Component         input_;
    std::string              input_text_;

    std::thread       ticker_;
    std::atomic<bool> ticking_{false};
    std::atomic<std::size_t> tick_{0};

    bool show_roundtable_ = true;
    bool follow_          = true;   ///< pin the transcript to the newest turn
    int  focus_turn_      = 0;
    bool should_exit_     = false;
};

}  // namespace batbot::ui
