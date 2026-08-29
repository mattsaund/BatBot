// SPDX-License-Identifier: MIT
// Tests for the parts of BatBot that need no model loaded: the subject table,
// the router grammar, keyword routing, config inheritance, the trust store,
// path expansion, and UTF-8 chunking.

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "batbot/config/config.hpp"
#include "batbot/engine/route_policy.hpp"
#include "batbot/llm/model_catalog.hpp"
#include "batbot/config/paths.hpp"
#include "batbot/routing/router.hpp"
#include "batbot/engine/state.hpp"
#include "batbot/routing/subject.hpp"
#include "batbot/util/text.hpp"
#include "batbot/config/trust.hpp"
#include "harness.hpp"

using namespace batbot;

namespace {

/// A directory that cleans itself up, so tests never leave files behind.
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

int main() {
    std::cout << "BatBot core tests\n\n";
    return harness::run_all();
}
