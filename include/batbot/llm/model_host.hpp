// SPDX-License-Identifier: MIT
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

#include "batbot/config/config.hpp"
#include "batbot/llm/loaded_model.hpp"
#include "batbot/routing/subject.hpp"

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace batbot {

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
