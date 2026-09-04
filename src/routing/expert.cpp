// SPDX-License-Identifier: MIT
//
// The roster, and everything generated from it.
//
// The list is the single source of truth. The labels the delegator chooses
// between, its system prompt, its worked examples, the keyword sets the
// model-free router scores with, and the order seats appear at the roundtable
// are all derived from it, so they cannot drift apart -- adding a seat adds it
// everywhere, whether the seat came from the defaults below or from a user
// typing `/newexpert` a minute ago.
#include "crucible/routing/expert.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace crucible {
namespace {

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view text) {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

/// Words that appear in every description ever written and therefore separate
/// nothing. The keyword router scores by count, so one of these in a seat's
/// list would win every prompt containing the word "with".
const std::set<std::string>& stop_words() {
    static const std::set<std::string> kStop{
        "about", "above", "after", "against", "along", "also", "among", "and", "another",
        "any", "anything", "are", "around", "because", "been", "before", "being", "below",
        "beside", "besides", "between", "both", "build", "building", "can", "come",
        "could", "deal", "deals", "does", "doing", "done", "down", "during", "each",
        "either", "else", "etc", "even", "ever", "every", "everything", "few", "find",
        "first", "for", "from", "general", "generally", "get", "give", "goes", "going",
        "good", "handle", "handles", "has", "have", "having", "help", "here", "how",
        "however", "into", "its", "just", "kind", "kinds", "know", "knowledge", "last",
        "like", "lot", "made", "make", "makes", "making", "many", "may", "might", "more",
        "most", "much", "must", "need", "needs", "new", "not", "now", "off", "often",
        "one", "only", "onto", "other", "others", "out", "over", "own", "part", "parts",
        "per", "put", "questions", "related", "same", "see", "seen", "set", "should",
        "similar", "since", "some", "something", "sort", "specific", "still", "stuff",
        "such", "take", "takes", "than", "that", "the", "their", "them", "then", "there",
        "these", "they", "thing", "things", "this", "those", "though", "through", "thus",
        "topic", "topics", "toward", "under", "until", "upon", "use", "used", "uses",
        "using", "very", "want", "was", "well", "were", "what", "when", "where", "which",
        "while", "who", "whose", "why", "will", "with", "within", "without", "work",
        "working", "works", "would", "you", "your"};
    return kStop;
}

/// The nine that ship.
///
/// Their blurbs, examples and keyword sets are measured rather than guessed:
/// this exact table is what scores 96% on the 54-prompt routing benchmark with
/// LFM2.5-1.2B. Two examples each, and the same number for every seat so none
/// is favoured by having more.
///
/// Neither example may resemble anything in tools/routebench.cpp, or the
/// benchmark is measuring how well the prompt was copied into it.
Expert builtin(const char* id, const char* name, const char* tag, const char* blurb,
               std::vector<std::string> examples, std::vector<std::string> keywords) {
    Expert expert;
    expert.id       = id;
    expert.name     = name;
    expert.tag      = tag;
    expert.blurb    = blurb;
    expert.examples = std::move(examples);
    expert.keywords = std::move(keywords);
    expert.builtin  = true;
    expert.routable = true;
    return expert;
}

Expert fallback_seat() {
    Expert seat;
    seat.id   = std::string(kFallbackId);
    seat.name = "Fallback";
    seat.tag  = "FALL";
    // Never offered to the delegator, so this text is for the settings screen
    // only -- it is not part of the routing prompt.
    seat.blurb =
        "takes prompts the delegator could not place, and prompts routed to a seat "
        "with no model";
    seat.builtin  = true;
    seat.routable = false;
    // No keywords on purpose: the fallback is reached by the no-match path, not
    // by competing for scores.
    return seat;
}

}  // namespace

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

Roster Roster::defaults() {
    Roster roster;
    roster.experts_ = {
        builtin("mathematics", "Mathematics", "MATH",
            "mathematics, algebra, calculus, proofs, geometry, statistics, probability, number theory",
            {"what is the integral of x squared",
             "what is the probability of rolling two sixes in a row"},
            {"integral","derivative","theorem","proof","matrix","algebra","calculus",
             "equation","polynomial","topology","geometry","probability","modulo",
             "eigenvalue","factorial","logarithm","prime","vector space","summation",
             "limit","differential","combinatorics","cardinality","isomorphism"}),

        builtin("programming", "Programming", "PROG",
            "programming, code, software, algorithms, data structures, debugging, systems, a codebase",
            {"my python script throws a KeyError",
             "my docker container exits the moment it starts"},
            {"code","function","compile","debug","refactor","python","c++","rust",
             "javascript","algorithm","segfault","repository","git","api","pointer",
             "recursion","runtime","syntax","framework","typescript","kernel","binary",
             "linked list","stack trace"}),

        builtin("physics", "Physics", "PHYS",
            "physics, forces, energy, light, thermodynamics, relativity, quantum, astronomy",
            {"why do heavy and light objects fall together",
             "why does a helium balloon rise"},
            {"quantum","relativity","momentum","thermodynamics","entropy","electromagnetic",
             "photon","lagrangian","hamiltonian","velocity","acceleration","gravity",
             "particle","wavelength","voltage","kinetic","newton","tensor field",
             "spacetime","fermion","boson","optics","friction","orbital mechanics"}),

        builtin("chemistry", "Chemistry", "CHEM",
            "chemistry, reactions, molecules, bonding, acids, pH, materials, the laboratory",
            {"what happens when sodium touches water", "why does salt melt ice"},
            {"molecule","reaction","atom","bond","stoichiometry","titration","catalyst",
             "organic","ion","ph","enthalpy","reagent","solvent","isotope",
             "periodic table","valence","oxidation","polymer","acid","alkane","molarity",
             "chromatography","electrolysis","compound"}),

        builtin("biology", "Biology", "BIO",
            "biology, cells, DNA, genetics, physiology, medicine, ecology, evolution",
            {"how do vaccines train the immune system",
             "how do muscles get oxygen during exercise"},
            {"cell","dna","protein","enzyme","gene","evolution","organism","mitochondria",
             "neuron","bacteria","virus","photosynthesis","chromosome","ecosystem",
             "species","metabolism","antibody","tissue","rna","physiology","genome",
             "receptor","hormone","allele"}),

        builtin("engineering", "Engineering", "ENG",
            "engineering, designing or building a physical thing, mechanical electrical and civil design, bolts, beams, loads, circuits, wiring, tolerances, materials, hardware, CAD",
            {"what preload should this bolted joint have",
             "how do I stop this bracket from vibrating"},
            {"circuit","torque","stress","beam","cad","tolerance","bearing","hydraulic",
             "actuator","load bearing","weld","gear","pcb","transistor","chassis",
             "structural","machining","alloy","fastener","turbine","thermal design","cnc",
             "schematic","manufacturing"}),

        builtin("philosophy", "Philosophy", "PHIL",
            "philosophy, ethics, right and wrong, logic, metaphysics, epistemology, free will, consciousness, knowledge, existence, meaning",
            {"can someone be blamed for an unavoidable act",
             "can a machine ever be said to understand anything"},
            {"ethics","epistemology","metaphysics","ontology","kant","stoic","morality",
             "consciousness","free will","utilitarian","existential","dialectic",
             "nietzsche","aristotle","virtue","phenomenology","determinism","socratic",
             "meaning of life","normative","a priori","solipsism","teleology","nihilism"}),

        builtin("sociology", "Sociology", "SOC",
            "society, economics, politics, history, psychology, culture, institutions, law, education, inequality, cities, populations, why people or groups behave as they do",
            {"why did rents rise faster than wages",
             "what makes a protest movement succeed"},
            {"society","culture","class","institution","survey","demographic","inequality",
             "norms","capitalism","policy","election","market","psychology","behaviour",
             "behavior","community","migration","ethnography","bureaucracy",
             "socialization","urbanization","gdp","labor","kinship"}),

        builtin("language", "Language", "LANG",
            "writing, grammar, spelling, punctuation, a sentence, a paragraph, an essay, proofreading, editing, rewriting, tone, style, summarising, translation, a word or its meaning, literature",
            {"what is the difference between affect and effect",
             "what is the plural of octopus"},
            {"grammar","translate","essay","sentence","rhetoric","poem","metaphor",
             "etymology","syntax rules","proofread","paragraph","literature","novel",
             "tone","phonetic","vocabulary","idiom","narrative","rewrite","spelling",
             "linguistic","prose","dialogue","summarize"}),

        fallback_seat(),
    };
    return roster;
}

Roster Roster::bare() {
    Roster roster;
    roster.experts_ = {fallback_seat()};
    return roster;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

const Expert& Roster::at(std::size_t index) const {
    // Out of range resolves to the fallback rather than to whatever is at zero.
    // An index that has gone stale -- a seat ejected while a turn was in flight
    // -- then reads as "nobody in particular", which is true, instead of
    // silently attributing the work to the first expert in the list.
    if (index < experts_.size()) {
        return experts_[index];
    }
    return experts_[fallback_index()];
}

std::optional<std::size_t> Roster::find(std::string_view key) const {
    const std::string needle = to_lower(trim(key));
    if (needle.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < experts_.size(); ++i) {
        const Expert& expert = experts_[i];
        if (needle == to_lower(expert.id) || needle == to_lower(expert.tag) ||
            needle == to_lower(expert.name)) {
            return i;
        }
    }
    return std::nullopt;
}

std::size_t Roster::fallback_index() const {
    for (std::size_t i = 0; i < experts_.size(); ++i) {
        if (!experts_[i].routable) {
            return i;
        }
    }
    // Unreachable while every constructor plants a fallback and `remove`
    // refuses to take it away. Answering with the last index keeps `at()` in
    // bounds if that ever stops being true.
    return experts_.empty() ? 0 : experts_.size() - 1;
}

std::vector<std::size_t> Roster::routable() const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < experts_.size(); ++i) {
        if (experts_[i].routable) {
            out.push_back(i);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

void Roster::reorder() {
    // The fallback goes last, wherever it was. It is not one of the specialists
    // and the bottom of the list is where the thing that catches what the
    // others did not belongs -- the roundtable draws the list in this order.
    const auto is_fallback = [](const Expert& e) { return !e.routable; };
    std::stable_partition(experts_.begin(), experts_.end(),
                          [&](const Expert& e) { return !is_fallback(e); });
}

bool Roster::add(Expert expert, std::string& error) {
    expert.name = trim(expert.name);
    if (expert.name.empty()) {
        error = "an expert needs a name";
        return false;
    }
    if (expert.id.empty()) {
        expert.id = make_expert_id(expert.name);
    }
    if (expert.id.empty()) {
        error = "\"" + expert.name + "\" has no letters or digits in it to make a name from";
        return false;
    }
    if (expert.id == kFallbackId) {
        error = "\"fallback\" is the seat that catches everything else and already exists";
        return false;
    }
    if (find(expert.id) || find(expert.name)) {
        error = "there is already an expert called " + expert.name;
        return false;
    }

    expert.blurb = trim(expert.blurb);
    if (expert.blurb.empty()) {
        error = "describe what " + expert.name + " is trained in, or the delegator "
                "has nothing to route on";
        return false;
    }

    if (expert.tag.empty()) {
        std::vector<std::string> taken;
        taken.reserve(experts_.size());
        for (const Expert& other : experts_) {
            taken.push_back(other.tag);
        }
        expert.tag = make_expert_tag(expert.name, taken);
    }
    if (expert.keywords.empty()) {
        expert.keywords = derive_keywords(expert.name, expert.blurb);
    }

    experts_.push_back(std::move(expert));
    reorder();
    return true;
}

bool Roster::remove(std::string_view key, std::string& error) {
    const std::optional<std::size_t> index = find(key);
    if (!index) {
        error = "no expert called \"" + std::string(key) + "\"";
        return false;
    }
    if (!experts_[*index].routable) {
        error = "the fallback seat cannot be ejected -- it is where prompts go when "
                "nothing else fits";
        return false;
    }
    experts_.erase(experts_.begin() + static_cast<std::ptrdiff_t>(*index));
    return true;
}

bool Roster::update(const ExpertId& id, const Expert& replacement) {
    for (Expert& expert : experts_) {
        if (expert.id == id) {
            // The id is the one field that cannot move: it is what the config
            // file and the stored sessions are keyed on.
            const ExpertId keep = expert.id;
            expert    = replacement;
            expert.id = keep;
            reorder();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// What the delegator sees
// ---------------------------------------------------------------------------

std::vector<std::string> Roster::router_labels() const {
    // The name, not the tag.
    //
    // This is worth 39 points. Scored on Crucible's 54-prompt benchmark with
    // LFM2.5-1.2B, answering with the four-letter tags gets 48% and answering
    // with the names gets 87%. The tags are not words: a delegator choosing
    // between "PHIL" and "PHYS" is comparing two spellings that share a first
    // token, and every philosophy question in the set went to physics.
    // "Philosophy" and "Physics" are things the model knows about.
    //
    // The tags stay in the system prompt, where they measurably help (dropping
    // them costs 6 points), and on the roundtable, where a fixed width is what
    // makes the chips line up.
    std::vector<std::string> labels;
    for (const Expert& expert : experts_) {
        if (expert.routable) {
            labels.push_back(expert.name);
        }
    }
    return labels;
}

std::string Roster::router_system_prompt() const {
    // Routable seats only, and deliberately compact. An earlier version
    // described each subject in prose and closed by naming a catch-all; small
    // models latched onto that closing line and answered it for almost
    // everything (16% accurate). The delegator's job is to pick a specialist,
    // so it is never shown a way to decline.
    std::string prompt =
        "You label a question with the one subject it belongs to.\n"
        "Reply with only a tag and a confidence.\n\n";
    for (const Expert& expert : experts_) {
        if (!expert.routable) {
            continue;  // the delegator is never offered the fallback seat
        }
        // Tag, name, then the remit. The tag earns its place here even though
        // the delegator answers with the name: on the 54-prompt benchmark,
        // listing the options without their tags costs 6 points (87% to 81%).
        // Reading it as a labelled menu appears to be what helps.
        prompt += expert.tag;
        prompt += "  ";
        prompt += expert.name;
        prompt += ": ";
        prompt += expert.blurb;
        prompt += "\n";
    }
    return prompt;
}

std::vector<std::pair<std::string, std::string>> Roster::router_examples() const {
    std::vector<std::pair<std::string, std::string>> examples;
    for (const Expert& expert : experts_) {
        if (!expert.routable) {
            continue;
        }
        for (const std::string& question : expert.examples) {
            if (question.empty()) {
                continue;
            }
            // The answer is exactly one of router_labels() and nothing else.
            // The delegator does not write a confidence -- that comes from
            // comparing the labels against each other -- and an example that
            // showed one would teach it to continue past the string being
            // scored.
            examples.emplace_back(question, expert.name);
        }
    }
    return examples;
}

// ---------------------------------------------------------------------------
// Deriving the fields a user is not asked for
// ---------------------------------------------------------------------------

std::string expert_label(const Roster& roster, const ExpertId& id) {
    if (const std::optional<std::size_t> index = roster.find(id)) {
        return roster.at(*index).name;
    }
    return id;
}

std::string make_expert_id(std::string_view name) {
    std::string id;
    bool pending_hyphen = false;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0) {
            if (pending_hyphen && !id.empty()) {
                id += '-';
            }
            pending_hyphen = false;
            id += static_cast<char>(std::tolower(byte));
        } else {
            pending_hyphen = true;
        }
    }
    return id;
}

std::string make_expert_tag(std::string_view name, const std::vector<std::string>& taken) {
    // Initials for a multi-word name, so "Rust Async" is RA rather than RUST
    // and cannot collide with a future "Rust" seat on its first four letters.
    std::string initials;
    bool at_start = true;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0) {
            if (at_start && initials.size() < 4) {
                initials += static_cast<char>(std::toupper(byte));
            }
            at_start = false;
        } else {
            at_start = true;
        }
    }

    std::string tag = initials.size() >= 2 ? initials : std::string();
    if (tag.empty()) {
        for (const char c : name) {
            const auto byte = static_cast<unsigned char>(c);
            if ((std::isalnum(byte) != 0) && tag.size() < 4) {
                tag += static_cast<char>(std::toupper(byte));
            }
        }
    }
    if (tag.empty()) {
        tag = "EXPT";
    }

    const auto is_taken = [&taken](const std::string& candidate) {
        return std::any_of(taken.begin(), taken.end(), [&](const std::string& other) {
            return to_lower(other) == to_lower(candidate);
        });
    };
    if (!is_taken(tag)) {
        return tag;
    }
    // Two seats sharing a chip is a bug you only notice once the wrong one
    // lights up, so a collision is broken rather than tolerated.
    for (int suffix = 2; suffix <= 9; ++suffix) {
        std::string candidate = tag.substr(0, 3) + std::to_string(suffix);
        if (!is_taken(candidate)) {
            return candidate;
        }
    }
    return tag;
}

std::vector<std::string> derive_keywords(std::string_view name, std::string_view blurb) {
    // Whole words of four characters or more that are not stop words. The
    // keyword router matches whole words and scores by count, so a short or
    // common word costs far more in false positives than it earns in recall --
    // this is the same principle the shipped keyword sets were curated on.
    std::vector<std::string> words;
    std::set<std::string>    seen;

    const auto harvest = [&](std::string_view text) {
        std::string current;
        const auto flush = [&]() {
            if (current.size() >= 4 && stop_words().count(current) == 0 &&
                seen.insert(current).second) {
                words.push_back(current);
            }
            current.clear();
        };
        for (const char c : text) {
            const auto byte = static_cast<unsigned char>(c);
            if ((std::isalnum(byte) != 0) || c == '+' || c == '#') {
                current += static_cast<char>(std::tolower(byte));
            } else {
                flush();
            }
        }
        flush();
    };

    harvest(name);
    harvest(blurb);
    return words;
}

std::string example_request_prompt(std::string_view name, std::string_view blurb) {
    // Asks for bare questions and nothing else. Every constraint here exists
    // because a model broke it in testing: they number lists, they explain
    // first, they answer their own question, and they drift towards the general
    // ("what is chemistry") when not told to be specific.
    std::string prompt = "An expert called \"";
    prompt += name;
    prompt += "\" handles: ";
    prompt += blurb;
    prompt +=
        "\n\nWrite exactly two short questions a person would ask that this expert "
        "should obviously answer.\n"
        "Rules: one question per line. No numbering, no bullets, no quotes, no "
        "explanation. Each question must be specific enough that it could not be "
        "asked of a different expert.\n";
    return prompt;
}

std::vector<std::string> parse_examples(std::string_view reply, std::size_t wanted) {
    std::vector<std::string> questions;
    std::istringstream       stream{std::string(reply)};
    std::string              line;

    while (std::getline(stream, line) && questions.size() < wanted) {
        std::string text = trim(line);
        if (text.empty()) {
            continue;
        }

        // Strip the decoration models add whatever they were told: "1. ",
        // "- ", "* ", "Q: ", and surrounding quotes.
        std::size_t start = 0;
        while (start < text.size() &&
               ((std::isdigit(static_cast<unsigned char>(text[start])) != 0) ||
                text[start] == '.' || text[start] == ')' || text[start] == '-' ||
                text[start] == '*' || text[start] == ' ')) {
            ++start;
        }
        text = trim(std::string_view(text).substr(start));
        if (text.size() > 2 && (text.compare(0, 2, "Q:") == 0 || text.compare(0, 2, "A:") == 0)) {
            text = trim(std::string_view(text).substr(2));
        }
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            text = trim(std::string_view(text).substr(1, text.size() - 2));
        }

        // A line of preamble ("Here are two questions:") ends in a colon and is
        // not a question; a real one is long enough to be worth showing a
        // model. Both filters are cheap and both fire in practice.
        if (text.size() < 12 || text.back() == ':') {
            continue;
        }
        questions.push_back(std::move(text));
    }
    return questions;
}

}  // namespace crucible
