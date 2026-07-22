#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"

#include <map>
#include <memory>
#include <mutex>

namespace Libs::Graphics {

struct GraphicContext;
struct VulkanBuffer;
class CommandBuffer;
class GpuResourceManager;
class TextureCache;
class ResourceMutex;

struct BufferImageCopySource {
	VulkanBuffer* buffer      = nullptr;
	uint64_t      offset      = 0;
	uint64_t      address     = 0;
	uint64_t      size        = 0;
	bool          cpu_current = false;
	// True when the guest range was CPU-dirty before coherence resolution. ObtainBufferForImage
	// may consume that tracker state while publishing the same bytes to a cached buffer.
	bool cpu_dirty = false;
};

struct BufferCacheRange {
	uint64_t address = 0;
	uint64_t size    = 0;
};

struct BufferBinding {
	VulkanBuffer& buffer;
	uint64_t      offset;
};

[[nodiscard]] bool MergeOverlappingBufferCacheRange(BufferCacheRange& merged,
                                                    BufferCacheRange  candidate) noexcept;
[[nodiscard]] bool CanMergeBufferCacheQueueMask(uint64_t queue_mask, uint32_t queue) noexcept;

class BufferCache {
public:
	static constexpr uint64_t CACHING_PAGE_SIZE = 16ull * 1024ull;
	static constexpr uint64_t GetBufferOffset(uint64_t vaddr) {
		return vaddr & (CACHING_PAGE_SIZE - 1);
	}

	BufferCache(GraphicContext& graphics, PageManager& page_manager, ResourceMutex& resource_mutex);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	[[nodiscard]] bool InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
	                                    PageFaultPhase phase) noexcept;
	[[nodiscard]] BufferBinding ObtainBuffer(CommandBuffer& command, uint64_t vaddr, uint64_t size,
	                                         bool is_written = false, bool is_read = true,
	                                         bool is_formatted = false);
	// Emulator-owned, CPU-current scratch only. Guest ranges must use ObtainBuffer so page
	// ownership is resolved before any CPU access.
	[[nodiscard]] bool UploadHostData(CommandBuffer& command, const void* src, uint64_t size,
	                                  uint64_t alignment, VulkanBuffer*& out_buffer,
	                                  uint64_t& out_offset, uint64_t& out_range);
	[[nodiscard]] VulkanBuffer&         ObtainNullBuffer(CommandBuffer& command);
	[[nodiscard]] BufferImageCopySource ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(CommandBuffer* command, uint64_t vaddr, uint64_t size, uint32_t value);
	void CopyBuffer(CommandBuffer* command, uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size);
	[[nodiscard]] bool HasPageOverlap(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               PublishImageBacking(uint64_t vaddr, uint64_t size);
	void ValidateGpuAccess(uint64_t vaddr, uint64_t size, bool is_read, bool is_written) const;
	void SetTextureCache(TextureCache& texture_cache);

	void ResetNullBuffer();

private:
	friend class GpuResourceManager;
	void ValidateUnmapMemory(uint64_t vaddr, uint64_t size);
	void CommitUnmapMemory(uint64_t vaddr, uint64_t size);
	void ValidateUnmapMemoryLocked(uint64_t vaddr, uint64_t size);
	struct CachedBuffer;
	struct ReadbackWorker;

	GraphicContext&               m_graphics;
	Common::Mutex                 m_mutex;
	std::shared_ptr<VulkanBuffer> m_null_buffer;
	// TODO: add LRU cache
	std::map<uint64_t, std::unique_ptr<CachedBuffer>> m_buffers;
	std::unique_ptr<ReadbackWorker>                   m_readback;
	RangeSet                                          m_gpu_modified_ranges;
	MemoryTracker                                     m_memory_tracker;
	PageManager&                                      m_page_manager;
	TextureCache*                                     m_texture_cache = nullptr;
	ResourceMutex&                                    m_resource_mutex;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
