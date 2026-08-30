// SPDX-License-Identifier: MIT
//
// Slash commands, and the notice channel they print through.
//
// Kept apart from the application shell so adding a command means editing one
// list in one file, rather than picking through the render and event loops.
#include "batbot/ui/app.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "batbot/config/paths.hpp"
#include "batbot/llm/model_catalog.hpp"
#include "batbot/runtime/devices.hpp"
#include "batbot/session/usage.hpp"
#include "batbot/ui/theme.hpp"
#include "batbot/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {

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
    rest = format::trim(rest);

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
        say("/resume               reopen an earlier conversation about this project");
        say("/new                  start a fresh conversation, keeping this one on disk");
        say("/usage                tokens spent this session and on this project");
        say("/experts              which seats are filled");
        say("/runtimes             install or remove CUDA / Vulkan / CPU backends");
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
        say("config:   " + paths::config_file().string());
        say("models:   " + config_.resolved_models_dir().string());
        say("runtimes: " + paths::runtimes_dir().string());
        say("project:  " + store_.project().dir.string());
        say("log:      " + paths::log_file().string());
        say("trust:    " + paths::trust_file().string());
        return true;
    }

    if (command == "resume" || command == "sessions") {
        state_.clear_notices();
        sessions_.open(store_);
        return true;
    }

    if (command == "new") {
        // The conversation so far is already on disk; this simply stops
        // appending to it, so /resume can still find it.
        persist_session();
        store_.begin_new_session();
        persisted_turns_ = 0;
        state_.clear_turns();
        state_.clear_notices();
        engine_->reset_history();
        follow_ = true;
        say("started a new conversation -- /resume reopens the previous one");
        return true;
    }

    if (command == "usage") {
        state_.clear_notices();
        const Snapshot snapshot = state_.snapshot();
        const TokenUsage& session = snapshot.session_usage;
        const TokenUsage& project = snapshot.project_usage;

        say("this session:  " + format_tokens(session.input_tokens) + " in, " +
            format_tokens(session.output_tokens) + " out, over " +
            std::to_string(session.turns) +
            (session.turns == 1 ? " turn" : " turns"));
        if (session.tokens_per_second() > 0.0) {
            say("               " + format::number(session.tokens_per_second(), 1) +
                " tok/s generating");
        }
        say("this project:  " + format_tokens(project.input_tokens) + " in, " +
            format_tokens(project.output_tokens) + " out, over " +
            std::to_string(project.turns) +
            (project.turns == 1 ? " turn" : " turns"));
        say("               " + store_.project().root.string());
        return true;
    }

    if (command == "runtimes" || command == "runtime") {
        // Straight to the panel rather than printing a list: every question
        // this command answers is on that screen, and the answer is usually
        // followed by wanting to change something.
        state_.clear_notices();
        open_settings();
        runtimes_.open();
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
        const std::vector<ComputeDevice> devices = compute_devices();
        if (devices.empty()) {
            say("no compute devices -- no runtime is loaded (see /runtimes)");
        }
        // The index is the point of this listing: it is what the GPU priority
        // order and Main GPU settings are written in terms of.
        for (const ComputeDevice& device : devices) {
            say("[" + std::to_string(device.index) + "] " + device.label() +
                "   " + device.backend + (device.is_gpu ? "" : "  (cpu)"));
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

}  // namespace batbot::ui
