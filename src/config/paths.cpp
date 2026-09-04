// SPDX-License-Identifier: MIT
//
// Where Crucible keeps its files, following the XDG base directory spec.
//
// The spec's awkward rule is honoured here: an XDG variable set to a *relative*
// path must be ignored rather than resolved, because resolving it against the
// working directory would scatter config wherever the user happened to be.
#include "crucible/config/paths.hpp"

#include <cstdlib>

namespace crucible::paths {
namespace {

std::filesystem::path home_dir() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home);
    }
    return std::filesystem::current_path();
}

/// Honour an XDG variable if it is set to an absolute path, per the spec:
/// a relative value must be ignored rather than resolved.
std::filesystem::path xdg_dir(const char* env_var, const char* fallback) {
    if (const char* value = std::getenv(env_var); value != nullptr && *value != '\0') {
        std::filesystem::path candidate(value);
        if (candidate.is_absolute()) {
            return candidate / "crucible";
        }
    }
    return home_dir() / fallback / "crucible";
}

}  // namespace

std::filesystem::path config_dir()  { return xdg_dir("XDG_CONFIG_HOME", ".config"); }
std::filesystem::path data_dir()    { return xdg_dir("XDG_DATA_HOME", ".local/share"); }
std::filesystem::path config_file() { return config_dir() / "config.json"; }
std::filesystem::path trust_file()  { return config_dir() / "trust.json"; }
std::filesystem::path models_dir()  { return data_dir() / "models"; }
std::filesystem::path log_file()    { return data_dir() / "crucible.log"; }

std::filesystem::path runtimes_dir()      { return data_dir() / "runtimes"; }
std::filesystem::path runtime_src_dir()   { return data_dir() / "runtime-src"; }
std::filesystem::path runtime_build_dir() { return data_dir() / "runtime-build"; }
std::filesystem::path projects_dir()      { return data_dir() / "projects"; }

std::filesystem::path executable_dir() {
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return self.parent_path();
}

std::filesystem::path expand_user(std::string_view raw) {
    if (raw.empty()) {
        return {};
    }

    std::filesystem::path path;
    if (raw == "~") {
        path = home_dir();
    } else if (raw.size() >= 2 && raw[0] == '~' && (raw[1] == '/')) {
        path = home_dir() / std::filesystem::path(std::string(raw.substr(2)));
    } else {
        path = std::filesystem::path(std::string(raw));
    }

    // weakly_canonical tolerates a path whose tail does not exist yet, which
    // matters for model files the user has not downloaded.
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : resolved;
}

}  // namespace crucible::paths
