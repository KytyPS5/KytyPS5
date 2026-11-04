#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute };

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

	/// Blocks all guest access to a range until the matching RemoveReadbackHold. Holds are counted
	/// apart from watchers, so a pending readback can share a page with a cache's read watch.
	void AddReadbackHold(uint64_t vaddr, uint64_t size);
	/// Releases one hold taken by AddReadbackHold on the same range.
	void RemoveReadbackHold(uint64_t vaddr, uint64_t size);
	/// Returns whether no watcher or hold blocks a read or write of the page holding vaddr. A page
	/// in a tracked region with no watchers returns true whether or not it was ever watched or is
	/// even mapped, so a caller treating true as a stale fault must first check that the guest may
	/// make this access. A page in no tracked region and any execute access return false.
	[[nodiscard]] bool WatchersAllowAccess(uint64_t vaddr, PageFaultAccess access) const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
