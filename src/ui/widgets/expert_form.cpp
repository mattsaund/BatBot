// SPDX-License-Identifier: MIT
#include "crucible/ui/widgets/expert_form.hpp"

#include "crucible/ui/theme.hpp"
#include "crucible/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {

void ExpertForm::open() {
    active_           = true;
    editing_existing_ = false;
    field_            = Field::Name;
    error_.clear();
    name_.begin({});
    blurb_.begin({});
}

void ExpertForm::open(const Expert& expert) {
    open();
    editing_existing_ = true;
    name_.begin(expert.name);
    blurb_.begin(expert.blurb);
}

void ExpertForm::close() {
    active_ = false;
    name_.cancel();
    blurb_.cancel();
    error_.clear();
}

void ExpertForm::set_error(std::string message) {
    error_ = std::move(message);
}

std::optional<ExpertForm::Result> ExpertForm::handle(const Event& event) {
    if (event == Event::Escape) {
        close();
        return std::nullopt;
    }

    if (event == Event::Tab || event == Event::ArrowDown) {
        field_ = Field::Blurb;
        return std::nullopt;
    }
    if (event == Event::TabReverse || event == Event::ArrowUp) {
        field_ = Field::Name;
        return std::nullopt;
    }

    if (event == Event::Return) {
        // Enter in the name box moves on rather than submitting. Pressing it
        // after typing a name is what everyone does, and treating that as
        // "done" would submit a form missing the one field that has to be
        // there.
        if (field_ == Field::Name) {
            field_ = Field::Blurb;
            return std::nullopt;
        }
        Result result{format::trim(name_.value()), format::trim(blurb_.value())};
        if (result.name.empty()) {
            error_ = "an expert needs a name";
            field_ = Field::Name;
            return std::nullopt;
        }
        if (result.blurb.empty()) {
            // Refused here rather than in the roster, so the cursor is already
            // in the box that needs filling.
            error_ = "describe what it is trained in -- that is what the delegator routes on";
            return std::nullopt;
        }
        // Left open. The caller closes it on success and calls set_error() on
        // failure, which is what keeps a rejected name editable instead of
        // making the whole form be typed again.
        error_.clear();
        return result;
    }

    // Anything else is text. The arrows are taken above, so the editor only
    // ever sees printable input, backspace and its own line shortcuts.
    LineEditor& editor = field_ == Field::Name ? name_ : blurb_;
    editor.handle(event);
    return std::nullopt;
}

Element ExpertForm::render() const {
    const auto box = [](const std::string& label, const std::string& hint,
                        const LineEditor& editor, bool focused) {
        return vbox({
            hbox({
                text(focused ? " > " : "   ") | color(theme::kAccent),
                text(label) | bold | color(focused ? theme::kAccent : theme::kPanelText),
            }),
            hbox({
                text("   "),
                editor.render() | flex,
            }) | (focused ? bgcolor(theme::kHighlight) : nothing),
            hbox({text("   "), text(hint) | color(theme::kMeta) | dim}),
        });
    };

    Elements body{
        text(" "),
        box("Expert name", "e.g. Rust Async, Tax Law, Kubernetes",
            name_, field_ == Field::Name),
        text(" "),
        box("Describe what the expert is trained in",
            "the delegator routes on this, so name the things it should take",
            blurb_, field_ == Field::Blurb),
        text(" "),
    };

    if (!error_.empty()) {
        body.push_back(hbox({text("   "), text(error_) | color(theme::kError) | bold}));
        body.push_back(text(" "));
    }

    return window(
        text(editing_existing_ ? " Edit expert " : " New expert ") | bold | color(theme::kAccent),
        vbox({
            vbox(std::move(body)) | flex,
            separator(),
            text(" tab/↑↓ switch box   enter next, then add   esc cancel ")
                | color(theme::kMeta) | dim,
        }))
        | size(WIDTH, LESS_THAN, 76) | size(HEIGHT, LESS_THAN, 18)
        | color(theme::kPanelText) | bgcolor(theme::kPanel) | clear_under | center;
}

}  // namespace crucible::ui
