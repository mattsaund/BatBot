// SPDX-License-Identifier: MIT
//
// A one-line text editor for the settings screen.
//
// Paths are long, so append-and-backspace is not enough: this keeps a cursor,
// moves by whole codepoints, and offers the two shortcuts that matter when you
// are correcting a directory -- drop the last path component, or clear the line
// and start again.
#pragma once

#include <cstddef>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace batbot::ui {

class LineEditor {
public:
    /// Start editing `initial`, cursor at the end -- the common edit is
    /// appending to or trimming a path tail rather than inserting at the front.
    void begin(std::string initial);

    /// Stop editing and forget the buffer.
    void cancel();

    bool active() const { return active_; }

    /// The text being edited. Named `value` rather than `text` because FTXUI
    /// has a free function of that name, which a member would shadow inside
    /// this class's own rendering code.
    const std::string& value() const { return buffer_; }

    /// Apply one key. Returns false for keys the editor does not use, so the
    /// caller can decide what Enter and Escape mean in its own context.
    bool handle(const ftxui::Event& event);

    /// The buffer with the cursor drawn in place. Scrolls horizontally to keep
    /// the cursor visible, which for a path means showing the end.
    ftxui::Element render() const;

private:
    bool        active_ = false;
    std::string buffer_;
    /// Byte offset into `buffer_`, always on a UTF-8 codepoint boundary.
    std::size_t cursor_ = 0;
};

}  // namespace batbot::ui
