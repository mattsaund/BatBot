// SPDX-License-Identifier: MIT
//
// The settings screen.
//
// Everything in config.json is editable here, so a working BatBot never
// requires dropping out to a text editor: assign a model to each expert seat
// and to the delegator, move the models directory, and tune sampling.
//
// This type owns the list of rows and decides what each one means. The two
// dialogs and the text editor are separate widgets, so the logic here stays
// about "what is editable" rather than "how a file picker behaves".
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "batbot/config/config.hpp"
#include "batbot/llm/model_catalog.hpp"
#include "batbot/ui/settings/directory_browser.hpp"
#include "batbot/ui/settings/line_editor.hpp"
#include "batbot/ui/settings/model_picker.hpp"

namespace batbot::ui {

/// What the settings screen wants the application to do after a key.
enum class SettingsAction {
    None,
    Close,          ///< leave the settings screen
    Apply,          ///< configuration changed: save it and hand it to the engine
    OpenRuntimes,   ///< hand over to the runtimes panel
};

class SettingsView {
public:
    explicit SettingsView(Config config);

    /// Re-read the models directory and rebuild the rows. Call on open, and
    /// whenever the models directory changes.
    void refresh();

    const Config& config() const { return config_; }
    void          set_config(Config config);

    bool dirty() const { return dirty_; }
    void mark_saved() { dirty_ = false; }

    ftxui::Element render() const;

    /// Apply one key. `consumed` reports whether the screen used it, so the
    /// application can fall through to its own bindings when it did not.
    SettingsAction handle(const ftxui::Event& event, bool& consumed);

    const std::string& status() const { return status_; }
    void set_status(std::string status) { status_ = std::move(status); }

private:
    /// What a row edits, which decides what Enter does to it.
    enum class Kind {
        Header,     ///< a section label; never selectable
        ModelRef,   ///< opens the model picker
        Directory,  ///< opens the directory browser
        Text,
        Int,
        Float,
        Bool,       ///< Enter toggles; there is nothing to type
        Enum,       ///< Enter cycles through `options`
        Panel,      ///< Enter hands off to a screen of its own
        Action,     ///< Enter does something once; there is no value to edit
    };

    /// What an Action row does. Named rather than matched on the label, so
    /// rewording a row cannot quietly break it.
    enum class ActionId {
        None,
        ResetModelsDir,
    };

    /// One line of the screen.
    ///
    /// A row points straight at the field it edits inside `config_`, so
    /// committing an edit is an assignment rather than a lookup. Exactly one
    /// pointer is non-null, chosen by `kind`.
    struct Row {
        Kind         kind = Kind::Header;
        std::string  label;
        std::string  help;
        std::string* text    = nullptr;
        int*         integer = nullptr;
        float*       real    = nullptr;
        bool*        flag    = nullptr;
        /// For ModelRef rows: which seat, as an index into Config::experts.
        /// kSubjectCount means the delegator.
        std::size_t  seat = 0;
        std::vector<std::string> options;  ///< for Enum rows
        ActionId     action = ActionId::None;  ///< for Action rows
    };

    void build_rows();
    void move_selection(int delta);

    /// The GPU priority order is a vector of device indices in the config and a
    /// comma-separated string in the editor, so the two are synced explicitly
    /// rather than pointing a Row at something that is not a std::string.
    void  gpu_priority_to_text();
    void  gpu_priority_from_text(const std::string& text);
    /// Device names behind the configured order, for the row's help line.
    std::string gpu_priority_help() const;

    /// Enter on the selected row: toggle, cycle, open a dialog, or start typing.
    void activate_selection();
    void run_action(ActionId action);
    void begin_typing();
    void commit_edit();

    std::string value_of(const Row& row) const;

    ftxui::Element render_row(const Row& row, std::size_t index) const;
    ftxui::Element footer_hint() const;

    Config                 config_;
    std::vector<Row>       rows_;
    std::vector<ModelFile> models_;

    std::size_t selected_ = 0;
    bool        dirty_    = false;
    std::string status_;

    /// Mirror of Config::gpu.priority for the line editor. See the sync pair.
    std::string      gpu_priority_text_;

    LineEditor       editor_;
    ModelPicker      picker_;
    DirectoryBrowser browser_;
    /// Which row the open dialog is filling.
    std::size_t      dialog_target_ = 0;
};

}  // namespace batbot::ui
