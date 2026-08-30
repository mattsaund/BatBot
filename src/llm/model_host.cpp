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

#include <llama.h>

#include "batbot/llm/sampling.hpp"
#include "batbot/runtime/registry.hpp"

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
    if (bridge == nullptr || bridge->progress == nullptr) {
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
    // install has none of its own yet, so the CPU runtime that shipped with
    // the binary is copied across first; after that this is a no-op.
    std::string seed_error;
    RuntimeRegistry::seed_from_bundle(seed_error);
    if (!seed_error.empty()) {
        log_to_file(GGML_LOG_LEVEL_WARN, ("runtime seed: " + seed_error + "\n").c_str(), nullptr);
    }
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

std::unique_ptr<LoadedModel> ModelHost::load(const ModelParams& params,
                                             const ProgressCallback& progress,
                                             std::string& error) {
    if (params.path.empty()) {
        error = "no model file configured";
        return nullptr;
    }
    if (!std::filesystem::exists(params.path)) {
        error = "model file not found: " + params.path;
        return nullptr;
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

    llama_model* model = llama_model_load_from_file(params.path.c_str(), model_params);
    if (model == nullptr) {
        error = "llama.cpp could not load " + params.path
              + " (see the BatBot log for details)";
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
    router_ = load(params, progress, error);
    return router_.get();
}

LoadedModel* ModelHost::acquire_expert(Subject subject,
                                       const ModelParams& params,
                                       const ProgressCallback& progress,
                                       std::string& error) {
    if (expert_ && loaded_expert_ == subject && expert_->path() == params.path) {
        return expert_.get();
    }

    // Free first, then load. Holding both at once would double the peak memory
    // and defeat the entire point of loading experts just in time.
    release_expert();

    expert_ = load(params, progress, error);
    if (expert_) {
        loaded_expert_ = subject;
    }
    return expert_.get();
}

void ModelHost::release_expert() {
    expert_.reset();
    loaded_expert_.reset();
}

}  // namespace batbot
