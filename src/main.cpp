// SPDX-License-Identifier: MIT
//
// crucible -- cd into a project, type `crucible`, get a local expert.
//
// This file is deliberately thin: parse the command line, deal with the two
// things that happen instead of starting the TUI (--uninstall, an untrusted
// folder), then hand off. Everything interesting is somewhere else.

#include <filesystem>
#include <string>
#include <vector>

#include "crucible/app/cli.hpp"
#include "crucible/app/trust_gate.hpp"
#include "crucible/app/uninstall.hpp"
#include "crucible/config/config.hpp"
#include "crucible/ui/app.hpp"
#include "crucible/util/platform.hpp"

int main(int argc, char** argv) {
    // Before anything is printed: --help draws the mark, and the mark is not
    // ASCII.
    crucible::util::use_utf8_console();

    const crucible::app::Options options = crucible::app::parse_arguments(argc, argv);
    if (options.should_exit) {
        return options.exit_code;
    }

    if (options.uninstall) {
        return crucible::run_uninstall(options.assume_yes);
    }

    // Trust is asked before anything is loaded, so declining costs nothing.
    const std::filesystem::path working_directory = std::filesystem::current_path();
    if (!options.skip_trust && !crucible::app::ensure_trusted(working_directory)) {
        return 1;
    }

    // Configuration problems are warnings, not failures: Crucible starts and
    // explains, rather than refusing to run over one bad field.
    std::vector<std::string> warnings;
    crucible::Config config = crucible::load_config(warnings);

    crucible::ui::App app(std::move(config), warnings);
    return app.run();
}
