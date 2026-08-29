// SPDX-License-Identifier: MIT
//
// One model file, loaded and ready to generate from.
//
// Freeing a LoadedModel releases the weights, which is precisely what an expert
// swap does -- so the lifetime of this object *is* the JIT loading strategy.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "batbot/config/config.hpp"

struct llama_model;
struct llama_context;

namespace batbot {

/// One turn of a conversation, as handed to the model's chat template.
struct ChatMessage {
    std::string role;     ///< "system" | "user" | "assistant"
    std::string content;
};

/// Reported after a generation finishes, for the status line.
struct GenerationStats {
    int    prompt_tokens = 0;
    int    output_tokens = 0;
    double prompt_ms     = 0.0;
    double output_ms     = 0.0;
    bool   cancelled     = false;
    /// Stopped at max_tokens rather than at an end-of-turn token.
    bool   hit_limit     = false;

    double tokens_per_second() const;
};

/// Called for each chunk of decoded text. Chunks are always complete UTF-8, so
/// the UI can append them straight to a string without splitting a codepoint.
using TokenCallback = std::function<void(std::string_view)>;

/// Return true to abort. Polled between tokens and during model loading.
using CancelCallback = std::function<bool()>;

/// Load progress in [0, 1].
using ProgressCallback = std::function<void(float)>;

class LoadedModel {
public:
    ~LoadedModel();
    LoadedModel(const LoadedModel&)            = delete;
    LoadedModel& operator=(const LoadedModel&) = delete;

    /// Render `messages` through the model's own chat template, falling back to
    /// ChatML for GGUFs that ship without one.
    std::string format_chat(const std::vector<ChatMessage>& messages,
                            bool add_assistant_prefix) const;

    /// Generate from `prompt`, streaming complete UTF-8 chunks to `on_token`.
    ///
    /// `grammar` (GBNF) is optional; when given, the sampler cannot leave it.
    /// That is what makes the delegator incapable of naming a subject that does
    /// not exist -- an invalid route is unrepresentable, not merely unlikely.
    GenerationStats generate(const std::string& prompt,
                             const ModelParams& params,
                             const TokenCallback& on_token,
                             const CancelCallback& cancel,
                             const std::string& grammar = {});

    const std::string& path() const { return path_; }
    std::uint64_t      params() const;       ///< parameter count, for the UI
    std::uint64_t      bytes() const;        ///< in-memory size
    int                n_ctx_train() const;  ///< context the model was trained for
    std::string        description() const;

private:
    friend class ModelHost;
    LoadedModel(llama_model* model, llama_context* ctx, std::string path);

    llama_model*   model_ = nullptr;
    llama_context* ctx_   = nullptr;
    std::string    path_;
};

}  // namespace batbot
