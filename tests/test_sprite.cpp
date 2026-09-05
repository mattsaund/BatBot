// SPDX-License-Identifier: MIT
//
// The flame.
//
// One property matters and it is not artistic: every frame the sprite produces
// is the same size. The expert panel lays the mark out beside a column of
// seats, and FTXUI sizes a vbox to its widest child -- so a frame one column
// narrow makes the whole panel step sideways for a few hundredths of a second,
// which reads as the mark twitching rather than burning.
//
// It is easy to break by hand: the frames are written out as text, and a
// trailing space that a stray editor eats is invisible in a diff.
#include "test_helpers.hpp"

#include "crucible/ui/widgets/flame_sprite.hpp"

namespace {

using crucible::Mood;
using crucible::ui::FlameSprite;

const std::vector<Mood>& all_moods() {
    static const std::vector<Mood> moods{Mood::Idle,     Mood::Routing, Mood::Loading,
                                         Mood::Thinking, Mood::Talking, Mood::Error};
    return moods;
}

/// Columns, not bytes: a multi-byte glyph is one column wide.
std::size_t columns(const std::string& line) {
    std::size_t width = 0;
    for (const char byte : line) {
        width += (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return width;
}

}  // namespace

TEST(every_frame_is_the_same_size) {
    for (const bool unicode : {false, true}) {
        for (const bool compact : {false, true}) {
            const FlameSprite  sprite(unicode);
            const std::size_t  rows = static_cast<std::size_t>(FlameSprite::height(compact));
            const std::size_t  cols = static_cast<std::size_t>(FlameSprite::width(compact));
            for (const Mood mood : all_moods()) {
                // Well past the longest flicker period, so both frames of every
                // mood are covered several times over.
                for (std::size_t tick = 0; tick < 64; ++tick) {
                    const std::vector<std::string> lines = sprite.render(mood, tick, compact);
                    CHECK_EQ(lines.size(), rows);
                    for (const std::string& line : lines) {
                        CHECK_EQ(columns(line), cols);
                    }
                }
            }
        }
    }
}

TEST(the_foot_of_the_flame_never_moves) {
    // Fire grows upward from where it is lit. If the base drifted with the
    // mood, the mark would slide up and down the panel every time the engine
    // changed state.
    for (const bool compact : {false, true}) {
        const FlameSprite sprite(true);
        const std::string foot = sprite.render(Mood::Talking, 0, compact).back();
        for (const Mood mood : all_moods()) {
            for (std::size_t tick = 0; tick < 64; ++tick) {
                const std::vector<std::string> lines = sprite.render(mood, tick, compact);
                CHECK_EQ(lines.back(), foot);
            }
        }
    }
}

TEST(the_flame_is_taller_the_harder_the_engine_is_working) {
    // The whole point of the sprite: a glance says whether anything is
    // happening. Measured as the first row with any ink in it, which is the
    // top of the flame.
    const FlameSprite sprite(false);
    const auto top = [&sprite](Mood mood) {
        const std::vector<std::string> lines = sprite.render(mood, 0, false);
        for (std::size_t row = 0; row < lines.size(); ++row) {
            if (lines[row].find_first_not_of(' ') != std::string::npos) {
                return row;
            }
        }
        return lines.size();
    };
    CHECK(top(Mood::Talking) < top(Mood::Loading));
    CHECK(top(Mood::Loading) < top(Mood::Idle));
}

TEST(nothing_left_of_the_crucible_that_used_to_stand_here) {
    // The mark was a pot on a stand with a fire under it. It is a flame now,
    // everywhere -- the window, the launcher icon, the banner and this. The
    // stand's rim is the piece that would survive a half-finished edit.
    const FlameSprite sprite(true);
    for (const bool compact : {false, true}) {
        for (const Mood mood : all_moods()) {
            for (const std::string& line : sprite.render(mood, 0, compact)) {
                CHECK(line.find("---") == std::string::npos);
                CHECK(line.find("___") == std::string::npos);
            }
        }
    }
}

TEST(plain_ascii_is_available_for_terminals_that_need_it) {
    const FlameSprite ascii(false);
    for (const bool compact : {false, true}) {
        for (const Mood mood : all_moods()) {
            for (std::size_t tick = 0; tick < 64; ++tick) {
                for (const std::string& line : ascii.render(mood, tick, compact)) {
                    for (const char byte : line) {
                        CHECK(static_cast<unsigned char>(byte) < 0x80U);
                    }
                }
            }
        }
    }
}
