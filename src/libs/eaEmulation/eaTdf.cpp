#include "libs/eaEmulation/eaTdf.h"

#include <cctype>
#include <fmt/format.h>

namespace Libs::EaEmulation {

namespace {

std::string TagName(uint32_t value) {
	std::string name;
	for (int shift = 18; shift >= 0; shift -= 6) name += static_cast<char>(((value >> shift) & 0x3f) + 0x20);
	while (!name.empty() && name.back() == ' ') name.pop_back();
	return name;
}

bool IsSecretKey(const std::string& key) {
	std::string lower;
	for (const char c: key) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	for (const char* word: {"secret", "key", "token", "password"}) {
		if (lower.find(word) != std::string::npos) return true;
	}
	return false;
}

// Walks a payload. `blank`, when set, is the same bytes as the reader, and gets the filler.
struct Walker {
	Reader   reader;
	uint8_t* blank   = nullptr;
	bool     dump    = true;
	size_t   blanked = 0;
	std::string out;

	void Line(int depth, const std::string& text) {
		if (dump) out += std::string(static_cast<size_t>(depth) * 2, ' ') + text + "\n";
	}

	// Reads a text value, returns where it starts and how long it is (without the end 0).
	std::string Text(size_t* start = nullptr) {
		const auto length = reader.Var();
		if (length < 1 || static_cast<uint64_t>(length) > reader.size - reader.at) {
			reader.ok = false;
			return {};
		}
		if (start != nullptr) *start = reader.at;
		std::string text(reinterpret_cast<const char*>(reader.data + reader.at), static_cast<size_t>(length) - 1);
		reader.at += static_cast<size_t>(length);
		return text;
	}

	std::string Value(uint8_t type, int depth) {
		if (depth > 16) {
			reader.ok = false;
			return {};
		}
		switch (type) {
			case Int:
			case Time: return fmt::format("{}", reader.Var());
			case Str: return fmt::format("\"{}\"", Text());
			case Bin: {
				const auto length = reader.Var();
				if (length < 0 || static_cast<uint64_t>(length) > reader.size - reader.at) {
					reader.ok = false;
					return {};
				}
				std::string hex;
				for (int64_t i = 0; i < length; i++) hex += fmt::format("{:02x}", reader.data[reader.at + static_cast<size_t>(i)]);
				reader.at += static_cast<size_t>(length);
				return "bin " + hex;
			}
			case Struct: {
				Line(depth, "{");
				Members(depth + 1, true);
				Line(depth, "}");
				return "struct";
			}
			case List: {
				const uint8_t item  = reader.Byte();
				const auto    count = reader.Var();
				Line(depth, fmt::format("list of {} x{}", item, count));
				for (int64_t i = 0; i < count && reader.ok; i++) Entry(depth + 1, item, "-");
				return "list";
			}
			case Map: {
				const uint8_t key_type   = reader.Byte();
				const uint8_t value_type = reader.Byte();
				const auto    count      = reader.Var();
				Line(depth, fmt::format("map {} -> {} x{}", key_type, value_type, count));
				for (int64_t i = 0; i < count && reader.ok; i++) {
					if (key_type == Str && value_type == Str) {
						const auto key = Text();
						size_t     start = 0;
						const auto value = Text(&start);
						if (blank != nullptr && reader.ok && !value.empty() && IsSecretKey(key)) {
							std::fill_n(blank + start, value.size(), static_cast<uint8_t>('x'));
							blanked++;
						}
						Line(depth + 1, fmt::format("\"{}\" = \"{}\"", key, value));
					} else {
						Entry(depth + 1, key_type, "key");
						Entry(depth + 1, value_type, "value");
					}
				}
				return "map";
			}
			case Union: {
				if (reader.Byte() == 0x7f) return "union (none)";
				const auto tag = reader.TagValue();
				Entry(depth, reader.Byte(), "union " + TagName(tag));
				return "union";
			}
			case ObjType: {
				const auto a = reader.Var();
				const auto b = reader.Var();
				return fmt::format("objtype {}/{}", a, b);
			}
			case ObjId: {
				const auto a = reader.Var();
				const auto b = reader.Var();
				const auto c = reader.Var();
				return fmt::format("objid {}/{}/{}", a, b, c);
			}
			case Float: {
				uint32_t bits = 0;
				for (int i = 0; i < 4; i++) bits = (bits << 8) | reader.Byte();
				return fmt::format("float bits 0x{:08x}", bits);
			}
			default: reader.ok = false; return {};
		}
	}

	// A value that is printed on one line, unless it is a group (then its lines follow).
	void Entry(int depth, uint8_t type, const std::string& label) {
		if (type == Struct || type == List || type == Map) {
			Line(depth, label + ":");
			Value(type, depth + 1);
		} else {
			const auto text = Value(type, depth);
			Line(depth, label + " " + text);
		}
	}

	void Members(int depth, bool until_zero) {
		while (reader.ok && reader.at < reader.size) {
			if (until_zero && reader.data[reader.at] == 0) {
				reader.at++;
				return;
			}
			const auto tag  = reader.TagValue();
			const auto type = reader.Byte();
			Entry(depth, type, TagName(tag) + " (type " + std::to_string(type) + ")");
		}
		if (until_zero) reader.ok = false;
	}
};

// Leaves `reader` at the value of `member` of the top-level struct `outer` when it has type `type`.
bool SeekMember(Reader& reader, uint32_t outer, uint32_t member, uint8_t type) {
	while (reader.ok && reader.at < reader.size) {
		const uint32_t tag        = reader.TagValue();
		const uint8_t  value_type = reader.Byte();
		if (tag == outer && value_type == Struct) {
			while (reader.ok && reader.at < reader.size && reader.data[reader.at] != 0) {
				const uint32_t member_tag  = reader.TagValue();
				const uint8_t  member_type = reader.Byte();
				if (member_tag == member) return reader.ok && member_type == type;
				reader.SkipValue(member_type);
			}
			return false;
		}
		reader.SkipValue(value_type);
	}
	return false;
}

} // namespace

int64_t IntegerMember(const std::vector<uint8_t>& payload, uint32_t outer, uint32_t member, int64_t fallback) {
	Reader reader {payload.data(), payload.size()};
	if (!SeekMember(reader, outer, member, Int)) return fallback;
	const auto value = reader.Var();
	return reader.ok ? value : fallback;
}

std::string TextMember(const std::vector<uint8_t>& payload, uint32_t outer, uint32_t member,
                       const std::string& fallback) {
	Reader reader {payload.data(), payload.size()};
	if (!SeekMember(reader, outer, member, Str)) return fallback;
	const auto length = reader.Var();
	if (!reader.ok || length < 1 || static_cast<size_t>(length) > reader.size - reader.at) return fallback;
	std::string text(reinterpret_cast<const char*>(reader.data + reader.at), static_cast<size_t>(length));
	while (!text.empty() && text.back() == 0) text.pop_back();
	return text;
}

std::string DumpTdf(const uint8_t* data, size_t size) {
	Walker walker;
	walker.reader = Reader {data, size};
	walker.Members(0, false);
	if (!walker.reader.ok) {
		std::string hex;
		for (size_t i = 0; i < size; i++) hex += fmt::format("{:02x}", data[i]);
		walker.out += "undecoded from byte " + std::to_string(walker.reader.at) + ": " + hex + "\n";
	}
	return walker.out;
}

size_t BlankSecrets(std::vector<uint8_t>& payload) {
	const std::vector<uint8_t> original = payload;
	Walker                     walker;
	walker.reader = Reader {original.data(), original.size()};
	walker.blank  = payload.data();
	walker.dump   = false;
	walker.Members(0, false);
	return walker.reader.ok ? walker.blanked : 0;
}

} // namespace Libs::EaEmulation
