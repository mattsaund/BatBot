// batbot -- cd into a project, type `batbot`, get a local expert.
//
// Startup order matters: the folder-trust prompt happens on the plain terminal
// before the alternate screen is entered, so the question the user is agreeing
// to is still on screen afterwards rather than being wiped by the TUI.

#include <iostream>
#include <string>
#include <vector>

#include "batbot/core/config.hpp"
#include "batbot/core/paths.hpp"
#include "batbot/core/trust.hpp"
#include "batbot/core/uninstall.hpp"
#include "batbot/ui/app.hpp"

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
)" << "  " << batbot::paths::config_file().string() << "\n\n";
}

/// Ask once, remember forever. Returns false if the user declines.
bool ensure_trusted(const std::filesystem::path& directory) {
    batbot::TrustStore store(batbot::paths::trust_file());
    if (store.is_trusted(directory)) {
        return true;
    }

    std::cout << kBanner << '\n'
              << "  BatBot is about to open in:\n\n"
              << "    " << directory.string() << "\n\n"
              << "  Trusting a folder lets BatBot work with the files in it and\n"
              << "  everything below it. Only trust folders whose contents you know.\n\n"
              << "  Trust this folder? [y/N] " << std::flush;

    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::cout << "\n  No answer, not trusting.\n";
        return false;
    }

    if (answer != "y" && answer != "Y" && answer != "yes" && answer != "Yes") {
        std::cout << "\n  Not trusted. Nothing was opened.\n";
        return false;
    }

    if (!store.trust(directory)) {
        // Worth saying out loud: the session still works, but the prompt will
        // come back next time, which would otherwise look like a bug.
        std::cout << "  (could not write " << batbot::paths::trust_file().string()
                  << "; you will be asked again next time)\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool skip_trust  = false;
    bool do_uninstall = false;
    bool assume_yes  = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            print_usage();
            return 0;
        }
        if (argument == "-v" || argument == "--version") {
            std::cout << "batbot " BATBOT_VERSION "\n";
            return 0;
        }
        if (argument == "--config") {
            std::cout << batbot::paths::config_file().string() << "\n";
            return 0;
        }
        if (argument == "--no-trust") {
            skip_trust = true;
            continue;
        }
        if (argument == "--uninstall") {
            do_uninstall = true;
            continue;
        }
        if (argument == "-y" || argument == "--yes") {
            assume_yes = true;
            continue;
        }
        std::cerr << "batbot: unknown option '" << argument << "' (try --help)\n";
        return 2;
    }

    if (do_uninstall) {
        return batbot::run_uninstall(assume_yes);
    }

    const std::filesystem::path working_directory = std::filesystem::current_path();
    if (!skip_trust && !ensure_trusted(working_directory)) {
        return 1;
    }

    std::vector<std::string> warnings;
    batbot::Config config = batbot::load_config(warnings);

    batbot::ui::App app(std::move(config), warnings);
    return app.run();
}
