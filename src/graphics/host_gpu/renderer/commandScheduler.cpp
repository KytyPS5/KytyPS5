#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/perfCounters.h"
#include "common/profiler.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/submitPlan.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

bool CommandScheduler::InCompletionOffGpuThread() noexcept {
	return InDeferredOperation() && !GuestGpu::IsGpuThread();
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	Common::LockGuard lock(graphics.queue_mutex);
	m_queue_timeline = graphics.queue_commits.AddTimeline(
	    [this](uint64_t tick) { return m_master.IsRetired(tick); });
}

CommandScheduler::~CommandScheduler() {
	Shutdown();
	Common::LockGuard lock(m_graphics.queue_mutex);
	m_graphics.queue_commits.RemoveTimeline(m_queue_timeline);
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
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.WaitRetired(CurrentTick() - 1);
	RunRetiredOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	m_graphics.WithQueueIdle([this] {
		std::lock_guard lock(m_operation_mutex);
		m_retired_while_draining.clear();
	});
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
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

bool CommandScheduler::SubmitIfDue(SubmitPoint point) {
	if (m_command.IsInvalid()) {
		return false;
	}
	const bool used = m_command.Used();
	if (used && m_open_since == std::chrono::steady_clock::time_point {}) {
		m_open_since = std::chrono::steady_clock::now();
	}
	uint32_t open_operations = 0;
	{
		std::lock_guard lock(m_operation_mutex);
		open_operations = m_open_operations;
	}
	SubmitState state {.open_work       = m_open_work,
	                   .open_callbacks  = point == SubmitPoint::Packet ? 0u : m_open_callbacks,
	                   .open_operations = open_operations,
	                   .open_used       = used,
	                   .about_to_wait  = point == SubmitPoint::Wait};
	// The clock is read only when the age can decide: it is the costly part of this check.
	if (point == SubmitPoint::GuestSubmission && used && state.open_callbacks == 0 &&
	    m_open_work < SUBMIT_WORK_LIMIT) {
		state.open_age_us = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
		                                                          m_open_since)
		        .count());
	}
	switch (PlanSubmit(state)) {
		case SubmitReason::None: return false;
		case SubmitReason::Callbacks: KYTY_PROFILER_MESSAGE("submit: callbacks"); break;
		case SubmitReason::Idle: KYTY_PROFILER_MESSAGE("submit: idle"); break;
		case SubmitReason::Operations: KYTY_PROFILER_MESSAGE("submit: operations"); break;
		case SubmitReason::Work: KYTY_PROFILER_MESSAGE("submit: work"); break;
		case SubmitReason::Age: KYTY_PROFILER_MESSAGE("submit: age"); break;
	}
	Flush();
	return true;
}

void CommandScheduler::FlushAndWait() {
	const auto tick = Submit();
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	KYTY_PROFILER_FUNCTION();
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	// Until every submission has retired, not only until the GPU has reached it: the deferred
	// operations below run only then, and a caller of Finish relies on them having run.
	m_master.WaitRetired(CurrentTick() - 1);
	BeginNext();
	bool pending = false;
	{
		std::lock_guard lock(m_operation_mutex);
		pending = !m_pending_operations.empty();
	}
	if (!pending) {
		return;
	}
	RunRetiredOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick);
		BeginNext();
	} else {
		m_master.Wait(tick);
	}
}

void CommandScheduler::RunRetiredOperations() {
	WaitPriorityOperations(CurrentTick() - 1);
	// Also for the other scheduler and the presents, and with nothing committed until every
	// deferred operation has run.
	m_graphics.WithQueueIdle([this] { PopPendingOperations(); });
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		uint64_t front_tick = 0;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty()) {
				return;
			}
			front_tick = m_pending_operations.front().tick;
			// Cheap early out, without holding off submissions: checked again below. An operation
			// of the open submission is never forced, also past the limit: the open command buffer
			// can still use what it destroys, and waiting for that submission here can block the
			// thread that has to submit it. Many of them make the GPU thread submit at the next
			// packet instead (see submitPlan.h).
			if (front_tick >= m_master.CurrentTick() ||
			    (m_pending_operations.size() <= MaxPendingOperations &&
			     !m_master.IsRetired(std::max(front_tick, m_master.CurrentTick() - 1)))) {
				return;
			}
		}
		WaitPriorityOperations(front_tick);

		// Deferred operations destroy resources. An operation runs only when every command buffer
		// committed to the queue so far has completed, also those of the other scheduler and of
		// the presents (see queueCommits.h), and no submission can be made between that check and
		// the end of the operation: any thread may get here, while the GPU thread submits.
		Common::LockGuard submissions_held(m_graphics.queue_mutex);
		PendingOperation  operation;
		{
			std::unique_lock lock(m_operation_mutex);
			if (m_pending_operations.empty()) {
				return;
			}
			if (m_pending_operations.front().tick != front_tick) {
				continue;
			}
			const auto last_submitted = m_master.CurrentTick() - 1;
			if (front_tick > last_submitted) {
				return; // of the open submission: see above
			}
			if (!m_master.IsRetired(last_submitted) || !m_graphics.queue_commits.AllDone()) {
				if (m_pending_operations.size() <= MaxPendingOperations) {
					return;
				}
				// The GPU never caught up: wait instead of holding resources without limit.
				lock.unlock();
				m_master.WaitRetired(last_submitted);
				lock.lock();
				if (m_pending_operations.empty() ||
				    m_pending_operations.front().tick != front_tick) {
					continue;
				}
				// Another submitter's command buffer or a present may still run. Submissions are
				// held off here, so the queue is drained.
				if (!m_graphics.queue_commits.AllDone()) {
					lock.unlock();
					m_graphics.WithQueueIdle([] {});
					lock.lock();
					if (m_pending_operations.empty() ||
					    m_pending_operations.front().tick != front_tick) {
						continue;
					}
				}
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {
	CheckActive();
	QueueOperationInAnyState(std::move(operation), priority);
}

void CommandScheduler::DeferDestruction(Common::UniqueFunction<void>&& operation) {
	QueueOperationInAnyState(std::move(operation), false);
}

void CommandScheduler::QueueOperationInAnyState(Common::UniqueFunction<void>&& operation,
                                                bool                           priority) {
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		queue.push({std::move(operation), CurrentTick()});
		if (priority) {
			// Runs when the open submission completes: the submission must go out for that.
			m_open_callbacks++;
		} else {
			m_open_operations++;
		}
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler != this) {
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
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
		m_master.WaitRetired(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
			RetireCallbackState(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::RetireCallbackState(Common::UniqueFunction<void>&& callback) {
	// A callback may own resources, such as a download buffer. They are destroyed with the
	// callback, so hand it to the deferred queue, which destroys resources only when no submission
	// holds them.
	std::lock_guard lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_pending_operations.push(
		    {[state = std::move(callback)]() mutable { state = {}; }, CurrentTick()});
		m_open_operations++;
		return;
	}
	// During shutdown the other scheduler and the presents may still run command buffers that
	// hold these resources: Shutdown destroys them once the queue is idle.
	m_retired_while_draining.push_back(std::move(callback));
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

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	m_dynamic_state.Invalidate();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	if (m_cpu_reads_writes) {
		m_cpu_reads_writes = false;
		m_command.EndRendering();
		vk::MemoryBarrier visible_to_cpu {};
		visible_to_cpu.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
		visible_to_cpu.dstAccessMask = vk::AccessFlagBits::eHostRead;
		m_command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                                   vk::PipelineStageFlagBits::eHost, {}, 1,
		                                   &visible_to_cpu, 0, nullptr, 0, nullptr);
	}
	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, m_master.AcquireFence(tick));
		PerfCounters::Add(PerfCounters::Counter::Submissions);
		if (result == vk::Result::eSuccess) {
			graphics.queue_commits.NoteSubmit(m_queue_timeline, tick);
		}
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer = nullptr;
	{
		// An operation queued between the tick's change and here is not counted: the limit is
		// a soft one.
		std::lock_guard lock(m_operation_mutex);
		m_open_operations = 0;
	}
	m_open_work      = 0;
	m_open_callbacks = 0;
	m_open_since     = {};
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
