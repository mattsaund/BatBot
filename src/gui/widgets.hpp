// SPDX-License-Identifier: MIT
//
// The small drawing helpers every panel shares.
//
// None of these know anything about Crucible; they are the vocabulary the
// panels are written in. They live here rather than in each panel so that a
// heading looks the same on every page -- which is most of what makes a window
// look like one program rather than six.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "crucible/cook/journal.hpp"
#include "crucible/engine/state.hpp"
#include "theme.hpp"

namespace crucible::gui {

/// Layout in multiples of the font size rather than in pixels.
///
/// The interface is loaded at the display's own scale, so a sidebar written as
/// 268 pixels is two thirds the width it should be on a 4K panel and the
/// composer under it gets clipped. Everything laid out here is in `em`, which
/// tracks whatever size the font was actually loaded at.
float em(float n);

/// Formatted text in one colour.
void text_coloured(ImU32 colour, const char* fmt, ...) IM_FMTARGS(2);

/// Wrapped body text in one colour.
void wrapped(ImU32 colour, const std::string& text);

/// A section heading: small, faint, spaced above.
void section(const char* label);

/// A page title.
void title(const char* label);

/// A model reference as it should be read.
///
/// The config stores a bare file name for a model in the models directory and
/// an absolute path for one anywhere else. The path is the useful thing to keep
/// and the useless thing to show: a combo box twenty ems wide renders
/// "/mnt/media_drive/.models/lmstudio-community/Qwen3-Cod" and stops, which
/// says nothing about which model it is. The full path is the tooltip.
std::string model_label(const std::string& reference);

/// A path trimmed from the left, so the end -- which is the part that says
/// which project this is -- survives.
std::string tail_of(const std::filesystem::path& path, std::size_t width);

/// The status mark for a seat, in the same vocabulary the terminal panel uses.
theme::Dot dot_for(SeatPhase phase);

/// A mood as one lowercase word, for the line under the mark.
const char* mood_text(Mood mood);

/// The colour a cook step is drawn in: red for a failure, orange for a write,
/// flame for anything that ran, faint for the rest.
ImU32 step_colour(const CookStep& step);

/// Subdirectories of `dir`, sorted, with hidden ones left out.
std::vector<std::filesystem::path> subdirectories(const std::filesystem::path& dir);

}  // namespace crucible::gui
