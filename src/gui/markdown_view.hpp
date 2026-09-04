// SPDX-License-Identifier: MIT
//
// Drawing the markdown a model wrote.
//
// The parsing is shared with the terminal (util/markdown.hpp), so both faces
// break a reply into the same blocks and only the drawing differs. This is the
// drawing.
//
// ImGui has no rich text: a run of styled words is not something you can hand
// to TextWrapped, because the font changes mid-line and the wrapping has to
// know about it. So paragraphs are laid out here, word by word, against the
// available width -- which is also what makes an inline `code` span able to
// carry a background without the rest of the line inheriting it.
#pragma once

#include <string>
#include <string_view>

#include <imgui.h>

namespace crucible::gui {

/// Render `text` as markdown at the current cursor.
///
/// `base` is the colour ordinary prose takes; headings, code and quotes have
/// their own. Consumes the full available width and advances the cursor past
/// what it drew.
void draw_markdown(std::string_view text, ImU32 base);

/// Render `text` as one fenced code block, without parsing it as markdown.
///
/// For content that is code by construction and must not be reinterpreted: a
/// diff, or a command's captured output. A `#` at the start of a shell line is
/// a comment, and passing it through the markdown parser would make it a
/// heading.
void draw_code_block(std::string_view text, ImU32 tint = 0);

}  // namespace crucible::gui
