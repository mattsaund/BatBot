// SPDX-License-Identifier: MIT
//
// The flame.
//
// It is drawn as whole frames rather than as a template with markers punched
// into it. The sprite that came before this one was a crucible -- a pot, a
// stand, and a fire above them -- and only the fire ever moved, so substituting
// three fixed-width markers into a constant vessel was the cheap way to animate
// it. Now the whole mark is the fire, and what changes between moods is how
// much of the canvas it fills, which no amount of marker substitution expresses.
//
// Every frame is padded to the same width and height at render time, so the
// expert panel never reflows and the flame never slides sideways as it burns.
// The foot is the one part that does not move: fire grows upward from where it
// is lit.
//
// The silhouette is the one src/gui/theme.cpp draws and packaging/crucible.svg
// carries, at the resolution a character grid allows.
#include "crucible/ui/widgets/flame_sprite.hpp"

#include <array>
#include <string_view>

#include "crucible/ui/theme.hpp"

namespace crucible::ui {
namespace {

// --- the full flame, 13 columns by 7 rows ---------------------------------
//
// Two frames per mood. The flicker is a waver in one row rather than a jump in
// the whole shape: a mark that leaps about is one you cannot sit next to, and
// the model load these states cover can run for a minute.

using FullFrame = std::array<const char*, 7>;

// Full plume, inner tongue lit. The only state that reaches the top row.
constexpr std::array<FullFrame, 2> kFullTalking{{
    {{R"(      /\)",
      R"(     /  \)",
      R"(    (    ))",
      R"(   (  /\  ))",
      R"(   ( (  ) ))",
      R"(    \ \/ /)",
      R"(     \__/)"}},
    {{R"(      /\)",
      R"(     (  ))",
      R"(    (    ))",
      R"(   (  /\  ))",
      R"(   ( )  ( ))",
      R"(    \ \/ /)",
      R"(     \__/)"}},
}};

// Routing, loading, thinking: burning steadily, no tongue. Deliberately calm --
// the status bubble underneath says which of the three it is, in words rather
// than in glyphs nobody can tell apart.
constexpr std::array<FullFrame, 2> kFullWorking{{
    {{"",
      "",
      R"(      /\)",
      R"(     /  \)",
      R"(    (    ))",
      R"(    \    /)",
      R"(     \__/)"}},
    {{"",
      "",
      R"(      /\)",
      R"(     (  ))",
      R"(    (    ))",
      R"(    \    /)",
      R"(     \__/)"}},
}};

// Banked, not out. An ember flicks off the tip every second or so -- enough
// movement that an idle flame still looks lit, and little enough that it is not
// asking to be watched.
constexpr std::array<FullFrame, 2> kFullIdle{{
    {{"", "", "", "",
      R"(      /\)",
      R"(     /  \)",
      R"(     \__/)"}},
    {{"", "", "",
      R"(      ')",
      R"(      /\)",
      R"(     /  \)",
      R"(     \__/)"}},
}};

// Gone out: smoke drifting off a cold foot. The one state the sprite shows by
// taking something away rather than by adding movement.
constexpr std::array<FullFrame, 2> kFullError{{
    {{"", "",
      R"(      ~)",
      R"(     ~)",
      R"(      ~)",
      "",
      R"(     \__/)"}},
    {{"", "",
      R"(     ~)",
      R"(      ~)",
      R"(     ~)",
      "",
      R"(     \__/)"}},
}};

// --- the compact flame, 11 columns by 5 rows -------------------------------
//
// A crop in spirit rather than in fact: the same shape with the waist taken
// out, because at five rows there is no room for both a taper and a belly.

using CompactFrame = std::array<const char*, 5>;

constexpr std::array<CompactFrame, 2> kCompactTalking{{
    {{R"(    /\)",
      R"(   /  \)",
      R"(  ( /\ ))",
      R"(  ( \/ ))",
      R"(   \__/)"}},
    {{R"(    /\)",
      R"(   (  ))",
      R"(  ( /\ ))",
      R"(  ( \/ ))",
      R"(   \__/)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactWorking{{
    {{"",
      R"(    /\)",
      R"(   /  \)",
      R"(   (  ))",
      R"(   \__/)"}},
    {{"",
      R"(    /\)",
      R"(   (  ))",
      R"(   (  ))",
      R"(   \__/)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactIdle{{
    {{"", "",
      R"(    /\)",
      R"(   /  \)",
      R"(   \__/)"}},
    {{"",
      R"(    ')",
      R"(    /\)",
      R"(   /  \)",
      R"(   \__/)"}},
}};

constexpr std::array<CompactFrame, 2> kCompactError{{
    {{"",
      R"(    ~)",
      R"(   ~)",
      "",
      R"(   \__/)"}},
    {{"",
      R"(   ~)",
      R"(    ~)",
      "",
      R"(   \__/)"}},
}};

/// How fast a mood flickers, in frames per alternation.
///
/// Talking is three times the rate of the waiting states because it is the one
/// moment the machine is actually producing, and it should be visibly different
/// from the waiting that surrounds it.
std::size_t period(Mood mood) {
    switch (mood) {
        case Mood::Talking: return 3;
        case Mood::Idle:    return 14;
        case Mood::Error:   return 10;
        default:            return 7;
    }
}

/// The two frames for a mood, at whichever size is in play.
///
/// Returned as pointers into the tables above rather than copied: these are
/// constant for the life of the program and a sprite is re-rendered every frame.
const char* const* frame_for(Mood mood, std::size_t tick, bool compact) {
    const std::size_t which = (tick / period(mood)) % 2;
    if (compact) {
        switch (mood) {
            case Mood::Idle:    return kCompactIdle[which].data();
            case Mood::Talking: return kCompactTalking[which].data();
            case Mood::Error:   return kCompactError[which].data();
            default:            return kCompactWorking[which].data();
        }
    }
    switch (mood) {
        case Mood::Idle:    return kFullIdle[which].data();
        case Mood::Talking: return kFullTalking[which].data();
        case Mood::Error:   return kFullError[which].data();
        default:            return kFullWorking[which].data();
    }
}

}  // namespace

FlameSprite::FlameSprite(bool unicode) : unicode_(unicode) {}

std::vector<std::string> FlameSprite::render(Mood mood, std::size_t tick, bool compact) const {
    const char* const* frame = frame_for(mood, tick, compact);
    const std::size_t  rows  = static_cast<std::size_t>(height(compact));
    const std::size_t  cols  = static_cast<std::size_t>(width(compact));

    std::vector<std::string> lines;
    lines.reserve(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        std::string line = frame[r];

        // Padded to a constant width. FTXUI sizes a vbox to its widest child,
        // so ragged lines would make the panel breathe in and out by a column
        // as the flame changes height. Done before the substitution below,
        // while every line is still one byte per column.
        if (line.size() < cols) {
            line.append(cols - line.size(), ' ');
        }

        // Smoke is the one glyph with a rounder form worth having, and it is a
        // one-for-one swap so the padding above still holds. The flame itself
        // stays ASCII in both modes: slashes and parentheses are drawn by every
        // font there is, and a mark that comes out as boxes on a machine whose
        // font lacks the box-drawing diagonals is worse than one drawn plainly
        // everywhere.
        if (unicode_) {
            for (std::size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '~') {
                    line.replace(i, 1, "\u2248");
                    i += std::string_view("\u2248").size() - 1;
                }
            }
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string thought_bubble(Mood mood, const std::string& status, std::size_t tick) {
    if (mood == Mood::Idle && status.empty()) {
        return {};
    }

    std::string text = status.empty() ? std::string(mood_label(mood)) : status;

    // Trailing dots that march, so a long model load still looks alive even
    // when the percentage has not moved.
    //
    // Padded back out to a constant width. The bubble and the flame are centred
    // inside the same column, so a bubble that grows by a character shifts the
    // whole sprite half a column -- which reads as the mark rocking side to
    // side once a second, and is far more distracting than the dots are useful.
    if (mood != Mood::Idle && mood != Mood::Error) {
        constexpr std::size_t kMaxDots = 3;
        const std::size_t dots = (tick / 4) % (kMaxDots + 1);
        text += std::string(dots, '.');
        text += std::string(kMaxDots - dots, ' ');
    }
    return "( " + text + " )";
}

ftxui::Color meta_color(bool highlighted) {
    return highlighted ? ftxui::Color(theme::kMetaOnHighlight) : ftxui::Color(theme::kMeta);
}

ftxui::Color mood_color(Mood mood) {
    switch (mood) {
        case Mood::Error:   return theme::kFlameError;
        case Mood::Idle:    return theme::kFlame;
        case Mood::Routing:
        case Mood::Loading:
        case Mood::Thinking:
        case Mood::Talking: return theme::kFlameBusy;
    }
    return theme::kFlame;
}

}  // namespace crucible::ui
