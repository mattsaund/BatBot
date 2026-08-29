// SPDX-License-Identifier: MIT
//
// batbot -- cd into a project, type `batbot`, get a local expert.
//
// This file is deliberately thin: parse the command line, deal with the two
// things that happen instead of starting the TUI (--uninstall, an untrusted
// folder), then hand off. Everything interesting is somewhere else.

#include <filesystem>
#include <string>
#include <vector>

#include "batbot/app/cli.hpp"
#include "batbot/app/trust_gate.hpp"
#include "batbot/app/uninstall.hpp"
#include "batbot/config/config.hpp"
#include "batbot/ui/app.hpp"

int main(int argc, char** argv) {
    const batbot::app::Options options = batbot::app::parse_arguments(argc, argv);
    if (options.should_exit) {
        return options.exit_code;
    }

    if (options.uninstall) {
        return batbot::run_uninstall(options.assume_yes);
    }

    // Trust is asked before anything is loaded, so declining costs nothing.
    const std::filesystem::path working_directory = std::filesystem::current_path();
    if (!options.skip_trust && !batbot::app::ensure_trusted(working_directory)) {
        return 1;
    }

    // Configuration problems are warnings, not failures: BatBot starts and
    // explains, rather than refusing to run over one bad field.
    std::vector<std::string> warnings;
    batbot::Config config = batbot::load_config(warnings);

    batbot::ui::App app(std::move(config), warnings);
    return app.run();
}
