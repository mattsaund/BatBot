// SPDX-License-Identifier: MIT
//
// The "do you trust this folder?" gate.
//
// Asked on the plain terminal before the alternate screen is entered, so the
// question the user agreed to is still on screen afterwards rather than being
// wiped by the TUI.
#pragma once

#include <filesystem>

namespace crucible::app {

/// Ask once, remember forever. Returns false if the user declines, in which
/// case nothing should be opened.
bool ensure_trusted(const std::filesystem::path& directory);

}  // namespace crucible::app
