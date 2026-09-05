// SPDX-License-Identifier: MIT
//
// The prompts Crucible measures its delegator against.
//
// In the library rather than in the tool, so that two things can read them: the
// benchmark itself, and the unit test that checks no worked example in the
// routing prompt is also a question on the answer sheet. An example that is
// also a test case measures how well the prompt was copied into the benchmark,
// which is how a routing change can look like an improvement while making
// nothing better.
#pragma once

#include <string_view>
#include <vector>

#include "crucible/routing/expert.hpp"

namespace crucible {

/// One prompt whose expert a knowledgeable person would not argue about.
struct RouteCase {
    /// The `Expert::id` that should take it. A string rather than a handle:
    /// these cases are written against the nine that ship, and they have to
    /// stay meaningful when the roster around them has been added to.
    std::string_view expect;
    std::string_view prompt;
};

/// Six per shipped expert, and deliberately unambiguous: a benchmark full of
/// genuinely debatable questions measures nothing.
///
/// Only the shipped nine are covered. A user-made expert has no answer sheet,
/// which is the honest position -- nobody but its author knows what should
/// route to it.
const std::vector<RouteCase>& benchmark_cases();

/// The roster those cases are written against.
///
/// Crucible ships no experts, so this is not a default anybody gets -- it is
/// the other half of the answer sheet, kept beside the questions. The tool and
/// the routing tests score against it; nothing builds a user's config from it.
const Roster& benchmark_roster();

}  // namespace crucible
