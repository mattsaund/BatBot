// XDG-aware locations for BatBot's own files.
#pragma once

#include <filesystem>
#include <string>

namespace batbot::paths {

/// $XDG_CONFIG_HOME/batbot, falling back to ~/.config/batbot.
std::filesystem::path config_dir();

/// $XDG_DATA_HOME/batbot, falling back to ~/.local/share/batbot.
std::filesystem::path data_dir();

/// config_dir()/config.json
std::filesystem::path config_file();

/// config_dir()/trust.json
std::filesystem::path trust_file();

/// data_dir()/models -- the default place BatBot looks for GGUF files.
std::filesystem::path models_dir();

/// data_dir()/batbot.log -- where llama.cpp's chatter is redirected so it
/// cannot scribble over the TUI.
std::filesystem::path log_file();

/// Expand a leading `~` and resolve to an absolute path. Does not require the
/// path to exist, so it is safe to call on a model path the user has not
/// downloaded yet.
std::filesystem::path expand_user(std::string_view raw);

}  // namespace batbot::paths
