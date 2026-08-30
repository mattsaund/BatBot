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

/// Fill in `tensor_split` and `main_gpu` on every model in `config` from the
/// machine's actual GPUs and the chosen split mode.
///
/// A no-op in "auto" mode, which leaves any hand-written `tensor_split` in the
/// config file exactly as it was. Returns a one-line description of what was
/// applied, for the log and the settings screen, or an empty string when
/// nothing was changed.
std::string apply_gpu_policy(Config& config);

}  // namespace batbot
