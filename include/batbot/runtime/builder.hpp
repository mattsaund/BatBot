// SPDX-License-Identifier: MIT
//
// Building a GPU runtime from source, on demand, from inside the TUI.
//
// Installing CUDA support means compiling one ggml backend against the same
// llama.cpp the binary was built from -- a few minutes of cmake that must not
// block the UI and must be abandonable halfway through. This runs it on a
// worker thread and publishes progress the settings screen polls.
//
// It deliberately does *not* install system packages. That needs root, and a
// TUI is the wrong place to ask for it; install.sh puts the SDKs in place, and
// what is missing is reported as advice instead.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "batbot/runtime/backend.hpp"
#include "batbot/util/subprocess.hpp"

namespace batbot {

/// Where a build has got to. Copied under a lock for each frame.
struct BuildProgress {
    enum class Phase {
        Idle,
        FetchingSource,  ///< cloning llama.cpp, the first time only
        Configuring,     ///< cmake -S -B: finds the SDK, generates the build
        Compiling,       ///< the long one, and the only phase with a percentage
        Installing,      ///< copying the module into the runtimes directory
        Done,
        Failed,
        Cancelled,
    };

    Phase       phase   = Phase::Idle;
    BackendKind kind    = BackendKind::Cpu;
    float       percent = 0.0F;   ///< 0..1, meaningful during Compiling
    std::string step;             ///< the file being compiled, or the phase
    std::string error;            ///< set when phase is Failed
    std::chrono::steady_clock::time_point started{};

    /// The tail of the build log, for a failure the user has to act on.
    std::vector<std::string> log_tail;

    /// Where the full log lives, which is what a bug report should carry.
    std::filesystem::path log_file;

    bool finished() const {
        return phase == Phase::Done || phase == Phase::Failed || phase == Phase::Cancelled;
    }
    bool running() const { return phase != Phase::Idle && !finished(); }

    /// "configuring", "compiling 42%" -- one line for the settings screen.
    std::string label() const;
};

/// Builds one runtime at a time, on a thread of its own.
class RuntimeBuilder {
public:
    RuntimeBuilder() = default;
    ~RuntimeBuilder();
    RuntimeBuilder(const RuntimeBuilder&)            = delete;
    RuntimeBuilder& operator=(const RuntimeBuilder&) = delete;

    /// Start building `kind`. `on_change` is called from the worker whenever
    /// progress moved, so the UI can redraw; it must be safe off the UI thread.
    /// Returns false if a build is already running.
    bool start(BackendKind kind, std::function<void()> on_change);

    /// Ask the running build to stop. The compiler is signalled, so this takes
    /// effect within a second rather than at the end of the build.
    void cancel();

    /// Cancel and wait for the worker to actually be gone.
    ///
    /// Needed at shutdown: the worker calls `on_change` to redraw, and that
    /// callback reaches into the application. Letting the thread outlive the
    /// objects it pokes is a use-after-free, so teardown blocks here.
    void stop();

    /// Forget a finished build, returning the builder to Idle.
    void dismiss();

    BuildProgress progress() const;

private:
    void run(BackendKind kind);

    /// Make sure runtime-src holds llama.cpp at the tag this binary was built
    /// against. Cloning it is the one step that needs the network.
    bool ensure_source(std::string& error);

    /// Run one command, streaming its output into the log and the progress.
    /// `parse_percent` turns "[ 42%]" lines into a percentage.
    bool run_command(const std::vector<std::string>& argv,
                     const std::filesystem::path& cwd,
                     BuildProgress::Phase phase,
                     bool parse_percent);

    void set_phase(BuildProgress::Phase phase, std::string step = {});
    void fail(std::string error);

    mutable std::mutex    mutex_;
    BuildProgress         progress_;
    std::vector<std::string> log_;      ///< the whole log, trimmed to a bound
    std::thread           worker_;
    std::atomic<bool>     cancel_{false};
    std::atomic<bool>     running_{false};
    std::function<void()> on_change_;

    /// The child currently running, so cancel() can signal it from the UI
    /// thread while the worker is blocked reading its output.
    std::mutex                         child_mutex_;
    std::unique_ptr<util::Subprocess>  child_;
};

}  // namespace batbot
