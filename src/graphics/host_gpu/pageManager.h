#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"

#include <memory>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };
enum class PageFaultPhase { Invalidate, Complete, Release };
enum class PageWatchMode { Write, ReadWrite };
enum class GpuAccess { Read, Write, ReadWrite };

using PageFaultHandler = bool (*)(void* context, PageFaultAccess access, uint64_t vaddr,
                                  uint64_t size, PageFaultPhase phase) noexcept;

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

	PageManager(PageFaultHandler fault_handler, void* fault_context);
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;
	[[nodiscard]] bool     IsTracked(uint64_t vaddr) const noexcept;
	[[nodiscard]] bool     IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	[[nodiscard]] bool HasGpuAccess(uint64_t vaddr, uint64_t size, GpuAccess access) const noexcept;

	void UpdatePageWatchers(bool track, uint64_t vaddr, uint64_t size,
	                        PageWatchMode mode = PageWatchMode::Write);
	void OnGpuMap(uint64_t vaddr, uint64_t size, GpuAccess access = GpuAccess::ReadWrite);
	void OnGpuUnmap(uint64_t vaddr, uint64_t size, GpuAccess access = GpuAccess::ReadWrite);

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;

private:
	void BeginBackingWrite(uint64_t vaddr, uint64_t size) noexcept;
	void EndBackingWrite(uint64_t vaddr, uint64_t size) noexcept;

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
