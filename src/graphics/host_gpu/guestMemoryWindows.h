#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOWS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOWS_H_

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

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

// The first and last window that a part of [file_offset, file_offset + size) can be placed in
// (GuestMemoryWindowIndex), or none when the range is empty or starts past the file.
[[nodiscard]] inline std::optional<std::pair<uint64_t, uint64_t>>
GuestMemoryWindowsCovering(uint64_t file_offset, uint64_t size, uint64_t window,
                           uint64_t file_size) noexcept {
	const auto stride = window / 2;
	if (stride == 0 || size == 0 || file_offset >= file_size) {
		return std::nullopt;
	}
	const auto last_byte = std::min(file_offset + size, file_size) - 1;
	return std::pair {file_offset / stride, last_byte / stride};
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOWS_H_ */
