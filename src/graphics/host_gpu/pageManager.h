#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/rangeSet.h"

#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };
enum class PageWatchMode { Write, ReadWrite };

class PageManager final {
public:
	class BackingWrite final {
	public:
		BackingWrite(PageManager& manager, uint64_t vaddr, uint64_t size) noexcept;
		~BackingWrite();
		KYTY_CLASS_NO_COPY(BackingWrite);

	private:
		PageManager& m_manager;
		uint64_t     m_vaddr = 0;
		uint64_t     m_size  = 0;
	};

	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;

	void UpdatePageWatchers(bool track, uint64_t vaddr, uint64_t size,
	                        PageWatchMode mode = PageWatchMode::Write);
	void OnGpuMap(uint64_t vaddr, uint64_t size);
	void OnGpuUnmap(uint64_t vaddr, uint64_t size);

	[[nodiscard]] std::vector<std::unique_ptr<BackingWrite>>
	ReserveBackingWrites(std::span<const RangeSet::Range> ranges);

private:
	void BeginBackingWrite(uint64_t vaddr, uint64_t size) noexcept;
	void EndBackingWrite(uint64_t vaddr, uint64_t size) noexcept;

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
