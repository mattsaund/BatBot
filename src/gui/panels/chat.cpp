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

/// The chat box: one prompt, nothing else.
///
/// Cooking has its own screen and its own controls. Putting both on one bar
/// meant every prompt was typed next to a Cook button that would have thrown
/// the prompt away, which is a question the user should not be asked to answer
/// on the way to sending a message.
void App::draw_chat_composer(const Snapshot& snapshot) {
    const std::shared_ptr<const Cook> cook = snapshot.cook;
    const bool asking  = cook && cook->state == CookState::Asking;
    const bool cooking = engine_->cooking();

    ImGui::BeginChild("chat-composer", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    const char* hint = asking  ? "answer the question on the cook screen"
                     : cooking ? "cooking -- ask anyway and it waits its turn"
                               : "ask anything";

    const float button = em(5.0F);
    const float width  = std::max(ImGui::GetContentRegionAvail().x - button
                                      - ImGui::GetStyle().ItemSpacing.x,
                                  em(6.0F));
    const bool entered = grow_input("##prompt", hint, prompt_, width, kComposerLines);
    ImGui::SameLine();
    const bool send = ImGui::Button(asking ? "Answer" : "Send", ImVec2(-FLT_MIN, 0));
    if (entered || send) {
        submit_prompt();
        // Enter should leave the caret where it was, or every reply costs a
        // click to get back to typing.
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::EndChild();
}

}  // namespace crucible::gui
