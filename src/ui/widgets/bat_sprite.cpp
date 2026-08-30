// SPDX-License-Identifier: MIT
//
// BatBot himself.
//
// The sprite is a fixed-width template with marker characters substituted at
// render time, so every frame is exactly the same size and the roundtable never
// reflows as he animates. Substitution is by marker rather than byte offset,
// which is what lets a multi-byte glyph stand in for a single-width slot.
#include "batbot/ui/widgets/bat_sprite.hpp"

#include <array>

#include "batbot/ui/theme.hpp"

namespace batbot::ui {
namespace {

// The sprite templates. Marker characters are substituted at render time:
//   F   -> forehead (the thinking star), display width 1
//   L,R -> eyes,                         display width 1
//   MMM -> mouth,                        display width 3
// Substitution is by marker, not byte offset, so multi-byte glyphs are fine as
// long as they occupy the same display width.
//
// Raw string literals keep the backslashes of the ears and feet readable.
constexpr std::array<const char*, 11> kFullBat{{
    R"(  /\           /\  )",
    R"( /  \_________/  \ )",
    R"(|   ___________   |)",
    R"(|  |     F     |  |)",
    R"(|  |  L     R  |  |)",
    R"(|  |    MMM    |  |)",
    R"(|  |___________|  |)",
    R"(|_________________|)",
    R"(       |   |       )",
    R"(      /|___|\      )",
    R"(        | |        )",
}};

// The compact bat is a head-only crop: at this size the body costs three rows
// and adds nothing, and the forehead needs its own line rather than sitting in
// the middle of the ear run, where a blank star leaves a visible gap.
constexpr std::array<const char*, 5> kCompactBat{{
    R"(  /\_______/\  )",
    R"( |     F     | )",
    R"( |  L     R  | )",
    R"( |    MMM    | )",
    R"( |___________| )",
}};

void replace_marker(std::string& line, std::string_view marker, const std::string& value) {
    if (const std::size_t pos = line.find(marker); pos != std::string::npos) {
        line.replace(pos, marker.size(), value);
    }
}

}  // namespace

BatSprite::BatSprite(bool unicode) : unicode_(unicode) {}

BatSprite::Face BatSprite::face_for(Mood mood, std::size_t tick) const {
    // Two glyph sets. The ASCII one is not a lesser fallback -- it is what most
    // terminals render most crisply at small sizes.
    const std::string open_eye   = unicode_ ? "\u25CF" : "o";  // ●
    const std::string shut_eye   = "-";
    const std::string dead_eye   = "x";
    const std::string smile      = unicode_ ? " \u203F " : "\\_/";  // ‿
    const std::string flat       = "___";

    Face face{" ", open_eye, open_eye, smile};

    switch (mood) {
        case Mood::Idle: {
            // A slow blink so an idle bat still looks alive: two frames shut
            // out of every twenty-four.
            const bool blinking = (tick % 24) >= 22;
            face.eye_left = face.eye_right = blinking ? shut_eye : open_eye;
            break;
        }
        case Mood::Routing: {
            // Concentrating, so: eyes shut and nothing moving. An earlier
            // version swept the eyes left and right, which read less as
            // thought than as a tic -- the thought bubble already says what
            // he is doing, and it says it without twitching.
            face.forehead = "*";
            face.eye_left = face.eye_right = shut_eye;
            face.mouth    = flat;
            break;
        }
        case Mood::Loading: {
            // The same still frame as Thinking. The forehead used to carry a
            // spinning |/-\ here, which is a lot of movement on a face for
            // something the thought bubble already says in words.
            face.forehead = "*";
            face.eye_left = face.eye_right = shut_eye;
            face.mouth    = flat;
            break;
        }
        case Mood::Thinking: {
            // Deliberately a still frame. Thinking is the longest thing
            // BatBot does, and a face that flickers for a minute is tiring to
            // sit next to; the star on the forehead is enough to say he is
            // working.
            face.forehead = "*";
            face.eye_left = face.eye_right = shut_eye;
            face.mouth    = flat;
            break;
        }
        case Mood::Talking: {
            // Eyes open and the same unhurried blink he has when idle, so the
            // change from thinking to talking is something you notice.
            const bool blinking = (tick % 24) >= 22;
            face.eye_left = face.eye_right = blinking ? shut_eye : open_eye;

            // A two-frame flap at a third of the old speed. The mouth is the
            // only thing that moves while he talks, which is what makes it
            // read as talking rather than as a face full of activity.
            static const std::array<std::string, 2> kMouths{{" o ", flat}};
            face.mouth = kMouths[(tick / 6) % kMouths.size()];
            break;
        }
        case Mood::Error: {
            face.eye_left = face.eye_right = dead_eye;
            face.mouth    = unicode_ ? " \u2040 " : " ~ ";
            break;
        }
    }

    return face;
}

std::vector<std::string> BatSprite::render(Mood mood, std::size_t tick, bool compact) const {
    const Face face = face_for(mood, tick);

    std::vector<std::string> lines;
    if (compact) {
        lines.assign(kCompactBat.begin(), kCompactBat.end());
    } else {
        lines.assign(kFullBat.begin(), kFullBat.end());
    }

    for (std::string& line : lines) {
        // MMM before the single-character markers: no overlap either way, but
        // longest-marker-first is the habit that keeps this correct if the
        // templates ever grow a two-character slot.
        replace_marker(line, "MMM", face.mouth);
        replace_marker(line, "F",   face.forehead);
        replace_marker(line, "L",   face.eye_left);
        replace_marker(line, "R",   face.eye_right);
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
    // Padded back out to a constant width. The bubble and the bat are centred
    // inside the same column, so a bubble that grows by a character shifts the
    // whole sprite half a column -- which reads as BatBot rocking side to side
    // once a second, and is far more distracting than the dots are useful.
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
        case Mood::Error:   return theme::kBatError;
        case Mood::Idle:    return theme::kBat;
        case Mood::Routing:
        case Mood::Loading:
        case Mood::Thinking:
        case Mood::Talking: return theme::kBatBusy;
    }
    return theme::kBat;
}

}  // namespace batbot::ui
