#pragma once

#include <filesystem>
#include <vector>
#include <map>
#include <string>
#include <string_view>

// Translatable text for the trophy system (toast, overview and viewer).
//
// Every language lives in its own UTF-8 JSON file named after the console locale, for example
// "en-US.json" or "es-ES.json", inside assets/localization. A file is a flat object that maps
// a string key to its text. Placeholders are written as {0}, {1}, ... Missing keys fall back to
// en-US.json and then to the built-in English text, so a partial translation still works.
namespace Common::Trophies {

inline constexpr const char* StringsDirectory = "assets/localization";

// Locale code (for example "es-ES") of a console language index.
[[nodiscard]] std::string_view LocaleName(int console_language);

class Strings {
public:
	// Built-in English only.
	Strings();

	[[nodiscard]] static Strings Load(const std::filesystem::path& directory, int console_language);

	[[nodiscard]] std::string Get(std::string_view key) const;
	[[nodiscard]] std::string Format(std::string_view                    key,
	                                 const std::vector<std::string_view>& args) const;

	// Applies the string table of one JSON file. Returns false if the file is missing or invalid.
	bool Merge(const std::filesystem::path& file);

private:
	std::map<std::string, std::string, std::less<>> m_texts;
};

} // namespace Common::Trophies
