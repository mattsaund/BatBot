// SPDX-License-Identifier: MIT
//
// The workshop: what an expert can do to a project rather than say about it.
//
// Reading a file, writing one, listing a directory, running a command, looking
// something up, and asking the user a question. Together with the cook loop
// these are what make Crucible able to work on a project over an hour instead of
// answering one question about it.
//
// Three things are load-bearing here and none of them is the tool list.
//
// The first is that every path is resolved inside one root and anything that
// escapes it is refused. An expert that can write outside the project is not a
// coding assistant, it is a remote shell, and the difference has to be
// structural rather than a matter of the model behaving.
//
// The second is that this is off until it is switched on, and the folder has to
// have been trusted. `crucible` is meant to be run by cd-ing into a project, and
// the trust gate has always said it would eventually read and write files
// there. This is that.
//
// The third is that the protocol is text. llama.cpp applies a chat template, it
// does not negotiate a tool schema, and Crucible cannot know which model is in
// the seat -- a convention every model can follow beats one that only the
// tool-trained ones can. This is the same reasoning, and the same shape, as the
// `SEARCH:` line in web_search.hpp.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/llm/model_host.hpp"
#include "crucible/tools/web_search.hpp"

namespace crucible::tools {

/// What an expert asked to do.
enum class ToolKind {
    None,
    List,    ///< LIST: <dir>
    Read,    ///< READ: <file>
    Write,   ///< WRITE: <file>, followed by a block
    Run,     ///< RUN: <command>
    Search,  ///< SEARCH: <query>
    Ask,     ///< ASK: <question>   -- pauses the cook for an answer
    Note,    ///< NOTE: <what it is doing>, journalled and shown, no effect
    Done,    ///< DONE: <summary>   -- this iteration is finished
};

std::string_view tool_kind_name(ToolKind kind);

struct ToolCall {
    ToolKind    kind = ToolKind::None;
    /// The path, command, query, question or summary on the verb's line.
    std::string argument;
    /// The body of a WRITE. Empty for every other verb.
    std::string content;
};

/// What happened, in the two registers it has to be reported in: `output` goes
/// back to the model, `summary` goes on screen and into the journal.
struct ToolResult {
    bool        ok = false;
    std::string output;
    std::string summary;
    /// Paths this call changed, relative to the root. The journal's record of
    /// what a cook actually did to the project.
    std::vector<std::string> changed;
};

struct WorkshopSettings {
    /// Off until switched on, like web search and for the same reason.
    bool enabled = false;

    /// Every path is resolved inside this and anything that escapes is refused.
    /// Normally the project directory the user started Crucible in.
    std::filesystem::path root;

    /// Whether RUN is available. Reading and writing a project you already
    /// trusted is one decision; executing arbitrary commands in it is another,
    /// and some people will want the first without the second.
    bool allow_run = true;

    /// How long a single command may take before it is killed. A build is
    /// minutes; a command that has not finished in this long is stuck, and a
    /// cook that waits forever on it has stopped cooking.
    int run_timeout_seconds = 120;

    /// How much of a command's output comes back to the model.
    ///
    /// A test suite can print a megabyte, and feeding that to an expert with an
    /// 8k context does not fail loudly -- it silently pushes the actual task out
    /// of the window. Truncation keeps the head and the tail, because a
    /// compiler puts the first error at the top and the summary at the bottom.
    std::size_t max_output_bytes = 6000;

    /// How much of a file a READ returns, for the same reason.
    std::size_t max_read_bytes = 32000;

    /// How many entries a LIST returns.
    std::size_t max_entries = 200;
};

/// The first tool call in a reply, or nothing when the expert is just talking.
///
/// `reasoning` is consulted only when `answer` has none, which is the same rule
/// web_search follows: a model that has written something for the user is
/// answering, not calling a tool, and a programming expert's reply about a
/// shell command is not a request to run it.
std::optional<ToolCall> parse_tool_call(std::string_view answer, std::string_view reasoning);

/// The verb a reply was reaching for but wrote wrongly, or None.
///
/// A line that opens with WRITE, RUN, ASK or DONE and no colon is a tool call
/// with a typo in it, not prose -- but it cannot be executed, because the
/// argument is not reliably an argument. `WRITE /path "fixed the bug"` means
/// the quoted text as a description, and obeying it would put the description
/// into the file instead of the code.
///
/// So it is detected rather than guessed at, and the caller answers with the
/// syntax. Returns None for a reply that was not trying to call anything.
ToolKind attempted_tool_call(std::string_view answer, std::string_view reasoning);

/// Resolve `relative` inside `root`.
///
/// Returns nothing when the result would be outside it -- an absolute path, a
/// `..` that climbs past the top, or a symlink pointing away. Symlinks are
/// resolved before the check rather than after, because a link is exactly how
/// you would get out of a directory that only compared strings.
///
/// The path does not have to exist: this is also how a WRITE decides where a
/// new file may go.
std::optional<std::filesystem::path> resolve_in_root(const std::filesystem::path& root,
                                                     std::string_view relative);

/// Keep the head and the tail of `text`, with a line in the middle saying what
/// was dropped. Returns `text` unchanged when it already fits.
std::string clamp_output(std::string_view text, std::size_t limit);

/// Do it.
///
/// Blocking -- a RUN can take the whole timeout -- so call it from the engine
/// thread. `cancel` is checked while a command runs, which is what makes
/// Ctrl-C during a cook stop the build rather than wait for it.
///
/// ToolKind::Ask and ToolKind::Done are not executed here: they are answers to
/// the cook loop rather than actions, and it handles them.
ToolResult run_tool(const ToolCall& call, const WorkshopSettings& settings,
                    const SearchSettings& search, const CancelCallback& cancel);

/// What to add to an expert's system prompt so it knows the workshop is there.
/// Lists only the verbs the settings actually allow.
std::string workshop_instructions(const WorkshopSettings& settings);

}  // namespace crucible::tools
