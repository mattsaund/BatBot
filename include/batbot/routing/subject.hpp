// SPDX-License-Identifier: MIT
// The nine fundamental subjects BatBot delegates to, plus the metadata the
// router and the roundtable both need to talk about them.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace batbot {

/// One expert seat at the roundtable.
///
/// The order here is the order the experts are drawn in the ring, so changing
/// it changes the visual layout. `Count` is the sentinel.
enum class Subject : std::uint8_t {
    Mathematics = 0,
    Programming,
    Physics,
    Chemistry,
    Biology,
    Engineering,
    Philosophy,
    Sociology,
    Language,
    /// The fallback seat. Not a subject and never chosen by the delegator:
    /// the engine sends work here when the delegator is not confident enough
    /// to commit, or when the subject it chose has no model behind it.
    Fallback,
    Count,
};

inline constexpr std::size_t kSubjectCount = static_cast<std::size_t>(Subject::Count);

/// Everything BatBot knows about a subject that is not user configuration.
struct SubjectInfo {
    Subject          subject;
    std::string_view id;       ///< stable key used in config.json ("mathematics")
    std::string_view name;     ///< human-facing name ("Mathematics")
    std::string_view tag;      ///< 4-char roundtable chip label ("MATH")
    std::string_view blurb;    ///< compact remit, shown in settings and fed to the router
    /// Two questions this expert should obviously take.
    ///
    /// Two rather than one, and the same number for every subject so none is
    /// favoured by having more. Measured on the 54-prompt benchmark with
    /// LFM2.5-1.2B: one example each scores 89% and never once chooses
    /// Language; two scores 96% and reaches every seat. A small delegator is
    /// working almost entirely from these, and one example of a subject as
    /// broad as "writing, editing, translation and grammar" does not describe
    /// it.
    ///
    /// Neither may resemble anything in tools/routebench.cpp, or the benchmark
    /// is measuring how well the prompt was copied into it.
    std::string_view example;
    std::string_view example2;

    /// Whether the delegator may name this subject.
    ///
    /// False only for Fallback, and deliberately: the delegator's job is to
    /// pick a specialist, and offering it an "anything else" option is exactly
    /// what an earlier prompt did -- it answered that for almost everything and
    /// routing collapsed to 16%. The engine decides when to fall back; the
    /// delegator is never asked to.
    bool routable = true;
};

/// The full table, indexed by `static_cast<size_t>(Subject)`.
const std::array<SubjectInfo, kSubjectCount>& all_subjects();

/// Metadata for one subject. Undefined for `Subject::Count`.
const SubjectInfo& subject_info(Subject s);

inline std::string_view subject_id(Subject s)   { return subject_info(s).id; }
inline std::string_view subject_name(Subject s) { return subject_info(s).name; }
inline std::string_view subject_tag(Subject s)  { return subject_info(s).tag; }

/// Parse a subject from its config id ("physics") or its router tag ("PHYS").
/// Case-insensitive. Returns nullopt if nothing matches.
std::optional<Subject> subject_from_string(std::string_view text);

/// The tags the delegator chooses between, in table order and parallel to
/// routable_subjects(). Trimmed: `subject_tag` pads to a fixed width for the
/// roundtable chips, and that padding would change how the tag tokenises.
std::vector<std::string> router_labels();

/// Subjects the delegator is allowed to choose.
std::vector<Subject> routable_subjects();

/// The system prompt handed to the router model, listing every subject and its
/// remit. Generated from the table.
std::string router_system_prompt();

/// Worked examples for the router, as (question, "TAG 0.95") pairs generated
/// from the subject table.
///
/// These are sent as real user/assistant turns rather than pasted into the
/// system prompt. On a 1.2B model that difference took routing accuracy from
/// 42% to 74% on BatBot's own benchmark: a small instruct model follows a
/// demonstrated dialogue far more reliably than a block of Q/A text.
std::vector<std::pair<std::string, std::string>> router_examples();

}  // namespace batbot
