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

	// Receives guest ranges that the CPU wrote while they were write-watched.
	using WriteSink = void (*)(void* context, uint64_t vaddr, uint64_t size);

	// On Linux, regions that take many write faults switch their write watches to
	// asynchronous userfaultfd write protection: the kernel lifts the protection on the
	// first write without raising a signal, and the writes are collected here instead.
	// Access watches stay synchronous. Callers must hold no tracker or cache locks.
	void HarvestWrites(WriteSink sink, void* context);
	// Collects the writes in [vaddr, vaddr + size) and passes them to `sink` while the
	// caller may hold tracker locks. The ranges are also queued for the next HarvestWrites,
	// so the remaining owners still see them.
	void HarvestRange(uint64_t vaddr, uint64_t size, WriteSink sink, void* context);
	// Counts a synchronous write fault toward switching its region to asynchronous watches.
	void NoteWriteFault(uint64_t vaddr) noexcept;
	[[nodiscard]] bool AsyncWriteWatchEnabled() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
