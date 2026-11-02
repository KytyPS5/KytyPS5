#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

	/// Re-arms host protection for [vaddr, vaddr+size) from the current watcher counts.
	/// Use it after the host view was replaced with a fresh read-write mapping while the
	/// counts were kept. Counts are never changed. Pages with a read watcher become
	/// no-access, pages with only write watchers become read-only and unwatched pages are
	/// left alone. Contiguous pages needing the same protection share one host call.
	/// A size of 0 is accepted as a no-op, unlike UpdatePageWatchers.
	void ReapplyProtection(uint64_t vaddr, uint64_t size);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
