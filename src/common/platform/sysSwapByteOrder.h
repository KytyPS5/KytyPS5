#ifndef KYTY_COMMON_PLATFORM_SYSSWAPBYTEORDER_H_
#define KYTY_COMMON_PLATFORM_SYSSWAPBYTEORDER_H_

#include "common/common.h"

#include <type_traits>

inline uint16_t SwapByteOrder16(uint16_t value) {
	uint16_t hi = value << 8u;
	uint16_t lo = value >> 8u;
	return hi | lo;
}

inline uint32_t SwapByteOrder32(uint32_t value) {
	return __builtin_bswap32(value);
}

inline uint64_t SwapByteOrder64(uint64_t value) {
	return __builtin_bswap64(value);
}

template <typename T>
inline void SwapByteOrder(T& x) {
	if (sizeof(x) == 2) {
		if (std::is_signed_v<T>) {
			x = static_cast<std::make_signed_t<T>>(
			    SwapByteOrder16(static_cast<std::make_unsigned_t<uint16_t>>(x)));
		} else {
			x = SwapByteOrder16(x);
		}
	}
	if (sizeof(x) == 4) {
		if (std::is_signed_v<T>) {
			x = static_cast<std::make_signed_t<T>>(
			    SwapByteOrder32(static_cast<std::make_unsigned_t<uint32_t>>(x)));
		} else {
			x = SwapByteOrder32(x);
		}
	}
	if (sizeof(x) == 8) {
		if (std::is_signed_v<T>) {
			x = static_cast<std::make_signed_t<T>>(
			    SwapByteOrder64(static_cast<std::make_unsigned_t<uint64_t>>(x)));
		} else {
			x = SwapByteOrder64(x);
		}
	}
}

template <typename T>
inline void NoSwapByteOrder(T& x) {}

#endif /* KYTY_COMMON_PLATFORM_SYSSWAPBYTEORDER_H_ */
