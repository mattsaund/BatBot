// The in-app settings screen.
//
// Everything in config.json is editable here, so a working BatBot never
// requires dropping out to a text editor: assign a model to each expert seat
// and to the delegator, move the models directory, and tune sampling.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "batbot/core/config.hpp"
#include "batbot/core/model_catalog.hpp"

namespace batbot::ui {

/// What the settings screen wants the app to do after handling a key.
enum class SettingsAction {
    None,
    Close,       ///< leave the settings screen
    Apply,       ///< config changed: save it and hand it to the engine
};

class SettingsView {
public:
    explicit SettingsView(Config config);

    /// Re-read the models directory and rebuild the row list. Call on open, and
    /// after the models directory changes.
    void refresh();

    /// Take the edited configuration.
    const Config& config() const { return config_; }
    void set_config(Config config);

    /// True when there are unsaved edits.
    bool dirty() const { return dirty_; }
    void mark_saved() { dirty_ = false; }

    ftxui::Element render() const;

    /// Returns what the app should do next. Consumes the event when it is one
    /// the settings screen uses.
    SettingsAction handle(const ftxui::Event& event, bool& consumed);

    /// A message to show after an action, e.g. a save confirmation.
    const std::string& status() const { return status_; }
    void set_status(std::string status) { status_ = std::move(status); }

private:
    /// One editable line.
    enum class Kind {
        Header,     ///< not selectable
        ModelRef,   ///< opens the model picker
        Directory,  ///< opens the directory browser
        Text,
        Int,
        Float,
        Bool,
        Enum,
    };

    struct Row {
        Kind        kind = Kind::Header;
        std::string label;
        std::string help;
        /// Where the value lives. Exactly one of these is used, chosen by kind.
        std::string* text  = nullptr;
        int*         integer = nullptr;
        float*       real  = nullptr;
        bool*        flag  = nullptr;
        /// For ModelRef rows: which seat this is (kSubjectCount means router).
        std::size_t  seat  = 0;
        /// For Enum rows.
        std::vector<std::string> options;
    };

    void build_rows();
    void move_selection(int delta);
    void begin_edit();
    void begin_typing();            ///< edit the selected row's value as text
    void commit_edit();
    void cancel_edit();
    bool handle_edit_key(const ftxui::Event& event);
    void open_picker();
    void choose_model(std::size_t index);

    void open_browser();
    void refresh_browser();
    void browse_enter();
    void browse_up();

    std::string value_of(const Row& row) const;

    ftxui::Element render_row(const Row& row, std::size_t index) const;
    ftxui::Element render_picker() const;
    ftxui::Element render_browser() const;

    Config                 config_;
    std::vector<Row>       rows_;
    std::vector<ModelFile> models_;

    std::size_t selected_ = 0;
    bool        dirty_    = false;

    bool        editing_ = false;
    std::string buffer_;
    std::size_t edit_cursor_ = 0;   ///< byte offset into buffer_, on a codepoint boundary

    bool        picking_       = false;
    std::size_t picker_index_  = 0;
    std::size_t picker_target_ = 0;   ///< row index the picker is filling

    /// One candidate directory in the browser, with the count of GGUFs inside
    /// it -- which is the thing that tells you whether it is the folder you
    /// meant.
    struct BrowseEntry {
        std::string           label;
        std::filesystem::path path;
        std::size_t           models = 0;
        bool                  is_parent = false;
    };

    bool                     browsing_ = false;
    std::filesystem::path    browse_path_;
    std::vector<BrowseEntry> browse_entries_;
    std::size_t              browse_index_  = 0;
    std::size_t              browse_target_ = 0;

    std::string status_;
};

}  // namespace batbot::ui
