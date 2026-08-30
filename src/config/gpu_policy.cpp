// SPDX-License-Identifier: MIT
//
// Applying the GPU split policy to every model that will be loaded.
#include "batbot/config/gpu_policy.hpp"

#include <filesystem>
#include <map>
#include <string>

#include "batbot/llm/model_shape.hpp"
#include "batbot/runtime/devices.hpp"

namespace batbot {
namespace {

/// GGUF headers already read during this call.
///
/// Nine experts usually name the same one or two files, and reading a header
/// costs tens of milliseconds; without this, applying a config would spend
/// half a second re-reading the same file.
using ShapeCache = std::map<std::string, ModelShape>;

const ModelShape& shape_of(ShapeCache& cache, const std::string& path) {
    static const ModelShape kNothing;
    if (path.empty()) {
        return kNothing;
    }
    const auto found = cache.find(path);
    if (found != cache.end()) {
        return found->second;
    }
    return cache.emplace(path, read_model_shape(path)).first->second;
}

void stamp(ModelParams& params, const std::vector<float>& split, int main_gpu) {
    params.tensor_split = split;
    params.main_gpu     = main_gpu;
}

/// Turn the two memory settings into the llama.cpp knobs they mean.
///
/// Applied to every model including the delegator, and applied whatever the
/// split mode is -- "where the compute happens" is a separate question from
/// "how it is divided between cards", and "auto" is an answer to the second
/// one only.
void stamp_memory(ModelParams& params, const GpuConfig& gpu, bool have_gpu) {
    if (have_gpu && gpu.gpu_only) {
        // Negative means every layer plus the output, which is what llama.cpp
        // does with -1 (see llama_model::n_gpu_layers). Overriding the
        // configured count is the point: a number left behind in the config is
        // exactly how a model ends up half on the processor.
        params.n_gpu_layers = -1;
        params.no_host      = true;
        params.gpu_only     = true;
    }
    if (gpu.vram_only) {
        params.no_host   = true;
        params.direct_io = true;
        params.vram_only = true;
    }
}

}  // namespace

std::string apply_gpu_policy(Config& config) {
    const std::vector<ComputeDevice> all_gpus = gpu_devices();
    const bool have_gpu = !all_gpus.empty();

    stamp_memory(config.router, config.gpu, have_gpu);
    stamp_memory(config.defaults, config.gpu, have_gpu);
    for (ModelParams& expert : config.experts) {
        stamp_memory(expert, config.gpu, have_gpu);
    }

    const GpuSplitMode mode = gpu_split_mode_from_id(config.gpu.mode);
    if (mode == GpuSplitMode::Auto) {
        return {};
    }

    const std::vector<ComputeDevice>& gpus = all_gpus;
    if (gpus.size() < 2 && mode != GpuSplitMode::Single) {
        // Nothing to divide. Saying so is better than silently writing a
        // one-element split that looks like it did something.
        return {};
    }

    // The delegator is small and lives on one device for its whole life;
    // splitting a 1B model across three cards costs more in transfers than it
    // saves in memory. Experts are the ones worth spreading.
    stamp(config.router, {}, config.gpu.main_gpu);

    // A split is per-model, not per-machine. Priority mode fills the cards in
    // order, and how far down the order a model reaches depends on how big it
    // is -- a 1B expert never leaves the first card, a 30B one uses all three.
    // So each seat gets the split computed for the file it actually names.
    //
    // Weights plus KV cache are what gets divided, because both follow the
    // layers. The compute buffers do not -- every card needs its own -- so
    // they are set aside from each card's capacity rather than shared out.
    ShapeCache shapes;
    const auto split_for = [&](const ModelParams& params) {
        const ModelShape& shape = shape_of(shapes, params.path);
        return compute_tensor_split(mode, gpus, config.gpu.priority, config.gpu.main_gpu,
                                    ModelFit{shape.resident_bytes(params.n_ctx),
                                             shape.compute_bytes(params.n_batch)});
    };

    std::vector<float> described;
    std::uint64_t      described_bytes = 0;
    std::string        described_model;
    for (ModelParams& expert : config.experts) {
        const std::vector<float> split = split_for(expert);
        if (split.empty()) {
            continue;
        }
        stamp(expert, split, config.gpu.main_gpu);
        // Keep the largest expert's arrangement: it is the one worth reporting,
        // the one most likely not to fit, and the sensible thing to hand to
        // `defaults` below.
        if (const std::uint64_t bytes = shape_of(shapes, expert.path).weights;
            described.empty() || bytes > described_bytes) {
            described       = split;
            described_bytes = bytes;
            described_model = expert.model;
        }
    }

    // `defaults` names no file of its own, so there is no model to fill the
    // cards with and no split that is really "its". It still has to be stamped
    // -- a hand-written tensor_split left in the config file would otherwise
    // survive a mode that is supposed to override it -- so it takes the
    // largest expert's, or a capacity-proportional one when no seat is filled.
    const std::vector<float> defaults_split =
        described.empty() ? split_for(config.defaults) : described;
    if (!defaults_split.empty()) {
        stamp(config.defaults, defaults_split, config.gpu.main_gpu);
    }
    if (described.empty()) {
        described = defaults_split;
    }

    if (described.empty()) {
        return {};
    }
    // Name the model. The split is worked out per-model now, so a bare list of
    // percentages would be a fact about something the reader has to guess at.
    const std::string layout = describe_split(gpus, described);
    return described_model.empty() ? layout : described_model + " -- " + layout;
}

}  // namespace batbot
