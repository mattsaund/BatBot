// SPDX-License-Identifier: MIT
//
// Turning numbers and strings into things a person reads at a glance.
//
// Small, but worth having in one place: three separate byte formatters had
// drifted apart across the codebase before this existed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace crucible::format {

/// Strip leading and trailing whitespace.
std::string trim(std::string text);

/// A double at fixed precision: `number(18.34567, 1)` is `"18.3"`.
std::string number(double value, int precision);

/// Milliseconds as a duration a person reads: `"840ms"`, `"3.2s"`.
std::string duration_ms(long milliseconds);

/// A byte count in the largest unit that keeps it readable: `"469 MB"`,
/// `"1.2 GB"`. Whole numbers below a kilobyte.
std::string bytes(std::uintmax_t count);

/// A path with the home directory written as `~`.
///
/// Panel titles sit beside the directory they are about, and Crucible's own
/// directories are four levels below $HOME -- spelling one out in full pushes
/// the title off its own header on any normal terminal.
std::string short_path(const std::filesystem::path& path);

}  // namespace crucible::format
