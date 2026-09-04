// SPDX-License-Identifier: MIT
// The delegation loop, running on its own thread.
//
// Everything llama.cpp touches lives here. The UI hands the engine a prompt and
// gets told, via AppState plus a wake callback, how the delegation is going.
// The engine blocks for seconds at a time loading a 30B expert; keeping it off
// the UI thread is what lets the roundtable keep animating while that happens.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "crucible/config/config.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/routing/router.hpp"
#include "crucible/engine/state.hpp"

namespace crucible {

class Engine {
public:
    /// `wake` is called whenever the state changed and the screen should be
    /// redrawn. It must be safe to call from a non-UI thread.
    Engine(Config config, AppState& state, std::function<void()> wake);
    ~Engine();
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    /// Start the worker and load the router model. Returns immediately.
    void start();

    /// Stop the worker, cancelling any generation in flight.
    void stop();

    /// Queue a prompt. `pinned` skips routing and sends it straight to that
    /// expert, which is how `/physics ...` works.
    void submit(std::string prompt, std::optional<Subject> pinned = std::nullopt);

    /// Ask the current generation to stop at the next token boundary.
    void cancel();

    /// Drop the resident expert, freeing its memory without exiting.
    void release_expert();

    /// Drop every loaded model and load the router again.
    ///
    /// For when the hardware under Crucible changed: a model picks its devices
    /// once, when it loads, so a runtime installed from the settings screen
    /// does nothing for the model that is already resident. This is what makes
    /// a new GPU backend take effect without restarting.
    void reload_models();

    /// Forget the conversation history sent to experts.
    void reset_history();

    /// Replace that history wholesale, which is what resuming a stored
    /// conversation needs: the expert has to see what is already on screen or
    /// it will answer the next question with no idea what came before.
    void restore_history(std::vector<ChatMessage> history);

    /// Replace the running configuration, as the settings screen does.
    ///
    /// Applied on the worker thread between requests, never mid-generation.
    /// A changed router model is reloaded; an expert whose model changed is
    /// evicted so the next prompt picks up the new file.
    void apply_config(Config config);

    /// A snapshot of the configuration currently in force.
    Config config() const;

    bool is_busy() const { return busy_.load(std::memory_order_relaxed); }

private:
    /// One unit of work for the engine thread. `kind` keeps config changes and
    /// expert releases on the same queue as prompts, so they are applied in
    /// order and never race with a generation in flight.
    enum class RequestKind { Prompt, ReleaseExpert, ReloadModels, ApplyConfig };

    struct Request {
        RequestKind            kind = RequestKind::Prompt;
        std::string            prompt;
        std::optional<Subject> pinned;
        Config                 config;
    };

    void run();
    void handle(const Request& request);
    void load_router();
    void load_router_if_resident();

    /// Make sure the delegator is loaded, freeing the expert first if there is
    /// no room for both. A no-op when it is already there.
    void ensure_router();

    /// Drop the delegator, wrapper first. Only called with "keep delegator
    /// loaded" off.
    void release_router();
    void do_apply_config(Config config);

    /// Pick the expert that will actually answer. The router names a subject;
    /// this decides what to do when that subject has no model configured.
    RouteDecision resolve(const Request& request);

    Config                 config_;
    mutable std::mutex     config_mutex_;   ///< guards config_ against the UI thread
    AppState&              state_;
    std::function<void()>  wake_;

    std::unique_ptr<ModelHost> host_;
    std::unique_ptr<Router>    router_;

    /// The delegator's measured bias, kept across reloads of the same file so
    /// an on-demand delegator does not re-measure it every prompt. See
    /// ModelRouter::bias.
    std::vector<float>         router_bias_;
    std::string                router_bias_for_;

    std::vector<ChatMessage> history_;

    std::thread             worker_;
    std::mutex              mutex_;
    std::condition_variable queued_;
    std::deque<Request>     pending_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       cancel_{false};
    std::atomic<bool>       busy_{false};
};

}  // namespace crucible
