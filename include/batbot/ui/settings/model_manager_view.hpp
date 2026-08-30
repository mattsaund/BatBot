// SPDX-License-Identifier: MIT
//
// The Manage models panel: deleting GGUFs from inside BatBot.
//
// Models are the biggest thing on the disk by a wide margin -- a handful of
// them is tens of gigabytes -- and up to now the only way to get rid of one was
// to leave the program, find the models directory and delete the file by hand.
// The list is already on screen in settings; this makes it actionable.
//
// Deleting is not undoable, so both routes go through a confirmation: one
// naming the file, and one for "delete all" naming how many and how much.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "batbot/llm/model_catalog.hpp"

namespace batbot::ui {

/// What the model panel wants from the application after a key.
enum class ModelManagerAction {
    None,
    Close,    ///< go back to the settings list
    Deleted,  ///< a file went; the caller should clear seats and save
};

class ModelManagerView {
public:
    /// Read `models_dir` and show what is in it. `in_use` names the files the
    /// config currently points at, so a row that is about to break a seat can
    /// say so before it is deleted rather than after.
    void open(std::filesystem::path models_dir, std::vector<std::string> in_use);
    void close() { open_ = false; }
    bool active() const { return open_; }

    ModelManagerAction handle(const ftxui::Event& event);
    ftxui::Element render() const;

    const std::string& status() const { return status_; }

    /// File names deleted since this was last called, and then forgotten.
    /// The caller uses them to empty any seat that pointed at one.
    std::vector<std::string> take_removed();

private:
    /// What a pending "Are you sure?" is about.
    enum class Pending {
        None,
        One,  ///< the selected file
        All,  ///< every file in the directory
    };

    void refresh();
    void confirm();

    /// Total bytes of everything listed, for the "delete all" confirmation.
    std::uintmax_t total_bytes() const;
    /// Is `name` assigned to a seat?
    bool in_use(const std::string& name) const;

    ftxui::Element render_model(std::size_t index) const;
    ftxui::Element render_confirm() const;

    std::filesystem::path    dir_;
    std::vector<ModelFile>   models_;
    std::vector<std::string> in_use_;
    std::vector<std::string> removed_;

    std::size_t selected_ = 0;
    bool        open_     = false;
    Pending     pending_  = Pending::None;
    std::string status_;
};

}  // namespace batbot::ui
