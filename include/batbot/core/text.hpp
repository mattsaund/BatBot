// Small text helpers that are worth testing on their own.
#pragma once

#include <string>

namespace batbot::detail {

/// Number of bytes in the UTF-8 sequence that starts with `lead`.
/// A stray continuation byte reports 1 so callers make progress instead of
/// stalling on malformed input.
int utf8_length(unsigned char lead);

/// Split `buffer` into a prefix of complete UTF-8 sequences and a remainder.
///
/// A single model token can end in the middle of a codepoint, so emitting raw
/// pieces would print replacement characters mid-word. The incomplete tail is
/// left in `buffer` until the bytes that finish it arrive.
///
/// Returns the complete prefix and erases it from `buffer`.
std::string take_complete_utf8(std::string& buffer);

}  // namespace batbot::detail
