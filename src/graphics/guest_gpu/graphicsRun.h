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
#include <semaphore>
#include <span>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class RenderContext;

// [DIAG cbsync] TEMPORARY command-buffer mutation instrumentation.
// Holds a fingerprint of a borrowed guest command stream so it can be re-checked when that
// stream is actually parsed. Remove this struct, the `diag_*` members of `Submission` and the
// two GuestGpu counters together with every other "[DIAG cbsync]" marker.
struct CbDiagFingerprint {
	uint64_t hash   = 0; // FNV-1a 64 over every dword of the stream
	uint64_t sum    = 0; // wrapping checksum of every dword
	uint32_t first  = 0;
	uint32_t last   = 0;
	uint32_t num_dw = 0;
};

// [DIAG cbsync] TEMPORARY: true when the guest byte range [address, address + size) overlaps the
// command stream the GPU thread is currently parsing. Used by the AGC command-buffer patch
// helpers to attribute writes that corrupt an in-flight parse. Diagnostic-only, read-only.
bool GpuDiagWriteOverlapsActiveParse(uint64_t address, uint64_t size);

class GuestGpu final {
public:
	explicit GuestGpu(RenderContext& renderer);
	~GuestGpu();
	KYTY_CLASS_NO_COPY(GuestGpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);
	// Retries blocked submissions now instead of after the 100 ms stall timeout.
	void               Wake();

	// Submitted command memory is borrowed and must remain valid until GPU execution completes.
	void Submit(std::span<const uint32_t> commands);
	void SubmitCompute(uint32_t queue, std::span<const uint32_t> commands);
	void SubmitFlipPreparation(uint64_t request_id);
	// Insert an ordered graphics drain; only a previous suspend point can block the caller.
	void SuspendPoint();
	// Wait for guest command processing, including all compute queues (not native GPU completion).
	void              WaitForIdle();
	[[nodiscard]] int GetFrameNum() const;

	[[nodiscard]] static bool IsGpuThread() noexcept;

private:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t ComputeQueueBase     = 0x20;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	enum class SubmissionType { Graphics, Compute, FlipPreparation, SuspendPoint };

	struct Submission {
		SubmissionType            type     = SubmissionType::Graphics;
		uint32_t                  queue_id = 0;
		std::span<const uint32_t> commands;
		std::span<const uint32_t> constant_commands;
		// Emulator-owned copies of the submitted command streams. The guest that handed over this
		// borrowed memory can rewrite it at any time (it only paces itself against its own frame
		// boundaries), but Kyty parses asynchronously on the GPU thread, so parsing must not read
		// guest memory. `commands` / `constant_commands` are re-pointed at these copies in
		// Enqueue(); the vectors are never resized afterwards, so their backing buffers stay valid
		// while the submission is moved between queues.
		std::vector<uint32_t> command_store;
		std::vector<uint32_t> constant_command_store;
		Pm4Execution              command_execution;
		Pm4Execution              constant_execution;
		bool                      started           = false;
		bool                      command_complete  = false;
		bool                      constant_complete = false;
		bool                      blocked           = false;
		uint64_t                  flip_request_id   = 0;

		// [DIAG cbsync] TEMPORARY instrumentation state. It lives on the submission object, not
		// in a table keyed by guest pointer, so a buffer address reused by another submission
		// cannot alias this record.
		uint64_t          diag_id           = 0; // unique, stable for the lifetime of this submission
		uint64_t          diag_enqueue_seq  = 0; // order in which Enqueue() accepted this submission
		uint64_t          diag_parse_seq    = 0; // global submission id assigned when parsing starts
		uint64_t          diag_check_count  = 0; // number of parse-time fingerprint re-checks
		uint32_t          diag_submit_depth = 0; // m_submission_count at Enqueue() time
		int               diag_submit_frame = 0; // m_done_num at Enqueue() time
		CbDiagFingerprint diag_submit_cmd {};    // fingerprint of `commands` at submit time
		CbDiagFingerprint diag_submit_const {};  // fingerprint of `constant_commands` at submit time
		// [DIAG cbsync] The original borrowed guest spans, kept only so the diagnostic can still
		// observe the guest mutating them while the emulator-owned copies above are parsed. Never
		// executed; the parse source is `commands` / `constant_commands` (the copies).
		std::span<const uint32_t> diag_guest_cmd;
		std::span<const uint32_t> diag_guest_const;

		// [DIAG cbsync] readable kind for the diagnostic log.
		[[nodiscard]] const char* DiagKind() const {
			switch (type) {
				case SubmissionType::Graphics: return "dcb";
				case SubmissionType::Compute: return "acb";
				case SubmissionType::FlipPreparation: return "flip-prep";
				case SubmissionType::SuspendPoint: return "suspend-point";
			}
			return "unknown";
		}
		Pm4Execution              execution;
		bool                      started         = false;
		bool                      blocked         = false;
		uint64_t                  flip_request_id = 0;
	};

	void              Enqueue(Submission submission);
	void              ProcessCommands();
	bool              Process(Submission& submission);
	static void       ThreadRun(void* data);
	CommandProcessor& GetProcessor(uint32_t queue_id);

	RenderContext&                                 m_renderer;
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
	bool                                           m_accepting         = true;
	bool                                           m_stopping          = false;
	bool                                           m_wake_pending      = false;
	bool                                           m_shutdown_complete = false;
	// Completion callbacks can outlive GuestGpu during renderer shutdown.
	std::shared_ptr<std::binary_semaphore> m_suspend_point_ready =
	    std::make_shared<std::binary_semaphore>(1);

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;
	std::jthread    m_thread;

	// [DIAG cbsync] TEMPORARY instrumentation counters (guarded by m_queue_mutex).
	uint64_t m_diag_submission_id = 0;
	uint64_t m_diag_enqueue_seq   = 0;

	friend class CommandProcessor;
};

// Submit-time owned dwords for the guest range starting at `address` in the submission the GPU
// thread is currently parsing, or an empty span when the range was not captured at submit time (or
// this is not the parsing thread). The deferred *draw* path uses this to read the vertex-input
// descriptor tables it reaches through GS/HS user-data pointers: those tables live in guest memory
// the guest may have recycled since the submission was queued, so they must be read from the same
// submit-time capture as the command stream.
std::span<const uint32_t> FindOwnedGuestRangeForCurrentSubmit(uint64_t address, uint32_t num_dw);
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
