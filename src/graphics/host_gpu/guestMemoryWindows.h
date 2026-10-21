#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOWS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOWS_H_

#include <algorithm>
#include <cstdint>
#include <optional>

namespace Libs::Graphics {

// The window that holds [file_offset, file_offset + size) of a file of `file_size` bytes, with
// windows of `window` bytes every `window / 2` bytes (guestMemoryImport.h), or none when the
// range does not fit in one.
[[nodiscard]] inline std::optional<uint64_t> GuestMemoryWindowIndex(uint64_t file_offset,
                                                                    uint64_t size, uint64_t window,
                                                                    uint64_t file_size) noexcept {
	const auto stride = window / 2;
	if (stride == 0 || size == 0 || file_offset >= file_size || size > file_size - file_offset) {
		return std::nullopt;
	}
	// The window that starts last at or before the range has the most room after it.
	const auto index = file_offset / stride;
	const auto begin = index * stride;
	const auto end   = begin + std::min(window, file_size - begin);
	if (size > end - file_offset) {
		return std::nullopt;
	}
	return index;
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOWS_H_ */
