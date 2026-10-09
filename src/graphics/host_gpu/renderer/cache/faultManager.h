#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_

#include "common/abi.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>
#include <span>

namespace Libs::Graphics {

class BufferCache;

// The fault buffer holds three page bitmaps (one bit per BufferCache caching page): shaders record
// device-address accesses that found no cached buffer in the first, pages that V#-table stores
// wrote in the second, and device-address accesses that found a cached buffer in the third. All
// are read back asynchronously, as are the hack marks after them (BufferCache::HACK_MARKS_OFFSET).
class FaultManager {
	static constexpr size_t MaxPendingFaults = 8;
	// Fault reads per read of the hack marks.
	static constexpr uint32_t HackMarksPeriod = 32;

public:
	using PagesHandler = Common::UniqueFunction<void, std::span<const uint64_t>>;

	FaultManager(GraphicContext& graphics, CommandScheduler& scheduler, BufferCache& buffer_cache);
	~FaultManager();
	KYTY_CLASS_NO_COPY(FaultManager);

	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return &m_fault_buffer; }
	// Records the first clear of the fault buffer; call before recording a shader that uses it.
	void                  PrepareFaultBuffer();
	void                  ProcessFaultBuffer();
	// Clears the write bitmap and, once the GPU work recorded so far completes, passes the guest
	// addresses of the written caching pages to `handler`.
	void                  ProcessWriteBuffer(PagesHandler&& handler);
	// The same for the pages that device-address accesses found cached.
	void                  ProcessUseBuffer(PagesHandler&& handler);

private:
	struct Reader {
		Reader(GraphicContext& graphics, CommandScheduler& scheduler, size_t capacity);

		Buffer                                download;
		size_t                                area_size = 0;
		std::array<uint64_t, MaxPendingFaults> ticks {};
		uint32_t                              current = 0;
	};

	void Process(Reader& reader, uint64_t bitmap_offset, PagesHandler&& handler);
	// Copies the hack marks to the host and clears them; once the GPU work recorded so far
	// completes, reports each marked workaround (HackNotice) with the shader hash and the count.
	void ProcessHackMarks();

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	BufferCache&            m_buffer_cache;
	Buffer                  m_fault_buffer;
	Reader                  m_faults;
	Reader                  m_writes;
	Reader                  m_uses;
	Buffer                  m_marks_download;
	uint64_t                m_marks_tick      = 0;
	uint32_t                m_marks_countdown = 0;
	vk::DescriptorSetLayout m_fault_process_desc_layout     = nullptr;
	vk::Pipeline            m_fault_process_pipeline        = nullptr;
	vk::PipelineLayout      m_fault_process_pipeline_layout = nullptr;
	bool                    m_cleared                       = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
