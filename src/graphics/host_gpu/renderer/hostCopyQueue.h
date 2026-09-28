#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HOSTCOPYQUEUE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HOSTCOPYQUEUE_H_

#include "common/common.h"
#include "common/spscQueue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <immintrin.h>
#include <thread>
#include <utility>

namespace Libs::Graphics {

// Host memory copies the execution thread of the GPU hands to a thread of their own: the guest
// data that draws stream into the stream buffer. One producer thread queues copies; the copy
// thread performs them in order. The producer gathers copies into batches and hands over a batch
// when it is full or on Flush(), so the handoff fence is paid once per batch. Completion is a
// counter of finished copies: waiting for it acquires the copied bytes (the copy thread publishes
// the count with a release store after the copies, ProgressCounter::WaitFor() acquires it).
//
// The producer must wait for its copies before anything may read their destinations (a queue
// submission, through Flush() and WaitFor() on the submitting thread) and before the guest may
// legally overwrite their sources (any guest-visible effect, through Drain()).
class HostCopyQueue {
public:
	static constexpr size_t BatchSize = 8;
	static constexpr size_t Batches   = 2048;

	// configure_thread runs first on the copy thread (pinning, priority).
	explicit HostCopyQueue(std::function<void()> configure_thread = {})
	    : m_batches(Batches), m_thread([this, configure = std::move(configure_thread)] {
		      if (configure) {
			      configure();
		      }
		      Run();
	      }) {}
	~HostCopyQueue() {
		(void)Flush();
		m_batches.Stop();
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}
	KYTY_CLASS_NO_COPY(HostCopyQueue);

	// Producer: queues a copy of `size` bytes; `source` must stay readable and `destination`
	// writable until the copy completed.
	void Copy(void* destination, const void* source, size_t size) {
		m_pending.jobs[m_pending.count++] = {destination, source, size};
		if (m_pending.count == BatchSize) {
			(void)Flush();
		}
	}
	// Producer: hands the gathered copies to the copy thread; returns the number of copies
	// queued so far.
	uint64_t Flush() {
		if (m_pending.count != 0) {
			m_queued += m_pending.count;
			m_batches.Push(std::move(m_pending));
			m_pending.count = 0;
		}
		return m_queued;
	}
	// Any thread: waits until the first `count` copies completed.
	void WaitFor(uint64_t count) noexcept { m_completed.WaitFor(count); }
	// Producer: waits until every queued copy completed.
	void Drain() {
		const auto queued = Flush();
		if (m_completed.Load() != queued) {
			m_completed.WaitFor(queued);
		}
	}

private:
	struct Job {
		void*       destination = nullptr;
		const void* source      = nullptr;
		size_t      size        = 0;
	};
	struct Batch {
		std::array<Job, BatchSize> jobs {};
		size_t                     count = 0;
	};

	void Run() {
		uint64_t done = 0;
		for (;;) {
			auto batch = m_batches.TryPop();
			if (!batch) {
				// Every queued copy completed: wake a waiter that may have gone to sleep.
				m_completed.Notify();
				batch = m_batches.Pop();
				if (!batch) {
					break;
				}
			}
			for (size_t i = 0; i < batch->count; i++) {
				const auto& job = batch->jobs[i];
				std::memcpy(job.destination, job.source, job.size);
			}
			done += batch->count;
			// The destinations are write-combined: their stores (and the non-temporal ones a large
			// memcpy makes) are not ordered before the release store below, so the consumer that
			// sees the count could hand the buffer to the GPU while the tail of these copies still
			// sits in this core's write-combining buffers. Drain them first.
			_mm_sfence();
			m_completed.Publish(done);
		}
		m_completed.Notify();
	}

	Common::SpscQueue<Batch> m_batches;
	Common::ProgressCounter  m_completed;
	// Producer side.
	alignas(64) Batch m_pending;
	uint64_t    m_queued = 0;
	std::thread m_thread;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HOSTCOPYQUEUE_H_
