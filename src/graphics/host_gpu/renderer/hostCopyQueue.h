#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HOSTCOPYQUEUE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HOSTCOPYQUEUE_H_

#include "common/alignment.h"
#include "common/common.h"
#include "common/spscQueue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <immintrin.h>
#include <memory>
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
// submission, through Flush() and WaitFor() on the submitting thread) and, for copies that read
// their source when they run (Copy()), before the guest may legally overwrite it (any
// guest-visible effect, through Drain()).
//
// The destinations are mapped device memory, which the CPU writes combined: a fence or a locked
// instruction on the writing thread waits until those writes drained over the bus. The producer
// fences on every operation it takes from its queue, so it hands even small writes to the copy
// thread; CopyInline() first copies sources that do not outlive the call into a ring of its own.
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
	// writable until the copy completed (see Drain()).
	void Copy(void* destination, const void* source, size_t size) {
		Queue(destination, source, size);
		m_source_copies = m_queued + m_pending.count;
	}
	// Producer: queues a copy of `size` bytes from a source that may change once this returns.
	// The bytes go through a ring of cached host memory that the copy thread copies from.
	void CopyInline(void* destination, const void* source, size_t size) {
		if (size > RingChunkSize) {
			std::memcpy(destination, source, size);
			return;
		}
		if (m_ring_offset + size > RingChunkSize) {
			NextRingChunk();
		}
		auto* staged = m_ring.get() + m_ring_chunk * RingChunkSize + m_ring_offset;
		std::memcpy(staged, source, size);
		m_ring_offset += Common::AlignUp(size, size_t {16});
		// The copy queued below is the (m_queued + m_pending.count + 1)-th one.
		m_ring_users[m_ring_chunk] = m_queued + m_pending.count + 1;
		Queue(destination, staged, size);
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
	// Producer: waits until the copies that read a source passed to Copy() completed, before the
	// guest may change it. Copies queued with CopyInline() read their own copy of the source.
	void Drain() {
		if (m_completed.Load() >= m_source_copies) {
			return;
		}
		(void)Flush();
		m_completed.WaitFor(m_source_copies);
	}

private:
	static constexpr size_t RingChunkSize = size_t {128} << 10u;
	static constexpr size_t RingChunks    = 64;

	void NextRingChunk() {
		m_ring_chunk  = (m_ring_chunk + 1) % RingChunks;
		m_ring_offset = 0;
		// The last copy that reads the chunk must have completed before it is rewritten; it
		// completed long ago unless the copy thread fell a whole ring behind.
		if (const auto user = m_ring_users[m_ring_chunk]; user != 0 && m_completed.Load() < user) {
			(void)Flush();
			m_completed.WaitFor(user);
		}
	}

	void Queue(void* destination, const void* source, size_t size) {
		m_pending.jobs[m_pending.count++] = {destination, source, size};
		if (m_pending.count == BatchSize) {
			(void)Flush();
		}
	}

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
	// Copies queued up to the last one that reads a source passed to Copy().
	uint64_t    m_source_copies = 0;
	// CopyInline() sources, in chunks; each chunk remembers the last copy that reads it.
	std::unique_ptr<std::byte[]>     m_ring = std::make_unique<std::byte[]>(RingChunkSize * RingChunks);
	std::array<uint64_t, RingChunks> m_ring_users {};
	size_t                           m_ring_chunk  = 0;
	size_t                           m_ring_offset = 0;
	std::thread m_thread;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HOSTCOPYQUEUE_H_
