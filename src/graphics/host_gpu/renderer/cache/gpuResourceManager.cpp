#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "kernel/memory.h"

#include <cinttypes>
#include <cstdio>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace Libs::Graphics {

namespace {

// The real host protection of a page, so a claimed-but-unresolved fault can be told apart from a
// genuine repeat. A tracker that believes a page is writable while the OS reports read-only is the
// signature of a protection update that the mask.None() early-out skipped.
uint32_t QueryHostProtection(uint64_t vaddr) noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(reinterpret_cast<const void*>(vaddr), &info, sizeof(info)) == 0) {
		return 0;
	}
	return info.State == MEM_COMMIT ? info.Protect : 0;
#else
	(void)vaddr;
	return 0;
#endif
}

} // namespace

// Safety net, not a fix. A fault this manager claims must actually be resolved: the filter resumes
// the guest instruction, which re-executes the same store. If the caches leave the page protected
// the store faults again immediately and the thread makes no progress at all -- a livelock that is
// indistinguishable from a hang and that no amount of waiting escapes. Whenever a single page is
// claimed this many times in a row without another page intervening, force the host protection
// open so the thread advances, and report the tracker state that caused it. A redundant
// VirtualProtect costs microseconds; a livelocked render thread costs the process.
void GpuResourceManager::ResolveRepeatedFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// Far above any legitimate consecutive-repeat burst (a page reprotected between two guest
	// stores repeats a handful of times), far below the tens of millions a real livelock reaches.
	constexpr uint64_t UNRESOLVED_FAULT_LIMIT = 4096;
	constexpr uint64_t MAX_REPORTS            = 16;

	static thread_local uint64_t last_page = 0;
	static thread_local uint64_t repeats   = 0;
	static thread_local uint64_t reported  = 0;

	const auto page = fault_vaddr & ~(TRACKER_PAGE_SIZE - 1);
	if (page != last_page) {
		last_page = page;
		repeats   = 1;
		return;
	}
	if (++repeats < UNRESOLVED_FAULT_LIMIT) {
		return;
	}
	repeats = 1;

	if (reported < MAX_REPORTS) {
		reported++;
		const auto page_state   = m_page_manager.DescribePage(page);
		const auto host_protect = QueryHostProtection(page);
		printf("FAULTLOOP page=0x%016" PRIx64 " at=0x%016" PRIx64 " access=%d claimed %" PRIu64
		       " times without progress; tracker(write_watchers=%" PRIu32 " access_watchers=%" PRIu32
		       " known=%d expected_protect=0x%02" PRIx32 ") host_protect=0x%08" PRIx32 "\n",
		       page, fault_vaddr, static_cast<int>(access), UNRESOLVED_FAULT_LIMIT,
		       page_state.write_watchers, page_state.access_watchers,
		       static_cast<int>(page_state.known), page_state.expected_protection, host_protect);
		RegionManager::PageDiagnostics region {};
		if (m_buffer_cache.DescribeTrackerPage(page, &region)) {
			printf("\t BUFFER REGION cpu_dirty=%d gpu_dirty=%d writable=%d readable=%d\n",
			       static_cast<int>(region.cpu_dirty), static_cast<int>(region.gpu_dirty),
			       static_cast<int>(region.writable), static_cast<int>(region.readable));
		}
		m_texture_cache.DescribePageImages(page);
		fflush(stdout);
	}

	(void)LibKernel::Memory::ProtectGuestHostMemory(page, TRACKER_PAGE_SIZE,
	                                                Common::VirtualMemory::Mode::ReadWrite);
}

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache) {}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	constexpr uint64_t fault_size = 8;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	ResolveRepeatedFault(access, fault_vaddr);
	return true;
}

bool GpuResourceManager::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
	}
	m_page_manager.OnGpuMap(vaddr, size);
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		if (m_scheduler.Active()) {
			const auto tick = m_scheduler.CurrentTick();
			m_scheduler.Finish();
			m_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_page_manager.OnGpuUnmap(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	if (m_gpu == nullptr) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void GpuResourceManager::PrepareBda() {
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
		m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
	});
	m_fault_process_pending = true;
}

void GpuResourceManager::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

} // namespace Libs::Graphics
