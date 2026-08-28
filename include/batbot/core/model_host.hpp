// The JIT model host: owns every llama.cpp resource and enforces the rule that
// makes BatBot's memory budget work -- the small router stays resident, and at
// most one large expert is loaded at any moment.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "batbot/core/config.hpp"
#include "batbot/core/subject.hpp"

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace batbot {

/// One turn of a conversation as handed to the chat template.
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
    bool   hit_limit     = false;  ///< stopped at max_tokens rather than an end-of-turn token

    double tokens_per_second() const;
};

/// Called for each chunk of decoded text. Chunks are always complete UTF-8, so
/// the UI can append them straight to a string without splitting a codepoint.
using TokenCallback = std::function<void(std::string_view)>;

/// Return true to abort. Polled between tokens and during model loading.
using CancelCallback = std::function<bool()>;

/// Load progress in [0, 1].
using ProgressCallback = std::function<void(float)>;

/// A model file plus the context and sampler state needed to generate from it.
/// Non-copyable, and freeing it releases the weights -- which is exactly what
/// an expert swap does.
class LoadedModel {
public:
    ~LoadedModel();
    LoadedModel(const LoadedModel&)            = delete;
    LoadedModel& operator=(const LoadedModel&) = delete;

    /// Render `messages` through the model's own chat template. Falls back to
    /// ChatML for GGUFs that ship without one.
    std::string format_chat(const std::vector<ChatMessage>& messages,
                            bool add_assistant_prefix) const;

    /// Generate from `prompt`, streaming complete UTF-8 chunks to `on_token`.
    /// `grammar` (GBNF) is optional; when given, the sampler cannot leave it --
    /// this is what makes the router incapable of naming a subject that does
    /// not exist.
    GenerationStats generate(const std::string& prompt,
                             const ModelParams& params,
                             const TokenCallback& on_token,
                             const CancelCallback& cancel,
                             const std::string& grammar = {});

    const std::string& path()   const { return path_; }
    std::uint64_t      params() const;  ///< parameter count, for the UI
    std::uint64_t      bytes()  const;  ///< in-memory size
    int                n_ctx_train() const;
    std::string        description() const;

private:
    friend class ModelHost;
    LoadedModel(llama_model* model, llama_context* ctx, std::string path);

    llama_model*   model_ = nullptr;
    llama_context* ctx_   = nullptr;
    std::string    path_;
};

/// Owns the llama.cpp backend and the currently-resident models.
///
/// Exactly one ModelHost should exist per process, and every method must be
/// called from the same thread -- BatBot runs it on the engine worker so the
/// UI thread never blocks behind a model load.
class ModelHost {
public:
    /// `log_path` receives llama.cpp's logging. Redirecting it is not optional:
    /// left on stderr it would draw straight over the TUI.
    explicit ModelHost(std::filesystem::path log_path);
    ~ModelHost();
    ModelHost(const ModelHost&)            = delete;
    ModelHost& operator=(const ModelHost&) = delete;

    /// Load the router and keep it resident for the rest of the session.
    /// Idempotent.
    LoadedModel* acquire_router(const ModelParams& params,
                                const ProgressCallback& progress,
                                std::string& error);

    /// Make `subject`'s expert the loaded one, freeing whichever expert was
    /// loaded before. This is the JIT swap, and the cost the design accepts in
    /// exchange for experts far larger than RAM would otherwise allow.
    /// Returns the already-loaded model without doing any work if `subject` is
    /// already resident.
    LoadedModel* acquire_expert(Subject subject,
                                const ModelParams& params,
                                const ProgressCallback& progress,
                                std::string& error);

    /// Free the resident expert, if any. The router is untouched.
    void release_expert();

    std::optional<Subject> loaded_expert() const { return loaded_expert_; }
    LoadedModel*           router()        const { return router_.get(); }
    LoadedModel*           expert()        const { return expert_.get(); }

    /// Names of the compute devices llama.cpp found, for the UI and `/devices`.
    static std::vector<std::string> devices();

private:
    std::unique_ptr<LoadedModel> load(const ModelParams& params,
                                      const ProgressCallback& progress,
                                      std::string& error);

    std::unique_ptr<LoadedModel> router_;
    std::unique_ptr<LoadedModel> expert_;
    std::optional<Subject>       loaded_expert_;
};

}  // namespace batbot
