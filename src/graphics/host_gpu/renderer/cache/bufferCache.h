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
#include "graphics/host_gpu/renderer/cache/pageDiff.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <set>
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
	static constexpr uint32_t CACHING_PAGEBITS = 14;
	static constexpr uint64_t CACHING_PAGESIZE = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES =
	    (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE = CACHING_NUMPAGES * sizeof(vk::DeviceAddress);
	// Device addresses stay below bit 63, which tags pages whose BDA stores the host owns.
	static constexpr uint64_t BDA_STORE_TRACKED_BIT = uint64_t {1} << 63;

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
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t>
	ObtainBuffer(uint64_t vaddr, uint64_t size, bool is_written, bool is_texel_buffer = false,
	             BufferId id = {}, bool raw_image_read = false);
	[[nodiscard]] StreamBuffer& GetUtilityBuffer(MemoryUsage usage) noexcept {
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
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void               ReconcileSupersededImage(Common::SlotId id);
	[[nodiscard]] bool CopyImageForBufferCopy(uint64_t source, uint64_t destination, uint64_t size);
	// Repeats a pattern of `words` dwords over the range; size is a multiple of the pattern.
	void FillBufferPattern(uint64_t vaddr, uint64_t size, const uint32_t* pattern, uint32_t words);
	void WriteMemory(uint64_t vaddr, const void* data, uint64_t size);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
	void               ProcessFaultBuffer();
	// A BDA fault on an owned page can only come from a store, which promotes the page to tracked.
	void ResolveBdaFault(uint64_t vaddr, uint64_t size);
	// A store prefetch tracks the first `store_window` bytes, so its pages are owned up front.
	void PrefetchBda(uint64_t vaddr, uint64_t size, bool store, uint64_t store_window = 64 * 1024);
	void ForgetBdaResidency(uint64_t vaddr, uint64_t size);
	void SynchronizeBda(const RangeSet& mapped);
	void QueueBdaSync(uint64_t vaddr, uint64_t size) { m_memory_tracker.QueueBdaSync(vaddr, size); }
	// Marks the tracked pages a BDA store can reach GPU-written, restricted to mapped memory.
	void               MarkBdaStoresInMapped(const RangeSet& mapped, bool all_tracked);
	[[nodiscard]] bool HasUnmarkedBdaStores() const { return !m_bda_unmarked_ranges.Empty(); }
	void               ClearUnmarkedBdaStores() { m_bda_unmarked_ranges.Clear(); }
	void               RunGarbageCollector();
	void               BeginPreciseWrites();
	void               EndPreciseWrites();

private:
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const {
		const auto* buffer = m_slot_buffers.try_get(id);
		return buffer == nullptr || buffer->is_deleted;
	}

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
	void ReadMemoryAttempt(uint64_t vaddr, uint64_t size, bool is_write, uint32_t attempt);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void                     ChangeRegister(BufferId id);
	void                     DeleteBuffer(BufferId id);
	[[nodiscard]] bool       SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                           bool is_written, bool is_texel_buffer,
	                                           bool raw_image_read = false);
	void                     SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                              bool whole_image = false);
	[[nodiscard]] bool WriteBackImage(Buffer& buffer, Common::SlotId id, uint64_t size,
	                                  bool whole_image);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void               MarkBdaStores(uint64_t vaddr, uint64_t size);
	void               MakePageTableResident(uint64_t table_offset, uint64_t size);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);

	// The tick of the recording that last wrote a range, kept until that tick completes.
	struct GpuWriteMark {
		uint64_t end  = 0;
		uint64_t tick = 0;
	};
	void MarkGpuWrite(uint64_t vaddr, uint64_t size);
	// The newest tick of a mark overlapping the range, or 0.
	[[nodiscard]] uint64_t NewestGpuWriteTick(uint64_t vaddr, uint64_t size) const;
	void                   PruneGpuWriteMarks();
	// Downloads without submitting the recording in progress; false leaves all state unchanged.
	[[nodiscard]] bool TryDownloadDetached(Buffer& buffer, uint64_t vaddr, uint64_t size);
	struct GuestDownload {
		int                         slot  = -1;
		vk::Fence                   fence = nullptr;
		std::unique_ptr<Buffer>     staging;
		std::vector<vk::BufferCopy> copies;
		RangeSet                    pages;
		uint64_t                    buffer_address = 0;
		uint64_t                    wait           = 0;
		size_t                      writes_start   = 0;
	};
	[[nodiscard]] bool BeginGuestDownload(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                      GuestDownload& download);
	void               NoteGuestFault(uint64_t vaddr, uint64_t size);
	void               NoteHotWrite(uint64_t vaddr, uint64_t size);
	void               RecordHotWriteBacks();
	// One copied range of a hot write-back: guest address, offset in the staging buffer, bytes.
	struct HotCopy {
		uint64_t address = 0;
		uint64_t staging = 0;
		uint64_t size    = 0;
	};
	void PublishHotWriteBack(const std::vector<HotCopy>& copies, const uint8_t* mapped,
	                         size_t writes_start);
	// One submit's hot write-back, published once its recording has completed.
	struct HotBatch {
		uint64_t                tick = 0;
		std::vector<HotCopy>    copies;
		std::unique_ptr<Buffer> staging;
		uint64_t                size         = 0;
		size_t                  writes_start = 0;
	};
	void PublishCompletedHotWriteBacks(uint64_t max_tick = UINT64_MAX);
	// Tick after which pending hot write-backs carry every GPU-written byte of the range, or 0.
	[[nodiscard]] uint64_t PendingHotWriteBackTick(uint64_t vaddr, uint64_t size);
	[[nodiscard]] size_t   OpenWriteLog();
	[[nodiscard]] bool     WrittenSince(size_t start, uint64_t begin, uint64_t end) const;
	void                   CloseWriteLog(size_t start);
	void FinishGuestDownload(GuestDownload& download, uint64_t vaddr, uint64_t size, bool is_write);
	[[nodiscard]] uint64_t PrepareDetachedDownload(uint64_t vaddr, uint64_t size);

	struct PreciseWrite {
		uint64_t                begin      = 0;
		uint64_t                end        = 0;
		uint64_t                page_begin = 0;
		uint64_t                page_end   = 0;
		RangeSet                before;
		RangeSet                rewritten;
		std::unique_ptr<Buffer> snapshot;
		std::unique_ptr<Buffer> changed;
		bool                    compared = false;
	};
	[[nodiscard]] bool CanTrackPreciseWrite(const Buffer& buffer, uint64_t vaddr, uint64_t size);
	void TrackPreciseWrite(Buffer& buffer, uint64_t vaddr, uint64_t size, RangeSet before);
	void ResolvePreciseWrite();

	GraphicContext&                                    m_graphics;
	CommandScheduler&                                  m_scheduler;
	FaultManager                                       m_fault_manager;
	Buffer                                             m_gds_buffer;
	Buffer                                             m_bda_pagetable_buffer;
	uint64_t                                           m_bda_table_chunk        = 0;
	uint32_t                                           m_bda_table_memory_types = 0;
	std::vector<VmaAllocation>                         m_bda_table_chunks;
	vk::Fence                                          m_bda_table_fence = nullptr;
	Common::SlotVector<Buffer>                         m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 8> m_checked_color_metadata;
	RangeSet                m_bda_tracked_ranges;
	// Tracked since the last mark, so stores that ran before tracking are owned without a rerun.
	RangeSet m_bda_unmarked_ranges;
	RangeSet m_bda_pinned_ranges;
	// Disjoint; a mark whose tick has completed no longer constrains anything and is pruned.
	std::map<uint64_t, GpuWriteMark>          m_gpu_write_marks;
	std::deque<std::pair<uint64_t, uint64_t>> m_guest_download_writes;
	// Absolute index of the log's first entry, and the starts of every open user.
	size_t                m_write_log_base = 0;
	std::multiset<size_t> m_write_log_users;
	bool                  m_guest_retry_async = true;
	// Tracker page -> garbage-collection tick of the last guest fault on it.
	std::unordered_map<uint64_t, uint64_t> m_guest_hot_pages;
	// Hot pages the recording in progress wrote.
	RangeSet             m_hot_written;
	std::deque<HotBatch> m_hot_batches;
	// A BDA store can reach any page, including ones no mark names yet.
	uint64_t      m_bda_store_tick = 0;
	MemoryTracker m_memory_tracker;
	StreamBuffer  m_staging_buffer;
	StreamBuffer  m_stream_buffer;
	StreamBuffer  m_download_buffer;
	StreamBuffer  m_device_buffer;
	TextureCache& m_texture_cache;
	uint64_t      m_total_used_memory  = 0;
	uint64_t      m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t      m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t      m_gc_tick            = 0;

	PageDiff                                          m_page_diff;
	std::deque<PreciseWrite>                          m_precise_writes;
	std::vector<std::unique_ptr<Buffer>>              m_precise_snapshots;
	std::map<std::pair<uint64_t, uint64_t>, uint32_t> m_precise_dense;
	bool                                              m_precise_scope   = false;
	bool                                              m_precise_marking = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
