// SPDX-License-Identifier: MIT
// Tests for the parts of BatBot that need no model loaded: the subject table,
// the router grammar, keyword routing, config inheritance, the trust store,
// path expansion, and UTF-8 chunking.

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gguf.h>
#include <nlohmann/json.hpp>

#include "batbot/config/config.hpp"
#include "batbot/config/gpu_policy.hpp"
#include "batbot/routing/completion.hpp"
#include "batbot/engine/route_policy.hpp"
#include "batbot/llm/model_catalog.hpp"
#include "batbot/llm/model_shape.hpp"
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

/// Write a GGUF carrying just the keys read_model_shape looks at.
///
/// Built with ggml's own writer rather than hand-rolled bytes, so the test
/// exercises the same encoding a real model file uses. `kv_heads` is written
/// as a single value when it has one entry and as a per-layer array otherwise,
/// which is exactly how the two families of model in the wild spell it.
void write_test_gguf(const std::filesystem::path& file, std::uint32_t layers,
                     std::uint32_t embd, std::uint32_t heads,
                     const std::vector<std::int32_t>& kv_heads, std::uint32_t key_len,
                     std::uint32_t vocab) {
    gguf_context* gguf = gguf_init_empty();
    gguf_set_val_str(gguf, "general.architecture", "testarch");
    gguf_set_val_u32(gguf, "testarch.block_count", layers);
    gguf_set_val_u32(gguf, "testarch.embedding_length", embd);
    gguf_set_val_u32(gguf, "testarch.attention.head_count", heads);
    gguf_set_val_u32(gguf, "testarch.attention.key_length", key_len);
    gguf_set_val_u32(gguf, "testarch.attention.value_length", key_len);
    gguf_set_val_u32(gguf, "testarch.vocab_size", vocab);

    if (kv_heads.size() == 1) {
        gguf_set_val_u32(gguf, "testarch.attention.head_count_kv",
                         static_cast<std::uint32_t>(kv_heads.front()));
    } else {
        gguf_set_arr_data(gguf, "testarch.attention.head_count_kv", GGUF_TYPE_INT32,
                          kv_heads.data(), kv_heads.size());
    }

    gguf_write_to_file(gguf, file.string().c_str(), /*only_meta=*/true);
    gguf_free(gguf);
}

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

TEST(priority_split_fills_the_first_card_before_touching_the_next) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "slow", 8 * kGb),
        fake_gpu(1, "fast", 8 * kGb),
    };
    // Device 1 is named first, and a model small enough to live there alone
    // must not be spread at all -- that is the whole point of "priority".
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 0}, 0, ModelFit{4 * kGb, 0});
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[1] - 1.0F) < 0.001F);
    CHECK(std::abs(split[0]) < 0.001F);
}

TEST(priority_split_spills_only_what_the_first_card_cannot_hold) {
    // 10 GB of usable space per card after headroom, and a 14 GB model: the
    // first card takes what it can and the second takes the remainder. The
    // old fixed falloff gave the first card 80% of everything regardless,
    // which is how a model that fits across the machine failed to load.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 10 * kGb),
        fake_gpu(1, "B", 10 * kGb),
    };
    const std::uint64_t usable = usable_memory(gpus[0]);
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{14 * kGb, 0});

    CHECK_EQ(split.size(), std::size_t{2});
    const auto expected_first = static_cast<float>(usable) / static_cast<float>(14 * kGb);
    CHECK(std::abs(split[0] - expected_first) < 0.01F);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
    // And the first card is never asked for more than it holds.
    CHECK(split[0] * static_cast<float>(14 * kGb) <= static_cast<float>(usable) + 1.0F);
}

TEST(no_card_is_ever_asked_for_more_than_it_can_hold) {
    // The bug this exists for, with the real shape of it: three cards of
    // 12/16/12 GB, the middle one ranked first, and a 32 GB model. The fixed
    // falloff handed the 16 GB card 76% -- 24 GB of weights -- and left 13 GB
    // untouched on the other two.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "RTX 4070",    12 * kGb),
        fake_gpu(1, "RTX 5060 Ti", 16 * kGb),
        fake_gpu(2, "RTX 3060",    12 * kGb),
    };
    const std::uint64_t model = 32 * kGb;
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 2, 0}, 0, ModelFit{model, 0});

    CHECK_EQ(split.size(), std::size_t{3});
    for (const ComputeDevice& gpu : gpus) {
        const auto share = split[static_cast<std::size_t>(gpu.index)];
        const auto bytes = static_cast<std::uint64_t>(share * static_cast<float>(model));
        CHECK(bytes <= usable_memory(gpu) + kGb / 64);
    }
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
    // Ranked first, so it carries the most.
    CHECK(split[1] > split[2]);
    CHECK(split[1] > split[0]);
}

TEST(priority_split_places_unranked_devices_after_ranked_ones) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 8 * kGb),
        fake_gpu(1, "B", 8 * kGb),
        fake_gpu(2, "C", 8 * kGb),
    };
    // Only device 2 is ranked. A GPU appearing later must never outrank one
    // that was deliberately placed, so a model that fits on one card goes
    // entirely to device 2.
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {2}, 0, ModelFit{4 * kGb, 0});
    CHECK_EQ(split.size(), std::size_t{3});
    CHECK(std::abs(split[2] - 1.0F) < 0.001F);
    CHECK(std::abs(split[0]) < 0.001F);
    CHECK(std::abs(split[1]) < 0.001F);
}

TEST(priority_split_ignores_repeats_and_devices_that_are_not_there) {
    const std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 8 * kGb), fake_gpu(1, "B", 8 * kGb)};
    // A device listed twice would otherwise take two shares of the split, and
    // device 7 is not in the machine at all.
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 1, 7}, 0, ModelFit{4 * kGb, 0});
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[1] - 1.0F) < 0.001F);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
}

TEST(a_bigger_model_reaches_further_down_the_priority_order) {
    // The reason the split is computed per-model rather than per-machine: the
    // same priority order means different things for a 1B delegator and a 30B
    // expert, and stamping one arrangement on both would either strand the
    // small model across three cards or refuse the large one.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 12 * kGb),
        fake_gpu(1, "B", 12 * kGb),
        fake_gpu(2, "C", 12 * kGb),
    };
    const std::vector<int> order{0, 1, 2};

    const std::vector<float> small =
        compute_tensor_split(GpuSplitMode::Priority, gpus, order, 0, ModelFit{2 * kGb, 0});
    const std::vector<float> large =
        compute_tensor_split(GpuSplitMode::Priority, gpus, order, 0, ModelFit{25 * kGb, 0});

    // The small one never leaves the first card.
    CHECK(std::abs(small[0] - 1.0F) < 0.001F);
    CHECK(std::abs(small[2]) < 0.001F);
    // The large one needs all three.
    CHECK(large[0] > 0.0F);
    CHECK(large[1] > 0.0F);
    CHECK(large[2] > 0.0F);
}

TEST(a_model_too_large_for_the_machine_is_divided_by_capacity) {
    // It will not load whatever is done here, and "Dedicated VRAM only" is
    // what turns that into a message. But piling the overflow onto one card
    // would make the failure worse than it has to be, so the split falls back
    // to what each card can hold.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "small", 4 * kGb),
        fake_gpu(1, "big",  12 * kGb),
    };
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{400 * kGb, 0});
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(split[1] > split[0]);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
}

TEST(priority_falls_back_to_capacity_when_the_model_size_is_unknown) {
    // An unfilled seat, or a path that no longer resolves. Dividing by
    // capacity at least never hands a card more than its share.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "small", 4 * kGb),
        fake_gpu(1, "big",  12 * kGb),
    };
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{});
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
// Slash-command completion
//
// The menu and the grey suggestion after the cursor both come from these two
// functions, so what they refuse to offer matters as much as what they offer.
// ---------------------------------------------------------------------------

TEST(a_lone_slash_offers_every_command) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/");
    CHECK(!matches.empty());
    CHECK(matches.size() == ui::all_commands().size());
}

TEST(typing_narrows_the_list_to_what_still_matches) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/re");
    CHECK(matches.size() == 2);
    CHECK(matches[0].name == "resume");
    CHECK(matches[1].name == "release");
}

TEST(the_experts_are_completable_too) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/phy");
    CHECK(matches.size() == 1);
    CHECK(matches[0].name == "physics");
    // An expert takes a prompt after it, so completing one leaves the cursor
    // ready to type rather than up against the end of the word.
    CHECK(ui::command_completion("/phy", matches[0]) == "sics ");
}

TEST(a_command_that_is_already_complete_is_not_suggested_back) {
    // Offering to complete "/help" to "/help" is noise, and it would put a
    // menu over the transcript for every finished command.
    CHECK(ui::command_matches("/help").empty());
}

TEST(nothing_is_offered_once_the_command_is_settled) {
    // A space means the user has moved on to the argument, and the menu gets
    // out of the way. This is over-determined -- typed_word() bails on a space
    // and no command name contains one, so prefix matching would reject these
    // anyway. Kept because it is the behaviour a reader wants pinned, not
    // because either mechanism alone is in doubt.
    CHECK(ui::command_matches("/physics why is the sky blue").empty());
    CHECK(ui::command_matches("/help ").empty());
    CHECK(ui::command_matches("/re sume").empty());
}

TEST(ordinary_text_is_not_a_command) {
    CHECK(ui::command_matches("hello").empty());
    CHECK(ui::command_matches("").empty());
    CHECK(ui::command_matches("what about /help").empty());
}

TEST(completion_is_case_insensitive_like_the_commands_themselves) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/RES");
    CHECK(matches.size() == 1);
    CHECK(matches[0].name == "resume");
}

TEST(completion_returns_only_what_is_missing) {
    // The caller appends it and draws it in grey, so returning the whole
    // command would double the part already typed.
    const ui::CommandInfo resume{"resume", "", false};
    CHECK(ui::command_completion("/re", resume) == "sume");
    CHECK(ui::command_completion("/", resume) == "resume");
    // A choice that does not match what is typed completes to nothing rather
    // than replacing the line under the user.
    CHECK(ui::command_completion("/xyz", resume).empty());
}

TEST(every_command_the_menu_offers_has_something_to_say_about_itself) {
    // The summary is the whole right-hand column of the menu and of /help.
    for (const ui::CommandInfo& command : ui::all_commands()) {
        CHECK(!command.name.empty());
        CHECK(!command.summary.empty());
    }
}

// ---------------------------------------------------------------------------
// GPU memory policy
//
// "GPU-only compute" and "Dedicated VRAM only" are settings about the machine;
// apply_gpu_policy is what turns them into the per-model flags llama.cpp is
// actually handed.
// ---------------------------------------------------------------------------

TEST(vram_only_is_stamped_onto_every_model) {
    Config config;
    config.gpu.vram_only = true;
    apply_gpu_policy(config);

    // The delegator as well as the experts: it is resident for the whole
    // session, so it is the last model that should be allowed to sit in RAM.
    CHECK(config.router.vram_only);
    CHECK(config.router.direct_io);
    CHECK(config.router.no_host);
    CHECK(config.defaults.vram_only);
    for (const ModelParams& expert : config.experts) {
        CHECK(expert.vram_only);
        CHECK(expert.direct_io);
    }
}

TEST(the_memory_policy_is_applied_even_in_auto_split_mode) {
    // Where the computing happens is a different question from how it is
    // divided between cards, and "auto" is an answer to the second one only.
    Config config;
    config.gpu.mode      = "auto";
    config.gpu.vram_only = true;
    apply_gpu_policy(config);
    CHECK(config.defaults.vram_only);
}

TEST(the_memory_policy_is_off_unless_it_is_asked_for) {
    Config config;
    config.gpu.vram_only = false;
    config.gpu.gpu_only  = false;
    apply_gpu_policy(config);
    CHECK(!config.defaults.vram_only);
    CHECK(!config.defaults.direct_io);
    CHECK(!config.defaults.no_host);
}

TEST(gpu_only_forces_every_layer_onto_the_card_or_does_nothing_at_all) {
    // On a machine with no GPU the processor is the only thing there is to
    // compute on, so overwriting the configured layer count would be a
    // destructive gesture with nothing to show for it.
    Config config;
    config.gpu.gpu_only          = true;
    config.defaults.n_gpu_layers = 12;
    apply_gpu_policy(config);

    if (gpu_devices().empty()) {
        CHECK(config.defaults.n_gpu_layers == 12);
        CHECK(!config.defaults.no_host);
    } else {
        // -1 is llama.cpp's "every layer plus the output".
        CHECK(config.defaults.n_gpu_layers == -1);
        CHECK(config.defaults.no_host);
    }
}

TEST(the_memory_settings_survive_a_round_trip_through_the_config_file) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "config.json";

    Config written;
    written.gpu.gpu_only  = false;
    written.gpu.vram_only = true;
    CHECK(save_config(written, file));

    std::vector<std::string> warnings;
    const Config read = load_config(file, warnings);
    CHECK(!read.gpu.gpu_only);
    CHECK(read.gpu.vram_only);
}

// ---------------------------------------------------------------------------
// Module naming
//
// ggml opens a backend module by an exact file name, so BatBot's idea of what
// one is called has to agree with ggml's own down to the character.
// ---------------------------------------------------------------------------

TEST(module_names_match_the_convention_ggml_loads_by) {
#ifdef _WIN32
    CHECK(module_prefix() == "ggml-");
    CHECK(module_suffix() == ".dll");
#else
    // macOS is not the odd one out: CMake gives a MODULE library the .so
    // suffix there too, which is why ggml only special-cases Windows.
    CHECK(module_prefix() == "libggml-");
    CHECK(module_suffix() == ".so");
#endif
}

// ---------------------------------------------------------------------------
// Model shape
//
// The GPU split and the "Dedicated VRAM only" check both need to know how much
// room a model will want before it is loaded. Both used to guess at it; these
// pin the arithmetic that replaced the guess.
// ---------------------------------------------------------------------------

TEST(kv_cache_grows_with_the_context_it_is_asked_for) {
    ModelShape shape;
    shape.known        = true;
    shape.layers       = 32;
    shape.kv_per_token = 1024;

    CHECK_EQ(shape.kv_bytes(1000), std::uint64_t{1024 * 1000});
    // Twice the context is twice the cache, which is the whole reason the
    // context size is the number worth suggesting when a model will not fit.
    CHECK_EQ(shape.kv_bytes(2000), 2 * shape.kv_bytes(1000));
    CHECK_EQ(shape.kv_bytes(0), std::uint64_t{0});
}

TEST(a_model_with_no_readable_attention_shape_claims_no_cache) {
    // Better to under-state a cache we could not measure than to refuse a
    // model on the strength of a number we invented.
    ModelShape shape;
    shape.known = true;
    CHECK_EQ(shape.kv_bytes(8192), std::uint64_t{0});
}

TEST(the_compute_allowance_follows_the_vocabulary_and_the_batch) {
    // The logits buffer is the large part, and it is vocabulary times batch.
    ModelShape small;
    small.vocab = 32000;
    ModelShape large;
    large.vocab = 151936;

    CHECK(large.compute_bytes(512) > small.compute_bytes(512));
    CHECK(small.compute_bytes(1024) > small.compute_bytes(512));
    // A batch of zero is not a reason to return nothing: there is still a
    // graph to run.
    CHECK(small.compute_bytes(0) > 0);
}

TEST(the_total_is_the_weights_plus_the_cache_plus_the_compute) {
    ModelShape shape;
    shape.known        = true;
    shape.weights      = 8ULL * 1024 * 1024 * 1024;
    shape.layers       = 32;
    shape.vocab        = 32000;
    shape.kv_per_token = 2048;

    CHECK_EQ(shape.resident_bytes(4096), shape.weights + shape.kv_bytes(4096));
    CHECK_EQ(shape.total_bytes(4096, 512),
             shape.resident_bytes(4096) + shape.compute_bytes(512));
    // The split divides what follows the layers; the compute buffers do not.
    CHECK(shape.total_bytes(4096, 512) > shape.resident_bytes(4096));
}

TEST(a_file_that_is_not_a_gguf_reports_its_size_and_nothing_else) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "not-a-model.gguf";
    { std::ofstream out(file); out << "this is not a model"; }

    const ModelShape shape = read_model_shape(file);
    CHECK(!shape.known);
    CHECK(shape.weights > 0);   // the caller can still fall back on the size
    CHECK_EQ(shape.kv_per_token, std::uint64_t{0});
}

TEST(a_missing_file_has_no_shape_at_all) {
    TempDir dir;
    const ModelShape shape = read_model_shape(dir.path() / "gone.gguf");
    CHECK(!shape.known);
    CHECK_EQ(shape.weights, std::uint64_t{0});
}

TEST(a_gguf_header_is_read_for_its_attention_shape) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "plain.gguf";
    write_test_gguf(file, /*layers=*/32, /*embd=*/4096, /*heads=*/32,
                    /*kv_heads=*/{8}, /*key_len=*/128, /*vocab=*/32000);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.layers, std::uint32_t{32});
    CHECK_EQ(shape.vocab, std::uint32_t{32000});
    // 32 layers x 8 KV heads x (128 + 128) x 2 bytes.
    CHECK_EQ(shape.kv_per_token, std::uint64_t{32} * 8 * 256 * 2);
}

TEST(a_hybrid_model_is_not_charged_for_the_layers_that_keep_no_cache) {
    // LFM2 and friends write head_count_kv as one value per layer, with a zero
    // for every layer that has no KV cache. Reading only the first value would
    // over-count such a model several times over -- and reading the scalar
    // form of the key would abort, since it is an array.
    TempDir dir;
    const std::filesystem::path file = dir.path() / "hybrid.gguf";
    write_test_gguf(file, /*layers=*/4, /*embd=*/2048, /*heads=*/32,
                    /*kv_heads=*/{0, 0, 8, 0}, /*key_len=*/64, /*vocab=*/65536);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.layers, std::uint32_t{4});
    // Only the third layer costs anything: 8 heads x (64 + 64) x 2 bytes.
    CHECK_EQ(shape.kv_per_token, std::uint64_t{8} * 128 * 2);
}

TEST(a_stated_head_dimension_beats_dividing_the_embedding_by_the_heads) {
    // qwen3next states a key length of 256 against an embedding of 2048 over
    // 16 heads, which would derive as 128 -- half the real cache.
    TempDir dir;
    const std::filesystem::path file = dir.path() / "stated.gguf";
    write_test_gguf(file, /*layers=*/48, /*embd=*/2048, /*heads=*/16,
                    /*kv_heads=*/{2}, /*key_len=*/256, /*vocab=*/151936);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.kv_per_token, std::uint64_t{48} * 2 * 512 * 2);
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
