#include "common/trophyStrings.h"

#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <vector>

namespace Common::Trophies {
namespace {

constexpr uintmax_t MaxStringsFileSize = uintmax_t {1} << 20u;

constexpr std::array<std::pair<std::string_view, std::string_view>, 26> DefaultTexts = {{
    {"trophy.toast.earned", "Trophy earned!"},
    {"trophy.grade.platinum", "Platinum"},
    {"trophy.grade.gold", "Gold"},
    {"trophy.grade.silver", "Silver"},
    {"trophy.grade.bronze", "Bronze"},
    {"trophy.hidden", "Hidden trophy"},
    {"trophy.numbered", "Trophy {0}"},
    {"trophy.viewer.window_title", "Trophy Viewer"},
    {"trophy.viewer.window_title_game", "Trophy Viewer - {0}"},
    {"trophy.viewer.progress", "Progress"},
    {"trophy.viewer.earned", "Earned"},
    {"trophy.viewer.all_trophies", "All trophies: {0}"},
    {"trophy.viewer.no_package", "No trophy package found in {0}."},
    {"trophy.viewer.unreadable_package", "Could not read trophy package {0}."},
    {"trophy.overview.title", "Trophies"},
    {"trophy.overview.total", "Total {0}"},
    {"trophy.overview.earned", "Earned {0}/{1}"},
    {"trophy.overview.empty", "No games with trophies found in the game folders."},
    {"trophy.inspector.title", "Trophy"},
    {"trophy.inspector.grade", "Grade"},
    {"trophy.inspector.status", "Status"},
    {"trophy.inspector.status_earned", "Earned"},
    {"trophy.inspector.status_not_earned", "Not earned"},
    {"trophy.inspector.earned_date", "Earned Date"},
    {"trophy.inspector.details", "Details"},
    {"trophy.inspector.show_hidden", "Show hidden information"},
}};

constexpr std::array<std::string_view, 30> Locales = {
    "ja-JP", "en-US", "fr-FR",   "es-ES",   "de-DE", "it-IT", "nl-NL", "pt-PT",
    "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO",
    "pl-PL", "pt-BR", "en-GB",   "tr-TR",   "es-419", "ar-AE", "fr-CA", "cs-CZ",
    "hu-HU", "el-GR", "ro-RO",   "th-TH",   "vi-VN", "id-ID"};

} // namespace

std::string_view LocaleName(int console_language) {
	return Locales[console_language >= 0 && console_language < static_cast<int>(Locales.size())
	                   ? static_cast<size_t>(console_language)
	                   : 1];
}

Strings::Strings() {
	for (const auto& [key, text]: DefaultTexts) {
		m_texts.emplace(key, text);
	}
}

bool Strings::Merge(const std::filesystem::path& file) {
	std::error_code error;
	const auto      size = std::filesystem::file_size(file, error);
	if (error || size > MaxStringsFileSize) {
		return false;
	}
	std::ifstream stream(file, std::ios::binary);
	if (!stream) {
		return false;
	}
	const std::string content((std::istreambuf_iterator<char>(stream)),
	                          std::istreambuf_iterator<char>());
	// Tolerate a UTF-8 byte order mark written by some editors.
	const std::string_view text =
	    content.starts_with("\xEF\xBB\xBF") ? std::string_view(content).substr(3) : content;
	const auto json = nlohmann::json::parse(text, nullptr, false);
	if (!json.is_object()) {
		return false;
	}
	for (const auto& [key, value]: json.items()) {
		if (value.is_string() && !key.empty() && key[0] != '_') {
			m_texts[key] = value.get<std::string>();
		}
	}
	return true;
}

Strings Strings::Load(const std::filesystem::path& directory, int console_language) {
	Strings    strings;
	const auto locale = LocaleName(console_language);
	strings.Merge(directory / "en-US.json");
	if (locale != "en-US") {
		strings.Merge(directory / (std::string(locale) + ".json"));
	}
	return strings;
}
std::string Strings::Get(std::string_view key) const {
	const auto it = m_texts.find(key);
	return it != m_texts.end() ? it->second : std::string(key);
}

std::string Strings::Format(std::string_view                    key,
                            const std::vector<std::string_view>& args) const {
	const auto  text = Get(key);
	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '{') {
			const auto close = text.find('}', i);
			if (close != std::string::npos && close > i + 1) {
				size_t     index = 0;
				const auto begin = text.data() + i + 1;
				const auto end   = text.data() + close;
				const auto [stop, parse_error] = std::from_chars(begin, end, index);
				if (parse_error == std::errc {} && stop == end) {
					if (index < args.size()) {
						result.append(args[index]);
					}
					i = close;
					continue;
				}
			}
		}
		result.push_back(text[i]);
	}
	return result;
}

} // namespace Common::Trophies
