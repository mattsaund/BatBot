// SPDX-License-Identifier: MIT
//
// What is installed in the runtimes directory, and getting it into ggml.
//
// BatBot keeps every runtime in one directory. Installing one puts a shared
// library there, uninstalling deletes it, and startup hands the directory to
// ggml. Nothing else in the program needs to know a backend can come and go.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "batbot/runtime/backend.hpp"

namespace batbot {

/// One runtime, as the settings screen sees it.
struct RuntimeStatus {
    BackendKind kind = BackendKind::Cpu;

    /// A module for this backend is present in the runtimes directory.
    bool installed = false;

    /// ggml loaded it this run and it reported at least one device. A runtime
    /// can be installed but inactive -- a CUDA build on a machine whose driver
    /// has since been removed still sits on disk, and saying so is more useful
    /// than pretending it works.
    bool   active       = false;
    int    device_count = 0;

    std::uintmax_t bytes = 0;   ///< total size of its module files

    /// Provenance, from the manifest. Empty for a runtime that arrived with
    /// the install rather than being built here.
    std::string llama_tag;
    std::string built_at;

    /// Whether a build could be attempted right now, and what is missing if
    /// not -- "needs nvcc" is a far better answer than a failed build.
    bool        buildable = true;
    std::string blocker;

    /// The module files, so uninstall knows what to delete.
    std::vector<std::filesystem::path> files;

    /// "4.1 MB", for the list.
    std::string size_label() const;
};

/// The runtimes directory as BatBot sees it.
///
/// Cheap to construct and safe to re-scan; the settings screen builds one per
/// refresh rather than holding state that can go stale.
class RuntimeRegistry {
public:
    /// Copy any backend present in the installed bundle but missing from the
    /// user's runtimes directory. This is what gives a fresh install a working
    /// CPU runtime without a build step, and it is a no-op afterwards.
    /// Returns the number of files copied.
    static int seed_from_bundle(std::string& error);

    /// Hand the runtimes directory to ggml. Call once, before any model is
    /// loaded; ggml scores the candidates and registers the best of each kind.
    static void load_all();

    /// True when this binary can load runtimes at all. A monolithic build
    /// (-DBATBOT_BACKEND_DL=OFF) cannot, and the settings screen says so
    /// instead of offering buttons that could not work.
    static bool loadable_backends_supported();

    /// Look at the runtimes directory and at what ggml actually registered.
    static std::vector<RuntimeStatus> scan();

    /// Delete a runtime's module files. The CPU runtime refuses to be removed:
    /// it is the fallback, and an install with nothing left cannot answer.
    /// Takes effect on the next start, since ggml cannot unload a backend that
    /// models may still be using.
    static bool remove(BackendKind kind, std::string& error);

private:
    /// Recorded next to the modules so a runtime can say where it came from.
    static std::filesystem::path manifest_file();
};

}  // namespace batbot
