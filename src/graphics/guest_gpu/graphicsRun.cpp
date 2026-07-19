#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/asyncJob.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
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
#include <list>
#include <thread>
#include <vector>

namespace Libs::Graphics {

static thread_local CommandProcessor* g_current_run_cp         = nullptr;
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
	std::vector<uint32_t> storage;
	uint32_t*             data   = nullptr;
	uint32_t              num_dw = 0;

	OwnedCmdBuffer() = default;
	OwnedCmdBuffer(const uint32_t* src, uint32_t count) { Assign(src, count); }

	OwnedCmdBuffer(const OwnedCmdBuffer& other): storage(other.storage), num_dw(other.num_dw) {
		data = (storage.empty() ? other.data : storage.data());
	}

	OwnedCmdBuffer& operator=(const OwnedCmdBuffer& other) {
		if (this != &other) {
			storage = other.storage;
			num_dw  = other.num_dw;
			data    = (storage.empty() ? other.data : storage.data());
		}
		return *this;
	}

	OwnedCmdBuffer(OwnedCmdBuffer&& other) noexcept
	    : storage(std::move(other.storage)), data(other.data), num_dw(other.num_dw) {
		data = (storage.empty() ? other.data : storage.data());
	}

	OwnedCmdBuffer& operator=(OwnedCmdBuffer&& other) noexcept {
		if (this != &other) {
			storage = std::move(other.storage);
			num_dw  = other.num_dw;
			data    = (storage.empty() ? other.data : storage.data());
		}
		return *this;
	}

	void Assign(const uint32_t* src, uint32_t count) {
		num_dw = count;
		if (src != nullptr && count != 0) {
			storage.assign(src, src + count);
			data = storage.data();
		} else {
			storage.clear();
			data = nullptr;
		}
	}
};

class GraphicsRing {
public:
	GraphicsRing(): m_draw_job("Thread_Gfx_Draw"), m_constant_job("Thread_Gfx_Const") {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	}
	~GraphicsRing() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(GraphicsRing);

	void Submit(OwnedCmdBuffer draw_buffer, OwnedCmdBuffer const_buffer, int handle, int index,
	            int flip_mode, int64_t flip_arg, bool trigger_agc_interrupt_on_done);
	void SubmitFlipPreparation();
	void Done();
	void WaitForIdle();
	bool IsIdle();

	void SetCp(CommandProcessor* cp) {
		m_cp = cp;
		Start();
	}

private:
	void Start() {
		Common::Thread t(ThreadBatchRun, this);
		t.Detach();
	}

	struct CmdBatch {
		OwnedCmdBuffer draw_buffer;
		OwnedCmdBuffer const_buffer;

		CommandProcessor::FlipInfo flip;
		bool                       trigger_agc_interrupt_on_done = false;
		bool                       prepare_cpu_flip              = false;
	};

	static void ThreadBatchRun(void* data);

	CmdBatch GetCmdBatch();

	Common::Mutex       m_mutex;
	Common::CondVar     m_cond_var;
	Common::CondVar     m_idle_cond_var;
	std::list<CmdBatch> m_cmd_batches;
	bool                m_done = true;
	bool                m_idle = true;

	AsyncJob m_draw_job;
	AsyncJob m_constant_job;

	CommandProcessor* m_cp = nullptr;
};

class ComputeRing {
public:
	ComputeRing() = default;
	~ComputeRing() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(ComputeRing);

	void Submit(OwnedCmdBuffer buffer, bool trigger_agc_interrupt_on_done);
	void Done();
	void WaitForIdle();
	bool IsIdle();

	void SetCp(CommandProcessor* cp) {
		m_cp = cp;
		Start();
	}

	[[nodiscard]] int GetQueueId() const { return m_queue_id; }
	void              SetQueueId(int id) { m_queue_id = id; }

private:
	void Start() {
		Common::Thread t(ThreadRun, this);
		t.Detach();
	}

	static void ThreadRun(void* data);

	Common::Mutex   m_mutex;
	Common::CondVar m_cond_var;
	Common::CondVar m_idle_cond_var;
	bool            m_done = true;
	bool            m_idle = true;

	CommandProcessor* m_cp       = nullptr;
	int               m_queue_id = -1;

	struct DirectBatch {
		OwnedCmdBuffer buffer;
		bool           trigger_agc_interrupt_on_done = false;
	};

	std::list<DirectBatch> m_direct_batches;
};

class Gpu {
public:
	Gpu() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		Init();
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
	void Init();
	void WaitLocked();

	ComputeRing* GetRing(uint32_t ring_id);

	Common::Mutex m_mutex;

	CommandProcessor* m_gfx_cp   = nullptr;
	GraphicsRing*     m_gfx_ring = nullptr;

	CommandProcessor* m_compute_cp[8]    = {};
	ComputeRing*      m_compute_ring[64] = {};

	std::atomic_int m_done_num = 0;
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
	OwnedCmdBuffer draw_buffer(cmd_draw_buffer, num_draw_dw);
	OwnedCmdBuffer const_buffer(cmd_const_buffer, num_const_dw);
	GpuMutexLock   lock(m_mutex);

	m_gfx_ring->Submit(std::move(draw_buffer), std::move(const_buffer), 0, 0, 0, 0,
	                   trigger_agc_interrupt_on_done);
}

void Gpu::SubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
                        bool trigger_agc_interrupt_on_done) {
	OwnedCmdBuffer buffer(cmd_buffer, num_dw);
	GpuMutexLock   lock(m_mutex);

	constexpr uint32_t compute_queue_base = 0x20u;
	constexpr uint32_t compute_queue_num  = 7u * 8u;
	EXIT_NOT_IMPLEMENTED(queue < compute_queue_base ||
	                     queue >= compute_queue_base + compute_queue_num);

	uint32_t compute_queue = queue - compute_queue_base;
	uint32_t pipe_id       = (compute_queue >> 3u) & 0x7u;
	uint32_t queue_id      = compute_queue & 0x7u;
	EXIT_NOT_IMPLEMENTED(pipe_id >= 7u);
	EXIT_NOT_IMPLEMENTED(queue_id >= 8u);

	uint32_t ring_id = compute_queue + 1u;

	auto* ring = GetRing(ring_id);

	ring->Submit(std::move(buffer), trigger_agc_interrupt_on_done);
}

void Gpu::SubmitFlipPreparation() {
	GpuMutexLock lock(m_mutex);
	m_gfx_ring->SubmitFlipPreparation();
}

void Gpu::Done() {
	GraphicsRing*     gfx_ring = nullptr;
	CommandProcessor* gfx_cp   = nullptr;
	ComputeRing*      compute_rings[64] {};
	CommandProcessor* compute_cps[8] {};

	{
		GpuMutexLock lock(m_mutex);

		gfx_ring = m_gfx_ring;
		gfx_cp   = m_gfx_cp;

		std::copy(std::begin(m_compute_ring), std::end(m_compute_ring), std::begin(compute_rings));
		std::copy(std::begin(m_compute_cp), std::end(m_compute_cp), std::begin(compute_cps));

		m_done_num++;
	}

	if (gfx_ring != nullptr) {
		gfx_ring->Done();
	}
	for (auto& cr: compute_rings) {
		if (cr != nullptr) {
			cr->Done();
		}
	}
	if (gfx_ring != nullptr) {
		gfx_ring->WaitForIdle();
	}
	if (gfx_cp != nullptr) {
		gfx_cp->BufferWait();
	}
	for (auto& cr: compute_rings) {
		if (cr != nullptr) {
			cr->WaitForIdle();
		}
	}
	for (auto& cp: compute_cps) {
		if (cp != nullptr) {
			cp->BufferWait();
		}
	}
}

int Gpu::GetFrameNum() {
	// Common::LockGuard lock(m_mutex);

	return m_done_num;
}

void Gpu::WaitLocked() {
	GraphicsRing*     gfx_ring = nullptr;
	CommandProcessor* gfx_cp   = nullptr;
	ComputeRing*      compute_rings[64] {};
	CommandProcessor* compute_cps[8] {};

	gfx_ring = m_gfx_ring;
	gfx_cp   = m_gfx_cp;
	std::copy(std::begin(m_compute_ring), std::end(m_compute_ring), std::begin(compute_rings));
	std::copy(std::begin(m_compute_cp), std::end(m_compute_cp), std::begin(compute_cps));

	if (gfx_ring != nullptr) {
		gfx_ring->WaitForIdle();
	}
	if (gfx_cp != nullptr) {
		gfx_cp->BufferWait();
	}
	for (auto& cr: compute_rings) {
		if (cr != nullptr) {
			cr->WaitForIdle();
		}
	}
	for (auto& cp: compute_cps) {
		if (cp != nullptr) {
			cp->BufferWait();
		}
	}
}

void Gpu::Init() {
	EXIT_IF(m_gfx_cp != nullptr);
	EXIT_IF(m_gfx_ring != nullptr);

	m_gfx_cp   = new CommandProcessor;
	m_gfx_ring = new GraphicsRing;
	m_gfx_cp->SetQueue(GraphicContext::QUEUE_GFX);
	m_gfx_ring->SetCp(m_gfx_cp);

	EXIT_IF(GraphicContext::QUEUE_COMPUTE_NUM < 8);
}

ComputeRing* Gpu::GetRing(uint32_t ring_id) {
	int v        = static_cast<int>(ring_id - 1);
	int pipe_id  = v / 8;
	int queue_id = v % 8;

	if (m_compute_cp[pipe_id] == nullptr) {
		m_compute_cp[pipe_id] = new CommandProcessor;
		m_compute_cp[pipe_id]->SetQueue(GraphicContext::QUEUE_COMPUTE_START + pipe_id);
	}

	if (m_compute_ring[v] == nullptr) {
		m_compute_ring[v] = new ComputeRing;
		m_compute_ring[v]->SetQueueId(queue_id);
		m_compute_ring[v]->SetCp(m_compute_cp[pipe_id]);
	}

	return m_compute_ring[v];
}

void CommandProcessor::SetQueue(int queue) {
	Common::LockGuard lock(m_mutex);
	if (queue < 0 || queue >= GraphicContext::QUEUES_NUM ||
	    (m_processors[queue] != nullptr && m_processors[queue] != this)) {
		EXIT("invalid command-processor queue registration: queue=%d owner=%p\n", queue,
		     static_cast<const void*>(m_processors[queue]));
	}
	m_scheduler.SetQueue(queue);
	m_processors[queue] = this;
}

void CommandProcessor::FinishReadbackTransaction() {
	if (GraphicsRunCurrentCommandProcessor() == nullptr || !m_readback_active) {
		EXIT("GPU readback finish requires a command-processor thread\n");
	}
	if (m_readback_finished) {
		return;
	}
	FinishCommandProcessors();
	m_readback_finished = true;
}

void CommandProcessor::FinishCommandProcessors() {
	std::array<CommandProcessor*, GraphicContext::QUEUES_NUM> processors {};
	uint32_t                                                  processor_count = 0;
	for (auto* processor: m_processors) {
		if (processor == nullptr ||
		    std::find(processors.begin(), processors.begin() + processor_count, processor) !=
		        processors.begin() + processor_count) {
			continue;
		}
		processors[processor_count++] = processor;
		processor->m_scheduler.SubmitForReadback();
	}
	for (uint32_t i = 0; i < processor_count; i++) {
		processors[i]->m_scheduler.ResumeAfterReadback();
	}
}

void CommandProcessor::Reset() {
	BufferWait();

	Common::LockGuard lock(m_mutex);

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
	Common::LockGuard lock(m_mutex);
	m_scheduler.Init();
}

void CommandProcessor::BufferFlush() {
	Common::LockGuard lock(m_mutex);
	m_scheduler.Flush();
}

void CommandProcessor::BufferFlushAndWait() {
	CommandBuffer* submitted = nullptr;

	{
		Common::LockGuard lock(m_mutex);
		submitted = m_scheduler.FlushAndGetSubmitted();
	}

	submitted->WaitForFence();
}

void CommandProcessor::BufferWait() {
	if (g_current_run_cp != this) {
		m_run_mutex.Lock();
		BufferInit();

		std::array<CommandBuffer*, CommandScheduler::BuffersNum> buffers {};
		{
			Common::LockGuard lock(m_mutex);
			m_scheduler.CopyBuffers(&buffers);
		}

		for (auto* buf: buffers) {
			buf->WaitForFenceAndReset();
		}

		m_run_mutex.Unlock();
		return;
	}

	BufferInit();

	Common::LockGuard lock(m_mutex);
	m_scheduler.WaitAll();
}

void CommandProcessor::ResetDeCe() {
	m_de_counter.mutex.Lock();
	m_de_counter.value = 0;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
	m_ce_counter.mutex.Lock();
	m_ce_counter.value = 0;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WaitCe() {
	m_de_counter.mutex.Lock();
	auto de_value = m_de_counter.value;
	m_de_counter.mutex.Unlock();

	m_ce_counter.mutex.Lock();
	while (!(m_ce_counter.value > de_value)) {
		m_ce_counter.cond_var.Wait(&m_ce_counter.mutex);
	}
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WaitDeDiff(uint32_t diff) {
	m_ce_counter.mutex.Lock();
	auto ce_value = m_ce_counter.value;
	m_ce_counter.mutex.Unlock();

	m_de_counter.mutex.Lock();
	while (!(ce_value - m_de_counter.value < diff)) {
		m_de_counter.cond_var.Wait(&m_de_counter.mutex);
	}
	m_de_counter.mutex.Unlock();
}

void CommandProcessor::IncremenetDe() {
	BufferFlush();
	BufferWait();

	m_de_counter.mutex.Lock();
	m_de_counter.value++;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
}

void CommandProcessor::IncremenetCe() {
	m_ce_counter.mutex.Lock();
	m_ce_counter.value++;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num) {
	Common::LockGuard lock(m_mutex);

	memcpy(m_const_ram + offset / 4, src, static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num) {
	Common::LockGuard lock(m_mutex);

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

static void YieldCommandProcessorWait(uint32_t poll_interval_cycles) noexcept {
	// PS5 specifies GPU poll cycles, not host microseconds. Yield at the scheduler boundary now;
	// a future resumable guest GPU scheduler can replace this one function without touching polling
	// or BufferCache coherence.
	(void)poll_interval_cycles;
	std::this_thread::yield();
}

template <typename T>
void CommandProcessor::WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t poll,
                                  uint32_t wait_op) {
	EXIT_IF(addr == nullptr);
	if ((wait_op & ~1u) != 0) {
		EXIT("unsupported wait_reg_mem operation: 0x%08" PRIx32 "\n", wait_op);
	}

	const auto addr_value = reinterpret_cast<uint64_t>(addr);
	const auto log_width  = static_cast<int>(sizeof(T) * 2u);
	const auto bits       = static_cast<unsigned>(sizeof(T) * 8u);

	uint64_t spin_count = 0;
	for (;;) {
		const auto value = *addr;
		if (TestWaitRegMemValue(value, ref, mask, func)) {
			break;
		}
		if ((++spin_count % 100000u) == 0) {
			LOGF("\t wait_reg_mem%u still waiting: addr = 0x%016" PRIx64 ", value = 0x%0*" PRIx64
			     ", ref = 0x%0*" PRIx64 ", mask = 0x%0*" PRIx64 ", func = %" PRIu32 "\n",
			     bits, addr_value, log_width, static_cast<uint64_t>(value), log_width,
			     static_cast<uint64_t>(ref), log_width, static_cast<uint64_t>(mask), func);
		}
		YieldCommandProcessorWait(poll);
	}
}

template void CommandProcessor::WaitRegMem<uint32_t>(uint32_t, const uint32_t*, uint32_t, uint32_t,
                                                     uint32_t, uint32_t);
template void CommandProcessor::WaitRegMem<uint64_t>(uint32_t, const uint64_t*, uint64_t, uint64_t,
                                                     uint32_t, uint32_t);

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num,
                                 uint32_t write_control) {
	Common::LockGuard lock(m_mutex);

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
	Common::LockGuard lock(m_mutex);
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
	Common::LockGuard lock(m_mutex);

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
		GetGpuResources()->FillBuffer(
		    CurrentBuffer(), dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu));
		return;
	}
	if (src_memory) {
		GetGpuResources()->CopyBuffer(CurrentBuffer(), dst_address_or_offset,
		                              src_address_or_offset_or_immediate, num_bytes);
		return;
	}
	EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
}

void GraphicsRing::Submit(OwnedCmdBuffer draw_buffer, OwnedCmdBuffer const_buffer, int handle,
                          int index, int flip_mode, int64_t flip_arg,
                          bool trigger_agc_interrupt_on_done) {
	EXIT_IF(m_cp == nullptr);

	Common::LockGuard lock(m_mutex);

	WindowWaitForGraphicInitialized();
	GraphicsRenderCreateContext();

	if (m_done) {
		while (!m_idle) {
			m_idle_cond_var.Wait(&m_mutex);
		}
		m_done = false;

		m_cp->Reset();
	}

	auto& buf                         = m_cmd_batches.emplace_back();
	buf.draw_buffer                   = std::move(draw_buffer);
	buf.const_buffer                  = std::move(const_buffer);
	buf.flip.handle                   = handle;
	buf.flip.index                    = index;
	buf.flip.flip_mode                = flip_mode;
	buf.flip.flip_arg                 = flip_arg;
	buf.trigger_agc_interrupt_on_done = trigger_agc_interrupt_on_done;

	m_idle = false;
	m_cond_var.Signal();
}

void GraphicsRing::SubmitFlipPreparation() {
	EXIT_IF(m_cp == nullptr);
	Common::LockGuard lock(m_mutex);

	WindowWaitForGraphicInitialized();
	GraphicsRenderCreateContext();
	if (m_done) {
		while (!m_idle) {
			m_idle_cond_var.Wait(&m_mutex);
		}
		m_done = false;
		m_cp->Reset();
	}

	auto& batch            = m_cmd_batches.emplace_back();
	batch.prepare_cpu_flip = true;
	m_idle                 = false;
	m_cond_var.Signal();
}

void GraphicsRing::Done() {
	Common::LockGuard lock(m_mutex);
	if (m_done) {
		while (!m_idle) {
			m_idle_cond_var.Wait(&m_mutex);
		}
	}
	m_done = true;
}

void GraphicsRing::WaitForIdle() {
	Common::LockGuard lock(m_mutex);
	while (!m_idle) {
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool GraphicsRing::IsIdle() {
	Common::LockGuard lock(m_mutex);
	return m_idle;
}

GraphicsRing::CmdBatch GraphicsRing::GetCmdBatch() {
	Common::LockGuard lock(m_mutex);

	while (m_cmd_batches.empty()) {
		m_idle = true;
		m_idle_cond_var.Signal();

		m_cond_var.Wait(&m_mutex);
	}

	m_idle = false;

	CmdBatch buf = std::move(m_cmd_batches.front());
	m_cmd_batches.pop_front();

	return buf;
}

void GraphicsRing::ThreadBatchRun(void* data) {
	EXIT_IF(data == nullptr);

	static std::atomic_uint64_t seq = 0;

	auto* ring = static_cast<GraphicsRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);

	for (;;) {
		CmdBatch buf = ring->GetCmdBatch();

		cp->RunLock();
		{
			cp->BufferInit();
			cp->SetSubmitId(++seq);
			if (buf.prepare_cpu_flip) {
				cp->PrepareCpuFlip();
			} else {
				cp->ResetDeCe();
				cp->SetFlip(buf.flip);
				ring->m_draw_job.Execute(
				    [cp, buf] { cp->Run(buf.draw_buffer.data, buf.draw_buffer.num_dw); });
				ring->m_constant_job.Execute(
				    [cp, buf] { cp->Run(buf.const_buffer.data, buf.const_buffer.num_dw); });
				ring->m_draw_job.Wait();
				ring->m_constant_job.Wait();
				cp->BufferFlush();
				if (buf.trigger_agc_interrupt_on_done) {
					Sync::TriggerEopEvent(0);
				}
			}
		}
		cp->RunUnlock();
	}
}

void Gpu::PauseSubmissions() {
	if (g_gpu_mutex_owned) {
		EXIT("GPU submissions are already paused by this thread\n");
	}
	g_gpu_mutex_owned = true;
	m_mutex.Lock();
	WaitLocked();
	LabelDrain();
}

void Gpu::ResumeSubmissions() {
	if (!g_gpu_mutex_owned) {
		EXIT("GPU submissions resumed without an active pause\n");
	}
	m_mutex.Unlock();
	g_gpu_mutex_owned = false;
}

void ComputeRing::ThreadRun(void* data) {
	EXIT_IF(data == nullptr);

	auto* ring = static_cast<ComputeRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);

	KYTY_PROFILER_THREAD("Thread_Compute");

	ring->m_mutex.Lock();

	for (;;) {
		while (ring->m_direct_batches.empty()) {
			ring->m_idle = true;
			ring->m_idle_cond_var.Signal();
			ring->m_cond_var.Wait(&ring->m_mutex);
		}

		ring->m_idle = false;

		uint32_t*   buffer                        = nullptr;
		uint32_t    num_dw                        = 0;
		bool        trigger_agc_interrupt_on_done = false;
		DirectBatch direct_batch;

		direct_batch = std::move(ring->m_direct_batches.front());
		ring->m_direct_batches.pop_front();
		buffer                        = direct_batch.buffer.data;
		num_dw                        = direct_batch.buffer.num_dw;
		trigger_agc_interrupt_on_done = direct_batch.trigger_agc_interrupt_on_done;

		static std::atomic<uint32_t> compute_batch_log_count {0};
		if (num_dw <= 128 && buffer != nullptr && compute_batch_log_count.fetch_add(1) < 32) {
			LOGF("compute direct batch: queue=%d, data=0x%016" PRIx64 ", num_dw=%" PRIu32 "\n",
			     ring->m_queue_id, reinterpret_cast<uint64_t>(buffer), num_dw);
			for (uint32_t i = 0; i < std::min<uint32_t>(num_dw, 16); i++) {
				LOGF("\t compute[%02" PRIu32 "] = 0x%08" PRIx32 "\n", i, buffer[i]);
			}
		}

		ring->m_mutex.Unlock();

		cp->RunLock();
		{
			cp->BufferInit();
			cp->ResetDeCe();

			GraphicsDbgDumpDcb("cc", num_dw, buffer);

			cp->Run(buffer, num_dw);

			cp->BufferFlush();
			if (trigger_agc_interrupt_on_done) {
				Sync::TriggerAgcUserInterrupt();
			}
		}
		cp->RunUnlock();

		ring->m_mutex.Lock();
	}
}

void ComputeRing::Submit(OwnedCmdBuffer buffer, bool trigger_agc_interrupt_on_done) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(buffer.data == nullptr);
	EXIT_IF(buffer.num_dw == 0);

	WindowWaitForGraphicInitialized();
	GraphicsRenderCreateContext();

	if (m_done) {
		m_done = false;
	}

	DirectBatch batch {};
	batch.buffer                        = std::move(buffer);
	batch.trigger_agc_interrupt_on_done = trigger_agc_interrupt_on_done;
	m_direct_batches.push_back(std::move(batch));

	m_idle = false;
	m_cond_var.Signal();
}

void ComputeRing::Done() {
	Common::LockGuard lock(m_mutex);
	m_done = true;
}

void ComputeRing::WaitForIdle() {
	Common::LockGuard lock(m_mutex);
	while (!m_idle) {
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool ComputeRing::IsIdle() {
	Common::LockGuard lock(m_mutex);
	return m_idle;
}

void CommandProcessor::Run(uint32_t* data, uint32_t num_dw) {
	KYTY_PROFILER_BLOCK("CommandProcessor::Run");

	struct RunScope {
		explicit RunScope(CommandProcessor* cp): prev(g_current_run_cp) { g_current_run_cp = cp; }
		~RunScope() { g_current_run_cp = prev; }

		CommandProcessor* prev;
	} run_scope(this);

	auto* cmd = data;
	auto  dw  = num_dw;
	for (;;) {
		if (dw == 0) {
			break;
		}

		EXIT_NOT_IMPLEMENTED(dw > num_dw);

		auto cmd_id = *cmd++;

		if (cmd_id == 0x80000000u) {
			dw--;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(dw < 2);

		auto op = (cmd_id >> 8u) & 0xffu;
		if (GraphicsRunDebugDumpEnabled()) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     num_dw - dw, cmd_id, op, KYTY_PM4_LEN(cmd_id));
		}

		if ((cmd_id & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			auto packet_len = KYTY_PM4_LEN(cmd_id);
			EXIT_NOT_IMPLEMENTED(packet_len == 0 || packet_len > dw);
			static std::atomic<uint32_t> skip_log_count {0};
			if (skip_log_count.fetch_add(1) < 2048) {
				LOGF("\t predicated skip: op=0x%02" PRIx32 ", r=0x%02" PRIx32 ", len=%" PRIu32
				     ", packet=0x%016" PRIx64 ", cmd_id=0x%08" PRIx32 "\n",
				     op, KYTY_PM4_R(cmd_id), packet_len, reinterpret_cast<uint64_t>(cmd - 1),
				     cmd_id);
			}
			if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd_id) == Pm4::R_RELEASE_MEM && packet_len >= 7) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 128) {
					const auto dst = cmd[2] | (static_cast<uint64_t>(cmd[3]) << 32u);
					const auto val = cmd[4] | (static_cast<uint64_t>(cmd[5]) << 32u);
					LOGF("\t predicated skip: R_RELEASE_MEM dst=0x%016" PRIx64
					     ", value=0x%016" PRIx64 ", action=0x%08" PRIx32
					     ", gcr/data/int=0x%08" PRIx32 "\n",
					     dst, val, cmd[0], cmd[1]);
				}
			}
			cmd += packet_len - 1u;
			dw -= packet_len;
			continue;
		}

		auto pfunc = g_cp_op_func[op];

		if (pfunc == nullptr) {
			const auto offset = num_dw - dw;
			LOGF("unknown PM4 packet: data=0x%016" PRIx64 ", num_dw=%" PRIu32
			     ", offset=0x%05" PRIx32 ", current=0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(data), num_dw, offset,
			     reinterpret_cast<uint64_t>(cmd - 1));
			const auto dump_begin = (offset > 8 ? offset - 8 : 0);
			const auto dump_end   = std::min<uint32_t>(num_dw, offset + 16);
			for (uint32_t i = dump_begin; i < dump_end; i++) {
				LOGF("\t%05" PRIx32 "%s %08" PRIx32 "\n", i, (i == offset ? ":" : " "), data[i]);
			}
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n", num_dw - dw, cmd_id);
		}

		auto s = pfunc(this, cmd_id & ~1u, cmd, dw, num_dw);

		// LOGF("\t %05" PRIx32 ": %u\n", num_dw - dw, s);

		cmd += s;
		dw -= s + 1;
	}
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size) {
	Common::LockGuard lock(m_mutex);

	m_index_type_and_size = index_type_and_size & 0x3u;
}

void CommandProcessor::SetIndexBaseAddress(uint64_t index_base_addr) {
	Common::LockGuard lock(m_mutex);

	m_index_base_addr = index_base_addr;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_buffer_size) {
	Common::LockGuard lock(m_mutex);

	m_index_buffer_size = index_buffer_size;
}

void CommandProcessor::SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr) {
	Common::LockGuard lock(m_mutex);

	m_draw_indirect_args_base_addr = draw_indirect_args_base_addr;
}

void CommandProcessor::SetDispatchIndirectArgsBaseAddress(
    uint64_t dispatch_indirect_args_base_addr) {
	Common::LockGuard lock(m_mutex);

	m_dispatch_indirect_args_base_addr = dispatch_indirect_args_base_addr;
}

void CommandProcessor::SetNumInstances(uint32_t num_instances) {
	Common::LockGuard lock(m_mutex);

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

	Common::LockGuard lock(m_mutex);

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
	Common::LockGuard lock(m_mutex);

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
	const auto frame_num = GraphicsRunGetFrameNum();
	if (frame_num >= 480) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
			const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
			LOGF("QueuePoint DrawIndex: frame=%u submit=%" PRIu64
			     " queue=%d index_count=%u instances=%u prim=%u "
			     "es=0x%016" PRIx64 " ps=0x%016" PRIx64
			     " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32 " oa_space=0x%08" PRIx32 "\n",
			     frame_num, m_submit_id, m_scheduler.Queue(), index_count, instance_count,
			     m_ucfg.GetPrimType(), m_sh_ctx.GetVs().es_regs.data_addr,
			     m_sh_ctx.GetPs().ps_regs.data_addr, m_ucfg.GetGdsOaState().GetIndex(),
			     oa.IsCounterEnabled() ? "true" : "false", oa.GetAddressBytes(),
			     oa.GetSpaceAvailable());
		}
	}

	RenderDrawIndex(m_submit_id, CurrentBuffer(), &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size,
	                index_count, index_addr, flags, type, instance_count,
	                render_target_slice_offset, vertex_offset_add, first_instance);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count,
                                       uint32_t flags) {
	Common::LockGuard lock(m_mutex);

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

	RenderDrawIndex(m_submit_id, CurrentBuffer(), &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size,
	                index_count, index_addr, flags, 1, m_num_instances);
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
		Common::LockGuard lock(m_mutex);

		CheckBuffer();
		frame_num = GraphicsRunGetFrameNum();
		if (GraphicsRunDebugDumpEnabled()) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 1024) {
				const auto& cs = m_sh_ctx.GetCs().cs_regs;
				const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
				LOGF("QueuePoint DispatchDirect: frame=%u submit=%" PRIu64
				     " queue=%d groups=%ux%ux%u local=%ux%ux%u mode=0x%08" PRIx32
				     " wave=%u cs=0x%016" PRIx64 " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32
				     " oa_space=0x%08" PRIx32 "\n",
				     frame_num, m_submit_id, m_scheduler.Queue(), thread_group_x, thread_group_y,
				     thread_group_z, std::max(cs.num_thread_x, 1u), std::max(cs.num_thread_y, 1u),
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

		RenderDispatchDirect(m_submit_id, CurrentBuffer(), &m_ctx, &m_sh_ctx, thread_group_x,
		                     thread_group_y, thread_group_z, mode);
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
	Common::LockGuard lock(m_mutex);

	CheckBuffer();
	const auto frame_num = GraphicsRunGetFrameNum();
	if (frame_num >= 480) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
			const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
			LOGF("QueuePoint DrawIndexAuto: frame=%u submit=%" PRIu64
			     " queue=%d index_count=%u instances=%u prim=%u "
			     "first_vertex=%u first_instance=%u es=0x%016" PRIx64 " ps=0x%016" PRIx64
			     " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32 " oa_space=0x%08" PRIx32 "\n",
			     frame_num, m_submit_id, m_scheduler.Queue(), index_count, instance_count,
			     m_ucfg.GetPrimType(), first_vertex, first_instance,
			     m_sh_ctx.GetVs().es_regs.data_addr, m_sh_ctx.GetPs().ps_regs.data_addr,
			     m_ucfg.GetGdsOaState().GetIndex(), oa.IsCounterEnabled() ? "true" : "false",
			     oa.GetAddressBytes(), oa.GetSpaceAvailable());
		}
	}

	RenderDrawIndexAuto(m_submit_id, CurrentBuffer(), &m_ctx, &m_ucfg, &m_sh_ctx, index_count,
	                    flags, render_target_slice_offset, instance_count, first_vertex,
	                    first_instance);
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

	Common::LockGuard lock(m_mutex);

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
	Common::LockGuard lock(m_mutex);

	CheckBuffer();

	GraphicsRenderMemoryBarrier(CurrentBuffer());
}

void CommandProcessor::TriggerEopEventAtEndOfPipe(uint32_t interrupt_context_id) {
	Common::LockGuard lock(m_mutex);

	CheckBuffer();

	Sync::TriggerEopEventAtEndOfPipe(CurrentBuffer(), interrupt_context_id);
}

void CommandProcessor::RenderTextureBarrier(uint64_t vaddr, uint64_t size) {
	Common::LockGuard lock(m_mutex);

	CheckBuffer();

	GraphicsRenderTextureBarrier(CurrentBuffer(), vaddr, size);
}

void CommandProcessor::DepthStencilBarrier(uint64_t vaddr, uint64_t size) {
	Common::LockGuard lock(m_mutex);

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
	Common::LockGuard lock(m_mutex);

	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}

	auto* command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeOnlyFlip(m_submit_id, command, m_flip.handle, m_flip.index,
	                               m_flip.flip_mode, m_flip.flip_arg, request);
	m_scheduler.Flush();
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value) {
	Common::LockGuard lock(m_mutex);

	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n"
		     "\t dst_gpu_addr = 0x%016" PRIx64 "\n"
		     "\t value        = 0x%08" PRIx32 "\n",
		     reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto* command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithFlip32(m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr),
	                                 value, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                 m_flip.flip_arg, request);
	m_scheduler.Flush();
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action,
                                         void* dst_gpu_addr, uint32_t value) {
	Common::LockGuard lock(m_mutex);

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
	auto* command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithInterruptWriteBackFlip32(
	    m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr), value, m_flip.handle,
	    m_flip.index, m_flip.flip_mode, m_flip.flip_arg, request);
	m_scheduler.Flush();
}

void CommandProcessor::PrepareCpuFlip() {
	Common::LockGuard lock(m_mutex);
	CheckBuffer();
	if (g_current_run_cp != nullptr) {
		EXIT("invalid graphics-thread CPU flip preparation\n");
	}
	struct RunScope {
		explicit RunScope(CommandProcessor* cp) { g_current_run_cp = cp; }
		~RunScope() { g_current_run_cp = nullptr; }
	};
	RunScope run_scope(this);

	auto prepared_id = Presentation::DisplayBufferPrepareNextFlipOnGpu(CurrentBuffer());
	m_scheduler.Flush();
	Presentation::DisplayBufferCompleteFlipFromGpu(prepared_id);
}

void CommandProcessor::SynchronizeGpu() {
	Common::LockGuard lock(m_mutex);
	FinishCommandProcessors();
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
	if (g_gpu == nullptr || g_current_run_cp != nullptr || g_submission_pause_depth == UINT32_MAX) {
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
	return g_current_run_cp != nullptr;
}

CommandProcessor* GraphicsRunCurrentCommandProcessor() noexcept {
	return g_current_run_cp;
}

void GraphicsRunFinishCommandProcessors() {
	if (g_current_run_cp == nullptr) {
		EXIT("GPU readback finish requires a command-processor thread\n");
	}
	g_current_run_cp->FinishReadbackTransaction();
}

bool GraphicsRunSubmissionLockHeld() noexcept {
	return g_submission_pause_depth != 0;
}

bool GraphicsRunGpuLockHeld() noexcept {
	return g_gpu_mutex_owned;
}

} // namespace Libs::Graphics
