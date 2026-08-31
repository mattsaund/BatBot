// SPDX-License-Identifier: MIT
//
// Owning llama.cpp.
//
// Two responsibilities that must not be separated: redirecting llama.cpp's
// logging away from stderr (it would draw straight over the TUI), and enforcing
// the rule that makes BatBot's memory budget work -- the small delegator stays
// resident, and at most one large expert is loaded at any moment.
//
// Every function here must be called from the same thread. BatBot runs it on
// the engine worker so the UI never blocks behind a model load.
#include "batbot/llm/model_host.hpp"

#include "batbot/util/text.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include <ggml-backend.h>
#include <llama.h>

#include "batbot/config/gpu_policy.hpp"
#include "batbot/llm/model_shape.hpp"
#include "batbot/llm/sampling.hpp"
#include "batbot/runtime/devices.hpp"
#include "batbot/runtime/registry.hpp"
#include "batbot/util/format.hpp"

namespace batbot {
namespace {


// --- llama.cpp logging -----------------------------------------------------
// llama.cpp writes progress and diagnostics to stderr by default. In a
// fullscreen TUI that lands on top of the interface, so everything is diverted
// to a file. The callback is global and can fire from llama.cpp's own threads,
// hence the mutex.
std::mutex   g_log_mutex;
std::ofstream g_log_stream;

void log_to_file(ggml_log_level level, const char* text, void* /*user_data*/) {
    if (text == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_stream.is_open()) {
        return;
    }
    const char* tag = "info ";
    switch (level) {
        case GGML_LOG_LEVEL_ERROR: tag = "error"; break;
        case GGML_LOG_LEVEL_WARN:  tag = "warn "; break;
        case GGML_LOG_LEVEL_DEBUG: tag = "debug"; break;
        default: break;
    }
    g_log_stream << tag << " | " << text;
    g_log_stream.flush();
}

/// Bridges llama.cpp's C progress callback to the std::function the UI gave us.
/// Returning false from the C callback aborts the load, which is how a cancel
/// during a multi-second expert swap actually takes effect.
struct ProgressBridge {
    const ProgressCallback* progress = nullptr;
    float                   last     = -1.0F;
};

bool progress_trampoline(float progress, void* user_data) {
    auto* bridge = static_cast<ProgressBridge*>(user_data);
    // An empty std::function is a legal argument -- "load this, I do not care
    // how far along it is" -- and calling one throws std::bad_function_call
    // out through llama.cpp's C boundary, where it surfaces as an unexplained
    // "failed to load model".
    if (bridge == nullptr || bridge->progress == nullptr || !*bridge->progress) {
        return true;
    }
    // llama.cpp calls this per tensor, which is far more often than a terminal
    // can usefully redraw. Throttle to whole percent.
    if (progress - bridge->last >= 0.01F || progress >= 1.0F) {
        bridge->last = progress;
        (*bridge->progress)(progress);
    }
    return true;
}

llama_split_mode split_mode_from_string(const std::string& name) {
    if (name == "none")   { return LLAMA_SPLIT_MODE_NONE; }
    if (name == "row")    { return LLAMA_SPLIT_MODE_ROW; }
    if (name == "tensor") { return LLAMA_SPLIT_MODE_TENSOR; }
    return LLAMA_SPLIT_MODE_LAYER;
}

/// Will this model fit in the free memory of the cards it would land on?
///
/// Returns an empty string when it fits (or when there is nothing to check --
/// no GPU, or a backend that does not report its memory), and an explanation
/// when it does not.
///
/// The figure is read from the model rather than guessed at. This used to be
/// the file size times a flat 1.25, which refused a 32 GB model on 34.8 GB of
/// free video memory: the allowance invented eight gigabytes of KV cache for a
/// model whose cache is under one. See model_shape.hpp.
///
/// `held` is what BatBot itself already has on the cards, and `holder` names
/// it -- the delegator, almost always. It is not subtracted, because whatever
/// is holding it is staying; it is named in the message, because on a machine
/// where a large expert only just fits, that share is the number the user can
/// actually act on.
std::string vram_shortfall(const std::string& path, const ModelParams& params,
                           std::uint64_t held, const std::string& holder) {
    const ModelShape shape = read_model_shape(path);
    if (shape.weights == 0) {
        return {};
    }

    const std::vector<ComputeDevice> gpus = gpu_devices();
    if (gpus.empty()) {
        return {};
    }

    // Which cards this model would actually land on, which is not always all
    // of them: "single" mode, and a priority order that gives a card nothing,
    // both leave some out -- and counting memory the model will never be
    // allowed to touch is how a check like this waves through the case it
    // exists to catch.
    //
    // tensor_split is indexed by ggml device index, the same number as
    // ComputeDevice::index, and a zero share means "not used". An empty split
    // is llama.cpp's own default: every GPU.
    const std::vector<float>& split = params.tensor_split;
    const auto share_of = [&split](const ComputeDevice& gpu) {
        const auto index = static_cast<std::size_t>(gpu.index);
        return index < split.size() ? split[index] : 0.0F;
    };
    const bool everywhere = split.empty();

    // LLAMA_SPLIT_MODE_NONE puts the whole model on main_gpu whatever the
    // split says.
    const bool single = params.split_mode == "none";

    std::uint64_t available = 0;
    std::uint64_t cards     = 0;
    std::string   where;
    for (const ComputeDevice& gpu : gpus) {
        if (single ? gpu.index != params.main_gpu
                   : (!everywhere && share_of(gpu) <= 0.0F)) {
            continue;
        }
        available += gpu.memory_free;
        ++cards;
        if (!where.empty()) {
            where += " + ";
        }
        where += (gpu.description.empty() ? gpu.name : gpu.description);
    }
    if (available == 0) {
        // A backend that does not report free memory. Nothing to compare
        // against, so let the load proceed rather than refuse on a guess.
        return {};
    }

    // Weights, plus the cache at the context actually configured, plus the
    // compute buffers -- less the input embedding, which llama.cpp keeps in
    // system memory whatever the offload settings say. A model whose header
    // could not be read falls back to a tenth over the file size: small enough
    // not to refuse anything that would have fitted, which is the right way to
    // be wrong when the numbers are unknown.
    std::uint64_t needed =
        static_cast<std::uint64_t>(static_cast<double>(shape.weights) * 1.1);
    if (shape.known) {
        // Every card pays for its own activations, so the allowance is counted
        // once per card rather than once per model -- the same arithmetic the
        // split planner does. A check that counted it once would pass a model
        // the planner then could not place, which is the one way for these two
        // to disagree that ends in a failed load rather than a message.
        needed = shape.resident_bytes(params.n_ctx) - shape.host_weights +
                 (shape.compute_bytes(params.n_batch) - shape.logit_bytes(params.n_batch)) *
                     std::max<std::uint64_t>(1, cards) +
                 shape.logit_bytes(params.n_batch);
    }
    if (needed <= available) {
        return {};
    }

    std::string detail;
    if (shape.known && shape.kv_bytes(params.n_ctx) > 0) {
        // Which part is too big matters: the weights cannot be changed, but
        // the context can, and it is the one number on the settings screen
        // that would make this fit.
        detail = " (" + format::bytes(shape.weights - shape.host_weights) +
                 " of weights plus " + format::bytes(shape.kv_bytes(params.n_ctx)) +
                 " of context at " + std::to_string(params.n_ctx) + " tokens)";
    }

    std::string advice = " -- close something using the GPU, lower the context size, "
                         "or turn off \"Dedicated VRAM only\" in settings";
    if (held > 0 && !holder.empty()) {
        advice = " -- " + holder + " is holding " + format::bytes(held) + " of that. "
                 "Lower the context size, use a smaller " + holder + ", or turn off "
                 "\"Dedicated VRAM only\" in settings";
    }

    return "needs about " + format::bytes(needed) + " of video memory" + detail +
           " but only " + format::bytes(available) + " is free on " + where + advice;
}

int resolve_threads(int configured) {
    if (configured > 0) {
        return configured;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    // Physical cores generally beat logical ones for GGML; halving is a decent
    // approximation without probing topology.
    return hardware > 1 ? static_cast<int>(hardware / 2) : 1;
}



}  // namespace

// ---------------------------------------------------------------------------
// ModelHost
// ---------------------------------------------------------------------------

ModelHost::ModelHost(std::filesystem::path log_path) {
    {
        std::error_code ec;
        std::filesystem::create_directories(log_path.parent_path(), ec);
        const std::lock_guard<std::mutex> lock(g_log_mutex);
        g_log_stream.open(log_path, std::ios::out | std::ios::trunc);
    }
    // Install the diversion before backend init so even startup chatter about
    // devices and backends goes to the file rather than over the TUI.
    llama_log_set(log_to_file, nullptr);

    // Bring the loadable runtimes in before llama.cpp initialises, so the
    // devices they provide are there from the first model load. A fresh
    // install has none: BatBot ships no backends, and this does nothing until
    // one has been built from the settings screen.
    RuntimeRegistry::load_all();

    llama_backend_init();
}

ModelHost::~ModelHost() {
    expert_.reset();
    router_.reset();
    llama_backend_free();

    const std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_stream.is_open()) {
        g_log_stream.close();
    }
}

std::uint64_t ModelHost::resident_bytes() const {
    std::uint64_t total = 0;
    if (router_) {
        total += router_->bytes();
    }
    if (expert_) {
        total += expert_->bytes();
    }
    return total;
}

std::vector<std::string> ModelHost::devices() {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        if (device == nullptr) {
            continue;
        }
        const char* description = ggml_backend_dev_description(device);
        names.emplace_back(description != nullptr ? description : "unknown device");
    }
    return names;
}

std::unique_ptr<LoadedModel> ModelHost::load(const ModelParams& requested,
                                             Role role,
                                             const ProgressCallback& progress,
                                             std::string& error) {
    if (requested.path.empty()) {
        error = "no model file configured";
        return nullptr;
    }
    if (!std::filesystem::exists(requested.path)) {
        error = "model file not found: " + requested.path;
        return nullptr;
    }
    // Ask before llama.cpp does. With no backend registered it throws "no CPU
    // backend found" from somewhere deep in the loader, which is a true
    // statement about ggml and a useless one about what to do next.
    if (ggml_backend_dev_count() == 0) {
        error = "no runtime installed -- type /runtimes and install one";
        return nullptr;
    }

    // Re-plan the split against the memory that is free at this instant.
    //
    // The one stamped on the config was worked out at startup, before the
    // delegator was loaded and before the desktop had done whatever it has
    // since done with the display card. Handing llama.cpp a plan for memory
    // that is no longer there is not a near miss: the load runs to the very
    // last allocation with 32 GB already uploaded and then fails on 48 MB of
    // KV cache. See refresh_gpu_split.
    ModelParams params = requested;
    if (role == Role::Delegator) {
        place_delegator(params, gpu_);
    } else {
        refresh_gpu_split(params, gpu_);
    }

    // Refuse before allocating anything, when asked to. A driver handed more
    // than the card holds does not fail -- it spills into system RAM and runs
    // on at a crawl -- so this is the only point at which saying no is cheap.
    if (params.vram_only) {
        // What is already on the cards, and what it is. Loading an expert
        // releases the previous one first, so the only thing resident then is
        // the delegator -- and loading the delegator after a settings change is
        // the mirror image of that.
        const std::uint64_t held = resident_bytes();
        const std::string   holder =
            role == Role::Expert ? "the delegator" : "the resident expert";
        if (const std::string shortfall = vram_shortfall(params.path, params, held, holder);
            !shortfall.empty()) {
            error = std::filesystem::path(params.path).filename().string() + " " + shortfall;
            return nullptr;
        }
    }

    ProgressBridge bridge{&progress, -1.0F};

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = params.n_gpu_layers;
    model_params.main_gpu     = params.main_gpu;
    model_params.split_mode   = split_mode_from_string(params.split_mode);
    model_params.progress_callback = progress_trampoline;
    model_params.progress_callback_user_data = &bridge;
    if (!params.tensor_split.empty()) {
        model_params.tensor_split = params.tensor_split.data();
    }

    // Keep the weights off the host. `no_host` drops the pinned staging buffer
    // llama.cpp would otherwise keep in RAM for transfers to the card, and
    // direct I/O reads the file straight to the device instead of through the
    // operating system's page cache -- which would otherwise hold a second,
    // full-size copy of every model in RAM long after it was uploaded.
    model_params.no_host = params.no_host;
    if (params.direct_io) {
        model_params.load_mode = LLAMA_LOAD_MODE_DIRECT_IO;
    }

    llama_model* model = llama_model_load_from_file(params.path.c_str(), model_params);
    if (model == nullptr) {
        error = "llama.cpp could not load " + params.path
              + " (see the BatBot log for details)";
        // The most likely reason, and the one the user can act on. With every
        // layer pinned to the GPU there is no partial offload to fall back on,
        // so a model that no longer fits fails outright rather than quietly
        // running part of itself on the processor -- which is the whole point
        // of the setting, but is worth saying when it is what just happened.
        if (params.gpu_only) {
            error += ". If it is too large for the card, turn off "
                     "\"GPU-only compute\" in settings to let it use the "
                     "processor for the rest";
        }
        return nullptr;
    }

    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx           = static_cast<std::uint32_t>(std::max(0, params.n_ctx));
    context_params.n_batch         = static_cast<std::uint32_t>(std::max(1, params.n_batch));
    context_params.n_threads       = resolve_threads(params.n_threads);
    context_params.n_threads_batch = context_params.n_threads;
    context_params.flash_attn_type = params.flash_attn ? LLAMA_FLASH_ATTN_TYPE_AUTO
                                                       : LLAMA_FLASH_ATTN_TYPE_DISABLED;

    llama_context* ctx = llama_init_from_model(model, context_params);
    if (ctx == nullptr) {
        llama_model_free(model);
        error = "could not create a context for " + params.path
              + " (n_ctx " + std::to_string(params.n_ctx) + " may not fit)";
        return nullptr;
    }

    return std::unique_ptr<LoadedModel>(new LoadedModel(model, ctx, params.path));
}

LoadedModel* ModelHost::acquire_router(const ModelParams& params,
                                       const ProgressCallback& progress,
                                       std::string& error) {
    if (router_ && router_->path() == params.path) {
        return router_.get();
    }

    // Make room, if the delegator needs it.
    //
    // Only reached with "keep delegator loaded" off, where an expert from the
    // previous prompt is still resident when the delegator comes back for the
    // next one. Asking first rather than loading and hoping matters because a
    // driver handed more than the card holds does not fail -- it spills into
    // system RAM, and a delegator running from there takes seconds per
    // decision with nothing on screen to say why.
    if (expert_) {
        ModelParams planned = params;
        place_delegator(planned, gpu_);
        if (!vram_shortfall(planned.path, planned, 0, {}).empty()) {
            release_expert();
        }
    }

    router_ = load(params, Role::Delegator, progress, error);
    return router_.get();
}

void ModelHost::release_router() {
    router_.reset();
}

/// Would these two be the same loaded model?
///
/// Only the fields that are baked into the model and its context at load time.
/// Sampling is not among them -- temperature and the rest are handed to
/// generate() per call, so two seats that differ only in how they sample can
/// share one loaded model.
bool same_load(const ModelParams& a, const ModelParams& b) {
    return a.path         == b.path
        && a.n_ctx        == b.n_ctx
        && a.n_batch      == b.n_batch
        && a.n_gpu_layers == b.n_gpu_layers
        && a.main_gpu     == b.main_gpu
        && a.split_mode   == b.split_mode
        && a.flash_attn   == b.flash_attn
        && a.tensor_split == b.tensor_split
        && a.no_host      == b.no_host
        && a.direct_io    == b.direct_io;
}

LoadedModel* ModelHost::acquire_expert(Subject subject,
                                       const ModelParams& params,
                                       const ProgressCallback& progress,
                                       std::string& error) {
    if (expert_ && loaded_expert_ == subject && expert_->path() == params.path) {
        return expert_.get();
    }

    // A different seat, but the same weights loaded the same way.
    //
    // Nothing needs to happen: the seats are BatBot's idea, not llama.cpp's,
    // and the model behind two of them is one model. Reloading it would cost
    // half a minute for a large expert to arrive at the file already in memory
    // -- which is what happened on every route change for anyone who has not
    // yet found nine different models to fill the table with.
    if (expert_ && expert_params_ && same_load(*expert_params_, params)) {
        loaded_expert_ = subject;
        return expert_.get();
    }

    // Free first, then load. Holding both at once would double the peak memory
    // and defeat the entire point of loading experts just in time.
    release_expert();

    expert_ = load(params, Role::Expert, progress, error);
    if (expert_) {
        loaded_expert_ = subject;
        expert_params_ = params;
    } else {
        expert_params_.reset();
    }
    return expert_.get();
}

void ModelHost::release_expert() {
    expert_.reset();
    loaded_expert_.reset();
    expert_params_.reset();
}

}  // namespace batbot
