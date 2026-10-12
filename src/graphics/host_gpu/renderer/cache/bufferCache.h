#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/bufferChunk.h"
#include "graphics/host_gpu/copyWorkers.h"
#include "graphics/host_gpu/guestWriteLog.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/writeWatchSet.h"

#include <map>
#include <mutex>
#include <optional>
#include <span>
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
	// Gives the whole range new bytes, on the GPU thread. Unlike InvalidateMemory followed by a
	// write, the bytes the GPU wrote there are not fetched first.
	void ReplaceMemory(uint64_t vaddr, const void* data, uint64_t size);
	// A CPU write faulted at `fault_vaddr`. Unprotects the whole aligned window around it when no
	// page in the window holds GPU-modified data, so a run of writes costs one fault, not one per
	// page. Pages the CPU did not write count as written, and the next GPU use uploads them.
	void                   InvalidateWrittenMemory(uint64_t fault_vaddr, bool window_is_mapped);
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
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Moves into `ranges` the guest ranges that gained bytes the GPU has not seen since the last
	// call, sorted and merged (guestWriteLog.h). False when the log overflowed or a full pass was
	// requested; the caller must then synchronize every buffer.
	[[nodiscard]] bool TakeGuestWrites(std::vector<GuestRange>& ranges);
	void               RequestFullSynchronization();
	// GPU thread. A guest submission starts (see GuestWriteLog::BeginSubmission).
	void BeginSubmission();
	// GPU thread. The GPU thread itself wrote guest memory (WRITE_DATA, a label): a later
	// draw of the same submission must see it, also on a page that stays open.
	void RecordGpuThreadWrite(uint64_t vaddr, uint64_t size);
	// A shader that writes through device addresses is being prepared. It can write any buffer,
	// so it counts as a write of every buffer for a read-back (see readbackPlan.h).
	void NoteAddressWrites() noexcept;
	// The draw or dispatch that the buffers were prepared for has been recorded, or given up.
	// Until this call a buffer that was prepared for writing counts as written by a command
	// that is still to come: a submission can happen between preparation and recording.
	void SettleGpuWrites() noexcept;
	// Every CPU or GPU write that touches a watched range gives it a new generation. Writers of
	// guest memory that do not go through the buffer cache, such as image downloads, notify it
	// themselves.
	[[nodiscard]] WriteWatchSet& WriteWatches() noexcept { return m_write_watches; }
	void               ProcessFaultBuffer();
	// Between Begin and End, uploads share one barrier before the first copy and one after the
	// last. A barrier between two copies makes the Metal backend start a new blit encoder for
	// each copy; a run of copies with nothing in between shares one. Only SynchronizeBuffersInRange
	// may run inside a batch: no draw, no dispatch, no ObtainBuffer.
	void               BeginUploadBatch();
	void               EndUploadBatch();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();

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
	[[nodiscard]] std::pair<uint8_t*, uint64_t> MapStaging(uint64_t size);
	// Copies the guest bytes of the open upload batch to the staging buffer.
	void RunBatchCopies();
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	static constexpr size_t  MaxGuestWriteLog = 4096;
	// The workers that copy an upload batch together with the GPU thread (copyWorkers.h).
	static constexpr uint32_t BatchCopyWorkers = 3;
	// A range of the staging buffer to flush after the batch copies.
	struct StagingFlush {
		uint64_t offset = 0;
		uint64_t size   = 0;
	};
	// Records a range for TakeGuestWrites.
	void RecordGuestWrite(uint64_t vaddr, uint64_t size);
	// GPU thread. One round of a read-back, by the rule of readbackPlan.h. Returns the submission
	// the caller must wait for before it asks again, or nothing when guest memory is current.
	[[nodiscard]] std::optional<uint64_t> ReadBack(uint64_t vaddr, uint64_t size, bool is_write,
	                                               bool caller_can_wait);
	// Copies the GPU-written bytes of the range from the buffer's memory to guest memory. The
	// last GPU-side write of the buffer must have finished.
	[[nodiscard]] bool PublishFromBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);

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
	uint64_t                                          m_address_write_tick = 0;
	// Prepared for a GPU write that is not recorded yet. See SettleGpuWrites.
	static constexpr size_t                           MaxUnsettledWriters = 256;
	std::vector<BufferId>                             m_unsettled_writers;
	bool                                              m_unsettled_everywhere = false;
	// Only tests set this: it keeps the copy path covered on a device where the CPU can read
	// buffer memory.
	bool                                              m_read_back_through_gpu = false;
	MemoryTracker                                     m_memory_tracker;
	WriteWatchSet                                      m_write_watches;
	bool                                              m_upload_batch_open    = false;
	bool                                              m_upload_batch_started = false;
	// See TakeGuestWrites. Written by the fault thread and the GPU thread.
	GuestWriteLog                                     m_guest_write_log {MaxGuestWriteLog};
	StreamBuffer                                      m_staging_buffer;
	// In an upload batch, UploadCopies records the CPU copies to the staging buffer, and the
	// copy workers run them all at once before the batch ends: the upload passes of a frame make
	// about a thousand uploads of about 140 KB each, too small to share one by one. The guest
	// memory stays mapped until then: PrepareBda ends the batch before it unlocks the mapping.
	CopyWorkers                                       m_copy_workers {BatchCopyWorkers};
	std::vector<CopyWorkers::Copy>                    m_batch_copies;
	std::vector<StagingFlush>                         m_batch_flushes;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	// The size of the chunk a new buffer covers (see bufferChunk.h). The tests that check how the
	// cache lays out its buffers set it to one page.
	static inline uint64_t s_chunk_size = BUFFER_CHUNK_SIZE;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
