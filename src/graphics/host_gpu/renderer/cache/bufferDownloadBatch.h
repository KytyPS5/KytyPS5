#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDOWNLOADBATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDOWNLOADBATCH_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// A contiguous run of a destination buffer to read back, in bytes from its start.
struct BufferDownloadRange {
	uint64_t src_offset = 0;
	uint64_t size       = 0;
};

// One copy inside a batch. src_offset is relative to the destination buffer and dst_offset is
// relative to the start of the batch, so the staging offset is only known once the batch has
// reserved its region.
struct BufferDownloadCopy {
	uint64_t src_offset = 0;
	uint64_t dst_offset = 0;
	uint64_t size       = 0;
};

// A set of copies that fits in a single staging reservation.
struct BufferDownloadBatch {
	std::vector<BufferDownloadCopy> copies;
	uint64_t                       total_size = 0;
};

// Split a download into batches that each fit one reservation of a staging buffer holding
// `capacity` bytes.
//
// Every range is padded up to `alignment` so two packed ranges never share a cache line,
// which is what the read-back relies on. A range larger than the capacity is split across
// batches rather than dropped, because a staging buffer refuses a reservation bigger than
// itself outright instead of waiting for space: an oversized range would fail permanently
// however empty the buffer happened to be. Batches are filled greedily in order, which is the
// fewest reservations the given ordering allows.
//
// Pure, so the packing is covered by unit tests without a device.
[[nodiscard]] std::vector<BufferDownloadBatch> SplitBufferDownload(
    const std::vector<BufferDownloadRange>& ranges, uint64_t capacity, uint64_t alignment);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDOWNLOADBATCH_H_
