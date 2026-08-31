// SPDX-License-Identifier: MIT
//
// The subject table, and everything generated from it.
//
// This table is the single source of truth. The labels the delegator chooses
// between, its system prompt, its worked examples, and the order seats appear
// at the roundtable are all derived from the array below, so they cannot drift
// apart:
// adding a subject here adds it everywhere, and the static assertions catch the
// one place that must be kept in step by hand.
#include "batbot/routing/subject.hpp"

#include <algorithm>
#include <cctype>

namespace batbot {
namespace {

constexpr std::array<SubjectInfo, kSubjectCount> kSubjects{{
    {Subject::Mathematics, "mathematics", "Mathematics", "MATH",
     "mathematics, algebra, calculus, proofs, geometry, statistics, probability, number theory",
     "what is the integral of x squared", "what is the probability of rolling two sixes in a row"},
    {Subject::Programming, "programming", "Programming", "PROG",
     "programming, code, software, algorithms, data structures, debugging, systems, a codebase",
     "my python script throws a KeyError", "my docker container exits the moment it starts"},
    {Subject::Physics, "physics", "Physics", "PHYS",
     "physics, forces, energy, light, thermodynamics, relativity, quantum, astronomy",
     "why do heavy and light objects fall together", "why does a helium balloon rise"},
    {Subject::Chemistry, "chemistry", "Chemistry", "CHEM",
     "chemistry, reactions, molecules, bonding, acids, pH, materials, the laboratory",
     "what happens when sodium touches water", "why does salt melt ice"},
    {Subject::Biology, "biology", "Biology", "BIO ",
     "biology, cells, DNA, genetics, physiology, medicine, ecology, evolution",
     "how do vaccines train the immune system", "how do muscles get oxygen during exercise"},
    {Subject::Engineering, "engineering", "Engineering", "ENG ",
     "engineering, designing or building a physical thing, mechanical electrical and civil design, bolts, beams, loads, circuits, wiring, tolerances, materials, hardware, CAD",
     "what preload should this bolted joint have", "how do I stop this bracket from vibrating"},
    {Subject::Philosophy, "philosophy", "Philosophy", "PHIL",
     "philosophy, ethics, right and wrong, logic, metaphysics, epistemology, free will, consciousness, knowledge, existence, meaning",
     "can someone be blamed for an unavoidable act", "can a machine ever be said to understand anything"},
    {Subject::Sociology, "sociology", "Sociology", "SOC ",
     "society, economics, politics, history, psychology, culture, institutions, law, education, inequality, cities, populations, why people or groups behave as they do",
     "why did rents rise faster than wages", "what makes a protest movement succeed"},
    {Subject::Language, "language", "Language", "LANG",
     "writing, grammar, spelling, punctuation, a sentence, a paragraph, an essay, proofreading, editing, rewriting, tone, style, summarising, translation, a word or its meaning, literature",
     "what is the difference between affect and effect", "what is the plural of octopus"},
    // Never offered to the delegator, so this text is for the settings screen
    // only -- it is not part of the routing prompt.
    {Subject::Fallback, "fallback", "Fallback", "FALL",
     "takes prompts the delegator could not place, and prompts routed to a seat "
     "with no model",
     "", "", /*routable=*/false},
}};

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Router tags are fixed-width for the roundtable chips, so "BIO " carries a
/// trailing space. Trim before comparing.
std::string_view trim(std::string_view text) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    while (!text.empty() && !not_space(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && !not_space(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

const std::array<SubjectInfo, kSubjectCount>& all_subjects() {
    return kSubjects;
}

const SubjectInfo& subject_info(Subject s) {
    const auto index = static_cast<std::size_t>(s);
    return kSubjects[index < kSubjectCount ? index : 0];
}

std::optional<Subject> subject_from_string(std::string_view text) {
    const std::string needle = to_lower(trim(text));
    if (needle.empty()) {
        return std::nullopt;
    }
    for (const SubjectInfo& info : kSubjects) {
        if (needle == to_lower(info.id) || needle == to_lower(trim(info.tag))) {
            return info.subject;
        }
    }
    return std::nullopt;
}

std::vector<Subject> routable_subjects() {
    std::vector<Subject> routable;
    for (const SubjectInfo& info : kSubjects) {
        if (info.routable) {
            routable.push_back(info.subject);
        }
    }
    return routable;
}

std::vector<std::string> router_labels() {
    // The name, not the tag.
    //
    // This is worth 39 points. Scored on BatBot's 54-prompt benchmark with
    // LFM2.5-1.2B, answering with the four-letter tags gets 48% and answering
    // with the subject names gets 87%. The tags are not words: a delegator
    // choosing between "PHIL" and "PHYS" is comparing two spellings that share
    // a first token, and every philosophy question in the set went to physics.
    // "Philosophy" and "Physics" are things the model knows about.
    //
    // The tags stay in the system prompt, where they measurably help (dropping
    // them costs 6 points), and on the roundtable, where a fixed width is what
    // makes the chips line up.
    std::vector<std::string> labels;
    for (const SubjectInfo& info : kSubjects) {
        if (info.routable) {
            labels.emplace_back(info.name);
        }
    }
    return labels;
}

std::string router_system_prompt() {
    // Routable subjects only, and deliberately compact. An earlier version
    // described each subject in prose and closed by naming a catch-all; small
    // models latched onto that closing line and answered it for almost
    // everything (16% accurate). The delegator's job is to pick a specialist,
    // so it is never shown a way to decline.
    std::string prompt =
        "You label a question with the one subject it belongs to.\n"
        "Reply with only a tag and a confidence.\n\n";
    for (const SubjectInfo& info : kSubjects) {
        if (!info.routable) {
            continue;  // the delegator is never offered the fallback seat
        }
        // Tag, name, then keywords. The tag earns its place here even though
        // the delegator now answers with the name: on the 54-prompt benchmark,
        // listing the options without their tags costs 6 points (87% to 81%).
        // Reading it as a labelled menu appears to be what helps.
        prompt += info.tag;
        prompt += "  ";
        prompt += info.name;
        prompt += ": ";
        prompt += info.blurb;
        prompt += "\n";
    }
    return prompt;
}

std::vector<std::pair<std::string, std::string>> router_examples() {
    std::vector<std::pair<std::string, std::string>> examples;
    examples.reserve(kSubjectCount);
    for (const SubjectInfo& info : kSubjects) {
        if (!info.routable) {
            continue;
        }
        // The answer is exactly one of router_labels() and nothing else. The
        // delegator no longer writes a confidence -- that comes from comparing
        // the labels against each other now -- and an example that showed one
        // would teach it to continue past the string being scored.
        examples.emplace_back(std::string(info.example), std::string(info.name));
        if (!info.example2.empty()) {
            examples.emplace_back(std::string(info.example2), std::string(info.name));
        }
    }
    return examples;
}

}  // namespace batbot
