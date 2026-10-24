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
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
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
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

namespace Libs::Graphics {

static thread_local CommandProcessor* g_current_processor = nullptr;
static thread_local Pm4Execution*     g_current_execution = nullptr;
static thread_local bool              g_gpu_thread        = false;
static thread_local GuestGpu*         g_gpu_state         = nullptr;

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

static bool GraphicsRunDebugDumpEnabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::LogDirection::Silent;
}

// [DIAG cbsync] TEMPORARY command-buffer mutation instrumentation.
// FNV-1a 64 bit over every dword of the stream plus a wrapping checksum and the first/last
// dwords, so a change is detected even if the hash collides and can be localised to an end.
// Reads only, exactly like the PM4 parser does; it never writes to the stream.
static CbDiagFingerprint CbDiagFingerprintSpan(std::span<const uint32_t> commands) {
	CbDiagFingerprint fp;
	if (commands.empty()) {
		return fp;
	}
	uint64_t hash = 0xcbf29ce484222325ull;
	uint64_t sum  = 0;
	for (const auto dw: commands) {
		sum += dw;
		for (uint32_t byte = 0; byte < 4; byte++) {
			hash ^= static_cast<uint64_t>((dw >> (byte * 8u)) & 0xffu);
			hash *= 0x00000100000001b3ull;
		}
	}
	fp.hash   = hash;
	fp.sum    = sum;
	fp.first  = commands.front();
	fp.last   = commands.back();
	fp.num_dw = static_cast<uint32_t>(commands.size());
	return fp;
}

static bool CbDiagFingerprintsMatch(const CbDiagFingerprint& a, const CbDiagFingerprint& b) {
	return a.num_dw == b.num_dw && a.hash == b.hash && a.sum == b.sum && a.first == b.first &&
	       a.last == b.last;
}

// [DIAG cbsync] TEMPORARY: the command stream the GPU thread is currently parsing (the
// emulator-owned copy) together with the original borrowed guest span that was submitted.
// `GuestGpu::Process` sets it immediately before `CommandProcessor::Process` and clears it right
// after. `ProcessPm4` uses it to detect that the guest changed the memory it handed over while the
// owned copy was being parsed; the AGC patch helpers use it to attribute writes. Only the GPU
// thread mutates it; the read from the guest thread in `GpuDiagWriteOverlapsActiveParse` is a
// benign diagnostic race. Read-only.
static const uint32_t* g_cb_diag_guest        = nullptr;
static const uint32_t* g_cb_diag_owned        = nullptr;
static uint32_t        g_cb_diag_num_dw       = 0;
static uint64_t        g_cb_diag_id           = 0;
static bool            g_cb_diag_header_logged = false;

static void CbDiagBeginParse(uint64_t id, std::span<const uint32_t> guest,
                             const std::vector<uint32_t>& owned) {
	if (guest.empty() || guest.size() != owned.size()) {
		g_cb_diag_guest         = nullptr;
		g_cb_diag_owned         = nullptr;
		g_cb_diag_num_dw        = 0;
		g_cb_diag_id            = 0;
		g_cb_diag_header_logged = false;
		return;
	}
	g_cb_diag_guest         = guest.data();
	g_cb_diag_owned         = owned.data();
	g_cb_diag_num_dw        = static_cast<uint32_t>(guest.size());
	g_cb_diag_id            = id;
	g_cb_diag_header_logged = false;
}

static void CbDiagEndParse() {
	g_cb_diag_guest         = nullptr;
	g_cb_diag_owned         = nullptr;
	g_cb_diag_num_dw        = 0;
	g_cb_diag_id            = 0;
	g_cb_diag_header_logged = false;
}

bool GpuDiagWriteOverlapsActiveParse(uint64_t address, uint64_t size) {
	const auto* guest = g_cb_diag_guest;
	if (guest == nullptr || size == 0 || g_cb_diag_num_dw == 0) {
		return false;
	}
	const uint64_t begin = reinterpret_cast<uint64_t>(guest);
	const uint64_t end   = begin + static_cast<uint64_t>(g_cb_diag_num_dw) * 4u;
	return address < end && begin < address + size;
}

// [DIAG cbsync] TEMPORARY: compare the original guest memory (`live`) against the emulator-owned
// copy (`reference`) and log the first/last differing dword, how many differ and a short bounded
// sample. Logs only when a difference is present; read-only. Both must hold `num_dw` dwords.
static void CbDiagLogDiff(uint64_t id, const char* phase, const char* stream, const uint32_t* live,
                          const uint32_t* reference, uint32_t num_dw, uint64_t base_address) {
	if (live == nullptr || reference == nullptr || num_dw == 0) {
		return;
	}
	uint32_t first_diff = num_dw;
	uint32_t last_diff  = 0;
	uint32_t count      = 0;
	for (uint32_t i = 0; i < num_dw; i++) {
		if (live[i] != reference[i]) {
			if (first_diff == num_dw) {
				first_diff = i;
			}
			last_diff = i;
			count++;
		}
	}
	if (count == 0) {
		return;
	}
	LOGF("[DIAG cbsync] DIFF id=%" PRIu64 " phase=%s stream=%s base=0x%016" PRIx64 " dw=%" PRIu32
	     " first_diff=0x%05" PRIx32 " first_addr=0x%016" PRIx64 " last_diff=0x%05" PRIx32
	     " count=%" PRIu32 " expected=0x%08" PRIx32 " actual=0x%08" PRIx32 "\n",
	     id, phase, stream, base_address, num_dw, first_diff,
	     base_address + static_cast<uint64_t>(first_diff) * 4u, last_diff, count,
	     reference[first_diff], live[first_diff]);
	uint32_t shown = 0;
	for (uint32_t i = first_diff; i <= last_diff && shown < 16u; i++) {
		if (live[i] != reference[i]) {
			LOGF("[DIAG cbsync] DIFF   off=0x%05" PRIx32 " addr=0x%016" PRIx64
			     " expected=0x%08" PRIx32 " actual=0x%08" PRIx32 "\n",
			     i, base_address + static_cast<uint64_t>(i) * 4u, reference[i], live[i]);
			shown++;
		}
	}
}

GuestGpu::GuestGpu(RenderContext& renderer): m_renderer(renderer) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	GraphicsInitJmpTables();
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

void GuestGpu::ProcessCommands() {
	EXIT_IF(!IsGpuThread());
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

void GuestGpu::Submit(std::span<const uint32_t> draw_commands,
                      std::span<const uint32_t> constant_commands) {
	if (draw_commands.empty()) {
		return;
	}
	Submission submission;
	submission.type              = SubmissionType::Graphics;
	submission.queue_id          = 0;
	submission.commands          = draw_commands;
	submission.constant_commands = constant_commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitCompute(uint32_t queue, std::span<const uint32_t> commands) {
	EXIT_IF(commands.empty());

	EXIT_NOT_IMPLEMENTED(queue < ComputeQueueBase || queue >= ComputeQueueBase + ComputeQueueCount);

	const auto compute_queue = queue - ComputeQueueBase;
	Submission submission;
	submission.type     = SubmissionType::Compute;
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
	EXIT_IF(queue_id >= QueueCount);
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
	m_saved_ctx.Reset();
	m_context_state_pushed             = false;
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_index_base_addr                  = 0;
	m_num_instances                    = 1;
	m_predicate_skip                   = false;
	m_user_data_marker                 = HW::UserSgprType::Unknown;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;

	std::memset(m_const_ram, 0, sizeof(m_const_ram));
}

void CommandProcessor::ApplyContextStateOperation(ContextStateOperation operation) {
	switch (operation) {
		case ContextStateOperation::Clear: m_ctx.Reset(); break;
		case ContextStateOperation::Push:
			EXIT_IF(m_context_state_pushed);
			m_saved_ctx            = m_ctx;
			m_context_state_pushed = true;
			break;
		case ContextStateOperation::Pop:
			EXIT_IF(!m_context_state_pushed);
			m_ctx                  = m_saved_ctx;
			m_saved_ctx            = {};
			m_context_state_pushed = false;
			break;
		case ContextStateOperation::PushClear:
			EXIT_IF(m_context_state_pushed);
			m_saved_ctx            = m_ctx;
			m_context_state_pushed = true;
			m_ctx.Reset();
			break;
		default: EXIT("unknown context state operation: %u\n", static_cast<uint32_t>(operation));
	}
}

void CommandProcessor::BufferInit() {
	GetScheduler().Begin(m_ctx, m_ucfg, m_sh_ctx);
}

void CommandProcessor::BufferFlush() {
	GetScheduler().Flush();
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

void CommandProcessor::WaitForRewind(bool valid) {
	if (!valid) {
		SuspendPm4();
	}
}

void CommandProcessor::IncrementDe() {
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

	if (write_one_address) {
		for (uint32_t i = 0; i < dw_num; i++) {
			dst[0] = src[i];
		}
	} else {
		memcpy(dst, src, static_cast<size_t>(dw_num) * sizeof(uint32_t));
	}
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	const auto value = Sync::ReadReferenceClock();
	std::memcpy(reinterpret_cast<void*>(dst_address), &value, num_bytes);
	static std::atomic<uint32_t> clock_log_count {0};
	if (clock_log_count.fetch_add(1) < 64) {
		LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64
		     " size=%u\n",
		     dst_address, value, num_bytes);
	}
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
	EXIT_NOT_IMPLEMENTED(dst_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(src_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(wait_for_previous > 1);
	EXIT_NOT_IMPLEMENTED(write_confirm > 1);
	EXIT_NOT_IMPLEMENTED(block_engine > 1);
	if (static_cast<uint32_t>(dst_address_or_offset) == 0x3022cu) {
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
	if (src_sel == 2) {
		buffer_cache.FillBuffer(
		    dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu), dst_gds);
		return;
	}
	bool src_gds = false;
	if (!decode_gds(src_sel, src_gds)) {
		EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
	}
	if (src_gds && dst_gds) {
		EXIT("unsupported dmaData GDS-to-GDS copy\n");
	}
	buffer_cache.CopyBuffer(dst_address_or_offset, src_address_or_offset_or_immediate, num_bytes,
	                        dst_gds, src_gds);
}

// [vb-diag] TEMPORARY: snapshot, at submit time, the vertex-buffer (V#) tables that a later draw
// will dereference through inline VS user-SGPR writes (SET_SH_REG covering GS slots 12/13). The
// draw path reads those tables from live guest memory, potentially long after the guest has
// recycled the memory; capturing the contents here - on the submitting thread, the last moment
// they are guaranteed to still be the state the stream was built against - shows whether the
// table was valid when the stream was handed over. Read-only, bounded to kMaxLogs tables.
static void VbDiagSnapshotSubmitTables(std::span<const uint32_t> stream, uint64_t diag_id) {
	constexpr uint32_t kMaxLogs = 256u;
	static std::atomic<uint32_t> logged {0};
	size_t i = 0;
	while (i < stream.size()) {
		const uint32_t header = stream[i];
		const uint32_t len    = (header == 0x80000000u) ? 1u : Pm4::PacketSizeDw(header);
		if (len == 0 || i + static_cast<size_t>(len) > stream.size()) {
			break;
		}
		const uint32_t opcode = (header >> 8u) & 0xffu;
		if (opcode == Pm4::IT_SET_SH_REG && len >= 4u) {
			const uint32_t cmd_offset = stream[i + 1];
			if (cmd_offset >= Pm4::SPI_SHADER_USER_DATA_GS_0 &&
			    cmd_offset <= Pm4::SPI_SHADER_USER_DATA_GS_0 + 31u) {
				const uint32_t slot       = cmd_offset - Pm4::SPI_SHADER_USER_DATA_GS_0;
				const uint32_t payload_dw = len - 2u;
				if (slot <= 12u && slot + payload_dw >= 14u) {
					const uint64_t ptr =
					    static_cast<uint64_t>(stream[i + 2 + (12u - slot)]) |
					    (static_cast<uint64_t>(stream[i + 2 + (13u - slot)]) << 32u);
					const uint32_t index = logged.fetch_add(1, std::memory_order_relaxed);
					if (index < kMaxLogs && ptr != 0u) {
						std::array<uint32_t, 16> words {};
						const bool ok = Libs::LibKernel::Memory::TryReadGuestMemory(
						    ptr, words.data(), static_cast<uint64_t>(words.size()) * sizeof(uint32_t));
						LOGF("[vb-diag] submit id=%" PRIu64 " off=0x%" PRIxPTR
						     " vsharp_ptr=0x%016" PRIx64 " readable=%s\n",
						    diag_id, static_cast<uintptr_t>(i * 4u), ptr, ok ? "yes" : "no");
						if (ok) {
							LOGF("[vb-diag] submit table:");
							for (const auto w: words) {
								LOGF(" %08" PRIx32, w);
							}
							LOGF("\n");
						}
					}
				}
			}
		}
		i += len;
	}
}

void GuestGpu::Enqueue(Submission submission) {
	EXIT_IF(submission.queue_id >= QueueCount);
	// Take ownership of the submitted command streams. The spans handed in point at guest memory,
	// which the guest may rewrite at any time; because parsing is deferred to the GPU thread, the
	// parser must read emulator-owned memory instead. The copies are made before the queue lock and
	// are never resized afterwards, so the re-pointed spans stay valid for the submission's life.
	// [DIAG cbsync] Remember the original borrowed spans so the diagnostic can still observe the
	// guest mutating them while the owned copy is parsed.
	submission.diag_guest_cmd   = submission.commands;
	submission.diag_guest_const = submission.constant_commands;
	submission.command_store.assign(submission.commands.begin(), submission.commands.end());
	submission.constant_command_store.assign(submission.constant_commands.begin(),
	                                         submission.constant_commands.end());
	submission.commands          = submission.command_store;
	submission.constant_commands = submission.constant_command_store;
	// Take submit-time ownership of every guest region the parser will dereference (the
	// IT_SET_*_REG_INDIRECT pair arrays and nested command buffers). This runs on the submitting
	// thread, before the submission is published to the GPU queue, which is the last moment the
	// referenced memory is guaranteed to still hold the submit-time contents: the guest cannot be
	// rewriting the buffer it has just handed over through this submit call. The reads go through
	// the bounds- and protection-checked Memory::TryReadGuestMemory so a stale, unmapped or
	// unreadable reference can never fault the emulator.
	//
	// A capture sequence number identifies this submit's capture independently of
	// submission.diag_id, which is only assigned later, under the queue lock.
	static std::atomic<uint64_t> capture_sequence {0};
	const uint64_t               capture_seq = ++capture_sequence;
	const Pm4GuestReader         guest_reader = [](uint64_t address, void* dst, uint64_t size) {
		if (Libs::LibKernel::Memory::TryReadGuestMemory(address, dst, size)) {
			return true;
		}
		// Immediate, bounded report of a rejected read. The caller records the referencing packet
		// details; this line proves the capture actually attempted the read and where it failed.
		constexpr uint32_t kMaxReadFailureLogs = 16u;
		static std::atomic<uint32_t> logged {0};
		const uint32_t index = logged.fetch_add(1, std::memory_order_relaxed);
		if (index < kMaxReadFailureLogs) {
			LOGF_COLOR(Log::Color::BrightRed,
			           "[pm4-ownership] guest read failed: addr=0x%016" PRIx64 " size=%" PRIu64
			           "\n",
			           address, size);
		}
		return false;
	};
	submission.command_execution.CaptureGuestRanges(submission.commands, guest_reader);
	submission.constant_execution.CaptureGuestRanges(submission.constant_commands, guest_reader);
	// A miss means a referenced guest range could not be read at submit time. The later parse will
	// then fail loudly for that reference instead of reading memory the guest may have rewritten, so
	// surface it here too, where the cause (a stale or malformed submit) is still visible. The
	// per-reference detail (address, count, referencing packet header and offset, reason) is
	// bounded by Pm4Ownership.
	if (submission.command_execution.OwnedGuestRangeMisses() != 0u ||
	    submission.constant_execution.OwnedGuestRangeMisses() != 0u ||
	    submission.command_execution.OwnedGuestRangeMalformed() != 0u ||
	    submission.constant_execution.OwnedGuestRangeMalformed() != 0u) {
		LOGF_COLOR(Log::Color::BrightRed,
		           "[pm4-ownership] submit capture incomplete: seq=%" PRIu64
		           " root=0x%016" PRIx64 " root_dw=%zu root_first=0x%08" PRIx32
		           " cmd(miss=%" PRIu32 ",malformed=%" PRIu32 ",ranges=%" PRIuPTR
		           ") const(miss=%" PRIu32 ",malformed=%" PRIu32 ",ranges=%" PRIuPTR ")\n",
		           capture_seq, reinterpret_cast<uint64_t>(submission.commands.data()),
		           submission.commands.size(),
		           submission.commands.empty() ? 0u : submission.commands.front(),
		           submission.command_execution.OwnedGuestRangeMisses(),
		           submission.command_execution.OwnedGuestRangeMalformed(),
		           submission.command_execution.OwnedGuestRangeCount(),
		           submission.constant_execution.OwnedGuestRangeMisses(),
		           submission.constant_execution.OwnedGuestRangeMalformed(),
		           submission.constant_execution.OwnedGuestRangeCount());
		const auto cmd_misses = submission.command_execution.DescribeOwnedGuestRangeMisses();
		if (!cmd_misses.empty()) {
			LOGF_COLOR(Log::Color::BrightRed, "[pm4-ownership] cmd %s\n", cmd_misses.c_str());
		}
		const auto const_misses = submission.constant_execution.DescribeOwnedGuestRangeMisses();
		if (!const_misses.empty()) {
			LOGF_COLOR(Log::Color::BrightRed, "[pm4-ownership] const %s\n", const_misses.c_str());
		}
	}
	// [DIAG cbsync] Fingerprint the borrowed guest memory as submitted; every later check compares
	// the guest memory against this.
	submission.diag_submit_cmd   = CbDiagFingerprintSpan(submission.diag_guest_cmd);
	submission.diag_submit_const = CbDiagFingerprintSpan(submission.diag_guest_const);
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	// [DIAG cbsync] Identity and enqueue order are attached to the submission object itself.
	submission.diag_id           = ++m_diag_submission_id;
	submission.diag_enqueue_seq  = ++m_diag_enqueue_seq;
	submission.diag_submit_depth = m_submission_count;
	submission.diag_submit_frame = m_done_num.load(std::memory_order_relaxed);
	if (submission.diag_submit_cmd.num_dw != 0 || submission.diag_submit_const.num_dw != 0) {
		LOGF("[DIAG cbsync] submit id=%" PRIu64 " seq=%" PRIu64 " kind=%s qid=%" PRIu32
		     " cmd=0x%016" PRIx64 " dw=%" PRIu32 " first=0x%08" PRIx32 " last=0x%08" PRIx32
		     " hash=0x%016" PRIx64 " sum=0x%016" PRIx64 " const=0x%016" PRIx64
		     " const_dw=%" PRIu32 " qdepth=%" PRIu32 " frame=%d\n",
		     submission.diag_id, submission.diag_enqueue_seq, submission.DiagKind(),
		     submission.queue_id, reinterpret_cast<uint64_t>(submission.diag_guest_cmd.data()),
		     submission.diag_submit_cmd.num_dw, submission.diag_submit_cmd.first,
		     submission.diag_submit_cmd.last, submission.diag_submit_cmd.hash,
		     submission.diag_submit_cmd.sum,
		     reinterpret_cast<uint64_t>(submission.diag_guest_const.data()),
		     submission.diag_submit_const.num_dw, submission.diag_submit_depth,
		     submission.diag_submit_frame);
	}
	// [vb-diag] TEMPORARY: capture the submit-time state of every VS vertex-buffer table this
	// stream references (bounded), before the submission is published to the GPU queue.
	VbDiagSnapshotSubmitTables(submission.commands, submission.diag_id);
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
	g_gpu_thread = true;
	g_gpu_state  = gpu;

	for (;;) {
		Submission                   submission;
		Common::UniqueFunction<void> command;
		bool                         has_submission = false;
		bool                         should_stop    = false;
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
					gpu->m_work_available.WaitFor(&gpu->m_queue_mutex, 100);
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
			command();

			Common::LockGuard lock(gpu->m_queue_mutex);
			gpu->m_processing = false;
			if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_idle.SignalAll();
			}
			continue;
		}

		EXIT_IF(!has_submission);
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
		if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
			gpu->m_idle.SignalAll();
		}
	}
}

bool GuestGpu::Process(Submission& submission) {
	const bool first_slice = !submission.started;
	auto&      cp          = GetProcessor(submission.queue_id);

	if (first_slice) {
		submission.started = true;
		cp.SetSubmitId(++m_submit_id);
		submission.diag_parse_seq = cp.GetSubmitId(); // [DIAG cbsync]
		cp.ResetDeCe();
		cp.SetFlip({});
	}

	// [DIAG cbsync] TEMPORARY: re-fingerprint the borrowed guest memory and compare it with the
	// fingerprint taken at submit time. Runs on the first slice and again on every resumption of a
	// suspended stream. This detects that the guest rewrote the memory it handed over; the parser
	// itself now reads the emulator-owned copy, so this no longer affects parsing. Read-only.
	if (submission.diag_submit_cmd.num_dw != 0 || submission.diag_submit_const.num_dw != 0) {
		const auto now_cmd     = CbDiagFingerprintSpan(submission.diag_guest_cmd);
		const auto now_const   = CbDiagFingerprintSpan(submission.diag_guest_const);
		const bool cmd_match   = CbDiagFingerprintsMatch(now_cmd, submission.diag_submit_cmd);
		const bool const_match = CbDiagFingerprintsMatch(now_const, submission.diag_submit_const);
		uint32_t   queue_depth = 0;
		{
			Common::LockGuard lock(m_queue_mutex);
			queue_depth = m_submission_count;
		}
		submission.diag_check_count++;
		const char* phase = first_slice ? "first" : "resume";
		LOGF("[DIAG cbsync] parse-%s id=%" PRIu64 " seq=%" PRIu64 " parse_seq=%" PRIu64
		     " kind=%s qid=%" PRIu32 " cmd=0x%016" PRIx64 " dw=%" PRIu32 " first=0x%08" PRIx32
		     " last=0x%08" PRIx32 " hash=0x%016" PRIx64 " sum=0x%016" PRIx64
		     " match=%s const_match=%s const=0x%016" PRIx64 " const_dw=%" PRIu32
		     " check=%" PRIu64 " qdepth=%" PRIu32 " frame=%d pending=%" PRIu32 "\n",
		     phase, submission.diag_id, submission.diag_enqueue_seq, submission.diag_parse_seq,
		     submission.DiagKind(), submission.queue_id,
		     reinterpret_cast<uint64_t>(submission.diag_guest_cmd.data()), now_cmd.num_dw,
		     now_cmd.first, now_cmd.last, now_cmd.hash, now_cmd.sum, cmd_match ? "yes" : "no",
		     const_match ? "yes" : "no",
		     reinterpret_cast<uint64_t>(submission.diag_guest_const.data()), now_const.num_dw,
		     submission.diag_check_count, queue_depth,
		     m_done_num.load(std::memory_order_relaxed),
		     m_pending_commands.load(std::memory_order_relaxed));
		if (!cmd_match) {
			LOGF("[DIAG cbsync] MISMATCH id=%" PRIu64 " phase=%s stream=cmd"
			     " submit_dw=%" PRIu32 " submit_first=0x%08" PRIx32 " submit_last=0x%08" PRIx32
			     " submit_hash=0x%016" PRIx64 " submit_sum=0x%016" PRIx64 " now_dw=%" PRIu32
			     " now_first=0x%08" PRIx32 " now_last=0x%08" PRIx32 " now_hash=0x%016" PRIx64
			     " now_sum=0x%016" PRIx64 "\n",
			     submission.diag_id, phase, submission.diag_submit_cmd.num_dw,
			     submission.diag_submit_cmd.first, submission.diag_submit_cmd.last,
			     submission.diag_submit_cmd.hash, submission.diag_submit_cmd.sum, now_cmd.num_dw,
			     now_cmd.first, now_cmd.last, now_cmd.hash, now_cmd.sum);
			CbDiagLogDiff(submission.diag_id, phase, "cmd", submission.diag_guest_cmd.data(),
			              submission.command_store.data(),
			              static_cast<uint32_t>(submission.command_store.size()),
			              reinterpret_cast<uint64_t>(submission.diag_guest_cmd.data()));
		}
		if (!const_match) {
			LOGF("[DIAG cbsync] MISMATCH id=%" PRIu64 " phase=%s stream=const"
			     " submit_dw=%" PRIu32 " submit_first=0x%08" PRIx32 " submit_last=0x%08" PRIx32
			     " submit_hash=0x%016" PRIx64 " submit_sum=0x%016" PRIx64 " now_dw=%" PRIu32
			     " now_first=0x%08" PRIx32 " now_last=0x%08" PRIx32 " now_hash=0x%016" PRIx64
			     " now_sum=0x%016" PRIx64 "\n",
			     submission.diag_id, phase, submission.diag_submit_const.num_dw,
			     submission.diag_submit_const.first, submission.diag_submit_const.last,
			     submission.diag_submit_const.hash, submission.diag_submit_const.sum,
			     now_const.num_dw, now_const.first, now_const.last, now_const.hash, now_const.sum);
			CbDiagLogDiff(submission.diag_id, phase, "const", submission.diag_guest_const.data(),
			              submission.constant_command_store.data(),
			              static_cast<uint32_t>(submission.constant_command_store.size()),
			              reinterpret_cast<uint64_t>(submission.diag_guest_const.data()));
		}
	} else if (first_slice) {
		// No command buffer (flip preparation / suspend point): still record when it is consumed,
		// so the pacing of the flip/suspend-point pipeline is visible next to the submissions.
		uint32_t queue_depth = 0;
		{
			Common::LockGuard lock(m_queue_mutex);
			queue_depth = m_submission_count;
		}
		LOGF("[DIAG cbsync] parse-first id=%" PRIu64 " seq=%" PRIu64 " parse_seq=%" PRIu64
		     " kind=%s qid=%" PRIu32 " qdepth=%" PRIu32 " frame=%d pending=%" PRIu32 " no-cmd\n",
		     submission.diag_id, submission.diag_enqueue_seq, submission.diag_parse_seq,
		     submission.DiagKind(), submission.queue_id, queue_depth,
		     m_done_num.load(std::memory_order_relaxed),
		     m_pending_commands.load(std::memory_order_relaxed));
	}

	cp.BufferInit();
	bool complete = true;

	switch (submission.type) {
		case SubmissionType::Graphics: {
			bool progressed = false;
			submission.constant_complete |= submission.constant_commands.empty();
			for (;;) {
				bool round_progress = false;
				if (!submission.constant_complete) {
					CbDiagBeginParse(submission.diag_id, submission.diag_guest_const,
					                 submission.constant_command_store);
					submission.constant_complete =
					    cp.Process(submission.constant_execution, submission.constant_commands) ==
					    Pm4ProcessResult::Complete;
					CbDiagEndParse();
					round_progress |= submission.constant_execution.MadeProgress();
				}
				cp.SetCeComplete(submission.constant_complete);
				if (!submission.command_complete) {
					CbDiagBeginParse(submission.diag_id, submission.diag_guest_cmd,
					                 submission.command_store);
					submission.command_complete =
					    cp.Process(submission.command_execution, submission.commands) ==
					    Pm4ProcessResult::Complete;
					CbDiagEndParse();
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
			break;
		}
		case SubmissionType::Compute: {
			const auto      num_dw = static_cast<uint32_t>(submission.commands.size());
			const auto*     buffer = submission.commands.data();
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
			CbDiagBeginParse(submission.diag_id, submission.diag_guest_cmd,
			                 submission.command_store);
			complete = cp.Process(submission.command_execution, submission.commands) ==
			           Pm4ProcessResult::Complete;
			CbDiagEndParse();
			if (submission.command_execution.MadeProgress()) {
				cp.BufferFlush();
			}
			break;
		}
		case SubmissionType::FlipPreparation:
			cp.PrepareCpuFlip(submission.flip_request_id);
			break;
		case SubmissionType::SuspendPoint:
			// Age caches at the frame boundary, not once per command list.
			m_renderer.RunGarbageCollector();
			cp.EmitGlobalBarrier();
			m_renderer.GetCommandScheduler().DeferPriorityOperation(
			    [ready = m_suspend_point_ready] { ready->release(); });
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

	ProcessPm4(execution);
	return execution.m_buffer_stack.empty() ? Pm4ProcessResult::Complete
	                                        : Pm4ProcessResult::Blocked;
}

void CommandProcessor::ProcessIndirectBuffer(std::span<const uint32_t> commands, bool chain) {
	EXIT_IF(g_current_execution == nullptr);
	EXIT_IF(!g_current_execution->m_next_buffer.empty());
	// `commands` is the submit-time owned snapshot looked up by address (see FindOwnedGuestRange);
	// it is not guest memory. It is copied once into the execution's buffer deque so that appending
	// later nested buffers never relocates this one, which keeps the spans held by BufferCursor
	// entries valid.
	g_current_execution->m_owned_buffers.emplace_back(commands.begin(), commands.end());
	g_current_execution->m_next_buffer = g_current_execution->m_owned_buffers.back();
	g_current_execution->m_chain       = chain;
}

std::span<const uint32_t> CommandProcessor::FindOwnedGuestRange(uint64_t address,
                                                                uint32_t num_dw) const {
	EXIT_IF(g_current_execution == nullptr);
	return g_current_execution->m_guest_ranges.Find(address, num_dw);
}

std::span<const uint32_t> FindOwnedGuestRangeForCurrentSubmit(uint64_t address, uint32_t num_dw) {
	if (g_current_execution == nullptr) {
		return {};
	}
	return g_current_execution->FindOwnedGuestRange(address, num_dw);
}

std::string CommandProcessor::DescribeOwnedGuestRangeMisses() const {
	EXIT_IF(g_current_execution == nullptr);
	return g_current_execution->DescribeOwnedGuestRangeMisses();
}

void CommandProcessor::SuspendPm4() {
	EXIT_IF(g_current_execution == nullptr);
	g_current_execution->m_suspended = true;
}

void CommandProcessor::ProcessPm4(Pm4Execution& execution) {
	while (!execution.m_buffer_stack.empty()) {
		if (g_gpu_state != nullptr) {
			g_gpu_state->ProcessCommands();
		}
		auto& cursor = execution.m_buffer_stack.back();
		EXIT_IF(cursor.offset_dw > cursor.commands.size());
		if (cursor.offset_dw == cursor.commands.size()) {
			execution.m_buffer_stack.pop_back();
			continue;
		}

		const auto* const packet        = cursor.commands.data() + cursor.offset_dw;
		const auto        total_dw      = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining_dw  = total_dw - cursor.offset_dw;
		const auto        packet_header = packet[0];
		const auto        opcode        = (packet_header >> 8u) & 0xffu;
		EXIT_NOT_IMPLEMENTED(remaining_dw > total_dw);

		// [DIAG cbsync] TEMPORARY: when walking the emulator-owned root stream, compare the dword
		// being decoded against the guest memory it was copied from. The parser reads the owned copy,
		// so this reports a guest rewrite of memory that is still nominally in flight, without
		// affecting parsing. Reported once per stream slice. Read-only.
		if (!g_cb_diag_header_logged && g_cb_diag_guest != nullptr &&
		    cursor.commands.data() == g_cb_diag_owned && cursor.offset_dw < g_cb_diag_num_dw) {
			const uint32_t guest_now = g_cb_diag_guest[cursor.offset_dw];
			if (guest_now != packet_header) {
				g_cb_diag_header_logged = true;
				LOGF("[DIAG cbsync] HEADER-CHANGED id=%" PRIu64 " offset=0x%05" PRIx32
				     " addr=0x%016" PRIx64 " parsed=0x%08" PRIx32 " guest_now=0x%08" PRIx32
				     " guest_base=0x%016" PRIx64 " dw=%" PRIu32 "\n",
				     g_cb_diag_id, cursor.offset_dw,
				     reinterpret_cast<uint64_t>(g_cb_diag_guest) +
				         static_cast<uint64_t>(cursor.offset_dw) * 4u,
				     packet_header, guest_now, reinterpret_cast<uint64_t>(g_cb_diag_guest),
				     g_cb_diag_num_dw);
			}
		}

		if (packet_header == 0x80000000u || Pm4::PacketSizeDw(packet_header) == 1u) {
			cursor.offset_dw++;
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
			cursor.offset_dw += packet_dw;
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
			// [DIAG cbsync] TEMPORARY: at the failure point, compare the guest memory with the
			// emulator-owned copy to localise the change. Read-only.
			if (g_cb_diag_guest != nullptr && base == g_cb_diag_owned) {
				CbDiagLogDiff(g_cb_diag_id, "crash", "cmd", g_cb_diag_guest, g_cb_diag_owned,
				              g_cb_diag_num_dw, reinterpret_cast<uint64_t>(g_cb_diag_guest));
			} else {
				LOGF("[DIAG cbsync] DIFF-SKIP reason=sub-buffer base=0x%016" PRIx64
				     " owned=0x%016" PRIx64 "\n",
				     reinterpret_cast<uint64_t>(base),
				     reinterpret_cast<uint64_t>(g_cb_diag_owned));
			}
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n",
			     total_dw - remaining_dw, packet_header);
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

	m_num_instances = num_instances;
}

void CommandProcessor::SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
                                      const volatile void* address, uint32_t count_in_dwords) {
	(void)count_in_dwords;
	uint64_t value = 0;

	switch (op) {
		case 0x00:
			m_predicate_skip = false;
			return;
		case 0x01: {
			EXIT_NOT_IMPLEMENTED(address == nullptr);
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
	if (op == 0x03) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 128) {
			LOGF("\t bool predication: addr=0x%016" PRIx64 ", value=0x%016" PRIx64
			     ", condition=%" PRIu32 ", skip=%u, wait_op=%" PRIu32 "\n",
			     reinterpret_cast<uint64_t>(address), value, condition,
			     m_predicate_skip ? 1u : 0u, wait_op);
		}
	}
}

void CommandProcessor::DrawIndex(DrawIndexArgs args) {
	args.index_type_and_size = m_index_type_and_size;
	if (args.instance_count == 0) {
		args.instance_count = m_num_instances;
	}
	if (GraphicsRunDebugDumpEnabled() && (args.base_vertex != 0 || args.first_instance != 0)) {
		LOGF("\t draw indexed offsets: base_vertex = %" PRId32 ", first_instance = %" PRIu32 "\n",
		     args.base_vertex, args.first_instance);
	}
	m_renderer.GetRenderExecutor().DrawIndex(m_submit_id, CurrentBuffer(), args);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count) {
	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(index_offset) * index_size);

	DrawIndex({.index_count = index_count, .index_addr = index_addr});
}

static void PatchIndirectDrawOffsets(CommandProcessor& cp, IndirectDrawRegisters registers,
                                     uint32_t vertex_offset, uint32_t instance_offset,
                                     uint32_t first_index) {
	const auto write = [&](uint32_t location, uint32_t value) {
		if (location == Pm4::SH_NOP) {
			return;
		}
		EXIT_NOT_IMPLEMENTED(location >= Pm4::SH_NUM ||
		                     g_hw_sh_indirect_func[location] == nullptr);
		g_hw_sh_indirect_func[location](cp, location, value);
	};
	write(registers.vertex_offset, vertex_offset);
	write(registers.instance_offset, instance_offset);
	write(registers.index_offset, first_index);
}

void CommandProcessor::DrawIndirect(uint32_t data_offset, IndirectDrawRegisters registers,
                                    uint32_t draw_initiator, bool indexed) {
	const auto args_size = indexed ? sizeof(DrawIndexedIndirectArgs) : sizeof(DrawIndirectArgs);
	DrawIndirectMulti(data_offset, 1, nullptr, args_size, registers, draw_initiator, indexed);
}

void CommandProcessor::DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
                                         const volatile uint32_t* count_addr,
                                         uint32_t stride_in_bytes, IndirectDrawRegisters registers,
                                         uint32_t draw_initiator, bool indexed) {
	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != (indexed ? 0u : 2u));
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

	uint64_t index_size = 0;
	if (indexed) {
		switch (m_index_type_and_size) {
			case 0: index_size = 2; break;
			case 1: index_size = 4; break;
			case 2: index_size = 1; break;
			default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
		}
	}

	for (uint32_t i = 0; i < draw_count; i++) {
		const auto args_addr = m_draw_indirect_args_base_addr + data_offset +
		                       static_cast<uint64_t>(i) * stride_in_bytes;

		if (!indexed) {
			auto* args = reinterpret_cast<const DrawIndirectArgs*>(args_addr);
			PatchIndirectDrawOffsets(*this, registers, args->start_vertex_location,
			                         args->start_instance_location, 0);
			m_num_instances = args->instance_count;
			DrawIndexAuto({.vertex_count   = args->vertex_count_per_instance,
			               .instance_count = args->instance_count,
			               .first_vertex   = args->start_vertex_location,
			               .first_instance = args->start_instance_location,
			               .offset_source  = DrawOffsetSource::IndirectArgs});
			continue;
		}

		auto* args = reinterpret_cast<const DrawIndexedIndirectArgs*>(args_addr);
		PatchIndirectDrawOffsets(*this, registers, args->base_vertex_location,
		                         args->start_instance_location, args->start_index_location);

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

		m_num_instances = args->instance_count;
		DrawIndex({.index_count    = index_count,
		           .index_addr     = index_addr,
		           .instance_count = args->instance_count,
		           .base_vertex    = static_cast<int32_t>(args->base_vertex_location),
		           .first_instance = args->start_instance_location,
		           .offset_source  = DrawOffsetSource::IndirectArgs});
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));

	uint32_t frame_num = 0;

	{
		frame_num = m_renderer.GetGpu().GetFrameNum();
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

		m_renderer.GetRenderExecutor().DispatchDirect(m_submit_id, CurrentBuffer(), thread_group_x,
		                                              thread_group_y, thread_group_z, mode, IsAsyncComputeQueue());
	}
}

void CommandProcessor::DispatchIndirect(uint64_t args_addr, uint32_t mode) {
	EXIT_NOT_IMPLEMENTED(args_addr == 0 || (args_addr & 3u) != 0);
	if ((mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0) {
		const auto* args = reinterpret_cast<const vk::DispatchIndirectCommand*>(args_addr);
		DispatchDirect(args->x, args->y, args->z, mode);
		return;
	}
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	m_renderer.GetRenderExecutor().DispatchIndirect(m_submit_id, CurrentBuffer(), args_addr, mode,
	                                                IsAsyncComputeQueue());
}

void CommandProcessor::DrawIndexAuto(DrawAutoArgs args) {
	if (args.instance_count == 0) {
		args.instance_count = m_num_instances;
	}
	m_renderer.GetRenderExecutor().DrawAuto(m_submit_id, CurrentBuffer(), args);
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index) {
	BufferFlush();

	m_renderer.GetVideoOut().WaitFlipDone(static_cast<int>(video_out_handle),
	                                      static_cast<int>(display_buffer_index));
}

template <typename T>
void CommandProcessor::WriteAtEndOfPipe(uint32_t cache_policy, uint32_t event_write_dest,
                                        uint32_t eop_event_type, uint32_t cache_action,
                                        uint32_t event_index, uint32_t event_write_source,
                                        void* dst_gpu_addr, T value, uint32_t interrupt_selector,
                                        uint32_t interrupt_context_id) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	auto& command = CurrentBuffer();

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
		case 0x01:
			if (!IsAsyncComputeQueue()) {
				TriggerEopEventAtEndOfPipe(interrupt_context_id);
				return;
			}
			with_interrupt = true;
			break;
		case 0x02: with_interrupt = true; break;
		default: EXIT("unknown interrupt_selector\n");
	}

	auto write32 = [&] {
		auto* dst  = static_cast<uint32_t*>(dst_gpu_addr);
		auto  data = static_cast<uint32_t>(value);
		EXIT_IF(dst == nullptr);
		(void)command.Handle();
		std::memcpy(dst, &data, sizeof(data));

		if (with_interrupt) {
			TriggerEopEventAtEndOfPipe(interrupt_context_id);
		}
	};

	switch (event_write_source) {
		case 0x01:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (eop_event_type == 0x2f && cache_action == 0x00 && event_index == 0x06) {
					auto* dst = static_cast<uint32_t*>(dst_gpu_addr);
					SynchronizeGpu();
					Sync::ReadGds(*m_renderer.GetBufferCache().GetGdsBuffer(), dst, value & 0xffffu,
					              value >> 16u);
					if (with_interrupt) {
						m_renderer.TriggerInterrupt(m_interrupt_event_id, interrupt_context_id);
					}
					return;
				}
			} else if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05) {
				write32();
				return;
			}
			break;
		case 0x02:
		case 0x04:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (event_write_source == 0x02 && eop_event_type == 0x2f && event_index == 0x06) {
					switch (cache_action) {
						case 0x00:
						case 0x38: write32(); return;
						default: break;
					}
				}
			} else {
				if (event_write_source == 0x04) {
					value = Sync::ReadReferenceClock();
				}
				auto write64 = [&] {
					auto* dst = static_cast<uint64_t*>(dst_gpu_addr);
					EXIT_IF(dst == nullptr);
					(void)command.Handle();
					std::memcpy(dst, &value, sizeof(value));

					if (with_interrupt) {
						TriggerEopEventAtEndOfPipe(interrupt_context_id);
					}
				};

				switch (cache_action) {
					case 0x00:
						switch (eop_event_type) {
							case 0x04:
								if (event_index == 0x05) {
									write64();
									return;
								}
								break;
							case 0x14:
							case 0x28:
							case 0x2f:
								if (event_index == 0x00) {
									write64();
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
							case 0x30:
								if (event_index == 0x00 && !with_interrupt) {
									write64();
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
								     event_index == 0x05) ||
								    (event_index == 0x00)) {
									write64();
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
								if (event_index == 0x00 && !with_interrupt) {
									write64();
									return;
								}
								break;
							case 0x2f:
								if (event_index == 0x06 && !with_interrupt) {
									write64();
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x3b:
						if (eop_event_type == 0x04 && event_index == 0x05 && with_interrupt) {
							write64();
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

void CommandProcessor::EmitGlobalBarrier() {
	Common::LockGuard lock(m_renderer.GetMutex());

	vk::MemoryBarrier2 barrier {};
	barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;

	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &barrier;
	GetScheduler().EndRendering();
	CurrentBuffer().Handle().pipelineBarrier2(dependency);
}

void CommandProcessor::TriggerEopEventAtEndOfPipe(uint32_t interrupt_context_id) {
	(void)CurrentBuffer().Handle();
	GetScheduler().DeferPriorityOperation(
	    [&renderer = m_renderer, event_id = m_interrupt_event_id, interrupt_context_id] {
		    renderer.TriggerInterrupt(event_id, interrupt_context_id);
	    });
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index,
                                    uint64_t event_address) {
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::TriggerEvent()\n"
		     "\t event_type  = 0x%08" PRIx32 "\n"
		     "\t event_index = 0x%08" PRIx32 "\n"
		     "\t address     = 0x%016" PRIx64 "\n",
		     event_type, event_index, event_address);
	}

	const auto valid_cache_event_index = event_index == 0x00000000 || event_index == 0x00000007;
	switch (event_type) {
		// CsPartialFlush, GsPartialFlush, PsPartialFlush.
		case 0x00000007:
		case 0x0000000f:
		case 0x00000010: EmitGlobalBarrier(); break;
		// CbDbDataWritebackInvalidate, CbDataWritebackInvalidate.
		case 0x00000016:
		case 0x00000031:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			EmitGlobalBarrier();
			break;
		// DbDataWritebackInvalidate, DbMetadataWritebackInvalidate, CbMetadataWritebackInvalidate.
		case 0x0000002a:
		case 0x0000002c:
		case 0x0000002e:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			EmitGlobalBarrier();
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
			static std::once_flag warning_once;
			std::call_once(warning_once, [] {
				std::printf("Warning: game uses occlusion queries, which are currently treated as "
				            "always visible; GPU usage may be higher and FPS may be lower.\n");
			});

			// Until host occlusion queries are implemented, publish an always-visible result. The
			// PS5 layout contains one interleaved begin/end pair per DB, and bit 63 marks a result
			// ready.
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

void CommandProcessor::Flip() {
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}

	auto& command = CurrentBuffer();
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	(void)command.Handle();
	GetScheduler().DeferPriorityOperation(
	    [&renderer = m_renderer, request] { renderer.GetVideoOut().CompleteFlip(request); });
	GetScheduler().Flush();
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value) {
	auto& command = CurrentBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n"
		     "\t dst_gpu_addr = 0x%016" PRIx64 "\n"
		     "\t value        = 0x%08" PRIx32 "\n",
		     reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	EXIT_IF(dst_gpu_addr == nullptr);
	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	(void)command.Handle();
	GetScheduler().DeferPriorityOperation(
	    [&renderer = m_renderer, request] { renderer.GetVideoOut().CompleteFlip(request); });
	GetScheduler().Flush();
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action,
                                         void* dst_gpu_addr, uint32_t value) {
	auto& command = CurrentBuffer();

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
	EXIT_IF(dst_gpu_addr == nullptr);
	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	(void)command.Handle();
	GetScheduler().DeferPriorityOperation(
	    [&renderer = m_renderer, event_id = m_interrupt_event_id, request] {
		    renderer.GetVideoOut().CompleteFlip(request);
		    renderer.TriggerInterrupt(event_id, 0);
	    });
	GetScheduler().Flush();
}

void CommandProcessor::PrepareCpuFlip(uint64_t request_id) {
	auto& command = CurrentBuffer();
	if (g_current_processor != nullptr) {
		EXIT("invalid graphics-thread CPU flip preparation\n");
	}
	struct ProcessorScope {
		explicit ProcessorScope(CommandProcessor& processor) { g_current_processor = &processor; }
		~ProcessorScope() { g_current_processor = nullptr; }
	};
	ProcessorScope processor_scope(*this);

	m_renderer.GetVideoOut().PrepareFlip(request_id, command);
	GetScheduler().DeferPriorityOperation(
	    [this, request_id] { m_renderer.GetVideoOut().CompleteFlip(request_id); });
	GetScheduler().Flush();
}

void CommandProcessor::SynchronizeGpu() {
	GetScheduler().Finish();
}

bool GuestGpu::IsGpuThread() noexcept {
	return g_gpu_thread;
}

} // namespace Libs::Graphics
