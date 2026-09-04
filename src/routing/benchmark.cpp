// SPDX-License-Identifier: MIT
//
// See benchmark.hpp.
#include "crucible/routing/benchmark.hpp"

namespace crucible {

const std::vector<RouteCase>& benchmark_cases() {
    static const std::vector<RouteCase> kCases{
    {Subject::Mathematics, "compute the derivative of x^3 sin(x)"},
    {Subject::Mathematics, "prove there are infinitely many prime numbers"},
    {Subject::Mathematics, "what is the determinant of a 3x3 matrix"},
    {Subject::Mathematics, "solve the quadratic equation 2x^2 - 5x + 3 = 0"},
    {Subject::Mathematics, "how many ways can I arrange 5 books on a shelf"},
    {Subject::Mathematics, "what is the sum of an infinite geometric series"},

    {Subject::Programming, "why does my C++ program segfault when I dereference this pointer"},
    {Subject::Programming, "write a binary search function in Python"},
    {Subject::Programming, "how do I undo the last commit in git"},
    {Subject::Programming, "what is the difference between a list and a tuple in Python"},
    {Subject::Programming, "my unit tests pass locally but fail in CI"},
    {Subject::Programming, "explain how a hash table handles collisions"},

    {Subject::Physics,     "why is the sky blue?"},
    {Subject::Physics,     "what is the Lagrangian of a simple pendulum"},
    {Subject::Physics,     "explain time dilation in special relativity"},
    {Subject::Physics,     "how much kinetic energy does a 2 kg ball have at 10 m/s"},
    {Subject::Physics,     "why does a gyroscope resist being tilted"},
    {Subject::Physics,     "what happens to entropy in an isolated system"},

    {Subject::Chemistry,   "balance the combustion reaction for propane"},
    {Subject::Chemistry,   "what is the pH of a 0.1 molar HCl solution"},
    {Subject::Chemistry,   "why is water a polar molecule"},
    {Subject::Chemistry,   "how many grams of NaCl are in 2 moles"},
    {Subject::Chemistry,   "what does a catalyst do to activation energy"},
    {Subject::Chemistry,   "explain the difference between an alkane and an alkene"},

    {Subject::Biology,     "how does DNA replication work in eukaryotic cells"},
    {Subject::Biology,     "what role do mitochondria play in the cell"},
    {Subject::Biology,     "how do vaccines produce immunity"},
    {Subject::Biology,     "what is the difference between mitosis and meiosis"},
    {Subject::Biology,     "why do antibiotics not work on viruses"},
    {Subject::Biology,     "how does photosynthesis convert light into sugar"},

    {Subject::Engineering, "what torque should I use on an M8 steel bolt"},
    {Subject::Engineering, "how do I size a steel beam for a 3 metre span"},
    {Subject::Engineering, "what gauge wire do I need for a 20 amp circuit"},
    {Subject::Engineering, "how do I choose a bearing for a rotating shaft"},
    {Subject::Engineering, "what tolerance should this press fit have"},
    {Subject::Engineering, "how do I design a heat sink for a 50 watt device"},

    {Subject::Philosophy,  "is free will compatible with determinism?"},
    {Subject::Philosophy,  "explain Kant's categorical imperative"},
    {Subject::Philosophy,  "what is the trolley problem meant to show"},
    {Subject::Philosophy,  "can we know anything with certainty"},
    {Subject::Philosophy,  "what makes an action morally right"},
    {Subject::Philosophy,  "what is the mind-body problem"},

    {Subject::Sociology,   "how does urbanisation affect social mobility"},
    {Subject::Sociology,   "what causes inflation in a modern economy"},
    {Subject::Sociology,   "why do voter turnout rates differ between countries"},
    {Subject::Sociology,   "what is the difference between a norm and a law"},
    {Subject::Sociology,   "how did the industrial revolution change family structure"},
    {Subject::Sociology,   "what does social capital mean"},

    {Subject::Language,    "proofread this paragraph and improve its tone"},
    {Subject::Language,    "translate 'good morning' into French"},
    {Subject::Language,    "when should I use a semicolon instead of a comma"},
    {Subject::Language,    "rewrite this sentence in the active voice"},
    {Subject::Language,    "what is the etymology of the word quarantine"},
    {Subject::Language,    "summarise this essay in two sentences"},
};
    return kCases;
}

}  // namespace crucible
