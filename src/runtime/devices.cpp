// SPDX-License-Identifier: MIT
//
// Device enumeration and the GPU split policy. See devices.hpp for the why.
#include "batbot/runtime/devices.hpp"

#include <algorithm>
#include <numeric>

#include <ggml-backend.h>

#include "batbot/util/format.hpp"

namespace batbot {
namespace {

/// Priority mode's weights.
///
/// llama.cpp reads tensor_split as proportions, not as "fill this one first",
/// so a true fill-in-order is not expressible. What is expressible is a steep
/// enough bias that the first device takes nearly everything it can hold and
/// the rest take the remainder -- which is the behaviour people actually want
/// from "priority". Each subsequent device gets a quarter of the weight of the
/// one before it, floored by its own capacity so a small card is never handed
/// more than it can fit.
constexpr float kPriorityFalloff = 0.25F;

}  // namespace

std::string ComputeDevice::label() const {
    std::string text = description.empty() ? name : description;
    if (memory_total > 0) {
        text += " (" + format::bytes(memory_total) + ")";
    }
    return text;
}

std::vector<ComputeDevice> compute_devices() {
    std::vector<ComputeDevice> devices;
    const std::size_t count = ggml_backend_dev_count();
    devices.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        ggml_backend_dev_t handle = ggml_backend_dev_get(i);
        if (handle == nullptr) {
            continue;
        }

        ComputeDevice device;
        device.index = static_cast<int>(i);
        if (const char* name = ggml_backend_dev_name(handle); name != nullptr) {
            device.name = name;
        }
        if (const char* description = ggml_backend_dev_description(handle); description != nullptr) {
            device.description = description;
        }
        if (ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(handle); reg != nullptr) {
            if (const char* reg_name = ggml_backend_reg_name(reg); reg_name != nullptr) {
                device.backend = reg_name;
            }
        }

        std::size_t free_bytes = 0;
        std::size_t total_bytes = 0;
        ggml_backend_dev_memory(handle, &free_bytes, &total_bytes);
        device.memory_free  = free_bytes;
        device.memory_total = total_bytes;

        device.is_gpu = ggml_backend_dev_type(handle) == GGML_BACKEND_DEVICE_TYPE_GPU;
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<ComputeDevice> gpu_devices() {
    std::vector<ComputeDevice> gpus;
    for (ComputeDevice& device : compute_devices()) {
        if (device.is_gpu) {
            gpus.push_back(std::move(device));
        }
    }
    return gpus;
}

std::string_view gpu_split_mode_id(GpuSplitMode mode) {
    switch (mode) {
        case GpuSplitMode::Auto:     return "auto";
        case GpuSplitMode::Even:     return "even";
        case GpuSplitMode::Priority: return "priority";
        case GpuSplitMode::Single:   return "single";
    }
    return "auto";
}

GpuSplitMode gpu_split_mode_from_id(std::string_view id) {
    if (id == "even")     { return GpuSplitMode::Even; }
    if (id == "priority") { return GpuSplitMode::Priority; }
    if (id == "single")   { return GpuSplitMode::Single; }
    return GpuSplitMode::Auto;
}

std::vector<float> compute_tensor_split(GpuSplitMode mode,
                                        const std::vector<ComputeDevice>& gpus,
                                        const std::vector<int>& order,
                                        int main_gpu) {
    // With one GPU there is nothing to divide, and an explicit split would
    // only be a way to get it wrong.
    if (gpus.size() < 2 || mode == GpuSplitMode::Auto) {
        return {};
    }

    // tensor_split is indexed by ggml device index, not by position in the GPU
    // list, so the vector has to be long enough to reach the highest one.
    std::size_t width = 0;
    for (const ComputeDevice& gpu : gpus) {
        width = std::max(width, static_cast<std::size_t>(gpu.index) + 1);
    }
    std::vector<float> split(width, 0.0F);

    if (mode == GpuSplitMode::Single) {
        const auto chosen = std::find_if(gpus.begin(), gpus.end(), [main_gpu](const ComputeDevice& gpu) {
            return gpu.index == main_gpu;
        });
        const int index = chosen != gpus.end() ? chosen->index : gpus.front().index;
        split[static_cast<std::size_t>(index)] = 1.0F;
        return split;
    }

    if (mode == GpuSplitMode::Even) {
        // "Even" means even in work, not even in count: a 16 GB card should
        // take twice the layers of an 8 GB one, or the small card runs out
        // first and the split fails for a model that would otherwise fit.
        // Devices that report no memory fall back to an equal share.
        const bool have_memory = std::all_of(gpus.begin(), gpus.end(), [](const ComputeDevice& gpu) {
            return gpu.memory_total > 0;
        });
        for (const ComputeDevice& gpu : gpus) {
            split[static_cast<std::size_t>(gpu.index)] =
                have_memory ? static_cast<float>(gpu.memory_total) : 1.0F;
        }
    } else {
        // Priority: walk the stated order, giving each device a fraction of
        // what the one before it got. Devices the user did not rank come last,
        // in ggml order, so a new GPU appearing never silently outranks one
        // that was deliberately placed.
        std::vector<int> ranked;
        for (const int index : order) {
            const bool known = std::any_of(gpus.begin(), gpus.end(), [index](const ComputeDevice& gpu) {
                return gpu.index == index;
            });
            const bool already = std::find(ranked.begin(), ranked.end(), index) != ranked.end();
            if (known && !already) {
                ranked.push_back(index);
            }
        }
        for (const ComputeDevice& gpu : gpus) {
            if (std::find(ranked.begin(), ranked.end(), gpu.index) == ranked.end()) {
                ranked.push_back(gpu.index);
            }
        }

        float weight = 1.0F;
        for (const int index : ranked) {
            split[static_cast<std::size_t>(index)] = weight;
            weight *= kPriorityFalloff;
        }
    }

    const float total = std::accumulate(split.begin(), split.end(), 0.0F);
    if (total <= 0.0F) {
        return {};
    }
    for (float& share : split) {
        share /= total;
    }
    return split;
}

std::string describe_split(const std::vector<ComputeDevice>& gpus,
                           const std::vector<float>& split) {
    if (split.empty()) {
        return "llama.cpp decides";
    }

    std::string text;
    for (const ComputeDevice& gpu : gpus) {
        const auto index = static_cast<std::size_t>(gpu.index);
        if (index >= split.size() || split[index] <= 0.0F) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        const std::string name = gpu.description.empty() ? gpu.name : gpu.description;
        text += name + " " + format::number(static_cast<double>(split[index]) * 100.0, 0) + "%";
    }
    return text.empty() ? "llama.cpp decides" : text;
}

}  // namespace batbot
