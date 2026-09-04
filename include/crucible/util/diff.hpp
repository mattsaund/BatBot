// SPDX-License-Identifier: MIT
//
// What a rewrite actually changed.
//
// The workshop's WRITE replaces a whole file, so "updated calc.py" is true and
// almost useless -- it does not say whether one character moved or the file was
// replaced with something unrelated. This turns the before and after into the
// few lines that differ, which is what a person reading a cook's journal wants
// to see and what the desktop app renders as a diff.
//
// Deliberately not a full LCS. Trimming the common head and tail is O(n) and
// resolves the case that actually occurs -- a model rewriting a file with one
// function changed -- into exactly the right hunk. Anything it cannot resolve
// that way is reported as a block replacement, which is honest: the tool really
// did replace the file, and a prettier diff would be inventing structure the
// operation did not have.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace crucible::util {

/// How many lines a change touched.
struct DiffStat {
    int added   = 0;
    int removed = 0;

    /// "+4 -2", or "unchanged" when nothing moved.
    std::string summary() const;
};

DiffStat diff_stat(std::string_view before, std::string_view after);

/// The changed lines, as `-` and `+` rows with a little context.
///
/// `max_lines` caps the whole thing; what is dropped is said so in place rather
/// than silently truncated, because a diff that stops without warning reads as
/// a complete diff of a smaller change.
///
/// Returns an empty string when the two are identical.
std::string unified_diff(std::string_view before, std::string_view after,
                         std::size_t max_lines = 40);

}  // namespace crucible::util
