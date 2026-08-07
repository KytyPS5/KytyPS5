#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include <queue>

#include <thread>
#include <vector>

namespace Libs::Graphics {

struct CommandSlot {
	Common::Mutex*    pool_mutex = nullptr;
	uint32_t          id         = 0;
	vk::CommandBuffer buffer     = nullptr;
	vk::Fence         fence      = nullptr;
	bool              busy       = false;

	void Reset();
};

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
	CommandBuffer& FlushAndGetSubmitted();
	void           Finish();
	void           FinishCurrent();
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool            Active() const noexcept { return m_current >= 0; }
	void                          CheckActive() const;
	RenderCommandBuffer&          Current() const;
	[[nodiscard]] uint64_t        CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool            IsFree(uint64_t tick);
	[[nodiscard]] RenderContext&  Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext& Graphics() const noexcept { return m_graphics; }

private:
	static constexpr size_t CommandBufferGrowStep = 4;

	class CommandPool {
	public:
		CommandPool() = default;
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		CommandSlot* Allocate(GraphicContext& graphics);

	private:
		void         Create(GraphicContext& graphics);
		CommandSlot* CreateSlot();
		void         Destroy();

		GraphicContext*         m_graphics = nullptr;
		Common::Mutex           m_mutex;
		vk::CommandPool         m_pool = nullptr;
		std::deque<CommandSlot> m_slots;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void                       BindCurrent() const;
	CommandBuffer&             SubmitCurrent(SubmitInfo& submit);
	void                       BeginNext();
	void                       PopPendingOperations(bool refresh_gpu_tick);
	[[nodiscard]] int          FindReusableBuffer(uint64_t gpu_tick) const;
	[[nodiscard]] size_t       GrowCommandBuffers();
	void                       PriorityOperationsThread(std::stop_token stop);
	void                       RunOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] CommandSlot* AllocateCommandBuffer();
	[[nodiscard]] uint64_t     NextSubmitSequence() noexcept;

	MasterSemaphore                                   m_master;
	RenderContext&                                    m_context;
	GraphicContext&                                   m_graphics;
	CommandPool                                       m_command_pool;
	std::vector<std::unique_ptr<RenderCommandBuffer>> m_buffers;
	std::vector<uint64_t>                             m_buffer_ticks;
	std::queue<PendingOperation>                      m_pending_operations;
	std::queue<PendingOperation>                      m_priority_operations;
	std::mutex                                        m_operation_mutex;
	std::condition_variable                           m_operation_available;
	std::jthread                                      m_priority_thread;
	bool                                              m_priority_active      = false;
	uint64_t                                          m_priority_active_tick = 0;
	OperationState                                    m_operation_state      = OperationState::Open;
	int                                               m_current              = -1;
	bool                                              m_recording            = false;
	HW::Context*                                      m_registers            = nullptr;
	HW::UserConfig*                                   m_user_config          = nullptr;
	HW::Shader*                                       m_shaders              = nullptr;
	std::atomic<uint64_t>                             m_submit_sequence      = 0;

	friend class CommandBuffer;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
