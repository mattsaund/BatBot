// SPDX-License-Identifier: MIT
// Tests for the parts of Crucible that need no model loaded: the subject table,
// the router grammar, keyword routing, config inheritance, the trust store,
// path expansion, and UTF-8 chunking.

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <random>
#include <map>
#include <set>

#include <ggml.h>
#include <gguf.h>
#include <nlohmann/json.hpp>

#include "crucible/config/config.hpp"
#include "crucible/config/gpu_policy.hpp"
#include "crucible/routing/benchmark.hpp"
#include "crucible/routing/completion.hpp"
#include "crucible/engine/route_policy.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/llm/model_shape.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/web_search.hpp"
#include "crucible/util/markdown.hpp"
#include "crucible/util/resources.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/routing/router.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/routing/expert.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/builder.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/session/store.hpp"
#include "crucible/session/usage.hpp"
#include "crucible/util/subprocess.hpp"
#include "crucible/util/text.hpp"
#include "crucible/config/trust.hpp"
#include "harness.hpp"

using namespace crucible;

namespace {

/// A directory that cleans itself up, so tests never leave files behind.
/// Point the XDG data directory at a temporary place, so a test that reads or
/// writes a real Crucible directory cannot touch the one belonging to whoever is
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
              / ("crucible-test-" + std::to_string(::getpid()) + "-"
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

/// The shipped roster, shared by every test that needs one. Built once: it is
/// immutable, and the routers take it by shared_ptr anyway.
const std::shared_ptr<const Roster>& shipped() {
    static const std::shared_ptr<const Roster> roster =
        std::make_shared<const Roster>(Roster::defaults());
    return roster;
}

ExpertId route_of(const std::string& prompt) {
    KeywordRouter router(shipped());
    return router.route(prompt, {}).expert;
}

/// One seat's state out of a snapshot, found by id rather than by index.
///
/// The seats vector is parallel to the roster the snapshot carries, and the
/// roster is no longer a fixed order known at compile time, so a test that
/// indexed by a hard-coded number would silently start reading its neighbour
/// the first time a seat moved.
SeatState seat_of(const Snapshot& snapshot, const ExpertId& id) {
    if (snapshot.roster) {
        if (const std::optional<std::size_t> row = snapshot.roster->find(id)) {
            if (*row < snapshot.seats.size()) {
                return snapshot.seats[*row];
            }
        }
    }
    return SeatState{};
}

}  // namespace

// ---------------------------------------------------------------------------
// The roster
// ---------------------------------------------------------------------------

TEST(the_shipped_roster_is_complete_and_unique) {
    const Roster roster = Roster::defaults();
    CHECK_EQ(roster.size(), std::size_t{10});

    std::set<std::string> ids;
    std::set<std::string> tags;
    for (const Expert& expert : roster.experts()) {
        CHECK(!expert.id.empty());
        CHECK(!expert.name.empty());
        CHECK(!expert.blurb.empty());
        CHECK(!expert.tag.empty());
        CHECK(expert.tag.size() <= std::size_t{4});
        CHECK(expert.builtin);

        // Two seats sharing an id would make one of them unreachable by slash
        // command and would collide in the config map; two sharing a tag would
        // put the same chip on two rows of the roundtable.
        CHECK(ids.insert(expert.id).second);
        CHECK(tags.insert(expert.tag).second);
    }
}

TEST(every_shipped_expert_but_the_fallback_is_routable_and_worked) {
    const Roster roster = Roster::defaults();
    for (const Expert& expert : roster.experts()) {
        if (expert.id == kFallbackId) {
            continue;
        }
        CHECK(expert.routable);
        // Two examples each and a keyword set each: both are what the two
        // routers actually read, and a seat missing either is a seat that
        // cannot be reached by that router.
        CHECK_EQ(expert.examples.size(), std::size_t{2});
        CHECK(!expert.keywords.empty());
    }
}

TEST(roster_lookup_accepts_ids_tags_names_and_case) {
    const Roster roster = Roster::defaults();
    const auto is = [&](const char* key, const char* id) {
        const std::optional<std::size_t> found = roster.find(key);
        return found.has_value() && roster.at(*found).id == id;
    };

    CHECK(is("physics",    "physics"));
    CHECK(is("PHYSICS",    "physics"));
    CHECK(is("PhYsIcS",    "physics"));
    CHECK(is("PHYS",       "physics"));
    CHECK(is("phys",       "physics"));
    CHECK(is("Physics",    "physics"));

    // Surrounding whitespace must not defeat a lookup: this is reached from a
    // typed slash command and from a config file someone hand-edited.
    CHECK(is("  biology ", "biology"));
    CHECK(is("BIO",        "biology"));

    CHECK(!roster.find("astrology").has_value());
    CHECK(!roster.find("").has_value());
    CHECK(!roster.find("   ").has_value());
}

TEST(every_shipped_expert_round_trips_through_its_own_strings) {
    const Roster roster = Roster::defaults();
    for (const Expert& expert : roster.experts()) {
        const std::optional<std::size_t> by_id   = roster.find(expert.id);
        const std::optional<std::size_t> by_tag  = roster.find(expert.tag);
        const std::optional<std::size_t> by_name = roster.find(expert.name);
        CHECK(by_id.has_value() && roster.at(*by_id).id == expert.id);
        CHECK(by_tag.has_value() && roster.at(*by_tag).id == expert.id);
        CHECK(by_name.has_value() && roster.at(*by_name).id == expert.id);
    }
}

TEST(an_expert_needs_only_a_name_and_a_description) {
    Roster roster = Roster::bare();
    Expert expert;
    expert.name  = "Rust Async";
    expert.blurb = "tokio, futures, pinning, async traits, executor tuning";

    std::string error;
    CHECK(roster.add(expert, error));
    CHECK(error.empty());

    const std::optional<std::size_t> found = roster.find("rust-async");
    CHECK(found.has_value());
    if (!found) {
        return;
    }
    const Expert& added = roster.at(*found);

    // Everything the user was not asked for is filled in.
    CHECK_EQ(added.id, std::string("rust-async"));
    CHECK_EQ(added.tag, std::string("RA"));   // initials, not the first four letters
    CHECK(!added.keywords.empty());
    CHECK(!added.builtin);
    CHECK(added.routable);

    // And the derived keywords are content words, not the whole description.
    CHECK(std::find(added.keywords.begin(), added.keywords.end(), "tokio")
          != added.keywords.end());
    CHECK(std::find(added.keywords.begin(), added.keywords.end(), "async")
          != added.keywords.end());
}

TEST(an_expert_without_a_description_is_refused) {
    Roster roster = Roster::bare();
    Expert expert;
    expert.name = "Vibes";

    // The blurb is the only thing the delegator routes on. A seat without one
    // is not a weak seat, it is an unreachable one, so this is refused at the
    // point of creation rather than accepted and left never chosen.
    std::string error;
    CHECK(!roster.add(expert, error));
    CHECK(!error.empty());
    CHECK(!roster.find("vibes").has_value());
}

TEST(a_name_that_is_already_taken_is_refused) {
    Roster roster = Roster::defaults();
    Expert expert;
    expert.name  = "Physics";
    expert.blurb = "a second physics seat";

    std::string error;
    CHECK(!roster.add(expert, error));
    CHECK(error.find("Physics") != std::string::npos);
    CHECK_EQ(roster.size(), std::size_t{10});
}

TEST(a_name_with_nothing_to_slugify_is_refused) {
    Roster roster = Roster::bare();
    Expert expert;
    expert.name  = "!!!";
    expert.blurb = "punctuation only";

    std::string error;
    CHECK(!roster.add(expert, error));
    CHECK(!error.empty());
}

TEST(a_colliding_tag_is_broken_rather_than_duplicated) {
    Roster roster = Roster::defaults();
    Expert expert;
    // "Mathematical Analysis" gives initials "MA", which is free -- so force
    // the collision with a single-word name whose first four letters are MATH.
    expert.name  = "Mathsy";
    expert.blurb = "a seat whose tag would otherwise clash with Mathematics";

    std::string error;
    CHECK(roster.add(expert, error));

    std::set<std::string> tags;
    for (const Expert& seat : roster.experts()) {
        // Two seats sharing a chip is a bug you only notice once the wrong one
        // lights up.
        CHECK(tags.insert(seat.tag).second);
    }
}

TEST(the_fallback_seat_cannot_be_ejected) {
    Roster roster = Roster::defaults();
    std::string error;
    CHECK(!roster.remove("fallback", error));
    CHECK(!error.empty());
    CHECK(roster.find("fallback").has_value());

    // A built-in specialist can be, though: the roster is the user's list.
    CHECK(roster.remove("chemistry", error));
    CHECK(!roster.find("chemistry").has_value());
    CHECK_EQ(roster.size(), std::size_t{9});
}

TEST(the_fallback_seat_stays_last_however_the_roster_is_edited) {
    Roster roster = Roster::defaults();
    Expert expert;
    expert.name  = "Tax Law";
    expert.blurb = "deductions, filing, corporate structure, capital gains";

    std::string error;
    CHECK(roster.add(expert, error));

    // The roundtable draws the roster in order and puts the catch-all at the
    // bottom. That is a property of the list, not of the widget, so a seat
    // added after the fallback must not end up below it.
    CHECK_EQ(roster.fallback_index(), roster.size() - 1);
    CHECK_EQ(roster.at(roster.size() - 1).id, std::string(kFallbackId));
    CHECK_EQ(roster.at(roster.size() - 2).id, std::string("tax-law"));
}

TEST(an_out_of_range_seat_reads_as_the_fallback) {
    const Roster roster = Roster::defaults();
    // A handle can go stale between a seat being ejected and a turn in flight
    // noticing. Reading "nobody in particular" is true; reading whoever is at
    // index zero would attribute the work to Mathematics.
    CHECK_EQ(roster.at(9999).id, std::string(kFallbackId));
}

TEST(expert_label_falls_back_to_the_id_it_was_given) {
    const Roster roster = Roster::defaults();
    CHECK_EQ(expert_label(roster, "physics"), std::string("Physics"));
    // A session recorded against a seat that has since been ejected still has
    // to render. The id is the honest answer.
    CHECK_EQ(expert_label(roster, "rust-async"), std::string("rust-async"));
}

TEST(ids_and_tags_are_derived_the_way_the_dialog_promises) {
    CHECK_EQ(make_expert_id("Rust Async"),      std::string("rust-async"));
    CHECK_EQ(make_expert_id("  C++  Templates "), std::string("c-templates"));
    CHECK_EQ(make_expert_id("Physics"),         std::string("physics"));
    CHECK_EQ(make_expert_id("!!!"),             std::string(""));

    CHECK_EQ(make_expert_tag("Rust Async", {}), std::string("RA"));
    CHECK_EQ(make_expert_tag("Chemistry",  {}), std::string("CHEM"));
    // Four initials at most, so a long name still fits the chip.
    CHECK_EQ(make_expert_tag("One Two Three Four Five", {}), std::string("OTTF"));
}

TEST(derived_keywords_drop_the_words_that_would_match_everything) {
    const std::vector<std::string> words =
        derive_keywords("Tax Law", "how to handle the things that you should know about filing");

    // The keyword router scores by count of whole-word matches, so a list
    // holding "the" or "about" wins every prompt ever typed.
    for (const std::string& word : words) {
        CHECK(word.size() >= 4);
        CHECK(word != "the");
        CHECK(word != "about");
        CHECK(word != "should");
        CHECK(word != "handle");
        CHECK(word != "things");
    }
    CHECK(std::find(words.begin(), words.end(), "filing") != words.end());
}

TEST(worked_examples_are_parsed_out_of_whatever_the_model_replied_with) {
    // Every shape here is one a model actually produced when asked for two
    // questions and told not to decorate them.
    const std::vector<std::string> got = parse_examples(
        "Here are two questions:\n"
        "1. What preload should an M8 bolt take in aluminium?\n"
        "2) \"How do I damp a resonant bracket at 40Hz?\"\n"
        "- a third one that should not be kept\n");

    CHECK_EQ(got.size(), std::size_t{2});
    CHECK_EQ(got[0], std::string("What preload should an M8 bolt take in aluminium?"));
    CHECK_EQ(got[1], std::string("How do I damp a resonant bracket at 40Hz?"));
}

TEST(a_preamble_line_is_not_mistaken_for_a_question) {
    // A line ending in a colon is the model introducing its list, and a very
    // short line is not a question worth showing a delegator.
    const std::vector<std::string> got = parse_examples(
        "Sure, here you go:\nok\nWhat is the yield strength of 6061-T6 aluminium?\n");
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK_EQ(got[0], std::string("What is the yield strength of 6061-T6 aluminium?"));
}

// ---------------------------------------------------------------------------
// Resource monitor
// ---------------------------------------------------------------------------

TEST(a_gpu_line_becomes_a_reading) {
    util::ResourceSample sample;
    CHECK(util::parse_gpu_line("NVIDIA GeForce RTX 4070, 2749, 12282, 55, 47", sample));
    CHECK_EQ(sample.name, "NVIDIA GeForce RTX 4070");
    CHECK_EQ(sample.used,  std::uint64_t{2749} * 1024 * 1024);
    CHECK_EQ(sample.total, std::uint64_t{12282} * 1024 * 1024);
    CHECK_EQ(sample.temperature_c, 55);
    CHECK_EQ(sample.busy_percent, 47);
    CHECK_EQ(sample.memory_percent(), 22);
}

TEST(a_card_that_reports_no_sensor_is_not_a_parse_failure) {
    // Plenty of cards report no temperature, and some report no utilisation.
    // That is a fact about the card, and the memory figures are still wanted.
    util::ResourceSample sample;
    CHECK(util::parse_gpu_line("Quadro K600, 100, 1024, [N/A], [N/A]", sample));
    CHECK_EQ(sample.temperature_c, -1);
    CHECK_EQ(sample.busy_percent, -1);
    CHECK_EQ(sample.memory_percent(), 10);

    // But a line with no memory in it is not a reading at all.
    CHECK(!util::parse_gpu_line("something went wrong", sample));
    CHECK(!util::parse_gpu_line("", sample));
}

TEST(memory_is_measured_as_available_not_free) {
    // The distinction that matters on a machine running Crucible: after reading a
    // 30 GB model, nearly all of "free" memory is page cache. Reporting 98%
    // used would be true of nothing anybody cares about.
    const std::string meminfo =
        "MemTotal:       49000000 kB\n"
        "MemFree:          800000 kB\n"
        "MemAvailable:   40000000 kB\n"
        "Buffers:          100000 kB\n";
    std::uint64_t used  = 0;
    std::uint64_t total = 0;
    CHECK(util::parse_meminfo(meminfo, used, total));
    CHECK_EQ(total, std::uint64_t{49000000} * 1024);
    CHECK_EQ(used,  std::uint64_t{9000000} * 1024);

    CHECK(!util::parse_meminfo("nothing useful here\n", used, total));
}

TEST(macos_page_counts_become_bytes_used) {
    // Trimmed from real vm_stat output. The trailing full stop is part of the
    // format, and the page size is 16 KiB on Apple silicon rather than 4.
    const std::string vm_stat =
        "Mach Virtual Memory Statistics: (page size of 16384 bytes)\n"
        "Pages free:                       100.\n"
        "Pages active:                     500.\n"
        "Pages inactive:                   200.\n"
        "Pages speculative:                 50.\n"
        "Pages wired down:                 150.\n"
        "Pages purgeable:                   25.\n";

    const std::uint64_t page  = 16384;
    const std::uint64_t total = std::uint64_t{1000} * page;
    std::uint64_t used = 0;
    CHECK(util::parse_vm_stat(vm_stat, page, total, used));

    // Free, speculative and purgeable are all available without evicting
    // anything anyone wants, which is 175 of the 1000 pages.
    CHECK_EQ(used, std::uint64_t{825} * page);

    // Nothing recognisable is not a reading, however long the text is.
    CHECK(!util::parse_vm_stat("no pages here\n", page, total, used));
    CHECK(!util::parse_vm_stat(vm_stat, page, /*total=*/0, used));
}

TEST(processor_time_is_split_into_busy_and_total) {
    // Fields: user nice system idle iowait irq softirq steal. Idle and iowait
    // are the two that are not work.
    std::uint64_t busy  = 0;
    std::uint64_t total = 0;
    CHECK(util::parse_stat("cpu  100 20 30 700 50 0 0 0\ncpu0 1 2 3 4\n", busy, total));
    CHECK_EQ(total, std::uint64_t{900});
    CHECK_EQ(busy,  std::uint64_t{150});

    // The per-core lines are not the aggregate, and must not be read as one.
    CHECK(!util::parse_stat("cpu0 1 2 3 4\n", busy, total));
}

TEST(a_processor_name_is_a_name_not_a_legal_notice) {
    CHECK_EQ(util::parse_cpu_name("model name\t: 12th Gen Intel(R) Core(TM) i5-12400\n"),
             "12th Gen Intel Core i5-12400");
    CHECK_EQ(util::parse_cpu_name("model name : AMD Ryzen 9 7950X 16-Core Processor\n"),
             "AMD Ryzen 9 7950X 16-Core");
    // The clock is not part of the name, and on a modern part it is not the
    // clock either.
    CHECK_EQ(util::parse_cpu_name("model name : Intel(R) Xeon(R) CPU E5-2670 @ 2.60GHz\n"),
             "Intel Xeon E5-2670");
    CHECK(util::parse_cpu_name("flags : fpu vme\n").empty());
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------

namespace {

/// The plain text of a run of spans, with the styling thrown away.
std::string flat_spans(const std::vector<markdown::Span>& spans) {
    std::string text;
    for (const markdown::Span& span : spans) {
        text += span.text;
    }
    return text;
}

std::string flat(const markdown::Block& block) {
    return flat_spans(block.spans);
}

}  // namespace

TEST(headings_lists_and_rules_are_recognised) {
    const std::vector<markdown::Block> blocks = markdown::parse(
        "## What a pointer is\n"
        "\n"
        "- a memory address\n"
        "- a reference tool\n"
        "\n"
        "1. first\n"
        "2) second\n"
        "\n"
        "> quoted\n"
        "---\n"
        "ordinary prose\n");

    std::vector<markdown::BlockKind> kinds;
    for (const markdown::Block& block : blocks) {
        if (block.kind != markdown::BlockKind::Blank) {
            kinds.push_back(block.kind);
        }
    }
    const std::vector<markdown::BlockKind> expected{
        markdown::BlockKind::Heading,  markdown::BlockKind::Bullet,
        markdown::BlockKind::Bullet,   markdown::BlockKind::Numbered,
        markdown::BlockKind::Numbered, markdown::BlockKind::Quote,
        markdown::BlockKind::Rule,     markdown::BlockKind::Paragraph};
    CHECK_EQ(kinds.size(), expected.size());
    for (std::size_t i = 0; i < kinds.size() && i < expected.size(); ++i) {
        CHECK(kinds[i] == expected[i]);
    }
    CHECK_EQ(blocks.front().level, 2);
    CHECK_EQ(flat(blocks.front()), "What a pointer is");
}

TEST(a_table_is_recognised_by_the_row_under_its_header) {
    const std::vector<markdown::Block> blocks = markdown::parse(
        "| Planet | Radius (km) | Moons |\n"
        "|--------|------------:|:-----:|\n"
        "| Mercury | 2,440 | 0 |\n"
        "| Mars | 3,390 | 2 |\n");

    CHECK_EQ(blocks.size(), std::size_t{4});  // header, rule, two rows
    CHECK(blocks[0].kind == markdown::BlockKind::TableRow);
    CHECK(blocks[1].kind == markdown::BlockKind::TableRule);
    CHECK(blocks[2].kind == markdown::BlockKind::TableRow);

    // Three columns, and the alignment the delimiter row asked for.
    CHECK_EQ(blocks[0].cells.size(), std::size_t{3});
    CHECK_EQ(blocks[1].marker, "lrc");

    // Cell text survives intact -- a thousands separator is not markup.
    CHECK_EQ(flat_spans(blocks[2].cells.at(1)), "2,440");
    CHECK_EQ(flat_spans(blocks[0].cells.at(0)), "Planet");
    CHECK_EQ(flat_spans(blocks[3].cells.at(2)), "2");
}

TEST(a_line_with_a_pipe_in_it_is_not_a_table) {
    // The failure this guards: an answer about shell pipelines is full of
    // pipes, and turning one into a one-row table would be worse than leaving
    // the pipes alone. What makes a table is the delimiter row under it.
    for (const std::string_view prose : {"run `ls | grep foo` to filter",
                                         "the options are a | b | c",
                                         "| not | a | table |"}) {
        for (const markdown::Block& block : markdown::parse(prose)) {
            CHECK(block.kind != markdown::BlockKind::TableRow);
        }
    }
}

TEST(a_table_without_outer_pipes_is_still_a_table) {
    // GitHub-flavoured markdown allows them to be left off, and models do.
    const std::vector<markdown::Block> blocks =
        markdown::parse("a | b\n--- | ---\n1 | 2\n");
    CHECK(blocks.at(0).kind == markdown::BlockKind::TableRow);
    CHECK(blocks.at(1).kind == markdown::BlockKind::TableRule);
    CHECK_EQ(blocks.at(2).cells.size(), std::size_t{2});
    CHECK_EQ(flat_spans(blocks.at(2).cells.at(1)), "2");
}

TEST(a_table_ends_where_its_rows_do) {
    const std::vector<markdown::Block> blocks =
        markdown::parse("| a | b |\n|---|---|\n| 1 | 2 |\nback to prose\n");
    CHECK(blocks.back().kind == markdown::BlockKind::Paragraph);
    CHECK_EQ(flat_spans(blocks.back().spans), "back to prose");
}

TEST(a_fenced_block_is_code_all_the_way_to_its_close) {
    const std::vector<markdown::Block> blocks = markdown::parse(
        "Here:\n```python\n# not a heading\n- not a bullet\n**not bold**\n```\ndone\n");

    int code = 0;
    for (const markdown::Block& block : blocks) {
        if (block.kind == markdown::BlockKind::Code) {
            ++code;
            // Nothing inside a fence is markup, which is the point of a fence.
            CHECK(block.spans.size() == 1);
            CHECK(block.spans.front().code);
            CHECK_EQ(block.marker, "python");
        }
    }
    CHECK_EQ(code, 3);
    CHECK(blocks.front().kind == markdown::BlockKind::Paragraph);
    CHECK(blocks.back().kind == markdown::BlockKind::Paragraph);
}

TEST(code_keeps_its_indentation) {
    // Re-flowed code is code that no longer runs.
    const std::vector<markdown::Block> blocks =
        markdown::parse("```\ndef f():\n    return 1\n```\n");
    CHECK_EQ(blocks.at(1).spans.front().text, "    return 1");
}

TEST(inline_styling_is_split_into_runs) {
    const std::vector<markdown::Span> spans =
        markdown::parse_inline("plain **bold** and `code` and *italic*");
    std::string bold;
    std::string code;
    std::string italic;
    for (const markdown::Span& span : spans) {
        if (span.bold)   { bold   += span.text; }
        if (span.code)   { code   += span.text; }
        if (span.italic) { italic += span.text; }
    }
    CHECK_EQ(bold, "bold");
    CHECK_EQ(code, "code");
    CHECK_EQ(italic, "italic");

    // And the markers themselves are gone.
    std::string all;
    for (const markdown::Span& span : spans) {
        all += span.text;
    }
    CHECK_EQ(all, "plain bold and code and italic");
}

TEST(a_lone_asterisk_is_not_the_start_of_anything) {
    // An expert writing "3 * 4" or a footnote marker must not turn the rest of
    // the line italic and lose the character while doing it.
    for (const std::string_view line : {"3 * 4 = 12", "see note *", "a_b_c and snake_case"}) {
        std::string all;
        for (const markdown::Span& span : markdown::parse_inline(line)) {
            all += span.text;
            CHECK(!span.italic);
        }
        CHECK_EQ(all, std::string(line));
    }
}

TEST(inline_code_is_not_searched_for_markup) {
    const std::vector<markdown::Span> spans = markdown::parse_inline("use `a ** b` for powers");
    for (const markdown::Span& span : spans) {
        CHECK(!span.bold);
    }
    std::string all;
    for (const markdown::Span& span : spans) {
        all += span.text;
    }
    CHECK_EQ(all, "use a ** b for powers");
}

TEST(text_with_no_markdown_in_it_survives_unchanged) {
    // The regression that matters: a model that writes plain prose must come
    // out exactly as it went in.
    const std::string plain = "Water boils at 100 C. That is 212 F, at sea level.";
    const std::vector<markdown::Block> blocks = markdown::parse(plain);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK(blocks.front().kind == markdown::BlockKind::Paragraph);
    CHECK_EQ(flat(blocks.front()), plain);
}

// ---------------------------------------------------------------------------
// Web search
// ---------------------------------------------------------------------------

TEST(a_query_is_encoded_before_it_becomes_a_url) {
    // The query is whatever the model wrote. An unencoded '&' turns one
    // parameter into two, and an unencoded newline splits the request.
    tools::SearchSettings settings;
    settings.provider = "wikipedia";

    const std::string url = tools::request_url("a&b c=d\ne", settings);
    CHECK(url.find("a%26b%20c%3Dd%0Ae") != std::string::npos);
    CHECK(url.find('\n') == std::string::npos);
    // And what is safe is left alone, so a URL stays readable.
    CHECK(tools::request_url("linux-kernel_v6.1~rc", settings).find(
              "linux-kernel_v6.1~rc") != std::string::npos);
}

TEST(each_provider_builds_the_url_it_needs) {
    tools::SearchSettings settings;

    settings.provider = "wikipedia";
    CHECK(tools::request_url("cats", settings).rfind("https://en.wikipedia.org/w/api.php", 0) == 0);

    // searxng is somebody's own instance, so without its address there is
    // nothing to ask -- and returning a half-built URL would send the query to
    // whatever happened to answer.
    settings.provider = "searxng";
    CHECK(tools::request_url("cats", settings).empty());
    settings.endpoint = "http://localhost:8888/";
    const std::string searx = tools::request_url("cats", settings);
    CHECK_EQ(searx, "http://localhost:8888/search?format=json&q=cats");  // no doubled slash

    settings.provider = "brave";
    CHECK(tools::request_url("cats", settings).rfind("https://api.search.brave.com", 0) == 0);

    // An unknown provider asks nobody.
    settings.provider = "altavista";
    CHECK(tools::request_url("cats", settings).empty());

    // And an empty query is not a search.
    settings.provider = "wikipedia";
    CHECK(tools::request_url("", settings).empty());
}

TEST(a_wikipedia_response_becomes_results) {
    // Trimmed from a real response. The snippet arrives with the matched words
    // wrapped in markup, which an expert should not have to read around.
    const std::string body = R"({"query":{"search":[
        {"title":"List of capitals of France","snippet":"The <span class=\"searchmatch\">capital</span>\n  of France has been Paris"},
        {"title":"Paris","snippet":"Paris is the capital of France"}]}})";

    const std::vector<tools::SearchResult> results = tools::parse_results("wikipedia", body, 5);
    CHECK_EQ(results.size(), std::size_t{2});
    CHECK_EQ(results[0].title, "List of capitals of France");
    CHECK_EQ(results[0].snippet, "The capital of France has been Paris");
    CHECK_EQ(results[0].url, "https://en.wikipedia.org/wiki/List%20of%20capitals%20of%20France");
    CHECK_EQ(results[1].title, "Paris");
}

TEST(the_result_limit_is_honoured_whatever_the_provider_sent) {
    const std::string body =
        R"({"results":[{"url":"a","title":"A"},{"url":"b","title":"B"},{"url":"c","title":"C"}]})";
    CHECK_EQ(tools::parse_results("searxng", body, 2).size(), std::size_t{2});
    CHECK_EQ(tools::parse_results("searxng", body, 99).size(), std::size_t{3});
}

TEST(a_response_that_is_not_what_was_expected_yields_nothing) {
    // A rate-limit page, an error object, a truncated body. Every one of these
    // has to come back empty rather than throw out of the engine thread.
    CHECK(tools::parse_results("wikipedia", "<html>rate limited</html>", 5).empty());
    CHECK(tools::parse_results("wikipedia", R"({"error":{"code":"badvalue"}})", 5).empty());
    CHECK(tools::parse_results("searxng",   R"({"results":"not an array"})", 5).empty());
    CHECK(tools::parse_results("brave",     R"({"web":{}})", 5).empty());
    CHECK(tools::parse_results("wikipedia", "", 5).empty());
    CHECK(tools::parse_results("nobody",    R"({"query":{"search":[]}})", 5).empty());
}

TEST(an_expert_asks_to_search_on_a_line_of_its_own) {
    CHECK_EQ(tools::search_request("SEARCH: rust 1.90 release date", ""),
             "rust 1.90 release date");
    CHECK_EQ(tools::search_request("Let me look that up.\nSEARCH: tallest building 2026", ""),
             "tallest building 2026");
    // Models dress it up; the query is still the query.
    CHECK_EQ(tools::search_request("**SEARCH:** who won the 2025 world cup**", ""),
             "who won the 2025 world cup");
    CHECK_EQ(tools::search_request("  SEARCH:   spaced out  ", ""), "spaced out");
}

TEST(an_expert_talking_about_searching_is_not_searching) {
    // The failure that matters: an expert explaining the tool, or quoting the
    // instructions back, must not be taken as using it.
    CHECK(tools::search_request("You can write SEARCH: followed by a query.", "").empty());
    CHECK(tools::search_request("The answer is 42.", "").empty());
    CHECK(tools::search_request("", "").empty());
    // A marker with nothing after it is not a query either.
    CHECK(tools::search_request("SEARCH:", "").empty());
}

TEST(a_models_own_tool_call_is_recognised_as_a_search) {
    // What gpt-oss actually does when told it can search: it ignores the
    // convention it was given and writes a call in its own format, on the
    // channel meant for tool calls -- which arrives here as reasoning, with
    // nothing at all on the channel the user reads.
    CHECK_EQ(tools::search_request(
                 "", R"(We need to search.{"query": "JWST launch date", "topn": 5})"),
             "JWST launch date");

    // Nested objects must not end the scan early.
    CHECK_EQ(tools::search_request("", R"({"args": {"depth": 2}, "query": "nested"})"),
             "nested");
}

TEST(a_model_that_answered_is_not_also_asking_to_search) {
    // The reasoning is only read when nothing was said to the user. Otherwise a
    // programming expert showing you a JSON object with a "query" field would
    // send that field to a search engine.
    CHECK(tools::search_request(R"(Here is the payload: {"query": "select 1"})",
                                R"(I should show them {"query": "select 1"})")
              .empty());
    // Whitespace is not an answer, though.
    CHECK_EQ(tools::search_request("  \n ", R"({"query": "still asking"})"), "still asking");
}

TEST(search_results_are_handed_to_the_expert_as_readable_text) {
    const std::vector<tools::SearchResult> results{
        {"Paris", "https://example.org/paris", "The capital of France."}};
    const std::string text = tools::format_for_model("capital of france", results);
    CHECK(text.find("capital of france") != std::string::npos);
    CHECK(text.find("https://example.org/paris") != std::string::npos);
    CHECK(text.find("The capital of France.") != std::string::npos);

    // Nothing found is said plainly rather than as an empty list, so the expert
    // answers from what it knows instead of inventing a citation.
    const std::string nothing = tools::format_for_model("obscure thing", {});
    CHECK(nothing.find("returned nothing") != std::string::npos);
}

TEST(search_does_nothing_at_all_until_it_is_switched_on) {
    // The whole reason the setting exists: no request leaves the machine while
    // it is off, whatever a model asks for.
    tools::SearchSettings settings;
    CHECK(!settings.enabled);
    std::string error;
    CHECK(tools::search("anything", settings, error).empty());
    CHECK(error.find("off") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Reasoning and answer
// ---------------------------------------------------------------------------

namespace {

/// Feed `text` through a filter one `chunk` bytes at a time, so a test can ask
/// what happens when a marker is split across the boundary -- which is the
/// ordinary case, since a marker is several tokens long.
ResponseFilter::Piece filter_in_chunks(std::string_view text, std::size_t chunk) {
    ResponseFilter filter;
    ResponseFilter::Piece total;
    for (std::size_t at = 0; at < text.size(); at += chunk) {
        const ResponseFilter::Piece piece = filter.feed(text.substr(at, chunk));
        total.reasoning += piece.reasoning;
        total.answer    += piece.answer;
    }
    const ResponseFilter::Piece last = filter.flush();
    total.reasoning += last.reasoning;
    total.answer    += last.answer;
    return total;
}

}  // namespace

TEST(a_model_that_marks_nothing_is_left_alone) {
    // The common case, and the one that must not regress: a model with no
    // reasoning convention at all produces exactly what it produced before.
    const std::string plain = "Water boils at 100 C at sea level.";
    for (const std::size_t chunk : {1U, 3U, 7U, 999U}) {
        const ResponseFilter::Piece out = filter_in_chunks(plain, chunk);
        CHECK_EQ(out.answer, plain);
        CHECK(out.reasoning.empty());
    }
}

TEST(harmony_channels_are_split_into_working_and_answer) {
    // What gpt-oss actually emits. llama.cpp classifies these markers as
    // user-defined rather than control, so they arrive as visible text and the
    // whole analysis channel lands in the transcript unless something sorts it.
    const std::string raw =
        "<|channel|>analysis<|message|>We need the boiling point. It is 100 C."
        "<|end|><|start|>assistant<|channel|>final<|message|>"
        "Water boils at 100 °C at sea level.";

    for (const std::size_t chunk : {1U, 2U, 5U, 11U, 999U}) {
        const ResponseFilter::Piece out = filter_in_chunks(raw, chunk);
        CHECK_EQ(out.answer, "Water boils at 100 °C at sea level.");
        CHECK_EQ(out.reasoning, "We need the boiling point. It is 100 C.");
    }
}

TEST(the_role_between_harmony_messages_is_not_part_of_either) {
    // "assistant" appears between <|end|> and <|channel|>. It is a header, and
    // putting it at the front of the answer is exactly the kind of stray text
    // that makes a reply look broken.
    const ResponseFilter::Piece out = filter_in_chunks(
        "<|channel|>analysis<|message|>x<|end|><|start|>assistant"
        "<|channel|>final<|message|>y", 1);
    CHECK_EQ(out.answer, "y");
    CHECK_EQ(out.reasoning, "x");
}

TEST(think_tags_are_split_the_same_way) {
    // DeepSeek-R1, Qwen3 and most of what followed them.
    for (const std::size_t chunk : {1U, 4U, 999U}) {
        const ResponseFilter::Piece out =
            filter_in_chunks("<think>Let me work this out.</think>The answer is 42.", chunk);
        CHECK_EQ(out.answer, "The answer is 42.");
        CHECK_EQ(out.reasoning, "Let me work this out.");
    }
}

TEST(a_less_than_sign_in_an_answer_is_still_a_less_than_sign) {
    // The filter holds text back when it might be the start of a marker, and
    // the risk is that it holds back something that never was one -- or worse,
    // silently eats it.
    const std::string code = "if (a < b) { x<y; } // <not a marker> <|nope|>";
    for (const std::size_t chunk : {1U, 3U, 999U}) {
        CHECK_EQ(filter_in_chunks(code, chunk).answer, code);
    }
}

TEST(a_reply_cut_off_mid_marker_does_not_lose_its_last_characters) {
    // Hitting the token limit part way through "<|chan" leaves bytes held back
    // that nothing else will ever resolve. flush() is what releases them.
    ResponseFilter filter;
    ResponseFilter::Piece out = filter.feed("done <|chan");
    CHECK_EQ(out.answer, "done ");
    out = filter.flush();
    CHECK_EQ(out.answer, "<|chan");
}

TEST(the_filter_knows_when_the_answer_has_started) {
    // What the status line reads to say "thinking" rather than "answering".
    ResponseFilter filter;
    CHECK(filter.answering());  // a model with no markers is answering at once
    filter.feed("<|channel|>analysis<|message|>working");
    CHECK(!filter.answering());
    filter.feed("<|end|><|start|>assistant<|channel|>final<|message|>hello");
    CHECK(filter.answering());
}

// ---------------------------------------------------------------------------
// Router labels
// ---------------------------------------------------------------------------

TEST(a_reasoning_format_is_asked_where_its_answer_actually_goes) {
    // The failure this exists for, and it is not a subtle one: a delegator
    // whose chat format opens the assistant turn with a channel marker was
    // being asked how likely each subject was as the very next token, at a
    // position where no word can occur at all. gpt-oss-20b scored 13% -- the
    // 11% that guessing gives -- and put 52 of 54 prompts in one seat.
    CHECK_EQ(answer_prefix("<|start|>user<|message|>hi<|end|><|start|>assistant"),
             "<|channel|>final<|message|>");

    // Formats whose assistant turn begins with the answer need nothing, which
    // is most of them.
    CHECK(answer_prefix("<|im_start|>assistant\n").empty());
    CHECK(answer_prefix("### Assistant:").empty());
    CHECK(answer_prefix("").empty());

    // And the header has to be at the end. One earlier in the conversation is a
    // turn that already happened.
    CHECK(answer_prefix("<|start|>assistant<|message|>hello<|return|>").empty());
}

TEST(router_labels_name_every_routable_seat_and_nothing_else) {
    const Roster roster = Roster::defaults();
    const std::vector<std::size_t> routable = roster.routable();
    const std::vector<std::string> labels   = roster.router_labels();

    // The two are read in lockstep by ModelRouter: labels[i] is what it scores
    // for routable()[i]. A mismatch in length or order would route every prompt
    // to the wrong seat while looking entirely healthy.
    CHECK_EQ(labels.size(), routable.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        CHECK_EQ(labels[i], roster.at(routable[i]).name);
    }
}

TEST(router_labels_carry_no_padding) {
    // A leading or trailing space changes how a label tokenises, which would
    // score it at a position the model was never shown.
    for (const std::string& label : Roster::defaults().router_labels()) {
        CHECK(!label.empty());
        CHECK(label.front() != ' ');
        CHECK(label.back() != ' ');
    }
}

TEST(the_delegator_is_never_offered_the_fallback_seat) {
    const Roster roster = Roster::defaults();
    const std::vector<std::string> labels = roster.router_labels();

    // Unrepresentable, not merely unlikely: the fallback is not among the
    // labels, so no score can name it.
    CHECK(std::find(labels.begin(), labels.end(), roster.fallback().name) == labels.end());
    CHECK_EQ(labels.size(), roster.size() - 1);

    // And the worked examples answer with exactly those labels -- an example
    // that answered anything else would teach the model to produce a string
    // the scorer never asks about.
    for (const auto& [question, answer] : roster.router_examples()) {
        CHECK(std::find(labels.begin(), labels.end(), answer) != labels.end());
    }
}

TEST(router_system_prompt_describes_every_expert) {
    const Roster roster = Roster::defaults();
    const std::string prompt = roster.router_system_prompt();
    for (const std::size_t index : roster.routable()) {
        const Expert& expert = roster.at(index);
        CHECK(prompt.find(expert.name) != std::string::npos);
        CHECK(prompt.find(expert.blurb) != std::string::npos);
    }

    // The prompt must not mention the fallback seat at all. Checking only that
    // the specialists are present would miss this -- and did: the guard was
    // dropped in an edit and the labels and examples stayed correct while the
    // prompt silently offered an extra option the scorer could never name.
    const Expert& fallback = roster.fallback();
    CHECK(prompt.find(fallback.name)  == std::string::npos);
    CHECK(prompt.find(fallback.blurb) == std::string::npos);
    CHECK(prompt.find("FALL")         == std::string::npos);
    // An earlier prompt closed by naming Language as the catch-all, and small
    // models answered LANG to nearly everything as a result (16% accurate
    // against 63% now). Keep that phrasing out.
    CHECK(prompt.find("fits no other") == std::string::npos);
}

TEST(an_added_expert_reaches_the_delegator_without_anything_else_being_edited) {
    Roster roster = Roster::defaults();
    Expert expert;
    expert.name     = "Tax Law";
    expert.blurb    = "deductions, filing, corporate structure, capital gains";
    expert.examples = {"can I deduct a home office", "when is capital gains tax due"};

    std::string error;
    CHECK(roster.add(expert, error));

    // This is the whole point of generating the delegator's inputs from the
    // list rather than storing them beside it: one call to add(), and the
    // labels, the system prompt and the worked examples all know about it.
    const std::vector<std::string> labels = roster.router_labels();
    CHECK(std::find(labels.begin(), labels.end(), "Tax Law") != labels.end());
    CHECK(roster.router_system_prompt().find("deductions, filing") != std::string::npos);

    int examples = 0;
    for (const auto& [question, answer] : roster.router_examples()) {
        examples += answer == "Tax Law" ? 1 : 0;
    }
    CHECK_EQ(examples, 2);
}

TEST(an_ejected_expert_leaves_the_delegator_prompt_entirely) {
    Roster roster = Roster::defaults();
    std::string error;
    CHECK(roster.remove("chemistry", error));

    const std::string prompt = roster.router_system_prompt();
    CHECK(prompt.find("Chemistry") == std::string::npos);
    CHECK(prompt.find("titration") == std::string::npos);

    const std::vector<std::string> labels = roster.router_labels();
    CHECK(std::find(labels.begin(), labels.end(), "Chemistry") == labels.end());
    for (const auto& [question, answer] : roster.router_examples()) {
        CHECK(answer != "Chemistry");
    }
}

TEST(the_fallback_is_the_last_seat_and_is_not_routable) {
    const Roster roster = Roster::defaults();
    CHECK_EQ(roster.size(), std::size_t{10});
    CHECK_EQ(roster.fallback_index(), std::size_t{9});

    const Expert& fallback = roster.fallback();
    CHECK_EQ(fallback.id,   std::string("fallback"));
    CHECK_EQ(fallback.name, std::string("Fallback"));

    // The delegator's job is to pick a specialist. Offering it an "anything
    // else" option is what collapsed routing to 16% once already.
    CHECK(!fallback.routable);
    CHECK_EQ(roster.routable().size(), roster.size() - 1);
}

TEST(every_expert_gets_the_same_number_of_worked_examples) {
    const Roster roster = Roster::defaults();
    const std::vector<std::pair<std::string, std::string>> examples = roster.router_examples();
    const std::vector<std::size_t> routable = roster.routable();

    // The same number each, because a seat with more examples than its
    // neighbours is a seat the delegator is being nudged towards -- and the
    // nudge is invisible in the score until another seat stops being reachable.
    CHECK_EQ(examples.size(), routable.size() * 2);

    std::map<std::string, int> seen;
    const std::vector<std::string> labels = roster.router_labels();
    for (const auto& [question, answer] : examples) {
        CHECK(!question.empty());
        // The answer is exactly the label that gets scored, and nothing else:
        // an example demonstrating a longer answer would teach the delegator to
        // continue past the string the scorer measures.
        CHECK(std::find(labels.begin(), labels.end(), answer) != labels.end());
        ++seen[answer];
    }
    for (const std::size_t index : routable) {
        CHECK_EQ(seen[roster.at(index).name], 2);
    }
}

TEST(no_worked_example_is_a_benchmark_prompt) {
    // The examples go into the delegator's prompt and the benchmark measures
    // it. An example that is also a test case measures how well the prompt was
    // copied into the answer sheet, which is how a routing change can look like
    // an improvement while making nothing better.
    //
    // Paraphrases count, so this is not string equality. It is shared *rare*
    // words: two questions that both say "semicolon" and "comma" are the same
    // question, while two that both say "what is the difference between" merely
    // have the same shape. Rarity is measured against the benchmark itself, so
    // there is no list of stop words to keep up to date.
    std::map<std::string, int> appearances;
    const auto words = [](std::string_view text) {
        std::set<std::string> out;
        std::string word;
        for (const char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else if (!word.empty()) {
                out.insert(std::exchange(word, {}));
            }
        }
        if (!word.empty()) {
            out.insert(word);
        }
        return out;
    };

    for (const RouteCase& test : benchmark_cases()) {
        for (const std::string& word : words(test.prompt)) {
            ++appearances[word];
        }
    }

    for (const auto& [question, answer] : Roster::defaults().router_examples()) {
        const std::set<std::string> example = words(question);
        for (const RouteCase& test : benchmark_cases()) {
            std::vector<std::string> rare;
            for (const std::string& word : words(test.prompt)) {
                if (appearances[word] <= 1 && example.count(word) > 0) {
                    rare.push_back(word);
                }
            }
            if (rare.size() >= 2) {
                std::printf("      example \"%s\"\n      reuses %s of benchmark \"%s\"\n",
                            question.c_str(), rare[0].c_str(), std::string(test.prompt).c_str());
            }
            CHECK(rare.size() < 2);
        }
    }
}

// ---------------------------------------------------------------------------
// Keyword router
// ---------------------------------------------------------------------------

TEST(keyword_router_picks_the_obvious_subject) {
    CHECK(route_of("compute the derivative of this polynomial") == "mathematics");
    CHECK(route_of("my code hits a segfault when I compile")    == "programming");
    CHECK(route_of("explain the lagrangian of this system")     == "physics");
    CHECK(route_of("balance this reaction and find the enthalpy") == "chemistry");
    CHECK(route_of("how does an enzyme change a protein")       == "biology");
    CHECK(route_of("what torque does this bearing take")        == "engineering");
    CHECK(route_of("is free will compatible with determinism")  == "philosophy");
    CHECK(route_of("how does migration reshape a community")    == "sociology");
    CHECK(route_of("proofread this paragraph for tone")         == "language");
}

TEST(keyword_router_falls_back_when_nothing_matches) {
    KeywordRouter router(shipped());
    const RouteDecision decision = router.route("hello there", {});
    // Fallback is the seat for an undecidable prompt. Reporting zero confidence
    // says "no decision" rather than inventing one.
    CHECK(decision.expert    == "fallback");
    CHECK(decision.source     == RouteSource::Fallback);
    CHECK(decision.confidence == 0.0F);
}

TEST(fallback_carries_no_keywords_so_it_never_wins_on_score) {
    // Fallback must not compete with the subjects: it is reached by the
    // no-match path. A prompt with real keywords must still go to its subject.
    CHECK(route_of("compute the derivative of this polynomial") == "mathematics");
    CHECK(route_of("balance this reaction and find the enthalpy") == "chemistry");

    // And the reverse: a prompt with nothing to match reaches Fallback.
    KeywordRouter router(shipped());
    CHECK(router.route("mmm", {}).expert == "fallback");
}

TEST(keyword_router_matches_whole_words_only) {
    // Keywords hide inside ordinary words: "ion" (Chemistry) sits in "question"
    // and "opinion", "cell" (Biology) sits in "excellent". None of them should
    // count, so this prompt matches nothing and falls through to General.
    // Substring matching would score Chemistry three times and route there.
    CHECK(route_of("an excellent question about your opinion") == "fallback");

    // "gene" (Biology) hides inside both "generate" and "general".
    CHECK(route_of("generate a general overview") == "fallback");

    // The same words as whole words must still match.
    CHECK(route_of("what is an ion")            == "chemistry");
    CHECK(route_of("describe a gene")           == "biology");
    CHECK(route_of("write a function to parse") == "programming");
}

TEST(keyword_router_confidence_reflects_ambiguity) {
    KeywordRouter router(shipped());
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
Config config_with(std::initializer_list<ExpertId> filled) {
    Config config;
    for (const ExpertId& seat : filled) {
        config.experts[seat].model = "some-model.gguf";
    }
    return config;
}

RouteDecision proposal(ExpertId expert, float confidence, RouteSource source) {
    RouteDecision decision;
    decision.expert     = std::move(expert);
    decision.confidence = confidence;
    decision.source     = source;
    return decision;
}

}  // namespace

TEST(a_decision_nobody_made_names_the_fallback_seat) {
    // A default-constructed decision is what the engine gets when the delegator
    // could not run -- no model assigned, or a load that failed. It has to name
    // the seat that means "undecided", because whatever it names is where the
    // prompt goes: defaulting to a real subject sent every such prompt to that
    // subject's expert as though something had chosen it.
    const RouteDecision nothing;
    CHECK(nothing.expert == "fallback");
    CHECK(nothing.source == RouteSource::Fallback);
    CHECK(nothing.confidence == 0.0F);
}

TEST(a_confident_route_to_a_filled_seat_stands) {
    const Config config = config_with({"physics", "fallback"});
    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.95F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
    CHECK(out.source  == RouteSource::Model);
}

TEST(an_unconfident_route_goes_to_the_fallback_seat) {
    Config config = config_with({"physics", "fallback"});
    config.routing.min_confidence = 0.60F;

    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.40F, RouteSource::Model), config);
    // Below the floor the delegator is treated as having made no decision.
    CHECK(out.expert == "fallback");
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("undecided") != std::string::npos);
    CHECK(out.detail.find("Physics")   != std::string::npos);
}

TEST(a_pinned_route_ignores_the_confidence_floor) {
    Config config = config_with({"physics", "fallback"});
    config.routing.min_confidence = 0.99F;

    // The user chose this expert; second-guessing them would be wrong even at
    // a confidence the model never reports.
    const RouteDecision out =
        apply_route_policy(proposal("physics", 1.0F, RouteSource::Forced), config);
    CHECK(out.expert == "physics");
    CHECK(out.source  == RouteSource::Forced);
}

TEST(a_zero_floor_disables_the_confidence_check) {
    Config config = config_with({"physics", "fallback"});
    config.routing.min_confidence = 0.0F;
    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.01F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
}

TEST(an_empty_seat_sends_work_to_the_fallback) {
    const Config config = config_with({"physics", "fallback"});
    const RouteDecision out =
        apply_route_policy(proposal("chemistry", 0.95F, RouteSource::Model), config);
    CHECK(out.expert == "fallback");
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("Chemistry has no model") != std::string::npos);
}

TEST(with_no_fallback_configured_any_filled_seat_is_used) {
    // A partly-configured install should still answer rather than fail, and
    // should say plainly that it substituted.
    const Config config = config_with({"physics"});
    const RouteDecision out =
        apply_route_policy(proposal("chemistry", 0.95F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("used Physics") != std::string::npos);
}

TEST(with_nothing_configured_the_route_reports_it) {
    const Config config;
    const RouteDecision out =
        apply_route_policy(proposal("chemistry", 0.95F, RouteSource::Model), config);
    CHECK(out.detail.find("no experts configured") != std::string::npos);
}

TEST(disabling_the_fallback_expert_skips_it_for_empty_seats) {
    Config config = config_with({"physics", "fallback"});
    config.routing.use_fallback_expert = false;

    const RouteDecision out =
        apply_route_policy(proposal("chemistry", 0.95F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
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
    const std::string emoji = "\xF0\x9F\xA6\x87";  // 🦇, which Crucible has earned
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

    const ModelParams& physics = config.expert("physics");
    CHECK_EQ(physics.n_ctx, 4096);
    CHECK_EQ(physics.n_gpu_layers, 7);
    CHECK(physics.temperature == 0.25F);

    // An explicit value must survive inheritance.
    const ModelParams& biology = config.expert("biology");
    CHECK_EQ(biology.n_ctx, 999);
    CHECK_EQ(biology.n_gpu_layers, 7);

    CHECK(config.has_expert("physics"));
    CHECK(config.has_expert("biology"));
    CHECK(!config.has_expert("chemistry"));
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
    CHECK(config.has_expert("physics"));
    CHECK_EQ(config.expert("physics").n_ctx, 8192);
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
    config.experts["physics"].model = "/elsewhere/phys.gguf";
    config.resolve_models();

    CHECK_EQ(config.router.path, std::string("/srv/gguf/router.gguf"));
    CHECK_EQ(config.expert("physics").path,
             std::string("/elsewhere/phys.gguf"));
}

TEST(a_seat_whose_file_is_gone_is_not_shown_as_ready) {
    TempDir dir;
    const auto present = dir.path() / "here.gguf";
    { std::ofstream out(present); out << "GGUF"; }

    Config config;
    config.models_dir = dir.path().string();
    config.experts["physics"].model = "here.gguf";
    config.experts["biology"].model = "gone.gguf";
    config.resolve_models();

    AppState state;
    state.configure_seats(config);
    const Snapshot snapshot = state.snapshot();

    CHECK(seat_of(snapshot, "physics").phase
          == SeatPhase::Dormant);
    // Assigned but absent must read differently from ready, or the roundtable
    // promises an expert that cannot answer.
    CHECK(seat_of(snapshot, "biology").phase
          == SeatPhase::Missing);
    CHECK(seat_of(snapshot, "chemistry").phase
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
    original.experts["physics"].model = "phys.gguf";
    original.experts["biology"].model = "bio.gguf";
    // One expert deliberately differs from defaults, to prove overrides survive.
    original.experts["biology"].n_ctx = 999;

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

    CHECK_EQ(reloaded.expert("physics").model,
             std::string("phys.gguf"));
    CHECK_EQ(reloaded.expert("biology").n_ctx, 999);
    // An expert that overrode nothing must still inherit the new defaults.
    CHECK_EQ(reloaded.expert("physics").n_ctx, 16384);
    CHECK_EQ(reloaded.configured_experts().size(), std::size_t{2});
}

TEST(saving_does_not_write_expert_fields_that_match_defaults) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    config.defaults.n_ctx = 4096;
    config.experts["physics"].model = "p.gguf";
    config.experts["physics"].n_ctx = 4096;  // same as default
    CHECK(save_config(config, file));

    // Parse rather than string-search: "router" legitimately carries an n_ctx
    // of its own, and a naive substring check could not tell the two apart.
    nlohmann::json doc;
    {
        std::ifstream in(file);
        in >> doc;
    }

    // The experts block is an array, because the order seats are drawn in is
    // the order they are written and a JSON object has no order to preserve.
    const nlohmann::json& experts = doc.at("experts");
    CHECK(experts.is_array());
    const nlohmann::json* physics = nullptr;
    for (const nlohmann::json& entry : experts) {
        if (entry.value("id", "") == "physics") {
            physics = &entry;
        }
    }
    CHECK(physics != nullptr);
    if (physics == nullptr) {
        return;
    }

    // Round-tripping every field would turn a short config into a wall of
    // redundant numbers the first time the user saved from the settings screen.
    CHECK(physics->contains("model"));
    CHECK(!physics->contains("n_ctx"));
    CHECK(!physics->contains("temperature"));

    // A built-in seat that has not been retuned carries no identity either:
    // nine seats with twenty-four keywords each would be two hundred lines
    // nobody wrote and nobody wants to read.
    CHECK(!physics->contains("keywords"));
    CHECK(!physics->contains("blurb"));
    CHECK(physics->value("builtin", false));

    // The defaults block still carries the real values.
    CHECK_EQ(doc.at("defaults").at("n_ctx").get<int>(), 4096);
}

TEST(a_user_made_expert_survives_a_round_trip_through_disk) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    Expert expert;
    expert.name     = "Rust Async";
    expert.blurb    = "tokio, futures, pinning, async traits, executor tuning";
    expert.examples = {"why does my future never wake", "how do I pin a self-referential struct"};
    std::string error;
    CHECK(config.roster.add(expert, error));
    config.experts["rust-async"].model = "rust.gguf";
    CHECK(save_config(config, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);

    const std::optional<std::size_t> found = reloaded.roster.find("rust-async");
    CHECK(found.has_value());
    if (!found) {
        return;
    }
    const Expert& back = reloaded.roster.at(*found);

    // Everything the delegator reads has to come back, not just the name: a
    // seat that reloaded without its blurb or its examples would still appear
    // on the roundtable and quietly stop being routable to.
    CHECK_EQ(back.name, std::string("Rust Async"));
    CHECK_EQ(back.blurb, expert.blurb);
    CHECK_EQ(back.examples.size(), std::size_t{2});
    CHECK_EQ(back.examples[0], std::string("why does my future never wake"));
    CHECK(!back.keywords.empty());
    CHECK(!back.builtin);
    CHECK_EQ(reloaded.expert("rust-async").model, std::string("rust.gguf"));

    // And it is a seat like any other by the time the delegator sees it.
    const std::vector<std::string> labels = reloaded.roster.router_labels();
    CHECK(std::find(labels.begin(), labels.end(), "Rust Async") != labels.end());
}

TEST(an_ejected_expert_does_not_come_back_on_the_next_load) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    std::string error;
    CHECK(config.roster.remove("chemistry", error));
    config.experts.erase("chemistry");
    CHECK(save_config(config, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);

    // The built-in table is a set of defaults, not a floor. Quietly restoring a
    // seat the user ejected would make /ejectexpert a no-op across restarts.
    CHECK(!reloaded.roster.find("chemistry").has_value());
    CHECK_EQ(reloaded.roster.size(), std::size_t{9});
    CHECK(reloaded.roster.find("physics").has_value());
}

TEST(a_config_whose_experts_are_all_gone_keeps_only_the_fallback) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << R"({"experts": []})";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    // Someone who deleted every seat wanted every seat deleted. The fallback
    // stays because the engine needs somewhere to send work that fits nowhere,
    // and it is not a specialist the delegator can choose.
    CHECK_EQ(config.roster.size(), std::size_t{1});
    CHECK_EQ(config.roster.at(0).id, std::string(kFallbackId));
    CHECK(config.roster.router_labels().empty());
}

TEST(a_hand_written_expert_needs_only_an_id_and_a_blurb) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        // The config file is documented as hand-editable, so the loader has to
        // accept the shape a person would actually type -- including the object
        // form, which is the natural way to write a keyed list by hand.
        std::ofstream out(file);
        out << R"({"experts": {"tax-law": {"blurb": "deductions and filing",
                                            "model": "tax.gguf"}}})";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    const std::optional<std::size_t> found = config.roster.find("tax-law");
    CHECK(found.has_value());
    if (!found) {
        return;
    }
    const Expert& expert = config.roster.at(*found);
    CHECK_EQ(expert.name, std::string("tax-law"));  // no "name" given: the id stands in
    CHECK(!expert.tag.empty());                     // derived
    CHECK(!expert.keywords.empty());                // derived
    CHECK_EQ(config.expert("tax-law").model, std::string("tax.gguf"));
}

TEST(an_expert_entry_with_no_blurb_is_skipped_with_a_warning) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << R"({"experts": [{"id": "vibes", "model": "x.gguf"}]})";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    // A seat the delegator cannot route to is worse than no seat: it appears on
    // the roundtable and is never chosen. Refusing it and saying so is the only
    // outcome the user can act on.
    CHECK(!config.roster.find("vibes").has_value());
    CHECK(!warnings.empty());
}

TEST(saving_a_seat_back_to_none_clears_it) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    config.experts["physics"].model = "p.gguf";
    CHECK(save_config(config, file));

    config.experts["physics"].model.clear();
    CHECK(save_config(config, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);
    CHECK(!reloaded.has_expert("physics"));
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
    CHECK_EQ(file.parent_path().filename().string(), std::string("crucible"));
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
    config.experts["mathematics"].model      = "maths.gguf";
    // An absolute reference is not relative to the models directory, so moving
    // that directory must leave it exactly where it points.
    config.experts["programming"].model      = "/opt/models/programming.gguf";
    config.resolve_models();
    CHECK_EQ(config.router.path, std::string("/mnt/external/ggufs/router.gguf"));

    config.models_dir.clear();
    config.resolve_models();
    CHECK_EQ(config.router.path, (paths::models_dir() / "router.gguf").string());
    CHECK_EQ(config.expert("mathematics").path, (paths::models_dir() / "maths.gguf").string());
    CHECK_EQ(config.expert("programming").path, std::string("/opt/models/programming.gguf"));
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
    CHECK(!backend_from_id("opencl").has_value());
    CHECK(!backend_from_id("").has_value());
}

TEST(ggml_calls_the_metal_backend_something_else_entirely) {
    // ggml registers it as "MTL", not "Metal". Matching on the id would leave
    // an installed Metal runtime reported as inactive for ever, with its
    // devices working perfectly the whole time -- a wrong panel over a working
    // machine, which is the kind of bug nobody thinks to look for.
    CHECK(backend_from_reg_name("MTL").has_value());
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("MTL")),
             static_cast<int>(BackendKind::Metal));
    CHECK(!backend_from_reg_name("Metal").has_value());  // ggml never says this

    // And every entry's registry name resolves to itself, so a future backend
    // cannot be added with the field left at whatever looked plausible.
    for (const BackendInfo& info : all_backends()) {
        const auto parsed = backend_from_reg_name(info.reg_name);
        CHECK(parsed.has_value());
        if (parsed) {
            CHECK_EQ(static_cast<int>(*parsed), static_cast<int>(info.kind));
        }
    }
}

TEST(a_backend_the_platform_cannot_have_is_not_offered) {
    // Metal exists only on Apple hardware; CUDA has not existed on macOS since
    // 2018. Listing either in the wrong place offers a build that cannot
    // succeed, which is worse than not offering it.
#ifdef __APPLE__
    CHECK(backend_available_here(BackendKind::Metal));
    CHECK(!backend_available_here(BackendKind::Cuda));
#else
    CHECK(!backend_available_here(BackendKind::Metal));
    CHECK(backend_available_here(BackendKind::Cuda));
#endif
    // These two run anywhere, and the CPU one has to: nothing loads without it.
    CHECK(backend_available_here(BackendKind::Cpu));
    CHECK(backend_available_here(BackendKind::Vulkan));
}

TEST(the_install_hint_names_a_package_manager_this_machine_has) {
    // The field it reads from is chosen by what is on PATH rather than assumed,
    // because telling a Fedora user to run apt is worse than telling them
    // nothing at all. Whichever manager this machine has, the hint has to name
    // it and the packages have to be the ones for it.
    const std::string cuda = install_hint(backend_info(BackendKind::Cuda));
    if (!cuda.empty()) {
        const BackendInfo& info = backend_info(BackendKind::Cuda);
        const bool apt    = cuda == "sudo apt install " + std::string(info.apt_packages);
        const bool dnf    = cuda == "sudo dnf install " + std::string(info.dnf_packages);
        const bool pacman = cuda == "sudo pacman -S " + std::string(info.pacman_packages);
        CHECK(apt || dnf || pacman);
    }

    // Metal names no package anywhere: it arrives with the Xcode command line
    // tools, which Homebrew itself requires, so anyone who could run the
    // suggestion already has what it would install.
    CHECK(install_hint(backend_info(BackendKind::Metal)).empty());
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
    // Runtimes outlive the Crucible that built them: they survive an uninstall
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
    // Crucible using hardware the machine has.
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

/// Write a GGUF with a real tensor table: a small hybrid model, one block in
/// `attention_every` carrying a K/V projection and the rest a recurrent state.
///
/// The metadata-only fixture above exercises the fallback path, where there is
/// nothing to read but hparams. This one exercises the path that matters for a
/// real model: what each block weighs, and which of them actually cache.
void write_test_gguf_with_tensors(const std::filesystem::path& file, std::uint32_t layers,
                                  std::uint32_t attention_every, std::uint32_t embd,
                                  std::uint32_t kv_width, std::uint32_t vocab) {
    ggml_init_params init{};
    init.mem_size   = static_cast<std::size_t>(layers + 8) * 4 * ggml_tensor_overhead();
    init.mem_buffer = nullptr;
    init.no_alloc   = true;  // shapes only; no tensor data is written
    ggml_context* ctx = ggml_init(init);

    gguf_context* gguf = gguf_init_empty();
    gguf_set_val_str(gguf, "general.architecture", "testarch");
    gguf_set_val_u32(gguf, "testarch.block_count", layers);
    gguf_set_val_u32(gguf, "testarch.embedding_length", embd);
    gguf_set_val_u32(gguf, "testarch.ssm.inner_size", 8);
    gguf_set_val_u32(gguf, "testarch.ssm.state_size", 4);

    const auto add = [&](const std::string& name, std::int64_t rows, std::int64_t columns) {
        ggml_tensor* tensor = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, rows, columns);
        ggml_set_name(tensor, name.c_str());
        gguf_add_tensor(gguf, tensor);
    };

    add("token_embd.weight", embd, vocab);
    add("output.weight", embd, vocab);
    add("output_norm.weight", embd, 1);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        const std::string prefix = "blk." + std::to_string(layer) + ".";
        add(prefix + "attn_norm.weight", embd, 1);
        if (attention_every > 0 && (layer + 1) % attention_every == 0) {
            add(prefix + "attn_q.weight", embd, embd);
            add(prefix + "attn_k.weight", embd, kv_width);
            add(prefix + "attn_v.weight", embd, kv_width);
        } else {
            add(prefix + "ssm_conv1d.weight", 4, 8);
            add(prefix + "ssm_out.weight", embd, embd);
        }
    }

    gguf_write_to_file(gguf, file.string().c_str(), /*only_meta=*/true);
    gguf_free(gguf);
    ggml_free(ctx);
}

/// Replay llama.cpp's own layer assignment for a `tensor_split`.
///
/// Copied from llama-model.cpp: the split is made cumulative, normalised, and
/// unit `i` goes to the first device whose share exceeds `i / total`. Anything
/// this file claims about layer counts is only true if it survives this.
std::vector<int> llama_cpp_assignment(const std::vector<float>& split, int units) {
    std::vector<float> cumulative(split.size(), 0.0F);
    float running = 0.0F;
    for (std::size_t i = 0; i < split.size(); ++i) {
        running      += split[i];
        cumulative[i] = running;
    }
    for (float& value : cumulative) {
        value /= running;
    }

    std::vector<int> counts(split.size(), 0);
    for (int unit = 0; unit < units; ++unit) {
        const auto share = static_cast<float>(unit) / static_cast<float>(units);
        const auto found = std::upper_bound(cumulative.begin(), cumulative.end(), share);
        const auto device = static_cast<std::size_t>(std::distance(cumulative.begin(), found));
        if (device < counts.size()) {
            ++counts[device];
        }
    }
    return counts;
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
    // Proportional to what each card can *offer*, which is its memory less the
    // headroom every card keeps back -- so a little over two thirds rather than
    // exactly two thirds, and the smaller card gives up proportionally more.
    const auto big   = static_cast<float>(usable_memory(gpus[0], kCardHeadroom));
    const auto small = static_cast<float>(usable_memory(gpus[1], kCardHeadroom));
    CHECK(std::abs(split[0] - big / (big + small)) < 0.001F);
    CHECK(std::abs(split[1] - small / (big + small)) < 0.001F);
    CHECK(split[0] > 2.0F / 3.0F);
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
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 0}, 0, ModelFit{4 * kGb, 0, 0, {}});
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
    const std::uint64_t usable = usable_memory(gpus[0], kCardHeadroom);
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{14 * kGb, 0, 0, {}});

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
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 2, 0}, 0, ModelFit{model, 0, 0, {}});

    CHECK_EQ(split.size(), std::size_t{3});
    for (const ComputeDevice& gpu : gpus) {
        const auto share = split[static_cast<std::size_t>(gpu.index)];
        const auto bytes = static_cast<std::uint64_t>(share * static_cast<float>(model));
        CHECK(bytes <= usable_memory(gpu, kCardHeadroom) + kGb / 64);
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
        compute_tensor_split(GpuSplitMode::Priority, gpus, {2}, 0, ModelFit{4 * kGb, 0, 0, {}});
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
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 1, 7}, 0, ModelFit{4 * kGb, 0, 0, {}});
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
        compute_tensor_split(GpuSplitMode::Priority, gpus, order, 0, ModelFit{2 * kGb, 0, 0, {}});
    const std::vector<float> large =
        compute_tensor_split(GpuSplitMode::Priority, gpus, order, 0, ModelFit{25 * kGb, 0, 0, {}});

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
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{400 * kGb, 0, 0, {}});
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

TEST(a_plan_lands_on_the_layer_counts_it_asked_for) {
    // The whole reason the split is built from unit counts rather than from a
    // proportion. llama.cpp places whole layers, so the only way to know a card
    // will hold what the arithmetic promised is to aim at a boundary -- and the
    // only way to know that worked is to replay llama.cpp's own rule.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 10 * kGb),
        fake_gpu(1, "B", 20 * kGb),
        fake_gpu(2, "C", 10 * kGb),
    };
    ModelFit fit;
    fit.units.assign(48, kGb / 2);          // 48 half-gigabyte layers
    fit.units.push_back(kGb / 4);           // plus a smaller output unit
    for (const std::uint64_t unit : fit.units) {
        fit.resident += unit;
    }

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {1, 0, 2}, 0, fit);
    CHECK(!plan.split.empty());
    CHECK_EQ(plan.units.size(), std::size_t{3});

    const std::vector<int> actual =
        llama_cpp_assignment(plan.split, static_cast<int>(fit.units.size()));
    CHECK_EQ(actual.size(), plan.units.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CHECK_EQ(actual[i], plan.units[i]);
    }
}

TEST(a_plan_lands_where_it_aimed_across_a_thousand_machines) {
    // The strong form of the test above, because one worked example is not
    // enough to trust arithmetic that has to land exactly on a boundary
    // llama.cpp computes in single precision from numbers of its own. A
    // thousand shapes of machine and model, and every one of them has to place
    // the layers this file said it would.
    std::mt19937 random(20240607);
    std::uniform_int_distribution<int> card_count(2, 5);
    std::uniform_int_distribution<int> card_gb(2, 32);
    std::uniform_int_distribution<int> unit_count(4, 120);
    std::uniform_int_distribution<int> unit_mb(40, 1200);

    for (int trial = 0; trial < 1000; ++trial) {
        std::vector<ComputeDevice> gpus;
        const int cards = card_count(random);
        for (int i = 0; i < cards; ++i) {
            gpus.push_back(fake_gpu(i, "card" + std::to_string(i),
                                    static_cast<std::uint64_t>(card_gb(random)) * kGb));
        }
        std::vector<int> order;
        for (int i = 0; i < cards; ++i) {
            order.push_back(i);
        }
        std::shuffle(order.begin(), order.end(), random);

        ModelFit fit;
        const int units = unit_count(random);
        for (int i = 0; i < units; ++i) {
            fit.units.push_back(static_cast<std::uint64_t>(unit_mb(random)) * 1024 * 1024);
            fit.resident += fit.units.back();
        }
        fit.per_card = static_cast<std::uint64_t>(unit_mb(random)) * 1024 * 1024 / 4;

        const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, order, 0, fit);
        if (plan.units.empty()) {
            continue;  // no memory to plan against; nothing is claimed
        }

        const std::vector<int> actual = llama_cpp_assignment(plan.split, units);
        for (std::size_t i = 0; i < plan.units.size(); ++i) {
            if (actual[i] != plan.units[i]) {
                CHECK_EQ(actual[i], plan.units[i]);
                return;  // one report is enough; a thousand would bury it
            }
        }
    }
}

TEST(a_card_the_priority_order_held_back_is_used_when_the_last_one_overflows) {
    // The greedy pass fills each card to its target and hands the leftovers to
    // whichever card is last in *index* order -- which is not the card the
    // priority order put last. So a card the order deliberately held short can
    // be sitting on spare capacity while another is handed more than it holds.
    //
    // Card 1 is ranked last, so priority gives it only what is left over; card 2
    // is last by index, so it receives the rounding slack. Without the
    // rebalance card 2 is overfilled and card 1 keeps room it was never asked
    // to give up.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "first",  4 * kGb),
        fake_gpu(1, "held",  16 * kGb),
        fake_gpu(2, "last",   4 * kGb),
    };
    ModelFit fit;
    fit.per_card = 0;
    for (int i = 0; i < 20; ++i) {
        fit.units.push_back(kGb);   // 20 GB in one-gigabyte layers
        fit.resident += kGb;
    }

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {0, 2, 1}, 0, fit);
    CHECK_EQ(plan.units.size(), std::size_t{3});

    for (const ComputeDevice& gpu : gpus) {
        const auto index = static_cast<std::size_t>(gpu.index);
        const auto held  = static_cast<std::uint64_t>(plan.units[index]) * kGb;
        CHECK(held <= usable_memory(gpu, kCardHeadroom));
    }
    int placed = 0;
    for (const int count : plan.units) {
        placed += count;
    }
    CHECK_EQ(placed, 20);
}

TEST(the_priority_card_takes_the_most_layers) {
    // Equal cards, so the only thing that can decide which carries more is the
    // order the user put them in.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 12 * kGb),
        fake_gpu(1, "B", 12 * kGb),
        fake_gpu(2, "C", 12 * kGb),
    };
    ModelFit fit;
    fit.units.assign(30, kGb);
    fit.resident = 30 * kGb;

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {2, 0, 1}, 0, fit);
    CHECK_EQ(plan.units.size(), std::size_t{3});
    CHECK(plan.units[2] >= plan.units[0]);
    CHECK(plan.units[2] >= plan.units[1]);
}

TEST(a_model_that_fits_on_the_first_card_never_leaves_it) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 12 * kGb),
        fake_gpu(1, "B", 12 * kGb),
    };
    ModelFit fit;
    fit.units.assign(8, kGb / 4);
    fit.resident = 2 * kGb;

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {1, 0}, 0, fit);
    CHECK_EQ(plan.units[0], 0);
    CHECK_EQ(plan.units[1], 8);
    // And llama.cpp agrees.
    const std::vector<int> actual = llama_cpp_assignment(plan.split, 8);
    CHECK_EQ(actual[0], 0);
    CHECK_EQ(actual[1], 8);
}

TEST(a_setting_the_hardware_cannot_honour_says_so) {
    // The failure this exists for is silent: a split mode saved on a machine
    // with one card reads as configured, saves, and does nothing.
    ComputeDevice cpu;
    cpu.index   = 3;
    cpu.name    = "CPU";
    cpu.backend = "CPU";
    cpu.is_gpu  = false;

    // Nothing but a processor.
    {
        const GpuSettingSupport support = gpu_setting_support({cpu});
        CHECK(!support.split.empty());
        CHECK(!support.gpu_only.empty());
        CHECK(!support.vram_only.empty());
        // The reason has to name the way out, or a dimmed row is just a dimmed
        // row.
        CHECK(support.split.find("Runtimes") != std::string::npos);
    }

    // One card. Dividing a model between cards is meaningless; keeping the work
    // off the processor and refusing an oversized model are not.
    {
        ComputeDevice one = fake_gpu(0, "RTX 4070", 12 * kGb);
        one.backend       = "CUDA";
        const GpuSettingSupport support = gpu_setting_support({one, cpu});
        CHECK(!support.split.empty());
        CHECK(support.split.find("CUDA") != std::string::npos);
        CHECK(support.gpu_only.empty());
        CHECK(support.vram_only.empty());
    }

    // Two cards on a backend that spreads a model: everything works.
    {
        ComputeDevice a = fake_gpu(0, "RTX 4070", 12 * kGb);
        ComputeDevice b = fake_gpu(1, "RTX 3060", 12 * kGb);
        a.backend = b.backend = "CUDA";
        const GpuSettingSupport support = gpu_setting_support({a, b, cpu});
        CHECK(support.split.empty());
        CHECK(support.gpu_only.empty());
        CHECK(support.vram_only.empty());
    }
}

TEST(a_backend_that_runs_one_device_cannot_be_asked_to_split) {
    // Metal is the case in hand: unified memory and one GPU, so there is
    // neither anything to divide nor anywhere to divide it to. Two devices
    // reported by such a backend would still not make a split possible.
    ComputeDevice a = fake_gpu(0, "Apple M3 Max", 48 * kGb);
    ComputeDevice b = fake_gpu(1, "Apple M3 Max", 48 * kGb);
    a.backend = b.backend = "MTL";

    const GpuSettingSupport support = gpu_setting_support({a, b});
    CHECK(!support.split.empty());
    CHECK(support.split.find("Metal") != std::string::npos);  // its name, not ggml's id
    // The memory settings are still real there: Metal reports a working set,
    // and going past it makes the machine swap.
    CHECK(support.gpu_only.empty());
    CHECK(support.vram_only.empty());
}

TEST(a_backend_that_reports_no_memory_cannot_refuse_a_model) {
    // vram_only compares a model against free device memory. A backend that
    // reports none makes it a setting that saves and then does nothing, which
    // is the state this whole mechanism exists to make visible.
    ComputeDevice a = fake_gpu(0, "some GPU", 0);
    ComputeDevice b = fake_gpu(1, "some GPU", 0);
    a.backend = b.backend = "Vulkan";
    a.memory_free = b.memory_free = 0;

    const GpuSettingSupport support = gpu_setting_support({a, b});
    CHECK(!support.vram_only.empty());
    CHECK(support.vram_only.find("Vulkan") != std::string::npos);
    // Two cards on a splitting backend, so this one is still fine.
    CHECK(support.split.empty());
    CHECK(support.gpu_only.empty());
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
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/", *shipped());
    CHECK(!matches.empty());
    CHECK(matches.size() == ui::all_commands(*shipped()).size());
}

TEST(typing_narrows_the_list_to_what_still_matches) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/re", *shipped());
    CHECK(matches.size() == 2);
    CHECK(matches[0].name == "resume");
    CHECK(matches[1].name == "release");
}

TEST(the_experts_are_completable_too) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/phy", *shipped());
    CHECK(matches.size() == 1);
    CHECK(matches[0].name == "physics");
    // An expert takes a prompt after it, so completing one leaves the cursor
    // ready to type rather than up against the end of the word.
    CHECK(ui::command_completion("/phy", matches[0]) == "sics ");
}

TEST(a_command_that_is_already_complete_is_not_suggested_back) {
    // Offering to complete "/help" to "/help" is noise, and it would put a
    // menu over the transcript for every finished command.
    CHECK(ui::command_matches("/help", *shipped()).empty());
}

TEST(nothing_is_offered_once_the_command_is_settled) {
    // A space means the user has moved on to the argument, and the menu gets
    // out of the way. This is over-determined -- typed_word() bails on a space
    // and no command name contains one, so prefix matching would reject these
    // anyway. Kept because it is the behaviour a reader wants pinned, not
    // because either mechanism alone is in doubt.
    CHECK(ui::command_matches("/physics why is the sky blue", *shipped()).empty());
    CHECK(ui::command_matches("/help ", *shipped()).empty());
    CHECK(ui::command_matches("/re sume", *shipped()).empty());
}

TEST(ordinary_text_is_not_a_command) {
    CHECK(ui::command_matches("hello", *shipped()).empty());
    CHECK(ui::command_matches("", *shipped()).empty());
    CHECK(ui::command_matches("what about /help", *shipped()).empty());
}

TEST(completion_is_case_insensitive_like_the_commands_themselves) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/RES", *shipped());
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
    for (const ui::CommandInfo& command : ui::all_commands(*shipped())) {
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
    for (const auto& [id, expert] : config.experts) {
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
// ggml opens a backend module by an exact file name, so Crucible's idea of what
// one is called has to agree with ggml's own down to the character.
// ---------------------------------------------------------------------------

TEST(a_conversation_only_re_reads_what_actually_changed) {
    using detail::reusable_prefix;

    // The ordinary case: the cache holds a prompt and the reply that followed
    // it, and the next turn is all of that plus a new question. Everything up
    // to the new question is already there.
    const std::vector<llama_token> after_turn_one{1, 2, 3, 4, 5};
    const std::vector<llama_token> turn_two{1, 2, 3, 4, 5, 6, 7};
    CHECK_EQ(reusable_prefix(after_turn_one, turn_two), std::size_t{5});

    // A different expert, or an edited history: nothing in common, so nothing
    // is kept.
    CHECK_EQ(reusable_prefix({1, 2, 3}, {9, 8, 7}), std::size_t{0});
    CHECK_EQ(reusable_prefix({}, {1, 2, 3}), std::size_t{0});

    // Diverging part way through keeps only what matched.
    CHECK_EQ(reusable_prefix({1, 2, 3, 4}, {1, 2, 9, 4}), std::size_t{2});

    // The cache holding more than the prompt asks for -- the reply is still in
    // there -- keeps the part that matches and no more.
    CHECK_EQ(reusable_prefix({1, 2, 3, 4, 5}, {1, 2, 3}), std::size_t{2});
}

TEST(a_prompt_is_never_reused_in_its_entirety) {
    using detail::reusable_prefix;

    // Sending the same prompt again is what pressing enter on an unchanged
    // line does. Reusing every token of it would leave llama_decode nothing to
    // decode and the sampler no logits to read, so one token is always held
    // back to be read again.
    CHECK_EQ(reusable_prefix({1, 2, 3}, {1, 2, 3}), std::size_t{2});
    CHECK_EQ(reusable_prefix({1}, {1}), std::size_t{0});
    CHECK_EQ(reusable_prefix({1, 2, 3, 4}, {1, 2, 3}), std::size_t{2});
}

TEST(cuda_targets_the_cards_that_are_there_and_nothing_else) {
    // This machine: an Ampere, an Ada and a Blackwell card, against a CUDA 12.0
    // toolkit that stops at Hopper. Real code for the two it can compile for,
    // and Hopper PTX for the Blackwell card to be compiled by the driver.
    const std::vector<int> toolkit_12_0{50, 52, 60, 61, 70, 75, 80, 86, 87, 89, 90};
    CHECK_EQ(cuda_architectures({89, 120, 86}, toolkit_12_0), "86-real;89-real;90-virtual");

    // One card, and a toolkit that knows it: one real architecture, and PTX at
    // the same level so a card added later still runs.
    CHECK_EQ(cuda_architectures({86}, toolkit_12_0), "86-real;86-virtual");

    // Duplicates are the ordinary case -- two identical cards -- and must not
    // produce the architecture twice.
    CHECK_EQ(cuda_architectures({86, 86, 89}, toolkit_12_0), "86-real;89-real;89-virtual");
}

TEST(cuda_architectures_declines_rather_than_guessing) {
    const std::vector<int> toolkit{75, 80, 86, 89, 90};

    // No driver, or no compiler: an empty answer means "use llama.cpp's
    // defaults", which is slow but always correct. Anything else here would be
    // a module the machine cannot run.
    CHECK(cuda_architectures({}, toolkit).empty());
    CHECK(cuda_architectures({86}, {}).empty());

    // A card older than the toolkit supports has nothing that can be emitted
    // for it, and inventing an architecture would not help.
    CHECK(cuda_architectures({61}, toolkit).empty());
}

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

TEST(a_tensor_table_says_which_blocks_actually_cache) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "hybrid-tensors.gguf";
    // 12 blocks, every fourth one attending -- the shape of a qwen3next, whose
    // header states one KV head count for all 48 blocks when only 12 have a
    // cache at all. Reading the tensors is what tells them apart.
    write_test_gguf_with_tensors(file, /*layers=*/12, /*attention_every=*/4,
                                 /*embd=*/64, /*kv_width=*/16, /*vocab=*/128);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.layers, std::uint32_t{12});
    // 12 blocks plus the output.
    CHECK_EQ(shape.units.size(), std::size_t{13});

    int caching = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        if (shape.units[i].kv_per_token > 0) {
            ++caching;
            // K and V are 16 wide each, two bytes an element.
            CHECK_EQ(shape.units[i].kv_per_token, std::uint64_t{(16 + 16) * 2});
            CHECK_EQ(shape.units[i].state, std::uint64_t{0});
        } else {
            CHECK(shape.units[i].state > 0);  // recurrent instead
        }
    }
    CHECK_EQ(caching, 3);
    CHECK_EQ(shape.kv_per_token, std::uint64_t{3 * (16 + 16) * 2});
    // The output unit holds output.weight, and caches nothing.
    CHECK(shape.units.back().weights > 0);
    CHECK_EQ(shape.units.back().kv_per_token, std::uint64_t{0});
}

TEST(the_input_embedding_is_not_charged_to_a_card) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "embedding.gguf";
    write_test_gguf_with_tensors(file, /*layers=*/4, /*attention_every=*/1,
                                 /*embd=*/64, /*kv_width=*/16, /*vocab=*/1024);

    const ModelShape shape = read_model_shape(file);
    // llama.cpp keeps the input layer in system memory whatever the offload
    // settings say, so counting it against video memory refuses models that fit.
    CHECK_EQ(shape.host_weights, std::uint64_t{64} * 1024 * sizeof(float));

    // And it is not also counted among the units, which is what a card is
    // asked to divide. The output projection is the same size and *is* placed,
    // so the two are only distinguishable by name.
    std::uint64_t placed = 0;
    for (const ModelUnit& unit : shape.units) {
        placed += unit.weights;
    }
    CHECK_EQ(shape.units.back().weights,
             shape.host_weights + std::uint64_t{64} * sizeof(float));  // output + its norm
    CHECK(placed > shape.host_weights);
}

TEST(a_header_without_a_tensor_table_still_reports_its_cache) {
    // The metadata-only fixture: nothing to place layer by layer, so `units`
    // stays empty and the split falls back to dividing proportionally -- but
    // the cache is still worth knowing, and the header alone gives it.
    TempDir dir;
    const std::filesystem::path file = dir.path() / "bare.gguf";
    write_test_gguf(file, /*layers=*/4, /*embd=*/64, /*heads=*/4,
                    /*kv_heads=*/{2}, /*key_len=*/16, /*vocab=*/128);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK(shape.units.empty());
    CHECK_EQ(shape.kv_per_token, std::uint64_t{4} * 2 * (16 + 16) * 2);
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
    turns[1].route = RouteDecision{"mathematics", 0.9F, RouteSource::Model, "picked"};

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
    CHECK_EQ(loaded[1].route->expert, ExpertId("mathematics"));
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

    // A second run of Crucible resumes it. Its tokens are already in the total.
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
    CHECK(child.start({"crucible-no-such-program"}, {}, {}, error));

    std::string line;
    while (child.read_line(line)) {
        // drain
    }
    // execvp failed in the child, which exits 127 the way a shell would.
    CHECK_EQ(child.wait(), 127);
}

TEST(on_path_finds_real_programs_and_not_invented_ones) {
    CHECK(util::on_path("sh"));
    CHECK(!util::on_path("crucible-definitely-not-a-program"));
    // An empty requirement means "nothing needed", which is how a backend with
    // no SDK says so.
    CHECK(util::on_path(""));
}

int main() {
    std::cout << "Crucible core tests\n\n";
    return harness::run_all();
}
