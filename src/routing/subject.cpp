// SPDX-License-Identifier: MIT
//
// The subject table, and everything generated from it.
//
// This table is the single source of truth. The routing grammar, the delegator's
// system prompt, its worked examples, and the order seats appear at the
// roundtable are all derived from the array below, so they cannot drift apart:
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
     "what is the integral of x squared"},
    {Subject::Programming, "programming", "Programming", "PROG",
     "programming, code, software, algorithms, data structures, debugging, systems, a codebase",
     "my python script throws a KeyError"},
    {Subject::Physics, "physics", "Physics", "PHYS",
     "physics, forces, energy, light, thermodynamics, relativity, quantum, astronomy",
     "why do heavy and light objects fall together"},
    {Subject::Chemistry, "chemistry", "Chemistry", "CHEM",
     "chemistry, reactions, molecules, bonding, acids, pH, materials, the laboratory",
     "what happens when sodium touches water"},
    {Subject::Biology, "biology", "Biology", "BIO ",
     "biology, cells, DNA, genetics, physiology, medicine, ecology, evolution",
     "how do vaccines train the immune system"},
    {Subject::Engineering, "engineering", "Engineering", "ENG ",
     "engineering, mechanical electrical and civil design, bolts, beams, circuits, hardware, CAD",
     "what preload should this bolted joint have"},
    {Subject::Philosophy, "philosophy", "Philosophy", "PHIL",
     "philosophy, ethics, logic, metaphysics, epistemology, free will, consciousness, meaning",
     "can someone be blamed for an unavoidable act"},
    {Subject::Sociology, "sociology", "Sociology", "SOC ",
     "society, economics, politics, history, psychology, culture, institutions",
     "why did rents rise faster than wages"},
    {Subject::Language, "language", "Language", "LANG",
     "writing, grammar, translation, editing, rhetoric, literature",
     "rewrite this sentence to sound more formal"},
    // Never offered to the delegator, so this text is for the settings screen
    // only -- it is not part of the routing prompt.
    {Subject::Fallback, "fallback", "Fallback", "FALL",
     "takes prompts the delegator could not place, and prompts routed to a seat "
     "with no model",
     "", /*routable=*/false},
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

std::string router_grammar() {
    // Constraining the router to this grammar means an invalid route is not
    // merely unlikely, it is unrepresentable: the sampler can only emit tokens
    // that keep the output on a path through these rules.
    std::string grammar = "root ::= subject \" \" confidence\n";
    grammar += "subject ::= ";
    bool first = true;
    for (const SubjectInfo& info : kSubjects) {
        if (!info.routable) {
            continue;
        }
        if (!first) {
            grammar += " | ";
        }
        grammar += '"';
        grammar += trim(info.tag);
        grammar += '"';
        first = false;
    }
    grammar += "\n";
    grammar += "confidence ::= \"0.\" [0-9] [0-9] | \"1.00\"\n";
    return grammar;
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
        // Tag, name, then keywords. Measured at temperature 0 on BatBot's own
        // 19-prompt benchmark with LFM2-1.2B: this form 63%, dropping the name
        // 58%, and the prose description this replaced 42%.
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
        examples.emplace_back(std::string(info.example),
                              std::string(trim(info.tag)) + " 0.95");
    }
    return examples;
}

}  // namespace batbot
