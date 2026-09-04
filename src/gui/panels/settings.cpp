// SPDX-License-Identifier: MIT
//
// The settings pages: General, Experts, Hardware, Tools, About.
//
// A list down the left and a page on the right, which is the shape every
// desktop application settles on because a single scrolling wall of switches
// cannot be navigated.
//
// Every change here goes through App::update_config, which writes the file and
// reconfigures the seats. Nothing on these pages edits the running state
// directly -- the config is the single place a setting lives, so the terminal
// program sees the same change on its next read.
#include "../app.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <filesystem>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/devices.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

void App::draw_settings() {
    // One list down the left and one page on the right. A single scrolling wall
    // of switches is what every desktop application starts with and none of
    // them keeps.
    ImGui::BeginChild("settings-nav", ImVec2(em(10.0F), 0), ImGuiChildFlags_Borders);
    const auto page = [this](const char* label, SettingsPage which) {
        if (ImGui::Selectable(label, settings_page_ == which, 0, ImVec2(0, em(1.5F)))) {
            settings_page_ = which;
        }
    };
    ImGui::Dummy(ImVec2(0, em(0.2F)));
    page("General",  SettingsPage::General);
    page("Experts",  SettingsPage::Experts);
    page("Hardware", SettingsPage::Hardware);
    page("Tools",    SettingsPage::Tools);
    page("About",    SettingsPage::About);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("settings-page", ImVec2(0, 0), ImGuiChildFlags_Borders);

    switch (settings_page_) {
        case SettingsPage::General: {
            title("General");

            section("DELEGATOR");
            wrapped(theme::kTextDim,
                    "A small model that reads your prompt and names the expert it "
                    "belongs to. It never answers; it only decides.");
            ImGui::SetNextItemWidth(em(20.0F));
            const bool open = ImGui::BeginCombo("Router model",
                                                model_label(config_.router.model).c_str());
            if (!config_.router.model.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", config_.router.path.c_str());
            }
            if (open) {
                if (ImGui::Selectable("(none) -- route on keywords instead",
                                      config_.router.model.empty())) {
                    update_config([](Config& config) { config.router.model.clear(); });
                }
                for (const ModelFile& file : models_) {
                    if (ImGui::Selectable((file.name + "   " + file.size_label()).c_str(),
                                          file.name == config_.router.model)) {
                        update_config([&file](Config& config) {
                            config.router.model = file.name;
                        });
                    }
                }
                ImGui::EndCombo();
            }

            bool keep = config_.routing.keep_delegator_loaded;
            if (ImGui::Checkbox("Keep the delegator in memory between prompts", &keep)) {
                update_config([keep](Config& config) {
                    config.routing.keep_delegator_loaded = keep;
                });
            }
            ImGui::SetItemTooltip(
                "Off frees it after each decision, leaving the expert the whole card.");

            float floor_value = config_.routing.min_confidence;
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::SliderFloat("Confidence floor", &floor_value, 0.0F, 1.0F, "%.2f")) {
                update_config([floor_value](Config& config) {
                    config.routing.min_confidence = floor_value;
                });
            }
            ImGui::SetItemTooltip(
                "Below this the delegator is treated as undecided. 0 disables the check.");

            section("MODELS");
            text_coloured(theme::kTextDim, "%s",
                          config_.resolved_models_dir().string().c_str());
            std::string dir = config_.models_dir;
            ImGui::SetNextItemWidth(-em(6.5F));
            if (ImGui::InputTextWithHint("##models-dir", "path to your GGUF files", &dir,
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                update_config([&dir](Config& config) { config.models_dir = dir; });
                refresh_models();
            }
            ImGui::SameLine();
            if (ImGui::Button("Rescan", ImVec2(-FLT_MIN, 0))) {
                refresh_models();
                say("found " + std::to_string(models_.size()) + " GGUF files");
            }

            section("APPEARANCE");
            bool reasoning = config_.ui.show_reasoning;
            if (ImGui::Checkbox("Keep a thinking model's working on screen", &reasoning)) {
                update_config([reasoning](Config& config) {
                    config.ui.show_reasoning = reasoning;
                });
            }
            break;
        }

        case SettingsPage::Experts: {
            draw_expert_list();
            ImGui::Dummy(ImVec2(0, em(0.6F)));
            ImGui::Separator();
            section("DEFAULT EXPERT");
            wrapped(theme::kTextDim,
                    "Takes prompts the delegator could not place, and prompts routed to "
                    "a seat with no model. Any expert can play this part; without one, "
                    "an uncertain route is taken at face value.");
            const std::string current = config_.routing.default_expert.empty()
                ? std::string("(none)")
                : expert_label(config_.roster, config_.routing.default_expert);
            ImGui::SetNextItemWidth(em(20.0F));
            if (ImGui::BeginCombo("##default-expert", current.c_str())) {
                if (ImGui::Selectable("(none)", config_.routing.default_expert.empty())) {
                    update_config([](Config& config) {
                        config.routing.default_expert.clear();
                    });
                }
                for (const Expert& expert : config_.roster.experts()) {
                    if (ImGui::Selectable(expert.name.c_str(),
                                          config_.routing.default_expert == expert.id)) {
                        update_config([&expert](Config& config) {
                            config.routing.default_expert = expert.id;
                        });
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }

        case SettingsPage::Hardware: {
            title("Hardware");

            section("DEVICES");
            const std::vector<ComputeDevice> devices = compute_devices();
            if (devices.empty()) {
                wrapped(theme::kTextDim,
                        "No compute devices -- no runtime is installed. Install one from "
                        "the terminal app with /runtimes: it compiles a GPU backend for "
                        "this machine, which takes minutes and wants a log rather than a "
                        "progress bar.");
            }
            for (const ComputeDevice& device : devices) {
                text_coloured(theme::kTextDim, "[%d] %s  %s", device.index,
                              device.label().c_str(), device.backend.c_str());
            }

            section("SPLIT");
            bool gpu_only = config_.gpu.gpu_only;
            if (ImGui::Checkbox("Keep every layer on the GPU", &gpu_only)) {
                update_config([gpu_only](Config& config) {
                    config.gpu.gpu_only = gpu_only;
                });
            }
            ImGui::SetItemTooltip(
                "A model 90%% offloaded runs at roughly the speed of one not offloaded "
                "at all.");
            bool vram_only = config_.gpu.vram_only;
            if (ImGui::Checkbox("Refuse a model that will not fit in VRAM", &vram_only)) {
                update_config([vram_only](Config& config) {
                    config.gpu.vram_only = vram_only;
                });
            }
            ImGui::SetItemTooltip(
                "Otherwise the driver spills into system RAM and the model runs about "
                "twenty times slower with nothing on screen to say why.");

            int context = config_.defaults.n_ctx;
            ImGui::SetNextItemWidth(em(14.0F));
            if (ImGui::InputInt("Context size", &context, 1024, 4096)) {
                context = std::clamp(context, 512, 1 << 20);
                update_config([context](Config& config) {
                    config.defaults.n_ctx = context;
                });
            }
            break;
        }

        case SettingsPage::Tools: {
            title("Tools");

            section("WORKSHOP");
            wrapped(theme::kTextDim,
                    "What a cook is allowed to do to this project. Off, Crucible only "
                    "answers questions about it. Every file an expert reads or writes is "
                    "resolved inside the project folder and anything that escapes it is "
                    "refused.");
            text_coloured(theme::kFlame, "%s", store_->project().root.string().c_str());

            bool workshop = config_.tools.workshop;
            if (ImGui::Checkbox("Let experts read and write files here", &workshop)) {
                update_config([workshop](Config& config) {
                    config.tools.workshop = workshop;
                });
            }
            bool allow_run = config_.tools.workshop_run;
            if (ImGui::Checkbox("Let them run commands too", &allow_run)) {
                update_config([allow_run](Config& config) {
                    config.tools.workshop_run = allow_run;
                });
            }
            if (config_.tools.workshop_run) {
                wrapped(theme::kError,
                        "A command starts in the project folder but is not confined to "
                        "it: a shell can cd anywhere and read anything you can. This is "
                        "a separate switch for that reason.");
            }
            ImGui::SetItemTooltip(
                "Editing a project you trusted and running commands as you are not the "
                "same decision.");

            int timeout = config_.tools.workshop_timeout;
            ImGui::SetNextItemWidth(em(12.0F));
            if (ImGui::SliderInt("Command timeout (s)", &timeout, 5, 600)) {
                update_config([timeout](Config& config) {
                    config.tools.workshop_timeout = timeout;
                });
            }

            section("WEB SEARCH");
            bool web = config_.tools.web_search;
            if (ImGui::Checkbox("Let experts look things up", &web)) {
                update_config([web](Config& config) { config.tools.web_search = web; });
            }
            ImGui::SetItemTooltip("The only thing Crucible sends off this machine.");
            if (config_.tools.web_search) {
                ImGui::SetNextItemWidth(em(14.0F));
                if (ImGui::BeginCombo("Provider", config_.tools.search_provider.c_str())) {
                    for (const char* which : {"wikipedia", "searxng", "brave"}) {
                        if (ImGui::Selectable(which,
                                              config_.tools.search_provider == which)) {
                            update_config([which](Config& config) {
                                config.tools.search_provider = which;
                            });
                        }
                    }
                    ImGui::EndCombo();
                }
                std::string endpoint = config_.tools.search_endpoint;
                ImGui::SetNextItemWidth(em(22.0F));
                if (ImGui::InputTextWithHint("Endpoint", "http://localhost:8888", &endpoint,
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                    update_config([&endpoint](Config& config) {
                        config.tools.search_endpoint = endpoint;
                    });
                }
            }
            break;
        }

        case SettingsPage::About: {
            title("Crucible " CRUCIBLE_VERSION);
            wrapped(theme::kTextDim,
                    "A local forge: experts on demand, projects that cook. This window "
                    "and the terminal program are the same engine -- same roster, same "
                    "cook loop, same config file.");

            section("FILES");
            text_coloured(theme::kTextDim, "config    %s",
                          paths::config_file().string().c_str());
            text_coloured(theme::kTextDim, "models    %s",
                          config_.resolved_models_dir().string().c_str());
            text_coloured(theme::kTextDim, "runtimes  %s",
                          paths::runtimes_dir().string().c_str());
            text_coloured(theme::kTextDim, "history   %s",
                          store_->project().dir.string().c_str());
            text_coloured(theme::kTextDim, "log       %s",
                          paths::log_file().string().c_str());

            section("TRUSTED FOLDERS");
            wrapped(theme::kTextDim,
                    "Crucible asks once per directory before it will read or write "
                    "there. These are the ones you have said yes to.");
            for (const std::filesystem::path& entry : trust_.entries()) {
                text_coloured(theme::kTextFaint, "%s", entry.string().c_str());
            }
            break;
        }
    }
    ImGui::EndChild();
}

}  // namespace crucible::gui
