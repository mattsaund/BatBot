// SPDX-License-Identifier: MIT
//
// Measuring what a frame is showing, so it can be scrolled by exact lines.
//
// FTXUI scrolls a frame by centring whatever is focused, and `focusPosition`
// can put that anywhere -- but only in absolute lines, which means knowing how
// tall the content turned out and how tall the window showing it is. Neither is
// known until layout, and layout is what is being positioned.
//
// So they are measured during layout and read on the next frame. One frame
// behind, which is a sixtieth of a second nobody sees, and it makes the
// alternative -- laying the whole transcript out twice per frame, or scrolling
// in vague proportions of an unknown total -- unnecessary.
#pragma once

#include <ftxui/dom/elements.hpp>

namespace crucible::ui {

/// Wrap `child` and record the height it is given into `*height`.
///
/// Inside a frame this measures the content; outside one it measures the
/// window. Both are wanted, and they are the same measurement.
ftxui::Element measure_height(ftxui::Element child, int* height);

}  // namespace crucible::ui
