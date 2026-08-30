// SPDX-License-Identifier: MIT
//
// The GPU priority panel: which graphics card gets filled first.
//
// Priority order used to be a text field holding "0, 2, 1" -- device indices,
// typed from memory, with the card names only visible in a help line. This is
// the same setting as a list you can rearrange: the cards are named, the order
// on screen is the order they are used, and nothing has to be looked up.
//
// Enter picks a card up, Enter on another swaps the two. Two keys, and the
// list always shows the result rather than a string that has to be decoded.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "batbot/runtime/devices.hpp"

namespace batbot::ui {

/// What the GPU order panel wants from the application after a key.
enum class GpuOrderAction {
    None,
    Close,   ///< go back to the settings list
    Apply,   ///< the order changed; the caller should mark the config dirty
};

class GpuOrderView {
public:
    /// Read the machine's GPUs and lay them out in `order` -- the configured
    /// priority, which may be empty, stale, or missing cards.
    void open(const std::vector<int>& order);
    void close() { open_ = false; }
    bool active() const { return open_; }

    GpuOrderAction handle(const ftxui::Event& event);
    ftxui::Element render() const;

    /// The arrangement on screen, as device indices best-first. Empty when
    /// there is nothing worth storing (fewer than two GPUs).
    std::vector<int> order() const;

    const std::string& status() const { return status_; }

private:
    ftxui::Element render_gpu(std::size_t position) const;

    std::vector<ComputeDevice> gpus_;
    std::size_t                selected_ = 0;
    /// The card Enter picked up, waiting for a destination.
    std::optional<std::size_t> holding_;
    bool                       open_ = false;
    std::string                status_;
};

}  // namespace batbot::ui
