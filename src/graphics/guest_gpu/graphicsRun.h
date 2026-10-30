#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/spscQueue.h"
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
#include <vector>

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
	// Runs a host command on the execution thread before the operations queued so far, and
	// waits for it. Only for work that no queued operation depends on (memory the guest unmaps
	// after the operations that used it executed).
	void SendUrgentCommandSync(Common::UniqueFunction<void>&& command);

	// Submitted command memory is borrowed and must remain valid until GPU execution completes.
	void Submit(std::span<const uint32_t> commands);
	void SubmitCompute(uint32_t queue, std::span<const uint32_t> commands);
	void SubmitFlipPreparation(uint64_t request_id);
	// Insert an ordered graphics drain; only a previous suspend point can block the caller.
	void SuspendPoint();
	// Wait for guest command processing, including all compute queues (not native GPU completion).
	void              WaitForIdle();
	[[nodiscard]] int GetFrameNum() const;

	// The thread that owns the renderer: the execution thread of a pipelined GPU.
	[[nodiscard]] static bool IsGpuThread() noexcept;

	// A pipelined GPU parses command buffers on its command processor thread and executes the
	// operations they produce, in order, on its execution thread.
	[[nodiscard]] bool Pipelined() const noexcept { return m_pipelined; }
	// Command processor thread.
	[[nodiscard]] uint64_t NextSequence() const noexcept { return m_emitted + 1; }
	// Returns the sequence of the operation.
	uint64_t               EmitOperation(GpuOperation&& operation);
	// The guest memory an emitted operation writes, and the value it writes when that is known
	// when the operation is emitted (`value` null otherwise, or larger than 8 bytes).
	void NoteGuestWrite(uint64_t sequence, uint64_t address, uint64_t size, const void* value);
	// Reads guest memory the command processor decides on as a serial GPU would at this point
	// of the stream: from the value of a pending write when it is known, otherwise from memory
	// once the operations that write it executed.
	void ReadDecisionMemory(const volatile void* address, void* value, size_t size);
	void                   WaitForExecution(uint64_t sequence) { m_executed.WaitFor(sequence); }
	// Waits until every operation emitted so far executed.
	void WaitForExecution() { m_executed.WaitFor(m_emitted); }
	// Command processor thread: the queue being processed waits on this guest memory.
	void NoteBlockedWait(uint64_t address, uint64_t size);
	// Command processor thread: whether another queue waits on guest memory in the range.
	[[nodiscard]] bool OtherQueueWaitsOn(uint64_t address, uint64_t size) const;
	[[nodiscard]] const GpuRegisterState& NeutralState() const noexcept { return m_neutral_state; }

private:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t ComputeQueueBase     = 0x20;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	enum class SubmissionType { Commands, FlipPreparation, SuspendPoint };

	struct Submission {
		SubmissionType            type     = SubmissionType::Commands;
		uint32_t                  queue_id = 0;
		std::span<const uint32_t> commands;
		Pm4Execution              execution;
		bool                      blocked         = false;
		bool                      yielded         = false;
		uint64_t                  flip_request_id = 0;
	};

	void              Enqueue(Submission submission);
	void              ProcessCommands();
	void              ExecutionThread();
	void              ResolveThread();
	void              StopExecution();
	// Command processor thread of a pipelined GPU: runs a command on the execution thread and
	// waits for it.
	void              ExecuteSync(Common::UniqueFunction<void>&& command);
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
	// Completion callbacks can outlive GuestGpu during renderer shutdown.
	std::shared_ptr<std::binary_semaphore> m_suspend_point_ready =
	    std::make_shared<std::binary_semaphore>(1);

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;

	// Command processor thread: the guest memory the blocked front submission of each queue
	// waits on (empty when it does not wait on memory).
	struct WaitRange {
		uint64_t begin = 0;
		uint64_t end   = 0;
	};
	std::array<WaitRange, QueueCount> m_wait_ranges {};
	uint32_t                          m_waiting_queues = 0;
	uint32_t                          m_current_queue  = 0;

	// Pipelined execution. m_emitted is only used by the command processor thread.
	struct GuestWrite {
		uint64_t sequence = 0;
		uint64_t address  = 0;
		uint64_t size     = 0;
		bool     known    = false;
		uint8_t  value[8] = {};
	};

	const bool                      m_pipelined;
	// With a resolve stage, the command processor queues operations for the resolve thread,
	// which forwards them to the execution thread.
	const bool                      m_resolving;
	Common::SpscQueue<GpuOperation> m_operations;
	Common::SpscQueue<GpuOperation> m_resolved;
	Common::ProgressCounter         m_executed;
	uint64_t                        m_emitted = 0;
	// Command processor thread: a ring of the writes of operations that may not have executed
	// yet, oldest first. It holds more writes than operations can be queued.
	static constexpr size_t PendingWriteCapacity = 16384;
	std::unique_ptr<GuestWrite[]> m_pending_writes;
	size_t                        m_pending_first = 0;
	size_t                        m_pending_count = 0;
	void                          ForgetExecutedWrites();
	// Commands that run before the next operation the execution thread takes.
	void                                      RunUrgentCommands();
	std::mutex                                m_urgent_mutex;
	std::vector<Common::UniqueFunction<void>> m_urgent_commands;
	std::atomic_uint32_t                      m_urgent_pending {0};
	// Game and host threads bump it when they queue work, under m_queue_mutex; the command
	// processor thread spins on it before it sleeps.
	std::atomic_uint64_t m_work_epoch {0};
	HW::Context                     m_neutral_context;
	HW::UserConfig                  m_neutral_user_config;
	HW::Shader                      m_neutral_shaders;
	GpuRegisterState                m_neutral_state;
	std::jthread                    m_executor;
	std::jthread                    m_resolver;

	std::jthread m_thread;

	friend class CommandProcessor;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
