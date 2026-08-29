// SPDX-License-Identifier: MIT
// The roundtable: BatBot at the centre, the nine experts seated around him,
// each lighting up as it is loaded and dimming as it is swapped back out.
#pragma once

#include <cstddef>

#include <ftxui/dom/elements.hpp>

#include "batbot/engine/state.hpp"
#include "batbot/ui/widgets/bat_sprite.hpp"

namespace batbot::ui {

/// Draw the full ring: three experts across the top, two down each side
/// flanking BatBot, two along the bottom.
ftxui::Element roundtable(const Snapshot& snapshot, const BatSprite& bat,
                          std::size_t tick, bool compact);

/// A single-line version for terminals with no room for the ring: the seats as
/// a row of chips, with BatBot's current mood on the end.
ftxui::Element roundtable_strip(const Snapshot& snapshot, std::size_t tick);

}  // namespace batbot::ui
