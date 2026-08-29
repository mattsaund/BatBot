// SPDX-License-Identifier: MIT
//
// The dialog that picks the models directory.
//
// Shows how many GGUFs sit in each candidate folder, which is what tells you
// at a glance whether it is the folder you meant.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace batbot::ui {

class DirectoryBrowser {
public:
    /// Open at `start`, or at the nearest existing ancestor if that folder has
    /// been deleted or unplugged -- otherwise the browser would open empty with
    /// no way onward.
    void open(std::filesystem::path start);

    void close() { active_ = false; }
    bool active() const { return active_; }

    /// True when the user asked to type a path instead of browsing, which the
    /// settings screen answers by handing them the line editor.
    bool wants_manual_entry() const { return wants_manual_entry_; }

    const std::filesystem::path& path() const { return path_; }

    /// Apply one key. Returns the chosen directory once the user commits, and
    /// nothing while still browsing or on cancel.
    std::optional<std::filesystem::path> handle(const ftxui::Event& event);

    ftxui::Element render() const;

private:
    /// One row: a subdirectory, or the ".." entry.
    struct Entry {
        std::string           label;
        std::filesystem::path path;
        std::size_t           models    = 0;
        bool                  is_parent = false;
    };

    void rescan();
    void go_up();

    bool                  active_ = false;
    bool                  wants_manual_entry_ = false;
    std::filesystem::path path_;
    std::vector<Entry>    entries_;
    /// Slot 0 is "[ use this directory ]"; entries start at 1.
    std::size_t           index_ = 0;

    /// Dotfiles are hidden by default -- a home folder holds dozens and none
    /// are interesting -- but the default models directory lives under
    /// ~/.local, so there has to be a way to reach them.
    bool                  show_hidden_  = false;
    std::size_t           hidden_count_ = 0;
};

}  // namespace batbot::ui
