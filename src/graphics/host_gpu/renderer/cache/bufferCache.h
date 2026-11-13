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
#include "graphics/host_gpu/sparseBinding.h"

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
	// Shaders mark the regions they reach through device addresses, after the fault bitmap.
	static constexpr uint32_t BDA_USE_REGIONBITS = 21;
	static constexpr uint64_t BDA_USE_REGIONSIZE = uint64_t {1} << BDA_USE_REGIONBITS;
	static constexpr uint64_t BDA_USE_NUMREGIONS =
	    (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> BDA_USE_REGIONBITS;
	static constexpr uint64_t FAULT_BITMAP_SIZE   = CACHING_NUMPAGES / 8;
	static constexpr uint64_t BDA_USE_BITMAP_SIZE = BDA_USE_NUMREGIONS / 8;
	static constexpr uint64_t FAULT_BUFFER_SIZE   = FAULT_BITMAP_SIZE + BDA_USE_BITMAP_SIZE;
	static_assert(BDA_USE_REGIONBITS >= CACHING_PAGEBITS &&
	              LOWER_ADDRESS_SIZE % BDA_USE_REGIONSIZE == 0);
	static_assert(FAULT_BITMAP_SIZE % 16 == 0 && BDA_USE_BITMAP_SIZE % 4 == 0);

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
	// A command-processor store; GPU-written memory takes it in queue order, not by a download.
	void WriteMemory(uint64_t vaddr, const void* data, uint64_t size);
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
	void               ProcessFaultBuffer();
	// Uploads the CPU-dirty bytes of the queued ranges `mapped` covers behind one barrier pair.
	void SynchronizeBda(const RangeSet& mapped);
	// A DMA shader used the range: a use mark or a fault reported it.
	void MarkGpuUse(uint64_t vaddr, uint64_t size);
	// Queue after the range's state change becomes visible, or a drain could consume it first.
	void               QueueBdaSync(uint64_t vaddr, uint64_t size);
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

	// Copies [previous upload's end, end) go to `buffer`.
	struct BufferUpload {
		Buffer* buffer;
		size_t  end;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	// A sparse table takes the pages its written entries need.
	void WritePageTable(uint64_t offset, std::span<const vk::DeviceAddress> entries);
	void BindPageTable(uint64_t offset, uint64_t size);
	// Points the page table at the whole BDA pages of [begin, end), which `buffer` holds.
	void WritePageEntries(const Buffer& buffer, uint64_t begin, uint64_t end);
	void TouchBuffer(const Buffer& buffer, uint64_t vaddr, uint64_t size);
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
	void UploadCopies(std::span<const BufferUpload> uploads, std::span<vk::BufferCopy> copies,
	                  uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);

	// With aliased sparse buffers, guest buffers are not joined. Each region of guest memory has
	// a window two regions long, so a range lies whole in the window of its first byte, at a
	// fixed offset, and only the pages in use take memory.
	struct WindowPage {
		SparsePage memory;
		size_t     lru_id = 0;
		// Not zero: unmapped by that release, and bound until its unbind runs.
		uint64_t release = 0;
		// Bound into the window of the region below as well.
		bool lower = false;
	};
	struct PageRelease {
		uint64_t address = 0;
		uint64_t release = 0;
	};
	struct FoundRange {
		uint64_t vaddr = 0;
		uint64_t size  = 0;
		BufferId id;
	};
	[[nodiscard]] bool     Windowed() const noexcept { return m_page_size != 0; }
	[[nodiscard]] uint64_t Region(uint64_t address) const noexcept {
		return address >> m_region_bits;
	}
	[[nodiscard]] BufferId EnsureWindow(uint64_t region);
	void                   CommitPages(uint64_t begin, uint64_t end);
	void                   AllocatePages(std::span<SparsePage> pages);
	void                   TouchPages(uint64_t vaddr, uint64_t size);
	// func(begin, end) for each run of neighbouring pages in one region.
	template <typename Func>
	void ForEachPageRun(std::span<const uint64_t> pages, Func&& func) const;
	// func(window, address, size) for the resident parts of a range, region by region.
	template <typename Func>
	void ForEachResidentPiece(uint64_t vaddr, uint64_t size, Func&& func);
	// Bytes the buffer holds from `address`, up to `size`: a window holds its resident pages.
	[[nodiscard]] uint64_t HeldBytes(const Buffer& buffer, uint64_t address, uint64_t size) const;
	void                   DownloadWindows(uint64_t vaddr, uint64_t size);
	// For sorted resident pages the GPU holds no newer bytes of. They stay bound for the work
	// recorded so far; the releases unbind them once it has run.
	[[nodiscard]] std::vector<PageRelease> ReleasePages(std::span<const uint64_t> pages);
	void                                   UnbindReleased(std::span<const PageRelease> releases);
	// Makes room in the device memory already held; false when idle pages cannot.
	[[nodiscard]] bool ReclaimPages(uint64_t bytes);
	void               WriteBackPages(std::span<const uint64_t> pages);
	void               CollectPages(uint64_t before, bool aggressive);
	void               RetireIdleWindows();

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	// The pages of a sparse table by position; empty for a table with memory of its own.
	std::vector<SparsePage>                           m_table_pages;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	// Zero while guest buffers are joined.
	uint64_t                                          m_page_size   = 0;
	uint32_t                                          m_region_bits = 0;
	// Windows by region, and their pages by address: resident, or released and still bound.
	std::map<uint64_t, BufferId>                      m_windows;
	std::unordered_map<uint64_t, WindowPage>          m_pages;
	std::unordered_map<uint64_t, uint64_t>            m_region_pages;
	RangeSet                                          m_resident;
	uint64_t                                          m_releases = 0;
	Common::LeastRecentlyUsedCache<uint64_t, uint64_t> m_page_lru;
	// Ranges DMA shaders were reported to use.
	RangeSet                                          m_dma_accessed;
	// Ranges found in the tick m_found_tick, by hash: no page a tick used leaves during it.
	std::array<FoundRange, 256>                       m_found;
	uint64_t                                          m_found_tick = 0;
	// Ranges already touched in the garbage-collection tick m_touched_tick.
	RangeSet                                          m_touched;
	uint64_t                                          m_touched_tick = 0;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 8>                           m_checked_color_metadata;
	MemoryTracker                                     m_memory_tracker;
	std::mutex                                        m_bda_sync_mutex;
	RangeSet                                          m_bda_sync;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
