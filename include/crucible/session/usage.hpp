// SPDX-License-Identifier: MIT
//
// Counting tokens.
//
// Three scopes, because they answer different questions:
//   turn    -- how fast was that reply?          (tok/s, shown as it streams)
//   session -- what has this conversation cost?  (since Crucible started)
//   project -- what has this codebase cost?      (across every session, ever)
//
// Nothing here is billed -- Crucible runs locally. The numbers are for
// understanding: which expert is slow, whether an answer is about to run out
// of context, and how much work a project has actually taken.
#pragma once

#include <cstdint>
#include <string>

namespace crucible {

struct GenerationStats;

/// Tokens in, tokens out, and the time it took.
struct TokenUsage {
    std::uint64_t input_tokens  = 0;  ///< prompt tokens fed to a model
    std::uint64_t output_tokens = 0;  ///< tokens generated
    std::uint64_t turns         = 0;

    /// Time spent generating output, in milliseconds. Prompt ingestion is not
    /// counted: including it would make tok/s depend on prompt length, which
    /// hides the number people actually want to compare between experts.
    double output_ms = 0.0;

    void add(const GenerationStats& stats);
    void add(const TokenUsage& other);

    std::uint64_t total_tokens() const { return input_tokens + output_tokens; }

    /// Output tokens per second across everything counted here, or 0.
    double tokens_per_second() const;
};

/// A token count at terminal width: 847, "1.2k", "3.4M".
std::string format_tokens(std::uint64_t count);

/// "tok ↑ 12.1k  ↓ 4.3k  ·  38.2 tok/s" -- the status-bar readout.
///
/// The counts carry the unit because two bare numbers next to an arrow are
/// not self-explanatory; the rate carries its own.
///
/// `live_tps` overrides the average while a reply is streaming, so the number
/// reflects the answer happening now rather than the session so far.
/// `unicode` false spells the arrows out, for the terminals `ui.unicode`
/// exists for.
std::string usage_readout(const TokenUsage& usage, double live_tps, bool unicode = true);

}  // namespace crucible
