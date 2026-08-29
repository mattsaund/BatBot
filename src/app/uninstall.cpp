// SPDX-License-Identifier: MIT
//
// Removing BatBot.
//
// The three questions are separate on purpose, so a partial uninstall is
// possible -- but answering yes to all of them must leave nothing behind, which
// is what a clean reinstall test depends on.
#include "batbot/app/uninstall.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "batbot/llm/model_catalog.hpp"
#include "batbot/config/paths.hpp"
#include "batbot/util/format.hpp"

namespace batbot {
namespace {

/// The path of the running executable. Uninstalling the binary that is asking
/// the question is the whole point, so resolve it rather than guessing at
/// /usr/local/bin.
std::filesystem::path own_path() {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : exe;
}

std::uintmax_t directory_size(const std::filesystem::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (it->is_regular_file(file_ec) && !file_ec) {
            total += std::filesystem::file_size(it->path(), file_ec);
        }
    }
    return total;
}

bool ask(const std::string& question, bool default_yes) {
    std::cout << "  " << question << (default_yes ? " [Y/n] " : " [y/N] ") << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    if (answer.empty()) {
        return default_yes;
    }
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "Yes";
}

bool remove_path(const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    if (ec) {
        std::cout << "  could not remove " << target.string() << ": " << ec.message() << "\n";
        if (ec == std::errc::permission_denied) {
            std::cout << "  try: sudo rm -rf " << target.string() << "\n";
        }
        return false;
    }
    return true;
}

}  // namespace

int run_uninstall(bool assume_yes) {
    const std::filesystem::path binary = own_path();
    const std::filesystem::path config = paths::config_dir();
    const std::filesystem::path data   = paths::data_dir();
    const std::filesystem::path models = paths::models_dir();

    std::cout << "\n  Uninstalling BatBot\n\n";

    if (!binary.empty()) {
        std::cout << "  binary   " << binary.string() << "\n";
    }
    if (std::filesystem::exists(config)) {
        std::cout << "  config   " << config.string() << "\n";
    }
    if (std::filesystem::exists(data)) {
        std::cout << "  data     " << data.string() << "  ("
                  << format::bytes(directory_size(data)) << ")\n";
    }

    const std::vector<ModelFile> found = scan_models(models);
    if (!found.empty()) {
        std::cout << "  models   " << found.size() << " file"
                  << (found.size() == 1 ? "" : "s") << " in " << models.string() << "\n";
    }
    std::cout << "\n";

    // --- the binary --------------------------------------------------------
    bool removed_binary = false;
    if (!binary.empty() && std::filesystem::exists(binary)) {
        if (assume_yes || ask("Remove the batbot binary?", true)) {
            // Unlinking a running executable is fine on Linux: the kernel keeps
            // the inode alive until this process exits.
            removed_binary = remove_path(binary);
            if (removed_binary) {
                std::cout << "  removed " << binary.string() << "\n";
            }
        }
    }

    // --- configuration -----------------------------------------------------
    if (std::filesystem::exists(config)) {
        if (assume_yes || ask("Remove your configuration and folder-trust list?", true)) {
            if (remove_path(config)) {
                std::cout << "  removed " << config.string() << "\n";
            }
        } else {
            std::cout << "  kept    " << config.string() << "\n";
        }
    }

    // --- models and data ---------------------------------------------------
    // BatBot ships no models. Everything in this directory was downloaded or
    // placed by the user, so it is never removed on a default or -y run.
    if (std::filesystem::exists(data)) {
        if (!found.empty()) {
            std::cout << "\n  The models directory holds " << found.size()
                      << " model file" << (found.size() == 1 ? "" : "s") << " ("
                      << format::bytes(directory_size(models)) << ") that you supplied.\n";
        }
        if (assume_yes || ask("Remove the models directory and logs too?", true)) {
            if (remove_path(data)) {
                std::cout << "  removed " << data.string() << "\n";
            }
        } else {
            std::cout << "  kept    " << data.string() << "\n";
        }
    }

    std::cout << "\n  Done.";
    if (!removed_binary && !binary.empty()) {
        std::cout << " The binary is still at " << binary.string() << ".";
    }
    std::cout << "\n\n";
    return 0;
}

}  // namespace batbot
