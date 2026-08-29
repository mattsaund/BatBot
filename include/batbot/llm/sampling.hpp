// SPDX-License-Identifier: MIT
//
// Building a llama.cpp sampler chain from BatBot's configuration.
#pragma once

#include <string>

#include "batbot/config/config.hpp"

struct llama_sampler;
struct llama_vocab;

namespace batbot::llm {

/// Assemble the sampler chain for one generation.
///
/// Order is deliberate. A grammar, when present, goes in first so it filters
/// the candidate set before any truncation sampler runs -- otherwise top-k
/// could discard the only tokens the grammar would have allowed, leaving the
/// sampler with nothing legal to pick.
///
/// The caller owns the result and must free it with `llama_sampler_free`.
llama_sampler* build_sampler_chain(const llama_vocab* vocab,
                                   const ModelParams& params,
                                   const std::string& grammar);

}  // namespace batbot::llm
