// SPDX-License-Identifier: MIT
//
// The compute devices ggml found, and how a model is spread across them.
//
// A machine with three GPUs can hold a far larger expert than any one of them,
// but only if the layers are divided sensibly. "Sensibly" is not one rule:
// three identical cards want an even split, while a 16 GB card next to two
// 8 GB ones usually wants the big one filled first. Both are offered.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace batbot {

/// One device ggml registered, in ggml's own index order -- which is what
/// `main_gpu` and `tensor_split` are indexed by.
struct ComputeDevice {
    int         index = 0;
    std::string name;         ///< ggml's short name, e.g. "CUDA0"
    std::string description;  ///< the human one, e.g. "NVIDIA GeForce RTX 4070"
    std::string backend;      ///< the registry that owns it, e.g. "CUDA"

    std::uint64_t memory_total = 0;
    std::uint64_t memory_free  = 0;

    /// False for the CPU device, which is always present and is never part of
    /// a GPU split.
    bool is_gpu = false;

    /// "NVIDIA GeForce RTX 4070 (8.0 GB)"
    std::string label() const;
};

/// Every device ggml knows about. Empty until a runtime has been loaded.
std::vector<ComputeDevice> compute_devices();

/// Just the GPUs, in ggml index order.
std::vector<ComputeDevice> gpu_devices();

/// How work is divided between GPUs.
enum class GpuSplitMode {
    /// Let llama.cpp decide. One GPU, or a backend that cannot split, ends up
    /// here whatever else is configured.
    Auto,
    /// Proportional to each card's memory, so every GPU finishes its share at
    /// about the same time. The right default for cards of different sizes.
    Even,
    /// Fill the GPUs in a stated order, spilling into the next only when the
    /// previous is full. Keeps a model whole on the fastest card when it fits,
    /// and leaves the others free for something else.
    Priority,
    /// Everything on one device, named by `main_gpu`.
    Single,
};

std::string_view gpu_split_mode_id(GpuSplitMode mode);
GpuSplitMode     gpu_split_mode_from_id(std::string_view id);

/// Lay a configured priority order over the devices that actually exist.
///
/// Returns `gpus` rearranged: the ones `order` names first, in that order, then
/// everything `order` did not mention, in ggml index order. An index naming a
/// card that is not in the machine is dropped rather than leaving a hole, and a
/// card that appeared since the order was written lands at the end instead of
/// vanishing -- so a config written with two GPUs plugged in still means
/// something on the day only one is.
std::vector<ComputeDevice> apply_priority_order(const std::vector<ComputeDevice>& gpus,
                                                const std::vector<int>& order);

/// Turn a split mode into the `tensor_split` vector llama.cpp wants: one
/// weight per device, in ggml index order, summing to 1.
///
/// `order` lists device indices best-first and is only read in Priority mode.
/// An empty result means "no opinion", which llama.cpp reads as its default.
std::vector<float> compute_tensor_split(GpuSplitMode mode,
                                        const std::vector<ComputeDevice>& gpus,
                                        const std::vector<int>& order,
                                        int main_gpu);

/// A one-line explanation of what a split will actually do, for the settings
/// screen -- "RTX 4070 62%, RTX 3060 38%".
std::string describe_split(const std::vector<ComputeDevice>& gpus,
                           const std::vector<float>& split);

}  // namespace batbot
