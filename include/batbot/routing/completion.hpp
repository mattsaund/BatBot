// SPDX-License-Identifier: MIT
//
// Slash-command completion.
//
// Typing "/" opens a list of what could follow it, narrowing as more is typed,
// with the rest of the best match shown in grey after the cursor. Tab takes it.
//
// This file is the single list of commands BatBot has: /help prints it and the
// completion menu offers it, so a command cannot exist in one and not the
// other.
//
// It lives beside the router rather than under ui/ because there is no terminal
// anywhere in it -- matching a prefix against a list of names is a fact about
// strings. That is also what lets the unit tests reach it: they link the core
// library, which does not drag FTXUI along.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace batbot::ui {

/// One entry in the menu.
struct CommandInfo {
    std::string name;     ///< without the slash: "resume"
    std::string summary;  ///< the line /help prints beside it
    /// True for the nine subjects plus Fallback, which take a prompt after the
    /// command rather than acting on their own.
    bool takes_prompt = false;
};

/// Every command, in the order /help lists them: the built-ins first, then one
/// per expert seat.
const std::vector<CommandInfo>& all_commands();

/// The commands `input` could still become.
///
/// Empty unless `input` is a slash followed by a partial word and nothing
/// else. "/re" matches; "/resume " does not, because the command is settled
/// and what follows is an argument; "hello" is not a command at all. A bare
/// "/" matches everything, which is what opens the menu.
std::vector<CommandInfo> command_matches(std::string_view input);

/// What Tab would add to `input` to complete it to `choice`, including the
/// trailing space for a command that takes a prompt. Empty when there is
/// nothing to add.
///
/// Returns only the *added* text, not the whole line, so the caller appends
/// rather than replaces -- which is also exactly what is drawn in grey.
std::string command_completion(std::string_view input, const CommandInfo& choice);

}  // namespace batbot::ui
