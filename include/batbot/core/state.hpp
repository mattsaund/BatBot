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

#include "batbot/core/config.hpp"
#include "batbot/core/model_host.hpp"
#include "batbot/core/router.hpp"
#include "batbot/core/subject.hpp"

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
    std::optional<RouteDecision> route;
    bool                   streaming = false;
    bool                   cancelled = false;
    bool                   failed    = false;
    double                 tokens_per_second = 0.0;
    int                    output_tokens     = 0;
    long                   load_ms           = 0;  ///< JIT swap cost for this turn
};

/// A consistent copy of everything the renderer needs for one frame.
struct Snapshot {
    Mood                                mood = Mood::Idle;
    std::string                         status;
    std::array<SeatState, kSubjectCount> seats;
    std::optional<Subject>              resident;
    std::vector<Turn>                   turns;
    std::vector<std::string>            notices;
    bool                                busy = false;
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
    void set_route(std::size_t turn, RouteDecision route);
    void append_reply(std::size_t turn, std::string_view chunk);
    void finish_turn(std::size_t turn, const GenerationStats& stats, long load_ms);
    void fail_turn(std::size_t turn, std::string_view reason);

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
    std::vector<Turn>                    turns_;
    std::vector<std::string>             notices_;
    bool                                 busy_ = false;
};

}  // namespace batbot
