// SPDX-License-Identifier: MIT
// batbot-routebench -- measure how well a model does the delegator's job.
//
// Routing quality is the single thing that decides whether BatBot sends your
// question to the right expert, and it is not obvious from a model's size or
// its benchmark scores. This loads one candidate delegator, routes a fixed set
// of prompts whose subject is not in doubt, and reports how many it got right.
//
//   batbot-routebench ~/.local/share/batbot/models/LFM2-1.2B-Q8_0.gguf
//
// Sampling is greedy, so repeated runs give identical results and a change in
// the score is a real change rather than sampling noise.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "batbot/config/config.hpp"
#include "batbot/llm/model_host.hpp"
#include "batbot/config/paths.hpp"
#include "batbot/routing/router.hpp"

using namespace batbot;

namespace {

struct Case {
    Subject     expect;
    const char* prompt;
};

/// Prompts whose subject a knowledgeable person would not argue about. Keep it
/// that way: a benchmark full of genuinely ambiguous questions measures nothing.
const std::vector<Case> kCases{
    {Subject::Mathematics, "compute the derivative of x^3 sin(x)"},
    {Subject::Mathematics, "prove there are infinitely many prime numbers"},
    {Subject::Programming, "why does my C++ program segfault when I dereference this pointer"},
    {Subject::Programming, "write a binary search function in Python"},
    {Subject::Physics,     "why is the sky blue?"},
    {Subject::Physics,     "what is the Lagrangian of a simple pendulum"},
    {Subject::Physics,     "explain time dilation in special relativity"},
    {Subject::Chemistry,   "balance the combustion reaction for propane"},
    {Subject::Chemistry,   "what is the pH of a 0.1 molar HCl solution"},
    {Subject::Biology,     "how does DNA replication work in eukaryotic cells"},
    {Subject::Biology,     "what role do mitochondria play in the cell"},
    {Subject::Engineering, "what torque should I use on an M8 steel bolt"},
    {Subject::Engineering, "how do I size a steel beam for a 3 metre span"},
    {Subject::Philosophy,  "is free will compatible with determinism?"},
    {Subject::Philosophy,  "explain Kant's categorical imperative"},
    {Subject::Sociology,   "how does urbanisation affect social mobility"},
    {Subject::Sociology,   "what causes inflation in a modern economy"},
    {Subject::Language,    "proofread this paragraph and improve its tone"},
    {Subject::Language,    "translate 'good morning' into French"},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: batbot-routebench <router-model.gguf> [--gpu-layers N]\n");
        return 2;
    }

    ModelParams params;
    params.path        = paths::expand_user(argv[1]).string();
    params.model       = params.path;
    params.n_ctx       = 4096;
    params.n_gpu_layers = 0;
    params.temperature = 0.0F;  // greedy, so the number is reproducible
    params.max_tokens  = 16;

    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--gpu-layers") {
            params.n_gpu_layers = std::atoi(argv[i + 1]);
        }
    }

    ModelHost host(paths::log_file());
    std::string error;
    LoadedModel* model = host.acquire_router(params, [](float) {}, error);
    if (model == nullptr) {
        std::printf("could not load: %s\n", error.c_str());
        return 1;
    }

    std::printf("%s\n%.2fB parameters, %.1f GB\n\n", params.path.c_str(),
                static_cast<double>(model->params()) / 1e9,
                static_cast<double>(model->bytes()) / (1024.0 * 1024.0 * 1024.0));

    // Fallback is not in the grammar, so it can never appear here. This
    // measures the delegator's job -- picking a specialist -- and nothing else.
    ModelRouter router(*model, params);
    int    correct  = 0;
    double total_ms = 0.0;

    for (const Case& test : kCases) {
        const auto start = std::chrono::steady_clock::now();
        const RouteDecision decision = router.route(test.prompt, {});
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        total_ms += ms;

        const bool ok = decision.subject == test.expect;
        correct += ok ? 1 : 0;
        std::printf("%s  %-12s (want %-12s) conf %.2f  %4.0fms  %.44s\n",
                    ok ? "ok  " : "MISS",
                    std::string(subject_name(decision.subject)).c_str(),
                    std::string(subject_name(test.expect)).c_str(),
                    static_cast<double>(decision.confidence), ms, test.prompt);
    }

    std::printf("\n%d/%zu correct (%.0f%%), %.0fms per route\n", correct, kCases.size(),
                100.0 * static_cast<double>(correct) / static_cast<double>(kCases.size()),
                total_ms / static_cast<double>(kCases.size()));
    // Nine subjects, so anything near 11% is a model that is not reading the
    // prompt at all -- usually a chat template or prompt problem, not the model.
    return correct * 2 >= static_cast<int>(kCases.size()) ? 0 : 1;
}
