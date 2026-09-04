// SPDX-License-Identifier: MIT
//
// The crucible.
//
// The sprite is a fixed-width template with marker characters substituted at
// render time, so every frame is exactly the same size and the expert panel never
// reflows as the fire moves. Substitution is by marker rather than byte offset,
// which is what lets a multi-byte glyph stand in for a single-width slot.
//
// The vessel and its stand are constant. Everything that moves is fire: the
// plume above the mouth and the melt inside it. That is the whole design --
// how hard it is burning is how hard the machine is working, so a glance at
// the corner of the screen answers "is it doing anything" without reading a
// word.
#include "crucible/ui/widgets/crucible_sprite.hpp"

#include <array>

#include "crucible/ui/theme.hpp"

namespace crucible::ui {
namespace {

// The sprite templates. Marker characters are substituted at render time:
//   A         -> flame tip,           display width 1
//   BBB       -> flame body,          display width 3
//   CCCCC     -> flame base,          display width 5
//   MMMMMMMMM -> the melt, full,      display width 9
//   NNNNN     -> the melt, compact,   display width 5
// Substitution is by marker, not byte offset, so multi-byte glyphs are fine as
// long as they occupy the same display width.
//
// The melt has two markers rather than one because the two sizes need two
// widths, and a marker has to be exactly as wide as what replaces it or the
// line changes length. Both are looked for; only one is ever present.
//
// Raw string literals keep the backslashes of the vessel and the stand
// readable.
constexpr std::array<const char*, 11> kFullCrucible{{
    R"(         A         )",
    R"(        BBB        )",
    R"(       CCCCC       )",
    R"(   ,-----------,   )",
    R"(   \ MMMMMMMMM /   )",
    R"(    \         /    )",
    R"(     \_______/     )",
    R"(      /|   |\      )",
    R"(     / |___| \     )",
    R"(    /         \    )",
    R"(   '-----------'   )",
}};

// The compact crucible is a crop to the vessel: at this size the stand costs
// four rows and says nothing the pot does not already say, and the flame is
// what the sprite is for.
//
// It keeps the flame's lower two rows rather than its upper two. Idle burns
// only in the base row, so cropping from the top would leave an idle compact
// crucible with no fire in it at all -- which is the state it spends most of
// its life in, and the one where "is this thing on" most needs answering.
// The cost is the tip, which only Talking ever lights.
constexpr std::array<const char*, 5> kCompactCrucible{{
    R"(      BBB      )",
    R"(     CCCCC     )",
    R"(  ,---------,  )",
    R"(  \  NNNNN  /  )",
    R"(   \_______/   )",
}};

void replace_marker(std::string& line, std::string_view marker, const std::string& value) {
    if (const std::size_t pos = line.find(marker); pos != std::string::npos) {
        line.replace(pos, marker.size(), value);
    }
}

/// `unit` repeated to `width` display columns.
///
/// The melt is the one part whose width depends on which template is in play,
/// so it is built rather than written out twice per mood.
std::string band(const std::string& unit, std::size_t width) {
    std::string out;
    for (std::size_t i = 0; i < width; ++i) {
        out += unit;
    }
    return out;
}

}  // namespace

CrucibleSprite::CrucibleSprite(bool unicode) : unicode_(unicode) {}

CrucibleSprite::Fire CrucibleSprite::fire_for(Mood mood, std::size_t tick, bool compact) const {
    // Two glyph sets. The ASCII one is not a lesser fallback -- it is what most
    // terminals render most crisply at small sizes.
    const std::string wave = unicode_ ? "≈" : "~";  // ≈
    const std::string dead = "_";

    const std::size_t melt_width = compact ? 5 : 9;

    Fire fire{" ", "   ", "     ", band(wave, melt_width)};

    switch (mood) {
        case Mood::Idle: {
            // Banked, not out. A single tick of flame over the melt, alternating
            // with a pair of sparks either side of it -- enough movement that an
            // idle crucible still looks lit, and little enough that it is not
            // asking to be watched.
            static const std::array<const char*, 2> kBase{{"  ^  ", " ,^, "}};
            fire.base = kBase[(tick / 12) % kBase.size()];
            break;
        }
        case Mood::Routing:
        case Mood::Loading:
        case Mood::Thinking: {
            // A steady column. The three phases before an answer starts can run
            // for a minute on a large expert, so this is deliberately calm: one
            // slow flicker of the enclosure around a core that does not move.
            // The status bubble underneath says which of the three it is, and
            // says it in words rather than in glyphs nobody can tell apart.
            static const std::array<const char*, 2> kMid{{" ^ ", " ^ "}};
            static const std::array<const char*, 2> kBase{{"(/^\\)", " /^\\ "}};
            fire.mid  = kMid[(tick / 6) % kMid.size()];
            fire.base = kBase[(tick / 6) % kBase.size()];
            break;
        }
        case Mood::Talking: {
            // Full boil, and the only state where the plume reaches the top row.
            // Three times the flicker rate of the loading states, plus a bubble
            // travelling across the melt: this is the one moment when the thing
            // is actually producing, and it should be visibly different from the
            // waiting that surrounds it.
            static const std::array<const char*, 2> kTip{{"^", "'"}};
            static const std::array<const char*, 2> kMid{{"(^)", ")^("}};
            static const std::array<const char*, 2> kBase{{"(/^\\)", "(\\^/)"}};
            const std::size_t frame = (tick / 3) % 2;
            fire.tip  = kTip[frame];
            fire.mid  = kMid[frame];
            fire.base = kBase[frame];

            // One bubble surfacing, walking left to right and wrapping. Built by
            // overwriting a cell of the band rather than by writing out every
            // position, so it stays correct at both melt widths.
            const std::size_t at = (tick / 4) % melt_width;
            fire.melt.clear();
            for (std::size_t i = 0; i < melt_width; ++i) {
                fire.melt += (i == at) ? "o" : wave;
            }
            break;
        }
        case Mood::Error: {
            // Gone out. Smoke where the flame was and a cold, flat melt -- the
            // one state the sprite shows by taking something away rather than
            // by adding movement.
            fire.mid  = " ~ ";
            fire.base = " ~~~ ";
            fire.melt = band(dead, melt_width);
            break;
        }
    }

    return fire;
}

std::vector<std::string> CrucibleSprite::render(Mood mood, std::size_t tick, bool compact) const {
    const Fire fire = fire_for(mood, tick, compact);

    std::vector<std::string> lines;
    if (compact) {
        lines.assign(kCompactCrucible.begin(), kCompactCrucible.end());
    } else {
        lines.assign(kFullCrucible.begin(), kFullCrucible.end());
    }

    for (std::string& line : lines) {
        // Longest marker first. The melt markers do not overlap the flame ones,
        // but CCCCC contains BBB's shape of problem if the templates ever grow,
        // and longest-first is the habit that keeps this correct when they do.
        replace_marker(line, "MMMMMMMMM", fire.melt);
        replace_marker(line, "NNNNN",     fire.melt);
        replace_marker(line, "CCCCC",     fire.base);
        replace_marker(line, "BBB",       fire.mid);
        replace_marker(line, "A",         fire.tip);
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
    // Padded back out to a constant width. The bubble and the crucible are
    // centred inside the same column, so a bubble that grows by a character
    // shifts the whole sprite half a column -- which reads as the pot rocking
    // side to side once a second, and is far more distracting than the dots
    // are useful.
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
