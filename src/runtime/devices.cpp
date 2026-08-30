// SPDX-License-Identifier: MIT
//
// Device enumeration and the GPU split policy. See devices.hpp for the why.
#include "batbot/runtime/devices.hpp"

#include <algorithm>
#include <numeric>

#include <ggml-backend.h>

#include "batbot/util/format.hpp"

namespace batbot {

std::uint64_t usable_memory(const ComputeDevice& gpu, std::uint64_t reserve) {
    // Free when the backend reports it, total when it does not. A backend that
    // reports neither gets nothing, and the caller falls back to an equal
    // share rather than dividing by zero.
    const std::uint64_t bytes = gpu.memory_free > 0 ? gpu.memory_free : gpu.memory_total;
    return bytes > reserve ? bytes - reserve : 0;
}

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

std::vector<ComputeDevice> apply_priority_order(const std::vector<ComputeDevice>& gpus,
                                                const std::vector<int>& order) {
    std::vector<ComputeDevice> arranged;
    arranged.reserve(gpus.size());

    const auto already_taken = [&arranged](int index) {
        return std::any_of(arranged.begin(), arranged.end(),
                           [index](const ComputeDevice& gpu) { return gpu.index == index; });
    };

    for (const int index : order) {
        const auto found = std::find_if(gpus.begin(), gpus.end(),
                                        [index](const ComputeDevice& gpu) {
                                            return gpu.index == index;
                                        });
        // An index listed twice would take two places in the order, and the
        // second one would silently push a real card down the list.
        if (found != gpus.end() && !already_taken(index)) {
            arranged.push_back(*found);
        }
    }

    for (const ComputeDevice& gpu : gpus) {
        if (!already_taken(gpu.index)) {
            arranged.push_back(gpu);
        }
    }
    return arranged;
}

std::vector<float> compute_tensor_split(GpuSplitMode mode,
                                        const std::vector<ComputeDevice>& gpus,
                                        const std::vector<int>& order,
                                        int main_gpu,
                                        ModelFit fit) {
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
        // Priority: fill each card in the stated order up to what it can
        // actually hold, and give the next card only what is left over. That
        // is what "priority" means, and it is the one arrangement that keeps a
        // model whole on the fastest card when it fits there.
        //
        // This used to be a fixed geometric falloff -- 1, 0.25, 0.0625 -- which
        // took no account of how large the cards were or how large the model
        // was. On a 12/16/12 GB machine it asked the first-ranked card for 76%
        // of every model, so a 32 GB model was handed 24 GB of weights for a
        // 16 GB card and failed to allocate, with 13 GB left untouched on the
        // other two. A proportion that ignores capacity is not a priority
        // order; it is a fixed ratio wearing one as a hat.
        // The order itself comes from apply_priority_order, which is the same
        // rule the GPU priority screen lays its list out with -- a card the
        // config names that is no longer plugged in is dropped, one that has
        // appeared since lands at the end.
        const std::vector<ComputeDevice> ranked = apply_priority_order(gpus, order);

        std::uint64_t remaining = fit.resident;
        const bool    filled    = fit.resident > 0;
        if (filled) {
            for (const ComputeDevice& gpu : ranked) {
                const std::uint64_t take = std::min(usable_memory(gpu, fit.per_card), remaining);
                split[static_cast<std::size_t>(gpu.index)] = static_cast<float>(take);
                remaining -= take;
            }
        }

        // Either the size is unknown, or the model does not fit in the cards at
        // all. Divide by capacity: it will not load either way, but a split
        // proportional to what each card holds is the arrangement that gets
        // closest, and it never hands a card more than the others.
        //
        // When it does not fit, "Dedicated VRAM only" is what turns this into
        // a message instead of a driver spilling into system RAM.
        if (!filled || remaining > 0) {
            for (const ComputeDevice& gpu : ranked) {
                const std::uint64_t capacity = usable_memory(gpu, fit.per_card);
                split[static_cast<std::size_t>(gpu.index)] =
                    capacity > 0 ? static_cast<float>(capacity) : 1.0F;
            }
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
