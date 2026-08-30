// SPDX-License-Identifier: MIT
//
// The table of compute backends BatBot can run on.
//
// A "runtime" is one loadable ggml backend: a shared library that teaches
// llama.cpp how to talk to a piece of hardware. BatBot does not compile any of
// them into the binary. They are files in a directory, built on demand and
// removable, which is what lets the settings screen add CUDA to an install
// that was CPU-only without rebuilding anything.
//
// This file is the single source of truth for what a backend is called, what
// it needs, and how it is built -- everything else derives from the table.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace batbot {

/// A backend BatBot knows how to build and load.
///
/// Adding one is a single entry in `all_backends()`: the builder, the settings
/// screen and the dependency check all read the table rather than switching on
/// the enum.
enum class BackendKind {
    Cpu,     ///< always available; the floor everything else falls back to
    Cuda,    ///< NVIDIA, via the CUDA toolkit
    Vulkan,  ///< any GPU with a Vulkan driver, including NVIDIA and AMD
};

inline constexpr std::size_t kBackendCount = 3;

/// Everything static about one backend.
struct BackendInfo {
    BackendKind      kind;
    std::string_view id;      ///< "cuda" -- also the ggml module name and the config value
    std::string_view name;    ///< "CUDA" -- as shown in settings
    std::string_view blurb;   ///< one line explaining when to pick it

    /// The ggml CMake option that turns this backend on, e.g. "GGML_CUDA".
    std::string_view cmake_option;

    /// The build target that produces the module, e.g. "ggml-cuda".
    std::string_view build_target;

    /// A program that must be on PATH to build it, or empty when the base
    /// toolchain is enough. This is what turns "the build failed" into "you
    /// need nvcc, install it with ...".
    std::string_view required_tool;

    /// Package to suggest when `required_tool` is missing, per package manager.
    std::string_view apt_packages;
    std::string_view dnf_packages;
    std::string_view pacman_packages;

    /// True when this backend can spread one model over several devices, which
    /// is what makes the GPU-split settings meaningful. The CPU backend cannot.
    bool multi_device;

    /// True when llama.cpp cannot load a model without this backend, whatever
    /// else is installed. Only the CPU one is: llama.cpp keeps the output
    /// layer and a few buffer types on the host no matter which GPU is doing
    /// the work, and throws "no CPU backend found" when there is none. So
    /// installing CUDA on its own is not enough, and the runtime builder
    /// quietly builds this one alongside whatever you asked for.
    bool required;
};

/// The backend table, in the order settings lists them.
const std::array<BackendInfo, kBackendCount>& all_backends();

/// Look up one entry. `kind` is always valid, so this never fails.
const BackendInfo& backend_info(BackendKind kind);

/// Parse an id as written in the config or a runtime manifest.
std::optional<BackendKind> backend_from_id(std::string_view id);

/// Map a ggml backend registry name ("CUDA", "Vulkan", "CPU") onto a kind.
/// Returns nothing for a backend BatBot does not manage, which is not an
/// error -- it just means the device came from somewhere else.
std::optional<BackendKind> backend_from_reg_name(std::string_view reg_name);

/// What a loadable backend module is called on this platform.
///
/// ggml opens its modules by an exact file name, so these have to agree with
/// ggml_backend_load_all_from_path down to the character: "libggml-cuda.so"
/// everywhere except Windows, where there is no `lib` and the extension is
/// ".dll". macOS is not the odd one out here -- CMake gives a MODULE library
/// the ".so" suffix there too, which is why ggml only special-cases Windows.
///
/// Everything that scans, installs or names a module goes through these rather
/// than writing ".so" inline, so porting is one edit rather than a search.
std::string_view module_prefix();
std::string_view module_suffix();

}  // namespace batbot
