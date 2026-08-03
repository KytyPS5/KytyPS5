#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

void CommandSlot::Reset() {
	EXIT_IF(buffer == nullptr);
	const auto result = buffer.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
	if (result != vk::Result::eSuccess) {
		EXIT("failed to reset Vulkan command buffer: %s (%d)\n", VulkanToString(result).c_str(),
		     static_cast<int>(result));
	}
}

CommandScheduler::CommandPool::~CommandPool() {
	Destroy();
}

void CommandScheduler::CommandPool::Create(GraphicContext& graphics) {
	EXIT_IF(m_pool != nullptr || m_graphics != nullptr ||
	        graphics.queue_family == static_cast<uint32_t>(-1));
	m_graphics = &graphics;

	vk::CommandPoolCreateInfo create {};
	create.sType            = vk::StructureType::eCommandPoolCreateInfo;
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandSlot* CommandScheduler::CommandPool::CreateSlot() {
	EXIT_IF(m_graphics == nullptr);
	auto& graphics = *m_graphics;

	vk::CommandBufferAllocateInfo allocate {};
	allocate.sType              = vk::StructureType::eCommandBufferAllocateInfo;
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer buffer    = nullptr;
	EXIT_IF(graphics.device.allocateCommandBuffers(&allocate, &buffer) != vk::Result::eSuccess);

	vk::FenceCreateInfo fence_create {};
	fence_create.sType = vk::StructureType::eFenceCreateInfo;
	fence_create.flags = vk::FenceCreateFlagBits::eSignaled;
	vk::Fence fence    = nullptr;
	if (graphics.device.createFence(&fence_create, nullptr, &fence) != vk::Result::eSuccess) {
		graphics.device.freeCommandBuffers(m_pool, 1, &buffer);
		EXIT("failed to create command-buffer fence\n");
	}

	auto& slot      = m_slots.emplace_back();
	slot.pool_mutex = &m_mutex;
	slot.id         = static_cast<uint32_t>(m_slots.size() - 1);
	slot.buffer     = buffer;
	slot.fence      = fence;
	return &slot;
}

CommandSlot* CommandScheduler::CommandPool::Allocate(GraphicContext& graphics) {
	Common::LockGuard lock(m_mutex);
	if (m_pool == nullptr) {
		Create(graphics);
	}
	EXIT_IF(m_graphics != &graphics);
	auto  found = std::ranges::find_if(m_slots, [](const auto& slot) { return !slot.busy; });
	auto* slot  = found != m_slots.end() ? &*found : CreateSlot();
	slot->busy  = true;
	slot->Reset();
	return slot;
}

void CommandScheduler::CommandPool::Destroy() {
	Common::LockGuard lock(m_mutex);
	if (m_pool == nullptr) {
		return;
	}
	EXIT_IF(std::ranges::any_of(m_slots, [](const auto& slot) { return slot.busy; }));
	EXIT_IF(m_graphics == nullptr);
	for (const auto& slot: m_slots) {
		m_graphics->device.destroyFence(slot.fence, nullptr);
	}
	m_graphics->device.destroyCommandPool(m_pool, nullptr);
	m_slots.clear();
	m_pool     = nullptr;
	m_graphics = nullptr;
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (Active() && m_recording) {
		Finish();
	}
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_registers   = &registers;
	m_user_config = &user_config;
	m_shaders     = &shaders;

	if (!Active()) {
		for (auto& buffer: m_buffers) {
			buffer = std::make_unique<RenderCommandBuffer>(*this);
		}
		m_current = 0;
	}

	BindCurrent();
	if (!m_recording) {
		Current().Begin();
		m_recording = true;
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && m_recording) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	SubmitCurrent(submit);
	BeginNext();
}

CommandBuffer& CommandScheduler::FlushAndGetSubmitted() {
	SubmitInfo submit;
	auto&      submitted = SubmitCurrent(submit);
	BeginNext();
	return submitted;
}

void CommandScheduler::Finish() {
	CheckActive();
	const auto tick = CurrentTick();
	if (m_recording) {
		SubmitInfo submit;
		SubmitCurrent(submit);
	}
	for (auto& buffer: m_buffers) {
		buffer->WaitForFenceAndReset();
	}
	m_master.Wait(tick);
	PopPendingOperations();
	BindCurrent();
	Current().Begin();
	m_recording = true;
}

void CommandScheduler::FinishCurrent() {
	SubmitInfo submit;
	auto&      submitted = SubmitCurrent(submit);
	submitted.WaitForFenceAndReset();
	m_master.Refresh();
	PopPendingOperations();
	submitted.Begin();
	m_recording = true;
}

void CommandScheduler::Wait(uint64_t tick) {
	CheckActive();
	EXIT_IF(tick > CurrentTick());
	if (tick >= CurrentTick()) {
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). Recycle the same command object so that reference remains valid.
		FinishCurrent();
		return;
	}
	m_master.Wait(tick);
	PopPendingOperations();
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		Common::UniqueFunction<void> callback;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			callback = std::move(m_pending_operations.front().callback);
			m_pending_operations.pop();
		}
		RunOperation(std::move(callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_pending_operations.push({std::move(operation), CurrentTick()});
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_priority_operations.push({std::move(operation), CurrentTick()});
		lock.unlock();
		m_operation_available.notify_one();
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

CommandSlot* CommandScheduler::AllocateCommandBuffer() {
	return m_command_pool.Allocate(m_graphics);
}

uint64_t CommandScheduler::NextSubmitSequence() noexcept {
	return m_submit_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active() || m_current >= BufferCount);
}

RenderCommandBuffer& CommandScheduler::Current() const {
	CheckActive();
	EXIT_IF(m_buffers[m_current] == nullptr);
	return *m_buffers[m_current];
}

void CommandScheduler::BindCurrent() const {
	EXIT_IF(m_registers == nullptr || m_user_config == nullptr || m_shaders == nullptr);
	Current().Bind(*m_registers, *m_user_config, *m_shaders);
}

CommandBuffer& CommandScheduler::SubmitCurrent(SubmitInfo& submit) {
	CheckActive();
	EXIT_IF(!m_recording);
	auto& submitted = Current();
	submitted.End();
	const auto signal_tick = m_master.NextTick();
	submit.AddSignal(m_master.Handle(), signal_tick);
	submitted.Execute(submit);
	m_recording = false;
	return submitted;
}

void CommandScheduler::BeginNext() {
	EXIT_IF(m_recording);
	m_current = (m_current + 1) % BufferCount;
	Current().WaitForFenceAndReset();
	PopPendingOperations();
	BindCurrent();
	Current().Begin();
	m_recording = true;
}

} // namespace Libs::Graphics
