#ifndef KYTY_COMMON_STRING_UTILS_H_
#define KYTY_COMMON_STRING_UTILS_H_

#include "common/common.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <codecvt>
#include <filesystem>
#include <locale>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Common {

inline std::string PathToString(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
	auto u8 = path.u8string();
	return {u8.begin(), u8.end()};
#else
	return path.u8string();
#endif
}

inline std::string PathToGenericString(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
	auto u8 = path.generic_u8string();
	return {u8.begin(), u8.end()};
#else
	return path.generic_u8string();
#endif
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
#if defined(__cpp_char8_t)
	return std::filesystem::path(std::u8string(text.begin(), text.end()));
#else
	return std::filesystem::u8path(text.begin(), text.end());
#endif
}

inline bool EqualNoCase(std::string_view lhs, std::string_view rhs) {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (size_t i = 0; i < lhs.size(); i++) {
		auto l = static_cast<unsigned char>(lhs[i]);
		auto r = static_cast<unsigned char>(rhs[i]);
		if (std::tolower(l) != std::tolower(r)) {
			return false;
		}
	}
	return true;
}

inline std::string ToLower(std::string_view text) {
	std::string ret(text);
	std::transform(ret.begin(), ret.end(), ret.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ret;
}

inline std::string Mid(std::string_view text, size_t first, size_t count = std::string_view::npos) {
	if (first >= text.size()) {
		return {};
	}
	return std::string(text.substr(first, count));
}

inline std::string RemoveLast(std::string_view text, size_t count) {
	if (count >= text.size()) {
		return {};
	}
	return std::string(text.substr(0, text.size() - count));
}

inline std::string RemoveFirst(std::string_view text, size_t count) {
	if (count >= text.size()) {
		return {};
	}
	return std::string(text.substr(count));
}

inline std::string ReplaceChar(std::string_view text, char old_char, char new_char) {
	std::string ret(text);
	std::replace(ret.begin(), ret.end(), old_char, new_char);
	return ret;
}

inline std::string ReplaceStr(std::string text, std::string_view old_str,
                              std::string_view new_str) {
	if (old_str.empty()) {
		return text;
	}

	size_t pos = 0;
	while ((pos = text.find(old_str, pos)) != std::string::npos) {
		text.replace(pos, old_str.size(), new_str);
		pos += new_str.size();
	}
	return text;
}

inline std::string DirectoryWithoutFilename(std::string_view text) {
	const auto pos = text.find_last_of('/');
	return pos == std::string_view::npos ? std::string() : std::string(text.substr(0, pos + 1));
}

inline std::string FilenameWithoutDirectory(std::string_view text) {
	const auto pos = text.find_last_of('/');
	return pos == std::string_view::npos ? std::string(text) : std::string(text.substr(pos + 1));
}

inline std::string FixFilenameSlash(std::string_view text) {
	return ReplaceChar(text, '\\', '/');
}

inline std::string FixDirectorySlash(std::string_view text) {
	auto ret = FixFilenameSlash(text);
	if (!ret.ends_with('/')) {
		ret += '/';
	}
	return ret;
}

inline std::vector<std::string> Split(std::string_view text, std::string_view sep,
                                      bool keep_empty = false) {
	std::vector<std::string> ret;
	if (sep.empty()) {
		for (char c: text) {
			ret.emplace_back(1, c);
		}
		return ret;
	}

	size_t start = 0;
	for (;;) {
		const auto pos = text.find(sep, start);
		if (pos == std::string_view::npos) {
			break;
		}
		if (keep_empty || pos != start) {
			ret.emplace_back(text.substr(start, pos - start));
		}
		start = pos + sep.size();
	}
	if (keep_empty || start != text.size()) {
		ret.emplace_back(text.substr(start));
	}
	return ret;
}

inline std::vector<std::string> Split(std::string_view text, char sep, bool keep_empty = false) {
	return Split(text, std::string_view(&sep, 1), keep_empty);
}

inline int32_t ToInt32(std::string_view text, int base = 10) {
	int32_t value = 0;
	std::from_chars(text.data(), text.data() + text.size(), value, base);
	return value;
}

inline std::string Utf16ToUtf8(std::u16string_view utf16) {
	if (utf16.empty()) {
		return {};
	}
	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
	return convert.to_bytes(utf16.data(), utf16.data() + utf16.size());
}

} // namespace Common

#endif /* KYTY_COMMON_STRING_UTILS_H_ */
