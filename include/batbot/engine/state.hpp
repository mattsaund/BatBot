// SPDX-License-Identifier: MIT
// The one piece of memory the engine thread and the UI thread share.
//
// The engine never touches FTXUI and the UI never touches llama.cpp. They meet
// only here: the engine mutates this state under a lock and pokes the screen,
// and the renderer takes a Snapshot under the same lock. Nothing else crosses.
#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "batbot/config/config.hpp"
#include "batbot/llm/model_host.hpp"
#include "batbot/routing/router.hpp"
#include "batbot/routing/subject.hpp"
#include "batbot/session/usage.hpp"

namespace batbot {

/// What BatBot himself is doing, which is what the sprite animates.
enum class Mood {
    Idle,
    Routing,   ///< the delegator is reading the prompt
    Loading,   ///< an expert is being swapped in
    Thinking,  ///< the expert is ingesting the prompt, no output yet
    Talking,   ///< tokens are streaming
    Error,
};

std::string_view mood_label(Mood mood);

/// A seat at the roundtable, as drawn.
enum class SeatPhase {
    Unconfigured,  ///< no GGUF assigned to this subject; drawn hollow
    Missing,       ///< a model is assigned but the file is not there
    Dormant,       ///< assigned, present on disk, not in memory
    Loading,       ///< being read in right now
    Active,        ///< resident and answering
};

struct SeatState {
    SeatPhase phase    = SeatPhase::Unconfigured;
    float     progress = 0.0F;  ///< 0..1 while Loading
};

/// One exchange. Kept as a unit so the transcript can show which expert
/// answered each turn, which is most of the point of the roundtable.
struct Turn {
    std::string            prompt;
    std::string            reply;

    /// The working a reasoning model does on the way to `reply`.
    ///
    /// Kept apart from the reply for two reasons. It is not the answer, and
    /// showing it as one is what made gpt-oss look broken. And it must not go
    /// back into the model's context next turn -- the format's own guidance is
    /// that previous reasoning is dropped, and feeding it back teaches the
    /// model that thinking aloud is part of the transcript.
    std::string            reasoning;

    /// What this turn looked up, one line each. Empty unless the expert used
    /// the web-search tool, and shown above the reply so the user can always
    /// see what left the machine.
    std::vector<std::string> searches;
    std::optional<RouteDecision> route;
    bool                   streaming = false;
    bool                   cancelled = false;
    bool                   failed    = false;
    double                 tokens_per_second = 0.0;
    int                    prompt_tokens     = 0;
    int                    output_tokens     = 0;
    long                   load_ms           = 0;  ///< JIT swap cost for this turn
};

/// A consistent copy of everything the renderer needs for one frame.
struct Snapshot {
    Mood                                mood = Mood::Idle;
    std::string                         status;
    std::array<SeatState, kSubjectCount> seats;
    std::optional<Subject>              resident;

    /// The expert this turn is flowing to, from the moment the delegator names
    /// it until the answer is finished. What the roundtable draws the line to,
    /// and what makes a seat's dot light up -- residency is a different
    /// question, and the status bar is where that is answered.
    std::optional<Subject>              linked;

    /// Is the delegator in memory and ready to route?
    ///
    /// Always true while it is set to stay loaded. With it set to load on
    /// demand this goes dark while an expert has the card and comes back when
    /// the delegator does, which is the honest picture of a machine that can
    /// only hold one of them at a time.
    bool delegator_ready = false;
    std::vector<Turn>                   turns;
    std::vector<std::string>            notices;
    bool                                busy = false;

    /// Tokens spent since BatBot started.
    TokenUsage session_usage;

    /// Tokens this project has ever spent, loaded from disk at startup and
    /// kept in step as the session goes on.
    TokenUsage project_usage;

    /// The rate of the reply currently streaming, or 0 when nothing is. Shown
    /// in preference to the session average, because while an answer is
    /// arriving that is the number being asked about.
    double live_tokens_per_second = 0.0;
};

class AppState {
public:
    Snapshot snapshot() const;

    void set_mood(Mood mood, std::string status = {});
    Mood mood() const;

    void set_seat(Subject subject, SeatPhase phase, float progress = 0.0F);
    void set_seat_progress(Subject subject, float progress);
    /// Recompute every seat from the config, distinguishing a seat with no
    /// model from one whose file has gone missing. Touches the filesystem, so
    /// call it on a config change rather than per frame.
    void configure_seats(const Config& config);
    void set_resident(std::optional<Subject> subject);

    /// Open a new turn and return its index.
    std::size_t begin_turn(std::string prompt);

    /// Append an already-finished turn, as `/resume` does. Its tokens are not
    /// added to the session total: they were spent in an earlier session and
    /// are already in the project total.
    void restore_turn(Turn turn);
    void set_route(std::size_t turn, RouteDecision route);
    void append_reply(std::size_t turn, std::string_view chunk);

    /// Append to a turn's reasoning. See Turn::reasoning.
    void append_reasoning(std::size_t turn, std::string_view chunk);

    /// Replace a turn's reply. Used when a round of generation turns out to
    /// have been a tool request rather than an answer.
    void set_reply(std::size_t turn, std::string text);

    /// Record a lookup this turn made. See Turn::searches.
    void add_search(std::size_t turn, std::string line);

    /// The expert work is flowing to, or nothing between turns.
    void set_linked(std::optional<Subject> subject);

    /// Whether the delegator is loaded and able to route.
    void set_delegator_ready(bool ready);
    void finish_turn(std::size_t turn, const GenerationStats& stats, long load_ms);
    void fail_turn(std::size_t turn, std::string_view reason);

    /// Report the rate of the reply in flight. Called every few tokens by the
    /// engine, and reset to 0 when the turn ends.
    void set_live_rate(double tokens_per_second);

    TokenUsage session_usage() const;
    TokenUsage project_usage() const;
    /// Seed the project total from disk. Called once, at startup.
    void set_project_usage(TokenUsage usage);

    void add_notice(std::string notice);
    void clear_notices();
    void clear_turns();

    void set_busy(bool busy);
    bool busy() const;

private:
    mutable std::mutex                   mutex_;
    Mood                                 mood_ = Mood::Idle;
    std::string                          status_;
    std::array<SeatState, kSubjectCount>  seats_{};
    std::optional<Subject>               resident_;
    std::optional<Subject>               linked_;
    bool                                 delegator_ready_ = false;
    std::vector<Turn>                    turns_;
    std::vector<std::string>             notices_;
    bool                                 busy_ = false;
    TokenUsage                           session_usage_;
    TokenUsage                           project_usage_;
    double                               live_rate_ = 0.0;
};

}  // namespace batbot
