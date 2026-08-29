// SPDX-License-Identifier: MIT
#include "batbot/app/trust_gate.hpp"

#include <iostream>
#include <string>

#include "batbot/app/cli.hpp"
#include "batbot/config/paths.hpp"
#include "batbot/config/trust.hpp"

namespace batbot::app {

bool ensure_trusted(const std::filesystem::path& directory) {
    TrustStore store(paths::trust_file());
    if (store.is_trusted(directory)) {
        return true;
    }

    std::cout << banner() << '\n'
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
        std::cout << "  (could not write " << paths::trust_file().string()
                  << "; you will be asked again next time)\n";
    }
    return true;
}

}  // namespace batbot::app
