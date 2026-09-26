#include "graphics/host_gpu/renderer/cache/bufferDownloadBatch.h"

#include "common/alignment.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

std::vector<BufferDownloadBatch> SplitBufferDownload(const std::vector<BufferDownloadRange>& ranges,
                                                     uint64_t capacity, uint64_t alignment) {
	std::vector<BufferDownloadBatch> batches;
	if (capacity == 0 || alignment == 0) {
		return batches;
	}
	// Round the budget down to a whole alignment unit. A batch total is always a multiple of
	// the alignment, so this keeps the room left in a batch a multiple of the alignment too,
	// which is what lets the loop below rely on a partial batch still having room for a
	// whole aligned unit.
	const uint64_t budget = capacity - capacity % alignment;
	if (budget == 0) {
		return batches;
	}

	BufferDownloadBatch batch;
	for (const auto& range: ranges) {
		uint64_t src       = range.src_offset;
		uint64_t remaining = range.size;
		while (remaining > 0) {
			if (batch.total_size >= budget) {
				batches.push_back(batch);
				batch = BufferDownloadBatch {};
			}
			// Room in this batch, which is at least one alignment unit because a non-empty
			// batch below is always shorter than the budget on entry.
			const uint64_t room = budget - batch.total_size;
			const uint64_t take = room < remaining ? room : remaining;
			batch.copies.push_back({src, batch.total_size, take});
			src += take;
			remaining -= take;
			batch.total_size = Common::AlignUp(batch.total_size + take, alignment);
		}
	}
	if (!batch.copies.empty()) {
		batches.push_back(batch);
	}
	return batches;
}

} // namespace Libs::Graphics
