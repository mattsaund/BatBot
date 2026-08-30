// SPDX-License-Identifier: MIT
//
// The Manage models panel. See model_manager_view.hpp for what it is for.
#include "batbot/ui/settings/model_manager_view.hpp"

#include <algorithm>
#include <numeric>
#include <system_error>

#include "batbot/ui/theme.hpp"
#include "batbot/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {

void ModelManagerView::open(std::filesystem::path models_dir, std::vector<std::string> in_use) {
    dir_    = std::move(models_dir);
    in_use_ = std::move(in_use);
    selected_ = 0;
    pending_  = Pending::None;
    status_.clear();
    removed_.clear();
    open_ = true;
    refresh();
}

void ModelManagerView::refresh() {
    models_ = scan_models(dir_);
    if (selected_ >= models_.size() && !models_.empty()) {
        selected_ = models_.size() - 1;
    }
}

std::vector<std::string> ModelManagerView::take_removed() {
    std::vector<std::string> taken;
    taken.swap(removed_);
    return taken;
}

std::uintmax_t ModelManagerView::total_bytes() const {
    return std::accumulate(models_.begin(), models_.end(), std::uintmax_t{0},
                           [](std::uintmax_t sum, const ModelFile& file) {
                               return sum + file.bytes;
                           });
}

bool ModelManagerView::in_use(const std::string& name) const {
    return std::find(in_use_.begin(), in_use_.end(), name) != in_use_.end();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

ModelManagerAction ModelManagerView::handle(const Event& event) {
    if (!open_) {
        return ModelManagerAction::None;
    }

    // A confirmation owns the keyboard while it is up, and only the keys it
    // names do anything. Treating every other event as "no" would be tidier to
    // write and quite wrong: the animation clock posts a custom event several
    // times a second, so the question would answer itself before it could be
    // read. Arrow keys are swallowed rather than passed through, since moving
    // the cursor under an open "delete this?" would leave it pointing at a
    // different file than the one being asked about.
    if (pending_ != Pending::None) {
        if (event == Event::Character('y') || event == Event::Character('Y') ||
            event == Event::Return) {
            confirm();
            return ModelManagerAction::Deleted;
        }
        if (event == Event::Character('n') || event == Event::Character('N') ||
            event == Event::Escape || event == Event::Character('q')) {
            pending_ = Pending::None;
            status_  = "nothing deleted";
        }
        return ModelManagerAction::None;
    }

    if (event == Event::Escape || event == Event::Character('q')) {
        open_ = false;
        return ModelManagerAction::Close;
    }

    // One row past the last file is the "delete all" button.
    const std::size_t rows = models_.size() + (models_.empty() ? 0 : 1);

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (selected_ > 0) {
            --selected_;
        }
        return ModelManagerAction::None;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (selected_ + 1 < rows) {
            ++selected_;
        }
        return ModelManagerAction::None;
    }

    if (event == Event::Character('r')) {
        refresh();
        status_ = "rescanned";
        return ModelManagerAction::None;
    }

    if (event == Event::Return || event == Event::Character('d') ||
        event == Event::Delete) {
        if (models_.empty()) {
            return ModelManagerAction::None;
        }
        pending_ = selected_ >= models_.size() ? Pending::All : Pending::One;
        status_.clear();
        return ModelManagerAction::None;
    }

    return ModelManagerAction::None;
}

void ModelManagerView::confirm() {
    const Pending what = pending_;
    pending_ = Pending::None;

    std::vector<ModelFile> targets;
    if (what == Pending::All) {
        targets = models_;
    } else if (selected_ < models_.size()) {
        targets.push_back(models_[selected_]);
    }

    int         deleted = 0;
    std::string failure;
    for (const ModelFile& file : targets) {
        std::error_code ec;
        std::filesystem::remove(file.path, ec);
        if (ec) {
            // Report the first failure and carry on with the rest: a "delete
            // all" that stops at a read-only file would leave the user unable
            // to tell what did go.
            if (failure.empty()) {
                failure = file.name + ": " + ec.message();
            }
            continue;
        }
        removed_.push_back(file.name);
        ++deleted;
    }

    refresh();
    if (selected_ >= models_.size()) {
        selected_ = models_.empty() ? 0 : models_.size();
    }

    if (deleted == 0) {
        status_ = failure.empty() ? "nothing was deleted" : "could not delete " + failure;
        return;
    }
    status_ = "deleted " + std::to_string(deleted) +
              (deleted == 1 ? " model" : " models");
    if (!failure.empty()) {
        status_ += ", but could not delete " + failure;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

Element ModelManagerView::render_model(std::size_t index) const {
    const bool selected = index == selected_;

    if (index >= models_.size()) {
        // The delete-all button, kept as the last row rather than behind a key
        // of its own: nobody presses shift-D to find out what it does, but
        // everybody reads a list to the bottom. Red whether or not it is
        // selected, because it is the one control here that cannot be undone.
        Element line = hbox({
            text(selected ? " ▸ " : "   "),
            text("Delete all models") | bold | color(theme::kError),
            text("  ·  " + std::to_string(models_.size()) + " files, " +
                 format::bytes(total_bytes())) |
                color(meta_color(selected)),
        });
        return selected ? line | bgcolor(theme::kHighlight) | ftxui::focus : line;
    }

    const ModelFile& file = models_[index];
    Elements row{
        text(selected ? " ▸ " : "   "),
        text(file.name) | flex,
        text("  "),
        text(file.size_label()) | color(meta_color(selected)),
    };
    if (in_use(file.name)) {
        // Worth saying before the delete rather than after: this is the file
        // whose removal empties a seat.
        row.push_back(text("  in use") | color(theme::kSeatActive));
    } else {
        row.push_back(text("        "));
    }

    Element line = hbox(std::move(row));
    return selected ? line | bgcolor(theme::kHighlight) | ftxui::focus : line;
}

Element ModelManagerView::render_confirm() const {
    Elements lines;
    lines.push_back(text(" Are you sure? ") | bold | color(theme::kError));
    lines.push_back(separator());

    if (pending_ == Pending::All) {
        lines.push_back(text(" Delete all " + std::to_string(models_.size()) +
                             " models, " + format::bytes(total_bytes()) + " in total?"));
        const auto used = static_cast<std::size_t>(
            std::count_if(models_.begin(), models_.end(), [this](const ModelFile& file) {
                return in_use(file.name);
            }));
        if (used > 0) {
            lines.push_back(text(" " + std::to_string(used) +
                                 (used == 1 ? " of them is assigned to a seat, which "
                                              "will be emptied."
                                            : " of them are assigned to seats, which "
                                              "will be emptied.")) |
                            color(theme::kNotice));
        }
    } else if (selected_ < models_.size()) {
        const ModelFile& file = models_[selected_];
        lines.push_back(text(" Delete " + file.name + "?"));
        lines.push_back(text(" " + file.size_label() + " · " + file.path.string()) |
                        color(theme::kMeta) | dim);
        if (in_use(file.name)) {
            lines.push_back(text(" It is assigned to a seat, which will be emptied.") |
                            color(theme::kNotice));
        }
    }

    lines.push_back(separator());
    lines.push_back(text(" This cannot be undone -- the file is not moved to a "
                         "wastebasket.") |
                    color(theme::kMeta));
    lines.push_back(text(" y or enter deletes   ·   n or esc cancels") |
                    color(theme::kMeta) | dim);

    return vbox(std::move(lines)) | border | size(WIDTH, LESS_THAN, 72) |
           color(Color::GrayLight) | bgcolor(Color::Black) | clear_under | center;
}

Element ModelManagerView::render() const {
    Elements body{
        hbox({
            // Pinned title, shrinkable path: a models directory can be
            // arbitrarily long, and when something has to give it should be
            // the part that repeats what is already on the settings screen
            // behind this panel, not the word saying what the panel is.
            text(" models ") | bold | color(theme::kBat) | size(WIDTH, EQUAL, 9),
            text("· " + format::short_path(dir_)) | color(theme::kMeta) | flex_shrink,
        }),
        separator(),
    };

    if (models_.empty()) {
        body.push_back(text("   no .gguf files here") | color(theme::kMeta));
        body.push_back(text("   put some in and press r") | color(theme::kMeta) | dim);
    } else {
        Elements rows;
        for (std::size_t i = 0; i <= models_.size(); ++i) {
            rows.push_back(render_model(i));
        }
        body.push_back(vbox(std::move(rows)) | yframe | flex);
        body.push_back(separator());
        body.push_back(text(" " + std::to_string(models_.size()) +
                            (models_.size() == 1 ? " model · " : " models · ") +
                            format::bytes(total_bytes())) |
                       color(theme::kMeta) | dim);
    }

    if (!status_.empty()) {
        body.push_back(separator());
        body.push_back(paragraph(status_) | color(theme::kNotice));
    }

    body.push_back(separator());
    body.push_back(text(" ↑↓ choose   enter or d delete   r rescan   esc back") |
                   color(theme::kMeta) | dim);

    Element screen = vbox(std::move(body)) | border | size(WIDTH, LESS_THAN, 84) |
                     size(HEIGHT, LESS_THAN, 26) | bgcolor(Color::Black) | clear_under;

    if (pending_ != Pending::None) {
        return dbox({std::move(screen), render_confirm()});
    }
    return screen;
}

}  // namespace batbot::ui
