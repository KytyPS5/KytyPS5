#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "common/uniqueFunction.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>

namespace Libs::Graphics {

class RenderContext;

/// An end-of-pipe label and/or interrupt held back behind image readbacks.
struct DeferredSignal {
	RenderContext*                         renderer             = nullptr;
	void*                                  label                = nullptr; // Null for no label.
	uint64_t                               value                = 0;
	uint32_t                               size                 = 0;
	bool                                   interrupt            = false;
	int                                    event_id             = 0;
	uint32_t                               interrupt_context_id = 0;
	std::shared_ptr<std::atomic<uint32_t>> pending;

	/// Writes the label, lets submissions blocked on it retry at once, and raises the interrupt.
	/// GPU thread only, outside the queue lock.
	void Land() const;
	/// Gives the signal up without landing it, for when the GPU has stopped taking work.
	void Drop() const;
};

class GuestGpu final {
public:
	explicit GuestGpu(RenderContext& renderer);
	~GuestGpu();
	KYTY_CLASS_NO_COPY(GuestGpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);
	// Lets blocked submissions retry on the GPU thread's next pass instead of after its stall poll
	// (100 us, or a timer tick on Windows). GPU thread only, outside the queue lock.
	void               RetryBlockedSubmissions();
	// Like SendCommand, but returns false instead of exiting once the GPU thread has stopped taking
	// work.
	[[nodiscard]] bool TrySendCommand(Common::UniqueFunction<void>&& command);
	/// Queues a deferred signal whose readbacks have landed, to land on the GPU thread in the order
	/// signals are queued. Called from the priority thread.
	void QueueReadySignal(const DeferredSignal& signal);
	/// Lands every queued ready signal, oldest first. GPU thread only.
	void LandReadySignals();

	// Submitted command memory is borrowed and must remain valid until GPU execution completes.
	void Submit(std::span<const uint32_t> commands);
	void SubmitCompute(uint32_t queue, std::span<const uint32_t> commands);
	void SubmitFlipPreparation(uint64_t request_id);
	// Insert an ordered graphics drain; only a previous suspend point can block the caller.
	void SuspendPoint();
	// Wait for guest command processing, including all compute queues (not native GPU completion).
	void              WaitForIdle();
	[[nodiscard]] int GetFrameNum() const;

	[[nodiscard]] static bool IsGpuThread() noexcept;

private:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t ComputeQueueBase     = 0x20;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	enum class SubmissionType { Graphics, Compute, FlipPreparation, SuspendPoint };

	struct Submission {
		SubmissionType            type     = SubmissionType::Graphics;
		uint32_t                  queue_id = 0;
		std::span<const uint32_t> commands;
		Pm4Execution              execution;
		bool                      started         = false;
		bool                      blocked         = false;
		uint64_t                  flip_request_id = 0;
	};

	void              Enqueue(Submission submission);
	void              ProcessCommands();
	bool              Process(Submission& submission);
	static void       ThreadRun(void* data);
	CommandProcessor& GetProcessor(uint32_t queue_id);

	RenderContext&                                 m_renderer;
	Common::Mutex                                  m_queue_mutex;
	std::mutex                                     m_shutdown_mutex;
	Common::CondVar                                m_work_available;
	Common::CondVar                                m_idle;
	std::array<std::deque<Submission>, QueueCount> m_queues;
	std::deque<Common::UniqueFunction<void>>       m_commands;
	std::atomic_uint32_t                           m_pending_commands {0};
	// Deferred signals whose readbacks have landed, waiting for the GPU thread to land them. A
	// separate queue so an unmap can land them before removing their memory.
	Common::Mutex                                  m_ready_mutex;
	std::deque<DeferredSignal>                     m_ready_signals;
	uint32_t                                       m_next_queue        = 0;
	uint32_t                                       m_submission_count  = 0;
	bool                                           m_processing        = false;
	bool                                           m_accepting         = true;
	// Commands are still accepted after m_accepting is cleared, until the GPU thread stops. A
	// submission still running at shutdown can be blocked on a deferred label that lands through a
	// command.
	bool                                           m_accepting_commands = true;
	bool                                           m_stopping          = false;
	bool                                           m_shutdown_complete = false;
	// Completion callbacks can outlive GuestGpu during renderer shutdown.
	std::shared_ptr<std::binary_semaphore> m_suspend_point_ready =
	    std::make_shared<std::binary_semaphore>(1);

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;
	std::jthread    m_thread;

	friend class CommandProcessor;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
