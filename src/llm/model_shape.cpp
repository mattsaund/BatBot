// SPDX-License-Identifier: MIT
//
// Reading a GGUF header. See model_shape.hpp for why this is read and not
// estimated.
#include "batbot/llm/model_shape.hpp"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include <gguf.h>

namespace batbot {
namespace {

/// Two bytes per element: llama.cpp's default KV cache is f16, and a quantised
/// cache would only ever be smaller. Erring large is the safe direction for
/// every caller here.
constexpr std::uint64_t kKvElementBytes = 2;

/// Room for the activations and workspace that are not the logits. A flat
/// figure because it depends on the graph rather than on anything in the
/// header, and it is small next to the weights of any model big enough for
/// this to matter.
constexpr std::uint64_t kActivationAllowance = 256ULL * 1024 * 1024;

std::string string_value(gguf_context* gguf, const char* key) {
    const std::int64_t id = gguf_find_key(gguf, key);
    if (id < 0 || gguf_get_kv_type(gguf, id) != GGUF_TYPE_STRING) {
        return {};
    }
    return gguf_get_val_str(gguf, id);
}

/// One unsigned value, or the per-layer list of them.
///
/// Both spellings are in the wild for the same key: a plain model writes
/// `attention.head_count_kv` as a number, while a hybrid one -- LFM2, and
/// anything else that alternates attention with something cheaper -- writes an
/// array with a zero for every layer that has no KV cache at all. Reading only
/// the scalar form would over-count those models by a wide margin.
///
/// gguf_get_val_* aborts the process on a type mismatch, so every read here is
/// behind a type check.
std::vector<std::uint64_t> unsigned_values(gguf_context* gguf, const std::string& key) {
    const std::int64_t id = gguf_find_key(gguf, key.c_str());
    if (id < 0) {
        return {};
    }

    const gguf_type type = gguf_get_kv_type(gguf, id);
    if (type == GGUF_TYPE_ARRAY) {
        const gguf_type element = gguf_get_arr_type(gguf, id);
        if (element != GGUF_TYPE_INT32 && element != GGUF_TYPE_UINT32) {
            return {};
        }
        const auto* data = static_cast<const std::int32_t*>(gguf_get_arr_data(gguf, id));
        if (data == nullptr) {
            return {};
        }
        std::vector<std::uint64_t> values;
        values.reserve(gguf_get_arr_n(gguf, id));
        for (std::size_t i = 0; i < gguf_get_arr_n(gguf, id); ++i) {
            values.push_back(data[i] < 0 ? 0 : static_cast<std::uint64_t>(data[i]));
        }
        return values;
    }
    if (type == GGUF_TYPE_UINT32) {
        return {gguf_get_val_u32(gguf, id)};
    }
    if (type == GGUF_TYPE_INT32) {
        const std::int32_t value = gguf_get_val_i32(gguf, id);
        return {value < 0 ? 0 : static_cast<std::uint64_t>(value)};
    }
    return {};
}

std::uint64_t first_or(const std::vector<std::uint64_t>& values, std::uint64_t fallback) {
    return values.empty() ? fallback : values.front();
}

}  // namespace

std::uint64_t ModelShape::kv_bytes(int n_ctx) const {
    if (n_ctx <= 0 || kv_per_token == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(n_ctx) * kv_per_token;
}

std::uint64_t ModelShape::compute_bytes(int n_batch) const {
    // The logits are the big one: a 65k vocabulary at a batch of 512 is 134 MB
    // on its own, which is most of what a small model's compute buffers come
    // to in practice.
    const auto batch = static_cast<std::uint64_t>(std::max(1, n_batch));
    const std::uint64_t logits = static_cast<std::uint64_t>(vocab) * batch * sizeof(float);
    return logits + kActivationAllowance;
}

std::uint64_t ModelShape::resident_bytes(int n_ctx) const {
    return weights + kv_bytes(n_ctx);
}

std::uint64_t ModelShape::total_bytes(int n_ctx, int n_batch) const {
    return resident_bytes(n_ctx) + compute_bytes(n_batch);
}

ModelShape read_model_shape(const std::filesystem::path& file) {
    ModelShape shape;

    std::error_code ec;
    shape.weights = std::filesystem::file_size(file, ec);
    if (ec) {
        shape.weights = 0;
        return shape;
    }

    gguf_init_params params{};
    params.no_alloc = true;   // metadata only: no tensor data is read
    params.ctx      = nullptr;
    gguf_context* gguf = gguf_init_from_file(file.string().c_str(), params);
    if (gguf == nullptr) {
        return shape;  // not a GGUF; the file size is still worth having
    }

    const std::string arch = string_value(gguf, "general.architecture");
    if (arch.empty()) {
        gguf_free(gguf);
        return shape;
    }

    const auto key = [&arch](const char* suffix) { return arch + "." + suffix; };

    const std::uint64_t layers = first_or(unsigned_values(gguf, key("block_count")), 0);
    const std::uint64_t embd   = first_or(unsigned_values(gguf, key("embedding_length")), 0);
    const std::uint64_t heads  = first_or(unsigned_values(gguf, key("attention.head_count")), 0);
    const std::vector<std::uint64_t> kv_heads =
        unsigned_values(gguf, key("attention.head_count_kv"));

    // Where a model states its head dimensions, use them. Deriving them as
    // embedding / heads is only right when the two happen to divide evenly,
    // which is not true of every architecture -- qwen3next states 256 against
    // an embedding of 2048 over 16 heads, which would derive as 128.
    const std::uint64_t key_len =
        first_or(unsigned_values(gguf, key("attention.key_length")),
                 heads > 0 ? embd / heads : 0);
    const std::uint64_t value_len =
        first_or(unsigned_values(gguf, key("attention.value_length")), key_len);

    shape.layers    = static_cast<std::uint32_t>(layers);
    shape.train_ctx = static_cast<std::uint32_t>(
        first_or(unsigned_values(gguf, key("context_length")), 0));

    // The vocabulary, for the logits buffer. Its size is the length of the
    // token list rather than a key of its own in most files.
    if (const std::int64_t tokens = gguf_find_key(gguf, "tokenizer.ggml.tokens");
        tokens >= 0 && gguf_get_kv_type(gguf, tokens) == GGUF_TYPE_ARRAY) {
        shape.vocab = static_cast<std::uint32_t>(gguf_get_arr_n(gguf, tokens));
    } else {
        shape.vocab = static_cast<std::uint32_t>(
            first_or(unsigned_values(gguf, key("vocab_size")), 0));
    }

    if (layers > 0 && key_len > 0 && !kv_heads.empty()) {
        // Per token of context: every layer's K and V together. A hybrid model
        // lists a zero for each layer that keeps no cache, and those cost
        // nothing, which is the whole reason for reading the array.
        std::uint64_t per_token = 0;
        for (std::uint64_t layer = 0; layer < layers; ++layer) {
            const std::uint64_t count =
                kv_heads.size() == 1 ? kv_heads.front()
                                     : (layer < kv_heads.size() ? kv_heads[layer] : 0);
            per_token += count * (key_len + value_len) * kKvElementBytes;
        }
        shape.kv_per_token = per_token;
    }

    shape.known = layers > 0;
    gguf_free(gguf);
    return shape;
}

}  // namespace batbot
