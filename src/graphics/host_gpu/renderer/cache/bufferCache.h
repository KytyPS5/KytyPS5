#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/bufferChunk.h"
#include "graphics/host_gpu/copyWorkers.h"
#include "graphics/host_gpu/gpuWriteTicks.h"
#include "graphics/host_gpu/guestMemoryImport.h"
#include "graphics/host_gpu/guestWriteLog.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/writeWatchSet.h"

#include <array>
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
class PendingLabels;

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
	// The guest unmapped [vaddr, vaddr + size), after InvalidateMemory and with the GPU idle. A
	// buffer that is guest memory itself holds the pages mapped when it was made; the address can
	// get other pages, so such buffers there are dropped, and the next use makes them again.
	void UnmapMemory(uint64_t vaddr, uint64_t size);
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
	// `skip_prefix`: bytes at the start the caller does not read; a staging copy leaves them as they
	// are (an expanded image takes its first layers from the image it replaces).
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size,
	                                                                uint64_t skip_prefix = 0);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	// Whether a buffer over the range is (or would be made) guest memory itself
	// (guestMemoryImport.h): the GPU reads what the CPU writes there, in GPU order.
	[[nodiscard]] bool IsGuestBacked(uint64_t vaddr, uint64_t size);
	// GPU thread. Reads or writes guest memory of a buffer that is guest memory itself, for the
	// command processor: after the buffer's last GPU write, and without the fault of a page
	// the GPU owns (through the direct-memory mapping). False when no such buffer holds the
	// range; the caller then accesses guest memory as usual.
	[[nodiscard]] bool ReadGuestBacked(uint64_t vaddr, void* data, uint64_t size);
	[[nodiscard]] bool WriteGuestBacked(uint64_t vaddr, const void* data, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	// Any thread (the front worker, see frontWorker.h); HasGpuDirtyBytes is for the GPU thread.
	[[nodiscard]] bool HasGpuDirtyBytesShared(uint64_t vaddr, uint64_t size);
	// Raised when GPU-written bytes are added: what a reader found clean before may not be now.
	[[nodiscard]] uint64_t GpuModifiedVersion() const noexcept { return m_gpu_modified_version; }
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
	// The guest mapped or unmapped [vaddr, vaddr + size): only buffers there can have changed.
	void RecordMappingChange(uint64_t vaddr, uint64_t size);
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
	// The end-of-pipe labels the host GPU has not completed: a GPU read recorded after a label
	// sees its value (ApplyPendingLabels), though guest memory gets it only on completion.
	void SetPendingLabels(PendingLabels* labels) noexcept { m_labels = labels; }
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
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
	// What a buffer created for [vaddr, vaddr + size) covers: its chunk, as one piece of direct
	// memory when it can be, clipped to copy-only pages, with the buffers it overlaps.
	[[nodiscard]] OverlapResult PlanBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	// Writes the labels pending over [vaddr, vaddr + size) into `buffer`, in GPU order.
	void ApplyPendingLabels(Buffer& buffer, uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] std::pair<uint8_t*, uint64_t> MapStaging(uint64_t size);
	// Copies guest memory that can have unmapped pages (a sparse mapping reads as zeros there).
	// The copy function of m_copy_workers.
	static void CopyGuestMemory(void* destination, const void* source, size_t size);
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
	// A read-back of a buffer that is guest memory itself: the GPU's writes there are complete,
	// and nothing is copied. True when the range held GPU-written pages.
	[[nodiscard]] bool ForgetGuestBackedWrites(uint64_t vaddr, uint64_t size);
	// Buffers that are guest memory itself and that shaders write through formatted stores become
	// copies (m_copy_only): at the next frame boundary, when no draw being prepared refers to them.
	void DemoteGuestBackedBuffers();
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	// Buffers over direct memory are guest memory itself where the host can import it.
	GuestMemoryImport                                 m_import;
	// What the GPU wrote to buffers that are guest memory, and when (gpuWriteTicks.h).
	GpuWriteTicks                                     m_guest_backed_writes;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// Held by the GPU thread to change m_gpu_modified_ranges and by others to read it.
	std::mutex                                        m_gpu_modified_mutex;
	// Raised when a range is added to m_gpu_modified_ranges (see HasGpuDirtyBytes).
	uint64_t                                          m_gpu_modified_version = 0;
	struct CleanPage {
		uint64_t page    = 0; // page + 1; 0: none
		uint64_t version = 0;
	};
	static constexpr uint32_t                         CleanPageBits = 12;
	std::array<CleanPage, 64>                         m_clean_pages {};
	uint64_t                                          m_address_write_tick = 0;
	// Prepared for a GPU write that is not recorded yet. See SettleGpuWrites.
	static constexpr size_t                           MaxUnsettledWriters = 256;
	std::vector<BufferId>                             m_unsettled_writers;
	// Memory that only buffers that are copies hold, and the buffers that are guest memory itself
	// still to become copies (DemoteGuestBackedBuffers). Formatted stores write image-like data
	// next to and over images; with the buffer as guest memory itself, Sonic Racing CrossWorlds
	// rendered its world and cars black in some frames, and in every frame when the host GPU
	// kept pace with the guest. As copies they render as before.
	// Only the pages written that way are copy-only. A buffer that is guest memory itself keeps
	// out of them, and a copy holds no more than the copy-only pages it touches and the request
	// (ClipToCopyOnly): the rest of a chunk stays guest memory, and CPU writes there need no
	// upload. FC 25 reuses memory: whole chunks made copy-only by one formatted store sent up
	// to 18 MB a frame of CPU writes through uploads.
	RangeSet                                          m_copy_only;
	// The pages marked copy-only since the last generation: a page that is not marked again for
	// a generation (CopyOnlyGeneration collections) stops being copy-only, and the copies over it
	// become guest memory again (ExpireCopyOnly). Memory a game reuses for other data then needs
	// no uploads.
	RangeSet                                          m_copy_only_marked;
	uint32_t                                          m_copy_only_age = 0;
	static constexpr uint32_t                         CopyOnlyGeneration = 120;
	void                                              ExpireCopyOnly();
	// A copy over memory that could be guest memory itself: nothing only the copy holds, no
	// copy-only page, one piece of placeable memory.
	[[nodiscard]] bool CanPromote(BufferId id);
	// The copies that can be promoted are deleted: their next use makes them guest memory
	// itself (CreateBuffer). At a collection, when no draw being prepared refers to them.
	void PromoteCopies(std::vector<BufferId>& ids);
	// Copies that uploaded CPU writes since the last collection while their memory could be
	// guest memory itself: a copy made for a reason that is gone (an overlap, a copy-only page
	// that expired under another buffer) is otherwise a copy for good, and every CPU write to
	// it faults and is uploaded.
	static constexpr size_t MaxUploadingCopies = 4096;
	std::vector<BufferId>   m_uploading_copies;
	// Adds the pages of a range to m_copy_only; false when it held them already.
	bool MarkCopyOnly(uint64_t vaddr, uint64_t size);
	// Narrows a new buffer's [begin, end) around [request_begin, request_end) (m_copy_only).
	void ClipToCopyOnly(uint64_t request_begin, uint64_t request_end, uint64_t& begin,
	                    uint64_t& end) const;
	std::vector<BufferId>                             m_demote;
	bool                                              m_unsettled_everywhere = false;
	// Only tests set this: it keeps the copy path covered on a device where the CPU can read
	// buffer memory.
	bool                                              m_read_back_through_gpu = false;
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 8>                           m_checked_color_metadata;
	MemoryTracker                                     m_memory_tracker;
	WriteWatchSet                                      m_write_watches;
	PendingLabels*                                     m_labels = nullptr;
	bool                                              m_upload_batch_open    = false;
	bool                                              m_upload_batch_started = false;
	// See TakeGuestWrites. Written by the fault thread and the GPU thread.
	GuestWriteLog                                     m_guest_write_log {MaxGuestWriteLog};
	StreamBuffer                                      m_staging_buffer;
	// In an upload batch, UploadCopies records the CPU copies to the staging buffer, and the
	// copy workers run them all at once before the batch ends: the upload passes of a frame make
	// about a thousand uploads of about 140 KB each, too small to share one by one. The guest
	// memory stays mapped until then: PrepareBda ends the batch before it unlocks the mapping.
	CopyWorkers                                       m_copy_workers {BatchCopyWorkers,
	                                                                 CopyGuestMemory};
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
