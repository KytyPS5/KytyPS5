#ifndef KYTY_LOADER_GUEST_CODE_H_
#define KYTY_LOADER_GUEST_CODE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Loader {
struct Program;
namespace GuestCode {

// Code as every build of a game seen has it: hex bytes, "??" for a byte a build may change (call
// and jump displacements, RIP-relative addresses, stack frame sizes), e.g.
// "55 48 89 e5 48 81 ec ?? ?? ?? ??".
class Pattern {
public:
	explicit Pattern(std::string_view text) {
		const auto digit = [](char c) {
			return c >= '0' && c <= '9'   ? c - '0'
			       : c >= 'a' && c <= 'f' ? c - 'a' + 10
			       : c >= 'A' && c <= 'F' ? c - 'A' + 10
			                              : -1;
		};
		for (size_t i = 0; i < text.size();) {
			if (text[i] == ' ') {
				i++;
				continue;
			}
			if (i + 1 >= text.size()) {
				Invalid();
				return;
			}
			if (text[i] == '?' && text[i + 1] == '?') {
				m_bytes.push_back(0);
				m_mask.push_back(0);
			} else if (digit(text[i]) >= 0 && digit(text[i + 1]) >= 0) {
				m_bytes.push_back(static_cast<uint8_t>(digit(text[i]) * 16 + digit(text[i + 1])));
				m_mask.push_back(0xff);
			} else {
				Invalid();
				return;
			}
			i += 2;
		}
		for (size_t i = 0; i < m_mask.size();) {
			size_t run = 0;
			while (i + run < m_mask.size() && m_mask[i + run] != 0) {
				run++;
			}
			if (run > m_anchor_size) {
				m_anchor      = i;
				m_anchor_size = run;
			}
			i += run + 1;
		}
	}

	[[nodiscard]] bool   Valid() const { return m_anchor_size != 0; }
	[[nodiscard]] size_t Size() const { return m_bytes.size(); }
	// `code` holds at least Size() bytes.
	[[nodiscard]] bool Matches(const uint8_t* code) const {
		for (size_t i = 0; i < m_bytes.size(); i++) {
			if ((code[i] & m_mask[i]) != m_bytes[i]) {
				return false;
			}
		}
		return true;
	}
	// The offsets in `code` where the pattern starts, at most `limit` of them.
	[[nodiscard]] std::vector<size_t> Find(std::span<const uint8_t> code, size_t limit) const {
		std::vector<size_t> found;
		if (!Valid() || code.size() < m_bytes.size()) {
			return found;
		}
		const auto*                              anchor = m_bytes.data() + m_anchor;
		const std::boyer_moore_horspool_searcher searcher(anchor, anchor + m_anchor_size);
		// The anchor of a match lies within [m_anchor, size - (Size() - m_anchor)) of `code`.
		const auto* begin = code.data() + m_anchor;
		const auto* end   = code.data() + code.size() - (m_bytes.size() - m_anchor - m_anchor_size);
		for (const auto* at = std::search(begin, end, searcher); at != end && found.size() < limit;
		     at             = std::search(at + 1, end, searcher)) {
			if (Matches(at - m_anchor)) {
				found.push_back(static_cast<size_t>(at - m_anchor - code.data()));
			}
		}
		return found;
	}

private:
	void Invalid() {
		m_bytes.clear();
		m_mask.clear();
	}

	std::vector<uint8_t> m_bytes, m_mask;                 // mask 0xff: the byte must match
	size_t               m_anchor = 0, m_anchor_size = 0; // the longest run without "??"
};

// Whether the host can read [address, address + size).
bool Readable(uint64_t address, uint64_t size);
// The addresses where the pattern starts in the program's executable segments, at most `limit`
// of them.
std::vector<uint64_t> Find(const Program& program, const Pattern& pattern, size_t limit);
// The one place the pattern starts in the program's code; nothing when there are none or several.
std::optional<uint64_t> FindUnique(const Program& program, const Pattern& pattern);

} // namespace GuestCode
} // namespace Loader

#endif /* KYTY_LOADER_GUEST_CODE_H_ */
