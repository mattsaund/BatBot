// SPDX-License-Identifier: MIT
//
// Applying the GPU split policy to every model that will be loaded.
#include "batbot/config/gpu_policy.hpp"

#include "batbot/runtime/devices.hpp"

namespace batbot {
namespace {

void stamp(ModelParams& params, const std::vector<float>& split, int main_gpu) {
    params.tensor_split = split;
    params.main_gpu     = main_gpu;
}

}  // namespace

std::string apply_gpu_policy(Config& config) {
    const GpuSplitMode mode = gpu_split_mode_from_id(config.gpu.mode);
    if (mode == GpuSplitMode::Auto) {
        return {};
    }

    const std::vector<ComputeDevice> gpus = gpu_devices();
    if (gpus.size() < 2 && mode != GpuSplitMode::Single) {
        // Nothing to divide. Saying so is better than silently writing a
        // one-element split that looks like it did something.
        return {};
    }

    const std::vector<float> split =
        compute_tensor_split(mode, gpus, config.gpu.priority, config.gpu.main_gpu);
    if (split.empty()) {
        return {};
    }

    // The delegator is small and lives on one device for its whole life;
    // splitting a 1B model across three cards costs more in transfers than it
    // saves in memory. Experts are the ones worth spreading.
    stamp(config.router, {}, config.gpu.main_gpu);
    stamp(config.defaults, split, config.gpu.main_gpu);
    for (ModelParams& expert : config.experts) {
        stamp(expert, split, config.gpu.main_gpu);
    }

    return describe_split(gpus, split);
}

}  // namespace batbot
