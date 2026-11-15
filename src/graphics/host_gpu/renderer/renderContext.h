#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/indirectWriteTables.h"
#include "graphics/host_gpu/renderer/lodStats.h"
#include "graphics/host_gpu/renderer/nativeRayTracing.h"
#include "graphics/host_gpu/renderer/occlusionQueries.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"
#include "graphics/host_gpu/renderer/pipeline/imageHeap.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/rayTracing.h"
#include "kernel/eventQueue.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <shared_mutex>
#include <span>
#include <deque>
#include <utility>
#include <vector>

namespace Libs::VideoOut {
class VideoOutDriver;
}

namespace Libs::Graphics {

struct ShaderBufferResource;

class GuestGpu;

class RenderContext {
public:
	explicit RenderContext(GraphicContext&              graphics,
	                       const std::filesystem::path& shader_store = ShaderStorePath());
	~RenderContext();
	KYTY_CLASS_NO_COPY(RenderContext);

	[[nodiscard]] GraphicContext&           GetGraphics() const noexcept { return m_graphics; }
	void                                    InitializeGpu(VideoOut::VideoOutDriver* video_out);
	void                                    ShutdownGpu();
	[[nodiscard]] GuestGpu&                 GetGpu() const;
	[[nodiscard]] VideoOut::VideoOutDriver& GetVideoOut() const;

	Common::Mutex&      GetMutex() { return m_mutex; }
	CommandScheduler&   GetCommandScheduler() { return m_command_scheduler; }
	PipelineCache&      GetPipelineCache() { return m_pipeline_cache; }
	DescriptorHeap&     GetDescriptorHeap() { return m_descriptor_heap; }
	ImageHeap&          GetImageHeap() { return m_image_heap; }
	SamplerCache&       GetSamplerCache() { return m_sampler_cache; }
	BufferCache&        GetBufferCache() { return m_buffer_cache; }
	TextureCache&       GetTextureCache() { return m_texture_cache; }
	IndirectWriteTables& GetIndirectWriteTables() { return m_indirect_write_tables; }
	NativeRayTracing&   GetNativeRayTracing() { return m_native_ray_tracing; }
	RenderExecutor&     GetRenderExecutor() { return m_render_executor; }
	OcclusionQueries&   GetOcclusionQueries() { return m_occlusion_queries; }
	RayTracing*         GetRayTracing() { return m_ray_tracing.get(); } // With ray queries only.
	LodStatsCounter&    GetLodStats() { return m_lod_stats; }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	// Before the emulated GPU writes guest memory (labels, WRITE_DATA): resolves a tracked page
	// as its write fault would, without raising the exception.
	void PrepareGpuWrite(const void* destination, uint64_t size) noexcept;
	// A completion's store of guest memory (labels, query results). A completion cannot read
	// memory back and later GPU work may own the page again at any moment, so the bytes go
	// through the backing store, which never faults; the caches then treat it as a CPU write,
	// except on pages the GPU owns.
	void WriteFromCompletion(void* destination, const void* data, uint64_t size) noexcept;
	// WriteFromCompletion() of values[i] at destination + i * stride (8-byte aligned qwords),
	// values[0] last.
	void WriteQwordsFromCompletion(void* destination, const uint64_t* values, uint32_t count,
	                               uint64_t stride) noexcept;
	// Execution thread, before each operation (see PrepareBda()).
	void NoteOperation(uint64_t submit_id, bool after_wait) noexcept {
		if (submit_id != m_bda_submit || after_wait) {
			m_bda_submit = submit_id;
			m_bda_generation++;
		}
	}
	// A small command-processor write that GPU-owned pages would turn into a readback of all
	// queued GPU work (see BufferCache::WriteAroundGpuWrites()); false when the caller must
	// PrepareGpuWrite() and store the bytes itself.
	[[nodiscard]] bool WriteAroundGpuWrites(void* destination, const void* data, uint64_t size);
	[[nodiscard]] bool InvalidateMemory(uint64_t vaddr, uint64_t size);
	// As InvalidateMemory before a host write of all of the range: the GPU-written bytes it replaces are dropped, not read
	// back (BufferCache::InvalidateMemoryForWrite).
	[[nodiscard]] bool InvalidateMemoryForWrite(uint64_t vaddr, uint64_t size);
	// The mapped parts of [vaddr, vaddr + size), added to `out`.
	void               MappedRanges(uint64_t vaddr, uint64_t size, RangeSet& out) const;
	// The game freed the range and keeps it mapped (LibKernel::Memory::NoteGameFree): queued for the GPU thread, whose
	// buffers it holds go as for an unmap (BufferCache::DropFreedRange). KYTY_ENGINE_FREE_BUFFERS=0: nothing; =2: their
	// GPU-written bytes dropped, not read back.
	void               FreedByGame(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	// Advances whenever a range is mapped or unmapped.
	[[nodiscard]] uint64_t MappedRangesVersion() const noexcept;
	// MappedRangesVersion without the lock (the scout and the table draws' ticket checks ask at every draw): written
	// only under m_mapped_ranges_mutex, with it; readers compare it for change only.
	[[nodiscard]] uint64_t MappingEpoch() const noexcept { return m_mapping_epoch.load(std::memory_order_acquire); }
	// PageManager's read-watcher filter (chenxiao07's GpuResourceManager names): a hint, see PageManager::HasReadWatchers.
	[[nodiscard]] bool HasReadWatchers(uint64_t vaddr, uint64_t size) const noexcept {
		return m_page_manager.HasReadWatchers(vaddr, size);
	}
	[[nodiscard]] const std::atomic<uint64_t>* ReadGranules() const noexcept { return m_page_manager.ReadGranules(); }
	// PageManager::WatchedSpan.
	[[nodiscard]] bool WatchedSpan(uint64_t vaddr, uint64_t size, uint64_t* first, uint64_t* end) const noexcept {
		return m_page_manager.WatchedSpan(vaddr, size, first, end);
	}
	// Execution thread: the guest frames (suspend points) executed so far.
	[[nodiscard]] uint64_t FrameNumber() const noexcept { return m_frame_number; }
	// Whether part of the range was unmapped after MappedRangesVersion() was `version` (true
	// when the log of unmaps no longer reaches back that far).
	[[nodiscard]] bool UnmappedSince(uint64_t version, uint64_t vaddr, uint64_t size) const;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	void               CacheDmaBases(const ShaderStageRuntime& runtime);
	// Caches the memory from the base of each buffer the program sizes from a header (see
	// ShaderInfo::SelfSizedBuffer); returns whether the program stores to one.
	bool               CacheSelfSizedBuffers(const ShaderStageRuntime& runtime);
	// Uploads the CPU writes of the guest memory that the device-address reads of the stages can
	// reach (see ShaderInfo::dma_windows); false when a stage's reach is not known, and nothing
	// was uploaded.
	[[nodiscard]] bool SynchronizeDmaFootprint(std::span<const ShaderStageRuntime* const> stages);
	// may_write: a shader of the operation may store through device addresses. full_sync: upload
	// the CPU writes of all cached memory first (SynchronizeDmaFootprint() did not cover the
	// operation).
	void               PrepareBda(bool may_write, bool full_sync = true);
	void               RunGarbageCollector();
	// In operation order, when a native build wrote a guest TLAS (CommandProcessor::EmitHleTlas()):
	// `size` bytes at `header`, its instances referencing the BLASes `blases` (sorted). The
	// dispatches that trace it synchronize that memory instead of everything (AddRayTracingReach()).
	void NoteHleTlas(uint64_t header, uint64_t size, std::vector<uint64_t> blases);
	// In operation order, before passes of the game build the TLAS at `header` themselves (0: a
	// TLAS not known yet, every one is forgotten).
	void ForgetHleTlas(uint64_t header);
	// In operation order, when sce::Psr captures the memory it builds or decompresses a BVH into:
	// `size` bytes at `address` (CommandProcessor::PsrBlasDispatch()).
	void NoteGuestBvhMemory(uint64_t address, uint64_t size);

	void AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void TriggerInterrupt(int event_id, uint32_t context_id);

private:
	// CacheDmaBases() for the guest TLAS of a program (see NativeRayTracing::FindGuestTlasSource()),
	// with m_mapped_ranges_mutex held.
	void CacheTlasMemory(const ShaderStageRuntime& runtime);
	// SynchronizeDmaFootprint() once the reach of the stages is to be enumerated.
	[[nodiscard]] bool SynchronizeDmaReach(std::span<const ShaderStageRuntime* const> stages);
	// SynchronizeDmaFootprint(): adds the spans a program that works on a table of records reaches
	// (ShaderInfo::dma_record_reach), read from the table as the guest's CPU wrote it. False when
	// the host cannot tell them; the caller holds m_mapped_ranges_mutex.
	[[nodiscard]] bool AddRecordReach(const ShaderStageRuntime& runtime);
	// SynchronizeDmaFootprint(): adds what a program that traces rays natively reaches beyond its
	// windows and tables (ShaderInfo::ray_query_reach): the TLAS the host built, the BLASes of its
	// instances, the BLASes the preparation of the dispatch may walk, and the buffers of its alpha
	// tests. False when the host cannot tell them (the TLAS is not one it built, the extent of a
	// BLAS is not known); the caller holds m_mapped_ranges_mutex.
	[[nodiscard]] bool AddRayTracingReach(const ShaderStageRuntime& runtime);
	// The V# of a buffer table of ShaderInfo::ray_query_reach.
	[[nodiscard]] bool RayQueryTableVsharp(const ShaderStageRuntime& runtime, uint32_t table,
	                                       ShaderBufferResource& vsharp);
	// The address of the header of the guest TLAS a program traces, and where the program reads
	// it from when that is guest memory (else 0); false when the host cannot read it.
	[[nodiscard]] bool GuestTlasHeader(const ShaderStageRuntime& runtime, const NativeRayTracing::TlasSource& source,
	                                   uint64_t& header, uint64_t& header_at);
	// The size of the memory of the guest BLAS at `blas` (0 when it is not mapped); false when it
	// is not known: the GPU wrote its header, in memory the library did not capture.
	[[nodiscard]] bool GuestBlasMemory(uint64_t blas, uint64_t& size);
	// AddRayTracingReach(): the buffer table of an alpha test, and the buffers its V#s describe
	// (m_dma_sets). False when the host cannot read the table.
	[[nodiscard]] bool AddBufferTable(const ShaderBufferResource& table);

	struct InterruptEqRegistration {
		LibKernel::EventQueue::KernelEqueue eq       = LibKernel::EventQueue::KERNEL_EQUEUE_INVALID;
		int                                 event_id = 0;
	};

	// Waits for the image write-backs over the range's pages; returns whether any overlapped.
	bool WaitWriteBacks(uint64_t vaddr, uint64_t size);

	GraphicContext&           m_graphics;
	Common::Mutex             m_mutex;
	RenderExecutor            m_render_executor;
	CommandScheduler          m_command_scheduler;
	DescriptorHeap            m_descriptor_heap;
	PipelineCache             m_pipeline_cache;
	SamplerCache              m_sampler_cache;
	PageManager               m_page_manager;
	BufferCache               m_buffer_cache;
	TextureCache              m_texture_cache;
	ImageHeap                 m_image_heap;
	IndirectWriteTables       m_indirect_write_tables {*this};
	OcclusionQueries          m_occlusion_queries {*this};
	LodStatsCounter           m_lod_stats {*this};
	NativeRayTracing          m_native_ray_tracing {m_graphics, m_command_scheduler, m_buffer_cache};
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	uint64_t                  m_mapped_ranges_version = 0;
	std::atomic<uint64_t>     m_mapping_epoch {0}; // (m_mapped_ranges_version's lock-free copy)
	// Ranges mapped since the last BDA synchronization (under m_mapped_ranges_mutex): that
	// synchronization forces their regions (BufferCache::ForceSynchronization()).
	std::vector<std::pair<uint64_t, uint64_t>> m_bda_mapped_since_sync;
	// The latest unmaps (under m_mapped_ranges_mutex): the version each moved to, and the range.
	struct UnmapEntry {
		uint64_t version = 0, vaddr = 0, size = 0;
	};
	std::deque<UnmapEntry> m_unmap_log;
	uint64_t               m_unmap_log_floor = 0; // the version of the newest unmap dropped
	uint64_t               m_frame_number    = 0;
	// SynchronizeDmaFootprint(): the spans of an operation.
	std::vector<std::pair<uint64_t, uint64_t>> m_dma_spans;
	// The guest TLASes the native builds wrote (NoteHleTlas()), and the memory sce::Psr captured
	// for its BVHs (NoteGuestBvhMemory(): size by address). Execution thread.
	struct HleTlas {
		uint64_t              header = 0;
		uint64_t              size   = 0;
		std::vector<uint64_t> blases;
	};
	std::vector<HleTlas>         m_hle_tlases;
	std::map<uint64_t, uint64_t> m_guest_bvh_memory;
	// The buffer tables of alpha tests (AddBufferTable()): the bytes of the table its ranges were
	// decoded from, the memory its V#s describe and what the buffer cache synchronized of it.
	struct BufferTable {
		uint64_t                address = 0;
		std::vector<uint8_t>    copy;
		RangeSet                ranges;
		BufferCache::SetRegions regions;
		// ReachLayout() when the copy was last compared with the table and when the buffers of
		// `ranges` were last synchronized.
		std::array<uint64_t, 3> compared_in {};
		std::array<uint64_t, 3> synced_in {};
		// Pages the CPU wrote that the comparison of another table took from the record of the
		// written pages (tables may share pages).
		std::vector<std::pair<uint64_t, uint64_t>> written;
	};
	// The BDA generation, buffer registration epoch and mapped range version (see PrepareBda()):
	// within one, what the guest's CPU writes after a synchronization waits for the next one.
	[[nodiscard]] std::array<uint64_t, 3> ReachLayout() const {
		return {m_bda_generation, m_buffer_cache.RegisterEpoch(), m_mapped_ranges_version};
	}
	std::vector<std::unique_ptr<BufferTable>> m_buffer_tables;
	// SynchronizeDmaFootprint(): the tables of an operation, and the BLASes its preparation may walk.
	std::vector<BufferTable*> m_dma_sets;
	std::vector<uint64_t>     m_walk_blases;
	// Epochs (CPU dirty, buffer registrations, mapped ranges) read before the last full BDA
	// synchronization.
	std::array<uint64_t, 3> m_bda_sync_epochs {UINT64_MAX, UINT64_MAX, UINT64_MAX};
	// Advances with each guest submission an operation belongs to, after each WAIT_REG_MEM the
	// command processor resolved and whenever the execution thread itself writes guest memory
	// the CPU tracks (see PrepareBda()).
	uint64_t m_bda_generation        = 1;
	uint64_t m_bda_synced_generation = 0;
	// The operations whose reach was not known (they took the synchronization of everything) and,
	// for each program that traces rays, their count at its last dispatch and whether one ran
	// before each of its last 8 dispatches (bit 0: the last) (see SynchronizeDmaFootprint()).
	uint64_t m_bda_unbounded_ops = 0;
	struct TracedProgram {
		uint64_t unbounded_ops = 0;
		uint8_t  recent        = 0;
	};
	std::map<uint64_t, TracedProgram> m_traced_programs;
	uint64_t m_bda_submit            = UINT64_MAX;
	std::unique_ptr<GuestGpu> m_gpu;
	VideoOut::VideoOutDriver* m_video_out = nullptr;
	bool                      m_fault_process_pending = false;
	// Device-address accesses ran since the use bitmap was last read; reads one in UseScanPeriod
	// collections (the buffer cache's age collection waits much longer).
	bool                      m_use_scan_pending      = false;
	uint32_t                  m_use_scan_countdown    = 0;

	std::unique_ptr<RayTracing> m_ray_tracing;

	Common::Mutex                        m_interrupt_mutex;
	std::vector<InterruptEqRegistration> m_interrupt_eqs;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
