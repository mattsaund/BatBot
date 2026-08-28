// The models directory: one folder holding every GGUF, with the config saying
// which file plays which role.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace batbot {

/// One GGUF sitting in the models directory.
struct ModelFile {
    std::string           name;   ///< file name, which is what the config stores
    std::filesystem::path path;   ///< absolute path on disk
    std::uintmax_t        bytes = 0;

    /// "4.1 GB", for the picker.
    std::string size_label() const;
};

/// Every *.gguf directly inside `dir`, sorted by name. A missing or unreadable
/// directory yields an empty list rather than an error: an empty models folder
/// is a normal first-run state, not a failure.
std::vector<ModelFile> scan_models(const std::filesystem::path& dir);

/// Turn a config reference into an absolute path.
///
/// A bare file name ("physics-q4.gguf") is looked up inside the models
/// directory, which is the normal case. A reference starting with `/` or `~`
/// is used as-is, so a model can still live anywhere on the system without
/// being moved into the folder.
std::filesystem::path resolve_model_ref(const std::filesystem::path& models_dir,
                                        std::string_view reference);

/// True when `reference` names a file inside the models directory rather than
/// pointing somewhere else on disk.
bool is_bare_name(std::string_view reference);

}  // namespace batbot
