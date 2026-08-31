// SPDX-License-Identifier: MIT
// One place for every colour BatBot draws with.
#pragma once

#include <ftxui/screen/color.hpp>

#include "batbot/engine/state.hpp"

namespace batbot::ui {

/// Named colours rather than RGB: they inherit the user's terminal palette, so
/// BatBot looks at home in whatever theme they already run.
namespace theme {

// BatBot is blue, the same family as the experts around him, so the roundtable
// reads as one object rather than a mascot dropped into a diagram.
inline constexpr ftxui::Color::Palette16 kBat        = ftxui::Color::Blue;
inline constexpr ftxui::Color::Palette16 kBatBusy    = ftxui::Color::BlueLight;
inline constexpr ftxui::Color::Palette16 kBatError   = ftxui::Color::RedLight;

// Seat states have to stay separable at a glance: dim grey means no model,
// blue means on disk, cyan means being read in, green means answering.
inline constexpr ftxui::Color::Palette16 kSeatActive       = ftxui::Color::GreenLight;
inline constexpr ftxui::Color::Palette16 kSeatLoading      = ftxui::Color::CyanLight;
inline constexpr ftxui::Color::Palette16 kSeatDormant      = ftxui::Color::Blue;
inline constexpr ftxui::Color::Palette16 kSeatUnconfigured = ftxui::Color::GrayDark;

inline constexpr ftxui::Color::Palette16 kUser    = ftxui::Color::CyanLight;
inline constexpr ftxui::Color::Palette16 kRoute   = ftxui::Color::Magenta;
inline constexpr ftxui::Color::Palette16 kMeta    = ftxui::Color::GrayDark;
// Notices are mostly informational -- /help, /models, startup device lines --
// so they take the calm colour. Anything genuinely wrong uses kError.
inline constexpr ftxui::Color::Palette16 kNotice  = ftxui::Color::CyanLight;
inline constexpr ftxui::Color::Palette16 kError   = ftxui::Color::RedLight;
inline constexpr ftxui::Color::Palette16 kAccent  = ftxui::Color::Magenta;

// The row under the cursor, in every list BatBot draws.
//
// kMeta is GrayDark and so is this, which meant secondary text -- a file size,
// a build date, a "(none)" -- vanished completely the moment its row was
// selected. Anything drawn on a highlighted row uses kMetaOnHighlight instead,
// and meta_color() below picks between the two.
inline constexpr ftxui::Color::Palette16 kHighlight       = ftxui::Color::GrayDark;
inline constexpr ftxui::Color::Palette16 kMetaOnHighlight = ftxui::Color::GrayLight;

// --- rendered markdown -----------------------------------------------------
// Models answer in markdown whether or not you ask them to. These are what its
// parts are drawn in; see util/markdown.hpp.
inline constexpr ftxui::Color::Palette16 kHeading = ftxui::Color::CyanLight;
inline constexpr ftxui::Color::Palette16 kCode    = ftxui::Color::GreenLight;
inline constexpr ftxui::Color::Palette16 kMarker  = ftxui::Color::Magenta;

}  // namespace theme

/// Secondary text, in the shade that stays readable on the row it is on.
ftxui::Color meta_color(bool highlighted);

/// The colour BatBot's sprite takes for a given mood.
ftxui::Color mood_color(Mood mood);

}  // namespace batbot::ui
