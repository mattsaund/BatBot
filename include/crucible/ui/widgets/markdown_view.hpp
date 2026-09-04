// SPDX-License-Identifier: MIT
//
// Drawing the markdown a model wrote. The parsing half is util/markdown.hpp,
// which knows nothing about terminals; this is the half that knows nothing
// about markdown beyond what that produced.
#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace crucible::ui {

/// One element per line of `text`, styled.
///
/// Returned as a list rather than a single element so the caller can put it in
/// whatever it is already building -- and so a reply and the stats under it
/// stay in one vbox rather than becoming a box inside a box.
///
/// `dim_all` renders everything muted, for reasoning rather than an answer.
std::vector<ftxui::Element> render_markdown(const std::string& text, bool dim_all = false);

}  // namespace crucible::ui
