#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_KEY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_KEY_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Libs::Graphics::SystemUi {

/// A name that tells an element apart from every other in the frame, so the frame can keep its
/// state, such as a box's scroll position, from one frame to the next. It is hashed as it is made,
/// so it holds a number and never the name, and nothing it was made from needs to outlive it. Keys
/// are frame wide, as an id is on a web page, so two elements that keep state need two keys. A
/// default key is none. A std::string makes one written as Key(text), as it can't convert itself.
struct Key {
	// The hash, 0 for none. A named key is never 0.
	uint64_t value = 0;

	/// No key.
	constexpr Key() = default;

	/// The key called `name`, so `.key = "pane"` reads as the name it is.
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr Key(const char* name): Key(std::string_view(name)) {}

	/// The key called `name`.
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr Key(std::string_view name): value(Named(FoldText(FNV_OFFSET, name))) {}

	/// Refused, as a literal 0 or null would otherwise be taken as a name and read through a null
	/// pointer. No key is Key() or {}.
	Key(int)            = delete;
	Key(std::nullptr_t) = delete;

	/// The key called `name` with `index`, for one of many built in a loop, as Key("row", 3).
	constexpr Key(std::string_view name, uint64_t index)
	    : value(Named(FoldNumber(FoldText(FNV_OFFSET, name), index))) {}

	/// A key made from this one and `part`, so one element can keep several kinds of state apart,
	/// as a box keeps its scroll position under With("scroll").
	[[nodiscard]] constexpr Key With(std::string_view part) const {
		Key derived;
		derived.value = Named(FoldText(FoldNumber(FNV_OFFSET, value), part));
		return derived;
	}

	/// Whether this is no key.
	[[nodiscard]] constexpr bool IsNone() const { return value == 0; }

	/// Whether two keys are the same, made from the same name, index and parts.
	constexpr bool operator==(const Key&) const = default;

private:
	// A 64 bit FNV-1a hash, small and good enough to tell a screen's keys apart.
	static constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
	static constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
	// What a name that hashes to 0 takes instead, as 0 is none.
	static constexpr uint64_t ZERO_STANDIN = 1;
	// The bits in a byte, for folding a number in a byte at a time.
	static constexpr uint64_t BYTE_BITS = 8;
	static constexpr uint64_t BYTE_MASK = 0xFF;

	// `hash` with each character of `text` folded in.
	static constexpr uint64_t FoldText(uint64_t hash, std::string_view text) {
		for (const char character: text) {
			hash = (hash ^ static_cast<unsigned char>(character)) * FNV_PRIME;
		}
		return hash;
	}

	// `hash` with each byte of `number` folded in, lowest first, so a key is the same on every
	// platform.
	static constexpr uint64_t FoldNumber(uint64_t hash, uint64_t number) {
		for (size_t byte = 0; byte < sizeof(number); ++byte) {
			hash = (hash ^ ((number >> (byte * BYTE_BITS)) & BYTE_MASK)) * FNV_PRIME;
		}
		return hash;
	}

	// `hash` as a named key's value, never 0.
	static constexpr uint64_t Named(uint64_t hash) { return hash == 0 ? ZERO_STANDIN : hash; }
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_KEY_H_
