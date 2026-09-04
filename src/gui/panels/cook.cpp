// SPDX-License-Identifier: MIT
//
// The cook view and the history of past cooks.
//
// A cook is a list of steps, each one a verb, a summary and the diff or output
// it produced. Steps are collapsed by default and expand in place: an hour of
// cooking is hundreds of them, and a wall of diffs is not a progress report.
//
// History is here because it reads the same journals -- a finished cook and a
// running one differ only in whether anything is still being appended.
#include "../app.hpp"

#include <imgui.h>
#include <algorithm>

#include "../markdown_view.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

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

}  // namespace crucible::gui
