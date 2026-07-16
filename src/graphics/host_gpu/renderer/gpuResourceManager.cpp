#include "graphics/host_gpu/renderer/gpuResourceManager.h"

#include "common/assert.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager()
    : m_page_manager(FaultThunk, this), m_buffer_cache(m_page_manager, m_resource_mutex),
      m_texture_cache(m_page_manager, m_buffer_cache, m_resource_mutex) {
	m_buffer_cache.SetTextureCache(m_texture_cache);
}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::FaultThunk(void* context, PageFaultAccess access, uint64_t vaddr,
                                    uint64_t size, PageFaultPhase phase) noexcept {
	return static_cast<GpuResourceManager*>(context)->InvalidateMemory(access, vaddr, size, phase);
}

bool GpuResourceManager::InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
                                          PageFaultPhase phase) noexcept {
	const bool buffer_handled = m_buffer_cache.InvalidateMemory(access, vaddr, size, phase);
	const bool image_handled  = m_texture_cache.InvalidateMemory(access, vaddr, size, phase);
	return buffer_handled || image_handled;
}

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	if (!m_page_manager.IsMapped(fault_vaddr, 1)) {
		return false;
	}
	if (LabelInCallback()) {
		EXIT("unsupported guest-memory fault from an asynchronous GPU label callback, "
		     "addr=0x%016" PRIx64 " access=%u\n",
		     fault_vaddr, static_cast<uint32_t>(access));
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		if (!GraphicsRunIsCommandProcessorGuestAccess()) {
			EXIT("unsupported page fault from a pre-owned resource transaction, addr=0x%016" PRIx64
			     " access=%u\n",
			     fault_vaddr, static_cast<uint32_t>(access));
		}
		ResourceMutex::FaultScope fault(m_resource_mutex);
		return m_page_manager.HandleFault(access, fault_vaddr);
	}
	// Stop command-processor jobs before taking the shared cache transaction. External readback
	// workers inherit this paused state and therefore never form resource -> submission lock inversion.
	GraphicsRunSubmissionLock submissions;
	ResourceMutex::FaultScope fault(m_resource_mutex);
	return m_page_manager.HandleFault(access, fault_vaddr);
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	return m_page_manager.IsMapped(vaddr, size);
}

std::unique_lock<ResourceMutex> GpuResourceManager::LockCommandProcessorGuestAccess(
    uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("invalid command-processor guest-memory access, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	return std::unique_lock(m_resource_mutex);
}

std::unique_lock<ResourceMutex> GpuResourceManager::LockAsynchronousGuestWrite(uint64_t vaddr,
                                                                                uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("invalid asynchronous guest-memory write, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	std::unique_lock transaction(m_resource_mutex);
	if (m_buffer_cache.HasPageOverlap(vaddr, size) || m_texture_cache.HasPageOverlap(vaddr, size) ||
	    m_texture_cache.HasMetaOverlap(vaddr, size)) {
		EXIT("asynchronous guest-memory write to a cached buffer/image range is unsupported, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	return transaction;
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size, GpuAccess access) {
	m_page_manager.OnGpuMap(vaddr, size, access);
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size, GpuAccess access) {
	if (!IsMapped(vaddr, size)) {
		EXIT("cannot unmap an unmapped GPU resource range\n");
	}
	m_texture_cache.UnmapMemory(vaddr, size);
	m_buffer_cache.UnmapMemory(vaddr, size);
	m_page_manager.OnGpuUnmap(vaddr, size, access);
}

void GpuResourceManager::FillBuffer(CommandBuffer* command, uint64_t vaddr, uint64_t size,
                                    uint32_t value) {
	if (g_render_ctx == nullptr || command == nullptr || command->IsInvalid()) {
		EXIT("cannot fill a buffer without a valid render command context\n");
	}
	Common::LockGuard lock(g_render_ctx->GetMutex());
	m_buffer_cache.FillBuffer(command, g_render_ctx->GetGraphicCtx(), vaddr, size, value);
}

void GpuResourceManager::CopyBuffer(CommandBuffer* command, uint64_t dst_vaddr, uint64_t src_vaddr,
                                    uint64_t size) {
	if (g_render_ctx == nullptr || command == nullptr || command->IsInvalid()) {
		EXIT("cannot copy a buffer without a valid render command context\n");
	}
	Common::LockGuard lock(g_render_ctx->GetMutex());
	m_buffer_cache.CopyBuffer(command, g_render_ctx->GetGraphicCtx(), dst_vaddr, src_vaddr, size);
}

} // namespace Libs::Graphics
