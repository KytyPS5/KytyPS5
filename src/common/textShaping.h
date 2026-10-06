#pragma once

#include <string>
#include <string_view>

namespace Common::Trophies {

// Converts a logical-order UTF-8 line into the form a renderer without text shaping can draw.
// Arabic letters are replaced by their joined presentation forms and the line is reordered
// right-to-left, keeping Latin text and digits readable. Text without Arabic is returned unchanged.
[[nodiscard]] std::string PrepareDisplayText(std::string_view utf8);

} // namespace Common::Trophies
