// SPDX-License-Identifier: MIT
//
// The two routers.
//
// KeywordRouter needs no model at all and keeps Crucible usable with nothing
// installed. ModelRouter asks the delegator, and does it by scoring every
// subject rather than by generating one: an invalid answer is not merely
// unlikely, there is nowhere for it to come from.
//
// The keyword table below matches whole words only. Substring matching sounds
// harmless until "ion" fires inside "function" and sends every programming
// question to Chemistry.
#include "crucible/routing/router.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>

namespace crucible {
namespace {

/// Keyword sets for the model-free router. Deliberately weighted towards terms
/// that are unambiguous markers of a field -- "integral" is maths, "titration"
/// is chemistry -- because a word that appears everywhere ("energy", "model")
/// costs more in false positives than it earns in recall.
struct KeywordSet {
    Subject                       subject;
    std::array<std::string_view, 24> words;
};

constexpr std::array<KeywordSet, kSubjectCount> kKeywords{{
    {Subject::Mathematics, {"integral","derivative","theorem","proof","matrix","algebra",
        "calculus","equation","polynomial","topology","geometry","probability","modulo",
        "eigenvalue","factorial","logarithm","prime","vector space","summation","limit",
        "differential","combinatorics","cardinality","isomorphism"}},
    {Subject::Programming, {"code","function","compile","debug","refactor","python","c++",
        "rust","javascript","algorithm","segfault","repository","git","api","pointer",
        "recursion","runtime","syntax","framework","typescript","kernel","binary",
        "linked list","stack trace"}},
    {Subject::Physics, {"quantum","relativity","momentum","thermodynamics","entropy",
        "electromagnetic","photon","lagrangian","hamiltonian","velocity","acceleration",
        "gravity","particle","wavelength","voltage","kinetic","newton","tensor field",
        "spacetime","fermion","boson","optics","friction","orbital mechanics"}},
    {Subject::Chemistry, {"molecule","reaction","atom","bond","stoichiometry","titration",
        "catalyst","organic","ion","ph","enthalpy","reagent","solvent","isotope",
        "periodic table","valence","oxidation","polymer","acid","alkane","molarity",
        "chromatography","electrolysis","compound"}},
    {Subject::Biology, {"cell","dna","protein","enzyme","gene","evolution","organism",
        "mitochondria","neuron","bacteria","virus","photosynthesis","chromosome",
        "ecosystem","species","metabolism","antibody","tissue","rna","physiology",
        "genome","receptor","hormone","allele"}},
    {Subject::Engineering, {"circuit","torque","stress","beam","cad","tolerance","bearing",
        "hydraulic","actuator","load bearing","weld","gear","pcb","transistor","chassis",
        "structural","machining","alloy","fastener","turbine","thermal design","cnc",
        "schematic","manufacturing"}},
    {Subject::Philosophy, {"ethics","epistemology","metaphysics","ontology","kant","stoic",
        "morality","consciousness","free will","utilitarian","existential","dialectic",
        "nietzsche","aristotle","virtue","phenomenology","determinism","socratic",
        "meaning of life","normative","a priori","solipsism","teleology","nihilism"}},
    {Subject::Sociology, {"society","culture","class","institution","survey","demographic",
        "inequality","norms","capitalism","policy","election","market","psychology",
        "behaviour","behavior","community","migration","ethnography","bureaucracy",
        "socialization","urbanization","gdp","labor","kinship"}},
    {Subject::Language, {"grammar","translate","essay","sentence","rhetoric","poem",
        "metaphor","etymology","syntax rules","proofread","paragraph","literature",
        "novel","tone","phonetic","vocabulary","idiom","narrative","rewrite","spelling",
        "linguistic","prose","dialogue","summarize"}},
    // Fallback carries no keywords on purpose: it is reached by the no-match
    // path, not by competing for scores. The entry still has to exist -- this
    // array is sized by kSubjectCount, and a missing one would be
    // value-initialised with subject 0, whose score it would then clobber.
    {Subject::Fallback, {}},
}};

// Every subject must appear exactly once, at its own index. Getting this wrong
// is silent: a missing entry is value-initialised to subject 0 and wipes that
// subject's score.
static_assert(kKeywords.size() == kSubjectCount, "one keyword set per subject");

constexpr bool keywords_are_index_aligned() {
    for (std::size_t i = 0; i < kKeywords.size(); ++i) {
        if (static_cast<std::size_t>(kKeywords[i].subject) != i) {
            return false;
        }
    }
    return true;
}
static_assert(keywords_are_index_aligned(), "keyword sets must be in Subject order");

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_word_char(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (std::isalnum(byte) != 0) || c == '+' || c == '#' || c == '_';
}

/// Count whole-word occurrences of `needle`. Substring matching would let "ion"
/// fire inside "function", which is how naive keyword routers end up sending
/// programming questions to chemistry.
int count_occurrences(const std::string& haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    int found = 0;
    std::size_t pos = haystack.find(needle);
    while (pos != std::string::npos) {
        const bool left_ok  = pos == 0 || !is_word_char(haystack[pos - 1]);
        const std::size_t after = pos + needle.size();
        const bool right_ok = after >= haystack.size() || !is_word_char(haystack[after]);
        if (left_ok && right_ok) {
            ++found;
        }
        pos = haystack.find(needle, pos + 1);
    }
    return found;
}

}  // namespace

std::string_view route_source_name(RouteSource source) {
    switch (source) {
        case RouteSource::Model:    return "router model";
        case RouteSource::Keyword:  return "keywords";
        case RouteSource::Forced:   return "pinned";
        case RouteSource::Fallback: break;
    }
    return "fallback";
}

RouteSource route_source_from_name(std::string_view name) {
    if (name == "router model") { return RouteSource::Model; }
    if (name == "keywords")     { return RouteSource::Keyword; }
    if (name == "pinned")       { return RouteSource::Forced; }
    return RouteSource::Fallback;
}

std::string answer_prefix(std::string_view rendered) {
    // Harmony, as llama.cpp's built-in formatter writes it: the assistant turn
    // is opened and left there. The channel has to be named before a message
    // can begin, and `final` is the channel an answer goes on.
    constexpr std::string_view kHarmonyOpen = "<|start|>assistant";
    if (rendered.size() >= kHarmonyOpen.size() &&
        rendered.compare(rendered.size() - kHarmonyOpen.size(), kHarmonyOpen.size(),
                         kHarmonyOpen) == 0) {
        return "<|channel|>final<|message|>";
    }
    return {};
}

// ---------------------------------------------------------------------------
// KeywordRouter
// ---------------------------------------------------------------------------

RouteDecision KeywordRouter::route(const std::string& prompt, const CancelCallback& /*cancel*/) {
    const std::string haystack = to_lower(prompt);

    std::array<int, kSubjectCount> scores{};
    int total = 0;
    for (const KeywordSet& set : kKeywords) {
        int score = 0;
        for (const std::string_view word : set.words) {
            if (word.empty()) {
                continue;
            }
            score += count_occurrences(haystack, word);
        }
        scores[static_cast<std::size_t>(set.subject)] = score;
        total += score;
    }

    const auto best = std::max_element(scores.begin(), scores.end());
    const int best_score = *best;

    RouteDecision decision;
    if (best_score == 0) {
        // Nothing matched, so there is no decision to report. Fallback is the
        // seat for exactly this, and the engine will substitute if it is empty.
        decision.subject    = Subject::Fallback;
        decision.confidence = 0.0F;
        decision.source     = RouteSource::Fallback;
        decision.detail     = "no subject keywords matched";
        return decision;
    }

    decision.subject = static_cast<Subject>(std::distance(scores.begin(), best));
    decision.source  = RouteSource::Keyword;
    // Confidence is this subject's share of all matches, so a prompt that hits
    // three fields at once reports the genuine ambiguity instead of false
    // certainty. Capped below 1.0 -- keywords are never proof.
    decision.confidence = total > 0
        ? std::min(0.95F, static_cast<float>(best_score) / static_cast<float>(total))
        : 0.30F;
    decision.detail = std::to_string(best_score) + " keyword match"
                    + (best_score == 1 ? "" : "es");
    return decision;
}

// ---------------------------------------------------------------------------
// ModelRouter
// ---------------------------------------------------------------------------

ModelRouter::ModelRouter(LoadedModel& model, ModelParams params,
                         std::string system_prompt_override)
    : model_(model),
      params_(std::move(params)),
      system_prompt_(system_prompt_override.empty() ? router_system_prompt()
                                                    : std::move(system_prompt_override)),
      examples_(router_examples()) {
    subjects_ = routable_subjects();
    labels_   = router_labels();
}

std::string ModelRouter::conversation(const std::string& question) const {
    // The worked examples go in as real user/assistant turns. Pasting the same
    // examples into the system prompt instead measured 42% against 74% on the
    // 1.2B delegator, so the shape of the conversation matters more here than
    // the wording does.
    std::vector<ChatMessage> messages;
    messages.reserve(examples_.size() * 2 + 2);
    messages.push_back({"system", system_prompt_});
    for (const auto& [example, answer] : examples_) {
        messages.push_back({"user", example});
        messages.push_back({"assistant", answer});
    }
    messages.push_back({"user", question});

    // The label is scored as the very next token, so the prompt has to end
    // where the answer would begin. See answer_prefix.
    std::string rendered = model_.format_chat(messages, true);
    rendered += answer_prefix(rendered);
    return rendered;
}

void ModelRouter::calibrate() {
    calibrated_ = true;
    bias_.assign(labels_.size(), 0.0F);

    // Content-free questions: whatever the delegator answers to these is not
    // about the question, because there is no question. Several of them, so the
    // measurement is of the model's prior rather than of one odd string.
    const std::array<const char*, 3> nothing{{"N/A", "", "..."}};

    int measured = 0;
    for (const char* question : nothing) {
        const std::vector<float> scores =
            model_.score_labels(conversation(question), labels_, {});
        if (scores.size() != bias_.size() || scores.front() <= kUnscored) {
            continue;
        }
        for (std::size_t i = 0; i < scores.size(); ++i) {
            bias_[i] += scores[i];
        }
        ++measured;
    }
    if (measured == 0) {
        bias_.assign(labels_.size(), 0.0F);  // uncalibrated is better than wrong
        return;
    }
    for (float& value : bias_) {
        value /= static_cast<float>(measured);
    }
}

std::vector<float> ModelRouter::raw_scores(const std::string& prompt) {
    return model_.score_labels(conversation(prompt), labels_, {});
}

RouteDecision ModelRouter::route(const std::string& prompt, const CancelCallback& cancel) {
    if (!calibrated_) {
        calibrate();
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<float> scores = model_.score_labels(conversation(prompt), labels_, cancel);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (cancel && cancel()) {
        RouteDecision decision;
        decision.source = RouteSource::Fallback;
        decision.detail = "routing cancelled";
        return decision;
    }
    if (scores.size() != labels_.size() || scores.front() <= kUnscored) {
        // The model could not be scored at all -- a decode failure, or a
        // context too small for the prompt. Keywords still work.
        RouteDecision decision = fallback_.route(prompt, cancel);
        decision.detail = "delegator could not be scored, used keywords";
        return decision;
    }

    // Take out what the delegator would have said to no question at all, then
    // read the rest as a distribution. See ModelRouter::bias_.
    for (std::size_t i = 0; i < scores.size(); ++i) {
        scores[i] -= calibration_ * bias_[i];
    }

    const auto best = static_cast<std::size_t>(
        std::distance(scores.begin(), std::max_element(scores.begin(), scores.end())));

    const float highest = scores[best];
    float       sum     = 0.0F;
    for (const float score : scores) {
        sum += std::exp(score - highest);
    }

    RouteDecision decision;
    decision.subject    = subjects_[best];
    decision.confidence = sum > 0.0F ? std::clamp(1.0F / sum, 0.0F, 1.0F) : 0.0F;
    decision.source     = RouteSource::Model;
    decision.detail     = std::to_string(elapsed.count()) + "ms";
    return decision;
}

}  // namespace crucible
