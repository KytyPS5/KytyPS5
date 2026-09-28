#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/hostCopyQueue.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler {
public:
	// With deferred_recording, command buffers are recorded into a CommandStream and a thread of
	// the scheduler replays them into Vulkan command buffers and submits them in order.
	CommandScheduler(RenderContext& context, GraphicContext& graphics,
	                 bool deferred_recording = false);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(const HW::Context& registers, const HW::UserConfig& user_config,
	                     const HW::Shader& shaders);
	void           EndRendering() { m_command.EndRendering(); }
	void           Flush();
	void           Flush(SubmitInfo& submit);
	void           Finish();
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	// Whether a deferred or priority operation that may write guest memory in
	// [address, address + size) is queued or running.
	[[nodiscard]] bool HasPendingGuestOperations(uint64_t address, uint64_t size);
	void               WaitPriorityOperations(uint64_t tick);
	// Completions that may act on guest memory: an unmap of memory one may write drains the GPU
	// while it is pending. They write at most [address, address + size), by default anywhere.
	void DeferOperation(Common::UniqueFunction<void>&& operation, uint64_t address = 0,
	                    uint64_t size = UINT64_MAX);
	void DeferPriorityOperation(Common::UniqueFunction<void>&& operation, uint64_t address = 0,
	                            uint64_t size = UINT64_MAX);
	// Completions that only act on host state (cache bookkeeping, releases, flips, interrupts).
	void DeferHostOperation(Common::UniqueFunction<void>&& operation, bool priority = false);
	[[nodiscard]] bool HasPendingPriorityOperations();
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] DynamicState&    GetDynamicState() noexcept { return m_dynamic_state; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }
	// Copies of guest data into the stream buffer that the execution thread hands off, with
	// deferred recording (null otherwise). Submissions wait for the copies recorded before them;
	// the owner of the scheduler drains them before any guest-visible effect.
	[[nodiscard]] HostCopyQueue* HostCopies() noexcept { return m_host_copies.get(); }

private:
	void QueueOperation(Common::UniqueFunction<void>&& operation);
	void QueuePriorityOperation(Common::UniqueFunction<void>&& operation);
	Common::UniqueFunction<void> TrackGuestOperation(Common::UniqueFunction<void>&& operation,
	                                                 uint64_t address, uint64_t size);

	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();
		// A command buffer for the recording thread, busy until Retire() gives it the tick of
		// its submission.
		[[nodiscard]] std::pair<vk::CommandBuffer, size_t> CommitDeferred();
		void                                               Retire(size_t index, uint64_t tick);

	private:
		static constexpr size_t GrowStep = 4;

		size_t Grow();
		// A command buffer the GPU finished with, marked busy until `busy_until`.
		size_t Acquire(uint64_t busy_until);

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	// A submission recorded into the stream: the recording thread submits it in order.
	struct RecordedSubmit {
		SubmitInfo info;
		uint64_t   tick            = 0;
		// Host copies queued before the submission.
		uint64_t   host_copies     = 0;
	};

	void BeginNext();
	void RecordingThread();
	void SubmitRecorded(vk::CommandBuffer& buffer, size_t buffer_index,
	                    const RecordedSubmit& recorded);
	void QueueSubmit(vk::CommandBuffer buffer, SubmitInfo& submit, uint64_t tick);
	void PriorityOperationsThread(std::stop_token stop);
	void RunOperation(Common::UniqueFunction<void>&& operation);

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	const bool                   m_deferred;
	CommandChunkQueue            m_chunk_queue;
	CommandStream                m_stream;
	CommandBuffer                m_command;
	DynamicState                 m_dynamic_state;
	std::queue<PendingOperation> m_pending_operations;
	// The tick of the oldest pending operation (UINT64_MAX without any), written under
	// m_operation_mutex: draws check it without the lock.
	std::atomic<uint64_t>        m_pending_front_tick {UINT64_MAX};
	// Calls since draws last queried the GPU timeline for pending operations.
	std::atomic<uint32_t>        m_pending_skips {0};
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	OperationState               m_operation_state      = OperationState::Open;
	uint64_t                     m_priority_active_tick = 0; // 0 while none runs
	// Guest memory that queued or running guest operations may write, [begin, end) by id.
	struct GuestWrite {
		uint64_t id    = 0;
		uint64_t begin = 0;
		uint64_t end   = 0;
	};
	std::mutex                   m_guest_writes_mutex;
	std::vector<GuestWrite>      m_guest_writes;
	uint64_t                     m_next_guest_write     = 0;
	std::unique_ptr<HostCopyQueue> m_host_copies;
	// Replays m_stream when recording is deferred; declared last so it stops first.
	std::jthread m_recording_thread;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
