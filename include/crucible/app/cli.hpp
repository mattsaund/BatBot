// SPDX-License-Identifier: MIT
//
// Command-line parsing, and the banner everything else prints above it.
#pragma once

#include <string>

namespace crucible::app {

/// What the command line asked for.
struct Options {
    bool skip_trust = false;  ///< --no-trust: do not ask about this folder
    bool uninstall  = false;  ///< --uninstall
    bool assume_yes = false;  ///< -y, only meaningful with --uninstall

    /// Set when the program should stop after parsing -- --help, --version and
    /// --config all print something and exit, as does a bad option.
    bool should_exit = false;
    int  exit_code   = 0;
};

/// The flame shown by --help and the trust prompt.
const char* banner();

/// Parse `argv`, printing usage or version as required. Never throws; an
/// unknown option sets `should_exit` with a non-zero `exit_code`.
Options parse_arguments(int argc, char** argv);

}  // namespace crucible::app
