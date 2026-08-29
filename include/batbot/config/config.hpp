// SPDX-License-Identifier: MIT
// BatBot's on-disk configuration: which GGUF backs each expert, and how each
// one should be loaded and sampled.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "batbot/routing/subject.hpp"

namespace batbot {

/// How a single model is loaded onto the hardware and sampled from.
///
/// Every field has a usable default, and each expert inherits from
/// `Config::defaults` for any field its own entry leaves unset. That way the
/// common case -- nine experts that differ only by file -- stays a nine-line
/// config.
struct ModelParams {
    /// The model as written in the config: normally just a file name inside the
    /// models directory ("physics-q4.gguf"), but an absolute or ~-path is
    /// accepted so a model can live anywhere. Empty means the seat is unfilled.
    std::string model;

    /// `model` resolved to an absolute path. Derived at load time and never
    /// written back to the file.
    std::string path;

    // Loading
    int  n_gpu_layers = -1;        ///< -1 offloads every layer it can fit.
    int  main_gpu     = 0;
    std::string split_mode = "layer";  ///< none | layer | row | tensor
    std::vector<float> tensor_split;   ///< per-GPU share; empty = let llama.cpp decide

    // Context
    int  n_ctx     = 8192;
    int  n_batch   = 512;
    int  n_threads = 0;            ///< 0 = hardware_concurrency()
    bool flash_attn = true;

    // Sampling
    float temperature   = 0.7F;
    float top_p         = 0.95F;
    int   top_k         = 40;
    float min_p         = 0.05F;
    float repeat_penalty = 1.05F;
    int   repeat_last_n  = 64;
    int   max_tokens     = 2048;   ///< hard cap per reply; -1 = until EOG
    std::uint32_t seed   = 0xFFFFFFFFU;  ///< 0xFFFFFFFF = random each run

    /// Fill any field this entry left at its default from `base`.
    /// The model is never inherited -- an expert without its own file is unfilled.
    void inherit_from(const ModelParams& base);
};

/// How the delegator's answer is acted on.
struct RoutingConfig {
    /// Below this confidence the delegator is treated as undecided and the
    /// prompt goes to the Fallback expert instead. 0 disables the check and
    /// every routed subject is taken at face value.
    float min_confidence = 0.60F;

    /// Send work to Fallback when the chosen subject has no model behind it,
    /// rather than substituting some other filled seat.
    bool use_fallback_expert = true;
};

/// Purely cosmetic knobs.
struct UiConfig {
    int  animation_ms   = 90;    ///< frame interval while BatBot is busy
    bool show_roundtable = true; ///< draw the ring (toggle at runtime with Ctrl-T)
    bool unicode        = true;  ///< false falls back to a pure-ASCII bat
};

/// The whole config file.
struct Config {
    /// Where the GGUFs live. Empty means the built-in default,
    /// ~/.local/share/batbot/models. May be moved anywhere on the system.
    std::string models_dir;

    ModelParams router;                              ///< the always-resident delegator
    ModelParams defaults;                            ///< inherited by every expert
    std::array<ModelParams, kSubjectCount> experts;  ///< indexed by Subject
    RoutingConfig routing;
    UiConfig      ui;

    std::string system_prompt =
        "You are BatBot, a focused local expert. Answer precisely and completely, "
        "showing your reasoning when it helps. Prefer concrete detail over hedging.";

    /// True when a GGUF is configured for this seat.
    bool has_expert(Subject s) const;

    /// Subjects that currently have a model file configured.
    std::vector<Subject> configured_experts() const;

    /// True when no expert and no router model is configured at all -- the
    /// state a first run lands in, which the UI explains rather than crashing on.
    bool is_empty() const;

    /// The models directory as an absolute path, applying the default when the
    /// config leaves it blank.
    std::filesystem::path resolved_models_dir() const;

    /// Re-resolve every model reference against the models directory. Call
    /// after changing `models_dir` or any model assignment.
    void resolve_models();
};

/// Load the config, creating a documented default file if none exists.
/// `warnings` collects non-fatal problems (a bad enum, a missing model file)
/// so the UI can surface them instead of failing the whole startup.
Config load_config(const std::filesystem::path& file, std::vector<std::string>& warnings);

/// Convenience overload using `paths::config_file()`.
Config load_config(std::vector<std::string>& warnings);

/// Write a fully-populated config with every field spelled out, so the file
/// doubles as the reference for what is tunable.
void write_default_config(const std::filesystem::path& file);

/// Serialise `config` back to disk. This is what the in-app settings editor
/// calls, so it must round-trip everything the loader understands.
/// Returns false if the file could not be written.
bool save_config(const Config& config, const std::filesystem::path& file);

/// Convenience overload using `paths::config_file()`.
bool save_config(const Config& config);

}  // namespace batbot
