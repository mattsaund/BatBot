#include "batbot/ui/app.hpp"

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>

#include "batbot/core/paths.hpp"
#include "batbot/ui/roundtable.hpp"
#include "batbot/ui/settings.hpp"
#include "batbot/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {
namespace {

std::string trim(std::string text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string format_double(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

/// Milliseconds as something a human reads at a glance: "840ms", "3.2s".
std::string format_duration(long ms) {
    if (ms < 1000) {
        return std::to_string(ms) + "ms";
    }
    return format_double(static_cast<double>(ms) / 1000.0, 1) + "s";
}

}  // namespace

App::App(Config config, const std::vector<std::string>& warnings)
    : config_(std::move(config)),
      bat_(config_.ui.unicode),
      settings_(config_),
      screen_(ScreenInteractive::Fullscreen()) {
    show_roundtable_ = config_.ui.show_roundtable;

    for (const std::string& warning : warnings) {
        state_.add_notice(warning);
    }
    state_.configure_seats(config_);

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
    stop_ticker();
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

Element App::render_turn(const Turn& turn) const {
    Elements block;

    block.push_back(hbox({
        text("you ") | color(theme::kUser) | bold,
        text("▸ ") | color(theme::kMeta),
        paragraph(turn.prompt) | flex,
    }));

    // The route line is the whole point of the roundtable made textual: which
    // expert took this turn, how sure BatBot was, and what the swap cost.
    if (turn.route) {
        const RouteDecision& route = *turn.route;
        std::string line = "⟶ " + std::string(subject_name(route.subject));
        line += " · " + format_double(route.confidence, 2);
        line += " · " + std::string(route_source_name(route.source));
        if (!route.detail.empty()) {
            line += " · " + route.detail;
        }
        if (turn.load_ms > 0) {
            line += " · swap " + format_duration(turn.load_ms);
        }
        block.push_back(text(line) | color(theme::kRoute));
    }

    if (!turn.reply.empty()) {
        Element body = paragraph(turn.reply);
        block.push_back(turn.failed ? body | color(theme::kError) : body);
    } else if (turn.streaming) {
        block.push_back(text("…") | color(theme::kMeta) | dim);
    }

    if (!turn.streaming && turn.output_tokens > 0) {
        std::string stats = std::to_string(turn.output_tokens) + " tok";
        if (turn.tokens_per_second > 0.0) {
            stats += " · " + format_double(turn.tokens_per_second, 1) + " tok/s";
        }
        if (turn.cancelled) {
            stats += " · cancelled";
        }
        block.push_back(text(stats) | color(theme::kMeta) | dim);
    }

    block.push_back(text(" "));
    return vbox(std::move(block));
}

Element App::render_welcome() const {
    const std::vector<Subject> configured = config_.configured_experts();
    if (!configured.empty()) {
        return vbox({
            text("BatBot is ready. " + std::to_string(configured.size())
                 + " of 9 expert seats are filled.") | color(theme::kMeta),
            text("Ask anything; BatBot picks the expert. /help lists the commands.")
                | color(theme::kMeta) | dim,
            text(" "),
        });
    }

    // First run. This is the screen most people will see first, so it says
    // exactly what to do rather than merely reporting that nothing works.
    // BatBot ships no models, so this is also where that expectation is set.
    return vbox({
        text("No expert models are assigned yet.") | color(theme::kNotice) | bold,
        text(" "),
        text("BatBot does not come with models -- bring your own GGUF files.")
            | color(theme::kMeta),
        text(" "),
        text("  1. Put your .gguf files in the models directory:") | color(theme::kMeta),
        text("       " + config_.resolved_models_dir().string()) | color(theme::kAccent),
        text("  2. Press ctrl-e to open settings.") | color(theme::kMeta),
        text("  3. Assign a model to the delegator and to any expert seats") | color(theme::kMeta),
        text("     you want, then ctrl-s to save.") | color(theme::kMeta),
        text(" "),
        text("A ~1B instruct model makes a good delegator. Any model can stand in")
            | color(theme::kMeta) | dim,
        text("as an expert while you try things out.") | color(theme::kMeta) | dim,
        text(" "),
    });
}

Element App::render_transcript(const Snapshot& snapshot) const {
    Elements rows;

    if (!snapshot.notices.empty()) {
        Elements notices;
        for (const std::string& notice : snapshot.notices) {
            // paragraph, not text: several of these are long enough to be cut
            // off at the panel edge, and a truncated warning is a useless one.
            notices.push_back(hbox({
                text("• ") | color(theme::kNotice),
                paragraph(notice) | color(theme::kNotice) | dim | flex,
            }));
        }
        rows.push_back(vbox(std::move(notices)));
        rows.push_back(text(" "));
    }

    if (snapshot.turns.empty()) {
        rows.push_back(render_welcome());
    }

    for (std::size_t i = 0; i < snapshot.turns.size(); ++i) {
        Element block = render_turn(snapshot.turns[i]);
        // Focus drives the scroll position: either the newest turn (follow
        // mode) or whichever turn the user paged to.
        const bool focused = follow_ ? (i + 1 == snapshot.turns.size())
                                     : (static_cast<int>(i) == focus_turn_);
        rows.push_back(focused ? block | ftxui::focus : block);
    }

    return vbox(std::move(rows)) | yframe;
}

Element App::render_status(const Snapshot& snapshot) const {
    std::string left = "● " + std::string(mood_label(snapshot.mood));
    if (snapshot.resident) {
        left += "  │  resident: " + std::string(subject_name(*snapshot.resident));
    } else {
        left += "  │  no expert loaded";
    }

    const std::string right = snapshot.busy
        ? "ctrl-c cancel  ·  ctrl-e settings  ·  ctrl-t table  ·  /help"
        : "ctrl-c quit  ·  ctrl-e settings  ·  ctrl-t table  ·  pgup/pgdn  ·  /help";

    return hbox({
        text(left) | color(mood_color(snapshot.mood)),
        filler(),
        text(right) | color(theme::kMeta) | dim,
    });
}

Element App::render() {
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

    return window(text(" BatBot " BATBOT_VERSION " ") | bold | color(theme::kBat),
                  vbox(std::move(rows)));
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

void App::say(std::string message) {
    state_.add_notice(std::move(message));
    screen_.PostEvent(Event::Custom);
}

bool App::handle_command(const std::string& text) {
    if (text.empty() || text.front() != '/') {
        return false;
    }

    std::istringstream stream(text.substr(1));
    std::string command;
    stream >> command;
    std::string rest;
    std::getline(stream, rest);
    rest = trim(rest);

    std::transform(command.begin(), command.end(), command.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (command == "quit" || command == "exit" || command == "q") {
        should_exit_ = true;
        screen_.Exit();
        return true;
    }

    if (command == "help" || command == "h") {
        state_.clear_notices();
        say("/help                 this list");
        say("/<subject> <prompt>   send straight to one expert, e.g. /physics why is the sky blue");
        say("/experts              which seats are filled");
        say("/devices              compute devices llama.cpp found");
        say("/release              unload the resident expert and free its memory");
        say("/clear                clear the transcript and the experts' history");
        say("/config               open the settings screen (also ctrl-e)");
        say("/models               list the .gguf files in the models directory");
        say("/paths                where the config, models and log live");
        say("/quit                 leave");
        return true;
    }

    if (command == "clear") {
        state_.clear_turns();
        state_.clear_notices();
        engine_->reset_history();
        follow_ = true;
        return true;
    }

    if (command == "config" || command == "settings") {
        open_settings();
        return true;
    }

    if (command == "paths") {
        state_.clear_notices();
        say("config: " + paths::config_file().string());
        say("models: " + config_.resolved_models_dir().string());
        say("log:    " + paths::log_file().string());
        say("trust:  " + paths::trust_file().string());
        return true;
    }

    if (command == "models") {
        state_.clear_notices();
        const std::filesystem::path dir = config_.resolved_models_dir();
        const std::vector<ModelFile> found = scan_models(dir);
        say("models directory: " + dir.string());
        if (found.empty()) {
            say("no .gguf files here yet -- drop some in, then use /config to assign them");
        }
        for (const ModelFile& file : found) {
            say("  " + file.name + "  (" + file.size_label() + ")");
        }
        return true;
    }

    if (command == "experts") {
        state_.clear_notices();
        say("models directory: " + config_.resolved_models_dir().string());
        for (const SubjectInfo& info : all_subjects()) {
            const ModelParams& params = config_.experts[static_cast<std::size_t>(info.subject)];
            if (params.model.empty()) {
                say(std::string(info.name) + ": (no model assigned)");
            } else if (!std::filesystem::exists(params.path)) {
                say(std::string(info.name) + ": " + params.model + "  -- MISSING at " + params.path);
            } else {
                say(std::string(info.name) + ": " + params.model);
            }
        }
        return true;
    }

    if (command == "devices") {
        state_.clear_notices();
        const std::vector<std::string> devices = ModelHost::devices();
        if (devices.empty()) {
            say("no compute devices reported yet");
        }
        for (const std::string& device : devices) {
            say("device: " + device);
        }
        return true;
    }

    if (command == "release") {
        engine_->release_expert();
        return true;
    }

    // Anything else that names a subject pins that expert for this prompt.
    if (const std::optional<Subject> subject = subject_from_string(command)) {
        if (rest.empty()) {
            say("usage: /" + command + " <prompt>");
            return true;
        }
        state_.clear_notices();
        engine_->submit(rest, subject);
        follow_ = true;
        return true;
    }

    say("unknown command: /" + command + "  (try /help)");
    return true;
}

void App::on_submit() {
    const std::string text = trim(input_text_);
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
