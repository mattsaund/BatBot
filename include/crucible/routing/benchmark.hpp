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

#include "crucible/routing/subject.hpp"

namespace crucible {

/// One prompt whose subject a knowledgeable person would not argue about.
struct RouteCase {
    Subject          expect;
    std::string_view prompt;
};

/// Six per subject, and deliberately unambiguous: a benchmark full of genuinely
/// debatable questions measures nothing.
const std::vector<RouteCase>& benchmark_cases();

}  // namespace crucible
