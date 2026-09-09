#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

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

// Matches the guest_gpu PixelPipeStatDump fallback convention: bit 63 marks a counter slot as
// written/ready, the low 63 bits are the sample count the guest masks out and diffs.
static constexpr uint64_t kOcclusionReadyBit = 1ull << 63u;

static bool OcclusionQueriesTrusted() {
	// Default on. Opt out with KYTY_OCCLUSION_QUERIES=0 if a title shows blinking / wrongly culled
	// geometry with real queries -- the fallback reports every primitive visible, which can never
	// cull something that is on screen.
	static const bool trusted = [] {
		const char* v = std::getenv("KYTY_OCCLUSION_QUERIES");
		return v == nullptr || v[0] != '0';
	}();
	return trusted;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	m_pending_occlusion.reserve(64);
}

CommandScheduler::~CommandScheduler() {
	Shutdown();
	if (m_occlusion_pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_occlusion_pool, nullptr);
		m_occlusion_pool = nullptr;
	}
}

bool CommandScheduler::EnsureOcclusionPool() {
	if (m_occlusion_pool != nullptr) {
		return true;
	}
	if (m_occlusion_pool_bad) {
		return false;
	}
	vk::QueryPoolCreateInfo info {};
	info.sType      = vk::StructureType::eQueryPoolCreateInfo;
	info.queryType  = vk::QueryType::eOcclusion;
	info.queryCount = OcclusionQuerySlots;
	if (m_graphics.device.createQueryPool(&info, nullptr, &m_occlusion_pool) !=
	        vk::Result::eSuccess ||
	    m_occlusion_pool == nullptr) {
		m_occlusion_pool     = nullptr;
		m_occlusion_pool_bad = true;
		return false;
	}
	return true;
}

void CommandScheduler::ResetOcclusionPool() {
	// Slot numbering restarts each recording; the pool must be reset outside a render pass, which
	// BeginCommand always is. Anything still pending from the previous recording has already been
	// drained (DrainOcclusionBeforeReuse), so reusing slot 0 here is safe.
	if (m_occlusion_pool == nullptr || m_command.IsInvalid()) {
		return;
	}
	m_command.Handle().resetQueryPool(m_occlusion_pool, 0, OcclusionQuerySlots);
	m_occlusion_next_slot = 0;
	m_occlusion_open_slot = -1;
}

void CommandScheduler::CloseOpenOcclusionQuery() {
	if (m_occlusion_open_slot < 0 || m_occlusion_pool == nullptr || m_command.IsInvalid()) {
		m_occlusion_open_slot = -1;
		return;
	}
	// A query may not outlive its render pass instance and must end in the buffer that began it.
	// Reached only when the guest never issued the matching end dump; the pending entry then
	// resolves as "visible".
	m_command.Handle().endQuery(m_occlusion_pool, static_cast<uint32_t>(m_occlusion_open_slot));
	m_pending_occlusion.push_back({m_occlusion_begin_addr, m_occlusion_open_slot});
	m_occlusion_open_slot = -1;
}

void CommandScheduler::DrainOcclusionBeforeReuse() {
	if (m_pending_occlusion.empty() && m_occlusion_open_slot < 0) {
		return;
	}
	// Results must be read back before the pool is reset for the next recording. The producing
	// work is already submitted, so wait on the last submitted tick and resolve.
	if (CurrentTick() > 1) {
		m_master.Wait(CurrentTick() - 1);
	}
	ResolveOcclusion();
}

bool CommandScheduler::SampleOcclusion(uint64_t dst_addr) {
	if (dst_addr == 0) {
		return false;
	}
	if (!OcclusionQueriesTrusted()) {
		// KYTY_OCCLUSION_QUERIES=0: publish a "visible" result for every occlusion-tested
		// primitive (begin counter 0, end counter large) instead of leaving the guest to read
		// stale memory. Disables occlusion culling; never wrongly culls on-screen geometry.
		// begin/end dumps land 8 bytes apart on the same block. Use the safe guest writer --
		// a result block that is not mapped yet during loading must not fault.
		const bool     is_end = m_occlusion_begin_addr != 0 && dst_addr == m_occlusion_begin_addr + 8;
		const uint64_t value =
		    is_end ? (kOcclusionReadyBit | (uint64_t {1} << 40u)) : kOcclusionReadyBit;
		(void)Libs::LibKernel::Memory::TryWriteBacking(dst_addr, &value, sizeof(value));
		m_occlusion_begin_addr = is_end ? 0 : dst_addr;
		return true;
	}
	if (!Active() || m_command.IsInvalid()) {
		return false;
	}
	// beginQuery / endQuery must sit inside a render pass instance. The guest opens one before the
	// occlusion-tested draw and keeps it open across the pair, so in practice it is; anything else
	// falls back rather than guessing.
	if (!m_command.IsRendering() || !EnsureOcclusionPool() || m_occlusion_pool == nullptr) {
		return false;
	}

	const bool is_end = m_occlusion_open_slot >= 0 && dst_addr == m_occlusion_begin_addr + 8;
	if (is_end) {
		m_command.Handle().endQuery(m_occlusion_pool,
		                            static_cast<uint32_t>(m_occlusion_open_slot));
		m_pending_occlusion.push_back({m_occlusion_begin_addr, m_occlusion_open_slot});
		m_occlusion_open_slot = -1;
		// "Visible" placeholder until the real result is resolved, so an unresolved query never
		// reads back as occluded.
		reinterpret_cast<volatile uint64_t*>(m_occlusion_begin_addr + 8)[0] = kOcclusionReadyBit | 1u;
		return true;
	}

	// A still-open query with no matching end: close it defensively before starting a new one.
	if (m_occlusion_open_slot >= 0) {
		CloseOpenOcclusionQuery();
	}
	if (m_occlusion_next_slot >= OcclusionQuerySlots) {
		return false; // pool exhausted this recording
	}

	const auto slot = m_occlusion_next_slot++;
	m_command.Handle().beginQuery(m_occlusion_pool, slot,
	                              m_graphics.occlusion_query_precise_enabled
	                                  ? vk::QueryControlFlagBits::ePrecise
	                                  : vk::QueryControlFlags {});
	m_occlusion_open_slot  = static_cast<int32_t>(slot);
	m_occlusion_begin_addr = dst_addr;

	// Begin dump resolves to zero for every DB; the guest takes end-minus-begin.
	auto* begin_slots = reinterpret_cast<volatile uint64_t*>(dst_addr);
	for (uint32_t db = 0; db < 16u; db++) {
		begin_slots[db * 2u] = kOcclusionReadyBit;
	}
	return true;
}

void CommandScheduler::ResolveOcclusion() {
	if (m_pending_occlusion.empty()) {
		m_occlusion_open_slot = -1;
		return;
	}

	uint32_t max_slot = 0;
	for (const auto& p: m_pending_occlusion) {
		if (p.slot >= 0 && static_cast<uint32_t>(p.slot) + 1 > max_slot) {
			max_slot = static_cast<uint32_t>(p.slot) + 1;
		}
	}

	std::vector<uint64_t> results(max_slot, 1); // default "visible" on any readback failure
	if (m_occlusion_pool != nullptr && max_slot != 0) {
		const auto status = m_graphics.device.getQueryPoolResults(
		    m_occlusion_pool, 0, max_slot, results.size() * sizeof(uint64_t), results.data(),
		    sizeof(uint64_t), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
		if (status != vk::Result::eSuccess) {
			std::fill(results.begin(), results.end(), uint64_t {1});
		}
	}

	for (const auto& p: m_pending_occlusion) {
		uint64_t count = 0;
		if (p.slot >= 0 && static_cast<uint32_t>(p.slot) < results.size()) {
			count = results[static_cast<uint32_t>(p.slot)];
		}
		// Publish into the OcclusionQueryResults block: begin counters zero, DB0 end counter the
		// visible-sample count, other DBs zero. The guest sums end-minus-begin across DBs, so the
		// total it computes is `count`.
		auto* block = reinterpret_cast<volatile uint64_t*>(p.base_addr);
		for (uint32_t db = 0; db < 16u; db++) {
			block[db * 2u + 0u] = kOcclusionReadyBit;
			block[db * 2u + 1u] = kOcclusionReadyBit | ((db == 0u) ? count : uint64_t {0});
		}
	}
	m_pending_occlusion.clear();
	m_occlusion_open_slot = -1;
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
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
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

void CommandScheduler::FlushAndWait() {
	const auto tick = Submit();
	m_master.Wait(tick);
	ResolveOcclusion();
	BeginNext();
}

void CommandScheduler::Finish() {
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	ResolveOcclusion();
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
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

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
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
	ResetOcclusionPool(); // outside any render pass here; slot numbering restarts per recording
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

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

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer = nullptr;
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	// Read back any occlusion results still owed from the previous recording before BeginCommand
	// resets the pool and reuses slot 0 (FlushAndWait / Finish already resolved; this covers the
	// plain async Flush path).
	DrainOcclusionBeforeReuse();
	BeginCommand();
}

} // namespace Libs::Graphics
