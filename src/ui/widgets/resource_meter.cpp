// SPDX-License-Identifier: MIT
//
// See resource_meter.hpp.
#include "batbot/ui/widgets/resource_meter.hpp"

#include <algorithm>
#include <string>

#include "batbot/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace batbot::ui {
namespace {

/// Width of the name column. Long enough for "GeForce RTX 5060 Ti" once the
/// vendor is dropped, short enough to leave the numbers room.
constexpr int kNameWidth = 19;

/// Green until it is worth noticing, then amber, then red. The thresholds are
/// where the thing being measured starts to matter: video memory near full is
/// the next model failing to load, and a card at ninety degrees is throttling.
Color pressure_color(int percent, int warn, int hot) {
    if (percent < 0)    { return theme::kMeta; }
    if (percent >= hot) { return theme::kError; }
    if (percent >= warn){ return theme::kSeatLoading; }
    return theme::kSeatActive;
}

/// "NVIDIA GeForce RTX 4070" is a brand and a part. Only one of them is
/// different from the card next to it.
std::string short_name(std::string name) {
    // Anywhere in the string, not just at the front: "12th Gen Intel Core
    // i5-12400" carries its vendor in the middle.
    for (const std::string_view vendor : {"NVIDIA ", "GeForce ", "Intel ", "Core ", "AMD "}) {
        for (std::size_t at = name.find(vendor); at != std::string::npos;
             at = name.find(vendor)) {
            name.erase(at, vendor.size());
        }
    }
    // What is left is still too long on some parts. Keep the end of it: the
    // model number is what distinguishes one from the next, and it is always
    // last.
    if (static_cast<int>(name.size()) > kNameWidth) {
        name.erase(0, name.size() - static_cast<std::size_t>(kNameWidth));
    }
    return name;
}

/// A right-aligned percentage, or two spaces and a dash when it is not known.
Element percent_cell(int percent, int warn, int hot) {
    const std::string text_value = percent < 0 ? "  -" : std::to_string(percent) + "%";
    return hbox({filler(), text(text_value) | color(pressure_color(percent, warn, hot))})
         | size(WIDTH, EQUAL, 4);
}

Element row(const util::ResourceSample& sample, std::string_view memory_label) {
    const std::string degrees =
        sample.temperature_c < 0 ? "   -" : std::to_string(sample.temperature_c) + "°";
    return hbox({
        text(short_name(sample.name)) | size(WIDTH, EQUAL, kNameWidth) | color(theme::kMeta),
        text(" ") ,
        text(std::string(memory_label)) | color(theme::kMeta) | dim,
        percent_cell(sample.memory_percent(), 80, 93),
        text("  "),
        percent_cell(sample.busy_percent, 101, 101),  // busy is information, not alarm
        text(" "),
        hbox({filler(), text(degrees)})
            | size(WIDTH, EQUAL, 5)
            | color(pressure_color(sample.temperature_c, 75, 87)),
    });
}

}  // namespace

Element resource_meter(const util::ResourceSnapshot& snapshot) {
    if (!snapshot.ready) {
        return text("");
    }
    Elements rows;
    for (const util::ResourceSample& gpu : snapshot.gpus) {
        rows.push_back(row(gpu, "vram"));
    }
    if (!snapshot.cpu.name.empty()) {
        rows.push_back(row(snapshot.cpu, " ram"));
    }
    if (rows.empty()) {
        return text("");
    }
    return vbox(std::move(rows));
}

}  // namespace batbot::ui
