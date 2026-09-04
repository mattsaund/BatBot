// SPDX-License-Identifier: MIT
//
// The window and the frame: opening it, one pass of drawing, and the actions
// the panels call back into.
//
// The panels themselves are in gui/panels/, one file each, and the helpers
// they are written with are in gui/widgets.hpp. What stays here is everything
// that owns state rather than draws it -- which is why an action like
// open_project or update_config lives in this file and nothing in panels/
// touches config_ directly.
#include "app.hpp"

#include <cstdio>
#include <system_error>

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace crucible::gui {

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

App::App(Config config, std::vector<std::string> warnings)
    : config_(std::move(config)),
      store_(std::make_unique<SessionStore>(Project::current())),
      trust_(paths::trust_file()) {
    for (std::string& warning : warnings) {
        notices_.push_back(std::move(warning));
    }

    state_.configure_seats(config_);
    state_.set_project_usage(store_->project_usage());

    engine_ = std::make_unique<Engine>(config_, state_, [this] {
        // The engine runs on its own thread and the window may be parked in
        // glfwWaitEvents. Without this the screen would not repaint until the
        // mouse moved, which during a model load is most of a minute.
        glfwPostEmptyEvent();
    });
    engine_->set_journal_dir(store_->project().dir);

    remember_project(store_->project().root);
    browse_      = store_->project().root;
    browse_text_ = browse_.string();
    refresh_models();
}

App::~App() {
    if (engine_) {
        engine_->stop();
    }
}

void App::say(std::string message) {
    notices_.push_back(std::move(message));
    // Only the last few. This is a status channel, not a log; the log is on
    // disk and the transcript is above it.
    if (notices_.size() > 6) {
        notices_.erase(notices_.begin());
    }
}

void App::refresh_models() {
    models_ = scan_models(config_.resolved_models_dir());
}

void App::update_config(const std::function<void(Config&)>& change) {
    change(config_);
    config_.resolve_models();
    state_.configure_seats(config_);
    if (!save_config(config_)) {
        say("could not write " + paths::config_file().string());
    }
    engine_->apply_config(config_);
}

void App::persist_session() {
    const Snapshot snapshot = state_.snapshot();
    std::size_t finished = 0;
    for (const Turn& turn : snapshot.turns) {
        finished += turn.streaming ? 0 : 1;
    }
    if (finished == persisted_turns_) {
        return;
    }
    std::string error;
    if (store_->save(snapshot.turns, snapshot.session_usage, error)) {
        persisted_turns_ = finished;
    }
}

void App::absorb_written_examples() {
    const std::vector<std::pair<ExpertId, std::vector<std::string>>> written =
        engine_->take_written_examples();
    if (written.empty()) {
        return;
    }
    update_config([&written](Config& config) {
        for (const auto& [id, examples] : written) {
            if (const std::optional<std::size_t> seat = config.roster.find(id)) {
                Expert expert = config.roster.at(*seat);
                expert.examples = examples;
                config.roster.update(id, expert);
            }
        }
    });
    for (const auto& [id, examples] : written) {
        say(expert_label(config_.roster, id) + ": the delegator wrote "
            + std::to_string(examples.size()) + " example questions to route on");
    }
}

void App::open_project(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        project_error_ = root.string() + " is not a directory";
        return;
    }
    if (engine_->cooking()) {
        // A cook is about the directory it started in, and its journal is keyed
        // to it. Moving the ground under it would produce a record of work done
        // somewhere it was not.
        say("finish or stop the cook before opening another project");
        return;
    }

    // The same store the terminal program asks on first use in a directory. A
    // folder trusted in one face is trusted in the other, because it is one
    // decision about one directory.
    if (!trust_.is_trusted(root)) {
        pending_trust_ = root;
        return;
    }

    const Project project = Project::at(root);
    persist_session();

    store_ = std::make_unique<SessionStore>(project);
    engine_->set_journal_dir(project.dir);
    engine_->reset_history();
    state_.clear_turns();
    state_.clear_notices();
    state_.set_cook(nullptr);
    state_.set_project_usage(store_->project_usage());
    persisted_turns_ = 0;
    expanded_.clear();
    notices_.clear();
    follow_      = true;
    browse_      = project.root;
    browse_text_ = browse_.string();

    remember_project(project.root);
    project_error_.clear();
    say("opened " + project.root.string());
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void App::submit_prompt() {
    const std::string text = format::trim(prompt_);
    if (text.empty()) {
        return;
    }
    prompt_.clear();
    follow_ = true;

    // A cook waiting on a question takes the next thing typed as its answer.
    // The screen is showing a question; nothing else would be a reasonable
    // reading of a line typed under it.
    if (const std::shared_ptr<const Cook> cook = state_.cook();
        cook && cook->state == CookState::Asking) {
        engine_->answer_cook(text);
        return;
    }
    engine_->submit(text);
}

void App::begin_cook() {
    const std::string goal = format::trim(cook_goal_);
    if (goal.empty()) {
        say("a cook needs a goal");
        return;
    }
    if (!config_.tools.workshop) {
        // Refused here rather than several model calls later, where it would
        // surface as the expert being told the workshop is off, over and over.
        say("cooking needs the workshop, which is off -- turn it on in Settings");
        view_          = View::Settings;
        settings_page_ = SettingsPage::Tools;
        return;
    }
    cook_goal_.clear();
    follow_ = true;
    view_   = View::Cook;
    expanded_.clear();
    engine_->start_cook(goal, cook_untimed_ ? 0 : cook_minutes_ * 60,
                        store_->project().root);
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------

void App::draw() {
    const Snapshot snapshot = state_.snapshot();

    if (sidebar_width_ <= 0.0F) {
        sidebar_width_ = em(17.0F);
    }

    // One window filling the viewport. Crucible is an application, not a
    // collection of floating panels, and a desktop app that opens with its own
    // windows scattered over the screen looks like a debug build.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("crucible", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    draw_sidebar(snapshot);
    draw_splitter();

    ImGui::BeginChild("main", ImVec2(0, 0));
    {
        const float composer =
            view_ == View::Chat || view_ == View::Cook ? em(7.7F) : 0.0F;
        ImGui::BeginChild("pane", ImVec2(0, -composer), ImGuiChildFlags_Borders);
        switch (view_) {
            case View::Chat:     draw_chat(snapshot); break;
            case View::Cook:     draw_cook(snapshot); break;
            case View::History:  draw_history();      break;
            case View::Settings: draw_settings();     break;
        }
        // Following the bottom, but only while the user is already there.
        // Yanking someone reading back through an hour-old cook to the end
        // every time a token arrives is the single most irritating thing a
        // streaming view can do.
        if (follow_ && (view_ == View::Chat || view_ == View::Cook)
            && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndChild();

        if (composer > 0.0F) {
            draw_composer(snapshot);
        }
    }
    ImGui::EndChild();

    draw_new_expert_modal();
    draw_project_modal();
    draw_trust_modal();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

int App::run() {
    glfwSetErrorCallback([](int code, const char* description) {
        std::fprintf(stderr, "crucible-gui: glfw error %d: %s\n", code, description);
    });
    if (glfwInit() == GLFW_FALSE) {
        std::fprintf(stderr, "crucible-gui: could not open a window. On Linux this "
                             "usually means there is no display, or no OpenGL driver.\n");
        return 1;
    }

    // GL 3.2 core: the oldest thing ImGui's backend is happy with, and old
    // enough that a decade-old integrated chip and a virtual machine both have
    // it. There is nothing here that wants a newer one.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    // Sized against the monitor rather than in fixed pixels: 1280x820 is a
    // reasonable window on a 1080p panel and a postage stamp on a 4K one.
    int width  = 1280;
    int height = 820;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor(); monitor != nullptr) {
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitor); mode != nullptr) {
            width  = std::clamp(static_cast<int>(mode->width * 0.62F), 1120, 2200);
            height = std::clamp(static_cast<int>(mode->height * 0.70F), 760, 1500);
        }
    }
    window_ = glfwCreateWindow(width, height, "Crucible", nullptr, nullptr);
    if (window_ == nullptr) {
        std::fprintf(stderr, "crucible-gui: could not create the window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini litter beside the project
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // The display's own scale, so the interface is the same physical size on a
    // 4K laptop panel as on a 1080p monitor. GLFW reports it per monitor; the
    // one the window opened on is the one that matters.
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor(); monitor != nullptr) {
        glfwGetMonitorContentScale(monitor, &scale_x, &scale_y);
    }
    const float scale = std::max(1.0F, scale_x);
    theme::load_fonts(scale);
    theme::apply();
    ImGui::GetStyle().ScaleAllSizes(scale);

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    engine_->start();

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        // Waiting rather than spinning. An idle Crucible should cost nothing,
        // and the engine posts an empty event whenever it has something new --
        // the timeout is only there so the cook clock keeps moving.
        glfwWaitEventsTimeout(state_.busy() ? 0.05 : 0.5);

        persist_session();
        absorb_written_examples();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        draw();
        ImGui::Render();

        int fb_width  = 0;
        int fb_height = 0;
        glfwGetFramebufferSize(window_, &fb_width, &fb_height);
        glViewport(0, 0, fb_width, fb_height);
        const ImVec4 ground = theme::to_vec(theme::kInk);
        glClearColor(ground.x, ground.y, ground.z, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }

    // The engine has a thread that calls back into this object, so it has to be
    // stopped before anything it might touch is torn down.
    engine_->stop();
    persist_session();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    return 0;
}

}  // namespace crucible::gui
