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
#include "graphics/host_gpu/renderer/occlusionQueries.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"
#include "graphics/host_gpu/renderer/pipeline/imageHeap.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/rayTracing.h"
#include "kernel/eventQueue.h"

#include <array>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace Libs::VideoOut {
class VideoOutDriver;
}

namespace Libs::Graphics {

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
	RenderExecutor&     GetRenderExecutor() { return m_render_executor; }
	OcclusionQueries&   GetOcclusionQueries() { return m_occlusion_queries; }
	RayTracing*         GetRayTracing() { return m_ray_tracing.get(); } // With ray queries only.

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
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	// Advances whenever a range is mapped or unmapped.
	[[nodiscard]] uint64_t MappedRangesVersion() const noexcept;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	void               CacheDmaBases(const ShaderStageRuntime& runtime);
	void               PrepareBda();
	void               RunGarbageCollector();

	void AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void TriggerInterrupt(int event_id, uint32_t context_id);

private:
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
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	uint64_t                  m_mapped_ranges_version = 0;
	// Ranges mapped since the last BDA synchronization (under m_mapped_ranges_mutex): that
	// synchronization forces their regions (BufferCache::ForceSynchronization()).
	std::vector<std::pair<uint64_t, uint64_t>> m_bda_mapped_since_sync;
	// SynchronizeDmaFootprint(): the spans of an operation.
	std::vector<std::pair<uint64_t, uint64_t>> m_dma_spans;
	// Epochs (CPU dirty, buffer registrations, mapped ranges) read before the last full BDA
	// synchronization.
	std::array<uint64_t, 3> m_bda_sync_epochs {UINT64_MAX, UINT64_MAX, UINT64_MAX};
	std::unique_ptr<GuestGpu> m_gpu;
	VideoOut::VideoOutDriver* m_video_out = nullptr;
	bool                      m_fault_process_pending = false;

	std::unique_ptr<RayTracing> m_ray_tracing;

	Common::Mutex                        m_interrupt_mutex;
	std::vector<InterruptEqRegistration> m_interrupt_eqs;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
