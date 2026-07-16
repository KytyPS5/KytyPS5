#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/resourceMutex.h"
#include "graphics/host_gpu/renderer/textureCache.h"

#include <cstdint>
#include <mutex>

namespace Libs::Graphics {

class CommandBuffer;

class GpuResourceManager {
public:
	GpuResourceManager();
	~GpuResourceManager();
	KYTY_CLASS_NO_COPY(GpuResourceManager);

	[[nodiscard]] BufferCache*  GetBufferCache() { return &m_buffer_cache; }
	[[nodiscard]] TextureCache* GetTextureCache() { return &m_texture_cache; }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	// Serializes synchronous command-processor guest-memory access against buffer/image ownership
	// publication. Address metadata keeps this seam ready for a future page-range lock.
	[[nodiscard]] std::unique_lock<ResourceMutex>
	LockCommandProcessorGuestAccess(uint64_t vaddr, uint64_t size);
	// Keeps an asynchronous host callback's validation and guest write in one cache transaction.
	// Cached buffer/image destinations are unsupported and fail before the callback can fault.
	[[nodiscard]] std::unique_lock<ResourceMutex> LockAsynchronousGuestWrite(uint64_t vaddr,
	                                                                        uint64_t size);
	void               MapMemory(uint64_t vaddr, uint64_t size, GpuAccess access);
	void               UnmapMemory(uint64_t vaddr, uint64_t size, GpuAccess access);
	void FillBuffer(CommandBuffer* command, uint64_t vaddr, uint64_t size, uint32_t value);
	void CopyBuffer(CommandBuffer* command, uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size);

private:
	static bool FaultThunk(void* context, PageFaultAccess access, uint64_t vaddr, uint64_t size,
	                       PageFaultPhase phase) noexcept;
	[[nodiscard]] bool InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
	                                    PageFaultPhase phase) noexcept;

	PageManager   m_page_manager;
	ResourceMutex m_resource_mutex;
	BufferCache   m_buffer_cache;
	TextureCache m_texture_cache;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
