// SPDX-License-Identifier: MIT
//
// crucible-gui -- the desktop face.
//
// As thin as src/main.cpp is, and for the same reason: parse the command line,
// deal with the folder trust gate, then hand off. Everything interesting is
// somewhere else, and almost all of it is shared with the terminal program.

#include <filesystem>
#include <string>
#include <vector>

#include "crucible/app/cli.hpp"
#include "crucible/app/trust_gate.hpp"
#include "crucible/app/uninstall.hpp"
#include "crucible/config/config.hpp"
#include "app.hpp"

int main(int argc, char** argv) {
    const crucible::app::Options options = crucible::app::parse_arguments(argc, argv);
    if (options.should_exit) {
        return options.exit_code;
    }

    if (options.uninstall) {
        return crucible::run_uninstall(options.assume_yes);
    }

    // Asked on the terminal that launched it, before a window is opened.
    // Trust is a decision about a directory, and it is the same decision for
    // both faces -- answering it in one is answering it in the other.
    const std::filesystem::path working_directory = std::filesystem::current_path();
    if (!options.skip_trust && !crucible::app::ensure_trusted(working_directory)) {
        return 1;
    }

    std::vector<std::string> warnings;
    crucible::Config config = crucible::load_config(warnings);

    crucible::gui::App app(std::move(config), std::move(warnings));
    return app.run();
}
