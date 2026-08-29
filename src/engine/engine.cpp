// SPDX-License-Identifier: MIT
//
// The delegation loop.
//
// Everything llama.cpp touches happens on this thread. The UI hands over a
// prompt and learns how it went through AppState plus a wake callback -- it
// never calls into the engine's internals, and the engine never calls into
// FTXUI.
//
// Config changes and expert releases ride the same queue as prompts, so they
// are applied in order and can never land in the middle of a generation.
#include "batbot/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "batbot/config/paths.hpp"
#include "batbot/engine/route_policy.hpp"

namespace batbot {
namespace {

using Clock = std::chrono::steady_clock;

long ms_since(Clock::time_point start) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

/// How many past turns to replay to an expert.
///
/// Bounded deliberately: a swapped-in expert re-ingests the whole history from
/// cold, so an unbounded transcript would make every turn slower than the last.
constexpr std::size_t kHistoryTurns = 12;

}  // namespace

Engine::Engine(Config config, AppState& state, std::function<void()> wake)
    : config_(std::move(config)), state_(state), wake_(std::move(wake)) {}

Engine::~Engine() {
    stop();
}

void Engine::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&Engine::run, this);
}

void Engine::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cancel_.store(true, std::memory_order_relaxed);
    queued_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Engine::submit(std::string prompt, std::optional<Subject> pinned) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(Request{RequestKind::Prompt, std::move(prompt), pinned, {}});
    }
    queued_.notify_one();
}

void Engine::apply_config(Config config) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(Request{RequestKind::ApplyConfig, {}, std::nullopt, std::move(config)});
    }
    queued_.notify_one();
}

Config Engine::config() const {
    const std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void Engine::cancel() {
    cancel_.store(true, std::memory_order_relaxed);
}

void Engine::release_expert() {
    // Queued rather than done inline: host_ belongs to the worker thread, and
    // reaching into it from the UI thread mid-generation would be a data race.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(Request{RequestKind::ReleaseExpert, {}, std::nullopt, {}});
    }
    queued_.notify_one();
}

void Engine::reset_history() {
    const std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
}

void Engine::run() {
    host_ = std::make_unique<ModelHost>(paths::log_file());

    for (const std::string& device : ModelHost::devices()) {
        state_.add_notice("device: " + device);
    }

    load_router();
    state_.configure_seats(config_);
    state_.set_mood(Mood::Idle);
    if (wake_) {
        wake_();
    }

    while (running_.load(std::memory_order_relaxed)) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock, [this] {
                return !pending_.empty() || !running_.load(std::memory_order_relaxed);
            });
            if (!running_.load(std::memory_order_relaxed)) {
                break;
            }
            request = std::move(pending_.front());
            pending_.pop_front();
        }

        if (request.kind == RequestKind::ReleaseExpert) {
            host_->release_expert();
            state_.set_resident(std::nullopt);
            state_.set_mood(Mood::Idle, "expert released");
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.kind == RequestKind::ApplyConfig) {
            do_apply_config(std::move(request.config));
            if (wake_) {
                wake_();
            }
            continue;
        }

        if (request.prompt.empty()) {
            continue;
        }

        busy_.store(true, std::memory_order_relaxed);
        state_.set_busy(true);
        cancel_.store(false, std::memory_order_relaxed);

        // llama.cpp reports some failures (a malformed grammar, a corrupt
        // GGUF) by throwing. Letting that escape the worker would terminate
        // the process and leave the user's terminal in raw mode, so every
        // request is contained: the turn fails, the session survives.
        try {
            handle(request);
        } catch (const std::exception& e) {
            state_.set_mood(Mood::Error, e.what());
            state_.add_notice(std::string("engine error: ") + e.what());
        } catch (...) {
            state_.set_mood(Mood::Error, "unknown engine error");
            state_.add_notice("engine error: unknown exception");
        }

        busy_.store(false, std::memory_order_relaxed);
        state_.set_busy(false);
        if (wake_) {
            wake_();
        }
    }

    // Free the models before the backend goes away.
    router_.reset();
    host_.reset();
}

void Engine::load_router() {
    if (config_.router.model.empty()) {
        router_ = std::make_unique<KeywordRouter>();
        state_.add_notice(
            "no delegator model assigned -- routing by keyword. Press ctrl-e and "
            "assign a small instruct model to the delegator for real routing.");
        return;
    }

    state_.set_mood(Mood::Loading, "loading router");
    if (wake_) {
        wake_();
    }

    std::string error;
    LoadedModel* model = host_->acquire_router(
        config_.router,
        [this](float progress) {
            state_.set_mood(Mood::Loading,
                            "loading router " + std::to_string(static_cast<int>(progress * 100))
                                + "%");
            if (wake_) {
                wake_();
            }
        },
        error);

    if (model == nullptr) {
        router_ = std::make_unique<KeywordRouter>();
        state_.add_notice("router: " + error + " -- falling back to keyword routing");
        return;
    }

    router_ = std::make_unique<ModelRouter>(*model, config_.router);
}

void Engine::do_apply_config(Config config) {
    config.resolve_models();

    std::string previous_router;
    std::string previous_expert;
    std::optional<Subject> resident = host_->loaded_expert();
    {
        const std::lock_guard<std::mutex> lock(config_mutex_);
        previous_router = config_.router.path;
        if (resident) {
            previous_expert = config_.experts[static_cast<std::size_t>(*resident)].path;
        }
        config_ = std::move(config);
    }

    const Config current = this->config();

    // Drop the resident expert if the file behind its seat changed, so the next
    // prompt loads what the user just chose rather than the old weights.
    if (resident) {
        const std::string now = current.experts[static_cast<std::size_t>(*resident)].path;
        if (now != previous_expert || now.empty()) {
            host_->release_expert();
            state_.set_resident(std::nullopt);
        }
    }

    // The router is resident for the whole session, so a change to it has to be
    // acted on here or it would never take effect.
    if (current.router.path != previous_router) {
        router_.reset();
        load_router();
    }

    state_.configure_seats(current);
    state_.set_resident(host_->loaded_expert());
    state_.set_mood(Mood::Idle, "settings applied");
}

RouteDecision Engine::resolve(const Request& request) {
    const CancelCallback cancel = [this] { return cancel_.load(std::memory_order_relaxed); };

    // Ask the delegator -- or skip it entirely for a pinned route.
    RouteDecision decision;
    if (request.pinned) {
        decision.subject    = *request.pinned;
        decision.confidence = 1.0F;
        decision.source     = RouteSource::Forced;
        decision.detail     = "pinned by slash command";
    } else if (router_) {
        decision = router_->route(request.prompt, cancel);
    }

    // Then decide what to do about it.
    return apply_route_policy(decision, config_);
}

void Engine::handle(const Request& request) {
    const std::size_t turn = state_.begin_turn(request.prompt);

    const CancelCallback cancel = [this] { return cancel_.load(std::memory_order_relaxed); };

    // --- route -------------------------------------------------------------
    state_.set_mood(Mood::Routing, "BatBot is reading the prompt");
    if (wake_) {
        wake_();
    }

    const RouteDecision decision = resolve(request);
    state_.set_route(turn, decision);
    if (wake_) {
        wake_();
    }

    if (cancel_.load(std::memory_order_relaxed)) {
        state_.fail_turn(turn, "cancelled before routing finished");
        state_.set_mood(Mood::Idle);
        return;
    }

    if (!config_.has_expert(decision.subject)) {
        state_.fail_turn(turn,
            "No expert model is configured yet. Edit " + paths::config_file().string()
            + " and point at least one expert at a GGUF file.");
        state_.set_mood(Mood::Error, "no experts configured");
        return;
    }

    // --- JIT swap ----------------------------------------------------------
    const ModelParams& params = config_.experts[static_cast<std::size_t>(decision.subject)];
    const bool already_resident = host_->loaded_expert() == decision.subject;

    long load_ms = 0;
    if (!already_resident) {
        state_.set_resident(std::nullopt);
        state_.set_seat(decision.subject, SeatPhase::Loading, 0.0F);
        state_.set_mood(Mood::Loading,
                        "swapping in " + std::string(subject_name(decision.subject)));
        if (wake_) {
            wake_();
        }
    }

    const auto load_start = Clock::now();
    std::string error;
    LoadedModel* expert = host_->acquire_expert(
        decision.subject, params,
        [this, &decision](float progress) {
            state_.set_seat_progress(decision.subject, progress);
            if (wake_) {
                wake_();
            }
        },
        error);
    load_ms = already_resident ? 0 : ms_since(load_start);

    if (expert == nullptr) {
        state_.set_seat(decision.subject, SeatPhase::Dormant);
        state_.fail_turn(turn, error);
        state_.set_mood(Mood::Error, error);
        return;
    }

    state_.set_resident(decision.subject);
    if (wake_) {
        wake_();
    }

    // --- generate ----------------------------------------------------------
    state_.set_mood(Mood::Thinking,
                    std::string(subject_name(decision.subject)) + " is reading");
    if (wake_) {
        wake_();
    }

    std::vector<ChatMessage> messages;
    messages.push_back({"system", config_.system_prompt});
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t keep = kHistoryTurns * 2;
        const std::size_t from = history_.size() > keep ? history_.size() - keep : 0;
        messages.insert(messages.end(), history_.begin() + static_cast<long>(from),
                        history_.end());
    }
    messages.push_back({"user", request.prompt});

    bool first_token = true;
    const GenerationStats stats = expert->generate(
        expert->format_chat(messages, true), params,
        [&](std::string_view chunk) {
            if (first_token) {
                first_token = false;
                state_.set_mood(Mood::Talking,
                                std::string(subject_name(decision.subject)) + " is answering");
            }
            state_.append_reply(turn, chunk);
            if (wake_) {
                wake_();
            }
        },
        cancel);

    state_.finish_turn(turn, stats, load_ms);

    // Only remember exchanges that actually produced something, so a cancelled
    // turn does not poison the context of the next one.
    if (!stats.cancelled && stats.output_tokens > 0) {
        const Snapshot current = state_.snapshot();
        const std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back({"user", request.prompt});
        history_.push_back({"assistant", turn < current.turns.size()
                                             ? current.turns[turn].reply
                                             : std::string{}});
    }

    state_.set_mood(Mood::Idle, stats.cancelled ? "cancelled" : "");
}

}  // namespace batbot
