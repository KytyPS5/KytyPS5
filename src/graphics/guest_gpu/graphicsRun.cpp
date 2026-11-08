#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/perfCounters.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/trace.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/computePrescan.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/errno.h"
#include "loader/timer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <tuple>
#include <vector>

namespace Libs::Graphics {

static thread_local CommandProcessor* g_current_processor = nullptr;
static thread_local Pm4Execution*     g_current_execution = nullptr;
static thread_local bool              g_gpu_thread        = false;
struct CurrentSubmission {
	std::span<const uint32_t>              commands;
	uint64_t                               hash         = 0;
	uint64_t                               submitted_ms = 0;
	std::shared_ptr<std::vector<uint32_t>> snapshot;
};
static thread_local CurrentSubmission g_current_submission;

static uint64_t HashCommands(std::span<const uint32_t> commands) {
	uint64_t hash = commands.size();
	for (const auto dword: commands) {
		hash = Common::Fnv64Step(hash, dword);
	}
	return hash;
}

// KYTY_TRACE_DCB=1: when each command buffer is submitted, started, finished and first changed by
// the guest (stderr, in ms).
namespace {

struct DcbTrace {
	uint64_t        id;
	const uint32_t* data;
	size_t          size;
	uint64_t        hash;
	uint64_t        submitted;
	int             state    = 0; // 0 queued, 1 started, 2 finished
	bool            reported = false;
};

bool TraceDcbEnabled() {
	static const bool enabled = std::getenv("KYTY_TRACE_DCB") != nullptr;
	return enabled;
}

std::mutex                             g_dcb_trace_mutex;
std::vector<std::unique_ptr<DcbTrace>> g_dcb_traces;
uint64_t                               g_dcb_trace_next   = 1;
thread_local uint64_t                  g_trace_current_id = 0;

uint64_t TraceNowMs() {
	return static_cast<uint64_t>(Loader::Timer::GetTimeMs());
}

void DcbTraceMonitor() {
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::lock_guard lock(g_dcb_trace_mutex);
		for (auto& t: g_dcb_traces) {
			if (t->reported || t->state == 2) {
				continue;
			}
			if (HashCommands({t->data, t->size}) != t->hash) {
				t->reported = true;
				fprintf(stderr,
				        "[dcb] #%" PRIu64 " (%zu dw at 0x%" PRIx64
				        ") CHANGED by the guest at %" PRIu64 " ms: %s, submitted %" PRIu64
				        " ms ago\n",
				        t->id, t->size, reinterpret_cast<uint64_t>(t->data), TraceNowMs(),
				        t->state == 0 ? "not started" : "started, not finished",
				        TraceNowMs() - t->submitted);
			}
		}
	}
}

uint64_t TraceDcbSubmitted(std::span<const uint32_t> commands, uint64_t hash) {
	static std::once_flag once;
	std::call_once(once, [] {
		Common::DebugCounters::SetEnabled(true);
		std::thread(DcbTraceMonitor).detach();
	});
	std::lock_guard lock(g_dcb_trace_mutex);
	const auto      id = g_dcb_trace_next++;
	g_dcb_traces.push_back(std::make_unique<DcbTrace>(
	    DcbTrace {id, commands.data(), commands.size(), hash, TraceNowMs()}));
	fprintf(stderr, "[dcb] #%" PRIu64 " submitted: %zu dw at 0x%" PRIx64 " t=%" PRIu64 " ms\n", id,
	        commands.size(), reinterpret_cast<uint64_t>(commands.data()), TraceNowMs());
	return id;
}

void TraceDcbState(uint64_t id, int state) {
	std::lock_guard lock(g_dcb_trace_mutex);
	for (auto& t: g_dcb_traces) {
		if (t->id == id) {
			t->state = state;
			using C  = Common::DebugCounters::Counter;
			fprintf(stderr,
			        "[dcb] #%" PRIu64 " %s t=%" PRIu64 " ms (so far: texture %" PRIu64
			        " MB, buffer %" PRIu64 " MB, gpu waits %" PRIu64 " for %" PRIu64
			        " ms, draws %" PRIu64 ", dispatches %" PRIu64 ")\n",
			        id, state == 1 ? "started" : "finished", TraceNowMs(),
			        Common::DebugCounters::Get(C::TextureUploadBytes) >> 20u,
			        Common::DebugCounters::Get(C::BufferUploadBytes) >> 20u,
			        Common::DebugCounters::Get(C::GpuWaits),
			        Common::DebugCounters::Get(C::GpuWaitNs) / 1000000u,
			        Common::DebugCounters::Get(C::Draws),
			        Common::DebugCounters::Get(C::Dispatches));
			if (state == 2) {
				// Keep the list short: finished buffers are not watched.
			}
			return;
		}
	}
}

} // namespace

static thread_local GuestGpu* g_gpu_state = nullptr;

GuestGpu::GuestGpu(RenderContext& renderer)
    : m_renderer(renderer),
      m_frame_pacer(std::make_shared<FramePacer>(Config::GetGpuFramesInFlight())) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_gfx_cp = std::make_unique<CommandProcessor>(renderer, 0);
	m_gfx_cp->Reset();
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
		m_work_available.SignalAll();
	}
	m_frame_pacer->Stop();
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
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_commands.push_back(std::move(command));
	m_pending_commands.fetch_add(1, std::memory_order_release);
	m_work_available.Signal();
}

void GuestGpu::SubmitRecordedBeforeWaiting() {
	auto& scheduler = m_renderer.GetCommandScheduler();
	if (scheduler.CurrentTick() == m_recorded_tick) {
		scheduler.SubmitIfDue(CommandScheduler::SubmitPoint::Wait);
	}
}

void GuestGpu::ProcessCommands() {
	EXIT_IF(!IsGpuThread());
	if (m_pending_commands.load(std::memory_order_acquire) != 0) {
		// A command may unmap or read back memory the pending work uses: it is recorded first.
		m_renderer.GetRenderExecutor().FlushPendingWork();
	}
	while (m_pending_commands.load(std::memory_order_acquire) != 0) {
		Common::UniqueFunction<void> command;
		{
			Common::LockGuard lock(m_queue_mutex);
			EXIT_IF(m_commands.empty());
			command = std::move(m_commands.front());
			m_commands.pop_front();
			EXIT_IF(m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
		}
		command();
	}
}

void GuestGpu::SendCommandSync(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return;
	}
	std::binary_semaphore done {0};
	SendCommand([operation = std::move(command), &done]() mutable {
		operation();
		done.release();
	});
	done.acquire();
}

bool GuestGpu::TrySendCommandSync(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return true;
	}
	std::binary_semaphore done {0};
	{
		Common::LockGuard lock(m_queue_mutex);
		if (!m_accepting) {
			return false;
		}
		m_commands.push_back([operation = std::move(command), &done]() mutable {
			operation();
			done.release();
		});
		m_pending_commands.fetch_add(1, std::memory_order_release);
		m_work_available.Signal();
	}
	done.acquire();
	return true;
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
	// finish the preceding graphics frame.
	m_frame_pacer->WaitForRoom();
	Submission submission;
	submission.type = SubmissionType::SuspendPoint;
	Enqueue(std::move(submission));
	m_done_num++;
}

std::shared_ptr<FramePacer> GuestGpu::RecordFrame() {
	m_frame_pacer->FrameRecorded();
	return m_frame_pacer;
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
	}
	return *processor;
}

void CommandProcessor::Reset() {
	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_saved_ctx.reset();
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_index_base_addr                  = 0;
	m_num_instances                    = 1;
	m_num_instances_addr               = 0;
	m_predicate_skip                   = false;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;
}

void CommandProcessor::ApplyContextStateOperation(ContextStateOperation operation) {
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

void CommandProcessor::BufferInit() {
	GetScheduler().Begin(m_ctx, m_ucfg, m_sh_ctx);
}

void CommandProcessor::BufferFlush() {
	GetScheduler().Flush();
	// The occlusion counters reach a guest that polls them without waiting for a later fence.
	m_renderer.GetOcclusionQueries().Publish();
}

void CommandProcessor::BufferFlushIfDue() {
	GetScheduler().SubmitIfDue(CommandScheduler::SubmitPoint::GuestSubmission);
}

void CommandProcessor::BufferWait() {
	BufferInit();
	GetScheduler().Finish();
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

	if (!TestWaitRegMemValue(ReadMemory(addr), ref, mask, func)) {
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

	// Completion labels can be reused immediately; memcpy may write a destination twice.
	auto* output = static_cast<volatile uint32_t*>(dst);
	for (uint32_t i = 0; i < dw_num; i++) {
		*output = src[i];
		output += !write_one_address;
	}
	NoteGuestWrite(dst, static_cast<uint64_t>(write_one_address ? 1u : dw_num) * sizeof(uint32_t));
}

void CommandProcessor::WriteGuest(void* dst, const void* src, uint64_t size) {
	std::memcpy(dst, src, size);
	NoteGuestWrite(dst, size);
}

void CommandProcessor::NoteGuestWrite(const void* dst, uint64_t size) {
	m_renderer.GetBufferCache().RecordGpuThreadWrite(reinterpret_cast<uint64_t>(dst), size);
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	const auto value = Sync::ReadReferenceClock();
	WriteGuest(reinterpret_cast<void*>(dst_address), &value, num_bytes);
	static std::atomic<uint32_t> clock_log_count {0};
	if (clock_log_count.fetch_add(1) < 64) {
		LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64
		     " size=%u\n",
		     dst_address, value, num_bytes);
	}
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
		NoteGuestWrite(const_cast<const uint32_t*>(dst), size);
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
	auto& buffer_cache = m_renderer.GetBufferCache();
	auto& executor     = m_renderer.GetRenderExecutor();
	// Behind the draws and dispatches before it, which read and write the same memory.
	const auto run = [&](auto action) {
		if (executor.HasPendingWork()) {
			executor.QueueDeferred(std::move(action));
		} else {
			action();
		}
	};
	if (src_sel == 2) {
		run([&buffer_cache, dst_address_or_offset, num_bytes, dst_gds,
		     value = static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu)] {
			buffer_cache.FillBuffer(dst_address_or_offset, num_bytes, value, dst_gds);
		});
		return;
	}
	bool src_gds = false;
	if (!decode_gds(src_sel, src_gds)) {
		EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
	}
	if (src_gds && dst_gds) {
		EXIT("unsupported dmaData GDS-to-GDS copy\n");
	}
	run([&buffer_cache, dst_address_or_offset, src = src_address_or_offset_or_immediate, num_bytes,
	     dst_gds, src_gds] {
		buffer_cache.CopyBuffer(dst_address_or_offset, src, num_bytes, dst_gds, src_gds);
	});
}

void GuestGpu::Enqueue(Submission submission) {
	EXIT_IF(submission.queue_id >= QueueCount);
	submission.commands_hash = HashCommands(submission.commands);
	submission.submitted_ms  = static_cast<uint64_t>(Loader::Timer::GetTimeMs());
	if (TraceDcbEnabled() && submission.type != SubmissionType::SuspendPoint &&
	    submission.type != SubmissionType::FlipPreparation) {
		submission.trace_id = TraceDcbSubmitted(submission.commands, submission.commands_hash);
	}
	static const bool snapshot = std::getenv("KYTY_DCB_SNAPSHOT") != nullptr;
	if (snapshot) {
		submission.snapshot = std::make_shared<std::vector<uint32_t>>(submission.commands.begin(),
		                                                              submission.commands.end());
	}
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_queues[submission.queue_id].push_back(std::move(submission));
	m_submission_count++;
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
	Trace::SetCriticalThread();
	g_gpu_thread = true;
	g_gpu_state  = gpu;

	for (;;) {
		Submission                   submission;
		Common::UniqueFunction<void> command;
		bool                         has_submission = false;
		bool                         should_stop    = false;
		{
			Common::LockGuard lock(gpu->m_queue_mutex);
			if (gpu->m_commands.empty() && gpu->m_submission_count == 0 && !gpu->m_stopping) {
				// Nothing more to record: what this thread recorded goes to the GPU now.
				gpu->m_queue_mutex.Unlock();
				gpu->SubmitRecordedBeforeWaiting();
				gpu->m_queue_mutex.Lock();
			}
			while (gpu->m_commands.empty() && gpu->m_submission_count == 0 && !gpu->m_stopping) {
				gpu->m_processing = false;
				gpu->m_idle.Signal();
				KYTY_TRACE_COLUMN(GpuIdle);
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
					// Every queue waits for the guest or the GPU: submit what this thread recorded.
					gpu->m_queue_mutex.Unlock();
					gpu->SubmitRecordedBeforeWaiting();
					gpu->m_queue_mutex.Lock();
					gpu->m_processing = false;
					{
						KYTY_TRACE_COLUMN(GpuIdle);
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
				has_submission    = true;
			}
		}
		if (should_stop) {
			gpu->m_gfx_cp->BufferWait();
			g_gpu_state  = nullptr;
			g_gpu_thread = false;
			return;
		}

		if (command) {
			EXIT_IF(g_current_processor != nullptr);
			{
				KYTY_TRACE_COLUMN(GpuCommand);
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

		EXIT_IF(!has_submission);
		const auto column =
		    submission.queue_id != 0 ? Trace::Column::GpuCompute : Trace::Column::GpuGraphics;
		bool complete = false;
		{
			const Trace::Scope                 scope {Trace::ColumnSite(column)};
			Common::DebugCounters::ScopedTimer busy(
			    Common::DebugCounters::Counter::GpuThreadBusyNs);
			complete = gpu->Process(submission);
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
		gpu->m_processing = false;
		if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
			gpu->m_idle.SignalAll();
		}
	}
}

bool GuestGpu::Process(Submission& submission) {
	KYTY_TRACE_SCOPE("gpu", "ProcessSubmission");
	g_current_submission = {submission.commands, submission.commands_hash, submission.submitted_ms,
	                        submission.snapshot};
	const bool first_slice = !submission.started;
	auto&      cp          = GetProcessor(submission.queue_id);
	g_trace_current_id     = submission.trace_id;
	if (first_slice && submission.trace_id != 0) {
		TraceDcbState(submission.trace_id, 1);
	}

	if (first_slice) {
		submission.started = true;
		cp.SetSubmitId(++m_submit_id);
		m_renderer.GetBufferCache().BeginSubmission();
		if (submission.type == SubmissionType::Commands) {
			cp.PrefetchPrograms(submission.commands);
		}
	}

	cp.BufferInit();
	bool complete = true;

	switch (submission.type) {
		case SubmissionType::Commands:
			complete =
			    cp.Process(submission.execution, submission.commands) == Pm4ProcessResult::Complete;
			if (complete) {
				m_renderer.FinishList();
			}
			if (submission.execution.MadeProgress()) {
				m_recorded_tick = m_renderer.GetCommandScheduler().CurrentTick();
				cp.BufferFlushIfDue();
			}
			break;
		case SubmissionType::FlipPreparation:
			cp.PrepareCpuFlip(submission.flip_request_id);
			break;
		case SubmissionType::SuspendPoint:
			// Age caches at the frame boundary, not once per command list.
			m_renderer.RunGarbageCollector();
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
	// A suspended or finished stream records nothing more: its last work is recorded now.
	m_renderer.GetRenderExecutor().FlushPendingWork();
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

// Packets that leave a draw or dispatch pending (RenderExecutor::FlushPendingWork): they set
// registers or the state the next draw reads, and record nothing; a draw or dispatch packet
// decides itself.
static bool KeepsWorkPending(const uint32_t* packet) {
	const auto header = packet[0];
	switch ((header >> 8u) & 0xffu) {
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
		case Pm4::IT_SET_SH_REG_INDIRECT:
		case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_DRAW_INDEX_OFFSET_2:
		case Pm4::IT_DRAW_INDEX_AUTO:
		// The draws record in order: one whose arguments the CPU reads records the work before
		// it first (CommandProcessor::DrawIndirectMulti).
		case Pm4::IT_DRAW_INDIRECT:
		case Pm4::IT_DRAW_INDEX_INDIRECT:
		case Pm4::IT_DRAW_INDIRECT_MULTI:
		case Pm4::IT_DRAW_INDEX_INDIRECT_MULTI:
		// Only CP state: queued draws have their argument addresses already.
		case Pm4::IT_SET_BASE:
		// Queued behind the pending work (CommandProcessor::DmaData).
		case Pm4::IT_DMA_DATA:
		case Pm4::IT_DISPATCH_DIRECT:
		case Pm4::IT_DRAW_INDEX_MULTI_INST:
		// Cache actions: the handler does nothing (CpOpAcquireMem).
		case Pm4::IT_ACQUIRE_MEM: return true;
		// Flushes and cache writebacks order nothing the renderer does not (TriggerEvent); an
		// occlusion counter dump writes memory.
		case Pm4::IT_EVENT_WRITE: return (packet[1] & 0x3fu) != 0x39u;
		case Pm4::IT_NOP: {
			// Padding and the user data markers; other markers flip or write memory.
			// Debug markers only log (CpOpPushMarker, CpOpPopMarker); a context state operation
			// changes the registers the queued work already copied.
			if (const auto r = KYTY_PM4_R(header);
			    r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER || r == Pm4::R_CONTEXT_STATE) {
				return true;
			}
			if (KYTY_PM4_R(header) != Pm4::R_ZERO) return false;
			const auto word = packet[1];
			return (word & 0xffff0000u) != 0x68750000u || word == 0x68750004u ||
			       word == 0x6875000du;
		}
		default: return false;
	}
}

// Packets that only set state or dispatch: the GPU runs the dispatches around them together.
// Any other packet may synchronize them (partial flushes, cache actions, waits, memory writes).
static bool LeavesComputeChainOpen(uint32_t packet_header) {
	switch ((packet_header >> 8u) & 0xffu) {
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_CONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_SET_QUEUE_REG:
		case Pm4::IT_SET_BASE:
		case Pm4::IT_CONTEXT_CONTROL:
		case Pm4::IT_CLEAR_STATE:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_DISPATCH_DIRECT: return true;
		case Pm4::IT_NOP: {
			const auto r = KYTY_PM4_R(packet_header);
			return r == Pm4::R_ZERO || r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER;
		}
		default: return false;
	}
}

namespace {

// The last packets the command processor took, for the message of a failure: a desynchronised
// stream shows which packet it started to differ after.
struct RecentPacket {
	uint32_t offset;
	uint32_t header;
	uint32_t total;
	uint32_t pad;
	uint64_t address;
};
thread_local RecentPacket g_recent_packets[24];
thread_local uint32_t     g_recent_packet_count = 0;

std::string RecentPacketsText() {
	std::string text;
	if (!g_current_submission.commands.empty()) {
		const auto& submission = g_current_submission;
		text += fmt::format(
		    "\n\t  submitted {} ms ago, {} dwords, commands {} since",
		    static_cast<uint64_t>(Loader::Timer::GetTimeMs()) - submission.submitted_ms,
		    submission.commands.size(),
		    submission.hash == HashCommands(submission.commands) ? "unchanged" : "CHANGED");
		if (submission.snapshot != nullptr) {
			uint32_t shown = 0;
			for (size_t i = 0; i < submission.commands.size() && shown < 24; i++) {
				if (submission.commands[i] != (*submission.snapshot)[i]) {
					text += fmt::format("\n\t  dword 0x{:05x}: was 0x{:08x} now 0x{:08x}", i,
					                    (*submission.snapshot)[i], submission.commands[i]);
					shown++;
				}
			}
		}
	}
	const auto count = std::min<uint32_t>(g_recent_packet_count, 24);
	for (uint32_t i = 0; i < count; i++) {
		const auto& p = g_recent_packets[(g_recent_packet_count - count + i) % 24];
		text += fmt::format("\n\t  off=0x{:05x}/0x{:05x} hdr=0x{:08x} at 0x{:x}", p.offset, p.total,
		                    p.header, p.address);
	}
	return text;
}

} // namespace

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
		g_recent_packets[g_recent_packet_count++ % 24] = {total_dw - remaining_dw, packet_header,
		                                                  total_dw, 0,
		                                                  reinterpret_cast<uint64_t>(packet)};

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
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n\tbuffer at 0x%" PRIx64
			     ", recent packets:%s\n",
			     offset, packet_header, reinterpret_cast<uint64_t>(packet - offset),
			     RecentPacketsText().c_str());
		}

		if (!KeepsWorkPending(packet)) {
			m_renderer.GetRenderExecutor().FlushPendingWork();
		}
		const auto packet_dw =
		    handler(*this, packet_header & ~1u, packet + 1, remaining_dw, total_dw) + 1;
		if (!LeavesComputeChainOpen(packet_header)) {
			// After the pending work: a dispatch queued last can open a chain when it is recorded.
			// Any other queued work ends a chain it follows itself.
			auto& executor = m_renderer.GetRenderExecutor();
			if (executor.LastPendingIsDispatch()) {
				executor.QueueDeferred(
				    [&scheduler = GetScheduler()] { scheduler.EndComputeChain(); });
			} else if (!executor.HasPendingWork()) {
				GetScheduler().EndComputeChain();
			}
		}
		// Whatever the packet prepared buffers for has been recorded by now.
		m_renderer.GetBufferCache().SettleGpuWrites();
		GetScheduler().SubmitIfDue(CommandScheduler::SubmitPoint::Packet);
		if (packet_dw > remaining_dw) {
			const auto offset = total_dw - remaining_dw;
			EXIT("PM4 packet overruns its buffer: header=0x%08" PRIx32 " opcode=0x%02" PRIx32
			     " packet_dw=%" PRIu64 " remaining_dw=%" PRIu64 " total_dw=%" PRIu64
			     " offset=0x%05" PRIx64 "\n\twords: %08" PRIx32 " %08" PRIx32 " %08" PRIx32
			     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n\trecent packets:%s\n",
			     packet_header, opcode, static_cast<uint64_t>(packet_dw),
			     static_cast<uint64_t>(remaining_dw), static_cast<uint64_t>(total_dw),
			     static_cast<uint64_t>(offset), packet[0], remaining_dw > 1 ? packet[1] : 0,
			     remaining_dw > 2 ? packet[2] : 0, remaining_dw > 3 ? packet[3] : 0,
			     remaining_dw > 4 ? packet[4] : 0, remaining_dw > 5 ? packet[5] : 0,
			     RecentPacketsText().c_str());
		}
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

	m_num_instances      = num_instances;
	m_num_instances_addr = 0;
}

uint32_t CommandProcessor::NumInstances() {
	if (m_num_instances_addr != 0) {
		m_num_instances      = *reinterpret_cast<const uint32_t*>(m_num_instances_addr);
		m_num_instances_addr = 0;
	}
	return m_num_instances;
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
			m_renderer.GetOcclusionQueries().Publish();
			// One begin/end pair per DB; bit 63 marks each counter ready.
			constexpr uint64_t ready_bit = 1ull << 63u;
			const auto* results = reinterpret_cast<const volatile uint64_t*>(address);
			for (uint32_t db = 0; db < 16u; db++) {
				const auto begin = results[db * 2u];
				const auto end   = results[db * 2u + 1u];
				if ((begin & end & ready_bit) == 0) {
					if (wait_op == 0) {
						SuspendPm4();
					} else {
						m_predicate_skip = false;
					}
					return;
				}
				value += end - begin;
			}
		} break;
		case 0x03:
			// The wait selector applies only to Z-pass query readiness.
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			value = *reinterpret_cast<const volatile uint64_t*>(address);
			break;
		default: EXIT("unknown predication op: 0x%08" PRIx32 "\n", op);
	}
	switch (condition) {
		case 0x00: m_predicate_skip = (value != 0); break;
		case 0x01: m_predicate_skip = (value == 0); break;
		default: EXIT("unknown predication condition: 0x%08" PRIx32 "\n", condition);
	}
}

bool CommandProcessor::DrawIndex(DrawIndexArgs args) {
	args.index_type_and_size = m_index_type_and_size;
	if (args.instance_count == 0) {
		args.instance_count = NumInstances();
	}
	return m_renderer.GetRenderExecutor().DrawIndex(m_submit_id, CurrentBuffer(), args);
}

const void* CommandProcessor::IndexAddress(uint32_t first_index) const {
	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}
	return reinterpret_cast<const void*>(m_index_base_addr + first_index * index_size);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count) {
	DrawIndex({.index_count = index_count, .index_addr = IndexAddress(index_offset)});
}

static void WriteIndirectDrawRegister(CommandProcessor& cp, uint32_t location, uint32_t value) {
	if (location == Pm4::SH_NOP) {
		return;
	}
	EXIT_NOT_IMPLEMENTED(location >= Pm4::SH_NUM || g_hw_sh_indirect_func[location] == nullptr);
	g_hw_sh_indirect_func[location](cp, location, value);
}

static void PatchIndirectDrawOffsets(CommandProcessor& cp, IndirectDrawRegisters registers,
                                     uint32_t vertex_offset, uint32_t instance_offset,
                                     uint32_t first_index, uint32_t draw_index) {
	WriteIndirectDrawRegister(cp, registers.vertex_offset, vertex_offset);
	WriteIndirectDrawRegister(cp, registers.instance_offset, instance_offset);
	WriteIndirectDrawRegister(cp, registers.index_offset, first_index);
	WriteIndirectDrawRegister(cp, registers.draw_index, draw_index);
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

void CommandProcessor::DrawIndirect(uint32_t data_offset, IndirectDrawRegisters registers,
                                    uint32_t draw_initiator, bool indexed) {
	const auto args_size =
	    indexed ? sizeof(vk::DrawIndexedIndirectCommand) : sizeof(vk::DrawIndirectCommand);
	DrawIndirectMulti(data_offset, 1, nullptr, args_size, registers, draw_initiator, indexed);
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

	auto& executor = m_renderer.GetRenderExecutor();
	// The CPU reads the count and, below, the arguments the GPU does not read: after the work
	// queued before them is recorded, since that work may write them.
	if (count_addr != nullptr) {
		executor.FlushPendingWork();
	}
	const auto draw_count = count_addr == nullptr
	                            ? max_count_or_count
	                            : std::min(uint32_t {*count_addr}, max_count_or_count);
	if (draw_count == 0) {
		return;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::IndirectDraws, draw_count);

	const auto args_addr = m_draw_indirect_args_base_addr + data_offset;
	// NUM_INSTANCES keeps the last draw's instance count.
	m_num_instances_addr = args_addr + static_cast<uint64_t>(draw_count - 1) * stride_in_bytes +
	                       offsetof(vk::DrawIndirectCommand, instanceCount);
	// The GPU reads the records if the shaders allow; the VS prolog removes Vulkan's bases
	// from the index VGPRs and copies them only to the user SGPRs the packet patches.
	DrawIndirectArgs records {args_addr, draw_count, stride_in_bytes};
	if (IndirectDrawOffsets(registers, records.offsets)) {
		if (!indexed && executor.DrawAuto(m_submit_id, CurrentBuffer(), {.indirect = records})) {
			return;
		}
		if (indexed && m_index_buffer_size != 0 &&
		    executor.DrawIndex(m_submit_id, CurrentBuffer(),
		                       {.index_count         = m_index_buffer_size,
		                        .index_addr          = IndexAddress(0),
		                        .index_type_and_size = m_index_type_and_size,
		                        .indirect            = records})) {
			return;
		}
	}

	executor.FlushPendingWork();
	const uint32_t max_indices = m_index_buffer_size != 0 ? m_index_buffer_size : UINT32_MAX;
	for (uint32_t i = 0; i < draw_count; i++) {
		const auto addr = args_addr + static_cast<uint64_t>(i) * stride_in_bytes;
		if (!indexed) {
			const auto& args = *reinterpret_cast<const vk::DrawIndirectCommand*>(addr);
			PatchIndirectDrawOffsets(*this, registers, args.firstVertex, args.firstInstance, 0, i);
			executor.DrawAuto(m_submit_id, CurrentBuffer(),
			                  {.vertex_count   = args.vertexCount,
			                   .instance_count = args.instanceCount,
			                   .first_vertex   = args.firstVertex,
			                   .first_instance = args.firstInstance,
			                   .offset_source  = DrawOffsetSource::IndirectArgs});
			continue;
		}

		const auto& args = *reinterpret_cast<const vk::DrawIndexedIndirectCommand*>(addr);
		PatchIndirectDrawOffsets(*this, registers, static_cast<uint32_t>(args.vertexOffset),
		                         args.firstInstance, args.firstIndex, i);
		executor.DrawIndex(m_submit_id, CurrentBuffer(),
		                   {.index_count         = std::min(args.indexCount, max_indices),
		                    .index_addr          = IndexAddress(args.firstIndex),
		                    .instance_count      = args.instanceCount,
		                    .index_type_and_size = m_index_type_and_size,
		                    .base_vertex         = args.vertexOffset,
		                    .first_instance      = args.firstInstance,
		                    .offset_source       = DrawOffsetSource::IndirectArgs});
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	m_renderer.GetRenderExecutor().DispatchDirect(m_submit_id, CurrentBuffer(), thread_group_x,
	                                              thread_group_y, thread_group_z, mode,
	                                              IsAsyncComputeQueue());
}

void CommandProcessor::ProcessPssr(const Pssr::Request& request) {
	EXIT_NOT_IMPLEMENTED(IsAsyncComputeQueue());
	m_renderer.GetRenderExecutor().ProcessPssr(CurrentBuffer(), request);
}

void CommandProcessor::PrefetchPrograms(std::span<const uint32_t> commands) {
	KYTY_PROFILER_FUNCTION();
	auto                            state = m_sh_ctx.GetCs();
	std::vector<PrescannedDispatch> dispatches;
	PrescanComputeDispatches(
	    commands, state,
	    [](uint64_t address, std::span<uint32_t> words) {
		    // As the CP reads indirect register pairs: the latest data, GPU writes included.
		    return LibKernel::Memory::TryReadBufferBacking(address, words.data(),
		                                                   words.size_bytes()) ||
		           LibKernel::Memory::TryReadBacking(address, words.data(), words.size_bytes());
	    },
	    dispatches);
	static const bool trace_misses = std::getenv("KYTY_TRACE_SHADERS") != nullptr;
	if (trace_misses) {
		std::fprintf(stderr, "[prescan] f=%" PRIu64 " words=%zu dispatches=%zu\n",
		             Trace::CurrentFrame(), commands.size(), dispatches.size());
	}
	if (dispatches.empty()) {
		return;
	}
	// Most dispatches of a list repeat a shader: each register set is looked up once.
	std::ranges::sort(dispatches, {}, [](const PrescannedDispatch& dispatch) {
		return std::tuple(dispatch.regs.cs_regs.data_addr, dispatch.mode);
	});
	Common::LockGuard         lock(m_renderer.GetMutex());
	auto&                     cache    = m_renderer.GetPipelineCache();
	const PrescannedDispatch* previous = nullptr;
	for (const auto& dispatch: dispatches) {
		if (previous != nullptr &&
		    std::memcmp(&previous->regs, &dispatch.regs, sizeof(dispatch.regs)) == 0 &&
		    previous->mode == dispatch.mode) {
			continue;
		}
		previous = &dispatch;
		cache.PrefetchComputeProgram(dispatch.regs, dispatch.mode, IsAsyncComputeQueue());
	}
}

void CommandProcessor::DispatchIndirect(uint64_t args_addr, uint32_t mode) {
	EXIT_NOT_IMPLEMENTED(args_addr == 0 || (args_addr & 3u) != 0);
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	m_renderer.GetRenderExecutor().DispatchIndirect(m_submit_id, CurrentBuffer(), args_addr, mode,
	                                                IsAsyncComputeQueue());
}

bool CommandProcessor::DrawIndexAuto(DrawAutoArgs args) {
	if (args.instance_count == 0) {
		args.instance_count = NumInstances();
	}
	return m_renderer.GetRenderExecutor().DrawAuto(m_submit_id, CurrentBuffer(), args);
}

// A GPU-thread wait for a flip checks this often.
constexpr uint32_t FlipWaitSliceMicros = 1000;

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index) {
	BufferFlush();

	while (!m_renderer.GetVideoOut().WaitFlipDone(static_cast<int>(video_out_handle),
	                                              static_cast<int>(display_buffer_index),
	                                              FlipWaitSliceMicros)) {
	}
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
	m_interrupt_since_flip |= interrupt;
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

	// Dumped occlusion counters are written before any end-of-pipe signal reaches the guest.
	m_renderer.GetOcclusionQueries().Publish();

	// A label beside GPU-written bytes goes to the backing store, so they are not downloaded.
	// Other labels go through the guest mapping: its fault keeps a clean cached copy current and
	// downloads GPU-written bytes under the label before the store.
	const auto write_label = [this, address](const void* value, uint64_t size) {
		const auto vaddr   = reinterpret_cast<uint64_t>(address);
		auto&      buffers = m_renderer.GetBufferCache();
		if (!GuestRange {vaddr, size}.Valid() || !buffers.IsRegionGpuModified(vaddr, size) ||
		    buffers.HasGpuDirtyBytes(vaddr, size) ||
		    !LibKernel::Memory::TryWriteBacking(vaddr, value, size)) {
			std::memcpy(address, value, size);
		}
		NoteGuestWrite(address, size);
	};
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
			NoteGuestWrite(address, uint64_t {gds >> 16u} * sizeof(uint32_t));
			break;
		}
		default: EXIT("unknown release_mem data selector: %" PRIu32 "\n", data_sel);
	}

	// GuestGpu::Process's end-of-slice flush submits the work this interrupt waits on.
	if (interrupt) {
		GetScheduler().DeferPriorityOperation(
		    [&renderer = m_renderer, event_id = m_interrupt_event_id, interrupt_context_id] {
			    renderer.TriggerInterrupt(event_id, interrupt_context_id);
		    });
	}
}

uint32_t CommandProcessor::ReadLabel(const uint32_t* address) {
	return ReadMemory(address);
}

template <typename T>
T CommandProcessor::ReadMemory(const T* address) {
	T value {};
	if (!m_renderer.GetBufferCache().ReadGuestBacked(reinterpret_cast<uint64_t>(address), &value,
	                                                 sizeof(T))) {
		value = *static_cast<const volatile T*>(address);
	}
	return value;
}

void CommandProcessor::WriteBackImages() {
	// GPU-written images are copied back without holding any signal: a held signal exposes the host
	// GPU's speed. Guest accesses to them wait for the copies.
	if (m_renderer.GetTextureCache().WriteBackImages()) {
		GetScheduler().Flush();
	}
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
			if (auto& queries = m_renderer.GetOcclusionQueries(); queries.Enabled()) {
				queries.Dump(event_address);
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
			NoteGuestWrite(reinterpret_cast<const void*>(event_address),
			               16u * 2u * sizeof(uint64_t));
			m_synthetic_occlusion_counter = (m_synthetic_occlusion_counter + 1u) & counter_mask;
			break;
		}
		default:
			EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

// interrupt: the queue's end-of-pipe interrupt is raised when the flip completes.
void CommandProcessor::Flip(int handle, int index, int flip_mode, int64_t flip_arg,
                            bool interrupt) {
	if (graphics_debug_dump_enabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}
	m_renderer.GetOcclusionQueries().Publish();

	auto& command = CurrentBuffer();
	auto  request = Sync::PrepareVideoOutFlip(command, handle, index, flip_mode, flip_arg);
	(void)command.Handle();
	m_interrupt_since_flip = false;
	CompleteFlipInOrder(request, interrupt);
	GetScheduler().Flush();
}

// The flip completes, with the interrupt, in order with the interrupts recorded before it:
// priority operations run in the order they were recorded.
void CommandProcessor::CompleteFlipInOrder(uint64_t request, bool interrupt) {
	GetScheduler().DeferPriorityOperation([&renderer = m_renderer, request,
	                                       frame     = m_renderer.GetGpu().RecordFrame(),
	                                       event_id  = interrupt ? m_interrupt_event_id : -1] {
		renderer.GetVideoOut().CompleteFlip(request);
		if (event_id >= 0) {
			renderer.TriggerInterrupt(event_id, 0);
		}
		frame->FrameCompleted();
	});
}

void CommandProcessor::PrepareCpuFlip(uint64_t request_id) {
	auto& command = CurrentBuffer();
	m_renderer.GetVideoOut().PrepareFlip(request_id, command);
	m_renderer.GetRenderExecutor().NoteFlip();
	GetScheduler().DeferPriorityOperation(
	    [this, request_id, frame = m_renderer.GetGpu().RecordFrame()] {
		    m_renderer.GetVideoOut().CompleteFlip(request_id);
		    frame->FrameCompleted();
	    });
	GetScheduler().Flush();
}

void CommandProcessor::SynchronizeGpu() {
	GetScheduler().Finish();
}

bool GuestGpu::IsGpuThread() noexcept {
	return g_gpu_state != nullptr;
}

} // namespace Libs::Graphics
