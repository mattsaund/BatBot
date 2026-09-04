// SPDX-License-Identifier: MIT
//
// The roundtable.
//
// Crucible on the left with a dot of his own, the experts in a column beside him
// with a dot each, and the fallback at the bottom of that column -- it is not
// one of the nine, and the bottom of the list is where the thing that catches
// what the others did not belongs.
//
// A line joins Crucible's dot to whichever seat the delegation chose, for as long
// as work is flowing to it. The dots say what is happening rather than what is
// in memory: a seat lights up when it is given the turn and goes dark when the
// answer is finished, and Crucible's own dot is lit exactly when the delegator is
// loaded and waiting -- which, with the delegator set to load on demand, is not
// most of the time.
#include "crucible/ui/widgets/roundtable.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace) -- DOM builders read far better unqualified

namespace crucible::ui {
namespace {

Color seat_color(SeatPhase phase) {
    switch (phase) {
        case SeatPhase::Active:       return theme::kSeatActive;
        case SeatPhase::Loading:      return theme::kSeatLoading;
        case SeatPhase::Dormant:      return theme::kSeatDormant;
        case SeatPhase::Missing:      return theme::kError;
        case SeatPhase::Unconfigured: break;
    }
    return theme::kSeatUnconfigured;
}

/// The marker to the left of a seat's name, which is how the ring reads at a
/// glance: filled means resident, hollow means on disk, a dot means no model.
std::string seat_marker(SeatPhase phase, std::size_t tick) {
    switch (phase) {
        case SeatPhase::Active:  return "◆";  // ◆ loaded and answering
        case SeatPhase::Dormant: return "◇";  // ◇ configured, on disk
        case SeatPhase::Loading: {
            static constexpr std::array<const char*, 4> kSpinner{{
                "◴", "◷", "◶", "◵"}};  // ◴◷◶◵
            return kSpinner[(tick / 2) % kSpinner.size()];
        }
        case SeatPhase::Missing:      return "✗";  // ✗ assigned, file not found
        case SeatPhase::Unconfigured: break;
    }
    return "·";  // · empty seat
}

/// One seat.
///
/// `narrow` drops the load percentage and the fixed width, for the one-line
/// strip where nine full-width chips would not fit in 80 columns.
Element seat(Subject subject, const SeatState& state, std::size_t tick,
             bool narrow = false) {
    const SubjectInfo& info = subject_info(subject);

    // The tag is padded to a fixed width for the one-line strip, where the
    // chips are laid end to end and the padding is what lines them up. In the
    // ring the box is already a fixed width and the label is centred in it, so
    // the padding only pushes the short tags half a column off centre.
    std::string_view tag = info.tag;
    if (!narrow) {
        while (!tag.empty() && tag.back() == ' ') {
            tag.remove_suffix(1);
        }
    }
    std::string label = seat_marker(state.phase, tick) + " " + std::string(tag);
    if (state.phase == SeatPhase::Loading && !narrow) {
        const int percent = static_cast<int>(state.progress * 100.0F);
        label += " " + std::to_string(percent) + "%";
    }

    Element chip = text(label) | color(seat_color(state.phase));
    if (state.phase == SeatPhase::Active) {
        chip = chip | bold;
    }
    if (state.phase == SeatPhase::Unconfigured) {
        chip = chip | dim;
    }
    if (narrow) {
        return chip;
    }
    // In the ring, every seat reserves room for the widest label
    // ("◴ MATH 100%") so the layout does not jitter while a model loads -- and
    // the label is centred in that room rather than left-aligned in it. A
    // six-column chip in an eleven-column box reads as three columns further
    // left than it is, which was enough to make the whole ring look adrift of
    // the crucible it is supposed to be arranged around.
    return chip | hcenter | size(WIDTH, EQUAL, 11);
}

/// Width of the gap the connector is drawn in, and where its vertical runs.
constexpr int kLinkWidth  = 8;
constexpr int kLinkColumn = 3;

/// One row of the connector between Crucible's dot and the chosen seat's.
///
/// An elbow: out from Crucible, down or up the column, then in to the seat. Drawn
/// a row at a time because that is how the rest of the panel is built, and the
/// three shapes it can take are the three cases below.
std::string link_row(int row, int from, int to) {
    // Box-drawing characters are multi-byte, so a row is assembled rather than
    // indexed.
    if (to < 0) {
        return std::string(static_cast<std::size_t>(kLinkWidth), ' ');  // nothing is running
    }
    const int lo = std::min(from, to);
    const int hi = std::max(from, to);

    std::string out;
    if (from == to && row == from) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += "─";
        }
        return out;
    }
    if (row == from) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += i < kLinkColumn ? "─" : (i == kLinkColumn ? (to > from ? "┐" : "┘") : " ");
        }
        return out;
    }
    if (row == to) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += i < kLinkColumn ? " " : (i == kLinkColumn ? (to > from ? "└" : "┌") : "─");
        }
        return out;
    }
    if (row > lo && row < hi) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += i == kLinkColumn ? "│" : " ";
        }
        return out;
    }
    return std::string(static_cast<std::size_t>(kLinkWidth), ' ');
}

}  // namespace

Element roundtable(const Snapshot& snapshot, const CrucibleSprite& sprite, std::size_t tick,
                   bool compact) {
    // Every subject in table order, with the fallback last -- it is not one of
    // the nine, and the bottom of the list is where the thing that catches what
    // the others did not belongs.
    std::vector<Subject> order = routable_subjects();
    order.push_back(Subject::Fallback);

    const auto row_of = [&order](Subject subject) {
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i] == subject) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    // Crucible sits level with the middle of the list, so the connector reaches
    // as far up as it does down.
    const int sprite_row = (static_cast<int>(order.size()) - 1) / 2;
    const int to_row  = snapshot.linked ? row_of(*snapshot.linked) : -1;

    // --- the seats, one per row ------------------------------------------
    Elements seat_rows;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Subject          subject = order[i];
        const SeatState&       state   = snapshot.seats[static_cast<std::size_t>(subject)];
        const SubjectInfo&     info    = subject_info(subject);
        const bool             lit     = to_row == static_cast<int>(i);

        std::string label = std::string(info.name);
        if (state.phase == SeatPhase::Loading) {
            label += " " + std::to_string(static_cast<int>(state.progress * 100.0F)) + "%";
        }

        Element name = text(label) | color(seat_color(state.phase));
        if (lit) {
            name = name | bold;
        }
        if (state.phase == SeatPhase::Unconfigured) {
            name = name | dim;
        }

        seat_rows.push_back(hbox({
            text(link_row(static_cast<int>(i), sprite_row, to_row))
                | color(to_row >= 0 ? theme::kSeatActive : theme::kMeta),
            text(seat_marker(state.phase, tick) + " ") | color(seat_color(state.phase)),
            std::move(name),
        }));
    }

    // --- Crucible, and his own dot -------------------------------------------
    //
    // The dot is lit when the delegator is loaded and waiting, which is exactly
    // when it can route the next prompt. With the delegator set to load on
    // demand it goes dark while an expert has the card, and that is the true
    // picture rather than a decoration.
    // The same diamond the seats use, and for the same reason: filled means
    // this one is doing something, hollow means it is there and waiting.
    const bool        ready = snapshot.delegator_ready && !snapshot.busy;
    const SeatPhase   phase = ready ? SeatPhase::Active : SeatPhase::Dormant;
    const std::string dot   = seat_marker(phase, tick);

    Elements sprite_lines;
    for (const std::string& line : sprite.render(snapshot.mood, tick, compact)) {
        sprite_lines.push_back(text(line) | color(mood_color(snapshot.mood)));
    }
    Element vessel = vbox(std::move(sprite_lines));

    // The label goes on the row directly above the dot, so the two read as one
    // thing rather than as a heading over the whole column.
    //
    // Both are pushed to the right of the column, which is what puts the dot
    // against the line: the connector leaves from the cell immediately after
    // it, so its horizontal run meets the diamond's right vertex rather than
    // starting somewhere out in the gap.
    Elements dot_column;
    for (int row = 0; row < static_cast<int>(order.size()); ++row) {
        if (row == sprite_row - 1) {
            dot_column.push_back(
                hbox({filler(), text("Crucible") | color(theme::kFlame) | bold}));
        } else if (row == sprite_row) {
            dot_column.push_back(
                hbox({filler(), text(dot) | color(seat_color(phase))}));
        } else {
            dot_column.push_back(text(" "));
        }
    }

    Element table = hbox({
        vbox({filler(), std::move(vessel), filler()}),
        text("  "),
        vbox(std::move(dot_column)) | size(WIDTH, EQUAL, 6),
        vbox(std::move(seat_rows)),
    });

    const std::string bubble = thought_bubble(snapshot.mood, snapshot.status, tick);
    if (compact || bubble.empty()) {
        return hbox({filler(), std::move(table), filler()});
    }
    return vbox({
        hbox({filler(), std::move(table), filler()}),
        text(bubble) | color(theme::kAccent) | hcenter,
    });
}

Element roundtable_strip(const Snapshot& snapshot, std::size_t tick) {
    Elements chips;
    for (const SubjectInfo& info : all_subjects()) {
        if (!chips.empty()) {
            chips.push_back(text("  "));
        }
        chips.push_back(seat(info.subject, snapshot.seats[static_cast<std::size_t>(info.subject)],
                             tick, /*narrow=*/true));
    }

    // Ten narrow chips come to about 76 columns, which leaves the mood very
    // little on an 80-column terminal -- and the seats are what the strip is
    // for. So the mood shrinks first: better a clipped mood than a clipped
    // roundtable.
    const std::string bubble = thought_bubble(snapshot.mood, snapshot.status, tick);
    chips.push_back(filler());
    chips.push_back(text(" "));  // never let the mood touch the last chip
    chips.push_back(text(bubble.empty() ? std::string("( idle )") : bubble)
                    | color(mood_color(snapshot.mood)) | flex_shrink);

    return hbox(std::move(chips));
}

}  // namespace crucible::ui
