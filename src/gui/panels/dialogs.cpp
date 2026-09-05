// SPDX-License-Identifier: MIT
//
// The three modals: new expert, open project, and the folder-trust question.
//
// A modal is used where the answer changes what the rest of the window means
// -- a different project is a different history, journal and workshop root --
// and nowhere else.
//
// The trust dialog is the one that matters: it asks before Crucible is allowed
// to read and write a directory, and it goes through the same store the
// terminal program uses, so a directory trusted in one face is trusted in the
// other.
#include "../app.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <filesystem>
#include <system_error>

#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_new_expert_modal() {
    if (expert_modal_open_) {
        ImGui::OpenPopup("New expert");
        expert_modal_open_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(em(30.0F), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New expert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // Two boxes and nothing else. The id, the chip, the keyword set and the
    // worked examples the delegator routes on are all derived or generated,
    // because those are things a person should not have to invent.
    wrapped(theme::kTextDim,
            "A name, and what it is trained in. Crucible works out the rest, and the "
            "delegator writes its own example questions once it is loaded.");
    ImGui::Dummy(ImVec2(0, em(0.5F)));

    text_coloured(theme::kTextFaint, "Expert name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", "Rust Async, Tax Law, Kubernetes", &new_expert_name_);

    ImGui::Dummy(ImVec2(0, em(0.4F)));
    text_coloured(theme::kTextFaint, "Describe what the expert is trained in");
    ImGui::InputTextMultiline("##blurb", &new_expert_blurb_, ImVec2(-FLT_MIN, em(5.2F)));
    text_coloured(theme::kTextFaint,
                  "The delegator routes on this, so name the things it should take.");

    if (!expert_error_.empty()) {
        ImGui::Dummy(ImVec2(0, em(0.4F)));
        wrapped(theme::kError, expert_error_);
    }

    ImGui::Dummy(ImVec2(0, em(0.6F)));
    if (ImGui::Button("Add expert", ImVec2(em(8.0F), 0))) {
        Expert expert;
        expert.name  = format::trim(new_expert_name_);
        expert.blurb = format::trim(new_expert_blurb_);

        Config      edited = config_;
        std::string error;
        if (!edited.roster.add(expert, error)) {
            // The dialog stays open with what was typed still in it: a name
            // collision is fixed by editing the name, not by typing the
            // description again.
            expert_error_ = error;
        } else {
            const ExpertId id = make_expert_id(expert.name);
            edited.experts[id] = ModelParams{};
            update_config([&edited](Config& config) { config = edited; });
            engine_->write_examples(id);
            say(expert.name + " has joined the experts");
            new_expert_name_.clear();
            new_expert_blurb_.clear();
            expert_error_.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        expert_error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_browse_modal() {
    const bool for_models = browse_for_ == BrowseFor::ModelsDir;
    const char* const kTitle = "Choose a folder";

    if (browse_modal_open_) {
        ImGui::OpenPopup(kTitle);
        browse_modal_open_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(em(38.0F), em(30.0F)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    text_coloured(theme::kTextDim, "%s", for_models
        ? "Where your GGUF files are. Crucible reads this directory; it never "
          "writes to it and never downloads into it."
        : "The directory Crucible will work in: its history, its cook journal "
          "and the root the workshop is confined to.");

    // A browser rather than a native file dialog. Crucible has no toolkit to
    // ask for one, and a directory list is what this needs anyway: you are
    // choosing a folder to work in, not a file to load.
    ImGui::SetNextItemWidth(-em(9.0F));
    if (ImGui::InputText("##path", &browse_text_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::filesystem::path typed = paths::expand_user(browse_text_);
        std::error_code ec;
        if (std::filesystem::is_directory(typed, ec)) {
            browse_ = typed;
            project_error_.clear();
        } else {
            project_error_ = browse_text_ + " is not a directory";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Up", ImVec2(em(3.2F), 0)) && browse_.has_parent_path()) {
        browse_      = browse_.parent_path();
        browse_text_ = browse_.string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Home", ImVec2(-FLT_MIN, 0))) {
        browse_      = paths::expand_user("~");
        browse_text_ = browse_.string();
    }

    text_coloured(theme::kTextFaint, "%s", browse_.string().c_str());

    ImGui::BeginChild("dirs", ImVec2(0, em(15.0F)), ImGuiChildFlags_Borders);
    for (const std::filesystem::path& entry : subdirectories(browse_)) {
        ImGui::PushID(entry.c_str());
        if (ImGui::Selectable((entry.filename().string() + "/").c_str())) {
            browse_      = entry;
            browse_text_ = browse_.string();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Creating a folder here rather than sending someone to a file manager:
    // "start a new project" is the other half of "open one", and both are the
    // same question about the same directory.
    ImGui::SetNextItemWidth(-em(9.0F));
    ImGui::InputTextWithHint("##new-folder", "new folder name", &new_folder_);
    ImGui::SameLine();
    if (ImGui::Button("Create", ImVec2(-FLT_MIN, 0)) && !format::trim(new_folder_).empty()) {
        const std::filesystem::path made = browse_ / format::trim(new_folder_);
        std::error_code ec;
        std::filesystem::create_directories(made, ec);
        if (ec) {
            project_error_ = "could not create " + made.string() + ": " + ec.message();
        } else {
            browse_      = made;
            browse_text_ = made.string();
            new_folder_.clear();
            project_error_.clear();
        }
    }

    if (!project_error_.empty()) {
        wrapped(theme::kError, project_error_);
    }

    ImGui::Separator();
    if (ImGui::Button(for_models ? "Use this folder" : "Open this folder",
                      ImVec2(em(12.0F), 0))) {
        const std::filesystem::path chosen = browse_;
        ImGui::CloseCurrentPopup();
        if (for_models) {
            update_config([&chosen](Config& config) {
                config.models_dir = chosen.string();
            });
            refresh_models();
            say("models directory: " + chosen.string() + " -- "
                + std::to_string(models_.size()) + " GGUF files");
        } else {
            open_project(chosen);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        project_error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_trust_modal() {
    if (pending_trust_ && !ImGui::IsPopupOpen("Trust this folder?")) {
        ImGui::OpenPopup("Trust this folder?");
    }

    ImGui::SetNextWindowSize(ImVec2(em(32.0F), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Trust this folder?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!pending_trust_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    wrapped(theme::kText, pending_trust_->string());
    ImGui::Dummy(ImVec2(0, em(0.4F)));
    wrapped(theme::kTextDim,
            "Crucible will keep this folder's history, and -- if the workshop is on -- "
            "read, write and run things inside it while cooking. It never touches "
            "anything outside it.");
    ImGui::Dummy(ImVec2(0, em(0.6F)));

    if (ImGui::Button("Trust and open", ImVec2(em(11.0F), 0))) {
        const std::filesystem::path root = *pending_trust_;
        pending_trust_.reset();
        trust_.trust(root);
        ImGui::CloseCurrentPopup();
        open_project(root);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em(5.6F), 0))) {
        // Declining the folder the window opened in is different from declining
        // one picked from the sidebar: there is no trusted project underneath to
        // fall back to, so cancelling would leave the user sitting in a
        // directory they just refused. Offer the picker instead of nothing.
        const bool was_the_open_project = *pending_trust_ == store_->project().root;
        pending_trust_.reset();
        ImGui::CloseCurrentPopup();
        if (was_the_open_project) {
            say("not trusted -- choose a folder to work in");
            open_browse(BrowseFor::Project);
        }
    }
    ImGui::EndPopup();
}

}  // namespace crucible::gui
