// SPDX-License-Identifier: MIT
#include "crucible/app/cli.hpp"

#include <iostream>

#include "crucible/config/paths.hpp"

namespace crucible::app {
namespace {

// The same vessel the TUI draws, at its liveliest frame. One shape for the
// program, whether you meet it in `--help` or in the corner of the screen.
constexpr const char* kBanner = R"(
         ^
        (^)
       (/^\)
   ,-----------,     Crucible )" CRUCIBLE_VERSION R"(
   \ ~~~~~~~~~ /     a local forge: experts on demand, projects that cook
    \         /
     \_______/
      /|   |\
     / |___| \
    /         \
   '-----------'
)";

void print_usage() {
    std::cout << kBanner << R"(
usage: crucible [options]

  -h, --help       show this and exit
  -v, --version    print the version and exit
      --config     print the config file path and exit
      --uninstall  remove Crucible, its config, and its data
      --no-trust   skip the folder trust prompt for this run
  -y, --yes        with --uninstall, answer yes to everything

Crucible reads its configuration from:
)" << "  " << paths::config_file().string() << "\n\n";
}

}  // namespace

const char* banner() {
    return kBanner;
}

Options parse_arguments(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "-h" || argument == "--help") {
            print_usage();
            options.should_exit = true;
            return options;
        }
        if (argument == "-v" || argument == "--version") {
            std::cout << "crucible " CRUCIBLE_VERSION "\n";
            options.should_exit = true;
            return options;
        }
        if (argument == "--config") {
            std::cout << paths::config_file().string() << "\n";
            options.should_exit = true;
            return options;
        }
        if (argument == "--no-trust") { options.skip_trust = true;  continue; }
        if (argument == "--uninstall") { options.uninstall = true;  continue; }
        if (argument == "-y" || argument == "--yes") { options.assume_yes = true; continue; }

        std::cerr << "crucible: unknown option '" << argument << "' (try --help)\n";
        options.should_exit = true;
        options.exit_code   = 2;
        return options;
    }

    return options;
}

}  // namespace crucible::app
