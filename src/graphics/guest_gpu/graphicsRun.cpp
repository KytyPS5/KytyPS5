#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <utility>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics {

static thread_local CommandProcessor* g_current_processor = nullptr;
static thread_local Pm4Execution*     g_current_execution = nullptr;
static thread_local bool              g_gpu_thread        = false;
// The command processor thread of a pipelined GPU.
static thread_local bool              g_cp_thread         = false;
static thread_local GuestGpu*         g_gpu_state         = nullptr;

// Operations a pipelined command processor may queue ahead of their execution.
constexpr size_t OperationQueueSize = 4096;
// Resolved operations queued for the execution thread; see RenderExecutor::ResolvedDrawRing.
constexpr size_t ResolvedQueueSize = 256;
// How long a command processor whose queues all wait spins before it retries them.
constexpr auto BlockedRetryInterval = std::chrono::microseconds(20);

// Reads memory other threads may write, with the widest aligned accesses.
static void CopyFromGuest(void* destination, const volatile void* source, size_t size) {
	auto*      out     = static_cast<uint8_t*>(destination);
	const auto address = reinterpret_cast<uint64_t>(source);
	if ((address & 7u) == 0 && (size & 7u) == 0) {
		const auto* words = static_cast<const volatile uint64_t*>(source);
		for (size_t i = 0; i < size / 8u; i++) {
			const uint64_t word = words[i];
			std::memcpy(out + i * 8u, &word, 8u);
		}
		return;
	}
	if ((address & 3u) == 0 && (size & 3u) == 0) {
		const auto* words = static_cast<const volatile uint32_t*>(source);
		for (size_t i = 0; i < size / 4u; i++) {
			const uint32_t word = words[i];
			std::memcpy(out + i * 4u, &word, 4u);
		}
		return;
	}
	const auto* bytes = static_cast<const volatile uint8_t*>(source);
	for (size_t i = 0; i < size; i++) {
		out[i] = bytes[i];
	}
}

GuestGpu::GuestGpu(RenderContext& renderer)
    : m_renderer(renderer),
      m_pipelined((Config::GpuPipelineStages() & Config::GPU_PIPELINE_COMMAND_PROCESSOR) != 0),
      m_resolving(m_pipelined && (Config::GpuPipelineStages() & Config::GPU_PIPELINE_RESOLVE) != 0),
      m_operations(OperationQueueSize), m_resolved(ResolvedQueueSize),
      m_neutral_state {&m_neutral_context, &m_neutral_user_config, &m_neutral_shaders} {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_gfx_cp = std::make_unique<CommandProcessor>(renderer, 0);
	m_gfx_cp->Reset();
	if (m_pipelined) {
		m_pending_writes = std::make_unique<GuestWrite[]>(PendingWriteCapacity);
		m_gfx_cp->AttachPipeline(this);
		m_executor = std::jthread([this] { ExecutionThread(); });
		if (m_resolving) {
			m_resolver = std::jthread([this] { ResolveThread(); });
		}
	}
	m_thread = std::jthread(ThreadRun, this);
}

GuestGpu::~GuestGpu() {
	Shutdown();
}

void GuestGpu::Shutdown() {
	std::lock_guard shutdown_lock(m_shutdown_mutex);
	if (m_shutdown_complete) {
		return;
	}
	{
		Common::LockGuard lock(m_queue_mutex);
		m_accepting = false;
		m_stopping  = true;
		m_work_epoch.fetch_add(1, std::memory_order_release);
		m_work_available.SignalAll();
	}
	if (m_thread.joinable()) {
		m_thread.join();
	}
	m_shutdown_complete = true;
}

bool GuestGpu::IsStopping() {
	Common::LockGuard lock(m_queue_mutex);
	return m_stopping;
}

void GuestGpu::SendCommand(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return;
	}
	if (g_cp_thread) {
		m_gfx_cp->EmitHostCommand(std::move(command));
		return;
	}
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_commands.push_back(std::move(command));
	m_pending_commands.fetch_add(1, std::memory_order_release);
	m_work_epoch.fetch_add(1, std::memory_order_release);
	m_work_available.Signal();
}

void GuestGpu::ProcessCommands() {
	EXIT_IF(!IsGpuThread() && !g_cp_thread);
	while (m_pending_commands.load(std::memory_order_acquire) != 0) {
		Common::UniqueFunction<void> command;
		{
			Common::LockGuard lock(m_queue_mutex);
			EXIT_IF(m_commands.empty());
			command = std::move(m_commands.front());
			m_commands.pop_front();
			EXIT_IF(m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
		}
		if (m_pipelined) {
			// Host commands run on the execution thread, after the operations emitted so far.
			m_gfx_cp->EmitHostCommand(std::move(command));
		} else {
			command();
		}
	}
}

void GuestGpu::SendCommandSync(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return;
	}
	if (g_cp_thread) {
		// A readback for a page the command processor faulted on: the operations emitted so far
		// may write that memory.
		ExecuteSync(std::move(command));
		return;
	}
	std::binary_semaphore done {0};
	SendCommand([operation = std::move(command), &done]() mutable {
		operation();
		done.release();
	});
	done.acquire();
}

void GuestGpu::SendUrgentCommandSync(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (!m_pipelined || IsGpuThread() || g_cp_thread) {
		SendCommandSync(std::move(command));
		return;
	}
	std::binary_semaphore done {0};
	{
		std::lock_guard lock(m_urgent_mutex);
		m_urgent_commands.push_back([operation = std::move(command), &done]() mutable {
			operation();
			done.release();
		});
		m_urgent_pending.fetch_add(1, std::memory_order_release);
	}
	// A busy execution thread runs the command before its next operation; an idle one waits for
	// operations, so queue one that runs it too.
	SendCommand([this] { RunUrgentCommands(); });
	done.acquire();
}

void GuestGpu::RunUrgentCommands() {
	if (m_urgent_pending.load(std::memory_order_acquire) == 0) {
		return;
	}
	m_renderer.GetRenderExecutor().InvalidateRenderTargetMemo();
	// Unmaps free memory that copies of executed draws may still read.
	if (auto* copies = m_renderer.GetCommandScheduler().HostCopies(); copies != nullptr) {
		copies->Drain();
	}
	std::vector<Common::UniqueFunction<void>> commands;
	{
		std::lock_guard lock(m_urgent_mutex);
		commands.swap(m_urgent_commands);
		m_urgent_pending.store(0, std::memory_order_relaxed);
	}
	for (auto& command: commands) {
		command();
	}
}

void GuestGpu::Submit(std::span<const uint32_t> commands) {
	if (commands.empty()) {
		return;
	}
	Submission submission;
	submission.type     = SubmissionType::Commands;
	submission.queue_id = 0;
	submission.commands = commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitCompute(uint32_t queue, std::span<const uint32_t> commands) {
	EXIT_IF(commands.empty());

	EXIT_NOT_IMPLEMENTED(queue < ComputeQueueBase || queue >= ComputeQueueBase + ComputeQueueCount);

	const auto compute_queue = queue - ComputeQueueBase;
	Submission submission;
	submission.type     = SubmissionType::Commands;
	submission.queue_id = 1 + compute_queue;
	submission.commands = commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitFlipPreparation(uint64_t request_id) {
	Submission submission;
	submission.type            = SubmissionType::FlipPreparation;
	submission.queue_id        = 0;
	submission.flip_request_id = request_id;
	Enqueue(std::move(submission));
}

void GuestGpu::SuspendPoint() {
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	// Do not hold a queue lock while waiting: asynchronous work may be needed to
	// finish the preceding graphics frame. The first point returns immediately.
	m_suspend_point_ready->acquire();
	Submission submission;
	submission.type = SubmissionType::SuspendPoint;
	Enqueue(std::move(submission));
	m_done_num++;
}

int GuestGpu::GetFrameNum() const {
	return m_done_num;
}

CommandProcessor& GuestGpu::GetProcessor(uint32_t queue_id) {
	if (queue_id == 0) {
		return *m_gfx_cp;
	}
	auto& processor = m_compute_cp[queue_id - 1];
	if (processor == nullptr) {
		processor = std::make_unique<CommandProcessor>(m_renderer, ComputeQueueBase + queue_id - 1);
		if (m_pipelined) {
			processor->AttachPipeline(this);
		}
	}
	return *processor;
}

uint64_t GuestGpu::EmitOperation(GpuOperation&& operation) {
	const auto sequence = ++m_emitted;
	operation.sequence  = sequence;
	m_operations.Push(std::move(operation));
	return sequence;
}

void GuestGpu::ForgetExecutedWrites() {
	// Executed writes are in guest memory now.
	const auto executed = m_executed.Load();
	while (m_pending_count != 0 && m_pending_writes[m_pending_first].sequence <= executed) {
		m_pending_first = (m_pending_first + 1) % PendingWriteCapacity;
		m_pending_count--;
	}
}

void GuestGpu::NoteGuestWrite(uint64_t sequence, uint64_t address, uint64_t size,
                              const void* value) {
	ForgetExecutedWrites();
	if (sequence <= m_executed.Load()) {
		return;
	}
	if (m_pending_count == PendingWriteCapacity) {
		WaitForExecution(m_pending_writes[m_pending_first].sequence);
		ForgetExecutedWrites();
	}
	auto& write = m_pending_writes[(m_pending_first + m_pending_count) % PendingWriteCapacity];
	write       = {.sequence = sequence, .address = address, .size = size};
	if (value != nullptr && size <= sizeof(write.value)) {
		write.known = true;
		std::memcpy(write.value, value, size);
	}
	m_pending_count++;
}

void GuestGpu::ReadDecisionMemory(const volatile void* address, void* value, size_t size) {
	ForgetExecutedWrites();
	const auto begin = reinterpret_cast<uint64_t>(address);
	const auto end   = begin + size;
	// The newest pending write that overlaps the range decides; older ones execute before it.
	for (size_t i = m_pending_count; i-- > 0;) {
		const auto* write = &m_pending_writes[(m_pending_first + i) % PendingWriteCapacity];
		if (write->address >= end || write->address + write->size <= begin) {
			continue;
		}
		if (write->known && write->address <= begin && end <= write->address + write->size) {
			std::memcpy(value, write->value + (begin - write->address), size);
			return;
		}
		// The value is only known once the write executed; waiting acquires it.
		WaitForExecution(write->sequence);
		break;
	}
	CopyFromGuest(value, address, size);
}

void GuestGpu::ExecuteSync(Common::UniqueFunction<void>&& command) {
	m_gfx_cp->EmitHostCommand(std::move(command));
	WaitForExecution();
}

void GuestGpu::ExecutionThread() {
	KYTY_PROFILER_THREAD("Thread_GpuExecute");
	Config::ConfigureGpuStageThread(Config::GpuStageThread::Execution);
	g_gpu_thread = true;
	// Waking the command processor costs a fence: publish every operation, but only look for a
	// waiter now and then and before sleeping.
	constexpr uint32_t NotifyInterval = 16;
	uint32_t           unnotified     = 0;
	auto&              operations     = m_resolving ? m_resolved : m_operations;
	for (;;) {
		if (m_urgent_pending.load(std::memory_order_relaxed) != 0) {
			RunUrgentCommands();
		}
		auto operation = operations.TryPop();
		if (!operation) {
			m_executed.Notify();
			unnotified = 0;
			operation  = operations.Pop();
			if (!operation) {
				break;
			}
		}
		operation->processor->Execute(*operation);
		const auto sequence = operation->sequence;
		// Destroy the callback before the command processor may reuse what it references.
		operation.reset();
		m_executed.Publish(sequence);
		if (++unnotified >= NotifyInterval) {
			m_executed.Notify();
			unnotified = 0;
		}
	}
	m_executed.Notify();
	g_gpu_thread = false;
}

static bool IndirectDrawOffsets(IndirectDrawRegisters registers, ShaderIndirectOffsets& offsets);

void GuestGpu::ResolveThread() {
	KYTY_PROFILER_THREAD("Thread_GpuResolve");
	Config::ConfigureGpuStageThread(Config::GpuStageThread::Resolve);
	auto& executor = m_renderer.GetRenderExecutor();
	for (;;) {
		auto operation = m_operations.TryPop();
		if (!operation) {
			operation = m_operations.Pop();
			if (!operation) {
				break;
			}
		}
		if (operation->kind == GpuOperationKind::DrawIndex ||
		    operation->kind == GpuOperationKind::DrawAuto) {
			// The register snapshots stay valid until the operation executed.
			operation->resolved =
			    executor.ResolveDrawAhead(*operation->state.context, *operation->state.user_config,
			                              *operation->state.shaders);
		} else if (operation->kind == GpuOperationKind::DrawIndirect) {
			// Resolved for the offsets the GPU-read records give the shaders; draws that read
			// their records on the CPU patch the SGPRs instead and resolve themselves.
			ShaderIndirectOffsets offsets;
			if (IndirectDrawOffsets(operation->draw_indirect.registers, offsets)) {
				operation->resolved = executor.ResolveDrawAhead(
				    *operation->state.context, *operation->state.user_config,
				    *operation->state.shaders, offsets, true);
			}
		} else if (operation->kind == GpuOperationKind::DispatchDirect) {
			const uint32_t groups[] = {operation->dispatch.thread_group_x,
			                           operation->dispatch.thread_group_y,
			                           operation->dispatch.thread_group_z};
			operation->resolved = executor.ResolveDispatchAhead(
			    *operation->state.context, *operation->state.shaders, groups,
			    operation->dispatch.mode, operation->processor->IsAsyncComputeQueue());
		} else if (operation->kind == GpuOperationKind::DispatchIndirect) {
			// The program does not depend on the GPU-written arguments, only on the registers
			// and the guest memory its resources read.
			operation->resolved = executor.ResolveIndirectDispatchAhead(
			    *operation->state.context, *operation->state.shaders, operation->dispatch.mode,
			    operation->dispatch.args_addr,
			    operation->processor->IsAsyncComputeQueue());
		}
		m_resolved.Push(std::move(*operation));
	}
	m_resolved.Stop();
}

void GuestGpu::StopExecution() {
	WaitForExecution();
	m_operations.Stop();
	if (m_resolver.joinable()) {
		m_resolver.join();
	}
	if (m_executor.joinable()) {
		m_executor.join();
	}
}

void CommandProcessor::Reset() {
	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_ctx_dirty    = true;
	m_ucfg_dirty   = true;
	m_sh_ctx_dirty = true;
	m_saved_ctx.reset();
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_index_base_addr                  = 0;
	m_num_instances.value              = 1;
	m_num_instances.sequence++;
	m_predicate_skip                   = false;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;
}

// Pops a pushed context, then resets the register state the operation bits name.
void CommandProcessor::ResetQueue(uint32_t op) {
	if ((op & Pm4::ResetQueueOp::PopContext) != 0 && m_saved_ctx.has_value()) {
		ApplyContextStateOperation(ContextStateOperation::Pop);
	}
	if ((op & Pm4::ResetQueueOp::CxState) != 0) {
		m_ctx.Reset();
	}
	if ((op & Pm4::ResetQueueOp::ShState) != 0) {
		m_sh_ctx.Reset();
	}
	if ((op & Pm4::ResetQueueOp::UcState) != 0) {
		m_ucfg.Reset();
	}
}

void CommandProcessor::ApplyContextStateOperation(ContextStateOperation operation) {
	m_ctx_dirty = true;
	switch (operation) {
		case ContextStateOperation::Clear: m_ctx.Reset(); break;
		case ContextStateOperation::Push:
			EXIT_IF(m_saved_ctx.has_value());
			m_saved_ctx.emplace(m_ctx);
			break;
		case ContextStateOperation::Pop:
			EXIT_IF(!m_saved_ctx.has_value());
			m_ctx = *m_saved_ctx;
			m_saved_ctx.reset();
			break;
		case ContextStateOperation::PushClear:
			EXIT_IF(m_saved_ctx.has_value());
			m_saved_ctx.emplace(m_ctx);
			m_ctx.Reset();
			break;
	}
}

GpuOperation CommandProcessor::MakeOperation(GpuOperationKind kind) {
	GpuOperation operation;
	operation.kind          = kind;
	operation.processor     = this;
	operation.submit_id     = m_submit_id;
	operation.num_instances = m_num_instances;
	operation.after_wait    = std::exchange(m_wait_since_operation, false);
	if (m_pipeline == nullptr) {
		operation.state = {&m_ctx, &m_ucfg, &m_sh_ctx};
	} else if (kind == GpuOperationKind::Callback) {
		// Only draws and dispatches read registers.
		operation.state = m_pipeline->NeutralState();
	} else {
		operation.state = SnapshotState(m_pipeline->NextSequence());
	}
	return operation;
}

template <typename T>
const T* CommandProcessor::Snapshot(SnapshotRing<T>& ring, const T& live, bool& dirty,
                                    uint64_t sequence, uint64_t* id) {
	static_assert(std::is_trivially_copyable_v<T>);
	if (dirty || ring.current == SnapshotRing<T>::Size) {
		if (ring.slots == nullptr) {
			ring.slots = std::make_unique<typename SnapshotRing<T>::Slot[]>(SnapshotRing<T>::Size);
			ring.last_use = std::make_unique<uint64_t[]>(SnapshotRing<T>::Size);
		}
		auto& slot = ring.slots[ring.next];
		// The execution thread publishes the sequence of an operation after it stops reading the
		// operation's snapshots; the wait acquires that.
		m_pipeline->WaitForExecution(ring.last_use[ring.next]);
		slot.value   = live;
		slot.id      = s_snapshot_ids.fetch_add(1, std::memory_order_relaxed) + 1;
		ring.current = ring.next;
		ring.next    = (ring.next + 1) % SnapshotRing<T>::Size;
		dirty        = false;
	}
	// The operation built from this state is the next one the command processor emits.
	ring.last_use[ring.current] = sequence;
	if (id != nullptr) {
		*id = ring.slots[ring.current].id;
	}
	return &ring.slots[ring.current].value;
}

GpuRegisterState CommandProcessor::SnapshotState(uint64_t sequence) {
	GpuRegisterState state;
	state.context     = Snapshot(m_ctx_snapshots, m_ctx, m_ctx_dirty, sequence, &state.context_id);
	state.user_config = Snapshot(m_ucfg_snapshots, m_ucfg, m_ucfg_dirty, sequence);
	state.shaders     = Snapshot(m_sh_ctx_snapshots, m_sh_ctx, m_sh_ctx_dirty, sequence);
	return state;
}

uint64_t CommandProcessor::Emit(GpuOperation&& operation) {
	if (m_pipeline != nullptr) {
		EXIT_IF(operation.kind != GpuOperationKind::Callback &&
		        m_pipeline->NextSequence() !=
		            m_ctx_snapshots.last_use[m_ctx_snapshots.current]);
		return m_pipeline->EmitOperation(std::move(operation));
	}
	Execute(operation);
	return 0;
}

void CommandProcessor::NoteGuestWrite(uint64_t sequence, uint64_t address, uint64_t size,
                                      const void* value) {
	if (m_pipeline != nullptr && size != 0) {
		m_pipeline->NoteGuestWrite(sequence, address, size, value);
	}
}

void CommandProcessor::EmitHostCommand(Common::UniqueFunction<void>&& command) {
	EmitCallback(std::move(command));
}

void CommandProcessor::ReadDecisionMemory(const volatile void* address, void* value,
                                          size_t size) {
	if (m_pipeline != nullptr) {
		m_pipeline->ReadDecisionMemory(address, value, size);
		return;
	}
	CopyFromGuest(value, address, size);
}

uint64_t CommandProcessor::EmitCallback(GpuCallback&& callback) {
	auto operation     = MakeOperation(GpuOperationKind::Callback);
	operation.callback = std::move(callback);
	return Emit(std::move(operation));
}

void CommandProcessor::Execute(GpuOperation& operation) {
	EXIT_IF(operation.processor != this);
	BindState(operation.state);
	auto& executor = m_renderer.GetRenderExecutor();
	executor.UseContextId(operation.state.context_id);
	if (operation.after_wait) {
		executor.InvalidateRenderTargetMemo();
	}
	if (operation.kind != GpuOperationKind::DrawIndex &&
	    operation.kind != GpuOperationKind::DrawAuto &&
	    operation.kind != GpuOperationKind::DrawIndirect) {
		executor.InvalidateRenderTargetMemo();
		// Guest-visible effects (labels, interrupts, flips, host commands) let the guest overwrite
		// what earlier draws read: their copies complete first.
		if (auto* copies = GetScheduler().HostCopies(); copies != nullptr) {
			copies->Drain();
		}
	}
	struct CommitWriteTicks {
		RenderContext& renderer;
		~CommitWriteTicks() {
			if (renderer.GetBufferCache().CommitWriteTicks()) {
				renderer.GetCommandScheduler().Flush();
			}
		}
	} commit_write_ticks {m_renderer};
	switch (operation.kind) {
		case GpuOperationKind::DrawIndex:
		case GpuOperationKind::DrawAuto: {
			CountDraw();
			executor.UseResolvedDraw(operation.resolved);
			if (operation.kind == GpuOperationKind::DrawIndex) {
				ExecuteDrawIndex(operation, operation.draw_index);
			} else {
				ExecuteDrawAuto(operation, operation.draw_auto);
			}
			executor.UseResolvedDraw(nullptr);
			if (operation.resolved != nullptr) {
				RenderExecutor::ReleaseResolvedDraw(operation.resolved);
			}
			break;
		}
		case GpuOperationKind::DrawIndirect:
			CountDraw();
			// The resolution serves the first draw of the packet (its programs do not depend on
			// the arguments); later draws resolve their own.
			executor.UseResolvedDraw(operation.resolved);
			ExecuteDrawIndirect(operation);
			executor.UseResolvedDraw(nullptr);
			if (operation.resolved != nullptr) {
				RenderExecutor::ReleaseResolvedDraw(operation.resolved);
			}
			break;
		case GpuOperationKind::DispatchDirect: {
			const auto& dispatch = operation.dispatch;
			executor.UseResolvedDraw(operation.resolved);
			ExecuteDispatchDirect(operation, dispatch.thread_group_x, dispatch.thread_group_y,
			                      dispatch.thread_group_z, dispatch.mode);
			executor.UseResolvedDraw(nullptr);
			if (operation.resolved != nullptr) {
				RenderExecutor::ReleaseResolvedDraw(operation.resolved);
			}
			break;
		}
		case GpuOperationKind::DispatchIndirect:
			executor.UseResolvedDraw(operation.resolved);
			ExecuteDispatchIndirect(operation);
			executor.UseResolvedDraw(nullptr);
			if (operation.resolved != nullptr) {
				RenderExecutor::ReleaseResolvedDraw(operation.resolved);
			}
			break;
		case GpuOperationKind::Callback:
			if (operation.callback) {
				operation.callback();
			}
			break;
	}
	// Starts the copies of the operation while the next ones are prepared.
	if (auto* copies = GetScheduler().HostCopies(); copies != nullptr) {
		(void)copies->Flush();
	}
}

void CommandProcessor::BindState(const GpuRegisterState& state) {
	GetScheduler().Begin(*state.context, *state.user_config, *state.shaders);
}

void CommandProcessor::BufferInit() {
	// Executing any operation binds its register state and begins a command buffer.
	if (m_pipeline == nullptr) {
		Emit(MakeOperation(GpuOperationKind::Callback));
	}
}

void CommandProcessor::BufferFlush() {
	EmitCallback([this] { GetScheduler().Flush(); });
}

void CommandProcessor::NoteRecordedWork() {
	const auto tick = GetScheduler().CurrentTick();
	if (tick != m_batch_tick) {
		m_batch_tick = tick;
		m_batch_work = 0;
	}
	m_batch_work++;
}

void CommandProcessor::BufferFlushIfBatchReady() {
	// Label writes reach guest memory when the packet is executed, and resources are captured
	// when a draw is recorded, so submitting here only starts the GPU earlier. A submission per
	// label costs a queue submit and a new command buffer, so wait for a batch of work.
	EmitCallback([this] {
		constexpr uint32_t MinBatchWork = 64;
		if (GetScheduler().CurrentTick() == m_batch_tick && m_batch_work >= MinBatchWork) {
			GetScheduler().Flush();
		}
	});
}

void CommandProcessor::BufferWait() {
	EmitCallback([this] { GetScheduler().Finish(); });
}

void CommandProcessor::RunGarbageCollector() {
	EmitCallback([this] { m_renderer.RunGarbageCollector(); });
}

void CommandProcessor::WaitForRewind(bool valid) {
	if (!valid) {
		SuspendPm4();
	}
}

bool TestWaitRegMemValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func) {
	switch (func) {
		case 0: return true;
		case 1: return (value & mask) < ref;
		case 2: return (value & mask) <= ref;
		case 3: return (value & mask) == ref;
		case 4: return (value & mask) != ref;
		case 5: return (value & mask) >= ref;
		case 6: return (value & mask) > ref;
		default: EXIT("unknown wait compare function: %" PRIu32 "\n", func);
	}

	return false;
}

template <typename T>
void CommandProcessor::WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t wait_op) {
	EXIT_IF(addr == nullptr);
	if ((wait_op & ~1u) != 0) {
		EXIT("unsupported wait_reg_mem operation: 0x%08" PRIx32 "\n", wait_op);
	}

	m_wait_since_operation = true;
	if (!TestWaitRegMemValue(ReadDecisionValue<T>(addr), ref, mask, func)) {
		SuspendPm4();
	}
}

template void CommandProcessor::WaitRegMem<uint32_t>(uint32_t, const uint32_t*, uint32_t, uint32_t,
                                                     uint32_t);
template void CommandProcessor::WaitRegMem<uint64_t>(uint32_t, const uint64_t*, uint64_t, uint64_t,
                                                     uint32_t);

void CommandProcessor::MemSemaphore(uint64_t* counter, bool signal, bool write) {
	if (signal) {
		*counter = write ? 1u : *counter + 1u;
	} else if (*counter == 0) {
		SuspendPm4();
	} else {
		(*counter)--;
	}
}

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num,
                                 uint32_t write_control) {
	const uint32_t dst_sel = ((write_control >> 30u) & 0x1u) | ((write_control >> 7u) & 0x1eu);
	const bool     write_one_address = ((write_control >> 16u) & 0x1u) != 0;

	switch (dst_sel) {
		case 0:
		case 2:
		case 4:
		case 5:
		case 6: break;
		default: EXIT("unsupported writeData destination selector 0x%02" PRIx32 "\n", dst_sel);
	}
	if (dw_num == 0) {
		return;
	}

	// The source dwords are in the command buffer, which stays valid until the guest sees a
	// later label.
	// Completion labels can be reused immediately; memcpy may write a destination twice, so
	// publish each word exactly once.
	const auto sequence = EmitCallback([this, dst, src, dw_num, write_one_address] {
		const auto* data  = write_one_address ? src + dw_num - 1 : src;
		const auto  bytes = uint64_t {write_one_address ? 1u : dw_num} * sizeof(uint32_t);
		if (m_renderer.WriteAroundGpuWrites(dst, data, bytes)) {
			return;
		}
		m_renderer.PrepareGpuWrite(dst, bytes);
		auto* output = static_cast<volatile uint32_t*>(dst);
		for (uint32_t i = 0; i < dw_num; i++) {
			*output = src[i];
			output += !write_one_address;
		}
	});
	if (write_one_address) {
		NoteGuestWrite(sequence, reinterpret_cast<uint64_t>(dst), sizeof(uint32_t),
		               src + dw_num - 1);
	} else {
		NoteGuestWrite(sequence, reinterpret_cast<uint64_t>(dst),
		               uint64_t {dw_num} * sizeof(uint32_t), src);
	}
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	const auto sequence = EmitCallback([dst_address, num_bytes] {
		const auto value = Sync::ReadReferenceClock();
		std::memcpy(reinterpret_cast<void*>(dst_address), &value, num_bytes);
		static std::atomic<uint32_t> clock_log_count {0};
		if (clock_log_count.fetch_add(1) < 64) {
			LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64
			     " size=%u\n",
			     dst_address, value, num_bytes);
		}
	});
	NoteGuestWrite(sequence, dst_address, num_bytes);
}

void CommandProcessor::GetLodStats(volatile uint32_t* dst, uint32_t size, bool reset) {
	auto& counters = m_renderer.GetRenderExecutor().m_mip_stats;
	// Each of up to 256 2-dword slots after the 16-dword header holds the clamped sample count
	// (dword 0 bits 0-23), the slot id (bits 24-31), the unclamped sample count (dword 1 bits 0-23)
	// and the finest mip sampled (bits 24-27, 15 for none). A non-zero header dword 0 then marks
	// the report complete.
	if (dst != nullptr && size >= 64) {
		const auto slots = std::min((size - 64) / 8, 256u);
		for (uint32_t slot = 0; slot < slots; slot++) {
			const auto& counter = counters[slot];
			dst[16 + slot * 2]  = (slot << 24u) | uint32_t {counter.clamped};
			dst[16 + slot * 2 + 1] =
			    (uint32_t {counter.finest_level} << 24u) | uint32_t {counter.unclamped};
		}
		dst[0] = 1;
	}
	if (reset) {
		counters = {};
	}
}

void CommandProcessor::DmaData(uint8_t dst_sel, uint64_t dst_address_or_offset, uint8_t src_sel,
                               uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes) {
	if (num_bytes == 0 || static_cast<uint32_t>(dst_address_or_offset) == 0x3022cu) {
		return;
	}
	auto decode_gds = [](uint8_t selector, bool& is_gds) {
		switch (selector) {
			case 0:
			case 3: is_gds = false; return true;
			case 1: is_gds = true; return true;
			default: return false;
		}
	};
	if (dst_sel == 2) {
		// kNowhere discards the GL2 prefetch destination without a guest-visible write.
		if (src_sel != 3) {
			EXIT("unsupported dmaData nowhere source selector 0x%02" PRIx8 "\n", src_sel);
		}
		return;
	}
	bool dst_gds = false;
	if (!decode_gds(dst_sel, dst_gds)) {
		EXIT("unsupported dmaData destination selector 0x%02" PRIx8 "\n", dst_sel);
	}
	bool src_gds = false;
	if (src_sel != 2) {
		if (!decode_gds(src_sel, src_gds)) {
			EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
		}
		if (src_gds && dst_gds) {
			EXIT("unsupported dmaData GDS-to-GDS copy\n");
		}
	}
	const auto sequence = EmitCallback([=, this] {
		ExecuteDmaData(dst_gds, dst_address_or_offset, src_sel, src_gds,
		               src_address_or_offset_or_immediate, num_bytes);
	});
	if (!dst_gds) {
		NoteGuestWrite(sequence, dst_address_or_offset, num_bytes);
	}
}

void CommandProcessor::ExecuteDmaData(bool dst_gds, uint64_t dst_address_or_offset,
                                      uint8_t src_sel, bool src_gds,
                                      uint64_t src_address_or_offset_or_immediate,
                                      uint32_t num_bytes) {
	auto& buffer_cache = m_renderer.GetBufferCache();
	if (src_sel == 2) {
		buffer_cache.FillBuffer(
		    dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu), dst_gds);
		return;
	}
	if (!src_gds) {
		// The copy reads occlusion counters once the GPU wrote them, as later work does.
		m_renderer.GetOcclusionQueries().WaitForCounters(src_address_or_offset_or_immediate,
		                                                 num_bytes);
	}
	buffer_cache.CopyBuffer(dst_address_or_offset, src_address_or_offset_or_immediate, num_bytes,
	                        dst_gds, src_gds);
}

void GuestGpu::Enqueue(Submission submission) {
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_queues[submission.queue_id].push_back(std::move(submission));
	m_submission_count++;
	m_work_epoch.fetch_add(1, std::memory_order_release);
	m_work_available.Signal();
}

void GuestGpu::WaitForIdle() {
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	Common::LockGuard lock(m_queue_mutex);
	while (m_processing || !m_commands.empty() || m_submission_count != 0) {
		m_idle.Wait(&m_queue_mutex);
	}
}

void GuestGpu::ThreadRun(void* data) {
	auto* gpu = static_cast<GuestGpu*>(data);
	EXIT_IF(gpu == nullptr);
	KYTY_PROFILER_THREAD("Thread_Gpu");
	g_gpu_thread = !gpu->m_pipelined;
	g_cp_thread  = gpu->m_pipelined;
	g_gpu_state  = gpu;
	if (gpu->m_pipelined) {
		Config::ConfigureGpuStageThread(Config::GpuStageThread::CommandProcessor);
	}

	// When every queue waits (WAIT_REG_MEM), a pipelined command processor retries without
	// sleeping until the spin budget is spent.
	std::optional<std::chrono::steady_clock::time_point> blocked_since;
	bool                                                  spin_blocked = false;

	for (;;) {
		Submission                   submission;
		Common::UniqueFunction<void> command;
		bool                         has_submission = false;
		bool                         should_stop    = false;
		if (spin_blocked) {
			spin_blocked      = false;
			const auto epoch  = gpu->m_work_epoch.load(std::memory_order_acquire);
			const auto target = std::chrono::steady_clock::now() + BlockedRetryInterval;
			while (gpu->m_work_epoch.load(std::memory_order_acquire) == epoch &&
			       std::chrono::steady_clock::now() < target) {
				Common::SpinPause();
			}
		}
		if (gpu->m_pipelined) {
			// The GPU is idle once the execution thread ran every operation too. Only this
			// thread emits operations, so none can follow the wait until it finds work.
			bool     no_work = false;
			uint64_t epoch   = 0;
			{
				Common::LockGuard lock(gpu->m_queue_mutex);
				no_work = gpu->m_commands.empty() && gpu->m_submission_count == 0 &&
				          !gpu->m_stopping;
			}
			if (no_work) {
				gpu->WaitForExecution();
				{
					Common::LockGuard lock(gpu->m_queue_mutex);
					epoch = gpu->m_work_epoch.load(std::memory_order_acquire);
					if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
						gpu->m_processing = false;
						gpu->m_idle.SignalAll();
					}
				}
				// Spin for the next submission; sleep below only when none came.
				Common::SpinWait spin;
				while (gpu->m_work_epoch.load(std::memory_order_acquire) == epoch && spin.Spin()) {
				}
			}
		}
		{
			Common::LockGuard lock(gpu->m_queue_mutex);
			while (gpu->m_commands.empty() && gpu->m_submission_count == 0 && !gpu->m_stopping) {
				gpu->m_processing = false;
				gpu->m_idle.Signal();
				gpu->m_work_available.Wait(&gpu->m_queue_mutex);
			}
			if (gpu->m_stopping && gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_processing = false;
				gpu->m_idle.SignalAll();
				should_stop = true;
			} else if (!gpu->m_commands.empty()) {
				command = std::move(gpu->m_commands.front());
				gpu->m_commands.pop_front();
				EXIT_IF(gpu->m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
				gpu->m_processing = true;
			} else {
				int selected_queue = -1;
				for (uint32_t offset = 0; offset < QueueCount; offset++) {
					const auto id = (gpu->m_next_queue + offset) % QueueCount;
					if (!gpu->m_queues[id].empty() && !gpu->m_queues[id].front().blocked) {
						selected_queue = static_cast<int>(id);
						break;
					}
				}
				if (selected_queue < 0) {
					gpu->m_processing = false;
					const auto now    = std::chrono::steady_clock::now();
					if (gpu->m_pipelined && !blocked_since) {
						blocked_since = now;
					}
					if (gpu->m_pipelined && now - *blocked_since < Common::SpinBudget()) {
						spin_blocked = true;
					} else {
						gpu->m_work_available.WaitFor(&gpu->m_queue_mutex, 100);
					}
					for (auto& queue: gpu->m_queues) {
						if (!queue.empty()) {
							queue.front().blocked = false;
						}
					}
					continue;
				}
				auto& queue = gpu->m_queues[static_cast<uint32_t>(selected_queue)];
				submission  = std::move(queue.front());
				queue.pop_front();
				gpu->m_submission_count--;
				gpu->m_next_queue = (static_cast<uint32_t>(selected_queue) + 1) % QueueCount;
				gpu->m_processing = true;
			}
		}
		if (should_stop) {
			gpu->m_gfx_cp->BufferWait();
			if (gpu->m_pipelined) {
				gpu->StopExecution();
			}
			g_gpu_state  = nullptr;
			g_gpu_thread = false;
			g_cp_thread  = false;
			return;
		}

		if (command) {
			EXIT_IF(g_current_processor != nullptr);
			if (gpu->m_pipelined) {
				// Stays processing until the command executed; see the idle wait above.
				gpu->m_gfx_cp->EmitHostCommand(std::move(command));
				continue;
			}
			{
				Common::DebugCounters::ScopedTimer busy(
				    Common::DebugCounters::Counter::GpuThreadBusyNs);
				command();
			}

			Common::LockGuard lock(gpu->m_queue_mutex);
			gpu->m_processing = false;
			if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_idle.SignalAll();
			}
			continue;
		}

		const bool complete = [&] {
			Common::DebugCounters::ScopedTimer busy(
			    Common::DebugCounters::Counter::GpuThreadBusyNs);
			return gpu->Process(submission);
		}();
		if (complete) {
			blocked_since.reset();
		}

		Common::LockGuard lock(gpu->m_queue_mutex);
		if (!complete) {
			submission.blocked = true;
			gpu->m_queues[submission.queue_id].push_front(std::move(submission));
			gpu->m_submission_count++;
		} else {
			for (auto& queue: gpu->m_queues) {
				if (!queue.empty()) {
					queue.front().blocked = false;
				}
			}
		}
		if (gpu->m_pipelined) {
			// Stays processing until the operations executed; see the idle wait above.
			continue;
		}
		gpu->m_processing = false;
		if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
			gpu->m_idle.SignalAll();
		}
	}
}

bool GuestGpu::Process(Submission& submission) {
	auto& cp = GetProcessor(submission.queue_id);

	cp.BufferInit();
	bool complete = true;

	switch (submission.type) {
		case SubmissionType::Commands:
			complete =
			    cp.Process(submission.execution, submission.commands) == Pm4ProcessResult::Complete;
			if (submission.execution.MadeProgress()) {
				cp.BufferFlush();
			}
			break;
		case SubmissionType::FlipPreparation:
			cp.PrepareCpuFlip(submission.flip_request_id);
			break;
		case SubmissionType::SuspendPoint:
			// Age caches at the frame boundary, not once per command list.
			cp.RunGarbageCollector();
			// Registered at the operation's place in the stream: a pipelined command processor
			// runs ahead of the recorded commands. Releasing the slot only touches host state.
			cp.EmitHostCommand([this, ready = m_suspend_point_ready] {
				m_renderer.GetCommandScheduler().DeferHostOperation([ready] { ready->release(); },
				                                                    true);
			});
			cp.BufferFlush();
			cp.Reset();
			break;
	}

	return complete;
}

Pm4ProcessResult CommandProcessor::Process(Pm4Execution&             execution,
                                           std::span<const uint32_t> commands) {
	KYTY_PROFILER_BLOCK("CommandProcessor::Process");
	EXIT_IF(g_current_execution != nullptr);
	EXIT_IF(commands.size() > UINT32_MAX);
	if (execution.m_buffer_stack.empty() && !commands.empty()) {
		execution.m_buffer_stack.push_back({commands});
	}
	execution.m_suspended     = false;
	execution.m_made_progress = false;

	struct ExecutionScope {
		explicit ExecutionScope(Pm4Execution& execution) { g_current_execution = &execution; }
		~ExecutionScope() { g_current_execution = nullptr; }
	} execution_scope(execution);

	ProcessPm4(execution);
	return execution.m_buffer_stack.empty() ? Pm4ProcessResult::Complete
	                                        : Pm4ProcessResult::Blocked;
}

void CommandProcessor::ProcessIndirectBuffer(std::span<const uint32_t> commands, bool chain) {
	EXIT_IF(g_current_execution == nullptr);
	EXIT_IF(!g_current_execution->m_next_buffer.empty());
	g_current_execution->m_next_buffer = commands;
	g_current_execution->m_chain       = chain;
}

void CommandProcessor::SuspendPm4() {
	EXIT_IF(g_current_execution == nullptr);
	g_current_execution->m_suspended = true;
}

void CommandProcessor::ProcessPm4(Pm4Execution& execution) {
	const bool debug_dump = graphics_debug_dump_enabled();
	while (!execution.m_buffer_stack.empty()) {
		if (g_gpu_state != nullptr) {
			g_gpu_state->ProcessCommands();
		}
		auto& cursor = execution.m_buffer_stack.back();
		if (cursor.offset_dw == cursor.commands.size()) {
			execution.m_buffer_stack.pop_back();
			continue;
		}

		const auto* const packet        = cursor.commands.data() + cursor.offset_dw;
		const auto        total_dw      = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining_dw  = total_dw - cursor.offset_dw;
		const auto        packet_header = packet[0];
		const auto        opcode        = (packet_header >> 8u) & 0xffu;

		if (packet_header == 0x80000000u || Pm4::PacketSizeDw(packet_header) == 1u) {
			cursor.offset_dw++;
			execution.m_made_progress = true;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(remaining_dw < 2);

		if (debug_dump) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     cursor.offset_dw, packet_header, opcode, KYTY_PM4_LEN(packet_header));
		}

		if ((packet_header & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			auto packet_dw = KYTY_PM4_LEN(packet_header);
			EXIT_NOT_IMPLEMENTED(packet_dw > remaining_dw);
			cursor.offset_dw += packet_dw;
			execution.m_made_progress = true;
			continue;
		}

		auto handler = g_cp_op_func[opcode];

		if (handler == nullptr) {
			const auto offset = cursor.offset_dw;
			LOGF("unknown PM4 packet: data=0x%016" PRIx64 ", num_dw=%" PRIu32
			     ", offset=0x%05" PRIx32 ", current=0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(packet - offset), total_dw, offset,
			     reinterpret_cast<uint64_t>(packet));
			const auto  dump_begin = (offset > 8 ? offset - 8 : 0);
			const auto  dump_end   = std::min<uint32_t>(total_dw, offset + 16);
			auto* const base       = packet - offset;
			for (uint32_t i = dump_begin; i < dump_end; i++) {
				LOGF("\t%05" PRIx32 "%s %08" PRIx32 "\n", i, (i == offset ? ":" : " "), base[i]);
			}
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n", offset, packet_header);
		}

		const auto packet_dw =
		    handler(*this, packet_header & ~1u, packet + 1, remaining_dw, total_dw) + 1;
		EXIT_IF(packet_dw > remaining_dw);
		if (execution.m_suspended) {
			return;
		}
		cursor.offset_dw += packet_dw;
		execution.m_made_progress = true;
		if (!execution.m_next_buffer.empty()) {
			// Chains and taken branches reuse the fetcher; only calls retain a return cursor.
			if (execution.m_chain) {
				cursor = {execution.m_next_buffer};
			} else {
				execution.m_buffer_stack.push_back({execution.m_next_buffer});
			}
			execution.m_next_buffer = {};
		}
	}
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size) {
	m_index_type_and_size = index_type_and_size & 0x3u;
}

void CommandProcessor::SetIndexBaseAddress(uint64_t index_base_addr) {
	m_index_base_addr = index_base_addr;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_buffer_size) {
	m_index_buffer_size = index_buffer_size;
}

void CommandProcessor::SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr) {
	m_draw_indirect_args_base_addr = draw_indirect_args_base_addr;
}

void CommandProcessor::SetDispatchIndirectArgsBaseAddress(
    uint64_t dispatch_indirect_args_base_addr) {
	m_dispatch_indirect_args_base_addr = dispatch_indirect_args_base_addr;
}

void CommandProcessor::SetNumInstances(uint32_t num_instances) {
	if (num_instances == 0) {
		num_instances = 1;
	}

	m_num_instances.value = num_instances;
	m_num_instances.sequence++;
}

uint32_t CommandProcessor::ResolveNumInstances(const GpuOperation& operation) {
	// An indirect draw executed after the last NUM_INSTANCES packet of the operation's stream
	// position set the persistent count.
	if (m_has_indirect_instances &&
	    m_indirect_instances_sequence == operation.num_instances.sequence) {
		const auto& gpu = m_gpu_indirect_instances;
		if (gpu.address != 0) {
			// The GPU read the records: read the last draw's instance count when a later draw
			// inherits it (the second dword of both record layouts), waiting for the GPU work that
			// wrote it.
			uint32_t count = gpu.count;
			if (gpu.count_address != 0) {
				const uint32_t written =
				    *reinterpret_cast<const volatile uint32_t*>(gpu.count_address);
				count = std::min(written, count);
			}
			m_indirect_instances =
			    count != 0 ? *reinterpret_cast<const volatile uint32_t*>(
			                     gpu.address + uint64_t {count - 1} * gpu.stride +
			                     offsetof(vk::DrawIndirectCommand, instanceCount))
			               : m_indirect_instances;
			m_gpu_indirect_instances = {};
		}
		return m_indirect_instances;
	}
	return operation.num_instances.value;
}

void CommandProcessor::SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
                                      const volatile void* address) {
	uint64_t value = 0;

	switch (op) {
		case 0x00:
			m_predicate_skip = false;
			return;
		case 0x01: {
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			// One begin/end pair per DB; bit 63 marks each counter ready.
			constexpr uint64_t      ready_bit = 1ull << 63u;
			std::array<uint64_t, 32> results {};
			ReadDecisionMemory(address, results.data(), sizeof(results));
			for (uint32_t db = 0; db < 16u; db++) {
				const auto begin = results[db * 2u];
				const auto end   = results[db * 2u + 1u];
				if ((begin & end & ready_bit) == 0) {
					if (wait_op == 0) {
						// Host query results reach guest memory from the execution thread once
						// the GPU finished them: make sure it does and they are published.
						if (!m_occlusion_wait_requested) {
							m_occlusion_wait_requested = true;
							EmitCallback([this] {
								auto& queries = m_renderer.GetOcclusionQueries();
								if (queries.HasPendingDumps()) {
									GetScheduler().Finish();
									queries.PublishCompleted(true);
								}
							});
						}
						SuspendPm4();
					} else {
						m_predicate_skip = false;
					}
					return;
				}
				value += end - begin;
			}
			m_occlusion_wait_requested = false;
		} break;
		case 0x03:
			// The wait selector applies only to Z-pass query readiness.
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			value = ReadDecisionValue<uint64_t>(address);
			break;
		default: EXIT("unknown predication op: 0x%08" PRIx32 "\n", op);
	}
	switch (condition) {
		case 0x00: m_predicate_skip = (value != 0); break;
		case 0x01: m_predicate_skip = (value == 0); break;
		default: EXIT("unknown predication condition: 0x%08" PRIx32 "\n", condition);
	}
}

// Submits the recorded work every N draws so the host GPU starts before a long buffer ends. The
// execution thread counts the draws it records into each command buffer.
void CommandProcessor::CountDraw() {
	if (m_flush_draws == 0) {
		return;
	}
	if (const auto tick = GetScheduler().CurrentTick(); tick != m_flush_tick) {
		m_flush_tick = tick;
		m_draws      = 0;
	}
	if (m_draws == m_flush_draws) {
		GetScheduler().Flush();
		m_flush_tick = GetScheduler().CurrentTick();
		m_draws      = 0;
	}
	m_draws++;
}

void CommandProcessor::DrawIndex(DrawIndexArgs args) {
	args.index_type_and_size = m_index_type_and_size;
	auto operation       = MakeOperation(GpuOperationKind::DrawIndex);
	operation.draw_index = args;
	Emit(std::move(operation));
}

void CommandProcessor::ExecuteDrawIndex(const GpuOperation& operation, DrawIndexArgs args) {
	NoteRecordedWork();
	if (args.instance_count == 0) {
		args.instance_count = ResolveNumInstances(operation);
	}
	m_renderer.GetRenderExecutor().DrawIndex(CurrentBuffer(), args);
}

const void* CommandProcessor::IndexAddress(uint64_t base, uint32_t type_and_size,
                                           uint32_t first_index) {
	uint64_t index_size = 0;
	switch (type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", type_and_size);
	}
	return reinterpret_cast<const void*>(base + first_index * index_size);
}

const void* CommandProcessor::IndexAddress(uint32_t first_index) const {
	return IndexAddress(m_index_base_addr, m_index_type_and_size, first_index);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count) {
	DrawIndex({.index_count = index_count, .index_addr = IndexAddress(index_offset)});
}

// Writes the GS user-data slots the packet patches; false when the shaders cannot read the
// offsets from GPU records: a first-index or subdraw-index destination, or another stage's
// user data.
static bool IndirectDrawOffsets(IndirectDrawRegisters registers, ShaderIndirectOffsets& offsets) {
	if (registers.index_offset != Pm4::SH_NOP || registers.draw_index != Pm4::SH_NOP) {
		return false;
	}
	for (const auto [location, slot]: {std::pair {registers.vertex_offset, &offsets.vertex},
	                                   std::pair {registers.instance_offset, &offsets.instance}}) {
		if (location == Pm4::SH_NOP) {
			continue;
		}
		const uint32_t index = location - Pm4::SPI_SHADER_USER_DATA_GS_0;
		if (index >= HW::UserSgprInfo::SGPRS_MAX) {
			return false;
		}
		*slot = static_cast<uint8_t>(index);
	}
	return true;
}

void CommandProcessor::DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
                                         const volatile uint32_t* count_addr,
                                         uint32_t stride_in_bytes, IndirectDrawRegisters registers,
                                         uint32_t draw_initiator, bool indexed) {
	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != (indexed ? 0u : 2u));
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);
	// The guest records match Vulkan's indirect commands byte for byte.
	EXIT_NOT_IMPLEMENTED((stride_in_bytes & 3u) != 0 ||
	                     stride_in_bytes < (indexed ? sizeof(vk::DrawIndexedIndirectCommand)
	                                                : sizeof(vk::DrawIndirectCommand)));

	auto operation          = MakeOperation(GpuOperationKind::DrawIndirect);
	operation.draw_indirect = {.args_base         = m_draw_indirect_args_base_addr,
	                           .data_offset       = data_offset,
	                           .max_count         = max_count_or_count,
	                           .count_addr        = count_addr,
	                           .stride            = stride_in_bytes,
	                           .draw_initiator    = draw_initiator,
	                           .index_type        = m_index_type_and_size,
	                           .index_buffer_size = m_index_buffer_size,
	                           .index_base        = m_index_base_addr,
	                           .indexed           = indexed,
	                           .multi             = true,
	                           .registers         = registers};
	Emit(std::move(operation));
}

static bool PatchUserSgpr(HW::Shader& shaders, uint32_t location, uint32_t value) {
	const auto in  = [location](uint32_t base) { return location >= base && location < base + 32u; };
	const auto set = [value](const HW::UserSgprInfo& sgprs, uint32_t id, auto&& setter) {
		if (sgprs.value[id] == value) {
			return false;
		}
		setter(id, value);
		return true;
	};
	if (in(Pm4::SPI_SHADER_USER_DATA_GS_0)) {
		return set(shaders.GetVs().gs_user_sgpr, location - Pm4::SPI_SHADER_USER_DATA_GS_0,
		           [&](uint32_t id, uint32_t v) { shaders.SetGsUserSgpr(id, v); });
	}
	if (in(Pm4::SPI_SHADER_USER_DATA_HS_0)) {
		return set(shaders.GetVs().hs_user_sgpr, location - Pm4::SPI_SHADER_USER_DATA_HS_0,
		           [&](uint32_t id, uint32_t v) { shaders.SetHsUserSgpr(id, v); });
	}
	if (in(Pm4::SPI_SHADER_USER_DATA_PS_0)) {
		return set(shaders.GetPs().ps_user_sgpr, location - Pm4::SPI_SHADER_USER_DATA_PS_0,
		           [&](uint32_t id, uint32_t v) { shaders.SetPsUserSgpr(id, v); });
	}
	EXIT("unsupported indirect draw offset register 0x%" PRIx32 "\n", location);
	return false;
}

// Writes the offsets of one draw of an indirect packet to the user SGPRs the packet names, as
// the CP does before the draw. Without a pipeline the registers are the live ones. A pipelined
// operation carries a register snapshot shared with other operations: a copy is patched and
// bound for this draw, and the ahead resolution (made from the unpatched registers) is dropped
// when a value changed. Returns whether the packet names any register.
bool CommandProcessor::PatchIndirectDrawOffsets(const GpuOperation& operation,
                                                uint32_t vertex_offset, uint32_t instance_offset,
                                                uint32_t first_index, uint32_t draw_index) {
	const auto& registers = operation.draw_indirect.registers;
	if (registers.vertex_offset == Pm4::SH_NOP && registers.instance_offset == Pm4::SH_NOP &&
	    registers.index_offset == Pm4::SH_NOP && registers.draw_index == Pm4::SH_NOP) {
		return false;
	}
	const std::pair<uint32_t, uint32_t> writes[] = {{registers.vertex_offset, vertex_offset},
	                                                {registers.instance_offset, instance_offset},
	                                                {registers.index_offset, first_index},
	                                                {registers.draw_index, draw_index}};
	if (m_pipeline == nullptr) {
		for (const auto& [location, value]: writes) {
			if (location == Pm4::SH_NOP) {
				continue;
			}
			EXIT_NOT_IMPLEMENTED(location >= Pm4::SH_NUM ||
			                     g_hw_sh_indirect_func[location] == nullptr);
			g_hw_sh_indirect_func[location](*this, location, value);
		}
		return true;
	}
	if (m_indirect_shaders == nullptr) {
		m_indirect_shaders = std::make_unique<HW::Shader>();
	}
	*m_indirect_shaders = *operation.state.shaders;
	bool changed        = false;
	for (const auto& [location, value]: writes) {
		if (location != Pm4::SH_NOP) {
			changed |= PatchUserSgpr(*m_indirect_shaders, location, value);
		}
	}
	if (changed) {
		m_renderer.GetRenderExecutor().UseResolvedDraw(nullptr);
	}
	GetScheduler().Begin(*operation.state.context, *operation.state.user_config,
	                     *m_indirect_shaders);
	return true;
}

// The arguments are read when the draw executes: earlier GPU work may produce them.
void CommandProcessor::ExecuteDrawIndirect(const GpuOperation& operation) {
	const auto& indirect = operation.draw_indirect;
	if (indirect.max_count == 0) {
		return;
	}
	const auto  args_addr = indirect.args_base + indirect.data_offset;
	const auto& registers = indirect.registers;
	// The GPU reads the records if the shaders allow; the VS prolog removes Vulkan's bases
	// from the index VGPRs and copies them only to the user SGPRs the packet patches. GPU work
	// (culling shaders) writes the records and the draw count: the host GPU reads the count too
	// (vkCmdDraw*IndirectCount), since reading it here would wait for all the GPU work recorded
	// so far. Like any GPU-read record, the count path draws with indirect_draw shaders.
	const auto count_address = reinterpret_cast<uint64_t>(indirect.count_addr);
	DrawIndirectArgs records {args_addr, indirect.max_count, indirect.stride};
	records.count_address = count_address;
	if ((count_address == 0 || (m_renderer.GetGraphics().indirect_draws_enabled &&
	                            (count_address & 3u) == 0)) &&
	    IndirectDrawOffsets(registers, records.offsets) &&
	    (!indirect.indexed || indirect.index_buffer_size != 0)) {
		// NUM_INSTANCES keeps the last draw's instance count.
		m_gpu_indirect_instances      = records;
		m_indirect_instances_sequence = operation.num_instances.sequence;
		m_has_indirect_instances      = true;
		NoteRecordedWork();
		auto& executor = m_renderer.GetRenderExecutor();
		if (indirect.indexed
		        ? executor.DrawIndex(CurrentBuffer(),
		                             {.index_count         = indirect.index_buffer_size,
		                              .index_addr          = IndexAddress(indirect.index_base,
		                                                                  indirect.index_type, 0),
		                              .index_type_and_size = indirect.index_type,
		                              .indirect            = records})
		        : executor.DrawAuto(CurrentBuffer(), {.indirect = records})) {
			Common::DebugCounters::Add(Common::DebugCounters::Counter::IndirectDraws,
			                           indirect.max_count);
			return;
		}
	}

	uint32_t draw_count = indirect.max_count;
	if (indirect.count_addr != nullptr) {
		draw_count = *indirect.count_addr;
		if (draw_count > indirect.max_count) {
			draw_count = indirect.max_count;
		}
	}
	m_gpu_indirect_instances = {};
	if (draw_count == 0) {
		return;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::IndirectDraws, draw_count);

	const uint32_t max_indices = indirect.index_buffer_size != 0 ? indirect.index_buffer_size : UINT32_MAX;
	for (uint32_t i = 0; i < draw_count; i++) {
		const auto* record_addr = reinterpret_cast<const void*>(
		    args_addr + static_cast<uint64_t>(i) * indirect.stride);

		if (!indirect.indexed) {
			vk::DrawIndirectCommand args {};
			std::memcpy(&args, record_addr, sizeof(args));
			m_indirect_instances          = args.instanceCount;
			m_indirect_instances_sequence = operation.num_instances.sequence;
			m_has_indirect_instances      = true;
			PatchIndirectDrawOffsets(operation, args.firstVertex, args.firstInstance, 0, i);
			ExecuteDrawAuto(operation, {.vertex_count   = args.vertexCount,
			                            .instance_count = args.instanceCount,
			                            .first_vertex   = args.firstVertex,
			                            .first_instance = args.firstInstance,
			                            .offset_source  = DrawOffsetSource::IndirectArgs});
			continue;
		}

		vk::DrawIndexedIndirectCommand args {};
		std::memcpy(&args, record_addr, sizeof(args));
		PatchIndirectDrawOffsets(operation, static_cast<uint32_t>(args.vertexOffset),
		                         args.firstInstance, args.firstIndex, i);

		const auto*    index_addr = IndexAddress(indirect.index_base, indirect.index_type, args.firstIndex);
		const uint32_t index_count = std::min(args.indexCount, max_indices);
		m_indirect_instances          = args.instanceCount;
		m_indirect_instances_sequence = operation.num_instances.sequence;
		m_has_indirect_instances      = true;
		ExecuteDrawIndex(operation, {.index_count         = index_count,
		                             .index_addr          = index_addr,
		                             .instance_count      = args.instanceCount,
		                             .index_type_and_size = indirect.index_type,
		                             .base_vertex    = args.vertexOffset,
		                             .first_instance = args.firstInstance,
		                             .offset_source  = DrawOffsetSource::IndirectArgs});
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
	GetShCtx().SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	auto operation     = MakeOperation(GpuOperationKind::DispatchDirect);
	operation.dispatch = {.thread_group_x = thread_group_x,
	                      .thread_group_y = thread_group_y,
	                      .thread_group_z = thread_group_z,
	                      .mode           = mode};
	Emit(std::move(operation));
}

void CommandProcessor::ExecuteDispatchDirect(const GpuOperation& operation, uint32_t thread_group_x,
                                             uint32_t thread_group_y, uint32_t thread_group_z,
                                             uint32_t mode) {
	NoteRecordedWork();

	m_renderer.GetRenderExecutor().DispatchDirect(CurrentBuffer(), thread_group_x,
	                                              thread_group_y, thread_group_z, mode,
	                                              IsAsyncComputeQueue());
}

void CommandProcessor::ProcessPssr(const Pssr::Request& request) {
	EXIT_NOT_IMPLEMENTED(IsAsyncComputeQueue());
	// It records into the command buffer of the operations around it, where they execute.
	EmitHostCommand([this, request] {
		m_renderer.GetRenderExecutor().ProcessPssr(CurrentBuffer(), request);
	});
}

void CommandProcessor::DispatchIndirect(uint64_t args_addr, uint32_t mode) {
	EXIT_NOT_IMPLEMENTED(args_addr == 0 || (args_addr & 3u) != 0);
	GetShCtx().SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	auto operation     = MakeOperation(GpuOperationKind::DispatchIndirect);
	operation.dispatch = {.mode = mode, .args_addr = args_addr};
	Emit(std::move(operation));
}

void CommandProcessor::ExecuteDispatchIndirect(const GpuOperation& operation) {
	const auto& dispatch = operation.dispatch;
	NoteRecordedWork();
	if ((dispatch.mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0) {
		// Earlier GPU work writes the thread counts: reading them here would wait for it.
		m_renderer.GetRenderExecutor().DispatchIndirectThreads(CurrentBuffer(), dispatch.args_addr,
		                                                       dispatch.mode,
		                                                       IsAsyncComputeQueue());
		return;
	}
	m_renderer.GetRenderExecutor().DispatchIndirect(CurrentBuffer(),
	                                                dispatch.args_addr, dispatch.mode,
	                                                IsAsyncComputeQueue());
}

void CommandProcessor::DrawIndexAuto(DrawAutoArgs args) {
	auto operation      = MakeOperation(GpuOperationKind::DrawAuto);
	operation.draw_auto = args;
	Emit(std::move(operation));
}

void CommandProcessor::ExecuteDrawAuto(const GpuOperation& operation, DrawAutoArgs args) {
	NoteRecordedWork();
	if (args.instance_count == 0) {
		args.instance_count = ResolveNumInstances(operation);
	}
	m_renderer.GetRenderExecutor().DrawAuto(CurrentBuffer(), args);
}

void CommandProcessor::WaitFlipDone(const uint64_t* label) {
	// Flips before this packet that the execution thread has not run yet can still make the
	// buffer busy, so the execution thread waits, in operation order, until no flip of the buffer
	// is pending (upstream's wait, without the displayed-buffer condition it reverted).
	EmitCallback([this, label] {
		GetScheduler().Flush();
		m_renderer.GetVideoOut().WaitFlipDone(label);
	});
}

void CommandProcessor::ReleaseMem(uint32_t data_sel, uint32_t interrupt_selector, void* address,
                                  uint64_t data, uint32_t interrupt_context_id,
                                  bool gl2_writeback) {
	if (graphics_debug_dump_enabled()) {
		LOGF("CommandProcessor::ReleaseMem()\n"
		     "\t data_sel           = %" PRIu32 "\n"
		     "\t interrupt_selector = %" PRIu32 "\n"
		     "\t address            = 0x%016" PRIx64 "\n"
		     "\t data               = 0x%016" PRIx64 "\n"
		     "\t interrupt_context  = 0x%08" PRIx32 "\n",
		     data_sel, interrupt_selector, reinterpret_cast<uint64_t>(address), data,
		     interrupt_context_id);
	}

	// QueuedInterrupt: kNone, kOnly, kAfterWriteConfirm, kDataAfterWriteConfirm, kSendIntCtx.
	EXIT_NOT_IMPLEMENTED(interrupt_selector > 4);
	const bool interrupt = (interrupt_selector != 0 && interrupt_selector != 3);
	// kSendIntCtx has no label, and kOnly writes none on graphics queues.
	if (address == nullptr || interrupt_selector == 4 ||
	    (interrupt_selector == 1 && !IsAsyncComputeQueue())) {
		data_sel = 0;
	}
	// A GL2 writeback makes GPU writes CPU-visible even without a signal.
	if (gl2_writeback) {
		WriteBackImages();
	}
	if (data_sel == 0 && !interrupt) {
		return;
	}

	const auto sequence = EmitCallback([=, this] {
		ExecuteReleaseMem(data_sel, interrupt, address, data, interrupt_context_id);
	});
	// What ExecuteReleaseMem() writes: the value, or the clock and GDS dwords, known only then.
	const auto destination = reinterpret_cast<uint64_t>(address);
	switch (data_sel) {
		case 1: {
			const auto value = static_cast<uint32_t>(data);
			NoteGuestWrite(sequence, destination, sizeof(value), &value);
			break;
		}
		case 2: NoteGuestWrite(sequence, destination, sizeof(data), &data); break;
		case 3: NoteGuestWrite(sequence, destination, sizeof(data)); break;
		case 5:
			NoteGuestWrite(sequence, destination,
			               uint64_t {static_cast<uint32_t>(data) >> 16u} * sizeof(uint32_t));
			break;
		default: break;
	}
}

void CommandProcessor::ExecuteReleaseMem(uint32_t data_sel, bool interrupt, void* address,
                                         uint64_t data, uint32_t interrupt_context_id) {
	(void)CurrentBuffer().Handle();

	// A label beside GPU-written bytes goes to the backing store, so they are not downloaded.
	// Other labels go through the guest mapping: its fault keeps a clean cached copy current and
	// downloads GPU-written bytes under the label before the store.
	const auto write_label = [this, address](const void* value, uint64_t size) {
		const auto vaddr   = reinterpret_cast<uint64_t>(address);
		auto&      buffers = m_renderer.GetBufferCache();
		if (!GuestRange {vaddr, size}.Valid() || !buffers.IsRegionGpuModified(vaddr, size) ||
		    buffers.HasGpuDirtyBytes(vaddr, size) ||
		    !LibKernel::Memory::TryWriteBacking(vaddr, value, size)) {
			m_renderer.PrepareGpuWrite(address, size);
			std::memcpy(address, value, size);
		}
	};
	// Occlusion counters reach guest memory in a priority completion once the GPU got past
	// their dump. A label released after a dump tells the guest that the counters are there: GTA
	// San Andreas DE writes one into every query object after its end dump and reads the query
	// once it is set. On the console the label lands after the counters, so it goes behind them
	// through the same completions instead of being written now. Labels keep their order: while
	// a deferred label is not written yet, later ones follow it through the completions too.
	// EOP interrupts always go through the priority completions, so they stay in order as well.
	if (data_sel != 0 && (m_renderer.GetOcclusionQueries().HasPendingDumps() ||
	                      m_deferred_labels.load(std::memory_order_acquire) != 0)) {
		const auto     vaddr = reinterpret_cast<uint64_t>(address);
		const uint64_t size  = data_sel == 1 ? sizeof(uint32_t) : sizeof(uint64_t);
		if (data_sel <= 3 && GuestRange {vaddr, size}.Valid() &&
		    !m_renderer.GetBufferCache().IsRegionGpuModified(vaddr, size)) {
			m_renderer.PrepareGpuWrite(address, size);
			m_deferred_labels.fetch_add(1, std::memory_order_relaxed);
			GetScheduler().DeferPriorityOperation(
			    [this, address, data, data_sel] {
				    // Later GPU work may own the page again by now (ASTRO BOT, Sky Garden).
				    if (data_sel == 1) {
					    const auto value = static_cast<uint32_t>(data);
					    m_renderer.WriteFromCompletion(address, &value, sizeof(value));
				    } else {
					    const uint64_t value = data_sel == 3 ? Sync::ReadReferenceClock() : data;
					    m_renderer.WriteFromCompletion(address, &value, sizeof(value));
				    }
				    m_deferred_labels.fetch_sub(1, std::memory_order_release);
			    },
			    vaddr, size);
			if (interrupt) {
				DeferEopInterrupt(interrupt_context_id);
			}
			return;
		}
		// A label that cannot be deferred (GPU-written memory, GDS) behind deferred ones waits
		// until the GPU and the completions queued before it, those labels included, have run.
		if (m_deferred_labels.load(std::memory_order_acquire) != 0) {
			auto&      scheduler = GetScheduler();
			const auto tick      = scheduler.CurrentTick();
			scheduler.Wait(tick);
			scheduler.WaitPriorityOperations(tick);
		}
	}
	// EndOfPipeData: kNone, kImmediate32b, kImmediate64b, kRefClk; 5 reads GDS.
	switch (data_sel) {
		case 0: break;
		case 1: {
			const auto value = static_cast<uint32_t>(data);
			write_label(&value, sizeof(value));
			break;
		}
		case 3: data = Sync::ReadReferenceClock(); [[fallthrough]];
		case 2: write_label(&data, sizeof(data)); break;
		case 5: {
			GetScheduler().Finish();
			const auto gds = static_cast<uint32_t>(data);
			Sync::ReadGds(*m_renderer.GetBufferCache().GetGdsBuffer(),
			              static_cast<uint32_t*>(address), gds & 0xffffu, gds >> 16u);
			break;
		}
		default: EXIT("unknown release_mem data selector: %" PRIu32 "\n", data_sel);
	}

	// GuestGpu::Process's end-of-slice flush submits the work this interrupt waits on.
	if (interrupt) {
		DeferEopInterrupt(interrupt_context_id);
	}
}

void CommandProcessor::WriteBackImages() {
	// Only storage readback queues images to write back; without it the operation would do nothing.
	if (!m_renderer.GetTextureCache().StorageReadbackEnabled()) {
		return;
	}
	// GPU-written images are copied back without holding any signal: a held signal exposes the host
	// GPU's speed. Guest accesses to them wait for the copies. The texture cache belongs to the
	// execution thread, which runs this in operation order.
	EmitCallback([this] {
		if (m_renderer.GetTextureCache().WriteBackImages()) {
			GetScheduler().Flush();
		}
	});
}

void CommandProcessor::DeferEopInterrupt(uint32_t interrupt_context_id) {
	(void)CurrentBuffer().Handle();
	GetScheduler().DeferHostOperation(
	    [&renderer = m_renderer, event_id = m_interrupt_event_id, interrupt_context_id] {
		    renderer.TriggerInterrupt(event_id, interrupt_context_id);
	    },
	    true);
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index,
                                    uint64_t event_address) {
	if (graphics_debug_dump_enabled()) {
		LOGF("CommandProcessor::TriggerEvent()\n"
		     "\t event_type  = 0x%08" PRIx32 "\n"
		     "\t event_index = 0x%08" PRIx32 "\n"
		     "\t address     = 0x%016" PRIx64 "\n",
		     event_type, event_index, event_address);
	}

	const auto sequence = EmitCallback([this, event_type, event_index, event_address] {
		ExecuteTriggerEvent(event_type, event_index, event_address);
	});
	if (event_type == 0x00000039) {
		// Occlusion counter pairs of 16 DBs.
		NoteGuestWrite(sequence, event_address, 16u * 2u * sizeof(uint64_t));
	}
}

void CommandProcessor::ExecuteTriggerEvent(uint32_t event_type, uint32_t event_index,
                                           uint64_t event_address) {
	const auto valid_cache_event_index = event_index == 0x00000000 || event_index == 0x00000007;
	// Partial flushes and cache writebacks only order GPU work, which the renderer's resource
	// barriers already order.
	switch (event_type) {
		// CsPartialFlush, GsPartialFlush, PsPartialFlush.
		case 0x00000007:
		case 0x0000000f:
		case 0x00000010: break;
		// CbDbDataWritebackInvalidate, CbDataWritebackInvalidate, DbDataWritebackInvalidate,
		// DbMetadataWritebackInvalidate, CbMetadataWritebackInvalidate.
		case 0x00000016:
		case 0x00000031:
		case 0x0000002a:
		case 0x0000002c:
		case 0x0000002e:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			break;
		case 0x0000000d:
		case 0x0000000e:
		case 0x00000012:
		case 0x00000017:
		case 0x00000018:
		case 0x00000019:
		case 0x0000001a:
		case 0x0000001b:
		case 0x00000038:
		case 0x0000003a:
			LOGF("\t temporary: ignoring unsupported event_write type 0x%08" PRIx32
			     ", index 0x%08" PRIx32 "\n",
			     event_type, event_index);
			break;
		case 0x00000039: {
			if (event_index != 0x00000001 || event_address == 0 || (event_address & 0x7u) != 0) {
				EXIT("invalid occlusion-counter dump: index=0x%08" PRIx32 ", address=0x%016" PRIx64
				     "\n",
				     event_index, event_address);
			}
			auto& queries = m_renderer.GetOcclusionQueries();
			if (queries.Enabled()) {
				queries.Dump(GetScheduler(), event_address);
				break;
			}
			static std::once_flag warning_once;
			std::call_once(warning_once, [] {
				std::printf("Warning: game uses occlusion queries, which are treated as always "
				            "visible without --occlusion-queries; GPU usage may be higher and FPS "
				            "may be lower.\n");
			});

			// Without host occlusion queries, publish an always-visible result. The PS5 layout
			// contains one interleaved begin/end pair per DB, and bit 63 marks a result ready.
			constexpr uint64_t ready_bit    = 1ull << 63u;
			constexpr uint64_t counter_mask = ready_bit - 1u;
			auto*              results      = reinterpret_cast<volatile uint64_t*>(event_address);
			const auto         value        = ready_bit | m_synthetic_occlusion_counter;
			for (uint32_t db = 0; db < 16u; db++) {
				results[db * 2u] = value;
			}
			m_synthetic_occlusion_counter = (m_synthetic_occlusion_counter + 1u) & counter_mask;
			break;
		}
		default:
			EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

void CommandProcessor::WriteLodStats(void* dst, uint32_t size) {
	if (size < 64) {
		return;
	}
	// Mip statistics are not collected: each of up to 256 2-dword slots after the 16-dword header
	// reports its id (dword 0 bits 24-31) and finest sampled mip 15 (dword 1 bits 24-27), i.e. no
	// samples. A non-zero dword 0 then marks the report complete.
	const auto slots    = std::min((size - 64) / 8, 256u);
	const auto sequence = EmitCallback([dst, slots] {
		auto* report = static_cast<volatile uint32_t*>(dst);
		for (uint32_t slot = 0; slot < slots; slot++) {
			report[16 + slot * 2]     = slot << 24u;
			report[16 + slot * 2 + 1] = 15u << 24u;
		}
		report[0] = 1;
	});
	NoteGuestWrite(sequence, reinterpret_cast<uint64_t>(dst), 64 + slots * 8);
}

void CommandProcessor::Flip(int handle, int index, int flip_mode, int64_t flip_arg) {
	if (graphics_debug_dump_enabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}

	EmitCallback([this, handle, index, flip_mode, flip_arg] {
		auto& command = CurrentBuffer();
		m_renderer.GetOcclusionQueries().OnFrame();
		auto  request = Sync::PrepareVideoOutFlip(command, handle, index, flip_mode, flip_arg);
		GetScheduler().DeferHostOperation(
		    [&renderer = m_renderer, request] { renderer.GetVideoOut().CompleteFlip(request); },
		    true);
		GetScheduler().Flush();
	});
}

void CommandProcessor::PrepareCpuFlip(uint64_t request_id) {
	if (g_current_processor != nullptr) {
		EXIT("invalid graphics-thread CPU flip preparation\n");
	}
	EmitCallback([this, request_id] {
		struct ProcessorScope {
			explicit ProcessorScope(CommandProcessor& processor) {
				g_current_processor = &processor;
			}
			~ProcessorScope() { g_current_processor = nullptr; }
		};
		ProcessorScope processor_scope(*this);

		m_renderer.GetVideoOut().PrepareFlip(request_id, CurrentBuffer());
		GetScheduler().DeferHostOperation(
		    [this, request_id] { m_renderer.GetVideoOut().CompleteFlip(request_id); }, true);
		GetScheduler().Flush();
	});
}

bool GuestGpu::IsGpuThread() noexcept {
	return g_gpu_thread;
}

} // namespace Libs::Graphics
