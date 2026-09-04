// SPDX-License-Identifier: MIT
//
// The left column: the mark, the project, recent projects, the experts and
// the view switcher.
//
// Everything in it is either navigation or a statement of where you are. It
// holds no state of its own -- the width and whether it is open live on App,
// because they are how this window is arranged rather than a preference.
#include "../app.hpp"

#include <imgui.h>
#include <algorithm>

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_sidebar(const Snapshot& snapshot) {
    if (!sidebar_open_) {
        // Collapsed: a strip wide enough for the mark and the button that
        // brings it back. Hiding it entirely would leave nothing to click.
        // Wide enough for the button's own label plus its frame padding. At
        // em(2.8) the arrow was clipped to an empty box, which is a button
        // nobody would think to press.
        ImGui::BeginChild("rail", ImVec2(em(3.6F), 0), ImGuiChildFlags_Borders);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        theme::draw_flame(ImGui::GetWindowDrawList(),
                          ImVec2(origin.x + em(1.0F), origin.y + em(0.9F)), em(0.8F));
        ImGui::Dummy(ImVec2(0, em(2.2F)));
        if (ImGui::Button(">", ImVec2(-FLT_MIN, 0))) {
            sidebar_open_ = true;
        }
        ImGui::SetItemTooltip("Show the sidebar");
        ImGui::EndChild();
        return;
    }

    ImGui::BeginChild("sidebar", ImVec2(sidebar_width_, 0), ImGuiChildFlags_Borders);

    // --- the mark ---------------------------------------------------------
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        theme::draw_flame(ImGui::GetWindowDrawList(),
                          ImVec2(origin.x + em(0.9F), origin.y + em(1.0F)), em(0.95F));

        ImGui::SetCursorScreenPos(ImVec2(origin.x + em(2.4F), origin.y + em(0.15F)));
        ImGui::PushFont(theme::bold());
        text_coloured(theme::kText, "CRUCIBLE");
        ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + em(2.4F), origin.y + em(1.25F)));
        text_coloured(snapshot.busy ? theme::kFlameBright : theme::kTextDim, "%s",
                      snapshot.status.empty() ? mood_text(snapshot.mood)
                                              : snapshot.status.c_str());

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + em(2.6F)));
    }
    ImGui::Separator();

    // --- the project ------------------------------------------------------
    //
    // At the top, because it is what everything else is about, and it is the
    // one question a window cannot answer for itself the way `cd` does.
    section("PROJECT");
    ImGui::PushFont(theme::bold());
    text_coloured(theme::kText, "%s", store_->project().name.c_str());
    ImGui::PopFont();
    text_coloured(theme::kTextFaint, "%s", tail_of(store_->project().root, 34).c_str());
    ImGui::SetItemTooltip("%s", store_->project().root.string().c_str());

    if (ImGui::Button("Open project", ImVec2(-FLT_MIN, 0))) {
        project_modal_open_ = true;
        browse_             = store_->project().root;
        browse_text_        = browse_.string();
        new_folder_.clear();
        project_error_.clear();
    }

    const std::vector<Project> recents = recent_projects(6);
    bool any_other = false;
    for (const Project& project : recents) {
        any_other = any_other || project.root != store_->project().root;
    }
    if (any_other) {
        section("RECENT");
        for (const Project& project : recents) {
            if (project.root == store_->project().root) {
                continue;
            }
            ImGui::PushID(project.root.c_str());
            if (ImGui::Selectable(project.name.c_str(), false, 0, ImVec2(0, em(1.3F)))) {
                open_project(project.root);
            }
            ImGui::SetItemTooltip("%s", project.root.string().c_str());
            ImGui::PopID();
        }
    }

    // --- the experts ---------------------------------------------------
    section("EXPERTS");
    const Roster& roster = snapshot.roster ? *snapshot.roster : config_.roster;
    if (roster.experts().empty()) {
        wrapped(theme::kTextFaint, "None yet.");
    }
    for (std::size_t i = 0; i < roster.size(); ++i) {
        const Expert&    expert = roster.at(i);
        const SeatState& seat   = i < snapshot.seats.size() ? snapshot.seats[i] : SeatState{};
        const bool linked = snapshot.linked && *snapshot.linked == expert.id;

        const ImVec2 at = ImGui::GetCursorScreenPos();
        theme::draw_dot(ImGui::GetWindowDrawList(),
                        ImVec2(at.x + em(0.42F), at.y + em(0.55F)), em(0.30F),
                        dot_for(seat.phase));
        ImGui::Dummy(ImVec2(em(1.05F), em(1.1F)));
        ImGui::SameLine();

        std::string label = expert.name;
        if (seat.phase == SeatPhase::Loading) {
            label += "  " + std::to_string(static_cast<int>(seat.progress * 100.0F)) + "%";
        }
        const ImU32 colour = linked                                ? theme::kFlameBright
                           : seat.phase == SeatPhase::Unconfigured ? theme::kTextFaint
                                                                   : theme::kText;
        if (linked) {
            ImGui::PushFont(theme::bold());
        }
        text_coloured(colour, "%s", label.c_str());
        if (linked) {
            ImGui::PopFont();
        }
        if (ImGui::IsItemHovered() && !expert.blurb.empty()) {
            ImGui::SetTooltip("%s", expert.blurb.c_str());
        }
    }

    ImGui::Dummy(ImVec2(0, em(0.2F)));
    if (ImGui::Button("Manage experts", ImVec2(-FLT_MIN, 0))) {
        view_          = View::Settings;
        settings_page_ = SettingsPage::Experts;
    }

    // --- navigation --------------------------------------------------------
    section("VIEW");
    const auto nav = [this](const char* label, View view) {
        if (ImGui::Selectable(label, view_ == view, 0, ImVec2(0, em(1.5F)))) {
            view_ = view;
        }
    };
    // No "Experts" entry: the section above is called that now, and managing
    // them lives in Settings, where the rest of the configuration is. Two
    // things called Experts in one sidebar is a question the user should not
    // have to answer.
    nav("Chat",     View::Chat);
    nav("Cook",     View::Cook);
    nav("History",  View::History);
    nav("Settings", View::Settings);

    // --- the footer --------------------------------------------------------
    //
    // Pushed to the bottom by a filler rather than by an absolute cursor
    // position: the sidebar scrolls when the roster is long, and a footer
    // pinned to the window height ends up either overlapping the list or below
    // the visible area depending on how far it has scrolled.
    const TokenUsage& usage = snapshot.session_usage;
    const float footer = em(4.1F);
    const float slack  = ImGui::GetContentRegionAvail().y - footer;
    if (slack > 0.0F) {
        ImGui::Dummy(ImVec2(0, slack));
    }
    ImGui::Separator();
    text_coloured(theme::kTextFaint, "%s in / %s out",
                  format_tokens(usage.input_tokens).c_str(),
                  format_tokens(usage.output_tokens).c_str());
    if (ImGui::Button("<< Hide", ImVec2(-FLT_MIN, 0))) {
        sidebar_open_ = false;
    }

    ImGui::EndChild();
}

void App::draw_splitter() {
    ImGui::SameLine(0.0F, 0.0F);
    if (!sidebar_open_) {
        return;
    }

    // An invisible button dragged sideways. ImGui has no splitter widget, and
    // this is what one is: a thing that is hovered, held, and reports how far
    // the mouse moved while it was held.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::to_vec(theme::kFlame));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::to_vec(theme::kFlameBright));
    ImGui::Button("##splitter", ImVec2(em(0.35F), -FLT_MIN));
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
        sidebar_width_ += ImGui::GetIO().MouseDelta.x;
    }
    // Clamped so it can be neither squeezed into nothing nor dragged over the
    // whole window -- either of which leaves no way back without a mouse hunt.
    const float most = std::max(em(14.0F), ImGui::GetWindowWidth() * 0.45F);
    sidebar_width_ = std::clamp(sidebar_width_, em(12.0F), most);

    ImGui::SameLine(0.0F, 0.0F);
}

}  // namespace crucible::gui
