#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class RenderContext;
struct GraphicContext;

// Guest GPU clock writes (end-of-pipe timestamps, COPY_DATA of the GPU clock), taken by the host
// GPU when the command executes. They reach guest memory, in the guest 100 MHz reference clock,
// when the command buffer completes and before the interrupts recorded after them.
class GpuTimestamps {
public:
	// Timestamps in flight. A write beyond them waits for the oldest to complete.
	static constexpr uint32_t QueryCount = 4096;

	// reference = reference_base + (ticks - device_base) * rate / 2^32
	struct Segment {
		uint64_t device_base    = 0;
		uint64_t reference_base = 0;
		uint64_t rate           = 0;
	};

	GpuTimestamps(GraphicContext& graphics, CommandScheduler& scheduler, RenderContext& context);
	~GpuTimestamps();
	KYTY_CLASS_NO_COPY(GpuTimestamps);

	// End of pipe: after all earlier commands complete. Otherwise when the command is reached.
	void Write(uint64_t vaddr, uint32_t size, bool end_of_pipe);

	// Conversion of host ticks at or after previous.device_base.
	[[nodiscard]] static uint64_t ToReference(const Segment& previous, const Segment& current,
	                                          uint64_t ticks);

private:
	struct Pending {
		uint64_t vaddr = 0;
		uint32_t query = 0;
		uint32_t size  = 0;
	};

	struct Batch {
		std::vector<Pending> writes;
		uint32_t             first_query = 0;
		Segment              previous;
		Segment              current;
		bool                 resolved = false;
	};

	struct Retry {
		uint64_t vaddr = 0;
		uint64_t value = 0;
		uint32_t size  = 0;
	};

	[[nodiscard]] bool SampleClocks(uint64_t& ticks, uint64_t& reference) const;
	void               InitializeWithSubmission(bool anchor);
	void               Calibrate();
	void               Resolve();
	void               Complete(const Batch& batch);
	void               StoreRetries();

	GraphicContext&        m_graphics;
	CommandScheduler&      m_scheduler;
	RenderContext&         m_context;
	uint64_t               m_mask = 0;
	Buffer                 m_readback;
	vk::QueryPool          m_pool = nullptr;
	std::shared_ptr<Batch> m_batch;
	uint64_t               m_issued = 0;
	std::atomic<uint64_t>  m_retired {0};
	std::mutex             m_mutex;
	std::vector<Retry>     m_retries;
	bool                   m_retrying        = false;
	bool                   m_calibrated      = false;
	uint64_t               m_first_ticks     = 0;
	uint64_t               m_first_reference = 0;
	uint64_t               m_last_ticks      = 0;
	uint64_t               m_last_reference  = 0;
	Segment                m_previous;
	Segment                m_current;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
