#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <atomic>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
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
	// Only a page watcher makes a fault the caches' to resolve. Any other fault in GPU-visible
	// memory (a guest protection, or a page briefly absent while its view is remapped) must not
	// create cache state here: before the first submission there is no command buffer for it.
	if (!IsWatched(access, fault_vaddr) || !IsMapped(fault_vaddr, fault_size)) {
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

bool RenderContext::IsWatched(PageFaultAccess access, uint64_t fault_vaddr) const noexcept {
	return m_page_manager.IsWatched(fault_vaddr, access);
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

// Generations are unique across contexts, so a thread's cached range never matches another one.
static uint64_t NextMappingGeneration() {
	static std::atomic<uint64_t> next {0};
	return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	// Shader resource evaluation probes dozens of words of one mapping per draw, and the shared
	// lock is contended by faulting guest threads. Remember the last mapping this thread found;
	// every map and unmap moves the generation and invalidates it.
	struct CachedRange {
		const RenderContext* owner      = nullptr;
		uint64_t             generation = 0;
		uint64_t             begin      = 0;
		uint64_t             end        = 0;
	};
	thread_local CachedRange cached;
	if (cached.owner == this &&
	    cached.generation == m_mapping_generation.load(std::memory_order_acquire) &&
	    vaddr >= cached.begin && size <= cached.end - vaddr && vaddr < cached.end) {
		return true;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	uint64_t begin = 0;
	uint64_t end   = 0;
	if (!m_mapped_ranges.FindContaining(vaddr, size, begin, end)) {
		return false;
	}
	cached = {this, m_mapping_generation.load(std::memory_order_relaxed), begin, end};
	return true;
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_mapping_generation.store(NextMappingGeneration(), std::memory_order_release);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		// Only the unmapped range loses its backing; the rest of a buffer keeps its GPU data.
		m_buffer_cache.InvalidateMemory(vaddr, size, true);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
		m_mapping_generation.store(NextMappingGeneration(), std::memory_order_release);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda(std::span<const uint64_t> address_bases) {
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	// A DMA read of memory without a cached buffer returns zero; the fault only backs the page for
	// later dispatches. A shader reading from a per-frame ring this way read zeros whenever the
	// ring had moved onto new pages. Back a window at each known base.
	constexpr uint64_t BaseWindow = 256ull << 10u;
	for (const auto base: address_bases) {
		if (base == 0) {
			continue;
		}
		m_mapped_ranges.ForEachInRange(base, BaseWindow, [&](uint64_t start, uint64_t end) {
			if (start == base) {
				(void)m_buffer_cache.FindBuffer(start, end - start);
			}
		});
	}
	// Every buffer must hold the guest's CPU writes before the shader reads memory directly. A
	// sweep of every buffer before each DMA draw and dispatch would mostly find nothing to upload,
	// so only the buffers in tracker regions with new CPU writes are visited. Regions are taken
	// before uploading, and writes from then on queue their region again.
	m_buffer_cache.TakeCpuModifiedRegions(m_bda_regions);
	const auto generation = m_mapping_generation.load(std::memory_order_relaxed);
	if (generation != m_bda_mapping_generation) {
		m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
			m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
		});
		m_bda_mapping_generation = generation;
	} else {
		for (const auto region: m_bda_regions) {
			m_mapped_ranges.ForEachInRange(
			    region, TRACKER_REGION_SIZE, [this](uint64_t start, uint64_t end) {
				    m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
			    });
		}
	}
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	// The caches age resources in collections, and this runs after every guest submission. A
	// title that submits dozens of times a frame had its buffers and images retired after a
	// fraction of a second unused, then recreated and uploaded again. Collect once per frame.
	const auto frame = m_gpu != nullptr ? m_gpu->GetFrameNum() : 0;
	if (frame == m_gc_frame) {
		return;
	}
	m_gc_frame = frame;
	m_texture_cache.RunGarbageCollector();
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
