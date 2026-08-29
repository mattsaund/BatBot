// SPDX-License-Identifier: MIT
//
// Generating text from a loaded model.
//
// The loop is deliberately plain: clear the KV cache, feed the whole prompt,
// then sample a token at a time. Re-feeding the conversation each turn is
// wasteful, but an expert swap invalidates the cache anyway, and correctness
// here is worth more than the saving. Prefix reuse is a later optimisation.
#include "batbot/llm/loaded_model.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

#include <llama.h>

#include "batbot/llm/sampling.hpp"
#include "batbot/util/text.hpp"

namespace batbot {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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

    llama_sampler* chain = llm::build_sampler_chain(vocab, params, grammar);

    // Guarded immediately: the chain must be freed on every path out of this
    // function, including the early returns below.
    struct ChainGuard {
        llama_sampler* chain;
        ~ChainGuard() { llama_sampler_free(chain); }
    } guard{chain};

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


}  // namespace batbot
