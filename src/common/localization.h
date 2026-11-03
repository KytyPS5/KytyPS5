#pragma once

#include <string_view>

namespace Common::Localization {

// Console language indices follow the system configuration; invalid indices use English.
[[nodiscard]] std::string_view LocaleName(int console_language);

// Translations have static lifetime. Missing translations use English, then the supplied key.
[[nodiscard]] std::string_view Get(int console_language, std::string_view key);

} // namespace Common::Localization
