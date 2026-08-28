#include "batbot/core/state.hpp"

#include <algorithm>
#include <filesystem>

namespace batbot {

std::string_view mood_label(Mood mood) {
    switch (mood) {
        case Mood::Idle:     return "idle";
        case Mood::Routing:  return "routing";
        case Mood::Loading:  return "loading";
        case Mood::Thinking: return "thinking";
        case Mood::Talking:  return "answering";
        case Mood::Error:    return "error";
    }
    return "idle";
}

Snapshot AppState::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    Snapshot copy;
    copy.mood     = mood_;
    copy.status   = status_;
    copy.seats    = seats_;
    copy.resident = resident_;
    copy.turns    = turns_;
    copy.notices  = notices_;
    copy.busy     = busy_;
    return copy;
}

void AppState::set_mood(Mood mood, std::string status) {
    const std::lock_guard<std::mutex> lock(mutex_);
    mood_   = mood;
    status_ = std::move(status);
}

Mood AppState::mood() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return mood_;
}

void AppState::set_seat(Subject subject, SeatPhase phase, float progress) {
    const auto index = static_cast<std::size_t>(subject);
    if (index >= kSubjectCount) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    seats_[index].phase    = phase;
    seats_[index].progress = progress;
}

void AppState::set_seat_progress(Subject subject, float progress) {
    const auto index = static_cast<std::size_t>(subject);
    if (index >= kSubjectCount) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    seats_[index].progress = progress;
}

void AppState::configure_seats(const Config& config) {
    // Resolve existence outside the lock: stat-ing nine files while holding the
    // mutex would block the render thread for no reason.
    std::array<SeatPhase, kSubjectCount> phases{};
    for (const SubjectInfo& info : all_subjects()) {
        const auto index = static_cast<std::size_t>(info.subject);
        const ModelParams& params = config.experts[index];
        if (params.model.empty()) {
            phases[index] = SeatPhase::Unconfigured;
        } else if (std::filesystem::exists(params.path)) {
            phases[index] = SeatPhase::Dormant;
        } else {
            // Assigned but absent. Saying so is better than showing a seat as
            // ready and only failing when someone routes a prompt to it.
            phases[index] = SeatPhase::Missing;
        }
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < kSubjectCount; ++i) {
        seats_[i] = SeatState{phases[i], 0.0F};
    }
}

void AppState::set_resident(std::optional<Subject> subject) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Demote whoever was previously active; only one expert is ever resident.
    for (SeatState& seat : seats_) {
        if (seat.phase == SeatPhase::Active) {
            seat.phase = SeatPhase::Dormant;
        }
    }
    resident_ = subject;
    if (subject) {
        const auto index = static_cast<std::size_t>(*subject);
        if (index < kSubjectCount) {
            seats_[index].phase    = SeatPhase::Active;
            seats_[index].progress = 1.0F;
        }
    }
}

std::size_t AppState::begin_turn(std::string prompt) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Turn turn;
    turn.prompt    = std::move(prompt);
    turn.streaming = true;
    turns_.push_back(std::move(turn));
    return turns_.size() - 1;
}

void AppState::set_route(std::size_t turn, RouteDecision route) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn < turns_.size()) {
        turns_[turn].route = std::move(route);
    }
}

void AppState::append_reply(std::size_t turn, std::string_view chunk) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn < turns_.size()) {
        turns_[turn].reply.append(chunk);
    }
}

void AppState::finish_turn(std::size_t turn, const GenerationStats& stats, long load_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn >= turns_.size()) {
        return;
    }
    Turn& entry = turns_[turn];
    entry.streaming         = false;
    entry.cancelled         = stats.cancelled;
    entry.tokens_per_second = stats.tokens_per_second();
    entry.output_tokens     = stats.output_tokens;
    entry.load_ms           = load_ms;
}

void AppState::fail_turn(std::size_t turn, std::string_view reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn >= turns_.size()) {
        return;
    }
    Turn& entry = turns_[turn];
    entry.streaming = false;
    entry.failed    = true;
    if (entry.reply.empty()) {
        entry.reply = reason;
    }
}

void AppState::add_notice(std::string notice) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Startup can produce one warning per misconfigured expert; keep the list
    // bounded so a badly broken config cannot push the chat off screen.
    if (notices_.size() < 32) {
        notices_.push_back(std::move(notice));
    }
}

void AppState::clear_notices() {
    const std::lock_guard<std::mutex> lock(mutex_);
    notices_.clear();
}

void AppState::clear_turns() {
    const std::lock_guard<std::mutex> lock(mutex_);
    turns_.clear();
}

void AppState::set_busy(bool busy) {
    const std::lock_guard<std::mutex> lock(mutex_);
    busy_ = busy;
}

bool AppState::busy() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

}  // namespace batbot
