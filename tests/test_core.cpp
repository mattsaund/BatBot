// SPDX-License-Identifier: MIT
// Tests for the parts of BatBot that need no model loaded: the subject table,
// the router grammar, keyword routing, config inheritance, the trust store,
// path expansion, and UTF-8 chunking.

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "batbot/config/config.hpp"
#include "batbot/config/gpu_policy.hpp"
#include "batbot/engine/route_policy.hpp"
#include "batbot/llm/model_catalog.hpp"
#include "batbot/config/paths.hpp"
#include "batbot/routing/router.hpp"
#include "batbot/engine/state.hpp"
#include "batbot/routing/subject.hpp"
#include "batbot/runtime/backend.hpp"
#include "batbot/runtime/devices.hpp"
#include "batbot/runtime/registry.hpp"
#include "batbot/session/store.hpp"
#include "batbot/session/usage.hpp"
#include "batbot/util/subprocess.hpp"
#include "batbot/util/text.hpp"
#include "batbot/config/trust.hpp"
#include "harness.hpp"

using namespace batbot;

namespace {

/// A directory that cleans itself up, so tests never leave files behind.
/// Point the XDG data directory at a temporary place, so a test that reads or
/// writes a real BatBot directory cannot touch the one belonging to whoever is
/// running the suite.
class ScopedDataHome {
public:
    explicit ScopedDataHome(const std::filesystem::path& dir) {
        if (const char* existing = std::getenv("XDG_DATA_HOME"); existing != nullptr) {
            previous_ = existing;
            had_      = true;
        }
        ::setenv("XDG_DATA_HOME", dir.c_str(), 1);
    }
    ~ScopedDataHome() {
        if (had_) {
            ::setenv("XDG_DATA_HOME", previous_.c_str(), 1);
        } else {
            ::unsetenv("XDG_DATA_HOME");
        }
    }
    ScopedDataHome(const ScopedDataHome&)            = delete;
    ScopedDataHome& operator=(const ScopedDataHome&) = delete;

private:
    std::string previous_;
    bool        had_ = false;
};

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path()
              / ("batbot-test-" + std::to_string(::getpid()) + "-"
                 + std::to_string(counter()++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    static int& counter() { static int n = 0; return n; }
    std::filesystem::path path_;
};

Subject route_of(const std::string& prompt) {
    KeywordRouter router;
    return router.route(prompt, {}).subject;
}

}  // namespace

// ---------------------------------------------------------------------------
// Subjects
// ---------------------------------------------------------------------------

TEST(subject_table_is_complete_and_unique) {
    CHECK_EQ(all_subjects().size(), kSubjectCount);

    for (std::size_t i = 0; i < kSubjectCount; ++i) {
        const SubjectInfo& info = all_subjects()[i];
        // The table must stay index-aligned with the enum: the roundtable and
        // the config both index into it by Subject.
        CHECK_EQ(static_cast<std::size_t>(info.subject), i);
        CHECK(!info.id.empty());
        CHECK(!info.name.empty());
        CHECK(!info.blurb.empty());
        // Tags are padded to a fixed width for the roundtable chips.
        CHECK_EQ(info.tag.size(), std::size_t{4});
    }
}

TEST(subject_parsing_accepts_ids_tags_and_case) {
    CHECK(subject_from_string("physics")    == Subject::Physics);
    CHECK(subject_from_string("PHYSICS")    == Subject::Physics);
    CHECK(subject_from_string("PhYsIcS")    == Subject::Physics);
    CHECK(subject_from_string("PHYS")       == Subject::Physics);
    CHECK(subject_from_string("phys")       == Subject::Physics);

    // Tags carry padding ("BIO "), which must not defeat a lookup either way.
    CHECK(subject_from_string("BIO")        == Subject::Biology);
    CHECK(subject_from_string("BIO ")       == Subject::Biology);
    CHECK(subject_from_string("  biology ") == Subject::Biology);

    CHECK(!subject_from_string("astrology").has_value());
    CHECK(!subject_from_string("").has_value());
    CHECK(!subject_from_string("   ").has_value());
}

TEST(every_subject_round_trips_through_its_own_strings) {
    for (const SubjectInfo& info : all_subjects()) {
        CHECK(subject_from_string(info.id)  == info.subject);
        CHECK(subject_from_string(info.tag) == info.subject);
    }
}

// ---------------------------------------------------------------------------
// Router grammar
// ---------------------------------------------------------------------------

TEST(router_grammar_names_every_subject) {
    const std::string grammar = router_grammar();

    CHECK(grammar.find("root ::=") != std::string::npos);
    CHECK(grammar.find("subject ::=") != std::string::npos);
    CHECK(grammar.find("confidence ::=") != std::string::npos);

    // If a routable subject were missing from the grammar the delegator could
    // never choose it, and the seat would be silently unreachable.
    for (const Subject subject : routable_subjects()) {
        std::string tag(subject_tag(subject));
        while (!tag.empty() && tag.back() == ' ') {
            tag.pop_back();
        }
        CHECK(grammar.find("\"" + tag + "\"") != std::string::npos);
    }
}

TEST(router_system_prompt_describes_every_expert) {
    const std::string prompt = router_system_prompt();
    for (const Subject subject : routable_subjects()) {
        const SubjectInfo& info = subject_info(subject);
        CHECK(prompt.find(std::string(info.name)) != std::string::npos);
        CHECK(prompt.find(std::string(info.blurb)) != std::string::npos);
    }

    // The prompt must not mention the fallback seat at all. Checking only that
    // subjects are present would miss this -- and did: the guard was dropped in
    // an edit and the grammar and examples stayed correct while the prompt
    // silently offered a tenth option the sampler could never emit.
    const SubjectInfo& fallback = subject_info(Subject::Fallback);
    CHECK(prompt.find(std::string(fallback.name))  == std::string::npos);
    CHECK(prompt.find(std::string(fallback.blurb)) == std::string::npos);
    CHECK(prompt.find("FALL")                      == std::string::npos);
    // An earlier prompt closed by naming Language as the catch-all, and small
    // models answered LANG to nearly everything as a result (16% accurate
    // against 63% now). Keep that phrasing out.
    CHECK(prompt.find("fits no other") == std::string::npos);
}

TEST(fallback_is_the_tenth_seat_and_is_not_routable) {
    CHECK_EQ(kSubjectCount, std::size_t{10});
    CHECK_EQ(static_cast<std::size_t>(Subject::Fallback), std::size_t{9});

    const SubjectInfo& fallback = subject_info(Subject::Fallback);
    CHECK_EQ(fallback.id,   std::string_view("fallback"));
    CHECK_EQ(fallback.name, std::string_view("Fallback"));

    // The delegator's job is to pick a specialist. Offering it an "anything
    // else" option is what collapsed routing to 16% once already.
    CHECK(!fallback.routable);
    CHECK_EQ(routable_subjects().size(), kSubjectCount - 1);

    CHECK(subject_from_string("fallback") == Subject::Fallback);
    CHECK(subject_from_string("FALL")     == Subject::Fallback);
}

TEST(the_grammar_cannot_name_the_fallback_seat) {
    const std::string grammar = router_grammar();
    // Unrepresentable, not merely unlikely: the sampler cannot emit it.
    CHECK(grammar.find("\"FALL\"") == std::string::npos);

    // The subject rule must list exactly the routable subjects, no more. Count
    // only within that line: the root and confidence rules carry quoted
    // literals of their own (" ", "0.", "1.00").
    const std::size_t rule_at = grammar.find("subject ::=");
    CHECK(rule_at != std::string::npos);
    const std::string rule = grammar.substr(rule_at, grammar.find('\n', rule_at) - rule_at);

    std::size_t quoted = 0;
    for (std::size_t at = rule.find('"'); at != std::string::npos;
         at = rule.find('"', at + 1)) {
        ++quoted;
    }
    CHECK_EQ(quoted / 2, routable_subjects().size());

    // And every routable tag is actually one of them.
    for (const Subject subject : routable_subjects()) {
        std::string tag(subject_tag(subject));
        while (!tag.empty() && tag.back() == ' ') {
            tag.pop_back();
        }
        CHECK(rule.find('"' + tag + '"') != std::string::npos);
    }
}

TEST(router_examples_demonstrate_every_subject_once) {
    const auto examples = router_examples();
    CHECK_EQ(examples.size(), routable_subjects().size());

    for (std::size_t i = 0; i < examples.size(); ++i) {
        const auto& [question, answer] = examples[i];
        CHECK(!question.empty());
        // The answer must be exactly what the grammar permits, or the examples
        // would demonstrate a format the model is then forbidden to produce.
        const std::string tag = answer.substr(0, answer.find(' '));
        const auto subject = subject_from_string(tag);
        CHECK(subject.has_value());
        if (subject) {
            CHECK(static_cast<std::size_t>(*subject) == i);
        }
        CHECK(answer.find(' ') != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Keyword router
// ---------------------------------------------------------------------------

TEST(keyword_router_picks_the_obvious_subject) {
    CHECK(route_of("compute the derivative of this polynomial") == Subject::Mathematics);
    CHECK(route_of("my code hits a segfault when I compile")    == Subject::Programming);
    CHECK(route_of("explain the lagrangian of this system")     == Subject::Physics);
    CHECK(route_of("balance this reaction and find the enthalpy") == Subject::Chemistry);
    CHECK(route_of("how does an enzyme change a protein")       == Subject::Biology);
    CHECK(route_of("what torque does this bearing take")        == Subject::Engineering);
    CHECK(route_of("is free will compatible with determinism")  == Subject::Philosophy);
    CHECK(route_of("how does migration reshape a community")    == Subject::Sociology);
    CHECK(route_of("proofread this paragraph for tone")         == Subject::Language);
}

TEST(keyword_router_falls_back_when_nothing_matches) {
    KeywordRouter router;
    const RouteDecision decision = router.route("hello there", {});
    // Fallback is the seat for an undecidable prompt. Reporting zero confidence
    // says "no decision" rather than inventing one.
    CHECK(decision.subject    == Subject::Fallback);
    CHECK(decision.source     == RouteSource::Fallback);
    CHECK(decision.confidence == 0.0F);
}

TEST(fallback_carries_no_keywords_so_it_never_wins_on_score) {
    // Fallback must not compete with the subjects: it is reached by the
    // no-match path. A prompt with real keywords must still go to its subject.
    CHECK(route_of("compute the derivative of this polynomial") == Subject::Mathematics);
    CHECK(route_of("balance this reaction and find the enthalpy") == Subject::Chemistry);

    // And the reverse: a prompt with nothing to match reaches Fallback.
    KeywordRouter router;
    CHECK(router.route("mmm", {}).subject == Subject::Fallback);
}

TEST(keyword_router_matches_whole_words_only) {
    // Keywords hide inside ordinary words: "ion" (Chemistry) sits in "question"
    // and "opinion", "cell" (Biology) sits in "excellent". None of them should
    // count, so this prompt matches nothing and falls through to General.
    // Substring matching would score Chemistry three times and route there.
    CHECK(route_of("an excellent question about your opinion") == Subject::Fallback);

    // "gene" (Biology) hides inside both "generate" and "general".
    CHECK(route_of("generate a general overview") == Subject::Fallback);

    // The same words as whole words must still match.
    CHECK(route_of("what is an ion")            == Subject::Chemistry);
    CHECK(route_of("describe a gene")           == Subject::Biology);
    CHECK(route_of("write a function to parse") == Subject::Programming);
}

TEST(keyword_router_confidence_reflects_ambiguity) {
    KeywordRouter router;
    const RouteDecision clear = router.route(
        "derivative integral polynomial theorem eigenvalue", {});
    const RouteDecision mixed = router.route(
        "derivative of the enzyme torque circuit", {});
    // A prompt pulling in four directions should not report the same certainty
    // as one that only ever points at maths.
    CHECK(clear.confidence > mixed.confidence);
    CHECK(clear.confidence <= 0.95F);
}

// ---------------------------------------------------------------------------
// Routing policy
//
// What happens to the delegator's answer. Extracted from the engine as a pure
// function precisely so these rules can be checked without loading a model.
// ---------------------------------------------------------------------------

namespace {

/// A config with the named seats filled, so policy tests read as a sentence.
Config config_with(std::initializer_list<Subject> filled) {
    Config config;
    for (const Subject seat : filled) {
        config.experts[static_cast<std::size_t>(seat)].model = "some-model.gguf";
    }
    return config;
}

RouteDecision proposal(Subject subject, float confidence, RouteSource source) {
    RouteDecision decision;
    decision.subject    = subject;
    decision.confidence = confidence;
    decision.source     = source;
    return decision;
}

}  // namespace

TEST(a_confident_route_to_a_filled_seat_stands) {
    const Config config = config_with({Subject::Physics, Subject::Fallback});
    const RouteDecision out =
        apply_route_policy(proposal(Subject::Physics, 0.95F, RouteSource::Model), config);
    CHECK(out.subject == Subject::Physics);
    CHECK(out.source  == RouteSource::Model);
}

TEST(an_unconfident_route_goes_to_the_fallback_seat) {
    Config config = config_with({Subject::Physics, Subject::Fallback});
    config.routing.min_confidence = 0.60F;

    const RouteDecision out =
        apply_route_policy(proposal(Subject::Physics, 0.40F, RouteSource::Model), config);
    // Below the floor the delegator is treated as having made no decision.
    CHECK(out.subject == Subject::Fallback);
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("undecided") != std::string::npos);
    CHECK(out.detail.find("Physics")   != std::string::npos);
}

TEST(a_pinned_route_ignores_the_confidence_floor) {
    Config config = config_with({Subject::Physics, Subject::Fallback});
    config.routing.min_confidence = 0.99F;

    // The user chose this expert; second-guessing them would be wrong even at
    // a confidence the model never reports.
    const RouteDecision out =
        apply_route_policy(proposal(Subject::Physics, 1.0F, RouteSource::Forced), config);
    CHECK(out.subject == Subject::Physics);
    CHECK(out.source  == RouteSource::Forced);
}

TEST(a_zero_floor_disables_the_confidence_check) {
    Config config = config_with({Subject::Physics, Subject::Fallback});
    config.routing.min_confidence = 0.0F;
    const RouteDecision out =
        apply_route_policy(proposal(Subject::Physics, 0.01F, RouteSource::Model), config);
    CHECK(out.subject == Subject::Physics);
}

TEST(an_empty_seat_sends_work_to_the_fallback) {
    const Config config = config_with({Subject::Physics, Subject::Fallback});
    const RouteDecision out =
        apply_route_policy(proposal(Subject::Chemistry, 0.95F, RouteSource::Model), config);
    CHECK(out.subject == Subject::Fallback);
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("Chemistry has no model") != std::string::npos);
}

TEST(with_no_fallback_configured_any_filled_seat_is_used) {
    // A partly-configured install should still answer rather than fail, and
    // should say plainly that it substituted.
    const Config config = config_with({Subject::Physics});
    const RouteDecision out =
        apply_route_policy(proposal(Subject::Chemistry, 0.95F, RouteSource::Model), config);
    CHECK(out.subject == Subject::Physics);
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("used Physics") != std::string::npos);
}

TEST(with_nothing_configured_the_route_reports_it) {
    const Config config;
    const RouteDecision out =
        apply_route_policy(proposal(Subject::Chemistry, 0.95F, RouteSource::Model), config);
    CHECK(out.detail.find("no experts configured") != std::string::npos);
}

TEST(disabling_the_fallback_expert_skips_it_for_empty_seats) {
    Config config = config_with({Subject::Physics, Subject::Fallback});
    config.routing.use_fallback_expert = false;

    const RouteDecision out =
        apply_route_policy(proposal(Subject::Chemistry, 0.95F, RouteSource::Model), config);
    CHECK(out.subject == Subject::Physics);
}

// ---------------------------------------------------------------------------
// UTF-8 streaming
// ---------------------------------------------------------------------------

TEST(utf8_lengths_are_read_from_the_lead_byte) {
    CHECK_EQ(detail::utf8_length('a'),  1);
    CHECK_EQ(detail::utf8_length(0xC3), 2);
    CHECK_EQ(detail::utf8_length(0xE2), 3);
    CHECK_EQ(detail::utf8_length(0xF0), 4);
    CHECK_EQ(detail::utf8_length(0x80), 1);  // stray continuation: make progress
}

TEST(complete_utf8_passes_straight_through) {
    std::string buffer = "hello";
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("hello"));
    CHECK(buffer.empty());

    buffer = "caf\xC3\xA9";  // café
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("caf\xC3\xA9"));
    CHECK(buffer.empty());
}

TEST(split_codepoints_are_held_back_until_complete) {
    // The exact failure this guards against: a token ending mid-codepoint would
    // otherwise be printed as a replacement character.
    std::string buffer = "abc\xE2\x80";           // '…' missing its last byte
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("abc"));
    CHECK_EQ(buffer, std::string("\xE2\x80"));

    buffer += "\xA6";                              // the byte finally arrives
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("\xE2\x80\xA6"));
    CHECK(buffer.empty());
}

TEST(a_four_byte_emoji_arriving_one_byte_at_a_time) {
    const std::string emoji = "\xF0\x9F\xA6\x87";  // 🦇, which BatBot has earned
    std::string buffer;
    std::string emitted;
    for (const char byte : emoji) {
        buffer += byte;
        emitted += detail::take_complete_utf8(buffer);
    }
    CHECK_EQ(emitted, emoji);
    CHECK(buffer.empty());
}

TEST(empty_buffer_is_handled) {
    std::string buffer;
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string());
    CHECK(buffer.empty());
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(missing_config_is_created_with_defaults) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    CHECK(std::filesystem::exists(file));
    CHECK(config.is_empty());
    CHECK(config.configured_experts().empty());
}

TEST(experts_inherit_unset_fields_from_defaults) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << R"({
          "defaults": { "n_ctx": 4096, "temperature": 0.25, "n_gpu_layers": 7 },
          "experts": {
            "physics": { "model": "/tmp/physics.gguf" },
            "biology": { "model": "/tmp/biology.gguf", "n_ctx": 999 }
          }
        })";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    const ModelParams& physics = config.experts[static_cast<std::size_t>(Subject::Physics)];
    CHECK_EQ(physics.n_ctx, 4096);
    CHECK_EQ(physics.n_gpu_layers, 7);
    CHECK(physics.temperature == 0.25F);

    // An explicit value must survive inheritance.
    const ModelParams& biology = config.experts[static_cast<std::size_t>(Subject::Biology)];
    CHECK_EQ(biology.n_ctx, 999);
    CHECK_EQ(biology.n_gpu_layers, 7);

    CHECK(config.has_expert(Subject::Physics));
    CHECK(config.has_expert(Subject::Biology));
    CHECK(!config.has_expert(Subject::Chemistry));
    CHECK_EQ(config.configured_experts().size(), std::size_t{2});
}

TEST(a_malformed_field_warns_but_keeps_the_rest) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        // n_ctx is a string, which is wrong. One bad field should not cost the
        // user the other eight experts.
        out << R"({
          "experts": { "physics": { "model": "/tmp/p.gguf", "n_ctx": "enormous" } }
        })";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    CHECK(!warnings.empty());
    CHECK(config.has_expert(Subject::Physics));
    CHECK_EQ(config.experts[static_cast<std::size_t>(Subject::Physics)].n_ctx, 8192);
}

TEST(unparseable_config_falls_back_instead_of_throwing) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << "{ this is not json at all";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    CHECK(!warnings.empty());
    CHECK(config.is_empty());
}

// ---------------------------------------------------------------------------
// Models directory
// ---------------------------------------------------------------------------

TEST(bare_names_resolve_inside_the_models_directory) {
    const std::filesystem::path dir = "/srv/models";

    // The normal case: the config names a file, the directory says where.
    CHECK_EQ(resolve_model_ref(dir, "physics-q4.gguf").string(),
             std::string("/srv/models/physics-q4.gguf"));

    // A path escapes the directory, so a model can live anywhere.
    CHECK_EQ(resolve_model_ref(dir, "/opt/big/expert.gguf").string(),
             std::string("/opt/big/expert.gguf"));

    // Relative-with-slash stays relative to the models directory.
    CHECK_EQ(resolve_model_ref(dir, "subdir/expert.gguf").string(),
             std::string("/srv/models/subdir/expert.gguf"));

    CHECK(resolve_model_ref(dir, "").empty());
}

TEST(is_bare_name_distinguishes_a_file_from_a_path) {
    CHECK(is_bare_name("expert.gguf"));
    CHECK(!is_bare_name("/abs/expert.gguf"));
    CHECK(!is_bare_name("~/models/expert.gguf"));
    CHECK(!is_bare_name("sub/expert.gguf"));
    CHECK(!is_bare_name(""));
}

TEST(scanning_finds_only_gguf_files_in_name_order) {
    TempDir dir;
    const auto touch = [&](const std::string& name, std::size_t bytes) {
        std::ofstream out(dir.path() / name, std::ios::binary);
        out << std::string(bytes, '\0');
    };
    touch("zeta.gguf", 3000);
    touch("alpha.gguf", 2000);
    touch("notes.txt", 10);          // wrong extension
    touch("archive.gguf.bak", 10);   // extension is .bak, not .gguf
    std::filesystem::create_directories(dir.path() / "nested.gguf");  // a directory

    const std::vector<ModelFile> found = scan_models(dir.path());
    CHECK_EQ(found.size(), std::size_t{2});
    if (found.size() == 2) {
        CHECK_EQ(found[0].name, std::string("alpha.gguf"));
        CHECK_EQ(found[1].name, std::string("zeta.gguf"));
        CHECK_EQ(found[0].bytes, std::uintmax_t{2000});
        CHECK(found[0].path.is_absolute() || found[0].path.string().find(dir.path().string()) == 0);
    }
}

TEST(scanning_a_missing_directory_is_not_an_error) {
    // A first run has no models directory yet; that is a normal state the UI
    // explains, not a failure that should propagate.
    CHECK(scan_models("/definitely/not/a/real/directory").empty());
}

TEST(models_dir_falls_back_to_the_default_when_blank) {
    Config config;
    CHECK(config.models_dir.empty());
    CHECK_EQ(config.resolved_models_dir(), paths::models_dir());

    config.models_dir = "/srv/gguf";
    CHECK_EQ(config.resolved_models_dir().string(), std::string("/srv/gguf"));
}

TEST(model_references_resolve_against_the_configured_directory) {
    Config config;
    config.models_dir = "/srv/gguf";
    config.router.model = "router.gguf";
    config.experts[static_cast<std::size_t>(Subject::Physics)].model = "/elsewhere/phys.gguf";
    config.resolve_models();

    CHECK_EQ(config.router.path, std::string("/srv/gguf/router.gguf"));
    CHECK_EQ(config.experts[static_cast<std::size_t>(Subject::Physics)].path,
             std::string("/elsewhere/phys.gguf"));
}

TEST(a_seat_whose_file_is_gone_is_not_shown_as_ready) {
    TempDir dir;
    const auto present = dir.path() / "here.gguf";
    { std::ofstream out(present); out << "GGUF"; }

    Config config;
    config.models_dir = dir.path().string();
    config.experts[static_cast<std::size_t>(Subject::Physics)].model = "here.gguf";
    config.experts[static_cast<std::size_t>(Subject::Biology)].model = "gone.gguf";
    config.resolve_models();

    AppState state;
    state.configure_seats(config);
    const Snapshot snapshot = state.snapshot();

    CHECK(snapshot.seats[static_cast<std::size_t>(Subject::Physics)].phase
          == SeatPhase::Dormant);
    // Assigned but absent must read differently from ready, or the roundtable
    // promises an expert that cannot answer.
    CHECK(snapshot.seats[static_cast<std::size_t>(Subject::Biology)].phase
          == SeatPhase::Missing);
    CHECK(snapshot.seats[static_cast<std::size_t>(Subject::Chemistry)].phase
          == SeatPhase::Unconfigured);
}

// ---------------------------------------------------------------------------
// Saving the config (what the in-app settings screen does)
// ---------------------------------------------------------------------------

TEST(saving_then_loading_round_trips_every_setting) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config original;
    original.models_dir           = "/srv/gguf";
    original.system_prompt        = "Be exceptionally terse.";
    original.router.model         = "router-1b.gguf";
    original.defaults.n_ctx       = 16384;
    original.defaults.temperature = 0.33F;
    original.defaults.n_gpu_layers = 42;
    original.defaults.split_mode  = "row";
    original.ui.animation_ms      = 55;
    original.ui.show_roundtable   = false;
    original.ui.unicode           = false;
    original.experts[static_cast<std::size_t>(Subject::Physics)].model = "phys.gguf";
    original.experts[static_cast<std::size_t>(Subject::Biology)].model = "bio.gguf";
    // One expert deliberately differs from defaults, to prove overrides survive.
    original.experts[static_cast<std::size_t>(Subject::Biology)].n_ctx = 999;

    CHECK(save_config(original, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);

    CHECK_EQ(reloaded.models_dir,    original.models_dir);
    CHECK_EQ(reloaded.system_prompt, original.system_prompt);
    CHECK_EQ(reloaded.router.model,  original.router.model);
    CHECK_EQ(reloaded.defaults.n_ctx, 16384);
    CHECK(reloaded.defaults.temperature == 0.33F);
    CHECK_EQ(reloaded.defaults.n_gpu_layers, 42);
    CHECK_EQ(reloaded.defaults.split_mode, std::string("row"));
    CHECK_EQ(reloaded.ui.animation_ms, 55);
    CHECK(!reloaded.ui.show_roundtable);
    CHECK(!reloaded.ui.unicode);

    CHECK_EQ(reloaded.experts[static_cast<std::size_t>(Subject::Physics)].model,
             std::string("phys.gguf"));
    CHECK_EQ(reloaded.experts[static_cast<std::size_t>(Subject::Biology)].n_ctx, 999);
    // An expert that overrode nothing must still inherit the new defaults.
    CHECK_EQ(reloaded.experts[static_cast<std::size_t>(Subject::Physics)].n_ctx, 16384);
    CHECK_EQ(reloaded.configured_experts().size(), std::size_t{2});
}

TEST(saving_does_not_write_expert_fields_that_match_defaults) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    config.defaults.n_ctx = 4096;
    config.experts[static_cast<std::size_t>(Subject::Physics)].model = "p.gguf";
    config.experts[static_cast<std::size_t>(Subject::Physics)].n_ctx = 4096;  // same as default
    CHECK(save_config(config, file));

    // Parse rather than string-search: nlohmann writes keys in alphabetical
    // order, so "router" -- which legitimately carries n_ctx -- follows the
    // experts block and would defeat a naive substring check.
    nlohmann::json doc;
    {
        std::ifstream in(file);
        in >> doc;
    }

    // Round-tripping every field would turn a short config into a wall of
    // redundant numbers the first time the user saved from the settings screen.
    const nlohmann::json& physics = doc.at("experts").at("physics");
    CHECK(physics.contains("model"));
    CHECK(!physics.contains("n_ctx"));
    CHECK(!physics.contains("temperature"));

    // The defaults block still carries the real values.
    CHECK_EQ(doc.at("defaults").at("n_ctx").get<int>(), 4096);
}

TEST(saving_a_seat_back_to_none_clears_it) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    config.experts[static_cast<std::size_t>(Subject::Physics)].model = "p.gguf";
    CHECK(save_config(config, file));

    config.experts[static_cast<std::size_t>(Subject::Physics)].model.clear();
    CHECK(save_config(config, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);
    CHECK(!reloaded.has_expert(Subject::Physics));
    CHECK(reloaded.configured_experts().empty());
}

// ---------------------------------------------------------------------------
// Trust store
// ---------------------------------------------------------------------------

TEST(trust_persists_and_covers_subdirectories) {
    TempDir dir;
    const auto store_file = dir.path() / "trust.json";
    const auto project    = dir.path() / "project";
    std::filesystem::create_directories(project / "src" / "deep");

    {
        TrustStore store(store_file);
        CHECK(!store.is_trusted(project));
        CHECK(store.trust(project));
        CHECK(store.is_trusted(project));
        // Trusting a folder covers everything beneath it.
        CHECK(store.is_trusted(project / "src"));
        CHECK(store.is_trusted(project / "src" / "deep"));
    }

    // A new store must see what the previous one wrote.
    TrustStore reloaded(store_file);
    CHECK(reloaded.is_trusted(project));
    CHECK(reloaded.is_trusted(project / "src" / "deep"));
}

TEST(trust_does_not_leak_across_sibling_prefixes) {
    TempDir dir;
    const auto store_file = dir.path() / "trust.json";
    const auto trusted   = dir.path() / "work";
    const auto sibling   = dir.path() / "work-secrets";
    std::filesystem::create_directories(trusted);
    std::filesystem::create_directories(sibling);

    TrustStore store(store_file);
    CHECK(store.trust(trusted));
    CHECK(store.is_trusted(trusted));
    // "work-secrets" merely starts with "work". A prefix comparison would
    // wrongly trust it; the check must be per path component.
    CHECK(!store.is_trusted(sibling));
    CHECK(!store.is_trusted(dir.path()));  // a parent is not trusted by a child
}

TEST(a_corrupt_trust_store_trusts_nothing_rather_than_failing) {
    TempDir dir;
    const auto store_file = dir.path() / "trust.json";
    {
        std::ofstream out(store_file);
        out << "not json";
    }
    TrustStore store(store_file);
    CHECK(!store.is_trusted(dir.path()));
    CHECK(store.entries().empty());
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

TEST(tilde_expands_to_the_home_directory) {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return;  // nothing meaningful to assert
    }
    const auto expanded = paths::expand_user("~/models/expert.gguf");
    CHECK(expanded.is_absolute());
    CHECK(expanded.string().rfind(std::string(home), 0) == 0);
    CHECK(expanded.string().find("~") == std::string::npos);

    // A tilde that is not a home reference must be left alone.
    CHECK(paths::expand_user("/tmp/~odd/file").string().find("~odd") != std::string::npos);
    CHECK(paths::expand_user("").empty());
}

TEST(xdg_config_home_is_honoured_when_absolute) {
    const auto file = paths::config_file();
    CHECK_EQ(file.filename().string(), std::string("config.json"));
    CHECK_EQ(file.parent_path().filename().string(), std::string("batbot"));
    CHECK(file.is_absolute());
}

TEST(an_empty_models_dir_means_the_default_so_resetting_is_clearing_it) {
    // This is what the "Reset to default" row does. It stores an empty string
    // rather than the resolved path, so a config written on one machine still
    // points somewhere sensible on another -- and so the default can move
    // without stranding anyone who reset.
    Config moved;
    moved.models_dir = "/mnt/external/ggufs";
    CHECK_EQ(moved.resolved_models_dir().string(), std::string("/mnt/external/ggufs"));

    moved.models_dir.clear();
    CHECK_EQ(moved.resolved_models_dir(), paths::models_dir());
}

TEST(resetting_the_models_dir_re_resolves_every_model_reference) {
    Config config;
    config.models_dir            = "/mnt/external/ggufs";
    config.router.model          = "router.gguf";
    config.experts[0].model      = "maths.gguf";
    // An absolute reference is not relative to the models directory, so moving
    // that directory must leave it exactly where it points.
    config.experts[1].model      = "/opt/models/programming.gguf";
    config.resolve_models();
    CHECK_EQ(config.router.path, std::string("/mnt/external/ggufs/router.gguf"));

    config.models_dir.clear();
    config.resolve_models();
    CHECK_EQ(config.router.path, (paths::models_dir() / "router.gguf").string());
    CHECK_EQ(config.experts[0].path, (paths::models_dir() / "maths.gguf").string());
    CHECK_EQ(config.experts[1].path, std::string("/opt/models/programming.gguf"));
}

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

TEST(the_backend_table_is_indexed_by_the_enum) {
    const auto& backends = all_backends();
    CHECK_EQ(backends.size(), kBackendCount);
    for (std::size_t i = 0; i < backends.size(); ++i) {
        // backend_info() indexes straight into the array, so a table written
        // out of order would silently return the wrong entry for every lookup.
        CHECK_EQ(static_cast<std::size_t>(backends[i].kind), i);
    }
}

TEST(every_backend_has_a_unique_id_and_round_trips) {
    for (const BackendInfo& info : all_backends()) {
        const auto parsed = backend_from_id(info.id);
        CHECK(parsed.has_value());
        CHECK_EQ(static_cast<int>(*parsed), static_cast<int>(info.kind));
        CHECK(!info.name.empty());
        CHECK(!info.blurb.empty());
        CHECK(!info.cmake_option.empty());
    }
    CHECK(!backend_from_id("metal").has_value());
    CHECK(!backend_from_id("").has_value());
}

TEST(ggml_registry_names_map_onto_backends_whatever_their_case) {
    // ggml reports "CUDA" and "Vulkan"; the ids are lower case. Getting this
    // wrong would make every installed runtime report itself as inactive.
    CHECK(backend_from_reg_name("CUDA").has_value());
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("CUDA")),
             static_cast<int>(BackendKind::Cuda));
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("Vulkan")),
             static_cast<int>(BackendKind::Vulkan));
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("CPU")),
             static_cast<int>(BackendKind::Cpu));
    CHECK(!backend_from_reg_name("BLAS").has_value());
}

TEST(a_runtime_built_against_another_llama_cpp_is_reported_as_stale) {
    // Runtimes outlive the BatBot that built them: they survive an uninstall
    // that keeps your data, and a reinstall from newer source lands on top of
    // them. ggml is not ABI-stable across releases, so one built for a
    // different tag loads and then crashes on the first tensor.
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    const std::filesystem::path runtimes = paths::runtimes_dir();
    std::filesystem::create_directories(runtimes);
    { std::ofstream module(runtimes / "libggml-cuda.so"); module << "not really a module"; }

    const auto status_for = [&](BackendKind kind) {
        for (const RuntimeStatus& status : RuntimeRegistry::scan()) {
            if (status.kind == kind) {
                return status;
            }
        }
        return RuntimeStatus{};
    };

    // The tag this binary wants: recorded, so not stale.
    {
        std::ofstream manifest(runtimes / "manifest.json");
        manifest << R"({"cuda":{"llama_tag":")"
                 << RuntimeStatus::required_llama_tag() << R"("}})";
    }
    CHECK(status_for(BackendKind::Cuda).installed);
    CHECK(!status_for(BackendKind::Cuda).stale);

    // Some other tag: stale, and the message needs the tag to name it.
    {
        std::ofstream manifest(runtimes / "manifest.json");
        manifest << R"({"cuda":{"llama_tag":"b0001"}})";
    }
    CHECK(status_for(BackendKind::Cuda).stale);
    CHECK_EQ(status_for(BackendKind::Cuda).llama_tag, std::string("b0001"));

    // No manifest entry at all means "cannot tell", which is not a reason to
    // tell someone their working runtime is broken.
    {
        std::ofstream manifest(runtimes / "manifest.json");
        manifest << "{}";
    }
    CHECK(status_for(BackendKind::Cuda).installed);
    CHECK(!status_for(BackendKind::Cuda).stale);

    // And a backend with no module is never stale, whatever the manifest says.
    CHECK(!status_for(BackendKind::Vulkan).installed);
    CHECK(!status_for(BackendKind::Vulkan).stale);
}

TEST(the_cpu_backend_is_the_one_every_other_runtime_needs) {
    // llama.cpp throws "no CPU backend found" whatever GPU is installed, which
    // is why the builder brings this one along with a CUDA or Vulkan install.
    CHECK(backend_info(BackendKind::Cpu).required);
    CHECK(!backend_info(BackendKind::Cuda).required);
    CHECK(!backend_info(BackendKind::Vulkan).required);
}

TEST(removing_a_runtime_that_is_not_installed_says_so) {
    // Scoped, and it has to be: RuntimeRegistry::remove deletes files under
    // the XDG data directory. Without this the test reaches into the runtimes
    // of whoever is running the suite and removes the one it is asking about.
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    // Every runtime can be removed now, including the CPU one -- an install
    // with no runtimes at all is the state a fresh install starts in.
    std::string error;
    CHECK(!RuntimeRegistry::remove(BackendKind::Cuda, error));
    CHECK(error.find("not installed") != std::string::npos);
}

TEST(only_multi_device_backends_advertise_gpu_splitting) {
    CHECK(!backend_info(BackendKind::Cpu).multi_device);
    CHECK(backend_info(BackendKind::Cuda).multi_device);
    CHECK(backend_info(BackendKind::Vulkan).multi_device);
}

// ---------------------------------------------------------------------------
// GPU priority order
//
// The order is edited in a panel and stored as device indices, so the config
// and the machine can disagree: a card unplugged since, or added since. What
// the panel shows has to be the machine's cards in the config's order, and
// nothing else.
// ---------------------------------------------------------------------------

namespace {

ComputeDevice gpu_at(int index, std::string name) {
    ComputeDevice device;
    device.index       = index;
    device.name        = std::move(name);
    device.description = device.name;
    device.is_gpu      = true;
    return device;
}

/// The device indices, as "2,0,1" -- a string so a mismatch prints both orders
/// rather than the address of a vector.
std::string indices_of(const std::vector<ComputeDevice>& devices) {
    std::string out;
    for (const ComputeDevice& device : devices) {
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(device.index);
    }
    return out;
}

}  // namespace

TEST(a_configured_gpu_order_is_laid_over_the_cards_present) {
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {2, 0, 1})), std::string{"2,0,1"});
}

TEST(an_unmentioned_gpu_goes_to_the_end_rather_than_vanishing) {
    // A card added since the order was written. Dropping it would quietly stop
    // BatBot using hardware the machine has.
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {2})), std::string{"2,0,1"});
}

TEST(an_order_naming_a_gpu_that_is_gone_leaves_no_hole) {
    // The config was written with three cards; two are plugged in today.
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {2, 1, 0})), std::string{"2,0"});
}

TEST(a_gpu_listed_twice_takes_only_its_first_place) {
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B")};
    const std::vector<ComputeDevice> ordered = apply_priority_order(gpus, {1, 1, 0});
    CHECK_EQ(ordered.size(), std::size_t{2});
    CHECK_EQ(indices_of(ordered), std::string{"1,0"});
}

TEST(no_configured_order_leaves_the_cards_in_ggml_order) {
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {})), std::string{"0,1,2"});
}

TEST(the_vulkan_backend_asks_for_the_spirv_headers) {
    // The package that was missing from the installer, and whose absence made
    // the Vulkan build fail at configure time with an error naming a CMake
    // package rather than anything installable.
    const std::string apt(backend_info(BackendKind::Vulkan).apt_packages);
    CHECK(apt.find("spirv-headers") != std::string::npos);
    CHECK(apt.find("glslc") != std::string::npos);
    CHECK(apt.find("libvulkan-dev") != std::string::npos);
}

// ---------------------------------------------------------------------------
// GPU splitting
// ---------------------------------------------------------------------------

namespace {

ComputeDevice fake_gpu(int index, std::string name, std::uint64_t bytes) {
    ComputeDevice device;
    device.index        = index;
    device.name         = name;
    device.description  = std::move(name);
    device.memory_total = bytes;
    device.memory_free  = bytes;
    device.is_gpu       = true;
    return device;
}

constexpr std::uint64_t kGb = 1024ULL * 1024ULL * 1024ULL;

float sum_of(const std::vector<float>& split) {
    float total = 0.0F;
    for (const float share : split) {
        total += share;
    }
    return total;
}

}  // namespace

TEST(auto_split_leaves_the_decision_to_llama_cpp) {
    const std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 8 * kGb), fake_gpu(1, "B", 8 * kGb)};
    CHECK(compute_tensor_split(GpuSplitMode::Auto, gpus, {}, 0).empty());
}

TEST(a_single_gpu_is_never_split) {
    const std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 8 * kGb)};
    CHECK(compute_tensor_split(GpuSplitMode::Even, gpus, {}, 0).empty());
    CHECK(compute_tensor_split(GpuSplitMode::Priority, gpus, {}, 0).empty());
}

TEST(even_split_is_proportional_to_memory_not_to_device_count) {
    // The whole point: a 16 GB card should take twice the work of an 8 GB one,
    // or the small card runs out first and a model that would have fit fails.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "big", 16 * kGb),
        fake_gpu(1, "small", 8 * kGb),
    };
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Even, gpus, {}, 0);

    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
    CHECK(std::abs(split[0] - (2.0F / 3.0F)) < 0.001F);
    CHECK(std::abs(split[1] - (1.0F / 3.0F)) < 0.001F);
}

TEST(even_split_falls_back_to_equal_shares_when_memory_is_unknown) {
    std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 0), fake_gpu(1, "B", 0)};
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Even, gpus, {}, 0);
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[0] - 0.5F) < 0.001F);
    CHECK(std::abs(split[1] - 0.5F) < 0.001F);
}

TEST(priority_split_favours_the_order_it_is_given) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "slow", 8 * kGb),
        fake_gpu(1, "fast", 8 * kGb),
    };
    // Device 1 is named first, so it must take the larger share even though
    // the two cards are identical and device 0 comes first by index.
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 0}, 0);
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(split[1] > split[0]);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
}

TEST(priority_split_places_unranked_devices_after_ranked_ones) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 8 * kGb),
        fake_gpu(1, "B", 8 * kGb),
        fake_gpu(2, "C", 8 * kGb),
    };
    // Only device 2 is ranked. A GPU appearing later must never outrank one
    // that was deliberately placed.
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Priority, gpus, {2}, 0);
    CHECK_EQ(split.size(), std::size_t{3});
    CHECK(split[2] > split[0]);
    CHECK(split[2] > split[1]);
}

TEST(priority_split_ignores_repeats_and_devices_that_are_not_there) {
    const std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 8 * kGb), fake_gpu(1, "B", 8 * kGb)};
    // A device listed twice would otherwise take two shares of the split.
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 1, 7}, 0);
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(split[1] > split[0]);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
}

TEST(single_mode_puts_everything_on_the_main_gpu) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 8 * kGb),
        fake_gpu(1, "B", 8 * kGb),
    };
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Single, gpus, {}, 1);
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[0]) < 0.001F);
    CHECK(std::abs(split[1] - 1.0F) < 0.001F);
}

TEST(the_split_vector_is_indexed_by_ggml_device_index_not_by_position) {
    // A machine whose GPUs are devices 1 and 3 (device 0 being the CPU) must
    // produce a four-long vector, or llama.cpp reads the weights off the end.
    const std::vector<ComputeDevice> gpus{fake_gpu(1, "A", 8 * kGb), fake_gpu(3, "B", 8 * kGb)};
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Even, gpus, {}, 1);
    CHECK_EQ(split.size(), std::size_t{4});
    CHECK(std::abs(split[0]) < 0.001F);
    CHECK(std::abs(split[2]) < 0.001F);
    CHECK(split[1] > 0.0F);
    CHECK(split[3] > 0.0F);
}

TEST(split_modes_round_trip_through_their_ids) {
    for (const GpuSplitMode mode : {GpuSplitMode::Auto, GpuSplitMode::Even,
                                    GpuSplitMode::Priority, GpuSplitMode::Single}) {
        CHECK_EQ(static_cast<int>(gpu_split_mode_from_id(gpu_split_mode_id(mode))),
                 static_cast<int>(mode));
    }
    // Anything unrecognised must be the safe option, not a crash.
    CHECK_EQ(static_cast<int>(gpu_split_mode_from_id("nonsense")),
             static_cast<int>(GpuSplitMode::Auto));
}

TEST(the_gpu_policy_leaves_a_hand_written_split_alone_in_auto_mode) {
    Config config;
    config.gpu.mode = "auto";
    config.defaults.tensor_split = {0.7F, 0.3F};

    CHECK(apply_gpu_policy(config).empty());
    CHECK_EQ(config.defaults.tensor_split.size(), std::size_t{2});
    CHECK(std::abs(config.defaults.tensor_split[0] - 0.7F) < 0.001F);
}

// ---------------------------------------------------------------------------
// Token accounting
// ---------------------------------------------------------------------------

TEST(token_counts_are_abbreviated_once_they_stop_being_readable) {
    CHECK_EQ(format_tokens(0), std::string("0"));
    CHECK_EQ(format_tokens(847), std::string("847"));
    CHECK_EQ(format_tokens(999), std::string("999"));
    CHECK_EQ(format_tokens(1000), std::string("1.0k"));
    CHECK_EQ(format_tokens(1234), std::string("1.2k"));
    CHECK_EQ(format_tokens(999999), std::string("1000.0k"));
    CHECK_EQ(format_tokens(3400000), std::string("3.4M"));
}

TEST(usage_accumulates_across_turns) {
    GenerationStats first;
    first.prompt_tokens = 100;
    first.output_tokens = 50;
    first.output_ms     = 1000.0;

    GenerationStats second;
    second.prompt_tokens = 200;
    second.output_tokens = 150;
    second.output_ms     = 3000.0;

    TokenUsage usage;
    usage.add(first);
    usage.add(second);

    CHECK_EQ(usage.input_tokens, std::uint64_t{300});
    CHECK_EQ(usage.output_tokens, std::uint64_t{200});
    CHECK_EQ(usage.turns, std::uint64_t{2});
    CHECK_EQ(usage.total_tokens(), std::uint64_t{500});
    // 200 output tokens in 4 seconds.
    CHECK(std::abs(usage.tokens_per_second() - 50.0) < 0.001);
}

TEST(a_rate_is_only_reported_once_there_is_something_to_divide) {
    TokenUsage empty;
    CHECK(std::abs(empty.tokens_per_second()) < 0.001);

    TokenUsage no_time;
    no_time.output_tokens = 100;
    CHECK(std::abs(no_time.tokens_per_second()) < 0.001);
}

TEST(the_readout_prefers_the_live_rate_while_a_reply_is_arriving) {
    TokenUsage usage;
    usage.input_tokens  = 1200;
    usage.output_tokens = 800;
    usage.output_ms     = 8000.0;   // an average of 100 tok/s

    const std::string average = usage_readout(usage, 0.0);
    CHECK(average.find("1.2k") != std::string::npos);
    CHECK(average.find("100.0 tok/s") != std::string::npos);
    // The counts have to say what they are: two bare numbers beside an arrow
    // tell the reader nothing.
    CHECK(average.find("tok") != std::string::npos);

    // While streaming, the number being asked about is the one happening now.
    const std::string live = usage_readout(usage, 42.5);
    CHECK(live.find("42.5 tok/s") != std::string::npos);
    CHECK(live.find("100.0 tok/s") == std::string::npos);

    // ui.unicode off means a terminal that cannot draw the arrows, so the
    // readout has to say the same thing in ASCII rather than emit mojibake.
    const std::string ascii = usage_readout(usage, 0.0, /*unicode=*/false);
    CHECK(ascii.find("tok in 1.2k") != std::string::npos);
    CHECK(ascii.find("out 800") != std::string::npos);
    CHECK(ascii.find("\u2191") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Session history
// ---------------------------------------------------------------------------

namespace {

Turn finished_turn(std::string prompt, std::string reply, int output_tokens) {
    Turn turn;
    turn.prompt        = std::move(prompt);
    turn.reply         = std::move(reply);
    turn.output_tokens = output_tokens;
    turn.streaming     = false;
    return turn;
}

}  // namespace

TEST(a_session_round_trips_through_disk) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::vector<Turn> turns{
        finished_turn("what is a tensor?", "A tensor is...", 42),
        finished_turn("and a manifold?", "A manifold is...", 58),
    };
    turns[1].route = RouteDecision{Subject::Mathematics, 0.9F, RouteSource::Model, "picked"};

    TokenUsage usage;
    usage.input_tokens  = 30;
    usage.output_tokens = 100;
    usage.turns         = 2;

    std::string error;
    CHECK(store.save(turns, usage, error));
    CHECK(error.empty());

    std::vector<Turn> loaded;
    TokenUsage loaded_usage;
    CHECK(store.load(store.session_id(), loaded, loaded_usage, error));
    CHECK_EQ(loaded.size(), std::size_t{2});
    CHECK_EQ(loaded[0].prompt, std::string("what is a tensor?"));
    CHECK_EQ(loaded[1].reply, std::string("A manifold is..."));
    CHECK_EQ(loaded[1].output_tokens, 58);
    CHECK(loaded[1].route.has_value());
    CHECK_EQ(static_cast<int>(loaded[1].route->subject), static_cast<int>(Subject::Mathematics));
    // How it was routed has to survive too: labelling a resumed turn
    // "fallback" when the delegator chose it is an untrue claim about history.
    CHECK_EQ(static_cast<int>(loaded[1].route->source), static_cast<int>(RouteSource::Model));
    CHECK(std::abs(loaded[1].route->confidence - 0.9F) < 0.001F);
    CHECK_EQ(loaded_usage.output_tokens, std::uint64_t{100});
}

TEST(every_route_source_survives_a_round_trip_through_its_name) {
    for (const RouteSource source : {RouteSource::Model, RouteSource::Keyword,
                                     RouteSource::Forced, RouteSource::Fallback}) {
        CHECK_EQ(static_cast<int>(route_source_from_name(route_source_name(source))),
                 static_cast<int>(source));
    }
    CHECK_EQ(static_cast<int>(route_source_from_name("something else")),
             static_cast<int>(RouteSource::Fallback));
}

TEST(a_reply_still_streaming_is_not_written) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::vector<Turn> turns{finished_turn("done", "an answer", 10)};
    Turn in_flight;
    in_flight.prompt    = "still going";
    in_flight.reply     = "half an ans";
    in_flight.streaming = true;
    turns.push_back(in_flight);

    std::string error;
    CHECK(store.save(turns, TokenUsage{}, error));

    std::vector<Turn> loaded;
    TokenUsage usage;
    CHECK(store.load(store.session_id(), loaded, usage, error));
    // A half-finished reply is not something to resume into.
    CHECK_EQ(loaded.size(), std::size_t{1});
    CHECK_EQ(loaded[0].prompt, std::string("done"));
}

TEST(sessions_are_listed_newest_first_with_the_first_prompt_as_the_title) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;

    store.adopt("20260101-120000");
    CHECK(store.save({finished_turn("the older question", "...", 5)}, TokenUsage{}, error));

    store.adopt("20260830-090000");
    CHECK(store.save({finished_turn("the newer question", "...", 5)}, TokenUsage{}, error));

    const std::vector<SessionSummary> listed = store.list();
    CHECK_EQ(listed.size(), std::size_t{2});
    CHECK_EQ(listed[0].id, std::string("20260830-090000"));
    CHECK_EQ(listed[0].title, std::string("the newer question"));
    CHECK_EQ(listed[1].title, std::string("the older question"));
    CHECK_EQ(listed[0].turns, 1);
}

TEST(a_multi_line_prompt_still_makes_a_one_line_title) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;
    CHECK(store.save({finished_turn("first line\nsecond line", "...", 1)}, TokenUsage{}, error));

    const std::vector<SessionSummary> listed = store.list();
    CHECK_EQ(listed.size(), std::size_t{1});
    // A newline in the title would break the picker's row layout.
    CHECK(listed[0].title.find('\n') == std::string::npos);
    CHECK(listed[0].title.find("second line") != std::string::npos);
}

TEST(the_project_total_counts_each_token_once_however_often_a_session_is_saved) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;

    TokenUsage usage;
    usage.input_tokens  = 100;
    usage.output_tokens = 200;
    usage.turns         = 1;

    std::vector<Turn> turns{finished_turn("q", "a", 200)};
    CHECK(store.save(turns, usage, error));
    CHECK_EQ(store.project_usage().output_tokens, std::uint64_t{200});

    // Saving the same session again after another turn must add the delta,
    // not the whole total a second time.
    turns.push_back(finished_turn("q2", "a2", 100));
    usage.input_tokens  = 150;
    usage.output_tokens = 300;
    usage.turns         = 2;
    CHECK(store.save(turns, usage, error));

    CHECK_EQ(store.project_usage().output_tokens, std::uint64_t{300});
    CHECK_EQ(store.project_usage().input_tokens, std::uint64_t{150});
    CHECK_EQ(store.project_usage().turns, std::uint64_t{2});
}

TEST(resuming_a_session_does_not_double_count_what_it_already_spent) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    std::string error;
    TokenUsage usage;
    usage.input_tokens  = 100;
    usage.output_tokens = 200;
    usage.turns         = 1;

    std::string id;
    {
        SessionStore store(Project::current());
        CHECK(store.save({finished_turn("q", "a", 200)}, usage, error));
        id = store.session_id();
        CHECK_EQ(store.project_usage().output_tokens, std::uint64_t{200});
    }

    // A second run of BatBot resumes it. Its tokens are already in the total.
    SessionStore resumed(Project::current());
    resumed.adopt(id);
    CHECK(resumed.save({finished_turn("q", "a", 200)}, usage, error));
    CHECK_EQ(resumed.project_usage().output_tokens, std::uint64_t{200});
}

TEST(a_session_can_be_deleted) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;
    CHECK(store.save({finished_turn("q", "a", 1)}, TokenUsage{}, error));
    CHECK_EQ(store.list().size(), std::size_t{1});

    CHECK(store.remove(store.session_id(), error));
    CHECK_EQ(store.list().size(), std::size_t{0});
    CHECK(!store.remove("20200101-000000", error));
}

TEST(projects_in_different_directories_do_not_share_history) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    // Two directories whose last component is the same -- the case a slug
    // alone would collide on, which is why the key carries a hash.
    TempDir a;
    TempDir b;
    std::filesystem::create_directories(a.path() / "src");
    std::filesystem::create_directories(b.path() / "src");

    const std::filesystem::path original = std::filesystem::current_path();
    std::filesystem::current_path(a.path() / "src");
    const Project first = Project::current();
    std::filesystem::current_path(b.path() / "src");
    const Project second = Project::current();
    std::filesystem::current_path(original);

    CHECK_EQ(first.name, std::string("src"));
    CHECK_EQ(second.name, std::string("src"));
    CHECK(first.dir != second.dir);
}

// ---------------------------------------------------------------------------
// Subprocess
// ---------------------------------------------------------------------------

TEST(a_child_process_reports_its_output_and_status) {
    util::Subprocess child;
    std::string error;
    CHECK(child.start({"sh", "-c", "echo one; echo two >&2; exit 3"}, {}, {}, error));
    CHECK(error.empty());

    std::vector<std::string> lines;
    std::string line;
    while (child.read_line(line)) {
        lines.push_back(line);
    }
    // stdout and stderr are merged, so both appear.
    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK_EQ(child.wait(), 3);
    // wait() is idempotent: the builder calls it from more than one place.
    CHECK_EQ(child.wait(), 3);
}

TEST(a_command_that_does_not_exist_fails_rather_than_hanging) {
    util::Subprocess child;
    std::string error;
    CHECK(child.start({"batbot-no-such-program"}, {}, {}, error));

    std::string line;
    while (child.read_line(line)) {
        // drain
    }
    // execvp failed in the child, which exits 127 the way a shell would.
    CHECK_EQ(child.wait(), 127);
}

TEST(on_path_finds_real_programs_and_not_invented_ones) {
    CHECK(util::on_path("sh"));
    CHECK(!util::on_path("batbot-definitely-not-a-program"));
    // An empty requirement means "nothing needed", which is how a backend with
    // no SDK says so.
    CHECK(util::on_path(""));
}

int main() {
    std::cout << "BatBot core tests\n\n";
    return harness::run_all();
}
