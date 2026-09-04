// SPDX-License-Identifier: MIT
//
// Drawing parsed markdown with Dear ImGui.
//
// util/markdown.hpp does the parsing, both faces share it, and this is the
// desktop half of the rendering -- ui/widgets/markdown_view.cpp is the
// terminal's.
//
// The work here is wrapping. ImGui::TextWrapped wraps one string in one font
// and one colour, which is no use for a paragraph whose bold run continues
// mid-sentence: drawing each span separately breaks the line at every style
// change instead of at the width. So the spans are flattened into words that
// each remember their own face, and the wrapping is done a word at a time.
//
// Word::space_after is why `**Bold**:` renders tight. A space between every
// pair of words is nearly right and wrong exactly where punctuation follows a
// styled run, which is where it is most noticeable.
#include "markdown_view.hpp"

#include <algorithm>
#include <vector>

#include "crucible/util/markdown.hpp"
#include "theme.hpp"

namespace crucible::gui {
namespace {

/// One word, with the style it inherited from its span.
///
/// Wrapping has to happen at this granularity rather than per span: a paragraph
/// is one span of prose, one of `code`, and one more of prose, and the line
/// break can fall anywhere in any of them.
struct Word {
    std::string text;
    bool        code   = false;
    bool        bold   = false;
    bool        italic = false;
    float       width  = 0.0F;
    bool        space_after = false;
};

ImFont* face_for(const Word& word) {
    if (word.bold) {
        return theme::bold();
    }
    if (word.italic) {
        return theme::italic();
    }
    return theme::body();
}

/// Split styled spans into measured words.
std::vector<Word> words_of(const std::vector<markdown::Span>& spans) {
    std::vector<Word> words;
    for (const markdown::Span& span : spans) {
        std::size_t i = 0;
        while (i < span.text.size()) {
            while (i < span.text.size() && span.text[i] == ' ') {
                ++i;  // leading spaces are carried by the previous word
                if (!words.empty()) {
                    words.back().space_after = true;
                }
            }
            const std::size_t start = i;
            while (i < span.text.size() && span.text[i] != ' ') {
                ++i;
            }
            if (i == start) {
                continue;
            }
            Word word;
            word.text   = span.text.substr(start, i - start);
            word.code   = span.code;
            word.bold   = span.bold;
            word.italic = span.italic;

            ImGui::PushFont(face_for(word));
            word.width = ImGui::CalcTextSize(word.text.c_str()).x;
            ImGui::PopFont();
            words.push_back(std::move(word));
        }
    }
    return words;
}

/// Draw one word at the cursor, with a background if it is inline code.
void draw_word(const Word& word, ImU32 colour) {
    ImGui::PushFont(face_for(word));
    if (word.code) {
        // Painted behind rather than around: an inline span cannot use a child
        // window without breaking the line it is part of.
        const ImVec2 at   = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::CalcTextSize(word.text.c_str());
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(at.x - 2.0F, at.y - 1.0F),
            ImVec2(at.x + size.x + 2.0F, at.y + size.y + 1.0F),
            theme::kRaised, 2.0F);
    }
    ImGui::PushStyleColor(ImGuiCol_Text,
                          theme::to_vec(word.code ? theme::kFlameBright : colour));
    ImGui::TextUnformatted(word.text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// Lay out `spans` as wrapped prose, indented by `indent` pixels.
void draw_spans(const std::vector<markdown::Span>& spans, ImU32 colour, float indent) {
    const std::vector<Word> words = words_of(spans);
    if (words.empty()) {
        ImGui::NewLine();
        return;
    }

    const float left      = ImGui::GetCursorPosX() + indent;
    const float available = ImGui::GetContentRegionAvail().x - indent;
    const float space     = ImGui::CalcTextSize(" ").x;

    float used  = 0.0F;
    bool  first = true;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const Word& word = words[i];

        // A space goes in only where the source had one. Adding one between
        // every pair renders `**Bold**: text` as "Bold : text", because the
        // colon is a separate word in a separate span and nothing said there
        // was whitespace between them.
        const bool  spaced  = !first && words[i - 1].space_after;
        const float advance = (spaced ? space : 0.0F) + word.width;

        if (!first && used + advance > available) {
            used  = 0.0F;
            first = true;
        }
        if (first) {
            ImGui::SetCursorPosX(left);
        } else {
            ImGui::SameLine(0.0F, spaced ? space : 0.0F);
        }
        draw_word(word, colour);
        used += advance;
        first = false;
    }
}

std::string joined(const std::vector<markdown::Span>& spans) {
    std::string out;
    for (const markdown::Span& span : spans) {
        out += span.text;
    }
    return out;
}

}  // namespace

void draw_code_block(std::string_view text, ImU32 tint) {
    if (text.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::to_vec(theme::kRaised));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

    // Sized to its content up to a cap, and scrolling horizontally rather than
    // wrapping: a wrapped line of code is a line that has silently changed
    // meaning, and alignment is half of what a diff communicates.
    const float line   = ImGui::GetTextLineHeightWithSpacing();
    const int   lines  = 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    const float height = std::min(line * static_cast<float>(std::min(lines, 24)) + 16.0F,
                                  line * 24.0F + 16.0F);

    ImGui::BeginChild(ImGui::GetID(text.data()), ImVec2(0, height),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(theme::body());

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view row =
            text.substr(start, end == std::string_view::npos ? end : end - start);

        // A diff colours itself: the marker in column one is the whole
        // vocabulary, and reading it here is cheaper than threading a type
        // through from whoever produced the text.
        ImU32 colour = tint != 0 ? tint : theme::kText;
        if (!row.empty() && tint == 0) {
            if (row.front() == '+') { colour = theme::kAdded; }
            else if (row.front() == '-') { colour = theme::kRemoved; }
            else if (row.front() == '@') { colour = theme::kFlame; }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(colour));
        ImGui::TextUnformatted(std::string(row).c_str());
        ImGui::PopStyleColor();

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void draw_markdown(std::string_view text, ImU32 base) {
    const std::vector<markdown::Block> blocks = markdown::parse(text);

    // Fenced code arrives a line at a time. Gathering the run back up means one
    // scrolling box per block rather than one per line, which is the difference
    // between a code block and a stack of them.
    std::string pending_code;
    const auto flush_code = [&pending_code]() {
        if (!pending_code.empty()) {
            draw_code_block(pending_code);
            pending_code.clear();
        }
    };

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const markdown::Block& block = blocks[i];
        if (block.kind != markdown::BlockKind::Code) {
            flush_code();
        }

        switch (block.kind) {
            case markdown::BlockKind::Code:
                if (!pending_code.empty()) {
                    pending_code += '\n';
                }
                pending_code += joined(block.spans);
                continue;

            case markdown::BlockKind::Blank:
                ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.35F));
                continue;

            case markdown::BlockKind::Rule:
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 4));
                continue;

            case markdown::BlockKind::Heading: {
                ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.35F));
                // Only the top two levels get the larger face. Below that the
                // step in size stops meaning anything and just makes a reply
                // look like a ransom note.
                ImGui::PushFont(block.level <= 2 ? theme::heading() : theme::bold());
                ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kFlame));
                ImGui::TextUnformatted(joined(block.spans).c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                continue;
            }

            case markdown::BlockKind::Quote: {
                // A bar down the left rather than an indent alone: a quote that
                // is only indented reads as a code block at a glance.
                const ImVec2 at = ImGui::GetCursorScreenPos();
                draw_spans(block.spans, theme::kTextDim, 14.0F);
                const ImVec2 after = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(at.x, at.y), ImVec2(at.x + 2.0F, after.y - 4.0F), theme::kFlame);
                continue;
            }

            case markdown::BlockKind::Bullet:
            case markdown::BlockKind::Numbered: {
                const float indent = 16.0F * static_cast<float>(block.level + 1);
                const std::string marker = block.kind == markdown::BlockKind::Bullet
                    ? std::string("\xE2\x80\xA2")  // •
                    : block.marker;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kFlame));
                ImGui::TextUnformatted(marker.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0F, ImGui::CalcTextSize(" ").x);
                draw_spans(block.spans, base, 0.0F);
                continue;
            }

            case markdown::BlockKind::TableRule:
                continue;  // the ruled line under a header carries no text

            case markdown::BlockKind::TableRow: {
                // Gather the whole run of rows, so one table is one ImGui table
                // rather than one per line.
                std::vector<const markdown::Block*> rows;
                std::size_t j = i;
                for (; j < blocks.size(); ++j) {
                    if (blocks[j].kind == markdown::BlockKind::TableRow) {
                        rows.push_back(&blocks[j]);
                    } else if (blocks[j].kind != markdown::BlockKind::TableRule) {
                        break;
                    }
                }
                i = j - 1;

                std::size_t columns = 0;
                for (const markdown::Block* row : rows) {
                    columns = std::max(columns, row->cells.size());
                }
                if (columns == 0) {
                    continue;
                }
                if (ImGui::BeginTable("##md", static_cast<int>(columns),
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                          | ImGuiTableFlags_SizingStretchProp)) {
                    for (std::size_t r = 0; r < rows.size(); ++r) {
                        ImGui::TableNextRow();
                        for (std::size_t c = 0; c < columns; ++c) {
                            ImGui::TableSetColumnIndex(static_cast<int>(c));
                            if (c >= rows[r]->cells.size()) {
                                continue;
                            }
                            // The first row is the header, which markdown marks
                            // by the ruled line under it rather than in the row
                            // itself.
                            ImGui::PushFont(r == 0 ? theme::bold() : theme::body());
                            ImGui::PushStyleColor(
                                ImGuiCol_Text,
                                theme::to_vec(r == 0 ? theme::kFlame : base));
                            ImGui::TextUnformatted(joined(rows[r]->cells[c]).c_str());
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                        }
                    }
                    ImGui::EndTable();
                }
                continue;
            }

            case markdown::BlockKind::Paragraph:
                break;
        }

        draw_spans(block.spans, base, 0.0F);
    }
    flush_code();
}

}  // namespace crucible::gui
