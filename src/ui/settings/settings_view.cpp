// SPDX-License-Identifier: MIT
#include "batbot/ui/settings/settings_view.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "batbot/runtime/devices.hpp"
#include "batbot/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {
namespace {

/// Width of the label column, in cells. Wide enough for the longest row label
/// on the screen ("Reset model path to default"), because a label that is cut
/// off mid-word turns a button into a riddle.
constexpr int kLabelWidth = 28;

/// Sentinel seat meaning "the delegator", which is not one of the subjects.
constexpr std::size_t kRouterSeat = kSubjectCount;

std::string format_float(float value) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.2f", static_cast<double>(value));
    return buffer.data();
}

}  // namespace

SettingsView::SettingsView(Config config) : config_(std::move(config)) {
    refresh();
}

void SettingsView::set_config(Config config) {
    config_ = std::move(config);
    dirty_  = false;
    refresh();
}

void SettingsView::refresh() {
    models_ = scan_models(config_.resolved_models_dir());
    build_rows();
    if (selected_ >= rows_.size()) {
        selected_ = 0;
    }
    // Never leave the cursor parked on a header, which cannot be edited.
    if (!rows_.empty() && rows_[selected_].kind == Kind::Header) {
        move_selection(1);
    }
}

// ---------------------------------------------------------------------------
// Rows
//
// Built fresh rather than kept, because the pointers below refer into config_
// and a reassignment of the whole struct would leave them dangling.
// ---------------------------------------------------------------------------

void SettingsView::build_rows() {
    rows_.clear();

    const auto header = [&](std::string label) {
        rows_.push_back({Kind::Header, std::move(label), "", nullptr, nullptr,
                         nullptr, nullptr, 0, {}});
    };

    header("MODELS");
    rows_.push_back({Kind::Directory, "Models directory",
                     "where BatBot looks for .gguf files", &config_.models_dir,
                     nullptr, nullptr, nullptr, 0, {}, ActionId::None});
    // The way back from a models directory that was moved somewhere awkward,
    // or onto a drive that is no longer plugged in.
    rows_.push_back({Kind::Action, "Reset model path to default",
                     "put the models directory back where BatBot expects it",
                     nullptr, nullptr, nullptr, nullptr, 0, {},
                     ActionId::ResetModelsDir});
    rows_.push_back({Kind::Panel, "Manage models",
                     "delete GGUF files you no longer want",
                     nullptr, nullptr, nullptr, nullptr, 0, {},
                     ActionId::None, PanelId::Models});

    header("DELEGATOR");
    rows_.push_back({Kind::ModelRef, "Router model",
                     "small model that picks the expert; blank falls back to keywords",
                     &config_.router.model, nullptr, nullptr, nullptr, kRouterSeat, {}});

    header("EXPERTS");
    for (const SubjectInfo& info : all_subjects()) {
        const auto seat = static_cast<std::size_t>(info.subject);
        rows_.push_back({Kind::ModelRef, std::string(info.name), std::string(info.blurb),
                         &config_.experts[seat].model, nullptr, nullptr, nullptr, seat, {}});
    }

    header("ROUTING");
    rows_.push_back({Kind::Float, "Min confidence",
                     "below this the delegator is treated as undecided",
                     nullptr, nullptr, &config_.routing.min_confidence, nullptr, 0, {}});
    rows_.push_back({Kind::Bool, "Keep delegator loaded",
                     "off frees it after each decision, leaving the expert the whole card",
                     nullptr, nullptr, nullptr, &config_.routing.keep_delegator_loaded, 0, {}});
    rows_.push_back({Kind::Bool, "Use fallback expert",
                     "empty seats send work to Fallback rather than elsewhere",
                     nullptr, nullptr, nullptr, &config_.routing.use_fallback_expert, 0, {}});

    header("DEFAULTS");
    ModelParams& d = config_.defaults;
    rows_.push_back({Kind::Int,   "Context size",   "tokens of context per expert",
                     nullptr, &d.n_ctx, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "GPU layers",     "-1 offloads as much as fits",
                     nullptr, &d.n_gpu_layers, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Batch size",     "prompt ingestion batch",
                     nullptr, &d.n_batch, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Threads",        "0 picks automatically",
                     nullptr, &d.n_threads, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Max tokens",     "hard cap on a single reply",
                     nullptr, &d.max_tokens, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Temperature",    "0 is greedy and deterministic",
                     nullptr, nullptr, &d.temperature, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Top-p",          "nucleus sampling cutoff",
                     nullptr, nullptr, &d.top_p, nullptr, 0, {}});
    rows_.push_back({Kind::Int,   "Top-k",          "candidates kept before sampling",
                     nullptr, &d.top_k, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Min-p",          "minimum relative probability",
                     nullptr, nullptr, &d.min_p, nullptr, 0, {}});
    rows_.push_back({Kind::Float, "Repeat penalty", "1.0 disables it",
                     nullptr, nullptr, &d.repeat_penalty, nullptr, 0, {}});
    rows_.push_back({Kind::Bool,  "Flash attention", "faster attention where supported",
                     nullptr, nullptr, nullptr, &d.flash_attn, 0, {}});
    rows_.push_back({Kind::Enum,  "Split granularity",
                     "what gets divided between GPUs: whole layers, or rows within them",
                     &d.split_mode, nullptr, nullptr, nullptr, 0,
                     {"layer", "row", "tensor", "none"}});

    header("HARDWARE");
    rows_.push_back({Kind::Panel, "Runtimes",
                     "install or remove CUDA / Vulkan / CPU backends",
                     nullptr, nullptr, nullptr, nullptr, 0, {},
                     ActionId::None, PanelId::Runtimes});
    rows_.push_back({Kind::Enum, "Multi-GPU split",
                     "how one expert is divided between the graphics cards",
                     &config_.gpu.mode, nullptr, nullptr, nullptr, 0,
                     {"auto", "even", "priority", "single"}});
    rows_.push_back({Kind::Int, "Main GPU",
                     "device index for small tensors, and the whole model in single mode",
                     nullptr, &config_.gpu.main_gpu, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Panel, "GPU priority order",
                     "which card is filled first, when the split is by priority",
                     nullptr, nullptr, nullptr, nullptr, 0, {},
                     ActionId::None, PanelId::GpuOrder});
    rows_.push_back({Kind::Bool, "GPU-only compute",
                     "put every layer on the GPU and keep the processor out of it",
                     nullptr, nullptr, nullptr, &config_.gpu.gpu_only, 0, {}});
    rows_.push_back({Kind::Bool, "Dedicated VRAM only",
                     "refuse a model that would spill out of video memory into RAM",
                     nullptr, nullptr, nullptr, &config_.gpu.vram_only, 0, {}});

    header("BEHAVIOUR");
    rows_.push_back({Kind::Text, "System prompt", "sent to every expert",
                     &config_.system_prompt, nullptr, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Enum, "Reasoning effort", "how hard a thinking model works",
                     &config_.reasoning_effort, nullptr, nullptr, nullptr, 0,
                     {"low", "medium", "high"}});

    header("TOOLS");
    rows_.push_back({Kind::Bool, "Web search",
                     "let experts look things up -- the only thing BatBot sends off the machine",
                     nullptr, nullptr, nullptr, &config_.tools.web_search, 0, {}});
    rows_.push_back({Kind::Enum, "Search provider", "where the looking up happens",
                     &config_.tools.search_provider, nullptr, nullptr, nullptr, 0,
                     {"wikipedia", "searxng", "brave"}});
    rows_.push_back({Kind::Text, "Search endpoint", "the address of your own searxng instance",
                     &config_.tools.search_endpoint, nullptr, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Text, "Search API key", "for brave; stored in the config in plain text",
                     &config_.tools.search_api_key, nullptr, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,  "Search results", "how many to hand the expert",
                     nullptr, &config_.tools.search_results, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Int,  "Search rounds", "how many times one prompt may search",
                     nullptr, &config_.tools.search_rounds, nullptr, nullptr, 0, {}});

    header("INTERFACE");
    rows_.push_back({Kind::Int,  "Animation ms", "frame interval while busy",
                     nullptr, &config_.ui.animation_ms, nullptr, nullptr, 0, {}});
    rows_.push_back({Kind::Bool, "Show roundtable", "draw the ring of experts",
                     nullptr, nullptr, nullptr, &config_.ui.show_roundtable, 0, {}});
    rows_.push_back({Kind::Bool, "Show reasoning", "keep a thinking model's working on screen",
                     nullptr, nullptr, nullptr, &config_.ui.show_reasoning, 0, {}});
    rows_.push_back({Kind::Bool, "Unicode glyphs", "off uses a pure-ASCII bat",
                     nullptr, nullptr, nullptr, &config_.ui.unicode, 0, {}});
}

// ---------------------------------------------------------------------------
// GPU priority order
//
// Stored as device indices and rearranged in a panel of its own, so all this
// screen has to do is name the cards behind those indices.
// ---------------------------------------------------------------------------

void SettingsView::set_gpu_priority(std::vector<int> order) {
    if (config_.gpu.priority == order) {
        return;
    }
    config_.gpu.priority = std::move(order);
    dirty_ = true;
    // The row's value column reads the new order straight out of the config,
    // so nothing else has to be kept in step.
}

std::vector<std::string> SettingsView::models_in_use() const {
    std::vector<std::string> names;
    const auto add = [&names](const std::string& model) {
        // Only bare file names: a seat pointing at /mnt/big/x.gguf does not
        // name anything in the models directory, so nothing there can be the
        // file it means.
        if (!model.empty() && is_bare_name(model) &&
            std::find(names.begin(), names.end(), model) == names.end()) {
            names.push_back(model);
        }
    };
    add(config_.router.model);
    for (const ModelParams& expert : config_.experts) {
        add(expert.model);
    }
    return names;
}

void SettingsView::forget_models(const std::vector<std::string>& names) {
    const auto gone = [&names](const std::string& model) {
        return !model.empty() &&
               std::find(names.begin(), names.end(), model) != names.end();
    };

    bool changed = false;
    if (gone(config_.router.model)) {
        config_.router.model.clear();
        config_.router.path.clear();
        changed = true;
    }
    for (ModelParams& expert : config_.experts) {
        if (gone(expert.model)) {
            expert.model.clear();
            expert.path.clear();
            changed = true;
        }
    }

    if (changed) {
        dirty_ = true;
    }
    // Rebuild either way: the file list at the top of the screen changed even
    // when no seat pointed at what went.
    refresh();
}

std::string SettingsView::gpu_priority_summary() const {
    if (config_.gpu.priority.empty()) {
        return "auto";
    }

    const std::vector<ComputeDevice> gpus = gpu_devices();

    std::string summary;
    for (const int index : config_.gpu.priority) {
        const auto found = std::find_if(gpus.begin(), gpus.end(),
                                        [index](const ComputeDevice& gpu) {
                                            return gpu.index == index;
                                        });
        if (!summary.empty()) {
            summary += " > ";
        }
        // A card in the config that is not in the machine is worth saying out
        // loud: it is the difference between a split that was configured and
        // one that will actually happen.
        summary += found == gpus.end()
                       ? "[device " + std::to_string(index) + " missing]"
                       : (found->description.empty() ? found->name : found->description);
    }
    return summary;
}

std::string SettingsView::value_of(const Row& row) const {
    switch (row.kind) {
        case Kind::Header:   return {};
        case Kind::Directory:
            // Blank means "the default". Showing the blank would leave an empty
            // field with nothing to edit, so resolve it first.
            return config_.resolved_models_dir().string();
        case Kind::ModelRef:
        case Kind::Text:
        case Kind::Enum:     return row.text != nullptr ? *row.text : std::string{};
        case Kind::Int:      return row.integer != nullptr ? std::to_string(*row.integer)
                                                           : std::string{};
        case Kind::Float:    return row.real != nullptr ? format_float(*row.real) : std::string{};
        case Kind::Bool:     return (row.flag != nullptr && *row.flag) ? "on" : "off";
        case Kind::Panel:
            // The GPU order row is worth reading without opening it; the
            // Runtimes row is not, since its panel is the whole story.
            if (row.panel == PanelId::GpuOrder) {
                return gpu_priority_summary();
            }
            if (row.panel == PanelId::Models) {
                return std::to_string(models_.size()) +
                       (models_.size() == 1 ? " model" : " models");
            }
            return std::string("›");
        case Kind::Action:  return "›";
    }
    return {};
}

void SettingsView::move_selection(int delta) {
    if (rows_.empty()) {
        return;
    }
    auto index = static_cast<long>(selected_);
    const auto count = static_cast<long>(rows_.size());
    // Walk until a non-header lands under the cursor. Bounded by the row count
    // so a screen of nothing but headers cannot spin forever.
    for (long step = 0; step < count; ++step) {
        index += delta;
        if (index < 0)      { index = count - 1; }
        if (index >= count) { index = 0; }
        if (rows_[static_cast<std::size_t>(index)].kind != Kind::Header) {
            selected_ = static_cast<std::size_t>(index);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void SettingsView::activate_selection() {
    const Row& row = rows_[selected_];

    switch (row.kind) {
        case Kind::Header:
            return;
        case Kind::Bool:
            // A checkbox needs no edit mode; Enter is the whole interaction.
            *row.flag = !*row.flag;
            dirty_    = true;
            return;
        case Kind::Enum: {
            const auto it = std::find(row.options.begin(), row.options.end(), *row.text);
            const std::size_t next = (it == row.options.end())
                ? 0
                : (static_cast<std::size_t>(std::distance(row.options.begin(), it)) + 1)
                      % row.options.size();
            *row.text = row.options[next];
            dirty_    = true;
            return;
        }
        case Kind::ModelRef:
            dialog_target_ = selected_;
            picker_.open(models_, *row.text, row.label);
            return;
        case Kind::Directory:
            dialog_target_ = selected_;
            browser_.open(config_.resolved_models_dir());
            return;
        case Kind::Panel:
            // Handled by the caller, which owns the panel.
            return;
        case Kind::Action:
            run_action(row.action);
            return;
        default:
            begin_typing();
            return;
    }
}

void SettingsView::run_action(ActionId action) {
    switch (action) {
        case ActionId::None:
            return;
        case ActionId::ResetModelsDir: {
            // An empty models_dir *means* the default, rather than storing the
            // resolved path -- so a config written on one machine still points
            // somewhere sensible on another.
            if (config_.models_dir.empty()) {
                status_ = "already using the default models directory";
                return;
            }
            config_.models_dir.clear();
            config_.resolve_models();
            dirty_  = true;
            // The path is on the row above; repeating it here only crowds the
            // footer out of the hint and the unsaved-changes marker.
            status_ = "models directory reset to the default";
            // Rebuilds the rows, so the seats show which models still resolve.
            refresh();
            return;
        }
    }
}

void SettingsView::begin_typing() {
    const Row& row = rows_[selected_];
    // Rows with nothing to type into: a header, a toggle, a cycled enum, a
    // row that opens a screen, or one that just does a thing. Opening an
    // editor on those would offer an edit that is silently discarded.
    switch (row.kind) {
        case Kind::Header:
        case Kind::Bool:
        case Kind::Enum:
        case Kind::Panel:
        case Kind::Action:
            return;
        default:
            break;
    }
    editor_.begin(value_of(row));
}

void SettingsView::commit_edit() {
    Row& row = rows_[selected_];
    const std::string text = editor_.value();
    editor_.cancel();

    switch (row.kind) {
        case Kind::Directory:
        case Kind::Text:
            *row.text = text;
            dirty_    = true;
            // Moving the models directory changes what the picker can offer.
            if (row.text == &config_.models_dir) {
                refresh();
            }
            break;
        case Kind::Int:
            // A typo should cost the edit, not the session: report it and keep
            // the previous value.
            try {
                *row.integer = std::stoi(text);
                dirty_       = true;
            } catch (const std::exception&) {
                status_ = "'" + text + "' is not a whole number";
            }
            break;
        case Kind::Float:
            try {
                *row.real = std::stof(text);
                dirty_    = true;
            } catch (const std::exception&) {
                status_ = "'" + text + "' is not a number";
            }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Input
//
// Dispatch order matters: whichever dialog or editor is open owns the keyboard,
// so a '.' typed into a path is never mistaken for the browser's hidden-files
// toggle.
// ---------------------------------------------------------------------------

SettingsAction SettingsView::handle(const Event& event, bool& consumed) {
    consumed = true;

    if (browser_.active()) {
        if (const auto chosen = browser_.handle(event)) {
            *rows_[dialog_target_].text = chosen->string();
            dirty_ = true;
            refresh();
            status_ = "models directory set";
        } else if (browser_.wants_manual_entry()) {
            selected_ = dialog_target_;
            editor_.begin(browser_.path().string());
        }
        return SettingsAction::None;
    }

    if (picker_.active()) {
        if (const auto chosen = picker_.handle(event)) {
            *rows_[dialog_target_].text = *chosen;
            dirty_ = true;
        }
        return SettingsAction::None;
    }

    if (editor_.active()) {
        if (event == Event::Return) {
            commit_edit();
        } else if (event == Event::Escape) {
            editor_.cancel();
        } else {
            editor_.handle(event);
        }
        return SettingsAction::None;
    }

    if (event == Event::ArrowUp   || event == Event::Character('k')) { move_selection(-1); return SettingsAction::None; }
    if (event == Event::ArrowDown || event == Event::Character('j')) { move_selection(1);  return SettingsAction::None; }
    if (event == Event::Return || event == Event::Character(' ')) {
        // A panel row is the application's to open: it owns the panel, because
        // the panel outlives this screen (a build keeps running after you
        // leave settings).
        if (!rows_.empty() && rows_[selected_].kind == Kind::Panel) {
            switch (rows_[selected_].panel) {
                case PanelId::Runtimes: return SettingsAction::OpenRuntimes;
                case PanelId::GpuOrder: return SettingsAction::OpenGpuOrder;
                case PanelId::Models:   return SettingsAction::OpenModels;
                case PanelId::None:     break;
            }
            return SettingsAction::None;
        }
        activate_selection();
        return SettingsAction::None;
    }
    if (event == Event::Character('e')) {
        // Type a value outright, skipping whatever dialog Enter would open.
        begin_typing();
        return SettingsAction::None;
    }
    if (event == Event::Character('r')) {
        refresh();
        status_ = "rescanned " + config_.resolved_models_dir().string();
        return SettingsAction::None;
    }
    if (event == Event::CtrlS) { return SettingsAction::Apply; }
    if (event == Event::Escape) { return SettingsAction::Close; }

    consumed = false;
    return SettingsAction::None;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

Element SettingsView::render_row(const Row& row, std::size_t index) const {
    if (row.kind == Kind::Header) {
        return vbox({
            text(" "),
            text("  " + row.label) | color(theme::kAccent) | bold,
        });
    }

    const bool selected = index == selected_;

    if (selected && editor_.active()) {
        return hbox({
            text(" > ") | color(theme::kAccent) | bold,
            text(row.label) | bold | size(WIDTH, EQUAL, kLabelWidth),
            editor_.render() | flex,
        }) | bgcolor(theme::kHighlight);
    }

    std::string value = value_of(row);
    Color value_color = theme::kUser;

    if (row.kind == Kind::Directory) {
        std::error_code ec;
        if (!std::filesystem::is_directory(value, ec)) {
            value += "  (does not exist)";
            value_color = theme::kError;
        }
    } else if (row.kind == Kind::ModelRef) {
        if (value.empty()) {
            value       = "(none)";
            value_color = theme::kMeta;
        } else {
            // A seat pointing at a file that is not there is the single most
            // useful thing this screen can say, so say it inline.
            const std::filesystem::path resolved =
                resolve_model_ref(config_.resolved_models_dir(), value);
            if (!std::filesystem::exists(resolved)) {
                value += "  (missing)";
                value_color = theme::kError;
            } else {
                value_color = theme::kSeatActive;
            }
        }
    } else if (row.kind == Kind::Bool) {
        value_color = (row.flag != nullptr && *row.flag) ? theme::kSeatActive : theme::kMeta;
    }

    // kMeta is the same grey as the highlight, so a "(none)" or an "off" on the
    // selected row would be invisible exactly when it is being looked at.
    Element value_element = (Color(value_color) == Color(theme::kMeta))
                                ? text(value) | color(meta_color(selected))
                                : text(value) | color(value_color);

    Element label = text(row.label);
    Element line  = hbox({
        text(selected ? " > " : "   ") | color(theme::kAccent) | bold,
        (selected ? label | bold : label) | size(WIDTH, EQUAL, kLabelWidth),
        std::move(value_element) | flex,
    });
    return selected ? line | bgcolor(theme::kHighlight) : line;
}

Element SettingsView::footer_hint() const {
    if (editor_.active()) {
        return text(" ←→ move   ctrl-a/e ends   ctrl-w del word   ctrl-u clear   "
                    "enter save   esc cancel ");
    }
    if (!rows_.empty() && selected_ < rows_.size()) {
        switch (rows_[selected_].kind) {
            case Kind::Directory:
                return text(" enter browse folders   e type a path   esc back ");
            case Kind::ModelRef:
                return text(" enter choose a model   r rescan   esc back ");
            case Kind::Panel:
                return text(" enter open   ↑↓ move   esc back ");
            case Kind::Action:
                return text(" enter reset to the default   ↑↓ move   esc back ");
            default:
                break;
        }
    }
    return text(" ↑↓ move   enter edit   e type   r rescan   esc back ");
}

Element SettingsView::render() const {
    Elements lines;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        Element row = render_row(rows_[i], i);
        lines.push_back(i == selected_ ? row | ftxui::focus : row);
    }

    const std::string dir   = config_.resolved_models_dir().string();
    const std::string count = std::to_string(models_.size()) + " model"
                            + (models_.size() == 1 ? "" : "s") + " found";

    Elements footer{footer_hint() | color(theme::kMeta) | dim};
    // No "unsaved changes" marker, because there is no such state: every
    // committed edit is written before the next key is read. Saying so once,
    // quietly, is what stops anyone hunting for the save key.
    if (status_.empty()) {
        footer.push_back(filler());
        footer.push_back(text("changes save automatically  ") | color(theme::kMeta) | dim);
    }
    if (!status_.empty()) {
        footer.push_back(filler());
        footer.push_back(text(status_ + "  ") | color(theme::kSeatActive));
    }

    Element screen = window(
        text(" Settings ") | bold | color(theme::kBat),
        vbox({
            hbox({
                text("  " + dir) | color(theme::kMeta),
                filler(),
                text(count + "  ") | color(theme::kMeta) | dim,
            }),
            separator(),
            vbox(std::move(lines)) | yframe | flex,
            separator(),
            hbox(std::move(footer)),
        }));

    // dbox layers a dialog over the list rather than replacing it, so the row
    // being changed stays visible behind it.
    if (picker_.active())  { return dbox({screen, picker_.render(dir)}); }
    if (browser_.active()) { return dbox({screen, browser_.render()}); }
    return screen;
}

}  // namespace batbot::ui
