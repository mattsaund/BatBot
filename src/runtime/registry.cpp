// SPDX-License-Identifier: MIT
//
// Scanning, seeding and loading the runtimes directory.
#include "batbot/runtime/registry.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>

#include <ggml-backend.h>

#include "batbot/config/paths.hpp"
#include "batbot/util/format.hpp"
#include "batbot/util/subprocess.hpp"

namespace batbot {
namespace {

using json = nlohmann::json;

/// ggml names its modules libggml-<backend>[-<variant>].so, so the backend a
/// file belongs to is the first component after the prefix. The CPU backend
/// ships as a dozen variants (haswell, icelake, ...) that all belong to the
/// same runtime, which is why this maps a file to a *backend* rather than
/// expecting one file per backend.
std::optional<BackendKind> kind_of_module(const std::filesystem::path& file) {
    const std::string name = file.filename().string();

    constexpr std::string_view kPrefix = "libggml-";
    if (name.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    // Only the bare .so is a module worth counting. The versioned aliases
    // (libggml-cpu-haswell.so.0) are the same file under another name, and
    // counting them would double every size shown in settings.
    if (!file.has_extension() || file.extension() != ".so") {
        return std::nullopt;
    }

    std::string rest = name.substr(kPrefix.size());
    rest.erase(rest.size() - 3);  // drop ".so"

    const std::size_t dash = rest.find('-');
    if (dash != std::string::npos) {
        rest.erase(dash);
    }
    return backend_from_id(rest);
}

/// Every module file in `dir`, grouped by backend.
std::map<BackendKind, std::vector<std::filesystem::path>> modules_in(
    const std::filesystem::path& dir) {
    std::map<BackendKind, std::vector<std::filesystem::path>> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return found;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_directory()) {
            continue;
        }
        if (const std::optional<BackendKind> kind = kind_of_module(entry.path())) {
            found[*kind].push_back(entry.path());
        }
    }
    for (auto& [kind, files] : found) {
        std::sort(files.begin(), files.end());
    }
    return found;
}

/// How many devices ggml registered for each backend it managed to load.
std::map<BackendKind, int> registered_devices() {
    std::map<BackendKind, int> counts;
    const std::size_t total = ggml_backend_dev_count();
    for (std::size_t i = 0; i < total; ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        if (device == nullptr) {
            continue;
        }
        ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(device);
        if (reg == nullptr) {
            continue;
        }
        if (const std::optional<BackendKind> kind =
                backend_from_reg_name(ggml_backend_reg_name(reg))) {
            ++counts[*kind];
        }
    }
    return counts;
}

json read_manifest(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        return json::object();
    }
    json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
    return parsed.is_object() ? parsed : json::object();
}

}  // namespace

std::string RuntimeStatus::size_label() const {
    return format::bytes(bytes);
}

std::filesystem::path RuntimeRegistry::manifest_file() {
    return paths::runtimes_dir() / "manifest.json";
}

bool RuntimeRegistry::loadable_backends_supported() {
#ifdef GGML_BACKEND_DL
    return true;
#else
    return false;
#endif
}

int RuntimeRegistry::seed_from_bundle(std::string& error) {
    if (!loadable_backends_supported()) {
        return 0;
    }

    const std::filesystem::path bundle = paths::bundled_runtimes_dir();
    const std::filesystem::path target = paths::runtimes_dir();
    if (bundle.empty() || bundle == target) {
        return 0;
    }

    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        error = "could not create " + target.string() + ": " + ec.message();
        return 0;
    }

    const auto have = modules_in(target);
    const auto offered = modules_in(bundle);

    int copied = 0;
    for (const auto& [kind, files] : offered) {
        // Seed a backend only when the user has none of it. A runtime the user
        // built themselves is newer than the bundled one and must win.
        if (have.count(kind) != 0) {
            continue;
        }
        for (const std::filesystem::path& file : files) {
            std::filesystem::copy_file(file, target / file.filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "could not copy " + file.filename().string() + ": " + ec.message();
                return copied;
            }
            ++copied;
        }
    }
    return copied;
}

void RuntimeRegistry::load_all() {
    if (!loadable_backends_supported()) {
        return;  // the backend is compiled in; there is nothing to load
    }
    const std::string dir = paths::runtimes_dir().string();
    ggml_backend_load_all_from_path(dir.c_str());
}

std::vector<RuntimeStatus> RuntimeRegistry::scan() {
    const auto installed = modules_in(paths::runtimes_dir());
    const auto devices   = registered_devices();
    const json manifest  = read_manifest(manifest_file());

    std::vector<RuntimeStatus> result;
    result.reserve(kBackendCount);

    for (const BackendInfo& info : all_backends()) {
        RuntimeStatus status;
        status.kind = info.kind;

        if (const auto found = installed.find(info.kind); found != installed.end()) {
            status.installed = true;
            status.files     = found->second;
            std::error_code ec;
            for (const std::filesystem::path& file : status.files) {
                status.bytes += std::filesystem::file_size(file, ec);
            }
        }

        if (const auto counted = devices.find(info.kind); counted != devices.end()) {
            status.active       = counted->second > 0;
            status.device_count = counted->second;
        }

        if (const auto entry = manifest.find(std::string(info.id)); entry != manifest.end()) {
            status.llama_tag = entry->value("llama_tag", "");
            status.built_at  = entry->value("built_at", "");
        }

        if (!info.required_tool.empty() && !util::on_path(std::string(info.required_tool))) {
            status.buildable = false;
            status.blocker   = std::string(info.required_tool) + " is not installed";
        }

        result.push_back(std::move(status));
    }
    return result;
}

bool RuntimeRegistry::remove(BackendKind kind, std::string& error) {
    const BackendInfo& info = backend_info(kind);
    if (!info.removable) {
        error = std::string(info.name) +
                " is the fallback runtime and cannot be removed -- without it "
                "BatBot has no way to run a model at all";
        return false;
    }

    const auto installed = modules_in(paths::runtimes_dir());
    const auto found = installed.find(kind);
    if (found == installed.end()) {
        error = std::string(info.name) + " is not installed";
        return false;
    }

    std::error_code ec;
    for (const std::filesystem::path& file : found->second) {
        // Take the versioned aliases with it: libggml-cuda.so.0 and
        // libggml-cuda.so.0.9.4 sit beside the module and would otherwise be
        // left behind as several hundred megabytes of orphan.
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(file.parent_path(), ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(file.filename().string(), 0) == 0) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
    if (ec) {
        error = "could not remove " + std::string(info.name) + ": " + ec.message();
        return false;
    }

    json manifest = read_manifest(manifest_file());
    manifest.erase(std::string(info.id));
    std::ofstream out(manifest_file());
    if (out) {
        out << manifest.dump(2) << '\n';
    }

    // The build tree is far larger than the module and is pure cache.
    std::filesystem::remove_all(paths::runtime_build_dir() / std::string(info.id), ec);
    return true;
}

}  // namespace batbot
