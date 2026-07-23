#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/displayBuffer.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>
#include <vector>

namespace Libs::Graphics {

static thread_local CommandProcessor* g_current_processor      = nullptr;
static thread_local Pm4Execution*     g_current_execution      = nullptr;
static thread_local uint32_t          g_submission_pause_depth = 0;
static thread_local bool              g_gpu_mutex_owned        = false;

class GpuMutexLock final {
public:
	explicit GpuMutexLock(Common::Mutex& mutex): m_mutex(mutex) {
		if (g_gpu_mutex_owned) {
			EXIT("recursive GPU mutex acquisition\n");
		}
		g_gpu_mutex_owned = true;
		m_mutex.Lock();
	}
	~GpuMutexLock() {
		if (!g_gpu_mutex_owned) {
			EXIT("invalid GPU mutex release\n");
		}
		m_mutex.Unlock();
		g_gpu_mutex_owned = false;
	}

private:
	Common::Mutex& m_mutex;
};

struct OwnedCmdBuffer {
	OwnedCmdBuffer() = default;
	explicit OwnedCmdBuffer(const uint32_t* data, uint32_t count) {
		EXIT_IF(data == nullptr && count != 0);
		if (count != 0) {
			m_words.assign(data, data + count);
		}
	}

	[[nodiscard]] bool      Empty() const noexcept { return m_words.empty(); }
	[[nodiscard]] uint32_t  Size() const noexcept { return static_cast<uint32_t>(m_words.size()); }
	[[nodiscard]] uint32_t* Data() noexcept { return m_words.data(); }

private:
	std::vector<uint32_t> m_words;
};

class Gpu {
public:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	Gpu() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		m_gfx_cp = std::make_unique<CommandProcessor>();
		Common::Thread thread(ThreadRun, this);
		thread.Detach();
	}
	~Gpu() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(Gpu);

	void Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
	            uint32_t num_const_dw, bool trigger_agc_interrupt_on_done);
	void SubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
	                   bool trigger_agc_interrupt_on_done);
	void SubmitFlipPreparation();
	void Done();
	void PauseSubmissions();
	void ResumeSubmissions();
	int  GetFrameNum();

private:
	enum class SubmissionType { Graphics, Compute, FlipPreparation };

	struct Submission {
		SubmissionType type     = SubmissionType::Graphics;
		uint32_t       queue_id = 0;
		OwnedCmdBuffer commands;
		OwnedCmdBuffer constant_commands;
		Pm4Execution   command_execution;
		Pm4Execution   constant_execution;
		bool           trigger_agc_interrupt_on_done = false;
		bool           reset_processor               = false;
		bool           started                       = false;
		bool           command_complete              = false;
		bool           constant_complete             = false;
		bool           blocked                       = false;
	};

	void              WaitLocked();
	void              Enqueue(Submission submission);
	void              WaitForIdle();
	bool              Process(Submission& submission);
	static void       ThreadRun(void* data);
	CommandProcessor& GetProcessor(uint32_t queue_id);

	Common::Mutex                                  m_submission_mutex;
	Common::Mutex                                  m_queue_mutex;
	Common::CondVar                                m_work_available;
	Common::CondVar                                m_idle;
	std::array<std::deque<Submission>, QueueCount> m_queues;
	uint32_t                                       m_next_queue       = 0;
	uint32_t                                       m_submission_count = 0;
	bool                                           m_processing       = false;
	bool                                           m_graphics_done    = true;

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;
};

static Gpu* g_gpu = nullptr;

static bool GraphicsRunDebugDumpEnabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::OutputDirection::Silent;
}

void GraphicsRunInit() {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	EXIT_IF(g_gpu != nullptr);

	GraphicsInitJmpTables();

	g_gpu = new Gpu;
}

void Gpu::Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
                 uint32_t num_const_dw, bool trigger_agc_interrupt_on_done) {
	GpuMutexLock lock(m_submission_mutex);
	Submission   submission;
	submission.type                          = SubmissionType::Graphics;
	submission.queue_id                      = 0;
	submission.commands                      = OwnedCmdBuffer(cmd_draw_buffer, num_draw_dw);
	submission.constant_commands             = OwnedCmdBuffer(cmd_const_buffer, num_const_dw);
	submission.trigger_agc_interrupt_on_done = trigger_agc_interrupt_on_done;
	submission.reset_processor               = m_graphics_done;
	m_graphics_done                          = false;
	Enqueue(std::move(submission));
}

void Gpu::SubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
                        bool trigger_agc_interrupt_on_done) {
	GpuMutexLock lock(m_submission_mutex);

	constexpr uint32_t compute_queue_base = 0x20u;
	EXIT_NOT_IMPLEMENTED(queue < compute_queue_base ||
	                     queue >= compute_queue_base + ComputeQueueCount);

	const auto compute_queue = queue - compute_queue_base;
	Submission submission;
	submission.type                          = SubmissionType::Compute;
	submission.queue_id                      = 1 + compute_queue;
	submission.commands                      = OwnedCmdBuffer(cmd_buffer, num_dw);
	submission.trigger_agc_interrupt_on_done = trigger_agc_interrupt_on_done;
	Enqueue(std::move(submission));
}

void Gpu::SubmitFlipPreparation() {
	GpuMutexLock lock(m_submission_mutex);
	Submission   submission;
	submission.type            = SubmissionType::FlipPreparation;
	submission.queue_id        = 0;
	submission.reset_processor = m_graphics_done;
	m_graphics_done            = false;
	Enqueue(std::move(submission));
}

void Gpu::Done() {
	GpuMutexLock lock(m_submission_mutex);
	WaitLocked();
	m_graphics_done = true;
	m_done_num++;
}

int Gpu::GetFrameNum() {
	return m_done_num;
}

void Gpu::WaitLocked() {
	WaitForIdle();
	m_gfx_cp->BufferWait();
}

CommandProcessor& Gpu::GetProcessor(uint32_t queue_id) {
	EXIT_IF(queue_id >= QueueCount);
	if (queue_id == 0) {
		return *m_gfx_cp;
	}
	auto& processor = m_compute_cp[queue_id - 1];
	if (processor == nullptr) {
		processor = std::make_unique<CommandProcessor>();
	}
	return *processor;
}

void CommandProcessor::FinishReadbackTransaction() {
	if (GraphicsRunCurrentCommandProcessor() != this || !m_readback_active) {
		EXIT("GPU readback finish requires a command-processor thread\n");
	}
	if (m_readback_finished) {
		return;
	}
	GetScheduler().FinishCurrent();
	m_readback_finished = true;
}

void CommandProcessor::Reset() {
	BufferWait();

	Sync::DeleteBuffers();

	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_user_data_marker                 = HW::UserSgprType::Unknown;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;

	std::memset(m_const_ram, 0, sizeof(m_const_ram));
}

void CommandProcessor::BufferInit() {
	GetScheduler().Begin(m_ctx, m_ucfg, m_sh_ctx);
}

void CommandProcessor::BufferFlush() {
	GetScheduler().Flush();
}

void CommandProcessor::BufferFlushAndWait() {
	auto& submitted = GetScheduler().FlushAndGetSubmitted();
	submitted.WaitForFenceOnly();
}

void CommandProcessor::BufferWait() {
	BufferInit();
	GetScheduler().Finish();
}

void CommandProcessor::ResetDeCe() {
	m_de_count    = 0;
	m_ce_count    = 0;
	m_ce_complete = false;
}

void CommandProcessor::WaitCe() {
	if (m_ce_count <= m_de_count && !m_ce_complete) {
		SuspendPm4();
	}
}

void CommandProcessor::WaitDeDiff(uint32_t diff) {
	EXIT_IF(m_de_count > m_ce_count);
	if (m_ce_count - m_de_count >= diff) {
		SuspendPm4();
	}
}

void CommandProcessor::IncrementDe() {
	BufferWait();
	m_de_count++;
}

void CommandProcessor::IncrementCe() {
	m_ce_count++;
}

void CommandProcessor::WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num) {
	memcpy(m_const_ram + offset / 4, src, static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num) {
	memcpy(dst, m_const_ram + offset / 4, static_cast<size_t>(dw_num) * 4);
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
void CommandProcessor::WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t poll,
                                  uint32_t wait_op) {
	EXIT_IF(addr == nullptr);
	if ((wait_op & ~1u) != 0) {
		EXIT("unsupported wait_reg_mem operation: 0x%08" PRIx32 "\n", wait_op);
	}

	(void)poll;
	if (!TestWaitRegMemValue(*addr, ref, mask, func)) {
		SuspendPm4();
	}
}

template void CommandProcessor::WaitRegMem<uint32_t>(uint32_t, const uint32_t*, uint32_t, uint32_t,
                                                     uint32_t, uint32_t);
template void CommandProcessor::WaitRegMem<uint64_t>(uint32_t, const uint64_t*, uint64_t, uint64_t,
                                                     uint32_t, uint32_t);

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num,
                                 uint32_t write_control) {
	const uint32_t dst_sel      = ((write_control >> 30u) & 0x1u) | ((write_control >> 7u) & 0x1eu);
	const uint32_t cache_policy = (write_control >> 25u) & 0x3u;
	const uint32_t increment    = (write_control >> 16u) & 0x1u;
	const uint32_t write_confirm = (write_control >> 20u) & 0x1u;

	if (dst_sel != 0 && dst_sel != 2 && dst_sel != 4 && dst_sel != 5) {
		EXIT("unsupported writeData destination selector 0x%02" PRIx32 "\n", dst_sel);
	}
	EXIT_NOT_IMPLEMENTED(increment != 0);

	if (cache_policy > 3 || write_confirm > 1) {
		LOGF("\t warning: unexpected write_data control 0x%08" PRIx32 "\n", write_control);
	}
	if (dw_num == 0) {
		return;
	}

	memcpy(dst, src, static_cast<size_t>(dw_num) * sizeof(uint32_t));
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	const auto value = Sync::ReadReferenceClock();
	std::memcpy(reinterpret_cast<void*>(dst_address), &value, num_bytes);
	LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64 " size=%u\n",
	     dst_address, value, num_bytes);
}

void CommandProcessor::DmaData(uint8_t engine, uint8_t dst_sel, uint8_t dst_cache_policy,
                               uint64_t dst_address_or_offset, uint8_t src_sel,
                               uint8_t  src_cache_policy,
                               uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes,
                               uint8_t wait_for_previous, uint8_t write_confirm,
                               uint8_t block_engine) {
	EXIT_NOT_IMPLEMENTED(engine > 1);
	if (num_bytes == 0) {
		return;
	}
	EXIT_NOT_IMPLEMENTED((num_bytes & 3u) != 0);
	EXIT_NOT_IMPLEMENTED(dst_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(src_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(wait_for_previous > 1);
	EXIT_NOT_IMPLEMENTED(write_confirm > 1);
	EXIT_NOT_IMPLEMENTED(block_engine > 1);
	const bool dst_memory = dst_sel == 0 || dst_sel == 3;
	const bool src_memory = src_sel == 0 || src_sel == 3;
	if (!dst_memory) {
		EXIT("unsupported dmaData destination selector 0x%02" PRIx8 "\n", dst_sel);
	}
	if (src_sel == 2) {
		GetGpuResources().FillBuffer(
		    CurrentBuffer(), dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu));
		return;
	}
	if (src_memory) {
		GetGpuResources().CopyBuffer(CurrentBuffer(), dst_address_or_offset,
		                             src_address_or_offset_or_immediate, num_bytes);
		return;
	}
	EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
}

void Gpu::Enqueue(Submission submission) {
	EXIT_IF(submission.queue_id >= QueueCount);
	Common::LockGuard lock(m_queue_mutex);
	m_queues[submission.queue_id].push_back(std::move(submission));
	m_submission_count++;
	m_work_available.Signal();
}

void Gpu::WaitForIdle() {
	Common::LockGuard lock(m_queue_mutex);
	while (m_processing || m_submission_count != 0) {
		m_idle.Wait(&m_queue_mutex);
	}
}

void Gpu::ThreadRun(void* data) {
	auto* gpu = static_cast<Gpu*>(data);
	EXIT_IF(gpu == nullptr);
	KYTY_PROFILER_THREAD("Thread_Gpu");

	for (;;) {
		Submission submission;
		{
			Common::LockGuard lock(gpu->m_queue_mutex);
			int               selected_queue = -1;
			while (selected_queue < 0) {
				while (gpu->m_submission_count == 0) {
					gpu->m_processing = false;
					gpu->m_idle.Signal();
					gpu->m_work_available.Wait(&gpu->m_queue_mutex);
				}
				for (uint32_t offset = 0; offset < QueueCount; offset++) {
					const auto id = (gpu->m_next_queue + offset) % QueueCount;
					if (!gpu->m_queues[id].empty() && !gpu->m_queues[id].front().blocked) {
						selected_queue = static_cast<int>(id);
						break;
					}
				}
				if (selected_queue < 0) {
					gpu->m_processing = false;
					gpu->m_work_available.WaitFor(&gpu->m_queue_mutex, 100);
					for (auto& queue: gpu->m_queues) {
						if (!queue.empty()) {
							queue.front().blocked = false;
						}
					}
				}
			}
			auto& queue = gpu->m_queues[static_cast<uint32_t>(selected_queue)];
			submission  = std::move(queue.front());
			queue.pop_front();
			gpu->m_submission_count--;
			gpu->m_next_queue = (static_cast<uint32_t>(selected_queue) + 1) % QueueCount;
			gpu->m_processing = true;
		}

		const bool complete = gpu->Process(submission);

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
		if (gpu->m_submission_count == 0) {
			gpu->m_idle.Signal();
		}
	}
}

bool Gpu::Process(Submission& submission) {
	auto&      cp          = GetProcessor(submission.queue_id);
	const bool first_slice = !submission.started;

	if (first_slice && submission.reset_processor) {
		cp.Reset();
	}

	if (first_slice) {
		submission.started = true;
		cp.SetSubmitId(++m_submit_id);
		cp.ResetDeCe();
		cp.SetFlip({});
	}

	cp.BufferInit();
	bool complete = true;

	switch (submission.type) {
		case SubmissionType::Graphics: {
			bool progressed = false;
			submission.constant_complete |= submission.constant_commands.Empty();
			for (;;) {
				bool round_progress = false;
				if (!submission.constant_complete) {
					submission.constant_complete =
					    cp.Process(
					        submission.constant_execution, submission.constant_commands.Data(),
					        submission.constant_commands.Size()) == Pm4ProcessResult::Complete;
					round_progress |= submission.constant_execution.MadeProgress();
				}
				cp.SetCeComplete(submission.constant_complete);
				if (!submission.command_complete) {
					submission.command_complete =
					    cp.Process(submission.command_execution, submission.commands.Data(),
					               submission.commands.Size()) == Pm4ProcessResult::Complete;
					round_progress |= submission.command_execution.MadeProgress();
				}
				progressed |= round_progress;
				complete = submission.command_complete && submission.constant_complete;
				if (complete || !round_progress) {
					break;
				}
			}
			if (progressed) {
				cp.BufferFlush();
			}
			if (complete && submission.trigger_agc_interrupt_on_done) {
				Sync::TriggerEopEvent(0);
			}
			break;
		}
		case SubmissionType::Compute: {
			const auto      num_dw                  = submission.commands.Size();
			auto*           buffer                  = submission.commands.Data();
			static uint32_t compute_batch_log_count = 0;
			if (first_slice && num_dw <= 128 && compute_batch_log_count++ < 32) {
				LOGF("compute direct batch: data=0x%016" PRIx64 ", num_dw=%" PRIu32 "\n",
				     reinterpret_cast<uint64_t>(buffer), num_dw);
				for (uint32_t i = 0; i < std::min<uint32_t>(num_dw, 16); i++) {
					LOGF("\t compute[%02" PRIu32 "] = 0x%08" PRIx32 "\n", i, buffer[i]);
				}
			}
			if (first_slice) {
				GraphicsDbgDumpDcb("cc", num_dw, buffer);
			}
			complete = cp.Process(submission.command_execution, buffer, num_dw) ==
			           Pm4ProcessResult::Complete;
			if (submission.command_execution.MadeProgress()) {
				cp.BufferFlush();
			}
			if (complete && submission.trigger_agc_interrupt_on_done) {
				Sync::TriggerAgcUserInterrupt();
			}
			break;
		}
		case SubmissionType::FlipPreparation: cp.PrepareCpuFlip(); break;
	}

	return complete;
}

void Gpu::PauseSubmissions() {
	if (g_gpu_mutex_owned) {
		EXIT("GPU submissions are already paused by this thread\n");
	}
	g_gpu_mutex_owned = true;
	m_submission_mutex.Lock();
	WaitLocked();
	LabelDrain();
}

void Gpu::ResumeSubmissions() {
	if (!g_gpu_mutex_owned) {
		EXIT("GPU submissions resumed without an active pause\n");
	}
	m_submission_mutex.Unlock();
	g_gpu_mutex_owned = false;
}

Pm4ProcessResult CommandProcessor::Process(Pm4Execution& execution, uint32_t* buffer,
                                           uint32_t size_dw) {
	KYTY_PROFILER_BLOCK("CommandProcessor::Process");
	EXIT_IF(g_current_execution != nullptr);
	if (execution.m_buffer_stack.empty() && size_dw != 0) {
		execution.m_buffer_stack.push_back({buffer, size_dw, size_dw, 0});
	}
	execution.m_suspended     = false;
	execution.m_made_progress = false;

	struct ExecutionScope {
		ExecutionScope(CommandProcessor& processor, Pm4Execution& execution)
		    : previous_processor(g_current_processor), previous_execution(g_current_execution) {
			g_current_processor = &processor;
			g_current_execution = &execution;
		}
		~ExecutionScope() {
			g_current_processor = previous_processor;
			g_current_execution = previous_execution;
		}

		CommandProcessor* previous_processor;
		Pm4Execution*     previous_execution;
	} execution_scope(*this, execution);

	ProcessPm4(execution, 0);
	return execution.m_buffer_stack.empty() ? Pm4ProcessResult::Complete
	                                        : Pm4ProcessResult::Blocked;
}

void CommandProcessor::ProcessIndirectBuffer(uint32_t* buffer, uint32_t size_dw) {
	EXIT_IF(g_current_execution == nullptr);
	if (size_dw == 0) {
		return;
	}
	auto&      execution  = *g_current_execution;
	const auto stop_depth = execution.m_buffer_stack.size();
	execution.m_buffer_stack.push_back({buffer, size_dw, size_dw, 0});
	ProcessPm4(execution, stop_depth);
}

void CommandProcessor::SuspendPm4() {
	EXIT_IF(g_current_execution == nullptr);
	g_current_execution->m_suspended = true;
}

void CommandProcessor::ProcessPm4(Pm4Execution& execution, size_t stop_depth) {
	while (execution.m_buffer_stack.size() > stop_depth) {
		const auto buffer_index = execution.m_buffer_stack.size() - 1;
		auto&      cursor       = execution.m_buffer_stack[buffer_index];
		if (cursor.deferred_advance_dw != 0) {
			EXIT_IF(cursor.deferred_advance_dw > cursor.remaining_dw);
			cursor.next_packet += cursor.deferred_advance_dw;
			cursor.remaining_dw -= cursor.deferred_advance_dw;
			cursor.deferred_advance_dw = 0;
			execution.m_made_progress  = true;
			continue;
		}
		if (cursor.remaining_dw == 0) {
			execution.m_buffer_stack.pop_back();
			continue;
		}

		auto* const packet        = cursor.next_packet;
		const auto  remaining_dw  = cursor.remaining_dw;
		const auto  total_dw      = cursor.total_dw;
		const auto  packet_header = packet[0];
		const auto  opcode        = (packet_header >> 8u) & 0xffu;
		EXIT_NOT_IMPLEMENTED(remaining_dw > total_dw);

		if (packet_header == 0x80000000u) {
			cursor.next_packet++;
			cursor.remaining_dw--;
			execution.m_made_progress = true;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(remaining_dw < 2);

		if (GraphicsRunDebugDumpEnabled()) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     total_dw - remaining_dw, packet_header, opcode, KYTY_PM4_LEN(packet_header));
		}

		if ((packet_header & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			auto packet_dw = KYTY_PM4_LEN(packet_header);
			EXIT_NOT_IMPLEMENTED(packet_dw == 0 || packet_dw > remaining_dw);
			static std::atomic<uint32_t> skip_log_count {0};
			if (skip_log_count.fetch_add(1) < 2048) {
				LOGF("\t predicated skip: op=0x%02" PRIx32 ", r=0x%02" PRIx32 ", len=%" PRIu32
				     ", packet=0x%016" PRIx64 ", cmd_id=0x%08" PRIx32 "\n",
				     opcode, KYTY_PM4_R(packet_header), packet_dw,
				     reinterpret_cast<uint64_t>(packet), packet_header);
			}
			if (opcode == Pm4::IT_NOP && KYTY_PM4_R(packet_header) == Pm4::R_RELEASE_MEM &&
			    packet_dw >= 7) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 128) {
					const auto dst = packet[3] | (static_cast<uint64_t>(packet[4]) << 32u);
					const auto val = packet[5] | (static_cast<uint64_t>(packet[6]) << 32u);
					LOGF("\t predicated skip: R_RELEASE_MEM dst=0x%016" PRIx64
					     ", value=0x%016" PRIx64 ", action=0x%08" PRIx32
					     ", gcr/data/int=0x%08" PRIx32 "\n",
					     dst, val, packet[1], packet[2]);
				}
			}
			cursor.next_packet += packet_dw;
			cursor.remaining_dw -= packet_dw;
			execution.m_made_progress = true;
			continue;
		}

		auto handler = g_cp_op_func[opcode];

		if (handler == nullptr) {
			const auto offset = total_dw - remaining_dw;
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
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n",
			     total_dw - remaining_dw, packet_header);
		}

		const auto packet_dw =
		    handler(*this, packet_header & ~1u, packet + 1, remaining_dw, total_dw) + 1;
		EXIT_IF(packet_dw > remaining_dw);
		if (execution.m_suspended) {
			if (execution.m_buffer_stack.size() > buffer_index + 1) {
				execution.m_buffer_stack[buffer_index].deferred_advance_dw = packet_dw;
			}
			return;
		}
		EXIT_IF(execution.m_buffer_stack.size() != buffer_index + 1);
		execution.m_buffer_stack[buffer_index].next_packet += packet_dw;
		execution.m_buffer_stack[buffer_index].remaining_dw -= packet_dw;
		execution.m_made_progress = true;
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

	m_num_instances = num_instances;
}

void CommandProcessor::SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
                                      const volatile void* address, uint32_t count_in_dwords) {
	if (wait_op != 0) {
		BufferFlushAndWait();
	}

	(void)count_in_dwords;

	switch (op) {
		case 0x00: {
			m_predicate_skip = false;
		} break;
		case 0x03: {
			EXIT_NOT_IMPLEMENTED(address == nullptr);

			auto value = *reinterpret_cast<const volatile uint64_t*>(address);

			switch (condition) {
				case 0x00: m_predicate_skip = (value != 0); break;
				case 0x01: m_predicate_skip = (value == 0); break;
				default: EXIT("unknown predication condition: 0x%08" PRIx32 "\n", condition);
			}
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 128) {
				LOGF("\t bool predication: addr=0x%016" PRIx64 ", value=0x%016" PRIx64
				     ", condition=%" PRIu32 ", skip=%u, wait_op=%" PRIu32 "\n",
				     reinterpret_cast<uint64_t>(address), value, condition,
				     m_predicate_skip ? 1u : 0u, wait_op);
			}
		} break;
		default: EXIT("unknown predication op: 0x%08" PRIx32 "\n", op);
	}
}

void CommandProcessor::DrawIndex(uint32_t index_count, const void* index_addr, uint32_t flags,
                                 uint32_t type, uint32_t instance_count, const void* object_ids,
                                 uint32_t render_target_slice_offset, int32_t vertex_offset_add,
                                 uint32_t first_instance) {
	CheckBuffer();

	if (instance_count == 0) {
		instance_count = m_num_instances;
	}
	if (object_ids != nullptr) {
		LOGF("\t draw indexed multi-instanced objectIds = 0x%016" PRIx64 "\n",
		     reinterpret_cast<uint64_t>(object_ids));
	}
	if (render_target_slice_offset != 0) {
		LOGF("\t draw render target slice offset = %" PRIu32 "\n", render_target_slice_offset);
	}
	if (vertex_offset_add != 0 || first_instance != 0) {
		LOGF("\t draw indexed offsets: vertex_offset_add = %" PRId32 ", first_instance = %" PRIu32
		     "\n",
		     vertex_offset_add, first_instance);
	}
	RenderDrawIndex(m_submit_id, CurrentBuffer(), m_index_type_and_size, index_count, index_addr,
	                flags, type, instance_count, render_target_slice_offset, vertex_offset_add,
	                first_instance);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count,
                                       uint32_t flags) {
	CheckBuffer();

	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(index_offset) * index_size);

	RenderDrawIndex(m_submit_id, CurrentBuffer(), m_index_type_and_size, index_count, index_addr,
	                flags, 1, m_num_instances);
}

void CommandProcessor::DrawIndirect(uint32_t data_offset, uint32_t draw_initiator, bool indexed) {
	struct DrawIndirectArgs {
		uint32_t vertex_count_per_instance;
		uint32_t instance_count;
		uint32_t start_vertex_location;
		uint32_t start_instance_location;
	};
	struct DrawIndexedIndirectArgs {
		uint32_t index_count_per_instance;
		uint32_t instance_count;
		uint32_t start_index_location;
		uint32_t base_vertex_location;
		uint32_t start_instance_location;
	};

	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != 2u);
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);

	const auto* args_addr =
	    reinterpret_cast<const void*>(m_draw_indirect_args_base_addr + data_offset);

	if (!indexed) {
		DrawIndirectArgs args {};
		std::memcpy(&args, args_addr, sizeof(args));
		if (args.instance_count != 1u || args.start_vertex_location != 0u ||
		    args.start_instance_location != 0u) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 64) {
				LOGF("\t warning: partial DrawIndirect args: vertex_count=%" PRIu32
				     ", instance_count=%" PRIu32 ", start_vertex=%" PRIu32
				     ", start_instance=%" PRIu32 "\n",
				     args.vertex_count_per_instance, args.instance_count,
				     args.start_vertex_location, args.start_instance_location);
			}
		}
		DrawIndexAuto(args.vertex_count_per_instance, 0, 0, args.instance_count,
		              args.start_vertex_location, args.start_instance_location);
		return;
	}

	DrawIndexedIndirectArgs args {};
	std::memcpy(&args, args_addr, sizeof(args));
	if (args.base_vertex_location != 0u || args.start_instance_location != 0u) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 64) {
			LOGF("\t warning: partial DrawIndexIndirect args: index_count=%" PRIu32
			     ", instance_count=%" PRIu32 ", start_index=%" PRIu32 ", base_vertex=%" PRIu32
			     ", start_instance=%" PRIu32 "\n",
			     args.index_count_per_instance, args.instance_count, args.start_index_location,
			     args.base_vertex_location, args.start_instance_location);
		}
	}

	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(args.start_index_location) * index_size);

	const uint32_t index_count =
	    (m_index_buffer_size != 0 ? std::min(args.index_count_per_instance, m_index_buffer_size)
	                              : args.index_count_per_instance);
	if (GraphicsRunDebugDumpEnabled() && index_count != args.index_count_per_instance) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
			LOGF("\t DrawIndexIndirect: clamped index_count from %" PRIu32 " to %" PRIu32
			     " using INDEX_BUFFER_SIZE\n",
			     args.index_count_per_instance, index_count);
		}
	}

	DrawIndex(index_count, index_addr, 0, 1, args.instance_count, nullptr, 0,
	          static_cast<int32_t>(args.base_vertex_location), args.start_instance_location);
}

void CommandProcessor::DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
                                         const volatile uint32_t* count_addr,
                                         uint32_t stride_in_bytes, uint32_t draw_initiator,
                                         bool indexed) {
	struct DrawIndirectArgs {
		uint32_t vertex_count_per_instance;
		uint32_t instance_count;
		uint32_t start_vertex_location;
		uint32_t start_instance_location;
	};
	struct DrawIndexedIndirectArgs {
		uint32_t index_count_per_instance;
		uint32_t instance_count;
		uint32_t start_index_location;
		uint32_t base_vertex_location;
		uint32_t start_instance_location;
	};

	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != 2u);
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);

	uint32_t draw_count = max_count_or_count;
	if (count_addr != nullptr) {
		draw_count = *count_addr;
		if (draw_count > max_count_or_count) {
			draw_count = max_count_or_count;
		}
	}

	if (draw_count == 0) {
		return;
	}

	const auto args_size = indexed ? sizeof(DrawIndexedIndirectArgs) : sizeof(DrawIndirectArgs);
	EXIT_NOT_IMPLEMENTED(stride_in_bytes < args_size);

	for (uint32_t i = 0; i < draw_count; i++) {
		const auto args_addr = m_draw_indirect_args_base_addr + data_offset +
		                       static_cast<uint64_t>(i) * stride_in_bytes;

		if (!indexed) {
			auto* args = reinterpret_cast<const DrawIndirectArgs*>(args_addr);
			if (args->instance_count != 1u || args->start_vertex_location != 0u ||
			    args->start_instance_location != 0u) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 64) {
					LOGF("\t warning: partial DrawIndirectMulti args[%u]: vertex_count=%" PRIu32
					     ", instance_count=%" PRIu32 ", start_vertex=%" PRIu32
					     ", start_instance=%" PRIu32 "\n",
					     i, args->vertex_count_per_instance, args->instance_count,
					     args->start_vertex_location, args->start_instance_location);
				}
			}
			DrawIndexAuto(args->vertex_count_per_instance, 0, 0, args->instance_count,
			              args->start_vertex_location, args->start_instance_location);
			continue;
		}

		auto* args = reinterpret_cast<const DrawIndexedIndirectArgs*>(args_addr);
		if (args->base_vertex_location != 0u || args->start_instance_location != 0u) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 64) {
				LOGF("\t warning: partial DrawIndexIndirectMulti args[%u]: index_count=%" PRIu32
				     ", instance_count=%" PRIu32 ", start_index=%" PRIu32 ", base_vertex=%" PRIu32
				     ", start_instance=%" PRIu32 "\n",
				     i, args->index_count_per_instance, args->instance_count,
				     args->start_index_location, args->base_vertex_location,
				     args->start_instance_location);
			}
		}

		uint64_t index_size = 0;
		switch (m_index_type_and_size) {
			case 0: index_size = 2; break;
			case 1: index_size = 4; break;
			case 2: index_size = 1; break;
			default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
		}

		auto* index_addr = reinterpret_cast<const void*>(
		    m_index_base_addr + static_cast<uint64_t>(args->start_index_location) * index_size);

		const uint32_t index_count =
		    (m_index_buffer_size != 0
		         ? std::min(args->index_count_per_instance, m_index_buffer_size)
		         : args->index_count_per_instance);
		if (GraphicsRunDebugDumpEnabled() && index_count != args->index_count_per_instance) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
				LOGF("\t DrawIndexIndirectMulti: clamped index_count from %" PRIu32 " to %" PRIu32
				     " using INDEX_BUFFER_SIZE\n",
				     args->index_count_per_instance, index_count);
			}
		}

		DrawIndex(index_count, index_addr, 0, 1, args->instance_count, nullptr, 0,
		          static_cast<int32_t>(args->base_vertex_location), args->start_instance_location);
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
	uint32_t frame_num = 0;
	uint32_t local_x   = 1;
	uint32_t local_y   = 1;
	uint32_t local_z   = 1;

	{
		CheckBuffer();
		frame_num = GraphicsRunGetFrameNum();
		if (GraphicsRunDebugDumpEnabled()) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 1024) {
				const auto& cs = m_sh_ctx.GetCs().cs_regs;
				const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
				LOGF("QueuePoint DispatchDirect: frame=%u submit=%" PRIu64
				     " groups=%ux%ux%u local=%ux%ux%u mode=0x%08" PRIx32 " wave=%u cs=0x%016" PRIx64
				     " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32 " oa_space=0x%08" PRIx32
				     "\n",
				     frame_num, m_submit_id, thread_group_x, thread_group_y, thread_group_z,
				     std::max(cs.num_thread_x, 1u), std::max(cs.num_thread_y, 1u),
				     std::max(cs.num_thread_z, 1u), mode, static_cast<uint32_t>(cs.wave_size),
				     cs.data_addr, m_ucfg.GetGdsOaState().GetIndex(),
				     oa.IsCounterEnabled() ? "true" : "false", oa.GetAddressBytes(),
				     oa.GetSpaceAvailable());
			}
		}

		const auto& cs = m_sh_ctx.GetCs().cs_regs;
		local_x        = std::max(cs.num_thread_x, 1u);
		local_y        = std::max(cs.num_thread_y, 1u);
		local_z        = std::max(cs.num_thread_z, 1u);
		if (cs.wave_size == 64u) {
			static std::atomic_bool logged_wave64_shader {false};
			if (!logged_wave64_shader.exchange(true, std::memory_order_relaxed)) {
				LOGF("warning: executing wave64 compute shader cs=0x%016" PRIx64 "\n",
				     cs.data_addr);
				std::printf("warning: executing wave64 compute shader cs=0x%016" PRIx64 "\n",
				            cs.data_addr);
				std::fflush(stdout);
			}
		}

		RenderDispatchDirect(m_submit_id, CurrentBuffer(), thread_group_x, thread_group_y,
		                     thread_group_z, mode);
	}

	constexpr uint32_t DispatchInitiatorUseThreadDimensions = 1u << 5u;
	auto               group_count = [](uint32_t threads, uint32_t group_size) {
		return (threads == 0
		            ? 0u
		            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
	};

	auto groups_x = thread_group_x;
	auto groups_y = thread_group_y;
	auto groups_z = thread_group_z;
	if ((mode & DispatchInitiatorUseThreadDimensions) != 0) {
		groups_x = group_count(thread_group_x, local_x);
		groups_y = group_count(thread_group_y, local_y);
		groups_z = group_count(thread_group_z, local_z);
	}

	const uint64_t invocations =
	    static_cast<uint64_t>(groups_x) * groups_y * groups_z * local_x * local_y * local_z;
	if (invocations != 0) {
		BufferFlushAndWait();
	}
}

void CommandProcessor::DispatchIndirect(uint32_t data_offset, uint32_t mode) {
	struct DispatchIndirectArgs {
		uint32_t thread_group_x;
		uint32_t thread_group_y;
		uint32_t thread_group_z;
	};

	EXIT_NOT_IMPLEMENTED(m_dispatch_indirect_args_base_addr == 0);

	const auto args_addr = m_dispatch_indirect_args_base_addr + data_offset;
	auto*      args      = reinterpret_cast<const DispatchIndirectArgs*>(args_addr);

	DispatchDirect(args->thread_group_x, args->thread_group_y, args->thread_group_z, mode);
}

void CommandProcessor::DrawIndexAuto(uint32_t index_count, uint32_t flags,
                                     uint32_t render_target_slice_offset, uint32_t instance_count,
                                     uint32_t first_vertex, uint32_t first_instance) {
	CheckBuffer();

	RenderDrawIndexAuto(m_submit_id, CurrentBuffer(), index_count, flags,
	                    render_target_slice_offset, instance_count, first_vertex, first_instance);
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index) {
	BufferFlush();

	VideoOut::VideoOutWaitFlipDone(static_cast<int>(video_out_handle),
	                               static_cast<int>(display_buffer_index));
}

template <typename T>
void CommandProcessor::WriteAtEndOfPipe(uint32_t cache_policy, uint32_t event_write_dest,
                                        uint32_t eop_event_type, uint32_t cache_action,
                                        uint32_t event_index, uint32_t event_write_source,
                                        void* dst_gpu_addr, T value, uint32_t interrupt_selector,
                                        uint32_t interrupt_context_id) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		const auto bits      = static_cast<unsigned>(sizeof(T) * 8u);
		const auto log_width = static_cast<int>(sizeof(T) * 2u);

		LOGF("CommandProcessor::WriteAtEndOfPipe%u()\n"
		     "\t cache_policy        = 0x%08" PRIx32 "\n"
		     "\t event_write_dest    = 0x%08" PRIx32 "\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t event_index         = 0x%08" PRIx32 "\n"
		     "\t event_write_source  = 0x%08" PRIx32 "\n"
		     "\t interrupt_selector  = 0x%08" PRIx32 "\n"
		     "\t interrupt_context   = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%0*" PRIx64 "\n",
		     bits, cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
		     event_write_source, interrupt_selector, interrupt_context_id,
		     reinterpret_cast<uint64_t>(dst_gpu_addr), log_width, static_cast<uint64_t>(value));
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);

	bool with_interrupt = false;
	switch (interrupt_selector) {
		case 0x00:
		case 0x03: with_interrupt = false; break;
		case 0x01: Sync::TriggerEopEventAtEndOfPipe(CurrentBuffer(), interrupt_context_id); return;
		case 0x02: with_interrupt = true; break;
		default: EXIT("unknown interrupt_selector\n");
	}

	auto write32 = [&](bool with_writeback) {
		auto* dst  = static_cast<uint32_t*>(dst_gpu_addr);
		auto  data = static_cast<uint32_t>(value);
		std::memcpy(dst, &data, sizeof(data));

		if (with_interrupt) {
			if (with_writeback) {
				Sync::WriteAtEndOfPipeWithInterruptWriteBack32(m_submit_id, CurrentBuffer(), dst,
				                                               data, interrupt_context_id);
			} else {
				Sync::WriteAtEndOfPipeWithInterrupt32(m_submit_id, CurrentBuffer(), dst, data,
				                                      interrupt_context_id);
			}
		} else if (with_writeback) {
			Sync::WriteAtEndOfPipeWithWriteBack32(m_submit_id, CurrentBuffer(), dst, data);
		} else {
			Sync::WriteAtEndOfPipe32(m_submit_id, CurrentBuffer(), dst, data);
		}
	};

	switch (event_write_source) {
		case 0x01:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (eop_event_type == 0x2f && cache_action == 0x00 && event_index == 0x06) {
					auto* dst = static_cast<uint32_t*>(dst_gpu_addr);
					SynchronizeGpu();
					Sync::ReadGds(dst, value & 0xffffu, value >> 16u);
					Sync::WriteAtEndOfPipeGds32(m_submit_id, CurrentBuffer(), dst, value & 0xffffu,
					                            value >> 16u);
					return;
				}
			} else if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05) {
				write32(false);
				return;
			}
			break;
		case 0x02:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (eop_event_type == 0x2f && event_index == 0x06) {
					switch (cache_action) {
						case 0x00: write32(false); return;
						case 0x38: write32(true); return;
						default: break;
					}
				}
			} else {
				auto write64 = [&](bool with_writeback) {
					auto* dst = static_cast<uint64_t*>(dst_gpu_addr);
					std::memcpy(dst, &value, sizeof(value));

					if (with_interrupt) {
						if (with_writeback) {
							Sync::WriteAtEndOfPipeWithInterruptWriteBack64(
							    m_submit_id, CurrentBuffer(), dst, value, interrupt_context_id);
						} else {
							Sync::WriteAtEndOfPipeWithInterrupt64(m_submit_id, CurrentBuffer(), dst,
							                                      value, interrupt_context_id);
						}
					} else if (with_writeback) {
						Sync::WriteAtEndOfPipeWithWriteBack64(m_submit_id, CurrentBuffer(), dst,
						                                      value);
					} else {
						Sync::WriteAtEndOfPipe64(m_submit_id, CurrentBuffer(), dst, value);
					}
				};

				switch (cache_action) {
					case 0x00:
						switch (eop_event_type) {
							case 0x04:
							case 0x28:
								if ((eop_event_type == 0x04 && event_index == 0x05) ||
								    (eop_event_type == 0x28 && event_index == 0x00)) {
									write64(false);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
							case 0x2f:
							case 0x30:
								if (event_index == 0x00 && !with_interrupt) {
									write64(false);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x38:
						switch (eop_event_type) {
							case 0x04:
							case 0x14:
							case 0x28:
								if (((eop_event_type == 0x04 || eop_event_type == 0x28) &&
								     event_index == 0x05 && !with_interrupt) ||
								    (event_index == 0x00)) {
									write64(true);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
								if (event_index == 0x00 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							case 0x2f:
								if (event_index == 0x06 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x3b:
						if (eop_event_type == 0x04 && event_index == 0x05 && with_interrupt) {
							write64(true);
							return;
						}
						break;
					default: break;
				}
			}
			break;
		case 0x04:
			if constexpr (sizeof(T) == sizeof(uint64_t)) {
				const auto clock = Sync::ReadReferenceClock();
				std::memcpy(dst_gpu_addr, &clock, sizeof(clock));
				switch (cache_action) {
					case 0x00:
						if (((eop_event_type == 0x04 && event_index == 0x05) ||
						     (eop_event_type == 0x28 && event_index == 0x00)) &&
						    !with_interrupt) {
							Sync::WriteAtEndOfPipeClockCounter(m_submit_id, CurrentBuffer(),
							                                   static_cast<uint64_t*>(dst_gpu_addr),
							                                   clock);
							return;
						}
						break;
					case 0x38:
						if (((eop_event_type == 0x04 &&
						      (event_index == 0x00 || event_index == 0x05)) ||
						     (eop_event_type == 0x28 && event_index == 0x00)) &&
						    !with_interrupt) {
							Sync::WriteAtEndOfPipeClockCounterWithWriteBack(
							    m_submit_id, CurrentBuffer(), static_cast<uint64_t*>(dst_gpu_addr),
							    clock);
							return;
						}
						break;
					default: break;
				}
			}
			break;
		default: break;
	}

	EXIT("unknown event type\n");
}

void CommandProcessor::WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint32_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

void CommandProcessor::WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint64_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

void CommandProcessor::MemoryBarrier() {
	CheckBuffer();

	GraphicsRenderMemoryBarrier(CurrentBuffer());
}

void CommandProcessor::TriggerEopEventAtEndOfPipe(uint32_t interrupt_context_id) {
	CheckBuffer();

	Sync::TriggerEopEventAtEndOfPipe(CurrentBuffer(), interrupt_context_id);
}

void CommandProcessor::RenderTextureBarrier(uint64_t vaddr, uint64_t size) {
	CheckBuffer();

	GraphicsRenderTextureBarrier(CurrentBuffer(), vaddr, size);
}

void CommandProcessor::DepthStencilBarrier(uint64_t vaddr, uint64_t size) {
	CheckBuffer();

	GraphicsRenderDepthStencilBarrier(CurrentBuffer(), vaddr, size);
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index) {
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::TriggerEvent()\n"
		     "\t event_type  = 0x%08" PRIx32 "\n"
		     "\t event_index = 0x%08" PRIx32 "\n",
		     event_type, event_index);
	}

	const auto valid_cache_event_index = event_index == 0x00000000 || event_index == 0x00000007;
	switch (event_type) {
		// CsPartialFlush, GsPartialFlush, PsPartialFlush.
		case 0x00000007:
		case 0x0000000f:
		case 0x00000010: MemoryBarrier(); break;
		// CbDbDataWritebackInvalidate, CbDataWritebackInvalidate.
		case 0x00000016:
		case 0x00000031:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			MemoryBarrier();
			SynchronizeGpu();
			break;
		// DbDataWritebackInvalidate, DbMetadataWritebackInvalidate, CbMetadataWritebackInvalidate.
		case 0x0000002a:
		case 0x0000002c:
		case 0x0000002e:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			MemoryBarrier();
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
		case 0x00000039:
		case 0x0000003a:
			LOGF("\t temporary: ignoring unsupported event_write type 0x%08" PRIx32
			     ", index 0x%08" PRIx32 "\n",
			     event_type, event_index);
			break;
		default:
			EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

void CommandProcessor::Flip() {
	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}

	auto& command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeOnlyFlip(m_submit_id, command, m_flip.handle, m_flip.index,
	                               m_flip.flip_mode, m_flip.flip_arg, request);
	GetScheduler().Flush();
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value) {
	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n"
		     "\t dst_gpu_addr = 0x%016" PRIx64 "\n"
		     "\t value        = 0x%08" PRIx32 "\n",
		     reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto& command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithFlip32(m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr),
	                                 value, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                 m_flip.flip_arg, request);
	GetScheduler().Flush();
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action,
                                         void* dst_gpu_addr, uint32_t value) {
	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::FlipWithInterrupt()\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%08" PRIx32 "\n",
		     eop_event_type, cache_action, reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	if (eop_event_type != 0x00000004 || cache_action != 0x00000038) {
		EXIT("unknown event type\n");
	}
	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto& command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithInterruptWriteBackFlip32(
	    m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr), value, m_flip.handle,
	    m_flip.index, m_flip.flip_mode, m_flip.flip_arg, request);
	GetScheduler().Flush();
}

void CommandProcessor::PrepareCpuFlip() {
	CheckBuffer();
	if (g_current_processor != nullptr) {
		EXIT("invalid graphics-thread CPU flip preparation\n");
	}
	struct ProcessorScope {
		explicit ProcessorScope(CommandProcessor& processor) { g_current_processor = &processor; }
		~ProcessorScope() { g_current_processor = nullptr; }
	};
	ProcessorScope processor_scope(*this);

	auto prepared_id = Presentation::DisplayBufferPrepareNextFlipOnGpu(CurrentBuffer());
	GetScheduler().Flush();
	Presentation::DisplayBufferCompleteFlipFromGpu(prepared_id);
}

void CommandProcessor::SynchronizeGpu() {
	GetScheduler().FinishCurrent();
}

void GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
                       uint32_t num_const_dw, bool trigger_agc_interrupt_on_done) {
	EXIT_IF(cmd_draw_buffer == nullptr);
	EXIT_IF(num_draw_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw,
	              trigger_agc_interrupt_on_done);
}

void GraphicsRunSubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
                              bool trigger_agc_interrupt_on_done) {
	EXIT_IF(cmd_buffer == nullptr);
	EXIT_IF(num_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	g_gpu->SubmitCompute(queue, cmd_buffer, num_dw, trigger_agc_interrupt_on_done);
}

void GraphicsRunSubmitFlipPreparation() {
	EXIT_IF(g_gpu == nullptr);
	g_gpu->SubmitFlipPreparation();
}

void GraphicsRunWait() {
	GraphicsRunSubmissionLock lock;
}

GraphicsRunSubmissionLock::GraphicsRunSubmissionLock() {
	if (g_gpu == nullptr || g_current_processor != nullptr ||
	    g_submission_pause_depth == UINT32_MAX) {
		EXIT("cannot acquire GPU submission lock in the current state\n");
	}
	if (g_submission_pause_depth++ == 0) {
		g_gpu->PauseSubmissions();
	}
}

GraphicsRunSubmissionLock::~GraphicsRunSubmissionLock() {
	if (g_gpu == nullptr || g_submission_pause_depth == 0) {
		EXIT("GPU submission lock released without ownership\n");
	}
	if (--g_submission_pause_depth == 0) {
		g_gpu->ResumeSubmissions();
	}
}

void GraphicsRunDone() {
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Done();
}

int GraphicsRunGetFrameNum() {
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->GetFrameNum();
}

bool GraphicsRunIsCommandProcessorThread() noexcept {
	return g_current_processor != nullptr;
}

CommandProcessor* GraphicsRunCurrentCommandProcessor() noexcept {
	return g_current_processor;
}

void GraphicsRunFinishScheduler() {
	if (g_current_processor == nullptr) {
		EXIT("GPU readback finish requires a command-processor thread\n");
	}
	g_current_processor->FinishReadbackTransaction();
}

bool GraphicsRunSubmissionLockHeld() noexcept {
	return g_submission_pause_depth != 0;
}

bool GraphicsRunGpuLockHeld() noexcept {
	return g_gpu_mutex_owned;
}

} // namespace Libs::Graphics
