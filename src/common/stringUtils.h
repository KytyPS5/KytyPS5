#ifndef KYTY_COMMON_STRING_UTILS_H_
#define KYTY_COMMON_STRING_UTILS_H_

#include "common/byteBuffer.h"
#include "common/common.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <codecvt>
#include <filesystem>
#include <fmt/format.h>
#include <locale>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Common {

constexpr uint32_t FIND_INVALID_INDEX = static_cast<uint32_t>(-1);

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

inline bool IsSpace(char c) {
	return std::isspace(static_cast<unsigned char>(c)) != 0;
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

inline bool ContainsStr(std::string_view text, std::string_view needle) {
	return text.find(needle) != std::string_view::npos;
}

inline bool StartsWith(std::string_view text, std::string_view prefix) {
	return text.starts_with(prefix);
}

inline bool StartsWith(std::string_view text, char prefix) {
	return !text.empty() && text.front() == prefix;
}

inline bool EndsWith(std::string_view text, std::string_view suffix) {
	return text.ends_with(suffix);
}

inline bool EndsWith(std::string_view text, char suffix) {
	return !text.empty() && text.back() == suffix;
}

inline bool ContainsChar(std::string_view text, char needle) {
	return text.find(needle) != std::string_view::npos;
}

inline bool IndexValid(std::string_view text, uint32_t index) {
	return index < text.size();
}

inline uint32_t FindIndex(std::string_view text, std::string_view needle, size_t from = 0) {
	const auto pos = text.find(needle, from);
	return pos == std::string_view::npos ? FIND_INVALID_INDEX : static_cast<uint32_t>(pos);
}

inline uint32_t FindIndex(std::string_view text, char needle, size_t from = 0) {
	const auto pos = text.find(needle, from);
	return pos == std::string_view::npos ? FIND_INVALID_INDEX : static_cast<uint32_t>(pos);
}

inline uint32_t FindLastIndex(std::string_view text, std::string_view needle,
                              size_t from = FIND_INVALID_INDEX) {
	if (needle.empty()) {
		return 0;
	}
	if (from == FIND_INVALID_INDEX || from > text.size()) {
		from = text.size();
	}
	const auto pos = text.rfind(needle, from);
	return pos == std::string_view::npos ? FIND_INVALID_INDEX : static_cast<uint32_t>(pos);
}

inline uint32_t FindLastIndex(std::string_view text, char needle,
                              size_t from = FIND_INVALID_INDEX) {
	if (from == FIND_INVALID_INDEX || from > text.size()) {
		from = text.size();
	}
	const auto pos = text.rfind(needle, from);
	return pos == std::string_view::npos ? FIND_INVALID_INDEX : static_cast<uint32_t>(pos);
}

inline std::string Mid(std::string_view text, size_t first, size_t count = std::string_view::npos) {
	if (first >= text.size()) {
		return {};
	}
	return std::string(text.substr(first, count));
}

inline std::string Left(std::string_view text, size_t count) {
	return Mid(text, 0, count);
}

inline std::string Right(std::string_view text, size_t count) {
	if (count >= text.size()) {
		return std::string(text);
	}
	return Mid(text, text.size() - count, count);
}

inline std::string RemoveLast(std::string_view text, size_t count) {
	if (count >= text.size()) {
		return {};
	}
	return Left(text, text.size() - count);
}

inline std::string RemoveFirst(std::string_view text, size_t count) {
	if (count >= text.size()) {
		return {};
	}
	return Right(text, text.size() - count);
}

inline std::string TrimRight(std::string_view text) {
	size_t end = text.size();
	while (end > 0 && IsSpace(text[end - 1])) {
		end--;
	}
	return std::string(text.substr(0, end));
}

inline std::string TrimLeft(std::string_view text) {
	size_t first = 0;
	while (first < text.size() && IsSpace(text[first])) {
		first++;
	}
	return std::string(text.substr(first));
}

inline std::string Trim(std::string_view text) {
	return TrimRight(TrimLeft(text));
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

inline std::string FilenameWithoutExtension(std::string_view text) {
	const auto pos = text.find_last_of('.');
	return pos == std::string_view::npos ? std::string(text) : std::string(text.substr(0, pos));
}

inline std::string ExtensionWithoutFilename(std::string_view text) {
	const auto pos = text.find_last_of('.');
	return pos == std::string_view::npos ? std::string() : std::string(text.substr(pos));
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

inline std::string Concat(const std::vector<std::string>& list, std::string_view sep) {
	std::string ret;
	for (size_t i = 0; i < list.size(); i++) {
		if (i != 0) {
			ret += sep;
		}
		ret += list[i];
	}
	return ret;
}

inline std::string Concat(const std::vector<std::string>& list, char sep) {
	return Concat(list, std::string_view(&sep, 1));
}

inline uint32_t ToUint32(std::string_view text, int base = 10) {
	uint32_t value = 0;
	std::from_chars(text.data(), text.data() + text.size(), value, base);
	return value;
}

inline uint64_t ToUint64(std::string_view text, int base = 10) {
	uint64_t value = 0;
	std::from_chars(text.data(), text.data() + text.size(), value, base);
	return value;
}

inline int32_t ToInt32(std::string_view text, int base = 10) {
	int32_t value = 0;
	std::from_chars(text.data(), text.data() + text.size(), value, base);
	return value;
}

inline int64_t ToInt64(std::string_view text, int base = 10) {
	int64_t value = 0;
	std::from_chars(text.data(), text.data() + text.size(), value, base);
	return value;
}

inline double ToDouble(std::string_view text) {
	return std::stod(std::string(text));
}

inline float ToFloat(std::string_view text) {
	return std::stof(std::string(text));
}

inline std::string SafeCsv(std::string_view text) {
	const bool add_space =
	    !text.empty() && (text.front() == '+' || text.front() == '=' || text.front() == '-');
	if (text.find('"') != std::string_view::npos || text.find(';') != std::string_view::npos ||
	    text.find('+') != std::string_view::npos || text.find('=') != std::string_view::npos ||
	    text.find('-') != std::string_view::npos) {
		return "\"" + std::string(add_space ? " " : "") +
		       ReplaceStr(std::string(text), "\"", "\"\"") + "\"";
	}
	return std::string(text);
}

inline std::string Utf16ToUtf8(const char16_t* utf16) {
	std::u16string                                                    input(utf16);
	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
	return convert.to_bytes(input);
}

inline ByteBuffer HexToBin(std::string_view text) {
	ByteBuffer ret;
	for (size_t i = 0; i + 1 < text.size(); i += 2) {
		ret.Add(static_cast<Byte>((ToUint32(text.substr(i, 2), 16) & 0xffu)));
	}
	return ret;
}

inline std::string HexFromBin(const ByteBuffer& bin) {
	std::string ret;
	ret.reserve(bin.Size() * 2);
	for (uint32_t i = 0; i < bin.Size(); i++) {
		ret += fmt::format("{:02X}", std::to_integer<uint8_t>(bin.At(i)));
	}
	return ret;
}

} // namespace Common

#endif /* KYTY_COMMON_STRING_UTILS_H_ */
