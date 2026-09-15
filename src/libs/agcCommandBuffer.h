#pragma once

#include <cstdint>

namespace Libs::Graphics::Gen5 {

inline uint64_t CommandBufferAvailableDW(const uint32_t* bottom, const uint32_t* top,
                                         const uint32_t* cursor_up, const uint32_t* cursor_down,
                                         uint32_t reserved_dw) {
	if (bottom == nullptr || top == nullptr || cursor_up == nullptr) {
		return 0;
	}
	const auto begin = reinterpret_cast<uintptr_t>(bottom);
	const auto end = reinterpret_cast<uintptr_t>(top);
	const auto up = reinterpret_cast<uintptr_t>(cursor_up);
	const auto down = cursor_down == nullptr ? end : reinterpret_cast<uintptr_t>(cursor_down);
	if (begin > up || up > down || down > end ||
	    ((begin | end | up | down) % alignof(uint32_t)) != 0) {
		return 0;
	}
	const auto available = static_cast<uint64_t>((down - up) / sizeof(uint32_t));
	return available > reserved_dw ? available - reserved_dw : 0;
}

} // namespace Libs::Graphics::Gen5
