// SPDX-License-Identifier: MIT
#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/markdown.hpp"
#include "theme.hpp"

namespace crucible::gui {
namespace {

/// Layout in multiples of the font size rather than in pixels.
///
/// The interface is loaded at the display's own scale, so a sidebar written as
/// 268 pixels is two thirds the width it should be on a 4K panel and the
/// composer under it gets clipped. Everything laid out here is therefore in
/// `em`, which tracks whatever size the font was actually loaded at.
float em(float n) {
    return n * ImGui::GetFontSize();
}

void text_coloured(ImU32 colour, const char* fmt, ...) IM_FMTARGS(2);
void text_coloured(ImU32 colour, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(colour));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

/// A model reference as it should be read.
///
/// The config stores a bare file name for a model in the models directory and
/// an absolute path for one anywhere else. The path is the useful thing to keep
/// and the useless thing to show: a combo box twenty ems wide renders
/// "/mnt/media_drive/.models/lmstudio-community/Qwen3-Cod" and stops, which
/// says nothing about which model it is. The full path is the tooltip.
std::string model_label(const std::string& reference) {
    if (reference.empty()) {
        return "(none)";
    }
    const std::size_t slash = reference.find_last_of("/\\");
    return slash == std::string::npos ? reference : reference.substr(slash + 1);
}

void wrapped(ImU32 colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(colour));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

/// A section heading in the sidebar: small, faint, spaced out.
void section(const char* label) {
    ImGui::Dummy(ImVec2(0, em(0.38F)));
    text_coloured(theme::kTextFaint, "%s", label);
    ImGui::Dummy(ImVec2(0, em(0.12F)));
}

theme::Dot dot_for(SeatPhase phase) {
    switch (phase) {
        case SeatPhase::Active:       return theme::Dot::Active;
        case SeatPhase::Loading:      return theme::Dot::Loading;
        case SeatPhase::Dormant:      return theme::Dot::Ready;
        case SeatPhase::Missing:      return theme::Dot::Missing;
        case SeatPhase::Unconfigured: break;
    }
    return theme::Dot::Empty;
}

const char* mood_text(Mood mood) {
    switch (mood) {
        case Mood::Routing:  return "routing";
        case Mood::Loading:  return "loading";
        case Mood::Thinking: return "thinking";
        case Mood::Talking:  return "answering";
        case Mood::Error:    return "error";
        case Mood::Idle:     break;
    }
    return "idle";
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

App::App(Config config, std::vector<std::string> warnings)
    : config_(std::move(config)), store_(Project::current()) {
    for (std::string& warning : warnings) {
        notices_.push_back(std::move(warning));
    }

    state_.configure_seats(config_);
    state_.set_project_usage(store_.project_usage());

    engine_ = std::make_unique<Engine>(config_, state_, [this] {
        // The engine runs on its own thread and the window may be parked in
        // glfwWaitEvents. Without this the screen would not repaint until the
        // mouse moved, which during a model load is most of a minute.
        glfwPostEmptyEvent();
    });
    engine_->set_journal_dir(store_.project().dir);
    refresh_models();
}

App::~App() {
    if (engine_) {
        engine_->stop();
    }
}

void App::say(std::string message) {
    notices_.push_back(std::move(message));
    // Only the last few. This is a status channel, not a log; the log is on
    // disk and the transcript is above it.
    if (notices_.size() > 6) {
        notices_.erase(notices_.begin());
    }
}

void App::refresh_models() {
    models_ = scan_models(config_.resolved_models_dir());
}

void App::update_config(const std::function<void(Config&)>& change) {
    change(config_);
    config_.resolve_models();
    state_.configure_seats(config_);
    if (!save_config(config_)) {
        say("could not write " + paths::config_file().string());
    }
    engine_->apply_config(config_);
}

void App::persist_session() {
    const Snapshot snapshot = state_.snapshot();
    std::size_t finished = 0;
    for (const Turn& turn : snapshot.turns) {
        finished += turn.streaming ? 0 : 1;
    }
    if (finished == persisted_turns_) {
        return;
    }
    std::string error;
    if (store_.save(snapshot.turns, snapshot.session_usage, error)) {
        persisted_turns_ = finished;
    }
}

void App::absorb_written_examples() {
    const std::vector<std::pair<ExpertId, std::vector<std::string>>> written =
        engine_->take_written_examples();
    if (written.empty()) {
        return;
    }
    update_config([&written](Config& config) {
        for (const auto& [id, examples] : written) {
            if (const std::optional<std::size_t> seat = config.roster.find(id)) {
                Expert expert = config.roster.at(*seat);
                expert.examples = examples;
                config.roster.update(id, expert);
            }
        }
    });
    for (const auto& [id, examples] : written) {
        say(expert_label(config_.roster, id) + ": the delegator wrote "
            + std::to_string(examples.size()) + " example questions to route on");
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void App::submit_prompt() {
    const std::string text = format::trim(prompt_);
    if (text.empty()) {
        return;
    }
    prompt_.clear();
    follow_ = true;

    // A cook waiting on a question takes the next thing typed as its answer.
    // The screen is showing a question; nothing else would be a reasonable
    // reading of a line typed under it.
    if (const std::shared_ptr<const Cook> cook = state_.cook();
        cook && cook->state == CookState::Asking) {
        engine_->answer_cook(text);
        return;
    }
    engine_->submit(text);
}

void App::begin_cook() {
    const std::string goal = format::trim(cook_goal_);
    if (goal.empty()) {
        say("a cook needs a goal");
        return;
    }
    if (!config_.tools.workshop) {
        // Refused here rather than several model calls later, where it would
        // surface as the expert being told the workshop is off, over and over.
        say("cooking needs the workshop, which is off -- turn it on in Settings");
        view_ = View::Settings;
        return;
    }
    cook_goal_.clear();
    follow_ = true;
    view_   = View::Cook;
    engine_->start_cook(goal, cook_untimed_ ? 0 : cook_minutes_ * 60,
                        store_.project().root);
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------

void App::draw_markdown(const std::string& text, ImU32 colour) {
    // The same parser the terminal uses, so a reply is broken into the same
    // blocks in both faces and only the drawing differs.
    for (const markdown::Block& block : markdown::parse(text)) {
        switch (block.kind) {
            case markdown::BlockKind::Blank:
                ImGui::Dummy(ImVec2(0, em(0.25F)));
                continue;
            case markdown::BlockKind::Rule:
                ImGui::Separator();
                continue;
            case markdown::BlockKind::Code: {
                // Monospace, and not wrapped. Code is the one place a
                // proportional face is actively worse and where alignment is
                // part of the meaning; a long line scrolls rather than folding.
                std::string line;
                for (const markdown::Span& span : block.spans) {
                    line += span.text;
                }
                ImGui::PushFont(theme::mono());
                ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kText));
                ImGui::TextUnformatted(("  " + line).c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                continue;
            }
            default:
                break;
        }

        std::string line;
        if (block.kind == markdown::BlockKind::Bullet) {
            line += std::string(static_cast<std::size_t>(block.level) * 2, ' ') + "- ";
        } else if (block.kind == markdown::BlockKind::Numbered) {
            line += block.marker + " ";
        } else if (block.kind == markdown::BlockKind::Quote) {
            line += "| ";
        } else if (block.kind == markdown::BlockKind::TableRow) {
            for (std::size_t i = 0; i < block.cells.size(); ++i) {
                if (i > 0) {
                    line += "   ";
                }
                for (const markdown::Span& span : block.cells[i]) {
                    line += span.text;
                }
            }
        }
        for (const markdown::Span& span : block.spans) {
            line += span.text;
        }
        if (block.kind == markdown::BlockKind::TableRule) {
            continue;  // the ruled line under a header carries no text
        }

        const ImU32 shade = block.kind == markdown::BlockKind::Heading ? theme::kFlame
                          : block.kind == markdown::BlockKind::Quote   ? theme::kTextDim
                                                                       : colour;
        wrapped(shade, line);
    }
}

// ---------------------------------------------------------------------------
// The sidebar
// ---------------------------------------------------------------------------

void App::draw_sidebar(const Snapshot& snapshot) {
    ImGui::BeginChild("sidebar", ImVec2(em(17.0F), 0), ImGuiChildFlags_Borders);

    // The mark, and the fire under it saying what the machine is doing. The
    // same idea as the terminal sprite: how hard it is burning is how hard the
    // engine is working.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList*  draw   = ImGui::GetWindowDrawList();
    const float  flicker = snapshot.busy ? 0.86F + 0.14F * std::sin(phase_ * 9.0F) : 0.72F;
    theme::draw_crucible(draw, ImVec2(origin.x + em(1.3F), origin.y + em(1.3F)),
                         em(1.0F) * flicker);

    ImGui::SetCursorScreenPos(ImVec2(origin.x + em(3.0F), origin.y + em(0.4F)));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kText));
    ImGui::TextUnformatted("CRUCIBLE");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + em(3.0F), origin.y + em(1.6F)));
    text_coloured(snapshot.busy ? theme::kFlameBright : theme::kTextDim, "%s",
                  snapshot.status.empty() ? mood_text(snapshot.mood) : snapshot.status.c_str());
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + em(3.2F)));

    ImGui::Dummy(ImVec2(0, em(0.12F)));
    ImGui::Separator();

    // --- the roundtable ---------------------------------------------------
    section("ROUNDTABLE");
    const Roster& roster = snapshot.roster ? *snapshot.roster : config_.roster;
    for (std::size_t i = 0; i < roster.size(); ++i) {
        const Expert&    expert = roster.at(i);
        const SeatState& seat   = i < snapshot.seats.size() ? snapshot.seats[i] : SeatState{};
        const bool linked = snapshot.linked && *snapshot.linked == expert.id;

        const ImVec2 at = ImGui::GetCursorScreenPos();
        theme::draw_dot(ImGui::GetWindowDrawList(),
                        ImVec2(at.x + em(0.42F), at.y + em(0.55F)), em(0.30F),
                        dot_for(seat.phase), phase_);
        ImGui::Dummy(ImVec2(em(1.05F), em(1.1F)));
        ImGui::SameLine();

        std::string label = expert.name;
        if (seat.phase == SeatPhase::Loading) {
            label += "  " + std::to_string(static_cast<int>(seat.progress * 100.0F)) + "%";
        }
        const ImU32 colour = linked                              ? theme::kFlameBright
                           : seat.phase == SeatPhase::Unconfigured ? theme::kTextFaint
                                                                   : theme::kText;
        text_coloured(colour, "%s", label.c_str());
        if (ImGui::IsItemHovered() && !expert.blurb.empty()) {
            ImGui::SetTooltip("%s", expert.blurb.c_str());
        }
    }

    ImGui::Dummy(ImVec2(0, em(0.38F)));
    if (ImGui::Button("+ New expert", ImVec2(-FLT_MIN, 0))) {
        expert_modal_open_ = true;
        new_expert_name_.clear();
        new_expert_blurb_.clear();
        expert_error_.clear();
    }

    // --- navigation --------------------------------------------------------
    section("VIEW");
    const auto nav = [this](const char* label, View view) {
        if (ImGui::Selectable(label, view_ == view, 0, ImVec2(0, em(1.5F)))) {
            view_ = view;
        }
    };
    nav("Chat",     View::Chat);
    nav("Cook",     View::Cook);
    nav("Experts",  View::Experts);
    nav("Settings", View::Settings);
    nav("History",  View::History);

    // --- the project -------------------------------------------------------
    section("PROJECT");
    wrapped(theme::kTextDim, store_.project().root.string());

    const TokenUsage& usage = snapshot.session_usage;
    ImGui::Dummy(ImVec2(0, em(0.25F)));
    text_coloured(theme::kTextFaint, "%s in / %s out",
                  format_tokens(usage.input_tokens).c_str(),
                  format_tokens(usage.output_tokens).c_str());

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Panes
// ---------------------------------------------------------------------------

void App::draw_chat(const Snapshot& snapshot) {
    if (snapshot.turns.empty() && notices_.empty()) {
        ImGui::Dummy(ImVec2(0, em(2.50F)));
        wrapped(theme::kTextDim, config_.configured_experts().empty()
            ? "No expert models are assigned yet. Open Experts and point one at a GGUF file."
            : "Ask anything and Crucible picks the expert. Or give it a goal and let it cook.");
    }

    for (const std::string& notice : notices_) {
        wrapped(theme::kTextDim, "- " + notice);
    }
    if (!notices_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.50F)));
    }

    for (const Turn& turn : snapshot.turns) {
        text_coloured(theme::kFlame, "you");
        wrapped(theme::kText, turn.prompt);
        ImGui::Dummy(ImVec2(0, em(0.25F)));

        if (turn.route) {
            std::string line = expert_label(config_.roster, turn.route->expert);
            line += "  ·  " + format::number(turn.route->confidence, 2);
            line += "  ·  " + std::string(route_source_name(turn.route->source));
            if (turn.load_ms > 0) {
                line += "  ·  swap " + format::duration_ms(turn.load_ms);
            }
            text_coloured(theme::kFlameBright, "%s", line.c_str());
        }

        if (config_.ui.show_reasoning && !turn.reasoning.empty()) {
            wrapped(theme::kTextFaint, turn.reasoning);
            ImGui::Dummy(ImVec2(0, em(0.25F)));
        }
        for (const std::string& search : turn.searches) {
            text_coloured(theme::kTextDim, "  searched: %s", search.c_str());
        }

        draw_markdown(turn.reply, turn.failed ? theme::kError : theme::kText);

        if (turn.tokens_per_second > 0.0) {
            text_coloured(theme::kTextFaint, "%s tok/s  ·  %d tokens",
                          format::number(turn.tokens_per_second, 1).c_str(),
                          turn.output_tokens);
        }
        ImGui::Dummy(ImVec2(0, em(0.88F)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em(0.50F)));
    }

    if (snapshot.cook) {
        draw_cook_pane(snapshot);
    }
}

void App::draw_cook_pane(const Snapshot& snapshot) {
    if (!snapshot.cook) {
        ImGui::Dummy(ImVec2(0, em(2.50F)));
        wrapped(theme::kTextDim,
                "Nothing is cooking. Give Crucible a goal below and a time to work on it, "
                "and it will read the project, change it, run it, and keep going.");
        return;
    }
    const Cook& cook = *snapshot.cook;

    text_coloured(theme::kFlame, "COOK");
    wrapped(theme::kText, cook.goal);

    std::string clock = format_duration(cook.duration());
    if (cook.budget_seconds > 0 && cook.state == CookState::Working) {
        const long left = cook.budget_seconds - cook.duration().count();
        clock += left > 0 ? "  ·  " + format_duration(std::chrono::seconds{left}) + " left"
                          : "  ·  time up";
    }
    text_coloured(theme::kTextDim, "%s  ·  round %d  ·  %s",
                  std::string(cook_state_name(cook.state)).c_str(), cook.iterations,
                  clock.c_str());

    if (cook.budget_seconds > 0) {
        const float done = std::clamp(
            static_cast<float>(cook.duration().count())
                / static_cast<float>(cook.budget_seconds), 0.0F, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::to_vec(theme::kFlame));
        ImGui::ProgressBar(done, ImVec2(-FLT_MIN, em(0.25F)), "");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, em(0.50F)));

    for (const CookStep& step : cook.steps) {
        const ImU32 colour = !step.ok                ? theme::kError
                           : step.kind == "write"    ? theme::kFlameBright
                           : step.kind == "run"      ? theme::kFlame
                           : step.kind == "done"     ? theme::kFlame
                                                     : theme::kTextFaint;
        // The verb in monospace so the column lines up, which is what makes a
        // hundred steps scannable rather than a wall.
        ImGui::PushFont(theme::mono());
        ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(colour));
        ImGui::TextUnformatted(step.kind.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine(em(4.6F));
        wrapped(step.ok ? theme::kText : theme::kError, step.summary);
    }

    if (cook.state == CookState::Asking && !cook.question.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.62F)));
        text_coloured(theme::kFlameBright, "It is asking:");
        wrapped(theme::kText, cook.question);
        text_coloured(theme::kTextFaint, "type an answer below and press enter");
    }

    if (!cook.outcome.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.62F)));
        ImGui::Separator();
        wrapped(theme::kTextDim, cook.outcome);
    }

    const std::vector<std::string> files = cook.files_touched();
    const bool finished = cook.state == CookState::Done || cook.state == CookState::Stopped
                       || cook.state == CookState::Failed;
    ImGui::Dummy(ImVec2(0, em(0.38F)));
    if (!files.empty()) {
        std::string list;
        for (std::size_t i = 0; i < files.size(); ++i) {
            list += (i == 0 ? "" : ", ") + files[i];
        }
        text_coloured(theme::kTextFaint, "changed");
        wrapped(theme::kFlameBright, list);
    } else if (finished) {
        // The outcome above is the expert's account of itself; this is the
        // fact. A model that talked its way through an edit it never made
        // writes a confident summary of having made it, and the only thing that
        // catches that is the journal saying nothing was written.
        text_coloured(theme::kError, "changed no files -- whatever it says, nothing on disk moved");
    }
}

void App::draw_experts() {
    text_coloured(theme::kFlame, "EXPERTS");
    wrapped(theme::kTextDim,
            "The delegator routes each prompt to one of these. Add your own with a name and "
            "a description of what it handles; everything else is worked out for you.");
    ImGui::Dummy(ImVec2(0, em(0.50F)));

    if (ImGui::Button("Rescan models directory")) {
        refresh_models();
        say("found " + std::to_string(models_.size()) + " GGUF files");
    }
    ImGui::SameLine();
    if (ImGui::Button("+ New expert")) {
        expert_modal_open_ = true;
        new_expert_name_.clear();
        new_expert_blurb_.clear();
        expert_error_.clear();
    }
    ImGui::Dummy(ImVec2(0, em(0.50F)));

    std::optional<ExpertId> eject;
    for (const Expert& expert : config_.roster.experts()) {
        ImGui::PushID(expert.id.c_str());
        ImGui::Separator();

        text_coloured(theme::kText, "%s", expert.name.c_str());
        ImGui::SameLine();
        text_coloured(theme::kTextFaint, "[%s]", expert.tag.c_str());
        wrapped(theme::kTextDim, expert.blurb);

        // The model assignment. A combo rather than a text box: the models
        // directory is the list of valid answers, and typing a file name is
        // how you get a seat that points at nothing.
        const ModelParams& params = config_.expert(expert.id);
        ImGui::SetNextItemWidth(em(20.0F));
        const bool open = ImGui::BeginCombo("##model", model_label(params.model).c_str());
        if (!params.model.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", params.path.empty() ? params.model.c_str()
                                                        : params.path.c_str());
        }
        if (open) {
            if (ImGui::Selectable("(none)", params.model.empty())) {
                update_config([&expert](Config& config) {
                    config.experts[expert.id].model.clear();
                    config.experts[expert.id].path.clear();
                });
            }
            for (const ModelFile& file : models_) {
                const bool chosen = file.name == params.model;
                if (ImGui::Selectable((file.name + "   " + file.size_label()).c_str(), chosen)) {
                    update_config([&expert, &file](Config& config) {
                        config.experts[expert.id].model = file.name;
                    });
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Eject")) {
            eject = expert.id;
        }
        ImGui::Dummy(ImVec2(0, em(0.25F)));
        ImGui::PopID();
    }

    // Applied after the loop: removing a seat while iterating over the roster
    // it belongs to would invalidate the iterator.
    if (eject) {
        const std::string name = expert_label(config_.roster, *eject);
        update_config([&eject](Config& config) {
            std::string error;
            config.roster.remove(*eject, error);
            config.experts.erase(*eject);
        });
        say(name + " has left the roundtable");
    }
}

void App::draw_settings() {
    text_coloured(theme::kFlame, "SETTINGS");
    ImGui::Dummy(ImVec2(0, em(0.50F)));

    text_coloured(theme::kTextFaint, "DELEGATOR");
    ImGui::SetNextItemWidth(em(20.0F));
    const bool router_open =
        ImGui::BeginCombo("Router model", model_label(config_.router.model).c_str());
    if (!config_.router.model.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", config_.router.path.c_str());
    }
    if (router_open) {
        if (ImGui::Selectable("(none)", config_.router.model.empty())) {
            update_config([](Config& config) { config.router.model.clear(); });
        }
        for (const ModelFile& file : models_) {
            if (ImGui::Selectable((file.name + "   " + file.size_label()).c_str(),
                                  file.name == config_.router.model)) {
                update_config([&file](Config& config) { config.router.model = file.name; });
            }
        }
        ImGui::EndCombo();
    }
    bool keep = config_.routing.keep_delegator_loaded;
    if (ImGui::Checkbox("Keep the delegator in memory between prompts", &keep)) {
        update_config([keep](Config& config) { config.routing.keep_delegator_loaded = keep; });
    }
    ImGui::SetItemTooltip("Off frees it after each decision, leaving the expert the whole card.");

    ImGui::Dummy(ImVec2(0, em(0.62F)));
    text_coloured(theme::kTextFaint, "WORKSHOP");
    wrapped(theme::kTextDim,
            "What a cook is allowed to do to this project. Off, Crucible only answers "
            "questions about it.");
    bool workshop = config_.tools.workshop;
    if (ImGui::Checkbox("Let experts read and write files here", &workshop)) {
        update_config([workshop](Config& config) { config.tools.workshop = workshop; });
    }
    text_coloured(theme::kTextFaint, "%s", store_.project().root.string().c_str());

    bool allow_run = config_.tools.workshop_run;
    if (ImGui::Checkbox("Let them run commands too", &allow_run)) {
        update_config([allow_run](Config& config) { config.tools.workshop_run = allow_run; });
    }
    int timeout = config_.tools.workshop_timeout;
    ImGui::SetNextItemWidth(em(10.0F));
    if (ImGui::SliderInt("Command timeout (s)", &timeout, 5, 600)) {
        update_config([timeout](Config& config) { config.tools.workshop_timeout = timeout; });
    }

    ImGui::Dummy(ImVec2(0, em(0.62F)));
    text_coloured(theme::kTextFaint, "TOOLS");
    bool web = config_.tools.web_search;
    if (ImGui::Checkbox("Web search", &web)) {
        update_config([web](Config& config) { config.tools.web_search = web; });
    }
    ImGui::SetItemTooltip("The only thing Crucible sends off this machine.");

    ImGui::Dummy(ImVec2(0, em(0.62F)));
    text_coloured(theme::kTextFaint, "HARDWARE");
    for (const ComputeDevice& device : compute_devices()) {
        text_coloured(theme::kTextDim, "[%d] %s  %s", device.index, device.label().c_str(),
                      device.backend.c_str());
    }
    if (compute_devices().empty()) {
        wrapped(theme::kTextDim,
                "No compute devices -- no runtime is installed. Install one from the terminal "
                "app with /runtimes; it compiles a GPU backend for this machine.");
    }

    ImGui::Dummy(ImVec2(0, em(0.62F)));
    text_coloured(theme::kTextFaint, "FILES");
    text_coloured(theme::kTextDim, "config   %s", paths::config_file().string().c_str());
    text_coloured(theme::kTextDim, "models   %s", config_.resolved_models_dir().string().c_str());
    text_coloured(theme::kTextDim, "runtimes %s", paths::runtimes_dir().string().c_str());
}

void App::draw_history() {
    text_coloured(theme::kFlame, "HISTORY");
    wrapped(theme::kTextDim, "Everything Crucible has done in this project.");
    ImGui::Dummy(ImVec2(0, em(0.62F)));

    text_coloured(theme::kTextFaint, "COOKS");
    const CookLog log(store_.project().dir);
    const std::vector<CookSummary> cooks = log.list();
    if (cooks.empty()) {
        wrapped(theme::kTextDim, "No cooks yet.");
    }
    for (const CookSummary& cook : cooks) {
        ImGui::Separator();
        text_coloured(theme::kText, "%s", cook.goal.c_str());
        text_coloured(theme::kTextDim, "%s  ·  %d %s  ·  %d steps  ·  %s  ·  %s",
                      cook.when().c_str(), cook.files,
                      cook.files == 1 ? "file" : "files", cook.steps,
                      format_duration(cook.duration).c_str(),
                      std::string(cook_state_name(cook.state)).c_str());
    }

    ImGui::Dummy(ImVec2(0, em(0.88F)));
    text_coloured(theme::kTextFaint, "CONVERSATIONS");
    const std::vector<SessionSummary> sessions = store_.list();
    if (sessions.empty()) {
        wrapped(theme::kTextDim, "No conversations yet.");
    }
    for (const SessionSummary& session : sessions) {
        ImGui::Separator();
        text_coloured(theme::kText, "%s", session.title.c_str());
        text_coloured(theme::kTextDim, "%s  ·  %d turns", session.when().c_str(), session.turns);
    }
}

// ---------------------------------------------------------------------------
// The composer
// ---------------------------------------------------------------------------

void App::draw_composer(const Snapshot& snapshot) {
    const std::shared_ptr<const Cook> cook = snapshot.cook;
    const bool asking  = cook && cook->state == CookState::Asking;
    const bool cooking = engine_->cooking();

    ImGui::BeginChild("composer", ImVec2(0, em(7.2F)),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    // --- the prompt line ---------------------------------------------------
    const char* hint = asking  ? "answer the question above"
                     : cooking ? "cooking -- ask anyway and it waits its turn"
                               : "ask anything";
    ImGui::SetNextItemWidth(-em(4.6F));
    const bool entered = ImGui::InputTextWithHint(
        "##prompt", hint, &prompt_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool send = ImGui::Button(asking ? "Answer" : "Send", ImVec2(-FLT_MIN, 0));
    if (entered || send) {
        submit_prompt();
        // Enter should leave the caret where it was, or every reply costs a
        // click to get back to typing.
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::Dummy(ImVec2(0, em(0.25F)));

    // --- the cook line -----------------------------------------------------
    if (cooking) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kFlameBright));
        ImGui::TextUnformatted("cooking");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Stop and finish", ImVec2(em(9.0F), 0))) {
            // Not a cancel: it makes a finishing pass to leave the project in a
            // state that runs.
            engine_->stop_cook();
            say("wrapping up -- finishing touches, then it will stop");
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop now", ImVec2(em(5.6F), 0))) {
            engine_->cancel();
        }
        ImGui::SetItemTooltip("Stops immediately, without the finishing pass.");
    } else {
        ImGui::SetNextItemWidth(-em(21.0F));
        ImGui::InputTextWithHint("##goal", "or give it a goal to cook on", &cook_goal_);
        ImGui::SameLine();

        ImGui::BeginDisabled(cook_untimed_);
        ImGui::SetNextItemWidth(em(6.2F));
        ImGui::SliderInt("##minutes", &cook_minutes_, 1, 180, "%d min");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("no limit", &cook_untimed_);
        ImGui::SetItemTooltip("Work until you stop it.");
        ImGui::SameLine();
        if (ImGui::Button("Cook", ImVec2(-FLT_MIN, 0))) {
            begin_cook();
        }
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// The new-expert dialog
// ---------------------------------------------------------------------------

void App::draw_new_expert_modal() {
    if (expert_modal_open_) {
        ImGui::OpenPopup("New expert");
        expert_modal_open_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(em(30.0F), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New expert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // Two boxes and nothing else. The id, the chip, the keyword set and the
    // worked examples the delegator routes on are all derived or generated,
    // because those are things a person should not have to invent.
    wrapped(theme::kTextDim,
            "A name, and what it is trained in. Crucible works out the rest, and the "
            "delegator writes its own example questions once it is loaded.");
    ImGui::Dummy(ImVec2(0, em(0.50F)));

    text_coloured(theme::kTextFaint, "Expert name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", "Rust Async, Tax Law, Kubernetes", &new_expert_name_);

    ImGui::Dummy(ImVec2(0, em(0.38F)));
    text_coloured(theme::kTextFaint, "Describe what the expert is trained in");
    ImGui::InputTextMultiline("##blurb", &new_expert_blurb_, ImVec2(-FLT_MIN, em(5.2F)));
    text_coloured(theme::kTextFaint,
                  "The delegator routes on this, so name the things it should take.");

    if (!expert_error_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.38F)));
        wrapped(theme::kError, expert_error_);
    }

    ImGui::Dummy(ImVec2(0, em(0.62F)));
    if (ImGui::Button("Add expert", ImVec2(em(8.0F), 0))) {
        Expert expert;
        expert.name  = format::trim(new_expert_name_);
        expert.blurb = format::trim(new_expert_blurb_);

        Config      edited = config_;
        std::string error;
        if (!edited.roster.add(expert, error)) {
            // The dialog stays open with what was typed still in it: a name
            // collision is fixed by editing the name, not by typing the
            // description again.
            expert_error_ = error;
        } else {
            const ExpertId id = make_expert_id(expert.name);
            edited.experts[id] = ModelParams{};
            update_config([&edited](Config& config) { config = edited; });
            engine_->write_examples(id);
            say(expert.name + " has joined the roundtable");
            new_expert_name_.clear();
            new_expert_blurb_.clear();
            expert_error_.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        expert_error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------

void App::draw() {
    const Snapshot snapshot = state_.snapshot();

    // One window filling the viewport. Crucible is an application, not a
    // collection of floating panels, and a desktop app that opens with its own
    // windows scattered over the screen looks like a debug build.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("crucible", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    draw_sidebar(snapshot);
    ImGui::SameLine();

    ImGui::BeginChild("main", ImVec2(0, 0));
    {
        const float composer =
            view_ == View::Chat || view_ == View::Cook ? em(7.7F) : 0.0F;
        ImGui::BeginChild("pane", ImVec2(0, -composer), ImGuiChildFlags_Borders);
        switch (view_) {
            case View::Chat:     draw_chat(snapshot);      break;
            case View::Cook:     draw_cook_pane(snapshot); break;
            case View::Experts:  draw_experts();           break;
            case View::Settings: draw_settings();          break;
            case View::History:  draw_history();           break;
        }
        // Following the bottom, but only while the user is already there.
        // Yanking someone reading back through an hour-old cook to the end
        // every time a token arrives is the single most irritating thing a
        // streaming view can do.
        if (follow_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndChild();

        if (composer > 0.0F) {
            draw_composer(snapshot);
        }
    }
    ImGui::EndChild();

    draw_new_expert_modal();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

int App::run() {
    glfwSetErrorCallback([](int code, const char* description) {
        std::fprintf(stderr, "crucible-gui: glfw error %d: %s\n", code, description);
    });
    if (glfwInit() == GLFW_FALSE) {
        std::fprintf(stderr, "crucible-gui: could not open a window. On Linux this usually "
                             "means there is no display, or no OpenGL driver.\n");
        return 1;
    }

    // GL 3.2 core: the oldest thing ImGui's backend is happy with, and old
    // enough that a decade-old integrated chip and a virtual machine both have
    // it. There is nothing here that wants a newer one.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    // Sized against the monitor rather than in fixed pixels: 1280x820 is a
    // reasonable window on a 1080p panel and a postage stamp on a 4K one.
    int width  = 1280;
    int height = 820;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor(); monitor != nullptr) {
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitor); mode != nullptr) {
            width  = std::clamp(static_cast<int>(mode->width * 0.62F), 1120, 2200);
            height = std::clamp(static_cast<int>(mode->height * 0.70F), 760, 1500);
        }
    }
    window_ = glfwCreateWindow(width, height, "Crucible", nullptr, nullptr);
    if (window_ == nullptr) {
        std::fprintf(stderr, "crucible-gui: could not create the window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini litter beside the project
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // The display's own scale, so the interface is the same physical size on a
    // 4K laptop panel as on a 1080p monitor. GLFW reports it per monitor; the
    // one the window opened on is the one that matters.
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor(); monitor != nullptr) {
        glfwGetMonitorContentScale(monitor, &scale_x, &scale_y);
    }
    const float scale = std::max(1.0F, scale_x);
    theme::load_fonts(scale);
    theme::apply();
    ImGui::GetStyle().ScaleAllSizes(scale);

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    engine_->start();

    double last = glfwGetTime();
    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        // Waiting rather than spinning. An idle Crucible should cost nothing,
        // and the engine posts an empty event whenever it has something new --
        // the timeout is only there so the cook clock and the fire keep moving.
        glfwWaitEventsTimeout(state_.busy() ? 0.05 : 0.25);

        const double now = glfwGetTime();
        phase_ += static_cast<float>(now - last);
        last = now;

        persist_session();
        absorb_written_examples();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        draw();
        ImGui::Render();

        int fb_width  = 0;
        int fb_height = 0;
        glfwGetFramebufferSize(window_, &fb_width, &fb_height);
        glViewport(0, 0, fb_width, fb_height);
        const ImVec4 ground = theme::to_vec(theme::kInk);
        glClearColor(ground.x, ground.y, ground.z, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }

    // The engine has a thread that calls back into this object, so it has to be
    // stopped before anything it might touch is torn down.
    engine_->stop();
    persist_session();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    return 0;
}

}  // namespace crucible::gui
