// SPDX-License-Identifier: MIT
// `crucible --uninstall`: remove Crucible without hunting for what it left behind.
#pragma once

namespace crucible {

/// Remove the running binary, the configuration, and the data directory.
///
/// Each is a separate question so a partial uninstall is possible, but
/// answering yes to all of them leaves nothing behind -- which is what a clean
/// reinstall test needs. `assume_yes` answers yes to every question.
///
/// Returns a process exit code.
int run_uninstall(bool assume_yes);

}  // namespace crucible
