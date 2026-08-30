// SPDX-License-Identifier: MIT
//
// The roundtable.
//
// Ten seats arranged 3 / 2 / 2 / 3 around BatBot, in the order the subject
// table declares them. Seat width is fixed so the ring does not jitter as a
// model loads and its percentage changes width.
#include "batbot/ui/widgets/roundtable.hpp"

#include <array>
#include <string>

#include "batbot/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace) -- DOM builders read far better unqualified

namespace batbot::ui {
namespace {

/// Where each subject sits at the table.
///
/// Ten seats split 3 / 2 / 2 / 3 so the ring stays symmetric about BatBot:
/// three across the top, one pair level with his head, one pair level with his
/// feet, and three along the bottom -- the last of which is the catch-all.
constexpr std::array<Subject, 3> kTopRow{{
    Subject::Mathematics, Subject::Programming, Subject::Physics}};
constexpr std::array<Subject, 2> kLeftColumn{{
    Subject::Chemistry, Subject::Engineering}};
constexpr std::array<Subject, 2> kRightColumn{{
    Subject::Biology, Subject::Philosophy}};
constexpr std::array<Subject, 3> kBottomRow{{
    Subject::Sociology, Subject::Language, Subject::Fallback}};

/// Width of the whole ring: two 11-wide seat columns, the 19-wide bat, and
/// enough air between them to look deliberate.
constexpr int kRingWidth = 63;

// The bat's column is pinned to this, rather than sized to whatever is in it.
//
// The thought bubble is wider than the 19-column sprite, so without a fixed
// width it is the bubble that decides how wide the column is -- and the two
// fillers either side of it then split an odd number of leftover columns
// differently, sliding BatBot a column sideways every time the status text
// changes length. "Fallback is answering" and "swapping in Fallback" differ by
// one character, which was enough.
//
// 37 fits the longest status there is ("BatBot is reading the prompt", 35
// columns once bubbled) and still leaves the seat columns their 11 each inside
// kRingWidth. A longer one -- an exception message -- is truncated, which is
// the right trade: the transcript shows the whole thing anyway.
constexpr int kBatColumnWidth = 37;

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

    std::string label = seat_marker(state.phase, tick) + " " + std::string(info.tag);
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
    // ("◴ MATH 100%") so the layout does not jitter while a model loads.
    return chip | size(WIDTH, EQUAL, 11);
}

Element seat_row(const Snapshot& snapshot, const Subject* subjects, std::size_t count,
                 std::size_t tick) {
    Elements chips;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            chips.push_back(text("   "));
        }
        chips.push_back(seat(subjects[i], snapshot.seats[static_cast<std::size_t>(subjects[i])],
                             tick));
    }
    return hbox(std::move(chips)) | hcenter;
}

Element bat_column(const Snapshot& snapshot, const BatSprite& bat, std::size_t tick,
                   bool compact) {
    Elements lines;

    const std::string bubble = thought_bubble(snapshot.mood, snapshot.status, tick);
    if (!compact) {
        // Always reserve the bubble row, even when empty, so BatBot does not
        // hop up and down as he starts and stops thinking.
        lines.push_back(bubble.empty()
                            ? text(" ")
                            : text(bubble) | color(theme::kAccent) | hcenter);
    }

    for (const std::string& line : bat.render(snapshot.mood, tick, compact)) {
        lines.push_back(text(line) | color(mood_color(snapshot.mood)) | hcenter);
    }

    Element column = vbox(std::move(lines));
    if (!compact) {
        column = column | size(WIDTH, EQUAL, kBatColumnWidth);
    }
    return column;
}

}  // namespace

Element roundtable(const Snapshot& snapshot, const BatSprite& bat, std::size_t tick,
                   bool compact) {
    Element top    = seat_row(snapshot, kTopRow.data(), kTopRow.size(), tick);
    Element bottom = seat_row(snapshot, kBottomRow.data(), kBottomRow.size(), tick);

    // The side columns hug BatBot's head and feet, with a filler between so
    // they spread to whatever height the sprite happens to be.
    const auto side_column = [&](const Subject* subjects) {
        return vbox({
            seat(subjects[0], snapshot.seats[static_cast<std::size_t>(subjects[0])], tick),
            filler(),
            seat(subjects[1], snapshot.seats[static_cast<std::size_t>(subjects[1])], tick),
        });
    };

    Element middle = hbox({
        side_column(kLeftColumn.data()),
        filler(),
        bat_column(snapshot, bat, tick, compact),
        filler(),
        side_column(kRightColumn.data()),
    });

    // Pin the ring to a fixed width and centre it. Left to its own devices the
    // filler() between the columns spreads the side seats to the panel edges,
    // which stops reading as a table BatBot is sitting at.
    return vbox({
        top,
        middle | flex,
        bottom,
    }) | size(WIDTH, EQUAL, kRingWidth) | hcenter;
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

    // Nine narrow chips come to roughly 70 columns, so the mood still fits on
    // an 80-column terminal -- which is the whole reason the strip exists.
    const std::string bubble = thought_bubble(snapshot.mood, snapshot.status, tick);
    chips.push_back(filler());
    chips.push_back(text(" "));  // never let the mood touch the last chip
    chips.push_back(text(bubble.empty() ? std::string("( idle )") : bubble)
                    | color(mood_color(snapshot.mood)));

    return hbox(std::move(chips));
}

}  // namespace batbot::ui
