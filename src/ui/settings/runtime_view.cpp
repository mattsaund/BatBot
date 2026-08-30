// SPDX-License-Identifier: MIT
//
// The Runtimes panel. See runtime_view.hpp for what it is for.
#include "batbot/ui/settings/runtime_view.hpp"

#include <algorithm>

#include "batbot/config/paths.hpp"
#include "batbot/ui/theme.hpp"
#include "batbot/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {
namespace {

/// A short phrase for the state a runtime is in, and the colour to say it in.
std::pair<std::string, Color> state_of(const RuntimeStatus& runtime) {
    if (runtime.active) {
        const std::string devices = std::to_string(runtime.device_count) +
                                    (runtime.device_count == 1 ? " device" : " devices");
        return {"active · " + devices, Color(theme::kSeatActive)};
    }
    if (runtime.installed) {
        // Installed but contributing nothing: the module is there and either
        // has not been picked up yet or found no hardware to drive.
        return {"installed · restart to load", Color(theme::kSeatDormant)};
    }
    if (!runtime.buildable) {
        return {"not installed · " + runtime.blocker, Color(theme::kMeta)};
    }
    return {"not installed", Color(theme::kMeta)};
}

}  // namespace

RuntimeView::RuntimeView(std::function<void()> wake) : wake_(std::move(wake)) {}

void RuntimeView::refresh() {
    runtimes_ = RuntimeRegistry::scan();
    devices_  = compute_devices();
    if (selected_ >= runtimes_.size()) {
        selected_ = runtimes_.empty() ? 0 : runtimes_.size() - 1;
    }
}

bool RuntimeView::building() const {
    return builder_.progress().running();
}

void RuntimeView::shutdown() {
    builder_.stop();
}

void RuntimeView::start_install() {
    if (selected_ >= runtimes_.size()) {
        return;
    }
    const RuntimeStatus& runtime = runtimes_[selected_];
    const BackendInfo&   info    = backend_info(runtime.kind);

    if (!RuntimeRegistry::loadable_backends_supported()) {
        status_ = "this build has its backend compiled in; runtimes cannot be added";
        return;
    }
    if (!runtime.buildable) {
        status_ = std::string(info.name) + ": " + runtime.blocker +
                  ".  sudo apt install " + std::string(info.apt_packages);
        return;
    }

    if (!builder_.start(runtime.kind, wake_)) {
        status_ = "a runtime is already being built";
        return;
    }
    status_ = "building the " + std::string(info.name) +
              " runtime -- this takes a few minutes and you can keep using BatBot";
}

void RuntimeView::start_remove() {
    if (selected_ >= runtimes_.size()) {
        return;
    }
    const RuntimeStatus& runtime = runtimes_[selected_];

    std::string error;
    if (RuntimeRegistry::remove(runtime.kind, error)) {
        status_ = std::string(backend_info(runtime.kind).name) +
                  " removed -- it stops being used when BatBot restarts";
        refresh();
    } else {
        status_ = error;
    }
}

RuntimeAction RuntimeView::handle(const Event& event) {
    if (!open_) {
        return RuntimeAction::None;
    }

    const BuildProgress build = builder_.progress();

    if (event == Event::Escape || event == Event::Character('q')) {
        if (build.running()) {
            // Leaving the panel does not stop the build -- it runs on its own
            // thread and the settings screen reports it when you come back.
            status_ = "the build continues in the background";
        }
        open_ = false;
        return RuntimeAction::Close;
    }

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (selected_ > 0) { --selected_; }
        confirming_remove_ = false;
        return RuntimeAction::None;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (selected_ + 1 < runtimes_.size()) { ++selected_; }
        confirming_remove_ = false;
        return RuntimeAction::None;
    }

    if (event == Event::Character('r')) {
        refresh();
        status_ = "rescanned " + paths::runtimes_dir().string();
        return RuntimeAction::Notify;
    }

    if (event == Event::Character('c') && build.running()) {
        builder_.cancel();
        status_ = "cancelling the build";
        return RuntimeAction::Notify;
    }

    if (event == Event::Return) {
        if (build.finished()) {
            // Enter on a finished build clears it and picks up the result.
            builder_.dismiss();
            refresh();
            return RuntimeAction::Notify;
        }
        if (build.running()) {
            status_ = "a build is already running -- press c to cancel it";
            return RuntimeAction::Notify;
        }
        confirming_remove_ = false;
        start_install();
        return RuntimeAction::Notify;
    }

    if (event == Event::Character('d') || event == Event::Delete) {
        if (build.running()) {
            status_ = "wait for the build to finish before removing a runtime";
            return RuntimeAction::Notify;
        }
        if (selected_ < runtimes_.size() && !runtimes_[selected_].installed) {
            status_ = "that runtime is not installed";
            return RuntimeAction::Notify;
        }
        if (!confirming_remove_) {
            confirming_remove_ = true;
            return RuntimeAction::Notify;
        }
        confirming_remove_ = false;
        start_remove();
        return RuntimeAction::Notify;
    }

    return RuntimeAction::None;
}

Element RuntimeView::render_runtime(const RuntimeStatus& runtime, bool selected) const {
    const BackendInfo& info = backend_info(runtime.kind);
    const auto [state_text, state_color] = state_of(runtime);

    std::string name(info.name);
    name.resize(std::max<std::size_t>(name.size(), 8), ' ');

    Elements meta;
    meta.push_back(text(state_text) | color(state_color));
    if (runtime.installed && runtime.bytes > 0) {
        meta.push_back(text("  ·  " + runtime.size_label()) | color(theme::kMeta) | dim);
    }
    if (!runtime.built_at.empty()) {
        meta.push_back(text("  ·  built " + runtime.built_at) | color(theme::kMeta) | dim);
    }

    Element row = vbox({
        hbox({
            text(selected ? " ▸ " : "   "),
            text(name) | bold,
            hbox(std::move(meta)),
        }),
        hbox({
            text("     "),
            paragraph(std::string(info.blurb)) | color(theme::kMeta) | dim,
        }),
    });

    if (selected) {
        row = row | bgcolor(Color::GrayDark);
    }
    return row;
}

Element RuntimeView::render_build() const {
    const BuildProgress build = builder_.progress();
    if (build.phase == BuildProgress::Phase::Idle) {
        return text("");
    }

    const BackendInfo& info = backend_info(build.kind);

    Elements lines;
    lines.push_back(hbox({
        text(" " + std::string(info.name) + ": ") | bold,
        text(build.label()),
    }));

    if (build.phase == BuildProgress::Phase::Compiling) {
        lines.push_back(hbox({
            text(" "),
            gauge(build.percent) | flex | color(theme::kBatBusy),
            text(" " + build.step) | color(theme::kMeta) | dim,
        }));
    } else if (build.running()) {
        lines.push_back(hbox({
            text(" "),
            text(build.step) | color(theme::kMeta) | dim,
        }));
    }

    if (build.phase == BuildProgress::Phase::Failed) {
        lines.push_back(text(" " + build.error) | color(theme::kError));
        for (const std::string& line : build.log_tail) {
            lines.push_back(text("   " + line) | color(theme::kMeta) | dim);
        }
        lines.push_back(text(" full log: " + build.log_file.string()) |
                        color(theme::kMeta) | dim);
        lines.push_back(text(" enter dismisses this") | color(theme::kMeta) | dim);
    }

    if (build.phase == BuildProgress::Phase::Done) {
        lines.push_back(text(" restart BatBot to start using it") |
                        color(theme::kSeatActive));
        lines.push_back(text(" enter dismisses this") | color(theme::kMeta) | dim);
    }

    return vbox(std::move(lines)) | border;
}

Element RuntimeView::render_devices() const {
    if (devices_.empty()) {
        return text("   no compute devices -- no runtime is loaded") |
               color(theme::kMeta) | dim;
    }

    Elements lines;
    for (const ComputeDevice& device : devices_) {
        lines.push_back(hbox({
            text("   [" + std::to_string(device.index) + "] "),
            text(device.label()) | color(device.is_gpu ? Color(theme::kSeatDormant)
                                                       : Color(theme::kMeta)),
            text("  " + device.backend) | color(theme::kMeta) | dim,
        }));
    }
    return vbox(std::move(lines));
}

Element RuntimeView::render() const {
    Elements rows;
    for (std::size_t i = 0; i < runtimes_.size(); ++i) {
        rows.push_back(render_runtime(runtimes_[i], i == selected_));
    }

    std::string hint = "↑↓ choose   enter install   d remove   r rescan   esc back";
    Color hint_color = Color(theme::kMeta);
    if (confirming_remove_) {
        hint = "press d again to remove this runtime";
        hint_color = Color(theme::kError);
    } else if (builder_.progress().running()) {
        hint = "c cancels the build   ·   esc leaves it running in the background";
    }

    Elements body{
        hbox({
            text(" runtimes ") | bold | color(theme::kBat),
            text("· " + paths::runtimes_dir().string()) | color(theme::kMeta) | dim,
        }),
        separator(),
        vbox(std::move(rows)),
        render_build(),
        separator(),
        text(" devices llama.cpp can see") | color(theme::kMeta),
        render_devices(),
    };

    if (!status_.empty()) {
        body.push_back(separator());
        body.push_back(paragraph(status_) | color(theme::kNotice));
    }
    body.push_back(separator());
    body.push_back(text(hint) | color(hint_color) | dim);

    return vbox(std::move(body)) | border | bgcolor(Color::Black) | clear_under;
}

}  // namespace batbot::ui
