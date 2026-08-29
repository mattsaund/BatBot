// SPDX-License-Identifier: MIT
// Deciding which expert answers.
//
// Two implementations share one interface so the delegation logic never has to
// care which is in play: a grammar-constrained LLM router (the real thing) and
// a keyword scorer (the fallback when no router GGUF is configured, which also
// keeps the whole UI usable with no models installed at all).
#pragma once

#include <memory>
#include <utility>
#include <vector>
#include <string>

#include "batbot/llm/model_host.hpp"
#include "batbot/routing/subject.hpp"

namespace batbot {

/// Where a routing decision came from, so the UI can be honest about how much
/// to trust it.
enum class RouteSource {
    Model,      ///< the router model chose it
    Keyword,    ///< keyword scoring chose it
    Fallback,   ///< nothing chose it; this is the only or default expert
    Forced,     ///< the user pinned a subject with a slash command
};

struct RouteDecision {
    Subject     subject    = Subject::Language;
    float       confidence = 0.0F;
    RouteSource source     = RouteSource::Fallback;
    std::string detail;     ///< short human-readable note for the status line
};

std::string_view route_source_name(RouteSource source);

class Router {
public:
    virtual ~Router() = default;
    virtual RouteDecision route(const std::string& prompt, const CancelCallback& cancel) = 0;
};

/// Scores the prompt against per-subject keyword sets. No model, no latency,
/// fully deterministic -- but blind to anything the word list does not cover.
class KeywordRouter final : public Router {
public:
    RouteDecision route(const std::string& prompt, const CancelCallback& cancel) override;
};

/// Runs the resident router model under `router_grammar()`, so its output is
/// structurally incapable of naming a subject that does not exist. Falls back
/// to keyword scoring if the model produces something unparseable anyway.
class ModelRouter final : public Router {
public:
    /// `system_prompt_override` is for experiments; empty uses the built-in.
    ModelRouter(LoadedModel& model, ModelParams params,
                std::string system_prompt_override = {});

    RouteDecision route(const std::string& prompt, const CancelCallback& cancel) override;

private:
    LoadedModel&  model_;
    ModelParams   params_;
    std::string   grammar_;
    std::string   system_prompt_;
    std::vector<std::pair<std::string, std::string>> examples_;
    KeywordRouter fallback_;
};

}  // namespace batbot
