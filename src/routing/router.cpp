// SPDX-License-Identifier: MIT
//
// The two routers.
//
// KeywordRouter needs no model at all and keeps BatBot usable with nothing
// installed. ModelRouter runs the delegator under a GBNF grammar, so its answer
// is structurally incapable of naming a subject that does not exist.
//
// The keyword table below matches whole words only. Substring matching sounds
// harmless until "ion" fires inside "function" and sends every programming
// question to Chemistry.
#include "batbot/routing/router.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace batbot {
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
      grammar_(router_grammar()),
      system_prompt_(system_prompt_override.empty() ? router_system_prompt()
                                                    : std::move(system_prompt_override)),
      examples_(router_examples()) {
    // The router emits "TAG 0.87" and nothing else, so a handful of tokens is
    // plenty; a larger budget would only buy latency on every single prompt.
    params_.max_tokens = 16;
}

RouteDecision ModelRouter::route(const std::string& prompt, const CancelCallback& cancel) {
    // The worked examples go in as real user/assistant turns. Pasting the same
    // examples into the system prompt instead measured 42% against 74% on the
    // 1.2B delegator, so the shape of the conversation matters more here than
    // the wording does.
    std::vector<ChatMessage> messages;
    messages.reserve(examples_.size() * 2 + 2);
    messages.push_back({"system", system_prompt_});
    for (const auto& [question, answer] : examples_) {
        messages.push_back({"user", question});
        messages.push_back({"assistant", answer});
    }
    messages.push_back({"user", prompt});

    std::string output;
    const GenerationStats stats = model_.generate(
        model_.format_chat(messages, true), params_,
        [&output](std::string_view chunk) { output += chunk; },
        cancel, grammar_);

    if (stats.cancelled) {
        RouteDecision decision;
        decision.source = RouteSource::Fallback;
        decision.detail = "routing cancelled";
        return decision;
    }

    // The grammar guarantees the shape, but a model can still be stopped early
    // or a future grammar change could loosen it, so parse defensively and fall
    // back to keywords rather than trusting a half-formed answer.
    std::istringstream stream(output);
    std::string tag;
    float confidence = 0.0F;
    if (stream >> tag) {
        if (const std::optional<Subject> subject = subject_from_string(tag)) {
            if (!(stream >> confidence)) {
                confidence = 0.5F;
            }
            RouteDecision decision;
            decision.subject    = *subject;
            decision.confidence = std::clamp(confidence, 0.0F, 1.0F);
            decision.source     = RouteSource::Model;
            decision.detail     = std::to_string(static_cast<int>(stats.prompt_ms
                                                                + stats.output_ms)) + "ms";
            return decision;
        }
    }

    RouteDecision decision = fallback_.route(prompt, cancel);
    decision.detail = "router output unparseable (\"" + output + "\"), used keywords";
    return decision;
}

}  // namespace batbot
