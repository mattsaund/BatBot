// SPDX-License-Identifier: MIT
//
// Rendering and driving the `/resume` list.
#include "batbot/ui/session_picker.hpp"

#include <algorithm>

#include "batbot/session/usage.hpp"
#include "batbot/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {

void SessionPicker::open(const SessionStore& store) {
    open_              = true;
    selected_          = 0;
    confirming_delete_ = false;
    project_name_      = store.project().name;
    sessions_          = store.list();
}

void SessionPicker::refresh(const SessionStore& store) {
    sessions_ = store.list();
    if (selected_ >= sessions_.size() && !sessions_.empty()) {
        selected_ = sessions_.size() - 1;
    }
    confirming_delete_ = false;
}

std::string SessionPicker::chosen() const {
    if (selected_ >= sessions_.size()) {
        return {};
    }
    return sessions_[selected_].id;
}

void SessionPicker::move(int delta) {
    if (sessions_.empty()) {
        return;
    }
    const int last = static_cast<int>(sessions_.size()) - 1;
    int next = static_cast<int>(selected_) + delta;
    next = std::clamp(next, 0, last);
    selected_ = static_cast<std::size_t>(next);
    confirming_delete_ = false;
}

SessionPickerAction SessionPicker::handle(const Event& event) {
    if (!open_) {
        return SessionPickerAction::None;
    }

    if (event == Event::Escape || event == Event::Character('q')) {
        open_ = false;
        return SessionPickerAction::Close;
    }
    if (event == Event::ArrowUp   || event == Event::Character('k')) { move(-1); return SessionPickerAction::None; }
    if (event == Event::ArrowDown || event == Event::Character('j')) { move(1);  return SessionPickerAction::None; }
    if (event == Event::PageUp)   { move(-10); return SessionPickerAction::None; }
    if (event == Event::PageDown) { move(10);  return SessionPickerAction::None; }
    if (event == Event::Home)     { selected_ = 0; return SessionPickerAction::None; }
    if (event == Event::End && !sessions_.empty()) {
        selected_ = sessions_.size() - 1;
        return SessionPickerAction::None;
    }

    if (event == Event::Character('d')) {
        // First press arms, second confirms. Deleting a conversation is not
        // undoable and `d` is next to the movement keys.
        if (sessions_.empty()) {
            return SessionPickerAction::None;
        }
        if (!confirming_delete_) {
            confirming_delete_ = true;
            return SessionPickerAction::None;
        }
        confirming_delete_ = false;
        return SessionPickerAction::Delete;
    }

    if (event == Event::Return && !sessions_.empty()) {
        open_ = false;
        return SessionPickerAction::Resume;
    }

    return SessionPickerAction::None;
}

Element SessionPicker::render() const {
    Elements rows;

    if (sessions_.empty()) {
        rows.push_back(text("  no saved conversations for this project yet") |
                       color(theme::kMeta) | dim);
        rows.push_back(text("") );
        rows.push_back(text("  a session is written once its first reply finishes") |
                       color(theme::kMeta) | dim);
    }

    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        const SessionSummary& session = sessions_[i];
        const bool chosen = i == selected_;

        // Fixed-width leading columns keep the titles aligned down the list,
        // which is what makes it scannable.
        std::string when = session.when();
        when.resize(std::max<std::size_t>(when.size(), 12), ' ');

        const std::string counts =
            std::to_string(session.turns) + (session.turns == 1 ? " turn" : " turns");

        Element row = hbox({
            text(chosen ? " ▸ " : "   "),
            text(when) | color(theme::kMeta),
            text(session.title) | flex,
            text("  " + counts + "  ") | color(theme::kMeta) | dim,
            text(format_tokens(session.usage.total_tokens()) + " tok") |
                color(theme::kMeta) | dim,
        });

        if (chosen) {
            row = row | inverted;
        }
        rows.push_back(row);
    }

    std::string hint = "↑↓ choose   enter resume   d delete   esc cancel";
    if (confirming_delete_) {
        hint = "press d again to delete this conversation, any other key to keep it";
    }

    return vbox({
        hbox({
            text(" resume ") | bold | color(theme::kBat),
            text("· " + project_name_) | color(theme::kMeta),
        }),
        separator(),
        vbox(std::move(rows)) | vscroll_indicator | yframe | flex,
        separator(),
        text(hint) | color(confirming_delete_ ? theme::kError : theme::kMeta) | dim,
    }) | border | bgcolor(Color::Black) | clear_under;
}

}  // namespace batbot::ui
