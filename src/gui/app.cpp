// SPDX-License-Identifier: MIT
#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <system_error>

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "markdown_view.hpp"
#include "theme.hpp"

namespace crucible::gui {
namespace {

/// Layout in multiples of the font size rather than in pixels.
///
/// The interface is loaded at the display's own scale, so a sidebar written as
/// 268 pixels is two thirds the width it should be on a 4K panel and the
/// composer under it gets clipped. Everything laid out here is in `em`, which
/// tracks whatever size the font was actually loaded at.
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

void wrapped(ImU32 colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(colour));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

/// A section heading: small, faint, spaced above.
void section(const char* label) {
    ImGui::Dummy(ImVec2(0, em(0.5F)));
    text_coloured(theme::kTextFaint, "%s", label);
    ImGui::Dummy(ImVec2(0, em(0.1F)));
}

/// A page title.
void title(const char* label) {
    ImGui::PushFont(theme::heading());
    text_coloured(theme::kFlame, "%s", label);
    ImGui::PopFont();
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

/// A path trimmed from the left, so the end -- which is the part that says
/// which project this is -- survives.
std::string tail_of(const std::filesystem::path& path, std::size_t width) {
    const std::string text = path.string();
    if (text.size() <= width) {
        return text;
    }
    return "..." + text.substr(text.size() - width);
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

ImU32 step_colour(const CookStep& step) {
    if (!step.ok) {
        return theme::kError;
    }
    if (step.kind == "write") {
        return theme::kAdded;
    }
    if (step.kind == "run" || step.kind == "done" || step.kind == "handoff") {
        return theme::kFlame;
    }
    return theme::kTextFaint;
}

/// Subdirectories of `dir`, sorted, with hidden ones left out.
std::vector<std::filesystem::path> subdirectories(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;  // a project picker full of .cache and .git helps nobody
        }
        found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

App::App(Config config, std::vector<std::string> warnings)
    : config_(std::move(config)),
      store_(std::make_unique<SessionStore>(Project::current())),
      trust_(paths::trust_file()) {
    for (std::string& warning : warnings) {
        notices_.push_back(std::move(warning));
    }

    state_.configure_seats(config_);
    state_.set_project_usage(store_->project_usage());

    engine_ = std::make_unique<Engine>(config_, state_, [this] {
        // The engine runs on its own thread and the window may be parked in
        // glfwWaitEvents. Without this the screen would not repaint until the
        // mouse moved, which during a model load is most of a minute.
        glfwPostEmptyEvent();
    });
    engine_->set_journal_dir(store_->project().dir);

    remember_project(store_->project().root);
    browse_      = store_->project().root;
    browse_text_ = browse_.string();
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
    if (store_->save(snapshot.turns, snapshot.session_usage, error)) {
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

void App::open_project(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        project_error_ = root.string() + " is not a directory";
        return;
    }
    if (engine_->cooking()) {
        // A cook is about the directory it started in, and its journal is keyed
        // to it. Moving the ground under it would produce a record of work done
        // somewhere it was not.
        say("finish or stop the cook before opening another project");
        return;
    }

    // The same store the terminal program asks on first use in a directory. A
    // folder trusted in one face is trusted in the other, because it is one
    // decision about one directory.
    if (!trust_.is_trusted(root)) {
        pending_trust_ = root;
        return;
    }

    const Project project = Project::at(root);
    persist_session();

    store_ = std::make_unique<SessionStore>(project);
    engine_->set_journal_dir(project.dir);
    engine_->reset_history();
    state_.clear_turns();
    state_.clear_notices();
    state_.set_cook(nullptr);
    state_.set_project_usage(store_->project_usage());
    persisted_turns_ = 0;
    expanded_.clear();
    notices_.clear();
    follow_      = true;
    browse_      = project.root;
    browse_text_ = browse_.string();

    remember_project(project.root);
    project_error_.clear();
    say("opened " + project.root.string());
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
        view_          = View::Settings;
        settings_page_ = SettingsPage::Tools;
        return;
    }
    cook_goal_.clear();
    follow_ = true;
    view_   = View::Cook;
    expanded_.clear();
    engine_->start_cook(goal, cook_untimed_ ? 0 : cook_minutes_ * 60,
                        store_->project().root);
}

// ---------------------------------------------------------------------------
// The sidebar
// ---------------------------------------------------------------------------

void App::draw_sidebar(const Snapshot& snapshot) {
    if (!sidebar_open_) {
        // Collapsed: a strip wide enough for the mark and the button that
        // brings it back. Hiding it entirely would leave nothing to click.
        // Wide enough for the button's own label plus its frame padding. At
        // em(2.8) the arrow was clipped to an empty box, which is a button
        // nobody would think to press.
        ImGui::BeginChild("rail", ImVec2(em(3.6F), 0), ImGuiChildFlags_Borders);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        theme::draw_flame(ImGui::GetWindowDrawList(),
                          ImVec2(origin.x + em(1.0F), origin.y + em(0.9F)), em(0.8F));
        ImGui::Dummy(ImVec2(0, em(2.2F)));
        if (ImGui::Button(">", ImVec2(-FLT_MIN, 0))) {
            sidebar_open_ = true;
        }
        ImGui::SetItemTooltip("Show the sidebar");
        ImGui::EndChild();
        return;
    }

    ImGui::BeginChild("sidebar", ImVec2(sidebar_width_, 0), ImGuiChildFlags_Borders);

    // --- the mark ---------------------------------------------------------
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        theme::draw_flame(ImGui::GetWindowDrawList(),
                          ImVec2(origin.x + em(0.9F), origin.y + em(1.0F)), em(0.95F));

        ImGui::SetCursorScreenPos(ImVec2(origin.x + em(2.4F), origin.y + em(0.15F)));
        ImGui::PushFont(theme::bold());
        text_coloured(theme::kText, "CRUCIBLE");
        ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + em(2.4F), origin.y + em(1.25F)));
        text_coloured(snapshot.busy ? theme::kFlameBright : theme::kTextDim, "%s",
                      snapshot.status.empty() ? mood_text(snapshot.mood)
                                              : snapshot.status.c_str());

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + em(2.6F)));
    }
    ImGui::Separator();

    // --- the project ------------------------------------------------------
    //
    // At the top, because it is what everything else is about, and it is the
    // one question a window cannot answer for itself the way `cd` does.
    section("PROJECT");
    ImGui::PushFont(theme::bold());
    text_coloured(theme::kText, "%s", store_->project().name.c_str());
    ImGui::PopFont();
    text_coloured(theme::kTextFaint, "%s", tail_of(store_->project().root, 34).c_str());
    ImGui::SetItemTooltip("%s", store_->project().root.string().c_str());

    if (ImGui::Button("Open project", ImVec2(-FLT_MIN, 0))) {
        project_modal_open_ = true;
        browse_             = store_->project().root;
        browse_text_        = browse_.string();
        new_folder_.clear();
        project_error_.clear();
    }

    const std::vector<Project> recents = recent_projects(6);
    bool any_other = false;
    for (const Project& project : recents) {
        any_other = any_other || project.root != store_->project().root;
    }
    if (any_other) {
        section("RECENT");
        for (const Project& project : recents) {
            if (project.root == store_->project().root) {
                continue;
            }
            ImGui::PushID(project.root.c_str());
            if (ImGui::Selectable(project.name.c_str(), false, 0, ImVec2(0, em(1.3F)))) {
                open_project(project.root);
            }
            ImGui::SetItemTooltip("%s", project.root.string().c_str());
            ImGui::PopID();
        }
    }

    // --- the roundtable ---------------------------------------------------
    section("ROUNDTABLE");
    const Roster& roster = snapshot.roster ? *snapshot.roster : config_.roster;
    if (roster.experts().empty()) {
        wrapped(theme::kTextFaint, "No experts yet. Add one in Settings.");
    }
    for (std::size_t i = 0; i < roster.size(); ++i) {
        const Expert&    expert = roster.at(i);
        const SeatState& seat   = i < snapshot.seats.size() ? snapshot.seats[i] : SeatState{};
        const bool linked = snapshot.linked && *snapshot.linked == expert.id;

        const ImVec2 at = ImGui::GetCursorScreenPos();
        theme::draw_dot(ImGui::GetWindowDrawList(),
                        ImVec2(at.x + em(0.42F), at.y + em(0.55F)), em(0.30F),
                        dot_for(seat.phase));
        ImGui::Dummy(ImVec2(em(1.05F), em(1.1F)));
        ImGui::SameLine();

        std::string label = expert.name;
        if (seat.phase == SeatPhase::Loading) {
            label += "  " + std::to_string(static_cast<int>(seat.progress * 100.0F)) + "%";
        }
        const ImU32 colour = linked                                ? theme::kFlameBright
                           : seat.phase == SeatPhase::Unconfigured ? theme::kTextFaint
                                                                   : theme::kText;
        if (linked) {
            ImGui::PushFont(theme::bold());
        }
        text_coloured(colour, "%s", label.c_str());
        if (linked) {
            ImGui::PopFont();
        }
        if (ImGui::IsItemHovered() && !expert.blurb.empty()) {
            ImGui::SetTooltip("%s", expert.blurb.c_str());
        }
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
    nav("History",  View::History);
    nav("Settings", View::Settings);

    // --- the footer --------------------------------------------------------
    //
    // Pushed to the bottom by a filler rather than by an absolute cursor
    // position: the sidebar scrolls when the roster is long, and a footer
    // pinned to the window height ends up either overlapping the list or below
    // the visible area depending on how far it has scrolled.
    const TokenUsage& usage = snapshot.session_usage;
    const float footer = em(4.1F);
    const float slack  = ImGui::GetContentRegionAvail().y - footer;
    if (slack > 0.0F) {
        ImGui::Dummy(ImVec2(0, slack));
    }
    ImGui::Separator();
    text_coloured(theme::kTextFaint, "%s in / %s out",
                  format_tokens(usage.input_tokens).c_str(),
                  format_tokens(usage.output_tokens).c_str());
    if (ImGui::Button("<< Hide", ImVec2(-FLT_MIN, 0))) {
        sidebar_open_ = false;
    }

    ImGui::EndChild();
}

void App::draw_splitter() {
    ImGui::SameLine(0.0F, 0.0F);
    if (!sidebar_open_) {
        return;
    }

    // An invisible button dragged sideways. ImGui has no splitter widget, and
    // this is what one is: a thing that is hovered, held, and reports how far
    // the mouse moved while it was held.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::to_vec(theme::kFlame));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::to_vec(theme::kFlameBright));
    ImGui::Button("##splitter", ImVec2(em(0.35F), -FLT_MIN));
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
        sidebar_width_ += ImGui::GetIO().MouseDelta.x;
    }
    // Clamped so it can be neither squeezed into nothing nor dragged over the
    // whole window -- either of which leaves no way back without a mouse hunt.
    const float most = std::max(em(14.0F), ImGui::GetWindowWidth() * 0.45F);
    sidebar_width_ = std::clamp(sidebar_width_, em(12.0F), most);

    ImGui::SameLine(0.0F, 0.0F);
}

// ---------------------------------------------------------------------------
// Chat
// ---------------------------------------------------------------------------

void App::draw_chat(const Snapshot& snapshot) {
    for (const std::string& notice : notices_) {
        wrapped(theme::kTextDim, "- " + notice);
    }
    if (!notices_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.5F)));
    }

    if (snapshot.turns.empty()) {
        ImGui::Dummy(ImVec2(0, em(1.5F)));
        wrapped(theme::kTextDim, config_.configured_experts().empty()
            ? "No expert models are assigned yet. Open Settings and point one at a "
              "GGUF file."
            : "Ask anything and Crucible picks the expert. Or give it a goal and let "
              "it cook.");
    }

    for (const Turn& turn : snapshot.turns) {
        ImGui::PushFont(theme::bold());
        text_coloured(theme::kFlame, "you");
        ImGui::PopFont();
        wrapped(theme::kText, turn.prompt);
        ImGui::Dummy(ImVec2(0, em(0.3F)));

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
            ImGui::Dummy(ImVec2(0, em(0.3F)));
        }
        for (const std::string& search : turn.searches) {
            text_coloured(theme::kTextDim, "  searched: %s", search.c_str());
        }

        // Rendered, not printed. Every instruction-tuned model answers in
        // markdown whether or not you ask it to, and shown raw that is a wall
        // of asterisks with the structure left for the reader to reconstruct.
        draw_markdown(turn.reply, turn.failed ? theme::kError : theme::kText);

        if (turn.tokens_per_second > 0.0) {
            text_coloured(theme::kTextFaint, "%s tok/s  ·  %d tokens",
                          format::number(turn.tokens_per_second, 1).c_str(),
                          turn.output_tokens);
        }
        ImGui::Dummy(ImVec2(0, em(0.6F)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em(0.4F)));
    }

    if (snapshot.cook) {
        draw_cook(snapshot);
    }
}

// ---------------------------------------------------------------------------
// Cook
// ---------------------------------------------------------------------------

void App::draw_cook_step(const CookStep& step, std::size_t index) {
    if (expanded_.size() <= index) {
        expanded_.resize(index + 1, false);
    }

    ImGui::PushID(static_cast<int>(index));

    // The verb in its own column so a hundred steps read as a list rather than
    // as a paragraph.
    std::string verb = step.kind;
    verb.resize(8, ' ');
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(step_colour(step)));
    ImGui::TextUnformatted(verb.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(em(5.2F));

    if (step.detail.empty()) {
        wrapped(step.ok ? theme::kText : theme::kError, step.summary);
    } else {
        // Clickable, because the detail is a diff or a page of build output,
        // and showing every one by default turns the journal into the log.
        const bool open = expanded_[index];
        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::to_vec(step.ok ? theme::kText : theme::kError));
        if (ImGui::Selectable((std::string(open ? "v  " : ">  ") + step.summary).c_str(),
                              open)) {
            expanded_[index] = !open;
        }
        ImGui::PopStyleColor();
        if (expanded_[index]) {
            draw_code_block(step.detail);
        }
    }
    ImGui::PopID();
}

void App::draw_cook(const Snapshot& snapshot) {
    if (!snapshot.cook) {
        ImGui::Dummy(ImVec2(0, em(1.5F)));
        wrapped(theme::kTextDim,
                "Nothing is cooking. Give Crucible a goal below and a time to work on "
                "it, and it will read the project, change it, run it, and keep going.");
        return;
    }
    const Cook& cook = *snapshot.cook;

    ImGui::PushFont(theme::bold());
    text_coloured(theme::kFlame, "COOK");
    ImGui::PopFont();
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

    // Who has worked on it. A cook is not one expert any more: a HANDOFF sends
    // the next piece of work back through the delegator, so a long one may pass
    // from a programming expert to a writing one and back.
    const std::vector<ExpertId> experts = cook.experts_used();
    if (experts.size() > 1) {
        std::string names;
        for (std::size_t i = 0; i < experts.size(); ++i) {
            names += (i == 0 ? "" : "  ->  ") + expert_label(config_.roster, experts[i]);
        }
        text_coloured(theme::kFlameBright, "%s", names.c_str());
    }

    if (cook.budget_seconds > 0) {
        const float done = std::clamp(
            static_cast<float>(cook.duration().count())
                / static_cast<float>(cook.budget_seconds), 0.0F, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::to_vec(theme::kFlame));
        ImGui::ProgressBar(done, ImVec2(-FLT_MIN, em(0.25F)), "");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    for (std::size_t i = 0; i < cook.steps.size(); ++i) {
        draw_cook_step(cook.steps[i], i);
    }

    if (cook.state == CookState::Asking && !cook.question.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.6F)));
        ImGui::PushFont(theme::bold());
        text_coloured(theme::kFlameBright, "It is asking:");
        ImGui::PopFont();
        wrapped(theme::kText, cook.question);
        text_coloured(theme::kTextFaint, "type an answer below and press enter");
    }

    if (!cook.outcome.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.6F)));
        ImGui::Separator();
        draw_markdown(cook.outcome, theme::kTextDim);
    }

    const std::vector<std::string> files = cook.files_touched();
    const bool finished = cook.state == CookState::Done || cook.state == CookState::Stopped
                       || cook.state == CookState::Failed;
    ImGui::Dummy(ImVec2(0, em(0.4F)));
    if (!files.empty()) {
        std::string list;
        for (std::size_t i = 0; i < files.size(); ++i) {
            list += (i == 0 ? "" : ", ") + files[i];
        }
        text_coloured(theme::kTextFaint, "changed");
        wrapped(theme::kAdded, list);
    } else if (finished) {
        // The outcome above is the expert's account of itself; this is the
        // fact. A model that talked its way through an edit it never made
        // writes a confident summary of having made it, and the only thing that
        // catches that is the journal saying nothing was written.
        text_coloured(theme::kError,
                      "changed no files -- whatever it says, nothing on disk moved");
    }
}

// ---------------------------------------------------------------------------
// Experts
// ---------------------------------------------------------------------------

void App::draw_expert_list() {
    title("Experts");
    wrapped(theme::kTextDim,
            "The delegator routes each prompt to one of these. Add your own with a "
            "name and a description of what it handles; everything else is worked "
            "out for you.");
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    if (ImGui::Button("+ New expert")) {
        expert_modal_open_ = true;
        new_expert_name_.clear();
        new_expert_blurb_.clear();
        expert_error_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan models")) {
        refresh_models();
        say("found " + std::to_string(models_.size()) + " GGUF files");
    }
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    if (config_.roster.experts().empty()) {
        wrapped(theme::kTextDim,
                "The roundtable is empty. Nothing can answer until there is an expert "
                "on it.");
    }

    std::optional<ExpertId> eject;
    for (const Expert& expert : config_.roster.experts()) {
        ImGui::PushID(expert.id.c_str());
        ImGui::Separator();

        ImGui::PushFont(theme::bold());
        text_coloured(theme::kText, "%s", expert.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        text_coloured(theme::kTextFaint, "[%s]", expert.tag.c_str());
        if (config_.routing.default_expert == expert.id) {
            ImGui::SameLine();
            text_coloured(theme::kFlame, "default");
        }
        wrapped(theme::kTextDim, expert.blurb);

        // The model assignment. A combo rather than a text box: the models
        // directory is the list of valid answers, and typing a file name is how
        // you get a seat that points at nothing.
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
                if (ImGui::Selectable((file.name + "   " + file.size_label()).c_str(),
                                      file.name == params.model)) {
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
        ImGui::Dummy(ImVec2(0, em(0.3F)));
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
            if (config.routing.default_expert == *eject) {
                config.routing.default_expert.clear();
            }
        });
        say(name + " has left the roundtable");
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void App::draw_settings() {
    // One list down the left and one page on the right. A single scrolling wall
    // of switches is what every desktop application starts with and none of
    // them keeps.
    ImGui::BeginChild("settings-nav", ImVec2(em(10.0F), 0), ImGuiChildFlags_Borders);
    const auto page = [this](const char* label, SettingsPage which) {
        if (ImGui::Selectable(label, settings_page_ == which, 0, ImVec2(0, em(1.5F)))) {
            settings_page_ = which;
        }
    };
    ImGui::Dummy(ImVec2(0, em(0.2F)));
    page("General",  SettingsPage::General);
    page("Experts",  SettingsPage::Experts);
    page("Hardware", SettingsPage::Hardware);
    page("Tools",    SettingsPage::Tools);
    page("About",    SettingsPage::About);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("settings-page", ImVec2(0, 0), ImGuiChildFlags_Borders);

    switch (settings_page_) {
        case SettingsPage::General: {
            title("General");

            section("DELEGATOR");
            wrapped(theme::kTextDim,
                    "A small model that reads your prompt and names the expert it "
                    "belongs to. It never answers; it only decides.");
            ImGui::SetNextItemWidth(em(20.0F));
            const bool open = ImGui::BeginCombo("Router model",
                                                model_label(config_.router.model).c_str());
            if (!config_.router.model.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", config_.router.path.c_str());
            }
            if (open) {
                if (ImGui::Selectable("(none) -- route on keywords instead",
                                      config_.router.model.empty())) {
                    update_config([](Config& config) { config.router.model.clear(); });
                }
                for (const ModelFile& file : models_) {
                    if (ImGui::Selectable((file.name + "   " + file.size_label()).c_str(),
                                          file.name == config_.router.model)) {
                        update_config([&file](Config& config) {
                            config.router.model = file.name;
                        });
                    }
                }
                ImGui::EndCombo();
            }

            bool keep = config_.routing.keep_delegator_loaded;
            if (ImGui::Checkbox("Keep the delegator in memory between prompts", &keep)) {
                update_config([keep](Config& config) {
                    config.routing.keep_delegator_loaded = keep;
                });
            }
            ImGui::SetItemTooltip(
                "Off frees it after each decision, leaving the expert the whole card.");

            float floor_value = config_.routing.min_confidence;
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderFloat("Confidence floor", &floor_value, 0.0F, 1.0F, "%.2f")) {
                update_config([floor_value](Config& config) {
                    config.routing.min_confidence = floor_value;
                });
            }
            ImGui::SetItemTooltip(
                "Below this the delegator is treated as undecided. 0 disables the check.");

            section("MODELS");
            text_coloured(theme::kTextDim, "%s",
                          config_.resolved_models_dir().string().c_str());
            std::string dir = config_.models_dir;
            ImGui::SetNextItemWidth(-em(6.5F));
            if (ImGui::InputTextWithHint("##models-dir", "path to your GGUF files", &dir,
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                update_config([&dir](Config& config) { config.models_dir = dir; });
                refresh_models();
            }
            ImGui::SameLine();
            if (ImGui::Button("Rescan", ImVec2(-FLT_MIN, 0))) {
                refresh_models();
                say("found " + std::to_string(models_.size()) + " GGUF files");
            }

            section("APPEARANCE");
            bool reasoning = config_.ui.show_reasoning;
            if (ImGui::Checkbox("Keep a thinking model's working on screen", &reasoning)) {
                update_config([reasoning](Config& config) {
                    config.ui.show_reasoning = reasoning;
                });
            }
            break;
        }

        case SettingsPage::Experts: {
            draw_expert_list();
            ImGui::Dummy(ImVec2(0, em(0.6F)));
            ImGui::Separator();
            section("DEFAULT EXPERT");
            wrapped(theme::kTextDim,
                    "Takes prompts the delegator could not place, and prompts routed to "
                    "a seat with no model. Any expert can play this part; without one, "
                    "an uncertain route is taken at face value.");
            const std::string current = config_.routing.default_expert.empty()
                ? std::string("(none)")
                : expert_label(config_.roster, config_.routing.default_expert);
            ImGui::SetNextItemWidth(em(20.0F));
            if (ImGui::BeginCombo("##default-expert", current.c_str())) {
                if (ImGui::Selectable("(none)", config_.routing.default_expert.empty())) {
                    update_config([](Config& config) {
                        config.routing.default_expert.clear();
                    });
                }
                for (const Expert& expert : config_.roster.experts()) {
                    if (ImGui::Selectable(expert.name.c_str(),
                                          config_.routing.default_expert == expert.id)) {
                        update_config([&expert](Config& config) {
                            config.routing.default_expert = expert.id;
                        });
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }

        case SettingsPage::Hardware: {
            title("Hardware");

            section("DEVICES");
            const std::vector<ComputeDevice> devices = compute_devices();
            if (devices.empty()) {
                wrapped(theme::kTextDim,
                        "No compute devices -- no runtime is installed. Install one from "
                        "the terminal app with /runtimes: it compiles a GPU backend for "
                        "this machine, which takes minutes and wants a log rather than a "
                        "progress bar.");
            }
            for (const ComputeDevice& device : devices) {
                text_coloured(theme::kTextDim, "[%d] %s  %s", device.index,
                              device.label().c_str(), device.backend.c_str());
            }

            section("SPLIT");
            bool gpu_only = config_.gpu.gpu_only;
            if (ImGui::Checkbox("Keep every layer on the GPU", &gpu_only)) {
                update_config([gpu_only](Config& config) {
                    config.gpu.gpu_only = gpu_only;
                });
            }
            ImGui::SetItemTooltip(
                "A model 90%% offloaded runs at roughly the speed of one not offloaded "
                "at all.");
            bool vram_only = config_.gpu.vram_only;
            if (ImGui::Checkbox("Refuse a model that will not fit in VRAM", &vram_only)) {
                update_config([vram_only](Config& config) {
                    config.gpu.vram_only = vram_only;
                });
            }
            ImGui::SetItemTooltip(
                "Otherwise the driver spills into system RAM and the model runs about "
                "twenty times slower with nothing on screen to say why.");

            int context = config_.defaults.n_ctx;
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::InputInt("Context size", &context, 1024, 4096)) {
                context = std::clamp(context, 512, 1 << 20);
                update_config([context](Config& config) {
                    config.defaults.n_ctx = context;
                });
            }
            break;
        }

        case SettingsPage::Tools: {
            title("Tools");

            section("WORKSHOP");
            wrapped(theme::kTextDim,
                    "What a cook is allowed to do to this project. Off, Crucible only "
                    "answers questions about it. Every path an expert names is resolved "
                    "inside the project folder and anything that escapes it is refused.");
            text_coloured(theme::kFlame, "%s", store_->project().root.string().c_str());

            bool workshop = config_.tools.workshop;
            if (ImGui::Checkbox("Let experts read and write files here", &workshop)) {
                update_config([workshop](Config& config) {
                    config.tools.workshop = workshop;
                });
            }
            bool allow_run = config_.tools.workshop_run;
            if (ImGui::Checkbox("Let them run commands too", &allow_run)) {
                update_config([allow_run](Config& config) {
                    config.tools.workshop_run = allow_run;
                });
            }
            ImGui::SetItemTooltip(
                "A separate decision: editing a project you trusted and executing "
                "arbitrary commands in it are not the same thing.");

            int timeout = config_.tools.workshop_timeout;
            ImGui::SetNextItemWidth(em(12.0F));
            if (ImGui::SliderInt("Command timeout (s)", &timeout, 5, 600)) {
                update_config([timeout](Config& config) {
                    config.tools.workshop_timeout = timeout;
                });
            }

            section("WEB SEARCH");
            bool web = config_.tools.web_search;
            if (ImGui::Checkbox("Let experts look things up", &web)) {
                update_config([web](Config& config) { config.tools.web_search = web; });
            }
            ImGui::SetItemTooltip("The only thing Crucible sends off this machine.");
            if (config_.tools.web_search) {
                ImGui::SetNextItemWidth(em(14.0F));
                if (ImGui::BeginCombo("Provider", config_.tools.search_provider.c_str())) {
                    for (const char* which : {"wikipedia", "searxng", "brave"}) {
                        if (ImGui::Selectable(which,
                                              config_.tools.search_provider == which)) {
                            update_config([which](Config& config) {
                                config.tools.search_provider = which;
                            });
                        }
                    }
                    ImGui::EndCombo();
                }
                std::string endpoint = config_.tools.search_endpoint;
                ImGui::SetNextItemWidth(em(22.0F));
                if (ImGui::InputTextWithHint("Endpoint", "http://localhost:8888", &endpoint,
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                    update_config([&endpoint](Config& config) {
                        config.tools.search_endpoint = endpoint;
                    });
                }
            }
            break;
        }

        case SettingsPage::About: {
            title("Crucible " CRUCIBLE_VERSION);
            wrapped(theme::kTextDim,
                    "A local forge: experts on demand, projects that cook. This window "
                    "and the terminal program are the same engine -- same roster, same "
                    "cook loop, same config file.");

            section("FILES");
            text_coloured(theme::kTextDim, "config    %s",
                          paths::config_file().string().c_str());
            text_coloured(theme::kTextDim, "models    %s",
                          config_.resolved_models_dir().string().c_str());
            text_coloured(theme::kTextDim, "runtimes  %s",
                          paths::runtimes_dir().string().c_str());
            text_coloured(theme::kTextDim, "history   %s",
                          store_->project().dir.string().c_str());
            text_coloured(theme::kTextDim, "log       %s",
                          paths::log_file().string().c_str());

            section("TRUSTED FOLDERS");
            wrapped(theme::kTextDim,
                    "Crucible asks once per directory before it will read or write "
                    "there. These are the ones you have said yes to.");
            for (const std::filesystem::path& entry : trust_.entries()) {
                text_coloured(theme::kTextFaint, "%s", entry.string().c_str());
            }
            break;
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

void App::draw_history() {
    title("History");
    wrapped(theme::kTextDim, "Everything Crucible has done in this project.");
    ImGui::Dummy(ImVec2(0, em(0.6F)));

    section("COOKS");
    const CookLog log(store_->project().dir);
    const std::vector<CookSummary> cooks = log.list();
    if (cooks.empty()) {
        wrapped(theme::kTextDim, "No cooks yet.");
    }
    for (const CookSummary& cook : cooks) {
        ImGui::Separator();
        wrapped(theme::kText, cook.goal);
        text_coloured(theme::kTextDim, "%s  ·  %d %s  ·  %d steps  ·  %s  ·  %s",
                      cook.when().c_str(), cook.files,
                      cook.files == 1 ? "file" : "files", cook.steps,
                      format_duration(cook.duration).c_str(),
                      std::string(cook_state_name(cook.state)).c_str());
    }

    ImGui::Dummy(ImVec2(0, em(0.8F)));
    section("CONVERSATIONS");
    const std::vector<SessionSummary> sessions = store_->list();
    if (sessions.empty()) {
        wrapped(theme::kTextDim, "No conversations yet.");
    }
    for (const SessionSummary& session : sessions) {
        ImGui::Separator();
        wrapped(theme::kText, session.title);
        text_coloured(theme::kTextDim, "%s  ·  %d turns", session.when().c_str(),
                      session.turns);
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

    const char* hint = asking  ? "answer the question above"
                     : cooking ? "cooking -- ask anyway and it waits its turn"
                               : "ask anything";
    ImGui::SetNextItemWidth(-em(4.8F));
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

    if (cooking) {
        ImGui::PushFont(theme::bold());
        text_coloured(theme::kFlameBright, "cooking");
        ImGui::PopFont();
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
        ImGui::SetNextItemWidth(-em(21.5F));
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
// Dialogs
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
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    text_coloured(theme::kTextFaint, "Expert name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", "Rust Async, Tax Law, Kubernetes", &new_expert_name_);

    ImGui::Dummy(ImVec2(0, em(0.4F)));
    text_coloured(theme::kTextFaint, "Describe what the expert is trained in");
    ImGui::InputTextMultiline("##blurb", &new_expert_blurb_, ImVec2(-FLT_MIN, em(5.2F)));
    text_coloured(theme::kTextFaint,
                  "The delegator routes on this, so name the things it should take.");

    if (!expert_error_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.4F)));
        wrapped(theme::kError, expert_error_);
    }

    ImGui::Dummy(ImVec2(0, em(0.6F)));
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

void App::draw_project_modal() {
    if (project_modal_open_) {
        ImGui::OpenPopup("Open project");
        project_modal_open_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(em(38.0F), em(30.0F)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Open project", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    // A browser rather than a native file dialog. Crucible has no toolkit to
    // ask for one, and a directory list is what this needs anyway: you are
    // choosing a folder to work in, not a file to load.
    ImGui::SetNextItemWidth(-em(9.0F));
    if (ImGui::InputText("##path", &browse_text_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::filesystem::path typed = paths::expand_user(browse_text_);
        std::error_code ec;
        if (std::filesystem::is_directory(typed, ec)) {
            browse_ = typed;
            project_error_.clear();
        } else {
            project_error_ = browse_text_ + " is not a directory";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Up", ImVec2(em(3.2F), 0)) && browse_.has_parent_path()) {
        browse_      = browse_.parent_path();
        browse_text_ = browse_.string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Home", ImVec2(-FLT_MIN, 0))) {
        browse_      = paths::expand_user("~");
        browse_text_ = browse_.string();
    }

    text_coloured(theme::kTextFaint, "%s", browse_.string().c_str());

    ImGui::BeginChild("dirs", ImVec2(0, em(15.0F)), ImGuiChildFlags_Borders);
    for (const std::filesystem::path& entry : subdirectories(browse_)) {
        ImGui::PushID(entry.c_str());
        if (ImGui::Selectable((entry.filename().string() + "/").c_str())) {
            browse_      = entry;
            browse_text_ = browse_.string();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Creating a folder here rather than sending someone to a file manager:
    // "start a new project" is the other half of "open one", and both are the
    // same question about the same directory.
    ImGui::SetNextItemWidth(-em(9.0F));
    ImGui::InputTextWithHint("##new-folder", "new folder name", &new_folder_);
    ImGui::SameLine();
    if (ImGui::Button("Create", ImVec2(-FLT_MIN, 0)) && !format::trim(new_folder_).empty()) {
        const std::filesystem::path made = browse_ / format::trim(new_folder_);
        std::error_code ec;
        std::filesystem::create_directories(made, ec);
        if (ec) {
            project_error_ = "could not create " + made.string() + ": " + ec.message();
        } else {
            browse_      = made;
            browse_text_ = made.string();
            new_folder_.clear();
            project_error_.clear();
        }
    }

    if (!project_error_.empty()) {
        wrapped(theme::kError, project_error_);
    }

    ImGui::Separator();
    if (ImGui::Button("Open this folder", ImVec2(em(12.0F), 0))) {
        const std::filesystem::path chosen = browse_;
        ImGui::CloseCurrentPopup();
        open_project(chosen);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        project_error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_trust_modal() {
    if (pending_trust_ && !ImGui::IsPopupOpen("Trust this folder?")) {
        ImGui::OpenPopup("Trust this folder?");
    }

    ImGui::SetNextWindowSize(ImVec2(em(32.0F), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Trust this folder?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!pending_trust_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    wrapped(theme::kText, pending_trust_->string());
    ImGui::Dummy(ImVec2(0, em(0.4F)));
    wrapped(theme::kTextDim,
            "Crucible will keep this folder's history, and -- if the workshop is on -- "
            "read, write and run things inside it while cooking. It never touches "
            "anything outside it.");
    ImGui::Dummy(ImVec2(0, em(0.6F)));

    if (ImGui::Button("Trust and open", ImVec2(em(11.0F), 0))) {
        const std::filesystem::path root = *pending_trust_;
        pending_trust_.reset();
        trust_.trust(root);
        ImGui::CloseCurrentPopup();
        open_project(root);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        pending_trust_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------

void App::draw() {
    const Snapshot snapshot = state_.snapshot();

    if (sidebar_width_ <= 0.0F) {
        sidebar_width_ = em(17.0F);
    }

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
    draw_splitter();

    ImGui::BeginChild("main", ImVec2(0, 0));
    {
        const float composer =
            view_ == View::Chat || view_ == View::Cook ? em(7.7F) : 0.0F;
        ImGui::BeginChild("pane", ImVec2(0, -composer), ImGuiChildFlags_Borders);
        switch (view_) {
            case View::Chat:     draw_chat(snapshot); break;
            case View::Cook:     draw_cook(snapshot); break;
            case View::Experts:  draw_expert_list();  break;
            case View::History:  draw_history();      break;
            case View::Settings: draw_settings();     break;
        }
        // Following the bottom, but only while the user is already there.
        // Yanking someone reading back through an hour-old cook to the end
        // every time a token arrives is the single most irritating thing a
        // streaming view can do.
        if (follow_ && (view_ == View::Chat || view_ == View::Cook)
            && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndChild();

        if (composer > 0.0F) {
            draw_composer(snapshot);
        }
    }
    ImGui::EndChild();

    draw_new_expert_modal();
    draw_project_modal();
    draw_trust_modal();
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
        std::fprintf(stderr, "crucible-gui: could not open a window. On Linux this "
                             "usually means there is no display, or no OpenGL driver.\n");
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

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        // Waiting rather than spinning. An idle Crucible should cost nothing,
        // and the engine posts an empty event whenever it has something new --
        // the timeout is only there so the cook clock keeps moving.
        glfwWaitEventsTimeout(state_.busy() ? 0.05 : 0.5);

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
