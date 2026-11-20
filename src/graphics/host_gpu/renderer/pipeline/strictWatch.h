#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_STRICTWATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_STRICTWATCH_H_

#include "common/common.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class PageManager;

// The large ranges of guest memory that the resolution of shader programs reads strictly (the
// T# arrays of bindless tables), with their pages write-watched while they are read: the check
// of such a read before its operation executes compares only the pages written since it. GTA V's
// tables are 2 MB, read for every operation that traces rays (four a frame in Fidelity mode), and
// the guest writes a few of their pages a frame. Each range has a version per page: the watch's
// version when the page was last watched, 0 while it is not (a write took the watch).
class StrictWatch {
public:
	explicit StrictWatch(PageManager& page_manager): m_page_manager(page_manager) {}
	~StrictWatch();
	KYTY_CLASS_NO_COPY(StrictWatch);

	// Strict reads of at least this size are watched.
	static constexpr uint64_t MinSize = 256 * 1024;

	// Before a read of the range: watches its pages; returns the version of the bytes read after
	// the call (a check of them compares the pages written since).
	[[nodiscard]] uint64_t Watch(uint64_t address, uint64_t size);
	// The byte runs of [address, address + size), as offsets from address, that writes may have
	// changed since a read of version `version` (all for 0); watches them again first.
	void Changed(uint64_t address, uint64_t size, uint64_t version,
	             std::vector<std::pair<uint64_t, uint64_t>>& runs);
	// A write to guest memory (a write fault on any thread, or a write that does not fault).
	void InvalidateMemory(uint64_t address, uint64_t size) {
		if (address < m_end.load(std::memory_order_acquire) &&
		    address + size > m_begin.load(std::memory_order_acquire)) {
			Invalidate(address, size);
		}
	}
	void UnmapMemory(uint64_t address, uint64_t size);

private:
	struct Range {
		uint64_t              base = 0; // (page aligned)
		uint64_t              end  = 0;
		std::vector<uint64_t> versions;
		uint64_t              used = 0;
	};
	static constexpr size_t MaxRanges = 16;

	Range& Find(uint64_t address, uint64_t size);
	// Watches the pages of the range that are not watched, with a new version.
	void Arm(Range& range);
	void Unwatch(Range& range, size_t first, size_t last);
	void Invalidate(uint64_t address, uint64_t size);
	void UpdateBounds();

	PageManager&                        m_page_manager;
	std::mutex                          m_mutex;
	std::vector<std::unique_ptr<Range>> m_ranges;
	uint64_t                            m_version = 0;
	uint64_t                            m_uses    = 0;
	// The span of the ranges, for the writes outside them.
	std::atomic<uint64_t> m_begin {UINT64_MAX};
	std::atomic<uint64_t> m_end {0};
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_STRICTWATCH_H_ */
