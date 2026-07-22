#include "graphics/host_gpu/renderer/gpuResourceManager.h"

#include "common/assert.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <optional>

namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics)
    : m_page_manager(FaultThunk, this), m_buffer_cache(graphics, m_page_manager, m_resource_mutex),
      m_texture_cache(graphics, m_page_manager, m_buffer_cache, m_resource_mutex) {
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
	if (auto* cp = GraphicsRunCurrentCommandProcessor(); cp != nullptr) {
		cp->BeginReadbackTransaction();
		bool handled = false;
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			handled = m_page_manager.HandleFault(access, fault_vaddr);
		}
		cp->EndReadbackTransaction();
		return handled;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported page fault from a pre-owned resource transaction, addr=0x%016" PRIx64
		     " access=%u\n",
		     fault_vaddr, static_cast<uint32_t>(access));
	}
	// Stop command-processor jobs before taking the shared cache transaction. External readback
	// workers inherit this paused state and therefore never form resource -> submission lock
	// inversion.
	std::optional<GraphicsRunSubmissionLock> submissions;
	if (GraphicsRunIsInitialized()) {
		submissions.emplace();
	}
	ResourceMutex::FaultScope fault(m_resource_mutex);
	return m_page_manager.HandleFault(access, fault_vaddr);
}

void GpuResourceManager::PrepareHostWrite(uint64_t vaddr, uint64_t size) {
	if (!m_page_manager.HasAnyMapping(vaddr, size)) {
		return;
	}
	if (LabelInCallback()) {
		EXIT("unsupported host write from an asynchronous GPU label callback, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto handle_range = [this, vaddr, size]() {
		if (!m_page_manager.HandleWriteRange(vaddr, size)) {
			EXIT("failed to prepare host write, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
	};
	if (auto* cp = GraphicsRunCurrentCommandProcessor(); cp != nullptr) {
		cp->BeginReadbackTransaction();
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			handle_range();
		}
		cp->EndReadbackTransaction();
		return;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported host write from a pre-owned resource transaction, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	std::optional<GraphicsRunSubmissionLock> submissions;
	if (GraphicsRunIsInitialized()) {
		submissions.emplace();
	}
	ResourceMutex::FaultScope fault(m_resource_mutex);
	handle_range();
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
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("cannot unmap from a pre-owned resource transaction\n");
	}
	std::optional<GraphicsRunSubmissionLock> submissions;
	if (GraphicsRunIsInitialized()) {
		submissions.emplace();
	}
	std::lock_guard          transaction(m_resource_mutex);
	m_texture_cache.ValidateUnmapMemory(vaddr, size);
	m_buffer_cache.ValidateUnmapMemory(vaddr, size);
	m_texture_cache.CommitUnmapMemory(vaddr, size);
	m_buffer_cache.CommitUnmapMemory(vaddr, size);
	m_page_manager.OnGpuUnmap(vaddr, size, access);
}

void GpuResourceManager::FillBuffer(CommandBuffer& command, uint64_t vaddr, uint64_t size,
                                    uint32_t value) {
	if (command.IsInvalid()) {
		EXIT("cannot fill a buffer without a valid render command context\n");
	}
	Common::LockGuard lock(GetRenderContext().GetMutex());
	m_buffer_cache.FillBuffer(&command, vaddr, size, value);
}

void GpuResourceManager::CopyBuffer(CommandBuffer& command, uint64_t dst_vaddr, uint64_t src_vaddr,
                                    uint64_t size) {
	if (command.IsInvalid()) {
		EXIT("cannot copy a buffer without a valid render command context\n");
	}
	Common::LockGuard lock(GetRenderContext().GetMutex());
	m_buffer_cache.CopyBuffer(&command, dst_vaddr, src_vaddr, size);
}

} // namespace Libs::Graphics
