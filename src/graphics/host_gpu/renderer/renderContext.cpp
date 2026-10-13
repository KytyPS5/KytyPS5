#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/bdaSyncSet.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_indirect_key_feedback(graphics, m_command_scheduler) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsAnyMapped(uint64_t vaddr, uint64_t size) const noexcept {
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Intersects(vaddr, size);
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
		// Newly mapped memory is exactly what a full walk would newly visit, and a range remapped
		// at an address that was just unmapped has to come back. Queue after the add.
		m_buffer_cache.QueueBdaSync(vaddr, size);
	}
	// New backing changes the bytes without a write fault. An image read while the range was
	// unmapped holds zeros and no page watch, so it must reload and re-arm.
	m_texture_cache.InvalidateMemory(vaddr, size);
}

void RenderContext::ProtectMemory(uint64_t vaddr, uint64_t size,
                                  Common::VirtualMemory::Mode mode) {
	m_page_manager.ReapplyProtection(vaddr, size, mode);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		const bool active = m_command_scheduler.Active();
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_buffer_cache.ForgetBdaResidency(vaddr, size);
		if (m_texture_cache.UnmapMemory(vaddr, size) && active) {
			m_command_scheduler.EndRendering();
		}
		if (active) {
			m_command_scheduler.WaitGuestWrites(vaddr, size);
		}
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
		// Only unmapped memory may leave the set: nothing can reach it, and MapMemory puts it
		// back if the guest remaps the same address.
		m_buffer_cache.DropBdaSync(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::ForgetMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory forget from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	if (IsAnyMapped(vaddr, size)) {
		UnmapMemory(vaddr, size);
		return;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	const auto forget = [this, vaddr, size] {
		const bool active = m_command_scheduler.Active();
		m_buffer_cache.ForgetBdaResidency(vaddr, size);
		if (m_texture_cache.UnmapMemory(vaddr, size) && active) {
			m_command_scheduler.EndRendering();
		}
	};
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		forget();
		return;
	}
	m_gpu->SendCommand(forget);
}

void RenderContext::PrepareBda(bool stores) {
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	// Drain, do not rediscover. The synchronise half used to walk every mapped range on every
	// DMA draw and every DMA dispatch; the set of ranges that can actually need an upload is
	// maintained at the points that dirty a page, create a region, register a buffer or change
	// the mapped set, so everything outside it is provably a no-op. Take the set before walking:
	// a queue that lands during the walk belongs to the next call, never to this one.
	const RangeSet pending = m_buffer_cache.TakeBdaSync();
	m_buffer_cache.BeginUploadBatch();
	ForEachBdaSyncRange(pending, m_mapped_ranges, [this](uint64_t start, uint64_t end) {
		m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
	});
	m_buffer_cache.EndUploadBatch();
	if (stores || m_buffer_cache.HasUnmarkedBdaStores()) {
		m_buffer_cache.MarkBdaStoresInMapped(m_mapped_ranges, stores);
		m_buffer_cache.ClearUnmarkedBdaStores();
	}
	m_fault_process_pending = true;
}

void RenderContext::PrefetchBda(uint64_t address, bool store) {
	constexpr uint64_t Window = 256 * 1024;
	if (address == 0) {
		return;
	}
	const auto begin = address & ~(BufferCache::CACHING_PAGESIZE - 1);
	if (begin > UINT64_MAX - Window) {
		return;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_mapped_ranges.ForEachInRange(begin, Window, [&](uint64_t start, uint64_t end) {
		if (start <= address && end > address) {
			m_buffer_cache.PrefetchBda(start, end - start, store);
		}
	});
}

void RenderContext::PrefetchBdaRange(uint64_t address, uint64_t size, bool store) {
	if (address == 0 || size == 0) {
		return;
	}
	const auto begin = address & ~(BufferCache::CACHING_PAGESIZE - 1);
	const auto last  = address + size - 1;
	if (last < address) {
		return;
	}
	const auto end = (last | (BufferCache::CACHING_PAGESIZE - 1)) + 1;
	if (end <= begin) {
		return;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_mapped_ranges.ForEachInRange(begin, end - begin, [&](uint64_t start, uint64_t finish) {
		m_buffer_cache.PrefetchBda(start, finish - start, store, finish - start);
	});
}

void RenderContext::RunGarbageCollector() {
	static const bool per_frame_texture_gc = [] {
		const char* text = std::getenv("KYTY_NO_PER_FRAME_TEXTURE_GC");
		return text == nullptr || std::strcmp(text, "0") == 0;
	}();
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	if (m_buffer_cache.HasUnmarkedBdaStores()) {
		std::shared_lock lock(m_mapped_ranges_mutex);
		m_buffer_cache.MarkBdaStoresInMapped(m_mapped_ranges, false);
		m_buffer_cache.ClearUnmarkedBdaStores();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector(per_frame_texture_gc);
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
