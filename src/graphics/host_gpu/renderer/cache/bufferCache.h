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
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class CommandRecorder;
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
	// The fault buffer holds three bitmaps of CACHING_NUMPAGES bits: pages that device-address
	// accesses found without a cached buffer, pages that V#-table stores wrote, and pages that
	// device-address accesses found cached (their buffers are in use).
	static constexpr uint64_t WRITE_BITMAP_OFFSET = CACHING_NUMPAGES / 8;
	static constexpr uint64_t USE_BITMAP_OFFSET   = 2 * WRITE_BITMAP_OFFSET;
	static constexpr uint64_t FAULT_BUFFER_SIZE   = 3 * WRITE_BITMAP_OFFSET;

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

	// `guest_fault`: a guest write faulted on the range (see ReadMemory()).
	void                   InvalidateMemory(uint64_t vaddr, uint64_t size, bool guest_fault = false);
	// A guest write faulted on a page the cache protects.
	void                   InvalidateFaultedPage(uint64_t vaddr);
	// A CPU write that already reached guest memory, from a thread that cannot read back:
	// pages the GPU owns stay GPU-owned, the rest become CPU-modified.
	void                   InvalidateUnlessGpuModified(uint64_t vaddr, uint64_t size) noexcept;
	// `guest_fault`: a guest access faulted on the range; the readback also takes the GPU-written
	// pages that follow it (see ReadAheadSize()).
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false,
	                                  bool guest_fault = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	// Caches memory a shader reached through a device address the host did not know beforehand.
	void CacheFaultedMemory(uint64_t vaddr, uint64_t size);
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
	// Copies host data into the stream buffer (on the host copy thread from the execution thread)
	// and returns its offset there.
	[[nodiscard]] uint64_t StreamUpload(const void* data, uint64_t size, uint64_t alignment);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	// Writes destination[i] = source[i % pattern] (dwords, i < count) in guest memory when neither
	// range holds GPU writes; false otherwise (the caller lets the GPU do it).
	[[nodiscard]] bool ReplicateBuffer(uint64_t destination, uint64_t count, uint64_t source,
	                                   uint64_t pattern);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasOnlyGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	// Execution thread: a small command-processor write (WRITE_DATA) to GPU-owned pages without
	// reading the GPU's work back first: the bytes go to the backing store and, in stream order,
	// to the cached buffer. False when the caller must take the faulting path.
	[[nodiscard]] bool WriteAroundGpuWrites(uint64_t vaddr, const void* data, uint64_t size);
	// Lock-free: false when no page of the tracking regions the range touches may be GPU dirty.
	[[nodiscard]] bool MayBeGpuModified(uint64_t vaddr, uint64_t size) const {
		return m_memory_tracker.MayBeGpuModified(vaddr, size);
	}
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
	void               PrepareFaultBuffer() { m_fault_manager.PrepareFaultBuffer(); }
	void               ProcessFaultBuffer();
	// Counts the buffers that shaders reached through device addresses since the last call as
	// used, once the GPU work recorded so far completes: the age collection keeps them.
	void               ProcessUseBuffer();
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
	// Execution thread, after each operation: the GPU writes the operation recorded get the
	// tick of the command buffer that holds them (see DownloadOnReadbackQueue()). True when the
	// operation wrote memory that readbacks keep waiting for: submitting its command buffer now
	// lets the GPU run it before the readback comes, which then waits for that submission only
	// instead of draining everything recorded since.
	[[nodiscard]] bool CommitWriteTicks();
	// Execution thread: reads [vaddr, vaddr + size), taking the bytes the GPU wrote from the
	// host-visible mirror of small ranges the execution thread reads again and again (the headers
	// that the shaders of a ray-tracing BVH build read from the buffers their predecessors write),
	// without a GPU round trip. False when the mirror does not hold the GPU's current bytes yet:
	// the caller reads them back, and the range is mirrored from the next submission on.
	[[nodiscard]] bool TryReadMirroredGpuWrites(uint64_t vaddr, void* data, uint64_t size);
	// The operation being recorded writes memory that readbacks often wait for soon after:
	// submit its command buffer once it is recorded while readbacks keep coming.
	void RequestFlushForReadbacks() noexcept {
		if ((m_device_address_hot != 0 &&
		     m_readback_count - m_device_address_hot < HotReadbackLifetime) ||
		    m_mirror_uses - m_mirror_flush_use < MirrorFlushLifetime) {
			m_flush_requested = true;
		}
	}
	// The operation being recorded accesses memory through device addresses.
	void NoteDeviceAddressUse() noexcept {
		m_device_address_pending = true;
		if (m_device_address_hot != 0 &&
		    m_readback_count - m_device_address_hot < HotReadbackLifetime) {
			m_flush_requested = true;
		}
	}

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
	void CollectStaleBuffers(uint64_t tick);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);
	// Execution thread: the bytes from vaddr a guest fault on [vaddr, vaddr + size) reads back,
	// with the GPU-written pages that follow it whose writes were submitted.
	[[nodiscard]] uint64_t ReadAheadSize(const Buffer& buffer, uint64_t vaddr, uint64_t size) const;
	// ReadMemory() for threads other than the execution thread: the execution thread records and
	// submits the download and keeps working while the faulting thread waits for it.
	void ReadMemoryAsync(uint64_t vaddr, uint64_t size, bool is_write, bool guest_fault);
	// Execution thread: the newest tick of a download in flight over [begin, end), or 0.
	[[nodiscard]] uint64_t InFlightReadbackTick(uint64_t begin, uint64_t end) const;
	// Execution thread, after a readback of [vaddr, vaddr + size) (pages [pages_begin, pages_end))
	// published the bytes downloaded at `tick`: the pages leave GPU ownership unless the GPU wrote
	// them again since, and a guest write marks them CPU-modified.
	void ReleaseReadback(uint64_t vaddr, uint64_t size, uint64_t pages_begin, uint64_t pages_end,
	                     uint64_t tick, bool is_write);
	// Reads GPU-written bytes whose last writes were already submitted: a copy on the readback
	// queue waits for those submissions only, not for the work queued after them.
	[[nodiscard]] bool DownloadOnReadbackQueue(Buffer& buffer, std::span<const vk::BufferCopy> copies,
	                                           uint64_t total_size);
	// The GPU-written ranges of [vaddr, vaddr + size) as copies into a packed staging buffer; they
	// leave the dirty ranges.
	void CollectDownloadCopies(const Buffer& buffer, uint64_t vaddr, uint64_t size,
	                           std::vector<vk::BufferCopy>& copies, uint64_t& total_size);
	// A guest thread's readback through the readback queue (see StartReadbackQueueRead()).
	struct ReadbackQueueRead;
	// Execution thread, for ReadMemoryAsync(): when the last writes of the GPU-written bytes of
	// [vaddr, vaddr + size) were already submitted, submits their copy on the readback queue,
	// waiting for those writes only, and returns true. The faulting thread then waits for the copy
	// in FinishReadbackQueueRead() instead of for everything submitted since.
	[[nodiscard]] bool StartReadbackQueueRead(const Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                          uint64_t pages_begin, uint64_t pages_end,
	                                          ReadbackQueueRead& read);
	// The faulting thread: waits for the copy and publishes the bytes.
	void FinishReadbackQueueRead(ReadbackQueueRead& read);
	// Before a buffer is destroyed: its copies on the readback queue are done. The copies wait for
	// earlier submissions only, so the timeline does not cover them.
	void WaitReadbackQueueCopies(vk::Buffer buffer);
	void NoteWrite(uint64_t vaddr, uint64_t size) {
		// Draws bind the same written ranges again and again.
		if (m_pending_writes.empty() || m_pending_writes.back() != std::pair {vaddr, size}) {
			m_pending_writes.emplace_back(vaddr, size);
			for (const auto& hot: m_readback_hot) {
				if (vaddr < hot.end && hot.begin < vaddr + size) {
					m_flush_requested = true;
					break;
				}
			}
		}
	}
	// A readback of [begin, end) waited for writes still being recorded (`drained`) or for
	// submitted ones; `device_address` when they were device-address writes.
	void NoteReadbackWait(uint64_t begin, uint64_t end, bool device_address, bool drained);
	// Whether to submit for a pending flush request now (see CommitWriteTicks()).
	[[nodiscard]] bool TakeFlushRequest();

	// The tick of the command buffer that last wrote each guest range a cached buffer holds.
	class WriteTicks {
	public:
		void Assign(uint64_t begin, uint64_t end, uint64_t tick);
		// The latest tick over [begin, end); UINT64_MAX when part of it has no tick.
		[[nodiscard]] uint64_t Latest(uint64_t begin, uint64_t end) const;

	private:
		struct Range {
			uint64_t end  = 0;
			uint64_t tick = 0;
		};
		std::map<uint64_t, Range> m_ranges;
	};
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
	// By page, the bytes of the GPU-written ranges (one bit per byte): reads around the GPU's writes
	// ask HasGpuDirtyBytes() a hundred thousand times a frame, mostly on the same few pages. A page's
	// mask is built on its first query in slot page % 128 and follows the ranges as they grow or
	// shrink over it.
	struct DirtyPageMask {
		uint64_t                 page = UINT64_MAX;
		std::array<uint64_t, 64> bits {};
	};
	std::array<DirtyPageMask, 128> m_dirty_masks {};
	// After m_gpu_modified_ranges gained (dirty) or lost [vaddr, vaddr + size).
	void NoteGpuModifiedChange(uint64_t vaddr, uint64_t size, bool dirty);
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 16>                          m_checked_color_metadata;
	RangeSet                                          m_released_ranges;
	bool                                              m_record_released = false;
	// The last buffers CollectStaleBuffers() released ([address, size] pairs, a ring): a device
	// address fault inside one of them means a shader reached memory the age release dropped.
	static constexpr size_t                           StaleRingSize = 32;
	std::array<std::atomic<uint64_t>, StaleRingSize * 2> m_stale_ring {};
	std::atomic<uint32_t>                             m_stale_ring_next {0};
	MemoryTracker                                     m_memory_tracker;
	uint64_t                                           m_gpu_write_generation = 0;
	WriteTicks                                        m_write_ticks;
	std::vector<std::pair<uint64_t, uint64_t>>        m_pending_writes;
	// Downloads for other threads' readbacks whose publication is pending: their pages stay
	// GPU-owned until the readback that recorded them released them (execution thread only).
	struct InFlightReadback {
		uint64_t begin = 0;
		uint64_t end   = 0;
		uint64_t tick  = 0;
	};
	std::vector<InFlightReadback>                     m_inflight_readbacks;
	uint64_t                                          m_device_address_tick    = 0;
	bool                                              m_device_address_pending = false;
	// Pages whose readbacks found the writes they need still being recorded, which drains the
	// GPU: an operation that writes them is submitted at once (see CommitWriteTicks()). An entry
	// lives while readbacks of it keep coming, up to HotReadbackLifetime readbacks without one.
	struct HotReadback {
		uint64_t begin    = 0;
		uint64_t end      = 0;
		uint64_t last_hit = 0;
	};
	static constexpr uint64_t HotReadbackLifetime = 4096;
	static constexpr size_t   MaxHotReadbacks     = 32;
	std::vector<HotReadback>  m_readback_hot;
	uint64_t                  m_readback_count = 0;
	// m_readback_count at the last readback that waited for device-address writes (0: none).
	uint64_t                  m_device_address_hot = 0;
	bool                      m_flush_requested    = false;
	// Requested submissions (see TakeFlushRequest()): at most one per interval, and the tick a
	// request was put off at (0: none put off).
	static constexpr std::chrono::microseconds MinRequestedFlushInterval {1000};
	std::chrono::steady_clock::time_point      m_last_requested_flush {};
	uint64_t                                   m_flush_deferred_tick = 0;
	struct ReadbackQueue {
		vk::CommandPool         pool    = nullptr;
		vk::CommandBuffer       command = nullptr;
		vk::Fence               fence   = nullptr;
		std::unique_ptr<Buffer> staging;
	};
	ReadbackQueue m_readback;
	// Copies on the readback queue that guest threads wait for (see StartReadbackQueueRead()); a
	// slot is busy from its submission until its thread published the bytes.
	struct ReadbackQueueSlot {
		ReadbackQueue     queue;
		vk::Buffer        source = nullptr;
		std::atomic<bool> busy {false};
	};
	std::array<ReadbackQueueSlot, 4> m_readback_slots;
	struct ReadbackQueueRead {
		ReadbackQueueSlot*                 slot = nullptr;
		std::vector<vk::BufferCopy>        copies;
		uint64_t                           buffer_address = 0;
		uint64_t                           total_size     = 0;
		// The submission of the bytes' last writes, and the one current when the copy was made.
		uint64_t                           needed   = 0;
		uint64_t                           recorded = 0;
		std::shared_ptr<std::atomic<bool>> published;
	};
	// The mirror: each line holds a copy of its bytes in a cached buffer, recorded at the end of
	// the command buffer `tick` (0: none since the line's last write) into `slot` of a
	// host-visible buffer. An operation that writes a line clears its tick and queues it for a
	// copy at the end of its command buffer (see RecordMirrorCopies()), so a copy whose tick is
	// at least the line's last write tick holds the GPU's bytes once that tick completed.
	struct MirrorLine {
		uint32_t slot       = 0;
		uint64_t tick       = 0;
		uint64_t dirty_tick = 0;
		uint64_t last_use   = 0;
	};
	static constexpr uint64_t MirrorLineSize      = 64;
	static constexpr uint64_t MirrorMaxRead       = 256;
	static constexpr uint32_t MaxMirrorLines      = 8192;
	// Mirror reads (m_mirror_uses) within which a line or a hit counts as recent.
	static constexpr uint64_t MirrorFlushLifetime = 4096;
	void                      MarkMirrorLinesWritten(uint64_t vaddr, uint64_t size, uint64_t tick);
	void                      AddMirrorLine(uint64_t line);
	void                      RecordMirrorCopies(CommandBuffer& command);
	std::map<uint64_t, MirrorLine> m_mirror_lines;
	std::vector<uint64_t>          m_mirror_dirty;
	std::vector<uint32_t>          m_mirror_free_slots;
	std::unique_ptr<Buffer>        m_mirror;
	uint64_t                       m_mirror_uses      = 0;
	uint64_t                       m_mirror_flush_use = 0;
	StreamBuffer                                      m_staging_buffer;
	struct UploadBatch;
	// SynchronizeBuffersInRanges(): the uploads of one synchronization, recorded together
	// behind one pair of barriers (see FlushUploadBatch()).
	bool                                              m_upload_batching   = false;
	uint64_t                                          m_upload_batch_tick = 0;
	struct UploadBatchBuffer {
		vk::Buffer        buffer;
		vk::DeviceAddress address = 0;
		uint32_t          count   = 0;
	};
	std::vector<UploadBatchBuffer>                    m_upload_batch_buffers;
	std::vector<vk::BufferCopy>                       m_upload_batch_copies;
	// The batch's copies as one dispatch (buffer_scatter.comp), created at the first batch that
	// copies to several buffers.
	vk::PipelineLayout                                m_scatter_layout   = nullptr;
	vk::Pipeline                                      m_scatter_pipeline = nullptr;
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
	// The last synchronization of a range bound as written (see ObtainBuffer()).
	struct WrittenMemo {
		uint64_t vaddr        = 0;
		uint64_t size         = 0;
		uint64_t buffer_epoch = 0;
		uint64_t cpu_epoch    = 0;
		uint64_t gpu_epoch    = 0;
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
	static constexpr size_t                                    WrittenMemoSlots = 1024;
	std::unique_ptr<std::array<WrittenMemo, WrittenMemoSlots>> m_written_memo =
	    std::make_unique<std::array<WrittenMemo, WrittenMemoSlots>>();
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
