// SPDX-License-Identifier: MIT
//
// See response_filter.hpp for what this is for.
#include "batbot/llm/response_filter.hpp"

#include <algorithm>
#include <cassert>
#include <array>
#include <string_view>

namespace batbot {
namespace {

/// Every marker either convention uses, longest first so that scanning finds
/// the longest match at a position rather than a prefix of it.
///
/// `<|constrain|>` is in the list only to be swallowed: it introduces the type
/// of a tool call's arguments, and printing it would be exactly the noise this
/// file exists to remove.
constexpr std::array<std::string_view, 10> kMarkers{{
    "<|constrain|>",
    "<|channel|>",
    "<|message|>",
    "<|return|>",
    "</think>",
    "<|start|>",
    "<|think>",
    "<think>",
    "<|end|>",
    "<|call|>",
}};

/// The longest marker, which is how far back a partial one can reach.
constexpr std::size_t longest_marker() {
    std::size_t longest = 0;
    for (const std::string_view marker : kMarkers) {
        longest = std::max(longest, marker.size());
    }
    return longest;
}

/// The marker beginning at `text`, or an empty view if none does.
std::string_view marker_at(std::string_view text) {
    for (const std::string_view marker : kMarkers) {
        if (text.substr(0, marker.size()) == marker) {
            return marker;
        }
    }
    return {};
}

/// Could `tail` be the beginning of a marker that has not finished arriving?
bool could_begin_marker(std::string_view tail) {
    if (tail.empty()) {
        return false;
    }
    return std::any_of(kMarkers.begin(), kMarkers.end(), [tail](std::string_view marker) {
        return marker.size() > tail.size() && marker.substr(0, tail.size()) == tail;
    });
}

/// Where the earliest possible marker starts, or npos.
///
/// A marker only ever starts with '<' in both conventions, so finding the next
/// one is a search for that character rather than a scan of the whole table at
/// every position.
std::size_t next_candidate(std::string_view text, std::size_t from) {
    return text.find('<', from);
}

}  // namespace

void ResponseFilter::drain(Piece& piece, bool final_chunk) {
    const auto emit = [this, &piece](std::string_view text) {
        if (text.empty()) {
            return;
        }
        switch (sink_) {
            case Sink::Answer:    piece.answer    += text; break;
            case Sink::Reasoning: piece.reasoning += text; break;
            case Sink::Discard:   break;
        }
    };

    std::size_t at = 0;
    while (at < buffer_.size()) {
        const std::string_view rest(buffer_.data() + at, buffer_.size() - at);
        const std::size_t candidate = next_candidate(rest, 0);

        // Everything up to the next '<' cannot be part of a marker.
        if (candidate == std::string_view::npos) {
            if (state_ == State::ChannelName) {
                channel_ += rest;
            } else {
                emit(rest);
            }
            at = buffer_.size();
            break;
        }
        if (candidate > 0) {
            if (state_ == State::ChannelName) {
                channel_ += rest.substr(0, candidate);
            } else {
                emit(rest.substr(0, candidate));
            }
            at += candidate;
            continue;
        }

        // At a '<'. Either a marker starts here, or one might once more text
        // arrives, or it is just a less-than sign in the answer.
        const std::string_view here(buffer_.data() + at, buffer_.size() - at);
        const std::string_view marker = marker_at(here);
        if (marker.empty()) {
            if (!final_chunk && could_begin_marker(here)) {
                break;  // hold it back; the next chunk decides
            }
            // A real '<'. Emit it and carry on past it, so the search for the
            // next candidate does not find this one again.
            if (state_ == State::ChannelName) {
                channel_ += '<';
            } else {
                emit(here.substr(0, 1));
            }
            at += 1;
            continue;
        }

        at += marker.size();

        if (marker == "<|channel|>") {
            channel_.clear();
            state_ = State::ChannelName;
        } else if (marker == "<|message|>") {
            // "final" is the answer; "analysis" and "commentary" are the model
            // working. A message with no channel at all is an answer -- that is
            // what a model using only part of the convention means by it.
            const bool final_channel = channel_.empty() || channel_ == "final";
            sink_  = final_channel ? Sink::Answer : Sink::Reasoning;
            state_ = State::Text;
        } else if (marker == "<|start|>" || marker == "<|end|>" || marker == "<|return|>" ||
                   marker == "<|call|>") {
            // Between messages: what follows is a role name and a header, not
            // anything anybody wants to read.
            channel_.clear();
            sink_  = Sink::Discard;
            state_ = State::Text;
        } else if (marker == "<think>" || marker == "<|think>") {
            sink_  = Sink::Reasoning;
            state_ = State::Text;
        } else if (marker == "</think>") {
            sink_  = Sink::Answer;
            state_ = State::Text;
        }
        // <|constrain|> falls through: swallowed, nothing else changes.
    }

    buffer_.erase(0, at);
}

ResponseFilter::Piece ResponseFilter::feed(std::string_view chunk) {
    Piece piece;
    buffer_ += chunk;
    drain(piece, /*final_chunk=*/false);
    // What is held back is only ever a partial marker, which is shorter than
    // the longest marker by definition -- `could_begin_marker` says so. So the
    // buffer cannot grow with the reply, and a paragraph full of less-than
    // signs is emitted as it arrives rather than accumulating to the end.
    assert(buffer_.size() < longest_marker());
    return piece;
}

ResponseFilter::Piece ResponseFilter::flush() {
    Piece piece;
    drain(piece, /*final_chunk=*/true);
    buffer_.clear();
    return piece;
}

}  // namespace batbot
