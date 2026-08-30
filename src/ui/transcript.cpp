// SPDX-License-Identifier: MIT
//
// Drawing the conversation: one block per turn, the notices above it, and the
// first-run guidance when there is nothing to show yet.
//
// Every turn carries the route line -- which expert answered, how confident the
// delegator was, and what the swap cost -- because that is the part of BatBot
// worth watching.
#include "batbot/ui/app.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "batbot/config/paths.hpp"
#include "batbot/llm/model_catalog.hpp"
#include "batbot/ui/theme.hpp"
#include "batbot/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {

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
        line += " · " + format::number(route.confidence, 2);
        line += " · " + std::string(route_source_name(route.source));
        if (!route.detail.empty()) {
            line += " · " + route.detail;
        }
        if (turn.load_ms > 0) {
            line += " · swap " + format::duration_ms(turn.load_ms);
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
            stats += " · " + format::number(turn.tokens_per_second, 1) + " tok/s";
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
        // Just "ready". The seat count that used to be here read "10 of 9
        // expert seats are filled" once a custom subject was added, which is
        // both wrong and a number nobody was waiting for.
        return vbox({
            text("BatBot is ready.") | color(theme::kMeta),
            text("Ask anything; BatBot picks the expert. /help lists the commands.")
                | color(theme::kMeta) | dim,
            text(" "),
        });
    }

    // First run. Two lines: what is missing, and the one key that fixes it.
    // Everything else this used to say -- where models go, what size delegator
    // to pick -- is said again in settings, at the moment it is actually
    // needed, and saying it twice only made the first screen a wall of text.
    return vbox({
        text("No expert models are assigned yet.") | color(theme::kNotice) | bold,
        text("Press ctrl-e to open settings and assign one.") | color(theme::kMeta) | dim,
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

}  // namespace batbot::ui
