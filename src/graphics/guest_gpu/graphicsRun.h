#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "common/uniqueFunction.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class RenderContext;

class GuestGpu final {
public:
	explicit GuestGpu(RenderContext& renderer);
	~GuestGpu();
	KYTY_CLASS_NO_COPY(GuestGpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);

	void Submit(uint32_t* draw_commands, uint32_t draw_size_dw, uint32_t* constant_commands,
	            uint32_t constant_size_dw, bool trigger_agc_interrupt_on_done = false);
	void SubmitCompute(uint32_t queue, uint32_t* commands, uint32_t size_dw,
	                   bool trigger_agc_interrupt_on_done = false);
	void SubmitFlipPreparation(uint64_t request_id);
	void Done();
	[[nodiscard]] int GetFrameNum() const;

	[[nodiscard]] static bool IsGpuThread() noexcept;

private:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	class OwnedCmdBuffer {
	public:
		OwnedCmdBuffer() = default;
		explicit OwnedCmdBuffer(const uint32_t* data, uint32_t count);

		[[nodiscard]] bool     Empty() const noexcept { return m_words.empty(); }
		[[nodiscard]] uint32_t Size() const noexcept {
			return static_cast<uint32_t>(m_words.size());
		}
		[[nodiscard]] uint32_t* Data() noexcept { return m_words.data(); }

	private:
		std::vector<uint32_t> m_words;
	};

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
		uint64_t       flip_request_id               = 0;
	};

	void              Enqueue(Submission submission);
	void              WaitForIdle();
	void              ProcessCommands();
	bool              Process(Submission& submission);
	static void       ThreadRun(void* data);
	CommandProcessor& GetProcessor(uint32_t queue_id);

	RenderContext&                                 m_renderer;
	Common::Mutex                                  m_submission_mutex;
	Common::Mutex                                  m_queue_mutex;
	std::mutex                                     m_shutdown_mutex;
	Common::CondVar                                m_work_available;
	Common::CondVar                                m_idle;
	std::array<std::deque<Submission>, QueueCount> m_queues;
	std::deque<Common::UniqueFunction<void>>       m_commands;
	std::atomic_uint32_t                           m_pending_commands {0};
	uint32_t                                       m_next_queue        = 0;
	uint32_t                                       m_submission_count  = 0;
	bool                                           m_processing        = false;
	bool                                           m_graphics_done     = true;
	bool                                           m_accepting         = true;
	bool                                           m_stopping          = false;
	bool                                           m_shutdown_complete = false;

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;
	std::jthread    m_thread;

	friend class CommandProcessor;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
