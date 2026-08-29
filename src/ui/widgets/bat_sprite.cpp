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
            // Eyes sweep left and right: BatBot is reading the prompt.
            face.forehead = "*";
            const bool look_left = (tick / 3) % 2 == 0;
            face.eye_left  = look_left ? open_eye : shut_eye;
            face.eye_right = look_left ? shut_eye : open_eye;
            face.mouth     = flat;
            break;
        }
        case Mood::Loading: {
            // The forehead star doubles as the load spinner.
            static constexpr std::array<const char*, 4> kSpinner{{"|", "/", "-", "\\"}};
            face.forehead = kSpinner[(tick / 2) % kSpinner.size()];
            face.eye_left = face.eye_right = shut_eye;
            face.mouth    = flat;
            break;
        }
        case Mood::Thinking: {
            face.forehead = (tick / 4) % 2 == 0 ? "*" : "+";
            face.eye_left = face.eye_right = shut_eye;
            face.mouth    = " o ";
            break;
        }
        case Mood::Talking: {
            // Four-frame mouth cycle, the direct descendant of the original
            // Python bat's two-frame o/- flap.
            static const std::array<std::string, 4> kMouths{{" o ", " O ", flat, " - "}};
            face.mouth = kMouths[(tick / 2) % kMouths.size()];
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
    if (mood != Mood::Idle && mood != Mood::Error) {
        text += std::string((tick / 4) % 4, '.');
    }
    return "( " + text + " )";
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
