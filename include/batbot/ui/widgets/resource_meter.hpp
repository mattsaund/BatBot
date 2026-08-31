// SPDX-License-Identifier: MIT
//
// The corner of the screen that says what the machine is doing.
#pragma once

#include <ftxui/dom/elements.hpp>

#include "batbot/util/resources.hpp"

namespace batbot::ui {

/// A compact block: one line per GPU and one for the processor, each with how
/// full its memory is, how busy it is, and how hot.
///
/// Drawn as an overlay rather than a column of its own, so it cannot move the
/// roundtable off centre -- which is the one thing the ring is arranged around.
ftxui::Element resource_meter(const util::ResourceSnapshot& snapshot);

}  // namespace batbot::ui
