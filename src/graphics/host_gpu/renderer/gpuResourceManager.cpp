#include "graphics/host_gpu/renderer/gpuResourceManager.h"

#include "common/assert.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_page_manager(FaultThunk, this),
      m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache, m_resource_mutex),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache, m_resource_mutex) {}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::FaultThunk(void* context, PageFaultAccess access, uint64_t vaddr,
                                    uint64_t size, PageFaultPhase phase) noexcept {
	return static_cast<GpuResourceManager*>(context)->InvalidateMemory(access, vaddr, size, phase);
}

bool GpuResourceManager::InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
                                          PageFaultPhase phase) noexcept {
	// Let the authoritative image materialize first. A clean overlapping buffer marks a write
	// fault CPU-dirty when it begins ownership transfer; doing that before image preflight would
	// make the image appear to race a real CPU write. Completion and release retain buffer-first
	// ordering so its pending fault is gone before TextureCache publishes the downloaded backing.
	if (phase == PageFaultPhase::Invalidate) {
		const bool image_handled  = m_texture_cache.InvalidateMemory(access, vaddr, size, phase);
		const bool buffer_handled = m_buffer_cache.InvalidateMemory(access, vaddr, size, phase);
		return buffer_handled || image_handled;
	}
	const bool buffer_handled = m_buffer_cache.InvalidateMemory(access, vaddr, size, phase);
	const bool image_handled  = m_texture_cache.InvalidateMemory(access, vaddr, size, phase);
	return buffer_handled || image_handled;
}

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	if (!m_page_manager.IsMapped(fault_vaddr, 1)) {
		return false;
	}
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported guest-memory fault from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " access=%u\n",
		     fault_vaddr, static_cast<uint32_t>(access));
	}
	bool       handled = false;
	const auto resolve = [this, access, fault_vaddr, &handled](CommandProcessor& cp) {
		cp.BeginReadbackTransaction();
		(void)m_buffer_cache.SynchronizeBacking(fault_vaddr, 1);
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			handled = m_page_manager.HandleFault(access, fault_vaddr);
		}
		cp.EndReadbackTransaction();
	};
	if (auto* cp = Gpu::CurrentCommandProcessor(); cp != nullptr) {
		resolve(*cp);
		return handled;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported page fault from a pre-owned resource transaction, addr=0x%016" PRIx64
		     " access=%u\n",
		     fault_vaddr, static_cast<uint32_t>(access));
	}
	EXIT_IF(m_gpu == nullptr);
	m_gpu->SendCommandSyncWithProcessor(resolve);
	return handled;
}

void GpuResourceManager::PrepareHostWrite(uint64_t vaddr, uint64_t size) {
	if (!m_page_manager.HasAnyMapping(vaddr, size)) {
		return;
	}
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported host write from an asynchronous GPU completion, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto handle_range = [this, vaddr, size] {
		if (!m_page_manager.HandleWriteRange(vaddr, size)) {
			EXIT("failed to prepare host write, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
	};
	const auto resolve = [this, &handle_range](CommandProcessor& cp) {
		cp.BeginReadbackTransaction();
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			handle_range();
		}
		cp.EndReadbackTransaction();
	};
	if (auto* cp = Gpu::CurrentCommandProcessor(); cp != nullptr) {
		resolve(*cp);
		return;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported host write from a pre-owned resource transaction, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	EXIT_IF(m_gpu == nullptr);
	m_gpu->SendCommandSyncWithProcessor(resolve);
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	return m_page_manager.IsMapped(vaddr, size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size, GpuAccess access) {
	m_page_manager.OnGpuMap(vaddr, size, access);
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size, GpuAccess access) {
	if (!IsMapped(vaddr, size)) {
		EXIT("cannot unmap an unmapped GPU resource range\n");
	}
	const auto unmap = [this, vaddr, size, access] {
		m_texture_cache.UnmapMemory(vaddr, size);
		m_buffer_cache.UnmapMemory(vaddr, size);
		m_page_manager.OnGpuUnmap(vaddr, size, access);
	};
	if (m_gpu == nullptr) {
		if (m_resource_mutex.IsOwnedByCurrentThread()) {
			EXIT("cannot synchronously unmap from a resource transaction\n");
		}
		unmap();
		return;
	}
	Gpu::SubmissionLock submissions(*m_gpu);
	m_gpu->SendCommandSync(unmap);
}

void GpuResourceManager::RunGarbageCollector() {
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

} // namespace Libs::Graphics
