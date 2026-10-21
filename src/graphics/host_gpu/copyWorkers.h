#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_COPYWORKERS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_COPYWORKERS_H_

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

// Copies a list of memory ranges on a few worker threads and the calling thread together. The
// upload passes of a frame copy about 150 MB of guest memory into the staging buffer on the GPU
// thread, and one thread copies at about half the speed that several threads reach under Rosetta.
//
// Run returns when every byte is copied. A small run is copied on the calling thread alone:
// waking the workers costs more. Runs come in bursts, so a worker spins for a short time after its
// work before it sleeps, and the next run of a burst does not wait for it to wake up.
//
// Each piece is copied by the copy function, memcpy by default. A reader of guest memory replaces it
// where a source range can have unmapped pages.
//
// One thread calls Run at a time.

namespace Libs::Graphics {

class CopyWorkers final {
public:
	struct Copy {
		void*       destination = nullptr;
		const void* source      = nullptr;
		size_t      size        = 0;
	};

	using CopyFunction = void (*)(void* destination, const void* source, size_t size);

	// A copy is split into pieces of this size, so the threads share a large copy.
	static constexpr size_t PieceSize = size_t {256} << 10;

	explicit CopyWorkers(uint32_t worker_count, CopyFunction copy = CopyBytes): m_copy(copy) {
		m_workers.reserve(worker_count);
		for (uint32_t i = 0; i < worker_count; i++) {
			m_workers.emplace_back([this] { Work(); });
		}
	}

	~CopyWorkers() {
		{
			std::scoped_lock lock {m_mutex};
			m_stop = true;
		}
		m_wake.notify_all();
		for (auto& worker: m_workers) {
			worker.join();
		}
	}

	CopyWorkers(const CopyWorkers&)            = delete;
	CopyWorkers& operator=(const CopyWorkers&) = delete;

	void Run(std::span<const Copy> copies) {
		size_t total = 0;
		for (const auto& copy: copies) {
			total += copy.size;
		}
		if (total < InlineMax) {
			for (const auto& copy: copies) {
				m_copy(copy.destination, copy.source, copy.size);
			}
			return;
		}
		{
			std::unique_lock lock {m_mutex};
			// A worker that woke late for the last run can still read the pieces.
			m_idle.wait(lock, [this] { return m_busy == 0; });
			m_pieces.clear();
			for (const auto& copy: copies) {
				for (size_t offset = 0; offset < copy.size; offset += PieceSize) {
					m_pieces.push_back({static_cast<uint8_t*>(copy.destination) + offset,
					                    static_cast<const uint8_t*>(copy.source) + offset,
					                    std::min(PieceSize, copy.size - offset)});
				}
			}
			m_next.store(0, std::memory_order_relaxed);
			m_done.store(0, std::memory_order_relaxed);
			m_generation.fetch_add(1, std::memory_order_release);
		}
		m_wake.notify_all();
		CopyPieces();
		const auto count = m_pieces.size();
		while (m_done.load(std::memory_order_acquire) != count) {
			std::this_thread::yield();
		}
	}

private:
	static constexpr size_t InlineMax = size_t {1} << 20;
	// About 50 µs: longer than the gap between two runs of a burst.
	static constexpr int SpinRounds = 2000;

	static void CopyBytes(void* destination, const void* source, size_t size) {
		std::memcpy(destination, source, size);
	}

	void CopyPieces() {
		const auto count = m_pieces.size();
		for (auto i = m_next.fetch_add(1, std::memory_order_relaxed); i < count;
		     i      = m_next.fetch_add(1, std::memory_order_relaxed)) {
			const auto& piece = m_pieces[i];
			m_copy(piece.destination, piece.source, piece.size);
			m_done.fetch_add(1, std::memory_order_release);
		}
	}

	void Work() {
		uint64_t seen = 0;
		for (;;) {
			for (int round = 0; round < SpinRounds &&
			                    m_generation.load(std::memory_order_acquire) == seen;
			     round++) {
				std::this_thread::yield();
			}
			{
				std::unique_lock lock {m_mutex};
				m_wake.wait(lock, [&] {
					return m_stop || m_generation.load(std::memory_order_relaxed) != seen;
				});
				if (m_stop) {
					return;
				}
				seen = m_generation.load(std::memory_order_relaxed);
				m_busy++;
			}
			CopyPieces();
			{
				std::scoped_lock lock {m_mutex};
				if (--m_busy == 0) {
					m_idle.notify_all();
				}
			}
		}
	}

	CopyFunction             m_copy;
	std::vector<std::thread> m_workers;
	std::mutex               m_mutex;
	std::condition_variable  m_wake;
	std::condition_variable  m_idle;
	// Under m_mutex: the workers that copy pieces of the current run.
	uint32_t                 m_busy = 0;
	bool                     m_stop = false;
	// Written by Run under m_mutex while no worker is busy, read by the busy workers.
	std::vector<Copy>        m_pieces;
	std::atomic<size_t>      m_next {0};
	std::atomic<size_t>      m_done {0};
	std::atomic<uint64_t>    m_generation {0};
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_COPYWORKERS_H_ */
