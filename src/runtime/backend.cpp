// SPDX-License-Identifier: MIT
//
// The backend table. See backend.hpp for why everything derives from it.
#include "batbot/runtime/backend.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace batbot {
namespace {

// Order matters: this is the order the settings screen lists runtimes in, and
// CPU comes first because it is the one that is always there.
constexpr std::array<BackendInfo, kBackendCount> kBackends{{
    {BackendKind::Cpu, "cpu", "CPU",
     "Runs on the processor. Always works, and the slowest option by a wide margin.",
     "GGML_CPU", "ggml-cpu",
     /*required_tool=*/"",
     /*apt=*/"build-essential", /*dnf=*/"gcc-c++ make", /*pacman=*/"base-devel",
     /*multi_device=*/false, /*removable=*/false},

    {BackendKind::Cuda, "cuda", "CUDA",
     "NVIDIA cards, using the CUDA toolkit. The fastest option on NVIDIA hardware.",
     "GGML_CUDA", "ggml-cuda",
     /*required_tool=*/"nvcc",
     /*apt=*/"nvidia-cuda-toolkit",
     /*dnf=*/"cuda-toolkit",
     /*pacman=*/"cuda",
     /*multi_device=*/true, /*removable=*/true},

    {BackendKind::Vulkan, "vulkan", "Vulkan",
     "Any GPU with a Vulkan driver -- NVIDIA, AMD or Intel. Slower than CUDA, "
     "but needs only the graphics driver you already have.",
     "GGML_VULKAN", "ggml-vulkan",
     /*required_tool=*/"glslc",
     // spirv-headers is easy to miss and the build fails at configure time
     // without it, several minutes in, with an error that names a CMake
     // package rather than anything installable.
     /*apt=*/"glslc libvulkan-dev spirv-headers",
     /*dnf=*/"glslc vulkan-loader-devel spirv-headers-devel",
     /*pacman=*/"shaderc vulkan-headers vulkan-icd-loader spirv-headers",
     /*multi_device=*/true, /*removable=*/true},
}};

}  // namespace

const std::array<BackendInfo, kBackendCount>& all_backends() {
    return kBackends;
}

const BackendInfo& backend_info(BackendKind kind) {
    return kBackends[static_cast<std::size_t>(kind)];
}

std::optional<BackendKind> backend_from_id(std::string_view id) {
    for (const BackendInfo& info : kBackends) {
        if (info.id == id) {
            return info.kind;
        }
    }
    return std::nullopt;
}

std::optional<BackendKind> backend_from_reg_name(std::string_view reg_name) {
    // ggml reports "CUDA", "Vulkan", "CPU"; our ids are lower case.
    std::string lowered(reg_name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return backend_from_id(lowered);
}

}  // namespace batbot
