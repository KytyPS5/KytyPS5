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

} // namespace Common

#endif /* KYTY_COMMON_HASH_H_ */
