#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRONTWORKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRONTWORKER_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace Libs::Graphics {

// A guest range a job read where the GPU may write (NoteFrontRead).
struct FrontRead {
	uint64_t vaddr = 0;
	uint64_t size  = 0;
};

// A thread that runs jobs in order for the GPU thread: the program lookups of the next draws and
// dispatches while the GPU thread records the ones before them (RenderExecutor::DrawIndex).
//
// A job reads guest memory as the GPU thread would at the job's place in the command stream; the
// GPU thread checks that nothing it did meanwhile wrote what the job read (Reads), and redoes the
// lookups itself when something did. A job also stops (aborts) where only the GPU thread may go
// on: GPU-written bytes it would have to read back, a program to translate. Both sides wait by
// spinning first: a draw takes microseconds, a sleeping thread wakes in tens of them.
class FrontWorker {
public:
	// Jobs started and not yet claimed by the GPU thread (Wait) at once, at most.
	static constexpr uint32_t Capacity = 8;

	FrontWorker();
	~FrontWorker();
	KYTY_CLASS_NO_COPY(FrontWorker);

	// GPU thread. Queues a job behind the others; returns its ticket. The job Capacity tickets
	// before it must have been claimed (its Reads are kept until then).
	uint64_t Start(std::function<void()> job);
	// GPU thread. Waits for the job; true when it aborted. `service` runs while a job takes long:
	// a job that faults on guest memory waits for the GPU thread's commands.
	[[nodiscard]] bool Wait(uint64_t ticket, const std::function<void()>& service);
	// GPU thread. Waits for every job started.
	void WaitIdle(const std::function<void()>& service);
	[[nodiscard]] bool Idle() const noexcept {
		return m_done.load(std::memory_order_acquire) == m_started.load(std::memory_order_relaxed);
	}
	// GPU thread, after Wait and before the ticket Capacity later starts: what the job read where
	// the GPU may write.
	[[nodiscard]] std::span<const FrontRead> Reads(uint64_t ticket) const noexcept {
		return m_slots[ticket % Capacity].reads;
	}

private:
	struct Slot {
		std::function<void()>  job;
		bool                   aborted = false;
		std::vector<FrontRead> reads;
	};

	void Run(const std::stop_token& stop);

	std::array<Slot, Capacity>  m_slots;
	// Tickets: m_started written by the GPU thread, m_done by the worker.
	std::atomic<uint64_t>       m_started {0};
	std::atomic<uint64_t>       m_done {0};
	std::atomic<bool>           m_sleeping {false};
	std::mutex                  m_mutex;
	std::condition_variable_any m_wake;
	std::jthread                m_thread;
};

// On the front worker thread, inside a job.
[[nodiscard]] bool InFrontJob() noexcept;
// The job stops here: the GPU thread redoes it.
void AbortFrontJob() noexcept;
// The job stopped (AbortFrontJob) since it began.
[[nodiscard]] bool FrontJobAborted() noexcept;
// Inside a job: it read guest memory the GPU may write (FrontWorker::Reads).
void NoteFrontRead(uint64_t vaddr, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRONTWORKER_H_
