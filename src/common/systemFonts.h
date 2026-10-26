#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace Common {

// Host fonts that cover CJK (Chinese, Japanese, Korean) glyphs, ordered by preference for the
// given console locale (see Common::Localization::LocaleName), so shared Han characters use the
// regional glyph forms. KYTY_CJK_FONT, when set, is tried first. Only existing regular files are
// returned. TrueType collections (.ttc) are included; callers should use face index 0.
std::vector<std::filesystem::path> FindCjkFontFiles(std::string_view locale);

} // namespace Common
