#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandBuffer;
class CommandScheduler;
class TextureCache;

struct BufferBinding {
	std::shared_ptr<void> owner;
	vk::Buffer            buffer = nullptr;
	uint64_t              offset = 0;
};

struct ImageBufferSource {
	Buffer*  buffer = nullptr;
	uint64_t offset = 0;
};

class BufferCache {
public:
	static constexpr uint64_t CACHING_PAGE_SIZE = 16ull * 1024ull;
	static constexpr uint64_t GetBufferOffset(uint64_t vaddr) {
		return vaddr & (CACHING_PAGE_SIZE - 1);
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                        InvalidateMemory(uint64_t vaddr, uint64_t size);
	void                        ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	void                        UnmapMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] BufferBinding ObtainBuffer(CommandBuffer& command, uint64_t vaddr, uint64_t size,
	                                         bool is_written = false, bool is_read = true,
	                                         bool is_formatted = false);
	[[nodiscard]] StreamBuffer& GetUtilityBuffer(MemoryUsage usage) noexcept;
	[[nodiscard]] Buffer&       GetGdsBuffer() noexcept { return m_gds_buffer; }
	[[nodiscard]] const Buffer& GetGdsBuffer() const noexcept { return m_gds_buffer; }
	[[nodiscard]] BufferBinding UploadTransient(const void* data, uint64_t size,
	                                            uint64_t alignment);
	[[nodiscard]] std::shared_ptr<Buffer> ObtainNullBuffer();
	[[nodiscard]] ImageBufferSource       ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds = false);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds = false,
	                bool src_gds = false);
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();

private:
	friend struct BufferCacheTestAccess;

	struct CacheRange {
		uint64_t address = 0;
		uint64_t size    = 0;
	};
	struct CachedBuffer;
	struct DownloadCopy;
	struct DownloadRange;
	struct RetiredBuffer;
	static constexpr uint64_t               DOWNLOAD_ALIGNMENT = 64;
	[[nodiscard]] static uint64_t           AlignDown(uint64_t value) noexcept;
	[[nodiscard]] static uint64_t           AlignUp(uint64_t value);
	[[nodiscard]] static constexpr uint64_t AlignDownload(uint64_t size) noexcept {
		return (size + DOWNLOAD_ALIGNMENT - 1) & ~(DOWNLOAD_ALIGNMENT - 1);
	}
	[[nodiscard]] static std::pair<uint64_t, uint64_t> DownloadEnvelope(const DownloadCopy& copy);
	[[nodiscard]] static bool ResolveOverlap(CacheRange& merged, CacheRange candidate) noexcept;
	void Upload(CommandBuffer& command, Buffer& destination, uint64_t destination_offset,
	            const void* source, uint64_t size);
	[[nodiscard]] CachedBuffer& GetOrCreateBuffer(CommandBuffer& command, uint64_t vaddr,
	                                              uint64_t size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::vector<DownloadRange> RecordDownloads(std::span<const DownloadCopy> copies);
	void PublishDownloads(std::span<const DownloadRange> downloads);
	void QueueGarbageDownload(std::span<const DownloadCopy> copies, RetiredBuffer retire);
	void WriteHostMemory(uint64_t vaddr, std::span<const uint8_t> data);
	void ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	Buffer                                            m_gds_buffer;
	Common::Mutex                                     m_mutex;
	std::shared_ptr<Buffer>                           m_null_buffer;
	std::map<uint64_t, std::unique_ptr<CachedBuffer>> m_buffers;
	RangeSet                                          m_gpu_modified_ranges;
	MemoryTracker                                     m_memory_tracker;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
