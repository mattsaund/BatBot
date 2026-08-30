// SPDX-License-Identifier: MIT
//
// Reading and writing config.json.
//
// Kept separate from the Config type itself so the rules about *what* a setting
// means stay in config.cpp, and the rules about how it survives a round trip
// to disk stay here.
//
// Loading is forgiving on purpose: a single mistyped field costs that field,
// not the other eight experts, and every problem is collected as a warning the
// UI can show rather than thrown.
#include "batbot/config/config.hpp"

#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

#include "batbot/llm/model_catalog.hpp"
#include "batbot/config/paths.hpp"

namespace batbot {
namespace {

using json = nlohmann::json;

/// Read one optional field, leaving the destination untouched when the key is
/// absent or holds the wrong type. A malformed entry is reported and skipped
/// rather than aborting the load -- a typo in one expert should not stop the
/// other eight from working.
template <typename T>
void read_field(const json& obj, const char* key, T& dest,
                std::string_view context, std::vector<std::string>& warnings) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return;
    }
    try {
        dest = it->get<T>();
    } catch (const json::exception& e) {
        warnings.emplace_back(std::string(context) + "." + key + ": " + e.what()
                              + " (keeping the default)");
    }
}

void read_model_params(const json& obj, ModelParams& params,
                       std::string_view context, std::vector<std::string>& warnings) {
    read_field(obj, "model",          params.model,          context, warnings);
    read_field(obj, "n_gpu_layers",   params.n_gpu_layers,   context, warnings);
    read_field(obj, "main_gpu",       params.main_gpu,       context, warnings);
    read_field(obj, "split_mode",     params.split_mode,     context, warnings);
    read_field(obj, "tensor_split",   params.tensor_split,   context, warnings);
    read_field(obj, "n_ctx",          params.n_ctx,          context, warnings);
    read_field(obj, "n_batch",        params.n_batch,        context, warnings);
    read_field(obj, "n_threads",      params.n_threads,      context, warnings);
    read_field(obj, "flash_attn",     params.flash_attn,     context, warnings);
    read_field(obj, "temperature",    params.temperature,    context, warnings);
    read_field(obj, "top_p",          params.top_p,          context, warnings);
    read_field(obj, "top_k",          params.top_k,          context, warnings);
    read_field(obj, "min_p",          params.min_p,          context, warnings);
    read_field(obj, "repeat_penalty", params.repeat_penalty, context, warnings);
    read_field(obj, "repeat_last_n",  params.repeat_last_n,  context, warnings);
    read_field(obj, "max_tokens",     params.max_tokens,     context, warnings);
    read_field(obj, "seed",           params.seed,           context, warnings);
}

/// Round a float before serialising it.
///
/// A float widened to double prints as 0.05000000074505806, which is correct
/// and unreadable. The config is meant to be edited by hand, so trim the noise.
double tidy(float value) {
    return std::round(static_cast<double>(value) * 10000.0) / 10000.0;
}

/// `include_model` is false for the `defaults` block, which describes how to
/// load models rather than naming one.
json model_params_to_json(const ModelParams& params, bool include_model = true) {
    json out = json{
        {"n_gpu_layers",   params.n_gpu_layers},
        {"main_gpu",       params.main_gpu},
        {"split_mode",     params.split_mode},
        {"n_ctx",          params.n_ctx},
        {"n_batch",        params.n_batch},
        {"n_threads",      params.n_threads},
        {"flash_attn",     params.flash_attn},
        {"temperature",    tidy(params.temperature)},
        {"top_p",          tidy(params.top_p)},
        {"top_k",          params.top_k},
        {"min_p",          tidy(params.min_p)},
        {"repeat_penalty", tidy(params.repeat_penalty)},
        {"repeat_last_n",  params.repeat_last_n},
        {"max_tokens",     params.max_tokens},
    };
    if (include_model) {
        out["model"] = params.model;
    }
    return out;
}

}  // namespace

bool save_config(const Config& config, const std::filesystem::path& file) {
    json experts = json::object();
    for (const SubjectInfo& info : all_subjects()) {
        const ModelParams& params = config.experts[static_cast<std::size_t>(info.subject)];
        json entry = json::object();
        entry["model"] = params.model;

        // Only write fields that differ from `defaults`. Round-tripping every
        // field would turn a nine-line config into a hundred-line one the first
        // time the user saved from the settings screen.
        const ModelParams& base = config.defaults;
        if (params.n_ctx          != base.n_ctx)          { entry["n_ctx"]          = params.n_ctx; }
        if (params.n_batch        != base.n_batch)        { entry["n_batch"]        = params.n_batch; }
        if (params.n_threads      != base.n_threads)      { entry["n_threads"]      = params.n_threads; }
        if (params.n_gpu_layers   != base.n_gpu_layers)   { entry["n_gpu_layers"]   = params.n_gpu_layers; }
        if (params.main_gpu       != base.main_gpu)       { entry["main_gpu"]       = params.main_gpu; }
        if (params.split_mode     != base.split_mode)     { entry["split_mode"]     = params.split_mode; }
        if (params.flash_attn     != base.flash_attn)     { entry["flash_attn"]     = params.flash_attn; }
        if (params.temperature    != base.temperature)    { entry["temperature"]    = params.temperature; }
        if (params.top_p          != base.top_p)          { entry["top_p"]          = params.top_p; }
        if (params.top_k          != base.top_k)          { entry["top_k"]          = params.top_k; }
        if (params.min_p          != base.min_p)          { entry["min_p"]          = params.min_p; }
        if (params.repeat_penalty != base.repeat_penalty) { entry["repeat_penalty"] = tidy(params.repeat_penalty); }
        if (params.repeat_last_n  != base.repeat_last_n)  { entry["repeat_last_n"]  = params.repeat_last_n; }
        if (params.max_tokens     != base.max_tokens)     { entry["max_tokens"]     = params.max_tokens; }

        experts[std::string(info.id)] = std::move(entry);
    }

    const json doc{
        {"$schema_note",
         "BatBot config. Models live in \"models_dir\"; each expert names a file "
         "inside it. An absolute or ~-path is also accepted. Anything an expert "
         "leaves out is inherited from \"defaults\". Editable in the app with ctrl-s."},
        {"models_dir",    config.models_dir},
        {"system_prompt", config.system_prompt},
        {"router",        model_params_to_json(config.router)},
        {"defaults",      model_params_to_json(config.defaults, /*include_model=*/false)},
        {"experts",       experts},
        {"gpu", json{
            {"mode",      config.gpu.mode},
            {"priority",  config.gpu.priority},
            {"main_gpu",  config.gpu.main_gpu},
            {"gpu_only",  config.gpu.gpu_only},
            {"vram_only", config.gpu.vram_only},
        }},
        {"routing", json{
            {"min_confidence",       tidy(config.routing.min_confidence)},
            {"use_fallback_expert", config.routing.use_fallback_expert},
        }},
        {"ui", json{
            {"animation_ms",    config.ui.animation_ms},
            {"show_roundtable", config.ui.show_roundtable},
            {"unicode",         config.ui.unicode},
        }},
    };

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    // Write to a sibling temp file and rename, so an interrupted save cannot
    // leave the user with a truncated config and no way back into the app.
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp);
        if (!out) {
            return false;
        }
        out << doc.dump(2) << '\n';
        if (!out.good()) {
            return false;
        }
    }

    std::filesystem::rename(temp, file, ec);
    if (ec) {
        // Rename can fail across filesystems; fall back to a direct write.
        std::ofstream out(file);
        if (!out) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        out << doc.dump(2) << '\n';
        std::filesystem::remove(temp, ec);
        return out.good();
    }
    return true;
}

bool save_config(const Config& config) {
    return save_config(config, paths::config_file());
}

void write_default_config(const std::filesystem::path& file) {
    Config defaults;

    json experts = json::object();
    for (const SubjectInfo& info : all_subjects()) {
        // Only `model` per expert: everything else inherits from "defaults",
        // so filling a seat is a one-line edit.
        experts[std::string(info.id)] = json{{"model", ""}};
    }

    ModelParams router_defaults;
    router_defaults.n_ctx       = 4096;
    router_defaults.temperature = 0.0F;   // greedy: routing wants determinism
    router_defaults.max_tokens  = 16;

    const json doc{
        {"$schema_note",
         "BatBot config. Drop your GGUF files in \"models_dir\" and name one per "
         "expert below. An absolute or ~-path also works. Anything an expert "
         "leaves out is inherited from \"defaults\". Editable in the app with ctrl-s."},
        {"models_dir", paths::models_dir().string()},
        {"system_prompt", defaults.system_prompt},
        {"router", model_params_to_json(router_defaults)},
        {"defaults", model_params_to_json(defaults.defaults, /*include_model=*/false)},
        {"experts", experts},
        {"gpu", json{
            {"mode",      defaults.gpu.mode},
            {"priority",  defaults.gpu.priority},
            {"main_gpu",  defaults.gpu.main_gpu},
            {"gpu_only",  defaults.gpu.gpu_only},
            {"vram_only", defaults.gpu.vram_only},
        }},
        {"routing", json{
            {"min_confidence",       defaults.routing.min_confidence},
            {"use_fallback_expert", defaults.routing.use_fallback_expert},
        }},
        {"ui", json{
            {"animation_ms",    defaults.ui.animation_ms},
            {"show_roundtable", defaults.ui.show_roundtable},
            {"unicode",         defaults.ui.unicode},
        }},
    };

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::ofstream out(file);
    if (out) {
        out << doc.dump(2) << '\n';
    }
}

Config load_config(const std::filesystem::path& file, std::vector<std::string>& warnings) {
    Config config;

    if (!std::filesystem::exists(file)) {
        write_default_config(file);
        // A freshly written default has no models in it; the UI turns that into
        // a "point me at some GGUFs" screen rather than an error.
        config.router.n_ctx       = 4096;
        config.router.temperature = 0.0F;   // greedy: same prompt, same expert
        config.router.max_tokens  = 16;
        return config;
    }

    json doc;
    try {
        std::ifstream in(file);
        in >> doc;
    } catch (const json::exception& e) {
        warnings.emplace_back("could not parse " + file.string() + ": " + e.what()
                              + " -- falling back to built-in defaults");
        return config;
    }

    if (!doc.is_object()) {
        warnings.emplace_back(file.string() + " is not a JSON object -- using defaults");
        return config;
    }

    read_field(doc, "system_prompt", config.system_prompt, "config", warnings);
    read_field(doc, "models_dir",    config.models_dir,    "config", warnings);

    if (const auto it = doc.find("defaults"); it != doc.end() && it->is_object()) {
        read_model_params(*it, config.defaults, "defaults", warnings);
    }

    config.router = config.defaults;
    config.router.model.clear();
    config.router.path.clear();
    config.router.n_ctx       = 4096;
    config.router.temperature = 0.0F;   // greedy: same prompt, same expert
    config.router.max_tokens  = 16;
    if (const auto it = doc.find("router"); it != doc.end() && it->is_object()) {
        read_model_params(*it, config.router, "router", warnings);
    }

    if (const auto experts = doc.find("experts"); experts != doc.end() && experts->is_object()) {
        for (const SubjectInfo& info : all_subjects()) {
            const auto entry = experts->find(std::string(info.id));
            if (entry == experts->end() || !entry->is_object()) {
                continue;
            }
            ModelParams& params = config.experts[static_cast<std::size_t>(info.subject)];
            read_model_params(*entry, params, info.id, warnings);
            params.inherit_from(config.defaults);
        }
    }

    if (const auto gpu = doc.find("gpu"); gpu != doc.end() && gpu->is_object()) {
        read_field(*gpu, "mode",      config.gpu.mode,      "gpu", warnings);
        read_field(*gpu, "main_gpu",  config.gpu.main_gpu,  "gpu", warnings);
        read_field(*gpu, "gpu_only",  config.gpu.gpu_only,  "gpu", warnings);
        read_field(*gpu, "vram_only", config.gpu.vram_only, "gpu", warnings);
        if (const auto priority = gpu->find("priority");
            priority != gpu->end() && priority->is_array()) {
            config.gpu.priority.clear();
            for (const json& index : *priority) {
                if (index.is_number_integer()) {
                    config.gpu.priority.push_back(index.get<int>());
                }
            }
        }
    }

    if (const auto routing = doc.find("routing"); routing != doc.end() && routing->is_object()) {
        read_field(*routing, "min_confidence",       config.routing.min_confidence,
                   "routing", warnings);
        read_field(*routing, "use_fallback_expert", config.routing.use_fallback_expert,
                   "routing", warnings);
    }

    if (const auto ui = doc.find("ui"); ui != doc.end() && ui->is_object()) {
        read_field(*ui, "animation_ms",    config.ui.animation_ms,    "ui", warnings);
        read_field(*ui, "show_roundtable", config.ui.show_roundtable, "ui", warnings);
        read_field(*ui, "unicode",         config.ui.unicode,         "ui", warnings);
    }

    // Resolve every reference once, here, so nothing downstream has to think
    // about the models directory, `~`, or relative paths.
    config.resolve_models();

    // Warn about files that are not there, but keep them configured: the user
    // may be mid-download, or about to point the models directory elsewhere.
    const auto check = [&](const ModelParams& params, std::string_view label) {
        if (!params.model.empty() && !std::filesystem::exists(params.path)) {
            warnings.emplace_back(std::string(label) + ": model not found at " + params.path);
        }
    };
    check(config.router, "router");
    for (const SubjectInfo& info : all_subjects()) {
        check(config.experts[static_cast<std::size_t>(info.subject)], info.id);
    }

    return config;
}

Config load_config(std::vector<std::string>& warnings) {
    return load_config(paths::config_file(), warnings);
}

}  // namespace batbot
