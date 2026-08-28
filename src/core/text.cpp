#include "batbot/core/text.hpp"

namespace batbot::detail {

int utf8_length(unsigned char lead) {
    if ((lead & 0x80U) == 0x00U) { return 1; }
    if ((lead & 0xE0U) == 0xC0U) { return 2; }
    if ((lead & 0xF0U) == 0xE0U) { return 3; }
    if ((lead & 0xF8U) == 0xF0U) { return 4; }
    return 1;
}

std::string take_complete_utf8(std::string& buffer) {
    std::size_t cut = buffer.size();

    // A sequence is at most 4 bytes, so only the last 4 can be incomplete.
    const std::size_t limit = buffer.size() > 4 ? buffer.size() - 4 : 0;
    for (std::size_t i = buffer.size(); i-- > limit;) {
        const auto byte = static_cast<unsigned char>(buffer[i]);
        if ((byte & 0xC0U) == 0x80U) {
            continue;  // continuation byte: keep walking back to the lead
        }
        const auto needed = static_cast<std::size_t>(utf8_length(byte));
        if (i + needed > buffer.size()) {
            cut = i;  // this sequence has not arrived in full yet
        }
        break;
    }

    std::string complete = buffer.substr(0, cut);
    buffer.erase(0, cut);
    return complete;
}

}  // namespace batbot::detail
