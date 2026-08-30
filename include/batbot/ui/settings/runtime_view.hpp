// SPDX-License-Identifier: MIT
//
// The Runtimes panel: install and remove compute backends from inside BatBot.
//
// This is the screen that makes a CPU-only install into a CUDA one without
// touching a terminal. It lists every backend BatBot knows how to build, what
// state it is in, and -- while a build runs -- how far along it is.
//
// It owns no llama.cpp state. Installing writes a file into the runtimes
// directory; ggml picks it up on the next start.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "batbot/runtime/builder.hpp"
#include "batbot/runtime/devices.hpp"
#include "batbot/runtime/registry.hpp"

namespace batbot::ui {

/// What the runtime panel wants from the application after a key.
enum class RuntimeAction {
    None,
    Close,   ///< go back to the settings list
    Notify,  ///< `status()` changed and is worth showing
};

class RuntimeView {
public:
    /// `wake` is called from the build thread when progress moved.
    explicit RuntimeView(std::function<void()> wake);

    /// Re-read the runtimes directory and the device list.
    void refresh();

    bool active() const { return open_; }
    void open()  { open_ = true; refresh(); }
    void close() { open_ = false; }

    RuntimeAction handle(const ftxui::Event& event);
    ftxui::Element render() const;

    const std::string& status() const { return status_; }

    /// True while a build is running, so the application knows not to let the
    /// user quit out from under it without a word.
    bool building() const;

    /// Stop any build and wait for its thread. Called during teardown, before
    /// the screen it redraws through goes away.
    void shutdown();

private:
    void start_install();
    void start_remove();

    ftxui::Element render_runtime(const RuntimeStatus& runtime, bool selected) const;
    ftxui::Element render_build() const;
    ftxui::Element render_devices() const;

    std::function<void()>      wake_;
    RuntimeBuilder             builder_;
    std::vector<RuntimeStatus> runtimes_;
    std::vector<ComputeDevice> devices_;

    std::size_t selected_ = 0;
    bool        open_     = false;
    std::string status_;

    /// Removing a runtime is a keypress away from installing one, so the first
    /// press arms and the second acts.
    bool confirming_remove_ = false;
};

}  // namespace batbot::ui
