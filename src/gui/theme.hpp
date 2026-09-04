// SPDX-License-Identifier: MIT
//
// The desktop application's palette and its mark.
//
// The same three colours the terminal uses, in the same roles: black is the
// ground, white is everything neutral, orange is heat and heat means work. The
// terminal expresses that in 256-colour indices because that is what a terminal
// has; here they are the exact RGB those indices name, so the two faces of
// Crucible are recognisably one program.
#pragma once

#include <imgui.h>

namespace crucible::gui::theme {

// The palette, as the 256-colour indices the TUI uses actually render.
// DarkOrange is 208 (#ff8700), Orange1 is 214 (#ffaf00), OrangeRed1 is 202
// (#ff5f00). Keeping the numbers identical is what stops the two faces drifting
// into two different oranges.
constexpr ImU32 kFlame      = IM_COL32(0xFF, 0x87, 0x00, 0xFF);
constexpr ImU32 kFlameBright = IM_COL32(0xFF, 0xAF, 0x00, 0xFF);
constexpr ImU32 kError      = IM_COL32(0xFF, 0x5F, 0x00, 0xFF);

constexpr ImU32 kInk        = IM_COL32(0x0B, 0x0B, 0x0C, 0xFF);  ///< the ground
constexpr ImU32 kPanel      = IM_COL32(0x14, 0x14, 0x16, 0xFF);
constexpr ImU32 kPanelEdge  = IM_COL32(0x26, 0x26, 0x2A, 0xFF);
constexpr ImU32 kRaised     = IM_COL32(0x1E, 0x1E, 0x21, 0xFF);

// A diff has to separate added from removed, and the palette has one saturated
// colour. So added is orange -- new, hot -- and removed is faint grey, which
// reads as "this is gone" and does not compete with it. Red for removed would
// sit a few degrees from the orange and be worse than either.
constexpr ImU32 kAdded      = IM_COL32(0xFF, 0xAF, 0x00, 0xFF);
constexpr ImU32 kRemoved    = IM_COL32(0x77, 0x77, 0x7D, 0xFF);

constexpr ImU32 kText       = IM_COL32(0xEC, 0xEC, 0xEC, 0xFF);
constexpr ImU32 kTextDim    = IM_COL32(0x8C, 0x8C, 0x92, 0xFF);
constexpr ImU32 kTextFaint  = IM_COL32(0x5A, 0x5A, 0x60, 0xFF);

ImVec4 to_vec(ImU32 colour);

/// Apply the whole style: colours, rounding, spacing.
void apply();

/// Load the interface faces.
///
/// JetBrains Mono, compiled into the binary -- see cmake/EmbedBinary.cmake for
/// why. One family for the whole interface, monospace throughout: Crucible's
/// other face is a terminal, and a desktop app in a proportional font beside a
/// TUI in a fixed one reads as two programs rather than two views of one.
///
/// Three weights, which is what rendering markdown needs: prose, **bold** and
/// *italic*. Falls back to a system monospace, and then to ImGui's built-in
/// bitmap face, because a missing typeface is not a reason to have no window.
///
/// `scale` comes from the display's DPI.
void load_fonts(float scale);

/// The faces, after load_fonts. `body()` is never null; the others fall back to
/// it when only one face could be loaded.
ImFont* body();
ImFont* bold();
ImFont* italic();

/// The larger face used for markdown headings.
ImFont* heading();

/// The mark: a flame, drawn rather than loaded.
///
/// Vector shapes rather than an image, for three reasons. It is one shape, so a
/// file would be mostly overhead. It has to be crisp at sixteen pixels in a
/// title bar and at two hundred on a splash, which one bitmap cannot do. And an
/// asset would have to be found at runtime, which means an install layout and a
/// search path for a picture the program can simply draw.
///
/// One silhouette, filled twice: the body and a hotter core leaning the other
/// way. The base curls inward to a notch, which is the whole difference between
/// something that reads as fire and something that reads as a leaf.
///
/// It does not move. Nothing in this file does.
void draw_flame(ImDrawList* draw, ImVec2 centre, float radius, float alpha = 1.0F);

/// A status diamond, the same vocabulary the terminal panel uses: filled
/// means working, hollow means ready, faint means nothing assigned. Static --
/// what moves is the percentage beside a loading seat, not the mark.
enum class Dot { Active, Loading, Ready, Missing, Empty };
void draw_dot(ImDrawList* draw, ImVec2 centre, float radius, Dot dot);

}  // namespace crucible::gui::theme
