#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_QUEUESUBMITTER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_QUEUESUBMITTER_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Libs::Graphics {

// Hands finished command buffers to the Vulkan queue from a dedicated thread. The emulation
// thread only reserves the timeline tick and pushes the work; the thread submits everything that
// is queued with a single vkQueueSubmit. A game issues hundreds of tiny submits per frame, so the
// per-call driver cost used to dominate the CPU time of the graphics thread.
//
// Submission order is identical to enqueue order. Anything that must observe the work on the
// host or on the queue (host waits, present, waitIdle) has to call Drain() first.
// KYTY_SYNC_SUBMIT=1 (or KYTY_GPU_SYNC=1) submits inline instead.
class QueueSubmitter {
public:
	struct DebugInfo {
		uint32_t op        = 0;
		uint64_t submit_id = 0;
		uint32_t arg0      = 0;
		uint32_t arg1      = 0;
		uint32_t arg2      = 0;
		uint32_t arg3      = 0;
		uint64_t arg4      = 0;
	};

	explicit QueueSubmitter(GraphicContext& graphics);
	~QueueSubmitter();
	KYTY_CLASS_NO_COPY(QueueSubmitter);

	// Reserves the next tick of `master`, signals it when the work finishes and returns it.
	uint64_t Enqueue(MasterSemaphore& master, SubmitInfo info, vk::CommandBuffer buffer,
	                 const DebugInfo& debug);
	// Blocks until everything enqueued before the call was passed to the driver.
	void Drain();

private:
	struct Item {
		SubmitInfo        info;
		vk::CommandBuffer buffer = nullptr;
		uint64_t          tick   = 0;
		DebugInfo         debug;
	};

	void Run(std::stop_token stop);
	void SubmitBatch(std::vector<Item>& batch);

	GraphicContext&                 m_graphics;
	bool                            m_inline = false;
	std::mutex                      m_mutex;
	std::condition_variable_any     m_work_available;
	std::condition_variable         m_progress;
	std::deque<Item>                m_pending;
	uint64_t                        m_enqueued  = 0;
	uint64_t                        m_submitted = 0;
	std::jthread                    m_thread;
};

// Null-safe helper for code that only has the GraphicContext.
void DrainQueueSubmits(GraphicContext& graphics);

} // namespace Libs::Graphics

#endif
