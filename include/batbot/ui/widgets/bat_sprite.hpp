// SPDX-License-Identifier: MIT
// BatBot himself: the ASCII mascot, and the animation that gives him a mouth
// that moves, eyes that blink, and a thinking star on his forehead.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "batbot/engine/state.hpp"

namespace batbot::ui {

/// Renders the bat as fixed-width text lines.
///
/// Every frame a sprite produces is the same width and height for a given
/// size, so the roundtable never reflows as he animates.
class BatSprite {
public:
    /// `unicode` picks rounder glyphs for the eyes and mouth; the ASCII set is
    /// the fallback for terminals or fonts that render them as boxes.
    explicit BatSprite(bool unicode);

    /// `tick` is a free-running frame counter; `compact` selects the short
    /// five-row bat used when the terminal is too short for the full one.
    std::vector<std::string> render(Mood mood, std::size_t tick, bool compact) const;

    static int width(bool compact)  { return compact ? 15 : 19; }
    static int height(bool compact) { return compact ? 5 : 11; }

private:
    struct Face {
        std::string forehead;  ///< display width 1
        std::string eye_left;  ///< display width 1
        std::string eye_right; ///< display width 1
        std::string mouth;     ///< display width 3
    };

    Face face_for(Mood mood, std::size_t tick) const;

    bool unicode_;
};

/// A one-line "thought bubble" shown above BatBot while he is working, e.g.
/// `( routing... )`. Returns an empty string when there is nothing to say.
std::string thought_bubble(Mood mood, const std::string& status, std::size_t tick);

}  // namespace batbot::ui
