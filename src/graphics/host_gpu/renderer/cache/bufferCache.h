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
class RenderContext;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

// The workarounds of recompiled shaders that mark the fault buffer when they change what the
// shader computes (BufferCache::HACK_MARKS_OFFSET).
enum class HackMark : uint32_t {
	Count,
};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	// The entry after the last page holds the device address of the native ray tracing context
	// (see NativeRayTracing::ContextAddress()), 0 without one.
	static constexpr uint64_t NATIVE_RT_CONTEXT_PAGE = CACHING_NUMPAGES;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    (CACHING_NUMPAGES + 1) * sizeof(vk::DeviceAddress);
	// The fault buffer holds three bitmaps of CACHING_NUMPAGES bits: pages that device-address
	// accesses found without a cached buffer, pages that V#-table stores wrote, and pages that
	// device-address accesses found cached (their buffers are in use). Then the hack marks.
	static constexpr uint64_t WRITE_BITMAP_OFFSET = CACHING_NUMPAGES / 8;
	static constexpr uint64_t USE_BITMAP_OFFSET   = 2 * WRITE_BITMAP_OFFSET;
	// A shader whose workaround (HackMark) changed what it computed marks the entry of its hash:
	// it ORs the two dwords of the hash and their complements (a shader alone in its entry leaves
	// each dword the complement of the next but one) and adds 1 to the count of the workaround.
	// FaultManager reads them back.
	static constexpr uint64_t HACK_MARKS_OFFSET   = 3 * WRITE_BITMAP_OFFSET;
	static constexpr uint32_t HACK_MARK_ENTRIES   = 256;
	static constexpr uint32_t HACK_MARK_DWORDS    = 8;
	static constexpr uint32_t HACK_MARK_COUNTS    = 4; // dword of the count of HackMark 0
	// With --rt-stats, the rays that the native ray tracing patch of the shader sent back to the
	// guest traversal (added, see EmitRayQueryInit): not a workaround, reported by NativeRayTracing.
	static constexpr uint32_t HACK_MARK_GUEST_RAYS = HACK_MARK_COUNTS + static_cast<uint32_t>(HackMark::Count);
	static constexpr uint64_t HACK_MARKS_SIZE     = uint64_t {HACK_MARK_ENTRIES} * HACK_MARK_DWORDS * 4;
	static constexpr uint64_t FAULT_BUFFER_SIZE   = HACK_MARKS_OFFSET + HACK_MARKS_SIZE;
	static_assert(HACK_MARK_GUEST_RAYS < HACK_MARK_DWORDS);

	static constexpr uint32_t HackMarkEntry(uint64_t shader_hash) {
		return (static_cast<uint32_t>(shader_hash) ^ static_cast<uint32_t>(shader_hash >> 32u)) %
		       HACK_MARK_ENTRIES;
	}

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
	            TextureCache& texture_cache, RenderContext* resources = nullptr);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	// `guest_fault`: a guest write faulted on the range (see ReadMemory()).
	void                   InvalidateMemory(uint64_t vaddr, uint64_t size, bool guest_fault = false);
	// Before a host write of all of [vaddr, vaddr + size) (any thread): as InvalidateMemory, but the GPU-written bytes the
	// write replaces are dropped, not read back (only those of its first and last page outside it are).
	void                   InvalidateMemoryForWrite(uint64_t vaddr, uint64_t size);
	// Moves when a buffer is registered or unregistered.
	[[nodiscard]] uint64_t RegistrationEpoch() const { return m_registration_epoch.load(std::memory_order_acquire); }
	// The registration epoch of the last change of a buffer covering the tracker region of `vaddr`: a V# resolved before
	// it, whose range lies in regions none of which changed since, still finds its buffer.
	[[nodiscard]] uint64_t RegionRegistrationEpoch(uint64_t vaddr) const {
		const auto region = vaddr / TRACKER_REGION_SIZE;
		return region < TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE ? m_region_registrations[region].load(std::memory_order_acquire)
		                                                           : 0;
	}
	// (This tree applies no write protection later than the upload that asks for it.)
	[[nodiscard]] bool DeferredProtectionPending(uint64_t) const { return false; }
	// A guest write faulted on a page the cache protects.
	void                   InvalidateFaultedPage(uint64_t vaddr);
	// RenderContext::UnmapMemory, after InvalidateMemory: the buffers the unmapped range holds whole go (nothing the guest
	// maps reaches them any more); the bytes of the buffers let go.
	uint64_t               DeleteUnmappedBuffers(uint64_t vaddr, uint64_t size);
	// The game's free blocks (its page allocator's, pages kept mapped) over [begin, end) added to `blocks` as (address,
	// size): false when not known (Loader::DemonsSoulsPageFree::FreeBlocksIn).
	using FreeBlocksQuery = bool (*)(uint64_t begin, uint64_t end, std::vector<std::pair<uint64_t, uint64_t>>* blocks);
	// What DropFreedRange's free block lists did: lists read, bytes cut off only thanks to them.
	struct FreeBlockSplits {
		uint64_t lists = 0, cut = 0;
	};
	// A buffer the range holds in part, mapped in no more than half of it now, is cut to its mapped parts (new buffers with
	// its bytes copied on the GPU); the bytes cut off. With `free_blocks` (a freed range, DropFreedRange), the game's free
	// blocks over it count as unmapped too.
	uint64_t               SplitUnmappedBuffers(uint64_t vaddr, uint64_t size, FreeBlocksQuery free_blocks = nullptr,
	                                            FreeBlockSplits* splits = nullptr);
	// The game freed [vaddr, vaddr + size) and keeps it mapped: its GPU-written bytes read back (`read_back`) or dropped,
	// then its buffers go as an unmapped range's do; the bytes of buffers let go. Nothing when no buffer is registered there.
	uint64_t               DropFreedRange(uint64_t vaddr, uint64_t size, bool read_back, FreeBlocksQuery free_blocks,
	                                      FreeBlockSplits* splits);
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
	// For host work that reads guest memory through device addresses (the BDA page table) instead
	// of through a binding: caches [vaddr, vaddr + size) in a buffer the page table reaches and
	// uploads the CPU writes that buffer does not hold yet. ObtainBuffer() may answer a small read
	// of CPU-written memory from the stream buffer, which the page table does not reach.
	void PrepareDeviceAddressRead(uint64_t vaddr, uint64_t size);
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
	// Execution thread: whether the operation about to execute streams its guest data through the
	// host copy queue's ring (see GpuOperation::stream_in_place).
	void SetStreamInPlace(bool in_place) noexcept { m_stream_in_place = in_place; }
	// Copies host data into the stream buffer (on the host copy thread from the execution thread)
	// and returns its offset there.
	[[nodiscard]] uint64_t StreamUpload(const void* data, uint64_t size, uint64_t alignment);
	// A copy of table blocks in a ring whose buffers have device addresses (table draws read their slots and the
	// buffers' addresses through the address pushed with the user data); valid for the command buffer being recorded.
	[[nodiscard]] vk::DeviceAddress TableUpload(const void* data, uint64_t size);
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
	// Native XPR records (RenderExecutor::NativeXpr*) bind cached buffers across frames.
	// The buffer `id` names while it is registered, else null.
	[[nodiscard]] Buffer* GetBufferIfLive(BufferId id) {
		return IsBufferInvalid(id) ? nullptr : &m_slot_buffers[id];
	}
	// Whether `id` is still the registered buffer with `handle` over the range; touches it for
	// the garbage collector as a use would.
	[[nodiscard]] bool TouchLiveBuffer(BufferId id, vk::Buffer handle, uint64_t vaddr, uint64_t size);
	// Uploads the CPU writes of the range into the buffer that holds it (ObtainBuffer() would
	// stream a small CPU-dirty range instead): afterwards the range is CPU clean, and a later CPU
	// write marks it dirty again.
	void EnsureBufferContents(uint64_t vaddr, uint64_t size);
	// Advances whenever a buffer is registered or unregistered.
	[[nodiscard]] uint64_t BufferSetEpoch() const noexcept { return m_buffer_set_epoch; }
	// (The BDA page table never grows here.)
	[[nodiscard]] static uint64_t BdaTableGeneration() noexcept { return 0; }
	[[nodiscard]] StreamBuffer& GetTableUploadBuffer();
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
	// What SynchronizeBuffersInSet() left of a set: per tracking region, its CPU-dirty epoch and
	// its count of forced synchronizations (+ 1) when a pass over the set last visited it, and
	// the number of the last pass over the set.
	struct SetRegions {
		std::vector<std::pair<uint64_t, uint64_t>> regions;
		uint32_t                                   pass = 0;
	};
	// Uploads the pending CPU writes of the buffers within `set`, a set that stays the same from
	// call to call (`regions` starts empty for a new set). A pass of the device-address
	// synchronization as SynchronizeBuffersInRanges(), over the set only: visits the tracking
	// regions with a buffer whose pages became CPU dirty, or that were forced, since the pass
	// that last visited them, or where a pass left pages CPU dirty since: that pass left no other
	// pending write under a buffer in their part of the set.
	void SynchronizeBuffersInSet(const RangeSet& set, SetRegions& regions);
	// How often a synchronization uploaded CPU writes of the range since it is watched (the first
	// call for a range starts to watch it; a few ranges at a time). With IsRegionCpuModified(),
	// tells whether the guest's CPU may have written the range: a write makes a page CPU dirty, and
	// the page stays so until a synchronization uploads it.
	[[nodiscard]] uint64_t WatchUploads(uint64_t vaddr, uint64_t size);
	void FlushUploadBatch();
	// FlushUploadBatch() as one dispatch; false when the batch does not fit it.
	bool ScatterUploadBatch(const CommandRecorder& native);
	// Advances whenever a buffer is registered: a new buffer may cover CPU-dirty pages that no
	// earlier synchronization uploaded.
	[[nodiscard]] uint64_t RegisterEpoch() const { return m_register_epoch; }
	// The last SynchronizeBuffersInRanges() left pages CPU dirty that it uploads every time (see
	// RegionManager::ConsumeCpuDirtyRangesKeepingBusy()): the next one must run even if no page
	// became CPU dirty since.
	[[nodiscard]] bool HasKeptPages() const noexcept { return m_kept_pages; }
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
	// chenxiao07's BufferCache::CpuModificationEpoch: the sum of the tracker regions' RegionManager::CpuModificationEpoch
	// over the range (up to 64 regions); 0 when a region is untracked or the range is larger: nothing provable.
	[[nodiscard]] uint64_t CpuModificationEpoch(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || !GuestRange {vaddr, size}.Valid()) return 0;
		constexpr uint64_t MaxRegions = 64;
		const uint64_t     first      = vaddr / TRACKER_REGION_SIZE;
		const uint64_t     last       = (vaddr + size - 1) / TRACKER_REGION_SIZE;
		if (last - first >= MaxRegions) return 0;
		uint64_t sum = 0;
		for (uint64_t region = first; region <= last; ++region) {
			const uint64_t begin = std::max(vaddr, region * TRACKER_REGION_SIZE);
			const uint64_t end   = std::min(vaddr + size, (region + 1) * TRACKER_REGION_SIZE);
			const uint64_t epoch = m_memory_tracker.CpuModificationEpoch(begin, end - begin);
			if (epoch == 0) return 0;
			sum += epoch;
		}
		return sum;
	}
	// The emulator wrote the range in guest memory (any thread): see MemoryTracker::NoteHostWrite.
	void NoteHostWrite(uint64_t vaddr, uint64_t size) { m_memory_tracker.NoteHostWrite(vaddr, size); }
	// The tracker's answer on CPU-written bytes the range may hold (what a cache of the bytes may keep).
	[[nodiscard]] bool IsRegionCpuDirty(uint64_t vaddr, uint64_t size) { return m_memory_tracker.IsRegionCpuModified(vaddr, size); }
	// The registered buffer over the whole range, else none (FindBuffer makes one).
	[[nodiscard]] BufferId RegisteredBuffer(uint64_t vaddr, uint64_t size) const {
		if (vaddr == 0 || !GuestRange {vaddr, size}.Valid()) return {};
		const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
		if (owner == nullptr || !*owner) return {};
		const auto* buffer = m_slot_buffers.try_get(*owner);
		return buffer != nullptr && !buffer->is_deleted && buffer->IsInBounds(vaddr, size) ? *owner : BufferId {};
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
	// GPU thread: InvalidateMemoryForWrite's work where GPU-written bytes are in the range.
	void DiscardGpuWrites(uint64_t vaddr, uint64_t size);
	void CollectStaleBuffers(uint64_t tick);
	// Whether no image is registered on the page where `vaddr` starts, so that no image can start at it
	// (FindImageFromRange() finds only images starting at the address).
	[[nodiscard]] bool NoImageStartsAt(uint64_t vaddr) const noexcept;
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	// Records (or adds to the upload batch) the copies UploadCopies() staged in `source`.
	void RecordBufferUpload(Buffer& buffer, std::span<const vk::BufferCopy> copies,
	                        vk::Buffer source);
	// SynchronizeBuffer() of read-only uses of ranges[i] of buffers[i], ordered by address.
	void               SynchronizeBuffers(std::span<Buffer* const>                       buffers,
	                                      std::span<const std::pair<uint64_t, uint64_t>> ranges);
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
	// With `followers`, the copy also takes the pages of m_guest_read_pages whose writes are done.
	[[nodiscard]] bool StartReadbackQueueRead(const Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                          uint64_t pages_begin, uint64_t pages_end,
	                                          ReadbackQueueRead& read, bool followers);
	// The pages guest threads read back by fault lately (execution thread). A guest reads the same
	// GPU results back frame after frame, a fault and two commands of the execution thread a page
	// (Demon's Souls: ~60 destinations of its GPU copies a frame, one after another from one
	// thread, which the frame waits for): a readback on the readback queue downloads the others
	// along whose GPU writes are done, exactly as their own faults would, and they fault no more.
	// A page leaves the list GuestReadPageLifetime guest readbacks after its last fault: if the
	// guest still reads it, it faults once and is back.
	struct GuestReadPage {
		uint64_t page = 0;
		uint64_t read = 0;
	};
	static constexpr size_t   MaxGuestReadPages     = 512;
	static constexpr uint64_t GuestReadPageLifetime = 16384;
	std::vector<GuestReadPage> m_guest_read_pages;
	uint64_t                   m_guest_reads = 0;
	void                       NoteGuestReadPages(uint64_t pages_begin, uint64_t pages_end);
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
	RenderContext*                                    m_resources = nullptr;
	std::atomic<uint64_t>                             m_registration_epoch {1};
	// Per tracker region, see RegionRegistrationEpoch.
	std::unique_ptr<std::atomic<uint64_t>[]> m_region_registrations =
	    std::make_unique<std::atomic<uint64_t>[]>(TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE);
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
		// The copy reads other buffers too (ReadbackQueueRead::followers).
		bool              many = false;
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
		// The other pages the copy downloads (see m_guest_read_pages).
		struct Follower {
			uint64_t                    page           = 0;
			uint64_t                    buffer_address = 0;
			vk::Buffer                  source         = nullptr;
			std::vector<vk::BufferCopy> copies;
		};
		std::vector<Follower> followers;
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
	std::unique_ptr<StreamBuffer> m_table_upload;
	// The buffer a read-only binding of a range found, while no buffer was registered or
	// unregistered and no page of the range became CPU dirty since (see ObtainBuffer()).
	struct ObtainMemo {
		uint64_t vaddr        = 0;
		uint64_t size         = 0;
		uint64_t buffer_epoch = 0;
		uint64_t cpu_epoch    = 0;
		uint64_t offset       = 0;
		BufferId id;
		// A texel buffer read: also needs no image to start on the range's first page (see
		// NoImageStartsAt()).
		bool texel = false;
	};
	// What the read-only bindings of ranges within one caching page found: its buffer and the
	// tracker pages of it (one bit each) that had nothing to upload, while no buffer was registered
	// or unregistered and no page of its tracking region became CPU dirty since (see
	// ObtainBuffer()).
	struct PageMemo {
		uint64_t page           = UINT64_MAX;
		uint64_t buffer_epoch   = 0;
		uint64_t cpu_epoch      = 0;
		uint64_t buffer_address = 0;
		BufferId id;
		uint32_t clean = 0;
	};
	static constexpr uint32_t AllPageBits = (1u << (CACHING_PAGESIZE / TRACKER_PAGE_SIZE)) - 1u;
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
	bool                                                     m_stream_in_place = false;
	std::unique_ptr<std::array<StreamMemo, StreamMemoSlots>> m_stream_memo =
	    std::make_unique<std::array<StreamMemo, StreamMemoSlots>>();
	static constexpr size_t                                  ObtainMemoSlots = 4096;
	std::unique_ptr<std::array<ObtainMemo, ObtainMemoSlots>> m_obtain_memo =
	    std::make_unique<std::array<ObtainMemo, ObtainMemoSlots>>();
	static constexpr size_t                              PageMemoSlots = 4096;
	std::unique_ptr<std::array<PageMemo, PageMemoSlots>> m_page_memo =
	    std::make_unique<std::array<PageMemo, PageMemoSlots>>();
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
	// How often each region was forced (see SynchronizeBuffersInSet()).
	std::vector<uint64_t>                      m_region_forces;
	// WatchUploads(): the ranges ([begin, end)) and their uploads.
	struct UploadWatch {
		uint64_t begin   = 0;
		uint64_t end     = 0;
		uint64_t uploads = 0;
	};
	std::array<UploadWatch, 4> m_upload_watches {};
	uint32_t                   m_upload_watch_next = 0;
	void NoteUpload(uint64_t address, uint64_t bytes) noexcept {
		for (auto& watch: m_upload_watches) {
			watch.uploads += address < watch.end && address + bytes > watch.begin ? 1 : 0;
		}
	}
	std::vector<uint64_t>                      m_synced_region_epochs;
	std::vector<std::pair<uint64_t, uint64_t>> m_sync_regions;
	// The buffers of a CPU-dirty span (see SynchronizeBuffersInRange()) and their copies.
	std::vector<Buffer*>                       m_span_buffers;
	std::vector<std::pair<uint64_t, uint64_t>> m_span_ranges;
	std::vector<std::vector<vk::BufferCopy>>   m_span_copies;
	std::vector<uint64_t>                      m_span_sizes;
	// The SynchronizeBuffersInRanges() calls so far, the number of the one running (0: none) and
	// whether the last one left pages CPU dirty.
	uint32_t m_device_syncs     = 0;
	uint32_t m_device_sync_pass = 0;
	// The last pass over everything (SynchronizeBuffersInRanges()).
	uint32_t m_device_full_pass = 0;
	bool     m_kept_pages       = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
