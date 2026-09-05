// SPDX-License-Identifier: MIT
//
// Removing Crucible.
//
// The three questions are separate on purpose, so a partial uninstall is
// possible -- but answering yes to all of them must leave nothing behind, which
// is what a clean reinstall test depends on.
#include "crucible/app/uninstall.hpp"

#include "crucible/util/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
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
///
/// This used to read /proc/self/exe directly, which is Linux -- on macOS it
/// returned nothing and the uninstaller could not find the binary it was being
/// asked to remove.
std::filesystem::path own_path() {
    return util::executable_path();
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

#if defined(_WIN32)
constexpr std::string_view kExeSuffix = ".exe";
#else
constexpr std::string_view kExeSuffix = "";
#endif

/// The other programs this install put beside the binary.
///
/// `crucible` is not the whole install. `crucible-gui` is the desktop face of
/// the same engine and `crucible-routebench` is the developer tool; both are
/// placed in the same bin/ by the same install component, and both are useless
/// once the libraries under them are gone. Leaving either behind puts a
/// program on PATH that can only fail, and makes a clean reinstall test lie.
std::vector<std::filesystem::path> companion_programs(const std::filesystem::path& binary) {
    std::vector<std::filesystem::path> found;
    if (binary.empty()) {
        return found;
    }
    const std::filesystem::path dir = binary.parent_path();
    for (const std::string_view name : {"crucible-gui", "crucible-routebench"}) {
        std::filesystem::path candidate =
            dir / (std::string(name) + std::string(kExeSuffix));
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && candidate != binary) {
            found.push_back(std::move(candidate));
        }
    }
    return found;
}

/// The shared libraries that sit beside the executable rather than under
/// lib/crucible.
///
/// That is the Windows layout, and it is not a quirk of the installer: the
/// loader there looks next to the executable and not at an RPATH, so the
/// install rule puts them in bin/. Same files, different place, and an
/// uninstaller that only knows lib/crucible would leave every byte of
/// llama.cpp behind on Windows.
std::vector<std::filesystem::path> companion_libraries(const std::filesystem::path& binary) {
    std::vector<std::filesystem::path> found;
    if (binary.empty() || kExeSuffix.empty()) {
        return found;  // elsewhere they live in lib/crucible, handled below
    }
    const std::filesystem::path dir = binary.parent_path();
    for (const char* name : {"llama.dll", "ggml.dll", "ggml-base.dll"}) {
        std::filesystem::path candidate = dir / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            found.push_back(std::move(candidate));
        }
    }
    return found;
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
    const std::vector<std::filesystem::path> programs  = companion_programs(binary);
    const std::vector<std::filesystem::path> beside    = companion_libraries(binary);

    std::cout << "\n  Uninstalling Crucible\n\n";

    if (!binary.empty()) {
        std::cout << "  binary   " << binary.string() << "\n";
    }
    if (std::filesystem::exists(config)) {
        std::cout << "  config   " << config.string() << "\n";
    }
    for (const std::filesystem::path& program : programs) {
        std::cout << "  program  " << program.string() << "\n";
    }
    for (const std::filesystem::path& lib : beside) {
        std::cout << "  library  " << lib.string() << "\n";
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
    bool dropped_programs = false;
    if (!binary.empty() && std::filesystem::exists(binary)) {
        if (assume_yes || ask("Remove crucible, crucible-gui and their libraries?", true)) {
            dropped_programs = true;
            // Unlinking a running executable is fine on Linux: the kernel keeps
            // the inode alive until this process exits.
            removed_binary = remove_path(binary);
            if (removed_binary) {
                std::cout << "  removed " << binary.string() << "\n";
            }
            // Everything else the install put down goes with it. The desktop
            // app and the developer tool are the same install, and the shared
            // libraries under them are useless once any of it is gone.
            for (const std::filesystem::path& program : programs) {
                if (remove_path(program)) {
                    std::cout << "  removed " << program.string() << "\n";
                }
            }
            for (const std::filesystem::path& lib : beside) {
                if (remove_path(lib)) {
                    std::cout << "  removed " << lib.string() << "\n";
                }
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
    bool dropped_config = false;
    if (std::filesystem::exists(config)) {
        if (assume_yes || ask("Remove your configuration and folder-trust list?", true)) {
            dropped_config = true;
            if (remove_path(config)) {
                std::cout << "  removed " << config.string() << "\n";
            }
        } else {
            std::cout << "  kept    " << config.string() << "\n";
        }
    }

    // --- models and data ---------------------------------------------------
    // Crucible ships no models, so everything in this directory was put there
    // by the user -- which is why it is a question of its own and not folded
    // into the one above. Answering yes does remove it, -y included: a clean
    // reinstall test depends on yes meaning yes.
    bool dropped_data = false;
    if (std::filesystem::exists(data)) {
        if (!found.empty()) {
            std::cout << "\n  The models directory holds " << found.size()
                      << " model file" << (found.size() == 1 ? "" : "s") << " ("
                      << format::bytes(directory_size(models)) << ") that you supplied.\n";
        }
        // This is also where the GPU runtimes the user built live, along with
        // the llama.cpp source they were built from and the project history.
        if (assume_yes || ask("Remove the models, runtimes, history and logs too?", true)) {
            dropped_data = true;
            if (remove_path(data)) {
                std::cout << "  removed " << data.string() << "\n";
            }
        } else {
            std::cout << "  kept    " << data.string() << "\n";
        }
    }

    // --- what is actually left ---------------------------------------------
    //
    // The promise of this command is that yes to everything leaves nothing
    // behind, and the only honest way to make that claim is to look. Only the
    // things the user agreed to remove are checked: something kept on purpose
    // is not litter, and reporting it as such would train people to ignore
    // this list.
    std::vector<std::filesystem::path> left;
    const auto survived = [&left](const std::filesystem::path& path) {
        std::error_code ec;
        if (!path.empty() && std::filesystem::exists(path, ec)) {
            left.push_back(path);
        }
    };
    if (dropped_programs) {
        survived(binary);
        for (const std::filesystem::path& program : programs) {
            survived(program);
        }
        for (const std::filesystem::path& lib : beside) {
            survived(lib);
        }
        survived(libs);
        survived(cache);
    }
    if (dropped_config) {
        survived(config);
    }
    if (dropped_data) {
        survived(data);
    }

    if (!left.empty()) {
        std::cout << "\n  These could not be removed:\n";
        for (const std::filesystem::path& path : left) {
            std::cout << "    " << path.string() << "\n";
        }
        std::cout << "\n  Remove them by hand, or re-run with sudo if they are"
                     " somewhere you cannot write.\n\n";
        return 1;
    }

    std::cout << "\n  Done.";
    if (!removed_binary && !binary.empty()) {
        std::cout << " The binary is still at " << binary.string() << ".";
    }
    std::cout << "\n\n";
    return 0;
}

}  // namespace crucible
