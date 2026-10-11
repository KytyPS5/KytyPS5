#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class StreamBuffer;

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
	void           FlushAndWait();
	void           Finish();
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	// `write_address`/`write_size` name the guest memory the operation writes, if any.
	void DeferPriorityOperation(Common::UniqueFunction<void>&& operation, uint64_t write_address = 0,
	                            uint64_t write_size = 0);
	// Newest tick a queued write of guest memory overlapping the range waits for, or 0.
	[[nodiscard]] uint64_t PendingGuestWriteTick(uint64_t address, uint64_t size);
	// Waits for every queued write of guest memory overlapping the range.
	void WaitGuestWrites(uint64_t address, uint64_t size);
	// Records into its own command buffer that waits only for `wait_tick`, already submitted.
	void RunDetached(uint64_t wait_tick, Common::UniqueFunction<void, vk::CommandBuffer>&& record);
	// Runs at the end of every recording, before it is submitted; it may record more commands.
	void SetBeforeSubmit(Common::UniqueFunction<void>&& hook) { m_before_submit = std::move(hook); }
	struct DetachedTicket {
		int       slot  = -1;
		vk::Fence fence = nullptr;
	};
	[[nodiscard]] DetachedTicket
	SubmitDetached(uint64_t wait_tick, Common::UniqueFunction<void, vk::CommandBuffer>&& record);
	void                      WaitDetached(const DetachedTicket& ticket);
	void                      ReleaseDetached(const DetachedTicket& ticket);
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] bool             AcceptsOperations();
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] DynamicState&    GetDynamicState() noexcept { return m_dynamic_state; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	// See StreamBuffer::RetagWatches. A stream buffer registers for its lifetime.
	void                           RegisterStreamBuffer(StreamBuffer* buffer);
	void                           UnregisterStreamBuffer(StreamBuffer* buffer);
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

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick        = 0;
		uint64_t                     write_begin = 0;
		uint64_t                     write_end   = 0;
	};

	void BeginNext();
	void RetagStreamWatches(uint64_t submitted_tick);
	void PriorityOperationsThread(std::stop_token stop);
	void RunOperation(Common::UniqueFunction<void>&& operation);

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	CommandBuffer                m_command;
	DynamicState                 m_dynamic_state;
	std::queue<PendingOperation> m_pending_operations;
	std::deque<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active        = false;
	uint64_t                     m_priority_active_tick   = 0;
	uint64_t                     m_priority_active_begin  = 0;
	uint64_t                     m_priority_active_end    = 0;
	vk::CommandPool              m_detached_pool          = nullptr;
	vk::CommandBuffer            m_detached_buffer        = nullptr;
	vk::Fence                    m_detached_fence         = nullptr;
	Common::UniqueFunction<void>          m_before_submit;
	bool                                  m_in_before_submit = false;
	struct DetachedSlot {
		vk::CommandBuffer command = nullptr;
		vk::Fence         fence   = nullptr;
		bool              busy    = false;
	};
	std::vector<DetachedSlot>    m_detached_slots;
	OperationState               m_operation_state        = OperationState::Open;
	std::mutex                   m_stream_buffer_mutex;
	std::vector<StreamBuffer*>   m_stream_buffers;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
