#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/resourceMutex.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include <cstdint>
#include <shared_mutex>

namespace Libs::Graphics {

class CommandScheduler;
class Gpu;

class GpuResourceManager {
public:
	GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler);
	~GpuResourceManager();
	KYTY_CLASS_NO_COPY(GpuResourceManager);

	[[nodiscard]] BufferCache&  GetBufferCache() { return m_buffer_cache; }
	[[nodiscard]] TextureCache& GetTextureCache() { return m_texture_cache; }
	void                        SetGpu(Gpu* gpu) noexcept { m_gpu = gpu; }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] bool InvalidateMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();

private:
	PageManager               m_page_manager;
	ResourceMutex             m_resource_mutex;
	BufferCache               m_buffer_cache;
	TextureCache              m_texture_cache;
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	Gpu*                      m_gpu = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
