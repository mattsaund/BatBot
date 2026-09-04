// SPDX-License-Identifier: MIT
//
// The chat transcript and the box you type into.
//
// One turn is a prompt, the route that was chosen for it, and the reply as it
// streams. The composer is here rather than with the frame because what it
// does depends on which view is showing: in Chat it sends a prompt, in Cook it
// starts or stops a cook.
#include "../app.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include "../markdown_view.hpp"
#include "crucible/util/format.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

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

}  // namespace crucible::gui
