// SPDX-License-Identifier: MIT
//
// See benchmark.hpp.
#include "crucible/routing/benchmark.hpp"

namespace crucible {

const std::vector<RouteCase>& benchmark_cases() {
    static const std::vector<RouteCase> kCases{
    {"mathematics", "compute the derivative of x^3 sin(x)"},
    {"mathematics", "prove there are infinitely many prime numbers"},
    {"mathematics", "what is the determinant of a 3x3 matrix"},
    {"mathematics", "solve the quadratic equation 2x^2 - 5x + 3 = 0"},
    {"mathematics", "how many ways can I arrange 5 books on a shelf"},
    {"mathematics", "what is the sum of an infinite geometric series"},

    {"programming", "why does my C++ program segfault when I dereference this pointer"},
    {"programming", "write a binary search function in Python"},
    {"programming", "how do I undo the last commit in git"},
    {"programming", "what is the difference between a list and a tuple in Python"},
    {"programming", "my unit tests pass locally but fail in CI"},
    {"programming", "explain how a hash table handles collisions"},

    {"physics", "why is the sky blue?"},
    {"physics", "what is the Lagrangian of a simple pendulum"},
    {"physics", "explain time dilation in special relativity"},
    {"physics", "how much kinetic energy does a 2 kg ball have at 10 m/s"},
    {"physics", "why does a gyroscope resist being tilted"},
    {"physics", "what happens to entropy in an isolated system"},

    {"chemistry", "balance the combustion reaction for propane"},
    {"chemistry", "what is the pH of a 0.1 molar HCl solution"},
    {"chemistry", "why is water a polar molecule"},
    {"chemistry", "how many grams of NaCl are in 2 moles"},
    {"chemistry", "what does a catalyst do to activation energy"},
    {"chemistry", "explain the difference between an alkane and an alkene"},

    {"biology", "how does DNA replication work in eukaryotic cells"},
    {"biology", "what role do mitochondria play in the cell"},
    {"biology", "how do vaccines produce immunity"},
    {"biology", "what is the difference between mitosis and meiosis"},
    {"biology", "why do antibiotics not work on viruses"},
    {"biology", "how does photosynthesis convert light into sugar"},

    {"engineering", "what torque should I use on an M8 steel bolt"},
    {"engineering", "how do I size a steel beam for a 3 metre span"},
    {"engineering", "what gauge wire do I need for a 20 amp circuit"},
    {"engineering", "how do I choose a bearing for a rotating shaft"},
    {"engineering", "what tolerance should this press fit have"},
    {"engineering", "how do I design a heat sink for a 50 watt device"},

    {"philosophy", "is free will compatible with determinism?"},
    {"philosophy", "explain Kant's categorical imperative"},
    {"philosophy", "what is the trolley problem meant to show"},
    {"philosophy", "can we know anything with certainty"},
    {"philosophy", "what makes an action morally right"},
    {"philosophy", "what is the mind-body problem"},

    {"sociology", "how does urbanisation affect social mobility"},
    {"sociology", "what causes inflation in a modern economy"},
    {"sociology", "why do voter turnout rates differ between countries"},
    {"sociology", "what is the difference between a norm and a law"},
    {"sociology", "how did the industrial revolution change family structure"},
    {"sociology", "what does social capital mean"},

    {"language", "proofread this paragraph and improve its tone"},
    {"language", "translate 'good morning' into French"},
    {"language", "when should I use a semicolon instead of a comma"},
    {"language", "rewrite this sentence in the active voice"},
    {"language", "what is the etymology of the word quarantine"},
    {"language", "summarise this essay in two sentences"},
};
    return kCases;
}

}  // namespace crucible
