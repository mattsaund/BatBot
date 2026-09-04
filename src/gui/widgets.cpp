// SPDX-License-Identifier: MIT
#include "widgets.hpp"

#include <algorithm>
#include <cstdarg>
#include <system_error>

#include "theme.hpp"

namespace crucible::gui {

float em(float n) {
    return n * ImGui::GetFontSize();
}

void text_coloured(ImU32 colour, const char* fmt, ...) IM_FMTARGS(2);
void text_coloured(ImU32 colour, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(colour));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void wrapped(ImU32 colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(colour));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void section(const char* label) {
    ImGui::Dummy(ImVec2(0, em(0.5F)));
    text_coloured(theme::kTextFaint, "%s", label);
    ImGui::Dummy(ImVec2(0, em(0.1F)));
}

void title(const char* label) {
    ImGui::PushFont(theme::heading());
    text_coloured(theme::kFlame, "%s", label);
    ImGui::PopFont();
}

std::string model_label(const std::string& reference) {
    if (reference.empty()) {
        return "(none)";
    }
    const std::size_t slash = reference.find_last_of("/\\");
    return slash == std::string::npos ? reference : reference.substr(slash + 1);
}

std::string tail_of(const std::filesystem::path& path, std::size_t width) {
    const std::string text = path.string();
    if (text.size() <= width) {
        return text;
    }
    return "..." + text.substr(text.size() - width);
}

theme::Dot dot_for(SeatPhase phase) {
    switch (phase) {
        case SeatPhase::Active:       return theme::Dot::Active;
        case SeatPhase::Loading:      return theme::Dot::Loading;
        case SeatPhase::Dormant:      return theme::Dot::Ready;
        case SeatPhase::Missing:      return theme::Dot::Missing;
        case SeatPhase::Unconfigured: break;
    }
    return theme::Dot::Empty;
}

const char* mood_text(Mood mood) {
    switch (mood) {
        case Mood::Routing:  return "routing";
        case Mood::Loading:  return "loading";
        case Mood::Thinking: return "thinking";
        case Mood::Talking:  return "answering";
        case Mood::Error:    return "error";
        case Mood::Idle:     break;
    }
    return "idle";
}

ImU32 step_colour(const CookStep& step) {
    if (!step.ok) {
        return theme::kError;
    }
    if (step.kind == "write") {
        return theme::kAdded;
    }
    if (step.kind == "run" || step.kind == "done" || step.kind == "handoff") {
        return theme::kFlame;
    }
    return theme::kTextFaint;
}

std::vector<std::filesystem::path> subdirectories(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;  // a project picker full of .cache and .git helps nobody
        }
        found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace crucible::gui
