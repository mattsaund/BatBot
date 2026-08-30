// SPDX-License-Identifier: MIT
//
// Turning the GPU split setting into the numbers llama.cpp wants.
//
// Kept apart from Config because it needs the device list, which does not
// exist until a runtime has been loaded -- so this runs on the engine thread
// at load time, not when the config is parsed.
#pragma once

#include <string>

#include "batbot/config/config.hpp"

namespace batbot {

/// Translate the machine-wide GPU settings into per-model load parameters.
///
/// Two independent things happen here:
///
///   * The memory policy -- GpuConfig::gpu_only and ::vram_only -- is stamped
///     onto every model, whatever the split mode is. Whether the processor
///     does any of the computing is not a question about how many cards there
///     are.
///   * `tensor_split` and `main_gpu` are filled in from the split mode. That
///     part is a no-op in "auto" mode, which leaves any hand-written
///     `tensor_split` in the config file exactly as it was.
///
/// Returns a one-line description of the split that was applied, for the log
/// and the settings screen, or an empty string when the split was left alone.
std::string apply_gpu_policy(Config& config);

}  // namespace batbot
