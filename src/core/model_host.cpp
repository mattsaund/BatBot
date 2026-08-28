#include "batbot/core/model_host.hpp"

#include "batbot/core/text.hpp"

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

namespace batbot {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

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

std::string piece_for(const llama_vocab* vocab, llama_token token) {
    std::string piece(64, '\0');
    int written = llama_token_to_piece(vocab, token, piece.data(),
                                       static_cast<int32_t>(piece.size()), 0, false);
    if (written < 0) {
        piece.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(vocab, token, piece.data(),
                                       static_cast<int32_t>(piece.size()), 0, false);
    }
    piece.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return piece;
}

std::vector<llama_token> tokenize(const llama_vocab* vocab, const std::string& text,
                                  bool add_special) {
    // A negative return is llama.cpp telling us how much room it actually needs.
    int needed = -llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                 nullptr, 0, add_special, true);
    if (needed <= 0) {
        return {};
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
    const int written = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                       tokens.data(), needed, add_special, true);
    tokens.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return tokens;
}

}  // namespace

// ---------------------------------------------------------------------------
// GenerationStats
// ---------------------------------------------------------------------------

double GenerationStats::tokens_per_second() const {
    if (output_ms <= 0.0 || output_tokens <= 0) {
        return 0.0;
    }
    return static_cast<double>(output_tokens) * 1000.0 / output_ms;
}

// ---------------------------------------------------------------------------
// LoadedModel
// ---------------------------------------------------------------------------

LoadedModel::LoadedModel(llama_model* model, llama_context* ctx, std::string path)
    : model_(model), ctx_(ctx), path_(std::move(path)) {}

LoadedModel::~LoadedModel() {
    if (ctx_ != nullptr) {
        llama_free(ctx_);
    }
    if (model_ != nullptr) {
        llama_model_free(model_);
    }
}

std::uint64_t LoadedModel::params() const { return llama_model_n_params(model_); }
std::uint64_t LoadedModel::bytes()  const { return llama_model_size(model_); }
int LoadedModel::n_ctx_train()      const { return llama_model_n_ctx_train(model_); }

std::string LoadedModel::description() const {
    std::string buffer(256, '\0');
    const int written = llama_model_desc(model_, buffer.data(),
                                         static_cast<int32_t>(buffer.size()));
    buffer.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return buffer;
}

std::string LoadedModel::format_chat(const std::vector<ChatMessage>& messages,
                                     bool add_assistant_prefix) const {
    std::vector<llama_chat_message> native;
    native.reserve(messages.size());
    for (const ChatMessage& message : messages) {
        native.push_back({message.role.c_str(), message.content.c_str()});
    }

    // A GGUF without an embedded template still has to be usable, so fall back
    // to ChatML rather than refusing to run the model.
    const char* tmpl = llama_model_chat_template(model_, nullptr);
    if (tmpl == nullptr) {
        tmpl = "chatml";
    }

    std::size_t capacity = 0;
    for (const ChatMessage& message : messages) {
        capacity += message.role.size() + message.content.size() + 32;
    }
    std::string buffer(std::max<std::size_t>(capacity * 2, 1024), '\0');

    int written = llama_chat_apply_template(tmpl, native.data(), native.size(),
                                            add_assistant_prefix, buffer.data(),
                                            static_cast<int32_t>(buffer.size()));
    if (written > static_cast<int>(buffer.size())) {
        buffer.resize(static_cast<std::size_t>(written));
        written = llama_chat_apply_template(tmpl, native.data(), native.size(),
                                            add_assistant_prefix, buffer.data(),
                                            static_cast<int32_t>(buffer.size()));
    }
    if (written < 0) {
        // Last resort: a plain transcript. Worse quality than the real
        // template, but it still produces an answer.
        std::string plain;
        for (const ChatMessage& message : messages) {
            plain += message.role + ": " + message.content + "\n";
        }
        if (add_assistant_prefix) {
            plain += "assistant: ";
        }
        return plain;
    }

    buffer.resize(static_cast<std::size_t>(written));
    return buffer;
}

GenerationStats LoadedModel::generate(const std::string& prompt,
                                      const ModelParams& params,
                                      const TokenCallback& on_token,
                                      const CancelCallback& cancel,
                                      const std::string& grammar) {
    GenerationStats stats;
    const llama_vocab* vocab = llama_model_get_vocab(model_);

    // Each turn starts from a clean slate. Re-feeding the whole conversation is
    // wasteful, but an expert swap invalidates the cache anyway, and it keeps
    // this loop obviously correct. Prefix reuse is a later optimisation.
    llama_memory_clear(llama_get_memory(ctx_), true);

    std::vector<llama_token> tokens = tokenize(vocab, prompt, true);
    if (tokens.empty()) {
        return stats;
    }

    const int context_size = static_cast<int>(llama_n_ctx(ctx_));
    if (static_cast<int>(tokens.size()) >= context_size) {
        // Keep the tail: the newest turn matters more than the oldest.
        tokens.erase(tokens.begin(),
                     tokens.end() - (context_size - std::min(256, context_size / 4)));
    }
    stats.prompt_tokens = static_cast<int>(tokens.size());

    // --- sampler chain -----------------------------------------------------
    llama_sampler* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());

    // Guarded immediately: llama_sampler_init_grammar throws on a malformed
    // grammar, and anything constructed after that point would not run.
    struct ChainGuard {
        llama_sampler* chain;
        ~ChainGuard() { llama_sampler_free(chain); }
    } guard{chain};

    if (!grammar.empty()) {
        // Added first so the grammar filters the candidate set before any
        // truncation sampler can throw away the only legal tokens.
        if (llama_sampler* g = llama_sampler_init_grammar(vocab, grammar.c_str(), "root");
            g != nullptr) {
            llama_sampler_chain_add(chain, g);
        }
    }
    if (params.repeat_penalty != 1.0F) {
        llama_sampler_chain_add(chain, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab), params.repeat_last_n, params.repeat_penalty,
            /*penalty_freq=*/0.0F, /*penalty_present=*/0.0F));
    }
    if (params.temperature <= 0.0F) {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(chain, llama_sampler_init_top_k(params.top_k));
        llama_sampler_chain_add(chain, llama_sampler_init_top_p(params.top_p, 1));
        llama_sampler_chain_add(chain, llama_sampler_init_min_p(params.min_p, 1));
        llama_sampler_chain_add(chain, llama_sampler_init_temp(params.temperature));
        std::uint32_t seed = params.seed;
        if (seed == 0xFFFFFFFFU) {
            seed = std::random_device{}();
        }
        llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
    }

    // --- prompt ingestion --------------------------------------------------
    const auto prompt_start = Clock::now();
    const int batch_size = std::max(1, params.n_batch);
    for (std::size_t offset = 0; offset < tokens.size(); offset += static_cast<std::size_t>(batch_size)) {
        if (cancel && cancel()) {
            stats.cancelled = true;
            stats.prompt_ms = ms_since(prompt_start);
            return stats;
        }
        const int count = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(batch_size), tokens.size() - offset));
        llama_batch batch = llama_batch_get_one(tokens.data() + offset, count);
        if (llama_decode(ctx_, batch) != 0) {
            stats.prompt_ms = ms_since(prompt_start);
            return stats;
        }
    }
    stats.prompt_ms = ms_since(prompt_start);

    // --- token generation --------------------------------------------------
    const auto output_start = Clock::now();
    const int limit = params.max_tokens > 0 ? params.max_tokens : context_size;
    std::string pending;  // holds bytes of an unfinished UTF-8 sequence

    for (int produced = 0; produced < limit; ++produced) {
        if (cancel && cancel()) {
            stats.cancelled = true;
            break;
        }

        // llama_sampler_sample() accepts the token into the chain itself, so
        // there is no llama_sampler_accept() call here on purpose: a second
        // accept advances the grammar twice and empties its stack, which
        // surfaces as "Unexpected empty grammar stack after accepting piece".
        const llama_token token = llama_sampler_sample(chain, ctx_, -1);
        if (llama_vocab_is_eog(vocab, token)) {
            break;
        }

        pending += piece_for(vocab, token);
        if (std::string ready = detail::take_complete_utf8(pending);
            !ready.empty() && on_token) {
            on_token(ready);
        }

        ++stats.output_tokens;
        if (produced + 1 >= limit) {
            stats.hit_limit = true;
        }

        llama_token next = token;
        llama_batch batch = llama_batch_get_one(&next, 1);
        if (llama_decode(ctx_, batch) != 0) {
            break;
        }
    }

    // Flush whatever is left, even if it is an incomplete sequence -- the model
    // is finished, so no more bytes are coming.
    if (!pending.empty() && on_token) {
        on_token(pending);
    }
    stats.output_ms = ms_since(output_start);
    return stats;
}

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
