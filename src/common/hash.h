#ifndef KYTY_COMMON_HASH_H_
#define KYTY_COMMON_HASH_H_

#include "common/common.h"

namespace Common {

inline uint32_t hash(const void* key, uint32_t key_len) {
	uint32_t hash = 0;

	const auto* ptr = static_cast<const uint8_t*>(key);

	while (key_len >= 4) {
		hash += ptr[0];
		hash += (hash << 10u);
		hash ^= (hash >> 6u);
		hash += ptr[1];
		hash += (hash << 10u);
		hash ^= (hash >> 6u);
		hash += ptr[2];
		hash += (hash << 10u);
		hash ^= (hash >> 6u);
		hash += ptr[3];
		hash += (hash << 10u);
		hash ^= (hash >> 6u);

		key_len -= 4;
		ptr += 4;
	}

	switch (key_len) {
		case 3:
			hash += ptr[2];
			hash += (hash << 10u);
			hash ^= (hash >> 6u);
			[[fallthrough]];
		case 2:
			hash += ptr[1];
			hash += (hash << 10u);
			hash ^= (hash >> 6u);
			[[fallthrough]];
		case 1:
			hash += ptr[0];
			hash += (hash << 10u);
			hash ^= (hash >> 6u);
			[[fallthrough]];
		default: break;
	}

	hash += (hash << 3u);
	hash ^= (hash >> 11u);
	hash += (hash << 15u);

	return hash;
}

inline uint32_t hash8(uint8_t key) {
	uint32_t hash = 0;

	uint8_t* ptr = &key;

	hash += ptr[0];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);

	hash += (hash << 3u);
	hash ^= (hash >> 11u);
	hash += (hash << 15u);

	return hash;
}

inline uint32_t hash16(uint16_t key) {
	uint32_t hash = 0;

	auto* ptr = reinterpret_cast<uint8_t*>(&key);

	hash += ptr[0];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[1];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);

	hash += (hash << 3u);
	hash ^= (hash >> 11u);
	hash += (hash << 15u);

	return hash;
}

inline uint32_t hash32(uint32_t key) {
	uint32_t hash = 0;

	auto* ptr = reinterpret_cast<uint8_t*>(&key);

	hash += ptr[0];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[1];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[2];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[3];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);

	hash += (hash << 3u);
	hash ^= (hash >> 11u);
	hash += (hash << 15u);

	return hash;
}

inline uint32_t hash64(uint64_t key) {
	uint32_t hash = 0;

	auto* ptr = reinterpret_cast<uint8_t*>(&key);

	hash += ptr[0];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[1];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[2];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[3];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[4];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[5];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[6];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);
	hash += ptr[7];
	hash += (hash << 10u);
	hash ^= (hash >> 6u);

	hash += (hash << 3u);
	hash ^= (hash >> 11u);
	hash += (hash << 15u);

	return hash;
}

} // namespace Common

#endif /* KYTY_COMMON_HASH_H_ */
