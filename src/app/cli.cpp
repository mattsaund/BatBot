// SPDX-License-Identifier: MIT
#include "batbot/app/cli.hpp"

#include <iostream>

#include "batbot/config/paths.hpp"

namespace batbot::app {
namespace {

constexpr const char* kBanner = R"(
   /\           /\
  /  \_________/  \      BatBot )" BATBOT_VERSION R"(
 |   ___________   |     a local roundtable of experts
 |  |           |  |
 |  |  o     o  |  |
 |  |    \_/    |  |
 |  |___________|  |
 |_________________|
)";

void print_usage() {
    std::cout << kBanner << R"(
usage: batbot [options]

  -h, --help       show this and exit
  -v, --version    print the version and exit
      --config     print the config file path and exit
      --uninstall  remove BatBot, its config, and its data
      --no-trust   skip the folder trust prompt for this run
  -y, --yes        with --uninstall, answer yes to everything

BatBot reads its configuration from:
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
            std::cout << "batbot " BATBOT_VERSION "\n";
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

        std::cerr << "batbot: unknown option '" << argument << "' (try --help)\n";
        options.should_exit = true;
        options.exit_code   = 2;
        return options;
    }

    return options;
}

}  // namespace batbot::app
