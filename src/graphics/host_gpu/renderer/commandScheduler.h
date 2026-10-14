#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler {
public:
	CommandScheduler(RenderContext& context, GraphicContext& graphics);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           BeginRendering(const RenderState& state);
	void           EndRendering();
	void           Flush();
	void           Flush(SubmitInfo& submit);
	// A draw or dispatch went into the open command buffer.
	void NoteWork() noexcept { m_open_work++; }
	// Where the GPU thread asks whether the open command buffer should go out (see submitPlan.h).
	enum class SubmitPoint : uint8_t {
		// Between two PM4 packets: the work limit alone decides, so that what a packet sequence
		// submits does not depend on the clock or on callbacks queued earlier. Packets that need
		// a submission (an end-of-pipe interrupt, a flip) submit explicitly.
		Packet,
		// A guest command buffer ended: the age limit applies too.
		GuestSubmission,
		// The GPU thread has nothing more to record right now.
		Wait,
	};
	// Submits the open command buffer when PlanSubmit says so. GPU thread only.
	bool SubmitIfDue(SubmitPoint point);
	void           FlushAndWait();
	void           Finish();
	// The open submission writes buffer memory that the CPU will read (see readbackPlan.h). The
	// submission then ends with the barrier that makes those writes visible to the CPU.
	void NoteWriteTheCpuReads() noexcept { m_cpu_reads_writes = true; }
	// Waits until the host GPU has finished a submission that was made before. Any thread. False
	// when the device no longer answers (the emulator is shutting down).
	[[nodiscard]] bool WaitSubmitted(uint64_t tick) noexcept { return m_master.TryWait(tick); }
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	// Runs the deferred operations of every submission so far. The caller has waited until
	// they retired.
	void                      RunRetiredOperations();
	// Guest-memory completions use the priority queue; normal callbacks maintain GPU resources.
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	// The same, also while no guest context is bound: a resource that is destroyed then, for
	// example when the guest unmaps memory, is held by the submissions in flight like any other.
	void                      DeferDestruction(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] bool        HasPendingPriorityOperations();
	[[nodiscard]] static bool InDeferredOperation() noexcept;
	// A completion runs on the priority thread, off the GPU thread: it cannot wait for work the
	// GPU thread does, such as a read-back.
	[[nodiscard]] static bool InCompletionOffGpuThread() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] DynamicState&    GetDynamicState() noexcept { return m_dynamic_state; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }

private:
	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();

	private:
		static constexpr size_t GrowStep = 4;

		size_t Grow();

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	// Pending operations hold resources; past this many the scheduler waits for the GPU.
	static constexpr size_t MaxPendingOperations = 4096;

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void BeginNext();
	void PriorityOperationsThread(std::stop_token stop);
	void QueueOperation(Common::UniqueFunction<void>&& operation, bool priority);
	// Without the check that the scheduler is active: a destruction can also be deferred while it
	// shuts down.
	void QueueOperationInAnyState(Common::UniqueFunction<void>&& operation, bool priority);
	bool m_cpu_reads_writes = false;
	// What the open command buffer holds, for SubmitIfDue. Reset at every submission.
	uint32_t                              m_open_work      = 0;
	uint32_t                              m_open_callbacks = 0;
	// Deferred operations queued for the open command buffer. Under m_operation_mutex: any
	// thread queues them. Reset at every submission.
	uint32_t                              m_open_operations = 0;
	std::chrono::steady_clock::time_point m_open_since {};
	void RunOperation(Common::UniqueFunction<void>&& operation);
	void RetireCallbackState(Common::UniqueFunction<void>&& callback);

	MasterSemaphore              m_master;
	// Priority callbacks that finished while the scheduler shut down. They may own resources, so
	// Shutdown destroys them once the queue is idle.
	std::vector<Common::UniqueFunction<void>> m_retired_while_draining;
	// This scheduler's timeline in GraphicContext::queue_commits.
	uint32_t                     m_queue_timeline = 0;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	CommandBuffer                m_command;
	DynamicState                 m_dynamic_state;
	std::queue<PendingOperation> m_pending_operations;
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
