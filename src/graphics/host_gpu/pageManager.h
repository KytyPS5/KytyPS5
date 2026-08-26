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

	// Diagnostics: the watcher state this manager believes a page has, and the protection that
	// state implies. Compared against the real host protection it shows tracker/OS divergence.
	struct PageDiagnostics {
		bool     known               = false;
		uint32_t write_watchers      = 0;
		uint32_t access_watchers     = 0;
		uint32_t expected_protection = 0;
	};
	[[nodiscard]] PageDiagnostics DescribePage(uint64_t vaddr) const;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);
	void OnGpuMap(uint64_t vaddr, uint64_t size);
	void OnGpuUnmap(uint64_t vaddr, uint64_t size);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
