#include "common/localization.h"

#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>

#include "localizationData.h"

namespace Common::Localization {
namespace {

constexpr std::array<std::string_view, 30> Locales = {
    "ja-JP", "en-US", "fr-FR",   "es-ES",   "de-DE", "it-IT", "nl-NL", "pt-PT",
    "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO",
    "pl-PL", "pt-BR", "en-GB",   "tr-TR",   "es-419", "ar-AE", "fr-CA", "cs-CZ",
    "hu-HU", "el-GR", "ro-RO",   "th-TH",   "vi-VN", "id-ID"};

size_t LanguageIndex(int console_language) {
	return console_language >= 0 && console_language < static_cast<int>(Locales.size())
	           ? static_cast<size_t>(console_language)
	           : 1;
}

} // namespace

std::string_view LocaleName(int console_language) {
	return Locales[LanguageIndex(console_language)];
}

std::string_view Get(int console_language, std::string_view key) {
	static const auto catalogs = [] {
		std::array<nlohmann::json, Locales.size()> result;
		for (size_t i = 0; i < Locales.size(); ++i) {
			const auto bytes = EmbeddedCatalog(Locales[i]);
			result[i] = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
		}
		return result;
	}();
	for (const auto index: std::array {LanguageIndex(console_language), size_t {1}}) {
		const auto& catalog = catalogs[index];
		const auto  found   = catalog.find(key);
		if (found != catalog.end() && found->is_string()) {
			const auto& text = found->get_ref<const std::string&>();
			if (!text.empty()) {
				return text;
			}
		}
	}
	return key;
}

} // namespace Common::Localization
