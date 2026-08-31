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
#include "batbot/routing/benchmark.hpp"
#include "batbot/routing/router.hpp"
#include <array>
#include <cstdlib>

using namespace batbot;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: batbot-routebench <router-model.gguf> [--gpu-layers N]\n");
        return 2;
    }

    float       calibration = ModelRouter::kCalibration;
    std::string explain;

    ModelParams params;
    params.path        = paths::expand_user(argv[1]).string();
    params.model       = params.path;
    params.n_ctx       = 4096;
    // Every layer on the GPU by default, which is what the app does. --gpu-layers 0
    // forces the processor, for measuring a machine with no GPU at all.
    params.n_gpu_layers = -1;
    params.temperature = 0.0F;  // greedy, so the number is reproducible
    params.max_tokens  = 16;

    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--quiet") { quiet = true; }
    }
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--gpu-layers") {
            params.n_gpu_layers = std::atoi(argv[i + 1]);
        }
        if (std::string(argv[i]) == "--calibration") {
            calibration = static_cast<float>(std::atof(argv[i + 1]));
        }
        if (std::string(argv[i]) == "--explain") {
            explain = argv[i + 1];
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
    router.set_calibration(calibration);

    // --explain dumps the arithmetic behind one decision, which is the only way
    // to tell a delegator that dislikes a subject from a calibration that is
    // taking it away.
    if (!explain.empty()) {
        router.route("warm up so the bias is measured", {});
        const std::vector<float> raw  = router.raw_scores(explain);
        const std::vector<float> bias = router.bias();
        const std::vector<Subject> subjects = routable_subjects();
        std::printf("%-14s %9s %9s %9s\n", "subject", "raw", "bias", "calibrated");
        for (std::size_t i = 0; i < subjects.size() && i < raw.size(); ++i) {
            const double adjusted =
                static_cast<double>(raw[i]) -
                static_cast<double>(calibration) *
                    (i < bias.size() ? static_cast<double>(bias[i]) : 0.0);
            std::printf("%-14s %9.2f %9.2f %9.2f\n",
                        std::string(subject_name(subjects[i])).c_str(),
                        static_cast<double>(raw[i]),
                        i < bias.size() ? static_cast<double>(bias[i]) : 0.0, adjusted);
        }
        std::printf("\nprompt: %s\n", explain.c_str());
        return 0;
    }
    std::printf("calibration %.2f\n\n", static_cast<double>(calibration));
    int    correct  = 0;
    double total_ms = 0.0;

    // [wanted][got], plus the margins, for the breakdown below.
    std::array<std::array<int, kSubjectCount>, kSubjectCount> confusion{};
    std::array<int, kSubjectCount> expected{};
    std::array<int, kSubjectCount> chosen{};

    for (const RouteCase& test : benchmark_cases()) {
        const auto start = std::chrono::steady_clock::now();
        const RouteDecision decision = router.route(std::string(test.prompt), {});
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        total_ms += ms;

        const bool ok = decision.subject == test.expect;
        correct += ok ? 1 : 0;
        ++expected[static_cast<std::size_t>(test.expect)];
        ++chosen[static_cast<std::size_t>(decision.subject)];
        ++confusion[static_cast<std::size_t>(test.expect)][static_cast<std::size_t>(decision.subject)];
        if (!quiet) {
            std::printf("%s  %-12s (want %-12s) conf %.2f  %4.0fms  %.44s\n",
                        ok ? "ok  " : "MISS",
                        std::string(subject_name(decision.subject)).c_str(),
                        std::string(subject_name(test.expect)).c_str(),
                        static_cast<double>(decision.confidence), ms, std::string(test.prompt).c_str());
        }
    }

    // Per subject, because an average hides the failure that matters. A
    // delegator can score 85% while never once choosing one of the nine, and
    // that seat is then unreachable however good the model is.
    std::printf("\n%-14s %-8s %-8s  where the misses went\n", "subject", "found", "chosen");
    for (const Subject subject : routable_subjects()) {
        const auto index = static_cast<std::size_t>(subject);
        std::string went;
        for (const Subject other : routable_subjects()) {
            const auto to = static_cast<std::size_t>(other);
            if (other != subject && confusion[index][to] > 0) {
                if (!went.empty()) {
                    went += ", ";
                }
                went += std::string(subject_name(other)) + " x"
                      + std::to_string(confusion[index][to]);
            }
        }
        // "chosen" counts how often the delegator picked this subject for
        // anything at all. A zero there is a seat nothing can reach.
        std::printf("%-14s %d/%-6d %-8d  %s\n",
                    std::string(subject_name(subject)).c_str(),
                    confusion[index][index], expected[index], chosen[index], went.c_str());
    }

    std::printf("\n%d/%zu correct (%.0f%%), %.0fms per route\n", correct, benchmark_cases().size(),
                100.0 * static_cast<double>(correct) / static_cast<double>(benchmark_cases().size()),
                total_ms / static_cast<double>(benchmark_cases().size()));
    // Nine subjects, so anything near 11% is a model that is not reading the
    // prompt at all -- usually a chat template or prompt problem, not the model.
    return correct * 2 >= static_cast<int>(benchmark_cases().size()) ? 0 : 1;
}
