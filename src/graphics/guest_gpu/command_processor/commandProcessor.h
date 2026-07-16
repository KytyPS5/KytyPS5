#ifndef GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMMAND_PROCESSOR_H
#define GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMMAND_PROCESSOR_H

#include "common/assert.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

class CommandProcessorGuestAccessScope;

inline constexpr uint32_t AcquireGcrGl2Writeback = 1u << 15u;

bool TestWaitRegMemValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func);

class CommandScheduler {
public:
	static constexpr int BuffersNum = 8;

	void SetQueue(int queue) { m_queue = queue; }
	int  Queue() const { return m_queue; }
	bool Active() const { return m_current >= 0 && m_current < BuffersNum; }
	void CheckActive() const { EXIT_IF(!Active()); }

	CommandBuffer* Current() const {
		CheckActive();
		EXIT_IF(m_buffers[m_current] == nullptr);
		return m_buffers[m_current];
	}

	void Init() {
		if (m_current >= 0) {
			return;
		}
		for (auto& buf: m_buffers) {
			EXIT_IF(buf != nullptr);
			buf = new CommandBuffer(m_queue);
		}
		m_current = 0;
		Current()->Begin();
	}

	void Flush() {
		SubmitCurrent();
		BeginNext();
	}

	CommandBuffer* FlushAndGetSubmitted() {
		auto* submitted = SubmitCurrent();
		BeginNext();
		return submitted;
	}

	void CopyBuffers(std::array<CommandBuffer*, BuffersNum>* out) const {
		EXIT_IF(out == nullptr);
		for (int i = 0; i < BuffersNum; i++) {
			auto* buf = m_buffers[i];
			EXIT_IF(buf == nullptr);
			(*out)[i] = buf;
		}
	}

	void WaitAll() {
		for (auto* buf: m_buffers) {
			EXIT_IF(buf == nullptr);
			buf->WaitForFenceAndReset();
		}
	}

private:
	CommandBuffer* SubmitCurrent() {
		auto* submitted = Current();
		submitted->End();
		submitted->Execute();
		return submitted;
	}

	void BeginNext() {
		m_current = (m_current + 1) % BuffersNum;
		Current()->WaitForFenceAndReset();
		Current()->Begin();
	}

	CommandBuffer* m_buffers[BuffersNum] = {};
	int            m_current             = -1;
	int            m_queue               = -1;
};

class CommandProcessor {
public:
	struct FlipInfo {
		int     handle    = 0;
		int     index     = 0;
		int     flip_mode = 0;
		int64_t flip_arg  = 0;
	};

	CommandProcessor() = default;
	~CommandProcessor() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(CommandProcessor);

	void Reset();

	void              BufferInit();
	void              BufferFlush();
	void              BufferFlushAndWait();
	void              BufferWait();
	[[nodiscard]] int BeginReadbackTransaction(bool mutex_preowned) {
		if (!mutex_preowned) {
			m_mutex.Lock();
		}
		m_scheduler.Flush();
		m_scheduler.WaitAll();
		return m_scheduler.Queue();
	}
	void EndReadbackTransaction(bool mutex_preowned) {
		if (!mutex_preowned) {
			m_mutex.Unlock();
		}
	}

	void RunLock() { m_run_mutex.Lock(); }
	void RunUnlock() { m_run_mutex.Unlock(); }

	HW::Context*    GetCtx() { return &m_ctx; }
	HW::UserConfig* GetUcfg() { return &m_ucfg; }
	HW::Shader*     GetShCtx() { return &m_sh_ctx; }

	void SetIndexType(uint32_t index_type_and_size);
	void SetIndexBaseAddress(uint64_t index_base_addr);
	void SetIndexBufferSize(uint32_t index_buffer_size);
	void SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr);
	void SetDispatchIndirectArgsBaseAddress(uint64_t dispatch_indirect_args_base_addr);
	void SetNumInstances(uint32_t num_instances);
	void DrawIndex(uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
	               uint32_t instance_count = 0, const void* object_ids = nullptr,
	               uint32_t render_target_slice_offset = 0, int32_t vertex_offset_add = 0,
	               uint32_t first_instance = 0);
	void DrawIndexOffset(uint32_t index_offset, uint32_t index_count, uint32_t flags);
	void DrawIndexAuto(uint32_t index_count, uint32_t flags,
	                   uint32_t render_target_slice_offset = 0, uint32_t instance_count = 1,
	                   uint32_t first_vertex = 0, uint32_t first_instance = 0);
	void DrawIndirect(uint32_t data_offset, uint32_t draw_initiator, bool indexed);
	void DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
	                       const volatile uint32_t* count_addr, uint32_t stride_in_bytes,
	                       uint32_t draw_initiator, bool indexed);
	void WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest,
	                        uint32_t eop_event_type, uint32_t cache_action, uint32_t event_index,
	                        uint32_t event_write_source, void* dst_gpu_addr, uint32_t value,
	                        uint32_t interrupt_selector, uint32_t interrupt_context_id = 0);
	void WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest,
	                        uint32_t eop_event_type, uint32_t cache_action, uint32_t event_index,
	                        uint32_t event_write_source, void* dst_gpu_addr, uint64_t value,
	                        uint32_t interrupt_selector, uint32_t interrupt_context_id = 0);
	void Flip();
	void Flip(void* dst_gpu_addr, uint32_t value);
	void FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action, void* dst_gpu_addr,
	                       uint32_t value);
	void SynchronizeGpu();
	void SynchronizeGpuRange(uint64_t vaddr, uint64_t size);
	void MemoryBarrier();
	void TriggerEopEventAtEndOfPipe(uint32_t interrupt_context_id);
	void RenderTextureBarrier(uint64_t vaddr, uint64_t size);
	void DepthStencilBarrier(uint64_t vaddr, uint64_t size);
	void DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y, uint32_t thread_group_z,
	                    uint32_t mode);
	void DispatchIndirect(uint32_t data_offset, uint32_t mode);
	void WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index);
	void TriggerEvent(uint32_t event_type, uint32_t event_index);

	void SetUserDataMarker(HW::UserSgprType type) { m_user_data_marker = type; }
	[[nodiscard]] HW::UserSgprType GetUserDataMarker() const { return m_user_data_marker; }
	void SetEmbeddedDataMarker(const uint32_t* buffer, uint32_t num_dw, uint32_t align) {}
	void PushMarker(const char* str) {}
	void PopMarker() {}

	void PrefetchL2(void* addr, uint32_t size) {}
	void ClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
	void ReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

	void ResetDeCe();
	void WaitCe();
	void WaitDeDiff(uint32_t diff);
	void IncremenetDe();
	void IncremenetCe();

	void WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num);
	void DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num);

	template <typename T>
	void WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t poll, uint32_t wait_op);
	void WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num, uint32_t write_control);
	void DmaData(uint8_t engine, uint8_t dst_sel, uint8_t dst_cache_policy,
	             uint64_t dst_address_or_offset, uint8_t src_sel, uint8_t src_cache_policy,
	             uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes,
	             uint8_t wait_for_previous, uint8_t write_confirm, uint8_t block_engine);
	void SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
	                    const volatile void* address, uint32_t count_in_dwords);
	[[nodiscard]] bool ShouldSkipPredicatedPackets() const { return m_predicate_skip; }

	void Run(uint32_t* data, uint32_t num_dw);

	void              SetQueue(int queue) { m_scheduler.SetQueue(queue); }
	[[nodiscard]] int GetQueue() const { return m_scheduler.Queue(); }

	[[nodiscard]] const FlipInfo& GetFlip() const { return m_flip; }
	void                          SetFlip(const FlipInfo& flip) { m_flip = flip; }

	[[nodiscard]] uint64_t GetSubmitId() const { return m_submit_id; }
	void                   SetSubmitId(uint64_t submit_id) { m_submit_id = submit_id; }

private:
	friend class CommandProcessorGuestAccessScope;

	struct Counter {
		Common::Mutex   mutex;
		Common::CondVar cond_var;
		uint32_t        value = 0;
	};

	template <typename T>
	void WriteAtEndOfPipe(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type,
	                      uint32_t cache_action, uint32_t event_index, uint32_t event_write_source,
	                      void* dst_gpu_addr, T value, uint32_t interrupt_selector,
	                      uint32_t interrupt_context_id);

	CommandBuffer*      CurrentBuffer() { return m_scheduler.Current(); }
	void                CheckBuffer() const { m_scheduler.CheckActive(); }
	GpuResourceManager* GetGpuResources() const {
		EXIT_IF(g_render_ctx == nullptr);
		return g_render_ctx->GetGpuResources();
	}

	HW::Context      m_ctx;
	HW::UserConfig   m_ucfg;
	HW::Shader       m_sh_ctx;
	HW::UserSgprType m_user_data_marker                 = HW::UserSgprType::Unknown;
	uint32_t         m_index_type_and_size              = 0;
	uint32_t         m_index_buffer_size                = 0;
	uint64_t         m_index_base_addr                  = 0;
	uint64_t         m_draw_indirect_args_base_addr     = 0;
	uint64_t         m_dispatch_indirect_args_base_addr = 0;
	uint32_t         m_num_instances                    = 1;

	Common::Mutex m_mutex;
	Common::Mutex m_run_mutex;

	CommandScheduler m_scheduler;

	Counter m_de_counter;
	Counter m_ce_counter;

	uint32_t m_const_ram[0x3000] = {0};

	FlipInfo m_flip;
	uint64_t m_submit_id      = 0;
	bool     m_predicate_skip = false;
};

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMMAND_PROCESSOR_H
