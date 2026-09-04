// SPDX-License-Identifier: MIT
// The crucible itself: the ASCII vessel, and the fire under it that says what
// the engine is doing.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "crucible/engine/state.hpp"

namespace crucible::ui {

/// Renders the crucible as fixed-width text lines.
///
/// Every frame a sprite produces is the same width and height for a given
/// size, so the expert panel never reflows as the fire moves.
///
/// The vessel never changes. Only the flame above it and the melt inside it
/// do, and they carry the whole of the animation: a bare ember when nothing is
/// running, a steady column while a model is being read in, and a full rolling
/// boil while tokens are coming out. That mapping is the point of the sprite --
/// how hard the fire is burning is how hard the machine is working.
class CrucibleSprite {
public:
    /// `unicode` picks rounder glyphs for the flame and the melt; the ASCII set
    /// is the fallback for terminals or fonts that render them as boxes.
    explicit CrucibleSprite(bool unicode);

    /// `tick` is a free-running frame counter; `compact` selects the short
    /// five-row crucible used when the terminal is too short for the full one.
    std::vector<std::string> render(Mood mood, std::size_t tick, bool compact) const;

    static int width(bool compact)  { return compact ? 15 : 19; }
    static int height(bool compact) { return compact ? 5 : 11; }

private:
    /// One frame of fire. Each field is a fixed display width so it can be
    /// substituted into the template without moving anything around it.
    struct Fire {
        std::string tip;    ///< display width 1
        std::string mid;    ///< display width 3
        std::string base;   ///< display width 5
        std::string melt;   ///< display width 9 (full) / 5 (compact)
    };

    Fire fire_for(Mood mood, std::size_t tick, bool compact) const;

    bool unicode_;
};

/// A one-line "status bubble" shown under the crucible while it is working,
/// e.g. `( routing... )`. Returns an empty string when there is nothing to say.
std::string thought_bubble(Mood mood, const std::string& status, std::size_t tick);

}  // namespace crucible::ui
