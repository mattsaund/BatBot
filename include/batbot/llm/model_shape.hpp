// SPDX-License-Identifier: MIT
//
// What a GGUF says about itself, read without loading it.
//
// Two things need to know how much room a model will want before committing to
// it: the GPU split, which fills cards in order and therefore has to know when
// one is full, and the "Dedicated VRAM only" check, which refuses a model
// rather than letting the driver spill it into system RAM.
//
// Both used to guess -- a flat fraction of the file size -- and a guess is
// exactly wrong here. Too generous and it refuses models that fit; too mean and
// it waves through ones that do not. A 32 GB model was refused on 34.8 GB of
// free VRAM because a 25% allowance invented eight gigabytes of overhead that
// were never going to be used.
//
// The real numbers are in the file. GGUF puts its metadata at the front, ggml
// can read it without touching a tensor, and it costs about 50 ms even for a
// 34 GB model -- so there is no reason to estimate what can simply be read.
#pragma once

#include <cstdint>
#include <filesystem>

namespace batbot {

/// The parts of a model's memory footprint that are knowable before loading.
struct ModelShape {
    /// False when the file is missing, is not a GGUF, or does not carry the
    /// keys this needs. Callers fall back to their own conservative guess
    /// rather than refusing something they cannot measure.
    bool known = false;

    std::uint64_t weights = 0;  ///< the file, which is what the tensors weigh
    std::uint32_t layers  = 0;
    std::uint32_t vocab   = 0;

    /// Bytes of KV cache one token of context costs, summed over every layer.
    /// Zero for an architecture whose attention shape could not be read.
    std::uint64_t kv_per_token = 0;

    /// The context the model was trained at, for reference; not used in the
    /// arithmetic here.
    std::uint32_t train_ctx = 0;

    /// KV cache at `n_ctx` tokens.
    std::uint64_t kv_bytes(int n_ctx) const;

    /// Roughly what the compute buffers and logits will want, over and above
    /// the weights and the cache. Scales with the batch and the vocabulary,
    /// which is where the large one -- the logits buffer -- comes from.
    std::uint64_t compute_bytes(int n_batch) const;

    /// Everything: weights, cache and compute.
    std::uint64_t total_bytes(int n_ctx, int n_batch) const;

    /// Weights plus cache -- the part that is divided between cards, since
    /// both follow the layers. The compute buffers do not, and are covered by
    /// the per-card headroom in devices.hpp instead.
    std::uint64_t resident_bytes(int n_ctx) const;
};

/// Read `file`'s header. Cheap: metadata only, no tensor data.
ModelShape read_model_shape(const std::filesystem::path& file);

}  // namespace batbot
