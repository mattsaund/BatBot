// SPDX-License-Identifier: MIT
//
// The roster the routing tests route against.
//
// Crucible ships no experts, so a test that needs a realistic roster has to say
// which one. That is the benchmark roster: nine seats whose blurbs and keyword
// sets are measured rather than guessed, kept in the library beside the
// benchmark cases that score against them.
//
// If a test needs an expert that is not one of those nine, build it in the test
// rather than adding it here -- the shared fixture is the thing the benchmark
// numbers refer to.
#pragma once

#include "crucible/routing/benchmark.hpp"
#include "crucible/routing/expert.hpp"

namespace crucible::testing {

inline Roster sample_roster() { return benchmark_roster(); }

}  // namespace crucible::testing
