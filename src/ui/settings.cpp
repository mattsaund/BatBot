#include "batbot/ui/settings.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

#include "batbot/core/paths.hpp"
#include "batbot/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {
namespace {

/// Sentinel seat value meaning "the delegator", which is not one of the nine.
constexpr std::size_t kRouterSeat = kSubjectCount;

/// Step one codepoint left/right through a UTF-8 string. Moving by bytes would
/// drop the cursor into the middle of a multibyte character, and a path can
/// easily contain one.
std::size_t utf8_prev(const std::string& text, std::size_t index) {
    if (index == 0) {
        return 0;
    }
    --index;
    while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
        --index;
    }
    return index;
}

std::size_t utf8_next(const std::string& text, std::size_t index) {
    if (index >= text.size()) {
        return text.size();
    }
    ++index;
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
        ++index;
    }
    return index;
}

std::string format_float(float value) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.2f", static_cast<double>(value));
    return buffer.data();
}

}  // namespace

SettingsView::SettingsView(Config config) : config_(std::move(config)) {
    refresh();
}

void SettingsView::set_config(Config config) {
    config_ = std::move(config);
    dirty_  = false;
    refresh();
}

void SettingsView::refresh() {
    models_ = scan_models(config_.resolved_models_dir());
    build_rows();
    if (selected_ >= rows_.size()) {
        selected_ = 0;
    }
    // Never leave the cursor parked on a header, which cannot be edited.
    if (!rows_.empty() && rows_[selected_].kind == Kind::Header) {
        move_selection(1);
    }
}

void SettingsView::build_rows() {
    rows_.clear();

    rows_.push_back({Kind::Header, "MODELS", "", nullptr, nullptr, nullptr, nullptr, 0, {}});

    rows_.push_back({Kind::Directory, "Models directory",
                     "where BatBot looks for .gguf files", &config_.models_dir,
                     nullptr, nullptr, nullptr, 0, {}});

    rows_.push_back({Kind::Header, "DELEGATOR", "", nullptr, nullptr, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::ModelRef, "Router model",
                     "small model that picks the expert; blank falls back to keywords",
                     &config_.router.model, nullptr, nullptr, nullptr, kRouterSeat, {}});

    rows_.push_back({Kind::Header, "EXPERTS", "", nullptr, nullptr, nullptr, nullptr, 0, {}});
    for (const SubjectInfo& info : all_subjects()) {
        const auto seat = static_cast<std::size_t>(info.subject);
        rows_.push_back({Kind::ModelRef, std::string(info.name), std::string(info.blurb),
                         &config_.experts[seat].model, nullptr, nullptr, nullptr, seat, {}});
    }

    rows_.push_back({Kind::Header, "DEFAULTS", "", nullptr, nullptr, nullptr, nullptr, 0, {}});
    ModelParams& d = config_.defaults;
    rows_.push_back({Kind::Int,   "Context size",   "tokens of context per expert",
                     nullptr, &d.n_ctx, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "GPU layers",     "-1 offloads as much as fits",
                     nullptr, &d.n_gpu_layers, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Batch size",     "prompt ingestion batch",
                     nullptr, &d.n_batch, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Threads",        "0 picks automatically",
                     nullptr, &d.n_threads, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Max tokens",     "hard cap on a single reply",
                     nullptr, &d.max_tokens, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Temperature",    "0 is greedy and deterministic",
                     nullptr, nullptr, &d.temperature, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Top-p",          "nucleus sampling cutoff",
                     nullptr, nullptr, &d.top_p, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Top-k",          "candidates kept before sampling",
                     nullptr, &d.top_k, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Min-p",          "minimum relative probability",
                     nullptr, nullptr, &d.min_p, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Repeat penalty", "1.0 disables it",
                     nullptr, nullptr, &d.repeat_penalty, nullptr, 0, {}});
    rows_.push_back({Kind::Bool,  "Flash attention", "faster attention where supported",
                     nullptr, nullptr, nullptr, &d.flash_attn, 0, {}});
    rows_.push_back({Kind::Enum,  "GPU split mode", "how an expert spreads across GPUs",
                     &d.split_mode, nullptr, nullptr, nullptr, 0,
                     {"layer", "row", "tensor", "none"}});

    rows_.push_back({Kind::Header, "BEHAVIOUR", "", nullptr, nullptr, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Text, "System prompt", "sent to every expert",
                     &config_.system_prompt, nullptr, nullptr, nullptr, 0, {}});

    rows_.push_back({Kind::Header, "INTERFACE", "", nullptr, nullptr, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,  "Animation ms", "frame interval while busy",
                     nullptr, &config_.ui.animation_ms, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Bool, "Show roundtable", "draw the ring of experts",
                     nullptr, nullptr, nullptr, &config_.ui.show_roundtable, 0, {}});
    rows_.push_back({Kind::Bool, "Unicode glyphs", "off uses a pure-ASCII bat",
                     nullptr, nullptr, nullptr, &config_.ui.unicode, 0, {}});
}

std::string SettingsView::value_of(const Row& row) const {
    switch (row.kind) {
        case Kind::Header:   return {};
        case Kind::Directory:
            // Blank means "the default". Showing the blank would leave the user
            // with an empty field and nothing to edit, so resolve it.
            return config_.resolved_models_dir().string();
        case Kind::ModelRef:
        case Kind::Text:
        case Kind::Enum:     return row.text != nullptr ? *row.text : std::string{};
        case Kind::Int:      return row.integer != nullptr ? std::to_string(*row.integer) : std::string{};
        case Kind::Float:    return row.real != nullptr ? format_float(*row.real) : std::string{};
        case Kind::Bool:     return (row.flag != nullptr && *row.flag) ? "on" : "off";
    }
    return {};
}

void SettingsView::move_selection(int delta) {
    if (rows_.empty()) {
        return;
    }
    auto index = static_cast<long>(selected_);
    const auto count = static_cast<long>(rows_.size());
    for (long step = 0; step < count; ++step) {
        index += delta;
        if (index < 0) {
            index = count - 1;
        }
        if (index >= count) {
            index = 0;
        }
        if (rows_[static_cast<std::size_t>(index)].kind != Kind::Header) {
            selected_ = static_cast<std::size_t>(index);
            return;
        }
    }
}

void SettingsView::begin_edit() {
    const Row& row = rows_[selected_];
    if (row.kind == Kind::Header) {
        return;
    }

    switch (row.kind) {
        case Kind::Bool:
            // A checkbox needs no edit mode; Enter is the whole interaction.
            *row.flag = !*row.flag;
            dirty_    = true;
            return;
        case Kind::Enum: {
            const auto it = std::find(row.options.begin(), row.options.end(), *row.text);
            const std::size_t next = (it == row.options.end())
                ? 0
                : (static_cast<std::size_t>(std::distance(row.options.begin(), it)) + 1)
                      % row.options.size();
            *row.text = row.options[next];
            dirty_    = true;
            return;
        }
        case Kind::ModelRef:
            open_picker();
            return;
        case Kind::Directory:
            open_browser();
            return;
        default:
            break;
    }

    begin_typing();
}

void SettingsView::begin_typing() {
    const Row& row = rows_[selected_];
    if (row.kind == Kind::Header || row.kind == Kind::Bool || row.kind == Kind::Enum) {
        return;
    }
    editing_     = true;
    buffer_      = value_of(row);
    // Start at the end: the common edit is appending or trimming a path tail.
    edit_cursor_ = buffer_.size();
}

void SettingsView::commit_edit() {
    Row& row = rows_[selected_];
    editing_ = false;

    switch (row.kind) {
        case Kind::Directory:
        case Kind::Text:
            *row.text = buffer_;
            dirty_    = true;
            // Moving the models directory changes what the picker can offer.
            if (row.text == &config_.models_dir) {
                refresh();
            }
            break;
        case Kind::Int:
            try {
                *row.integer = std::stoi(buffer_);
                dirty_       = true;
            } catch (const std::exception&) {
                status_ = "'" + buffer_ + "' is not a whole number";
            }
            break;
        case Kind::Float:
            try {
                *row.real = std::stof(buffer_);
                dirty_    = true;
            } catch (const std::exception&) {
                status_ = "'" + buffer_ + "' is not a number";
            }
            break;
        default:
            break;
    }
    buffer_.clear();
}

void SettingsView::cancel_edit() {
    editing_ = false;
    buffer_.clear();
    edit_cursor_ = 0;
}

/// A small line editor. Paths are long, so this needs more than append and
/// backspace: a cursor, jumps to either end, and word-wise deletion.
bool SettingsView::handle_edit_key(const Event& event) {
    if (event == Event::ArrowLeft) {
        edit_cursor_ = utf8_prev(buffer_, edit_cursor_);
        return true;
    }
    if (event == Event::ArrowRight) {
        edit_cursor_ = utf8_next(buffer_, edit_cursor_);
        return true;
    }
    if (event == Event::Home || event == Event::CtrlA) {
        edit_cursor_ = 0;
        return true;
    }
    if (event == Event::End || event == Event::CtrlE) {
        edit_cursor_ = buffer_.size();
        return true;
    }
    if (event == Event::CtrlU) {
        buffer_.clear();
        edit_cursor_ = 0;
        return true;
    }
    if (event == Event::CtrlW) {
        // Delete the previous path component, which is the edit you want when
        // retyping the tail of a directory.
        std::size_t cut = edit_cursor_;
        while (cut > 0 && (buffer_[cut - 1] == '/' || buffer_[cut - 1] == ' ')) {
            --cut;
        }
        while (cut > 0 && buffer_[cut - 1] != '/' && buffer_[cut - 1] != ' ') {
            --cut;
        }
        buffer_.erase(cut, edit_cursor_ - cut);
        edit_cursor_ = cut;
        return true;
    }
    if (event == Event::Backspace) {
        if (edit_cursor_ > 0) {
            const std::size_t previous = utf8_prev(buffer_, edit_cursor_);
            buffer_.erase(previous, edit_cursor_ - previous);
            edit_cursor_ = previous;
        }
        return true;
    }
    if (event == Event::Delete) {
        if (edit_cursor_ < buffer_.size()) {
            buffer_.erase(edit_cursor_, utf8_next(buffer_, edit_cursor_) - edit_cursor_);
        }
        return true;
    }
    if (event.is_character()) {
        buffer_.insert(edit_cursor_, event.character());
        edit_cursor_ += event.character().size();
        return true;
    }
    return false;
}

void SettingsView::open_picker() {
    picking_       = true;
    picker_target_ = selected_;
    picker_index_  = 0;

    // Start the cursor on whatever this seat already uses, so re-opening the
    // picker shows the current choice rather than the top of the list.
    const std::string& current = *rows_[selected_].text;
    for (std::size_t i = 0; i < models_.size(); ++i) {
        if (models_[i].name == current) {
            picker_index_ = i + 1;  // slot 0 is "(none)"
            break;
        }
    }
}

void SettingsView::choose_model(std::size_t index) {
    Row& row = rows_[picker_target_];
    if (index == 0) {
        row.text->clear();
    } else if (index - 1 < models_.size()) {
        // Store the bare file name: the config stays portable if the models
        // directory later moves.
        *row.text = models_[index - 1].name;
    }
    dirty_   = true;
    picking_ = false;
}

void SettingsView::open_browser() {
    browse_target_ = selected_;
    browse_index_  = 0;
    browse_path_   = config_.resolved_models_dir();

    // Start somewhere that exists. A configured directory that has been deleted
    // or unplugged would otherwise open an empty browser with no way onward.
    std::error_code ec;
    while (!browse_path_.empty() && !std::filesystem::is_directory(browse_path_, ec)) {
        const std::filesystem::path parent = browse_path_.parent_path();
        if (parent == browse_path_) {
            break;
        }
        browse_path_ = parent;
    }
    if (browse_path_.empty() || !std::filesystem::is_directory(browse_path_, ec)) {
        browse_path_ = paths::expand_user("~");
    }

    browsing_ = true;
    refresh_browser();
}

void SettingsView::refresh_browser() {
    browse_entries_.clear();

    if (browse_path_.has_parent_path() && browse_path_.parent_path() != browse_path_) {
        browse_entries_.push_back({"..", browse_path_.parent_path(), 0, true});
    }

    std::error_code ec;
    std::vector<BrowseEntry> children;
    for (std::filesystem::directory_iterator it(browse_path_, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code entry_ec;
        if (!it->is_directory(entry_ec) || entry_ec) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;  // hidden directories are rarely where models live
        }
        children.push_back({name + "/", it->path(),
                            scan_models(it->path()).size(), false});
    }
    std::sort(children.begin(), children.end(),
              [](const BrowseEntry& a, const BrowseEntry& b) { return a.label < b.label; });

    browse_entries_.insert(browse_entries_.end(), children.begin(), children.end());
    if (browse_index_ >= browse_entries_.size() + 1) {
        browse_index_ = 0;
    }
}

void SettingsView::browse_up() {
    const std::filesystem::path parent = browse_path_.parent_path();
    if (!parent.empty() && parent != browse_path_) {
        browse_path_  = parent;
        browse_index_ = 0;
        refresh_browser();
    }
}

void SettingsView::browse_enter() {
    // Slot 0 is always "use this directory", so choosing is one keystroke from
    // wherever you have navigated to.
    if (browse_index_ == 0) {
        *rows_[browse_target_].text = browse_path_.string();
        dirty_    = true;
        browsing_ = false;
        refresh();
        status_ = "models directory set";
        return;
    }

    const std::size_t index = browse_index_ - 1;
    if (index >= browse_entries_.size()) {
        return;
    }
    browse_path_  = browse_entries_[index].path;
    browse_index_ = 0;
    refresh_browser();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

SettingsAction SettingsView::handle(const Event& event, bool& consumed) {
    consumed = true;

    // --- the directory browser owns the keyboard while it is open ----------
    if (browsing_) {
        const std::size_t count = browse_entries_.size() + 1;  // + "use this directory"
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            browse_index_ = (browse_index_ + count - 1) % count;
            return SettingsAction::None;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            browse_index_ = (browse_index_ + 1) % count;
            return SettingsAction::None;
        }
        if (event == Event::Return) {
            browse_enter();
            return SettingsAction::None;
        }
        if (event == Event::ArrowLeft || event == Event::Backspace
            || event == Event::Character('h')) {
            browse_up();
            return SettingsAction::None;
        }
        if (event == Event::Character('~')) {
            browse_path_  = paths::expand_user("~");
            browse_index_ = 0;
            refresh_browser();
            return SettingsAction::None;
        }
        if (event == Event::Character('e')) {
            // Escape hatch for paths that are easier typed than navigated to,
            // such as a network mount that is not under home.
            browsing_ = false;
            editing_  = true;
            buffer_   = browse_path_.string();
            return SettingsAction::None;
        }
        if (event == Event::Escape) {
            browsing_ = false;
            return SettingsAction::None;
        }
        return SettingsAction::None;
    }

    // --- the model picker owns the keyboard while it is open ---------------
    if (picking_) {
        const std::size_t count = models_.size() + 1;  // + "(none)"
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            picker_index_ = (picker_index_ + count - 1) % count;
            return SettingsAction::None;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            picker_index_ = (picker_index_ + 1) % count;
            return SettingsAction::None;
        }
        if (event == Event::Return) {
            choose_model(picker_index_);
            return SettingsAction::None;
        }
        if (event == Event::Escape) {
            picking_ = false;
            return SettingsAction::None;
        }
        return SettingsAction::None;
    }

    // --- inline text editing ------------------------------------------------
    if (editing_) {
        if (event == Event::Return) {
            commit_edit();
            return SettingsAction::None;
        }
        if (event == Event::Escape) {
            cancel_edit();
            return SettingsAction::None;
        }
        handle_edit_key(event);
        return SettingsAction::None;
    }

    // --- navigation ---------------------------------------------------------
    if (event == Event::ArrowUp   || event == Event::Character('k')) { move_selection(-1); return SettingsAction::None; }
    if (event == Event::ArrowDown || event == Event::Character('j')) { move_selection(1);  return SettingsAction::None; }
    if (event == Event::Return || event == Event::Character(' ')) {
        begin_edit();
        return SettingsAction::None;
    }
    if (event == Event::Character('e')) {
        // Typing a path outright, without going through the browser: the right
        // move for a network mount or anything easier pasted than navigated to.
        begin_typing();
        return SettingsAction::None;
    }
    if (event == Event::Character('r')) {
        refresh();
        status_ = "rescanned " + config_.resolved_models_dir().string();
        return SettingsAction::None;
    }
    if (event == Event::CtrlS) {
        return SettingsAction::Apply;
    }
    if (event == Event::Escape) {
        return SettingsAction::Close;
    }

    consumed = false;
    return SettingsAction::None;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

Element SettingsView::render_row(const Row& row, std::size_t index) const {
    if (row.kind == Kind::Header) {
        return vbox({
            text(" "),
            text("  " + row.label) | color(theme::kAccent) | bold,
        });
    }

    const bool selected = index == selected_;
    const bool editing  = selected && editing_;

    if (editing) {
        // Draw the cursor where it is, not always at the end, and keep the tail
        // of a long path in view rather than the head -- when you are fixing a
        // directory it is the last component you care about.
        const std::size_t cut = std::min(edit_cursor_, buffer_.size());
        const std::string before = buffer_.substr(0, cut);
        const std::string under  = cut < buffer_.size()
            ? buffer_.substr(cut, utf8_next(buffer_, cut) - cut)
            : std::string(" ");
        const std::string after  = cut < buffer_.size()
            ? buffer_.substr(utf8_next(buffer_, cut))
            : std::string();

        return hbox({
            text(" > ") | color(theme::kAccent) | bold,
            text(row.label) | bold | size(WIDTH, EQUAL, 20),
            hbox({
                text(before),
                text(under) | inverted,
                text(after),
            }) | xframe | ftxui::focus | flex,
        }) | bgcolor(Color::GrayDark);
    }

    std::string value = value_of(row);
    Color value_color  = theme::kUser;

    if (row.kind == Kind::Directory) {
        std::error_code ec;
        if (!std::filesystem::is_directory(value, ec)) {
            value += "  (does not exist)";
            value_color = theme::kError;
        }
    } else if (row.kind == Kind::ModelRef) {
        if (value.empty()) {
            value       = "(none)";
            value_color = theme::kMeta;
        } else {
            // A seat pointing at a file that is not there is the single most
            // useful thing this screen can tell you, so say it inline.
            const std::filesystem::path resolved =
                resolve_model_ref(config_.resolved_models_dir(), value);
            if (!std::filesystem::exists(resolved)) {
                value += "  (missing)";
                value_color = theme::kError;
            } else {
                value_color = theme::kSeatActive;
            }
        }
    } else if (row.kind == Kind::Bool) {
        value_color = (row.flag != nullptr && *row.flag) ? theme::kSeatActive : theme::kMeta;
    }

    Element label = text(row.label);
    Element line  = hbox({
        text(selected ? " > " : "   ") | color(theme::kAccent) | bold,
        (selected ? label | bold : label) | size(WIDTH, EQUAL, 20),
        text(value) | color(value_color) | flex,
    });

    if (selected) {
        line = line | bgcolor(Color::GrayDark);
    }
    return line;
}

Element SettingsView::render_picker() const {
    Elements items;
    const std::string title = rows_[picker_target_].label;

    const auto entry = [&](std::size_t slot, const std::string& label, const std::string& note) {
        const bool on = slot == picker_index_;
        Element line = hbox({
            text(on ? " > " : "   ") | color(theme::kAccent),
            text(label) | flex,
            // An explicit gap: flex collapses to nothing when the file name is
            // long, which would run the name straight into its size.
            text("  "),
            text(note) | color(theme::kMeta) | dim,
        });
        if (!on) {
            return line;
        }
        // focus is what makes the enclosing yframe scroll. Without it the
        // selection simply walks off the bottom of a long list.
        return line | bgcolor(Color::GrayDark) | bold | ftxui::focus;
    };

    items.push_back(entry(0, "(none)", "leave this seat empty"));
    for (std::size_t i = 0; i < models_.size(); ++i) {
        items.push_back(entry(i + 1, models_[i].name, models_[i].size_label() + " "));
    }

    if (models_.empty()) {
        items.push_back(text(" "));
        items.push_back(text("  No .gguf files in " + config_.resolved_models_dir().string())
                        | color(theme::kNotice));
        items.push_back(text("  Put models there, then press r to rescan.")
                        | color(theme::kMeta) | dim);
    }

    // The explicit background matters: dbox composites this over the settings
    // list, and any cell the dialog does not paint lets the text underneath
    // show through -- which reads as corrupted file names.
    return window(text(" Choose a model for " + title + " ") | bold | color(theme::kAccent),
                  vbox({
                      vbox(std::move(items)) | yframe | flex,
                      separator(),
                      text(" ↑↓ move   enter choose   esc cancel ") | color(theme::kMeta) | dim,
                  }))
        | size(WIDTH, LESS_THAN, 76) | size(HEIGHT, LESS_THAN, 22)
        | color(Color::GrayLight) | bgcolor(Color::Black) | clear_under | center;
}

Element SettingsView::render_browser() const {
    Elements items;

    const std::size_t here = scan_models(browse_path_).size();
    const auto line = [&](std::size_t slot, const std::string& label,
                          const std::string& note, Color tint) {
        const bool on = slot == browse_index_;
        Element row = hbox({
            text(on ? " > " : "   ") | color(theme::kAccent),
            text(label) | color(tint) | flex,
            text("  "),
            text(note) | color(theme::kMeta) | dim,
        });
        if (!on) {
            return row;
        }
        // As in the picker: focus drives the yframe's scroll position.
        return row | bgcolor(Color::GrayDark) | bold | ftxui::focus;
    };

    items.push_back(line(0, "[ use this directory ]",
                         here == 0 ? "no models here"
                                   : std::to_string(here) + (here == 1 ? " model" : " models"),
                         here > 0 ? theme::kSeatActive : theme::kNotice));
    items.push_back(separator());

    for (std::size_t i = 0; i < browse_entries_.size(); ++i) {
        const BrowseEntry& entry = browse_entries_[i];
        // The model count is the whole point of browsing: it tells you which
        // folder is the one you meant without opening each in turn.
        const std::string note = entry.is_parent || entry.models == 0
            ? std::string{}
            : std::to_string(entry.models) + (entry.models == 1 ? " model" : " models");
        items.push_back(line(i + 1, entry.label, note,
                             entry.models > 0 ? theme::kSeatActive : theme::kUser));
    }

    if (browse_entries_.empty()) {
        items.push_back(text("   (no subdirectories)") | color(theme::kMeta) | dim);
    }

    return window(text(" Models directory ") | bold | color(theme::kAccent),
                  vbox({
                      text(" " + browse_path_.string()) | color(theme::kMeta),
                      separator(),
                      vbox(std::move(items)) | yframe | flex,
                      separator(),
                      text(" ↑↓ move   enter open/choose   ← up   ~ home   e type   esc cancel ")
                          | color(theme::kMeta) | dim,
                  }))
        | size(WIDTH, LESS_THAN, 80) | size(HEIGHT, LESS_THAN, 24)
        | color(Color::GrayLight) | bgcolor(Color::Black) | clear_under | center;
}

Element SettingsView::render() const {
    Elements lines;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        Element row = render_row(rows_[i], i);
        lines.push_back(i == selected_ ? row | ftxui::focus : row);
    }

    const std::string dir   = config_.resolved_models_dir().string();
    const std::string count = std::to_string(models_.size()) + " model"
                            + (models_.size() == 1 ? "" : "s") + " found";

    Element header = hbox({
        text("  " + dir) | color(theme::kMeta),
        filler(),
        text(count + "  ") | color(theme::kMeta) | dim,
    });

    std::string hint = " ↑↓ move   enter edit   e type   r rescan   ctrl-s save   esc back ";
    if (!rows_.empty() && selected_ < rows_.size()) {
        switch (rows_[selected_].kind) {
            case Kind::Directory:
                hint = " enter browse folders   e type a path   ctrl-s save   esc back ";
                break;
            case Kind::ModelRef:
                hint = " enter choose a model   r rescan   ctrl-s save   esc back ";
                break;
            default:
                break;
        }
    }
    if (editing_) {
        hint = " ←→ move   ctrl-a/e ends   ctrl-w del word   ctrl-u clear   enter save   esc cancel ";
    }

    Elements footer{text(hint) | color(theme::kMeta) | dim};
    if (dirty_) {
        footer.push_back(filler());
        footer.push_back(text("unsaved changes  ") | color(theme::kNotice) | bold);
    }
    if (!status_.empty()) {
        footer.push_back(filler());
        footer.push_back(text(status_ + "  ") | color(theme::kSeatActive));
    }

    Element body = vbox({
        header,
        separator(),
        vbox(std::move(lines)) | yframe | flex,
        separator(),
        hbox(std::move(footer)),
    });

    Element screen = window(text(" Settings ") | bold | color(theme::kBat), body);

    if (picking_) {
        // dbox layers the dialog over the settings list rather than replacing
        // it, so the row being changed stays visible behind it.
        return dbox({screen, render_picker()});
    }
    if (browsing_) {
        return dbox({screen, render_browser()});
    }
    return screen;
}

}  // namespace batbot::ui
