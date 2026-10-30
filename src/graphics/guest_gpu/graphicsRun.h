#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "common/uniqueFunction.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/framePacer.h"
#include "graphics/guest_gpu/pendingLabels.h"

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

class GuestGpu final {
public:
	explicit GuestGpu(RenderContext& renderer);
	~GuestGpu();
	KYTY_CLASS_NO_COPY(GuestGpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);
	// The same, for a caller that comes back with a second part of work it has begun: when the
	// GPU thread no longer takes commands (shutdown), nothing runs and the result is false.
	[[nodiscard]] bool TrySendCommandSync(Common::UniqueFunction<void>&& command);

	// Submitted command memory is borrowed and must remain valid until GPU execution completes.
	void Submit(std::span<const uint32_t> commands);
	void SubmitCompute(uint32_t queue, std::span<const uint32_t> commands);
	void SubmitFlipPreparation(uint64_t request_id);
	// A guest frame boundary: ages the caches in order with the guest's work. It blocks the
	// caller while Config::GetGpuFramesInFlight() recorded frames wait for the host GPU.
	void SuspendPoint();
	// Counts a flip the GPU thread records; the host GPU's completion of it calls
	// FrameCompleted on the returned pacer.
	[[nodiscard]] std::shared_ptr<FramePacer> RecordFrame();
	// End-of-pipe labels the host GPU has not completed (pendingLabels.h).
	[[nodiscard]] const std::shared_ptr<PendingLabels>& Labels() const { return m_labels; }

	// Wait for guest command processing, including all compute queues (not native GPU completion).
	void              WaitForIdle();
	[[nodiscard]] int GetFrameNum() const;

	[[nodiscard]] static bool IsGpuThread() noexcept;

	// GPU thread: runs the commands other threads sent (SendCommand), as between packets.
	void ServiceCommands() { ProcessCommands(); }
	// GPU thread: writes the labels the host GPU completed, as a wait that needs them does.
	void ServiceLabels() {
		if (m_labels_ready.load(std::memory_order_acquire)) {
			WriteCompletedLabels();
		}
	}

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
		// Of the commands when they were submitted (a failure tells if the guest changed them).
		uint64_t                  commands_hash   = 0;
		uint64_t                  submitted_ms    = 0;
		uint64_t                  trace_id        = 0;
		// A copy of the commands, to say what changed (KYTY_DCB_SNAPSHOT=1).
		std::shared_ptr<std::vector<uint32_t>> snapshot;
	};

	void              Enqueue(Submission submission);
	void              SubmitRecordedBeforeWaiting();
	void              ProcessCommands();
	// GPU thread. Writes the labels the host GPU completed, with their interrupts.
	void              WriteCompletedLabels();
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
	uint32_t                                       m_next_queue        = 0;
	uint32_t                                       m_submission_count  = 0;
	bool                                           m_processing        = false;
	bool                                           m_accepting         = true;
	bool                                           m_stopping          = false;
	bool                                           m_shutdown_complete = false;
	// Frames recorded ahead of the host GPU. Completion callbacks can outlive GuestGpu during
	// renderer shutdown.
	std::shared_ptr<FramePacer> m_frame_pacer;
	// The renderer's (RenderContext::GetLabels).
	std::shared_ptr<PendingLabels> m_labels;
	// Completed labels wait for the GPU thread (set under m_queue_mutex, read without it too).
	std::atomic_bool                m_labels_ready {false};
	std::vector<PendingLabels::Label> m_completed_labels;

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	// The host submission (scheduler tick) that this thread last recorded guest work into. The
	// thread submits before it waits only when that submission is still open: what others
	// record into the scheduler (the test harness does) is theirs to submit.
	uint64_t        m_recorded_tick = UINT64_MAX;
	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;
	std::jthread    m_thread;

	friend class CommandProcessor;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
