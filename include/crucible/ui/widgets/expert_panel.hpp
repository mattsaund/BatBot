// SPDX-License-Identifier: MIT
// The expert panel: Crucible on the left, the experts listed beside him,
// each lighting up as it is loaded and dimming as it is swapped back out.
#pragma once

#include <cstddef>

#include <ftxui/dom/elements.hpp>

#include "crucible/engine/state.hpp"
#include "crucible/ui/widgets/crucible_sprite.hpp"

namespace crucible::ui {

/// Draw the panel: the crucible on the left, the experts in a column beside it
/// with a status diamond each, and a connector to whichever one the delegation
/// chose.
ftxui::Element expert_panel(const Snapshot& snapshot, const CrucibleSprite& sprite,
                            std::size_t tick, bool compact);

/// A single-line version for terminals with no room for the panel: the experts
/// as a row of chips, with Crucible's current mood on the end.
ftxui::Element expert_strip(const Snapshot& snapshot, std::size_t tick);

}  // namespace crucible::ui
