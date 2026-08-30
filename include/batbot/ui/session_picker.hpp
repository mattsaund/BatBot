// SPDX-License-Identifier: MIT
//
// The `/resume` list: past conversations about this project, newest first.
//
// A separate widget for the same reason the model picker is one -- it is a
// modal list with its own keys, and folding it into the application shell
// would put two unrelated state machines in one event handler.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "batbot/session/store.hpp"

namespace batbot::ui {

/// What the picker wants the application to do after a key.
enum class SessionPickerAction {
    None,
    Close,    ///< dismissed without choosing
    Resume,   ///< load `chosen()` into the transcript
    Delete,   ///< remove `chosen()` from disk, then stay open
};

class SessionPicker {
public:
    /// Fill the list from `store`. Called each time the picker opens, so a
    /// session saved since last time is there.
    void open(const SessionStore& store);
    void close() { open_ = false; }
    bool active() const { return open_; }

    /// Re-read the list, keeping the selection in range. Used after a delete.
    void refresh(const SessionStore& store);

    /// The highlighted session, or empty when the list is.
    std::string chosen() const;

    SessionPickerAction handle(const ftxui::Event& event);
    ftxui::Element render() const;

private:
    void move(int delta);

    std::vector<SessionSummary> sessions_;
    std::size_t selected_ = 0;
    bool        open_     = false;
    std::string project_name_;

    /// Set while the highlighted row is one keypress from being deleted, so a
    /// stray `d` cannot throw away a conversation.
    bool confirming_delete_ = false;
};

}  // namespace batbot::ui
