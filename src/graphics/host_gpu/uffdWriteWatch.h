#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_UFFDWRITEWATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_UFFDWRITEWATCH_H_

#include "common/common.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <cstdint>
#include <shared_mutex>
#include <vector>

// Which pages the CPU wrote, seen without a fault reaching the emulator: userfaultfd write
// protection in asynchronous mode (Linux 6.7). The kernel resolves a write to a protected page
// itself, in under a microsecond, and PAGEMAP_SCAN lists the pages written since and protects
// them again. A write that faults through a signal handler costs some 40 microseconds.
//
// Hot pages use it (writeHeat.h): they stay open, and instead of being uploaded at every use,
// they are uploaded when a scan finds them written (MemoryTracker::TakeArmedWrites).
//
// Guest memory is registered as it is mapped for the GPU; only registered memory is protected.
// Where the kernel has no such protection, Get() is null and hot pages stay dirty as before.
// KYTY_UFFD=0 turns it off.

namespace Libs::Graphics {

class UffdWriteWatch final {
public:
	// The process's watch, or null.
	[[nodiscard]] static UffdWriteWatch* Get();

	KYTY_CLASS_NO_COPY(UffdWriteWatch);

	// Guest memory that was mapped: the watch can protect it from now on.
	void Register(uint64_t vaddr, uint64_t size);
	// Guest memory that is no longer mapped.
	void Unregister(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool Covers(uint64_t vaddr, uint64_t size) const;

	// Write-protects the pages of the range: their next writes are seen by TakeWritten.
	void Protect(uint64_t vaddr, uint64_t size);
	// Appends the runs of the range written since they were protected, and protects them again.
	// When the kernel cannot tell, the whole range counts as written.
	void TakeWritten(uint64_t vaddr, uint64_t size, std::vector<GuestRange>& written);

private:
	UffdWriteWatch(int uffd, int pagemap): m_uffd(uffd), m_pagemap(pagemap) {}

	int                       m_uffd    = -1;
	int                       m_pagemap = -1;
	mutable std::shared_mutex m_mutex;
	RangeSet                  m_registered;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_UFFDWRITEWATCH_H_
