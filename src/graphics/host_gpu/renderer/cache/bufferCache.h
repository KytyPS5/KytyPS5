#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <map>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);
	// The fault buffer holds two bitmaps of CACHING_NUMPAGES bits: pages that device-address
	// accesses found without a cached buffer, then pages that V#-table stores wrote.
	static constexpr uint64_t WRITE_BITMAP_OFFSET = CACHING_NUMPAGES / 8;
	static constexpr uint64_t FAULT_BUFFER_SIZE   = 2 * WRITE_BITMAP_OFFSET;

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	// A guest write faulted on a page the cache protects.
	void                   InvalidateFaultedPage(uint64_t vaddr);
	// A CPU write that already reached guest memory, from a thread that cannot read back:
	// pages the GPU owns stay GPU-owned, the rest become CPU-modified.
	void                   InvalidateUnlessGpuModified(uint64_t vaddr, uint64_t size) noexcept;
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	// Copies guest memory into the mapped stream buffer, possibly on the host copy thread (see
	// CommandScheduler::HostCopies()).
	void StreamGuestData(uint8_t* destination, uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasOnlyGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
	void               PrepareFaultBuffer() { m_fault_manager.PrepareFaultBuffer(); }
	void               ProcessFaultBuffer();
	void ProcessWriteBuffer(FaultManager::PagesHandler&& handler) {
		m_fault_manager.ProcessWriteBuffer(std::move(handler));
	}
	// Records stores that a shader made through device addresses, known once it completed: the
	// bytes of [vaddr, vaddr + size) that a cached buffer holds become GPU modified, except on
	// pages the CPU dirtied since, whose upload replaces them.
	void NoteGpuWrites(uint64_t vaddr, uint64_t size);
	// Calls func(address, size) for the runs of [vaddr, vaddr + size) the CPU may have written
	// since the previous call over them (see MemoryTracker::ConsumeCpuWrites).
	template <typename Func>
	void ConsumeCpuWrites(uint64_t vaddr, uint64_t size, Func&& func) {
		m_memory_tracker.ConsumeCpuWrites(vaddr, size, std::forward<Func>(func));
	}
	// Calls func(address, size) for the ranges whose buffers the garbage collector released since
	// the previous call, and forgets them; the first call starts recording them. Merging buffers
	// keeps every address cached.
	template <typename Func>
	void ConsumeReleasedRanges(Func&& func) {
		m_released_ranges.ForEach(func);
		m_released_ranges.Clear();
		m_record_released = true;
	}
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	// Uploads the pending CPU writes of the buffers within `ranges`. Visits only the tracking
	// regions that hold a registered buffer and whose pages became CPU dirty since the previous
	// call, or that were forced since (a buffer registered there, ForceSynchronization()): that
	// call left no pending write under a buffer anywhere else in `ranges`. `all` visits every
	// region that holds a buffer.
	void SynchronizeBuffersInRanges(const RangeSet& ranges, bool all);
	// The next SynchronizeBuffersInRanges() visits the regions of the range whatever their epoch
	// (the range was mapped: parts of buffers there were outside the ranges synchronized so far).
	void ForceSynchronization(uint64_t vaddr, uint64_t size);
	// Uploads the pending CPU writes of the buffers within each [first, second) span, recorded
	// together as SynchronizeBuffersInRanges() records them.
	void SynchronizeBuffersInSpans(std::span<const std::pair<uint64_t, uint64_t>> spans);
	void FlushUploadBatch();
	// FlushUploadBatch() as one dispatch; false when the batch does not fit it.
	bool ScatterUploadBatch(const CommandRecorder& native);
	// Advances whenever a buffer is registered: a new buffer may cover CPU-dirty pages that no
	// earlier synchronization uploaded.
	[[nodiscard]] uint64_t RegisterEpoch() const { return m_register_epoch; }
	// Advances whenever recorded work may write cached buffers (written bindings, fills, copies,
	// device-address stores).
	[[nodiscard]] uint64_t GpuWriteGeneration() const noexcept { return m_gpu_write_generation; }
	// Advances whenever a page of the range becomes CPU dirty (see
	// MemoryTracker::RegionCpuDirtyEpoch); pages that stay CPU dirty do not advance it.
	[[nodiscard]] uint64_t CpuDirtyEpoch(uint64_t vaddr, uint64_t size) const {
		uint64_t epoch = 0;
		for (auto index = vaddr / TRACKER_REGION_SIZE; index * TRACKER_REGION_SIZE < vaddr + size;
		     index++) {
			epoch += m_memory_tracker.RegionCpuDirtyEpoch(index);
		}
		return epoch;
	}
	void               RunGarbageCollector();

private:
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const { return !id || m_slot_buffers[id].is_deleted; }

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 16>                          m_checked_color_metadata;
	RangeSet                                          m_released_ranges;
	bool                                              m_record_released = false;
	MemoryTracker                                     m_memory_tracker;
	uint64_t                                           m_gpu_write_generation = 0;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_register_epoch     = 0;
	// Advances whenever a buffer is registered or unregistered.
	uint64_t m_buffer_set_epoch = 0;
	// The buffer a read-only binding of a range found, while no buffer was registered or
	// unregistered and no page of the range became CPU dirty since (see ObtainBuffer()).
	struct ObtainMemo {
		uint64_t vaddr        = 0;
		uint64_t size         = 0;
		uint64_t buffer_epoch = 0;
		uint64_t cpu_epoch    = 0;
		uint64_t offset       = 0;
		BufferId id;
	};
	// The stream copy a range got within the current draw window (see ObtainBuffer()).
	struct StreamMemo {
		uint64_t vaddr            = 0;
		uint64_t size             = 0;
		uint64_t window           = UINT64_MAX;
		uint64_t lap              = 0;
		uint64_t write_generation = 0;
		uint64_t offset           = 0;
	};
	static constexpr size_t                                  StreamMemoSlots = 1024;
	std::unique_ptr<std::array<StreamMemo, StreamMemoSlots>> m_stream_memo =
	    std::make_unique<std::array<StreamMemo, StreamMemoSlots>>();
	static constexpr size_t                                  ObtainMemoSlots = 4096;
	std::unique_ptr<std::array<ObtainMemo, ObtainMemoSlots>> m_obtain_memo =
	    std::make_unique<std::array<ObtainMemo, ObtainMemoSlots>>();
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	// Per tracking region (address / TRACKER_REGION_SIZE), grown to the highest region that held a
	// buffer: the number of registered buffers it holds, bitmaps of the regions holding any and of
	// the regions to visit at the next synchronization whatever their epoch, and the CPU-dirty
	// epoch as SynchronizeBuffersInRanges last visited it. A synchronization walks the bitmap of
	// regions with buffers instead of every region of the mapped ranges (thousands in Demon's
	// Souls, at each dispatch).
	void                                       NoteBufferRegions(const Buffer& buffer, bool insert);
	void                                       GrowRegionTables(uint64_t region);
	std::vector<uint32_t>                      m_region_buffer_count;
	std::vector<uint64_t>                      m_buffer_region_bits;
	std::vector<uint64_t>                      m_forced_region_bits;
	std::vector<uint64_t>                      m_synced_region_epochs;
	std::vector<std::pair<uint64_t, uint64_t>> m_sync_regions;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
