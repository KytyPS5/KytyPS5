#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <array>
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
	// KYTY_GPU_PROFILE: the host GPU's time of each draw and dispatch, added up by `key` (its
	// programs) and printed now and then. Begin and End go around one draw or dispatch.
	[[nodiscard]] static bool GpuProfileEnabled();
	// count_samples: false while another occlusion query is open (Vulkan allows one at a time).
	void                      GpuProfileBegin(bool count_samples = true);
	void                      GpuProfileEnd(uint64_t key);
	// The guest shader hash of a program id, for the profile's report.
	static void NoteProgramHash(uint64_t program_id, uint64_t guest_hash);
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
	// Without waiting: whether the priority operations of submissions up to `tick` have run.
	[[nodiscard]] bool PriorityOperationsDone(uint64_t tick);
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
	// The guest synchronized: dispatches recorded after this wait for those before
	// (CommandBuffer::ContinueComputeChain).
	void                           EndComputeChain();
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
	// Deferred operations queued for the open command buffer. Written under m_operation_mutex:
	// any thread queues them. SubmitIfDue reads it after every packet without the lock. Reset at
	// every submission.
	std::atomic<uint32_t>                 m_open_operations = 0;
	std::chrono::steady_clock::time_point m_open_since {};
	void RunOperation(Common::UniqueFunction<void>&& operation);
	void RetireCallbackState(Common::UniqueFunction<void>&& callback);

	MasterSemaphore              m_master;
	// KYTY_GPU_TIME: a timestamp at the start and the end of each command buffer, and the host
	// GPU's time between them added to PerfCounters::Counter::GpuMicroseconds once it completed.
	void                                         StartGpuTimer();
	void                                         StopGpuTimer(uint64_t tick);
	static constexpr uint32_t                    GpuTimerSlots = 256;
	vk::QueryPool                                m_gpu_timer_pool = nullptr;
	uint32_t                                     m_gpu_timer_slot = 0;
	std::deque<std::pair<uint64_t, uint32_t>>    m_gpu_timer_pending;
	static constexpr uint32_t                    ProfileBlocks      = 64;
	static constexpr uint32_t                    ProfileBlockQueries = 2048;
	struct ProfileBlock {
		uint64_t                                    tick = 0;
		uint32_t                                    used = 0;
		std::vector<uint64_t>                       keys;
	};
	vk::QueryPool                                m_profile_pool = nullptr;
	vk::QueryPool                                m_profile_occ = nullptr;
	bool                                         m_profile_counts_samples = false;
	vk::QueryPool                                m_profile_stats = nullptr;
	std::unordered_map<uint64_t, std::array<uint64_t, 3>> m_profile_stat_totals;
	std::unordered_map<uint64_t, uint64_t>       m_profile_samples;
	std::array<ProfileBlock, ProfileBlocks>      m_profile_blocks {};
	uint32_t                                     m_profile_block = 0;
	std::deque<uint32_t>                         m_profile_pending;
	std::unordered_map<uint64_t, std::pair<double, uint64_t>> m_profile_totals;
	uint64_t                                     m_profile_buffers = 0;
	void                                         StartGpuProfile();
	void                                         CollectGpuProfile(uint64_t tick);
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
