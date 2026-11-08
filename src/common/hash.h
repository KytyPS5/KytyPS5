#ifndef KYTY_COMMON_HASH_H_
#define KYTY_COMMON_HASH_H_

#include <cstdint>

namespace Common {

// FNV-1a over 64 bits: start from FNV64_OFFSET and mix in one value per step.
inline constexpr uint64_t FNV64_OFFSET = 0xcbf29ce484222325ull;

[[nodiscard]] constexpr uint64_t Fnv64Step(uint64_t hash, uint64_t value) {
	return (hash ^ value) * 0x100000001b3ull;
}

} // namespace Common

#endif /* KYTY_COMMON_HASH_H_ */
