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
constexpr ImU32 kFlameHot   = IM_COL32(0xFF, 0xE0, 0x9A, 0xFF);
constexpr ImU32 kError      = IM_COL32(0xFF, 0x5F, 0x00, 0xFF);

constexpr ImU32 kInk        = IM_COL32(0x0B, 0x0B, 0x0C, 0xFF);  ///< the ground
constexpr ImU32 kPanel      = IM_COL32(0x14, 0x14, 0x16, 0xFF);
constexpr ImU32 kPanelEdge  = IM_COL32(0x26, 0x26, 0x2A, 0xFF);
constexpr ImU32 kRaised     = IM_COL32(0x1E, 0x1E, 0x21, 0xFF);

constexpr ImU32 kText       = IM_COL32(0xEC, 0xEC, 0xEC, 0xFF);
constexpr ImU32 kTextDim    = IM_COL32(0x8C, 0x8C, 0x92, 0xFF);
constexpr ImU32 kTextFaint  = IM_COL32(0x5A, 0x5A, 0x60, 0xFF);

ImVec4 to_vec(ImU32 colour);

/// Apply the whole style: colours, rounding, spacing.
void apply();

/// Load the interface font, and a monospace one for code.
///
/// From the system rather than shipped. A bundled font is the usual answer and
/// it is the wrong one here: Crucible installs as a binary and a lib directory,
/// and adding a search path for a typeface -- which then has to be found again
/// after an install, a move, or a run from the build tree -- buys nothing over
/// using the font the machine already has. Every desktop has one of the
/// families tried here.
///
/// Falls back to ImGui's built-in bitmap font, which is ugly and always works.
/// `scale` comes from the display's DPI.
void load_fonts(float scale);

/// The monospace face, for code blocks and command output. Never null once
/// load_fonts has run -- it is the interface font when nothing better was found.
ImFont* mono();

/// The mark: a flame, drawn rather than loaded.
///
/// Vector shapes rather than an image, for three reasons. It is one shape, so a
/// file would be mostly overhead. It has to be crisp at sixteen pixels in a
/// title bar and at two hundred on a splash, which one bitmap cannot do. And an
/// asset would have to be found at runtime, which means an install layout and a
/// search path for a picture the program can simply draw.
///
/// Two stacked teardrops -- the body and a brighter core. Deliberately convex:
/// a real flame outline has a notch at its base, and filling a concave path is
/// a capability ImGui only grew recently. Two convex shapes read as a flame at
/// every size and work everywhere.
void draw_flame(ImDrawList* draw, ImVec2 centre, float radius, float alpha = 1.0F);

/// The flame with the vessel under it, for places with room for the whole
/// idea rather than just the mark.
void draw_crucible(ImDrawList* draw, ImVec2 centre, float radius);

/// A status diamond, the same vocabulary the terminal roundtable uses: filled
/// means working, hollow means ready, faint means nothing assigned.
enum class Dot { Active, Loading, Ready, Missing, Empty };
void draw_dot(ImDrawList* draw, ImVec2 centre, float radius, Dot dot, float phase);

}  // namespace crucible::gui::theme
