// SPDX-License-Identifier: MIT
//
// Removing Crucible.
//
// The three questions are separate on purpose, so a partial uninstall is
// possible -- but answering yes to all of them must leave nothing behind, which
// is what a clean reinstall test depends on.
#include "crucible/app/uninstall.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "crucible/llm/model_catalog.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"

namespace crucible {
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

/// $XDG_CACHE_HOME/crucible -- build trees left by install.sh when the checkout
/// was on a filesystem that cannot hold them. Pure cache, but it can be a
/// gigabyte, so it goes with the binary rather than being left behind.
std::filesystem::path cache_dir() {
    if (const char* value = std::getenv("XDG_CACHE_HOME"); value != nullptr && *value != '\0') {
        if (const std::filesystem::path candidate(value); candidate.is_absolute()) {
            return candidate / "crucible";
        }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache" / "crucible";
    }
    return {};
}

/// The developer tool installed beside the binary. It is part of the same
/// install, so it goes with it -- leaving a stray crucible-routebench on PATH
/// after an uninstall is exactly the kind of litter that makes a clean
/// reinstall test lie.
std::filesystem::path tool_path(const std::filesystem::path& binary) {
    if (binary.empty()) {
        return {};
    }
    const std::filesystem::path candidate = binary.parent_path() / "crucible-routebench";
    return std::filesystem::exists(candidate) ? candidate : std::filesystem::path{};
}

/// <prefix>/lib/crucible -- llama.cpp's shared libraries and the runtimes that
/// shipped with the install. The binary alone is not the whole program any
/// more, so removing it without these would leave most of the bytes behind.
std::filesystem::path library_dir(const std::filesystem::path& binary) {
    if (binary.empty()) {
        return {};
    }
    const std::filesystem::path candidate = binary.parent_path().parent_path() / "lib" / "crucible";
    return std::filesystem::exists(candidate) ? candidate : std::filesystem::path{};
}

}  // namespace

int run_uninstall(bool assume_yes) {
    const std::filesystem::path binary  = own_path();
    const std::filesystem::path config  = paths::config_dir();
    const std::filesystem::path data    = paths::data_dir();
    const std::filesystem::path models  = paths::models_dir();
    const std::filesystem::path libs    = library_dir(binary);
    const std::filesystem::path cache   = cache_dir();
    const std::filesystem::path tool    = tool_path(binary);

    std::cout << "\n  Uninstalling Crucible\n\n";

    if (!binary.empty()) {
        std::cout << "  binary   " << binary.string() << "\n";
    }
    if (std::filesystem::exists(config)) {
        std::cout << "  config   " << config.string() << "\n";
    }
    if (!tool.empty()) {
        std::cout << "  tool     " << tool.string() << "\n";
    }
    if (!libs.empty()) {
        std::cout << "  runtime  " << libs.string() << "  ("
                  << format::bytes(directory_size(libs)) << ")\n";
    }
    if (std::filesystem::exists(data)) {
        std::cout << "  data     " << data.string() << "  ("
                  << format::bytes(directory_size(data)) << ")\n";
    }
    if (!cache.empty() && std::filesystem::exists(cache)) {
        std::cout << "  cache    " << cache.string() << "  ("
                  << format::bytes(directory_size(cache)) << ")\n";
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
        if (assume_yes || ask("Remove the crucible binary?", true)) {
            // Unlinking a running executable is fine on Linux: the kernel keeps
            // the inode alive until this process exits.
            removed_binary = remove_path(binary);
            if (removed_binary) {
                std::cout << "  removed " << binary.string() << "\n";
            }
            // Everything else the install put down goes with it: the
            // routebench tool and the shared libraries beside it are both
            // useless without the binary.
            if (!tool.empty() && remove_path(tool)) {
                std::cout << "  removed " << tool.string() << "\n";
            }
            if (!libs.empty() && remove_path(libs)) {
                std::cout << "  removed " << libs.string() << "\n";
            }
            if (!cache.empty() && std::filesystem::exists(cache) && remove_path(cache)) {
                std::cout << "  removed " << cache.string() << "\n";
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
    // Crucible ships no models. Everything in this directory was downloaded or
    // placed by the user, so it is never removed on a default or -y run.
    if (std::filesystem::exists(data)) {
        if (!found.empty()) {
            std::cout << "\n  The models directory holds " << found.size()
                      << " model file" << (found.size() == 1 ? "" : "s") << " ("
                      << format::bytes(directory_size(models)) << ") that you supplied.\n";
        }
        // This is also where the GPU runtimes the user built live, along with
        // the llama.cpp source they were built from and the project history.
        if (assume_yes || ask("Remove the models, runtimes, history and logs too?", true)) {
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

}  // namespace crucible
