#include "graphics/host_gpu/renderer/frontWorker.h"

#include "common/assert.h"

#include <chrono>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <pthread.h>
#endif

namespace Libs::Graphics {

namespace {

thread_local bool t_in_job  = false;
thread_local bool t_aborted = false;
thread_local std::vector<FrontRead>* t_reads = nullptr;

inline void Pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	asm volatile("yield");
#endif
}

// A job takes a few microseconds; the spin covers the gap between two draws of the GPU thread.
constexpr auto WorkerSpin = std::chrono::microseconds(200);
// Longer than a job without a fault or a translation: the job may wait for the GPU thread.
constexpr auto ServiceAfter = std::chrono::microseconds(50);

} // namespace

bool InFrontJob() noexcept {
	return t_in_job;
}

void AbortFrontJob() noexcept {
	EXIT_IF(!t_in_job);
	t_aborted = true;
}

bool FrontJobAborted() noexcept {
	return t_aborted;
}

void NoteFrontRead(uint64_t vaddr, uint64_t size) {
	EXIT_IF(t_reads == nullptr);
	t_reads->push_back({vaddr, size});
}

FrontWorker::FrontWorker()
    : m_thread([this](const std::stop_token& stop) { Run(stop); }) {}

FrontWorker::~FrontWorker() {
	m_thread.request_stop();
	{
		std::lock_guard lock(m_mutex);
		m_wake.notify_all();
	}
}

uint64_t FrontWorker::Start(std::function<void()> job) {
	const auto ticket = m_started.load(std::memory_order_relaxed);
	EXIT_IF(ticket - m_done.load(std::memory_order_acquire) >= Capacity);
	m_slots[ticket % Capacity].job = std::move(job);
	m_started.store(ticket + 1, std::memory_order_seq_cst);
	if (m_sleeping.load(std::memory_order_seq_cst)) {
		std::lock_guard lock(m_mutex);
		m_wake.notify_one();
	}
	return ticket;
}

bool FrontWorker::Wait(uint64_t ticket, const std::function<void()>& service) {
	EXIT_IF(ticket >= m_started.load(std::memory_order_relaxed));
	if (m_done.load(std::memory_order_acquire) <= ticket) {
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t spins = 0; m_done.load(std::memory_order_acquire) <= ticket; spins++) {
			Pause();
			if ((spins & 255u) == 255u && std::chrono::steady_clock::now() - start > ServiceAfter) {
				service();
			}
		}
	}
	return m_slots[ticket % Capacity].aborted;
}

void FrontWorker::WaitIdle(const std::function<void()>& service) {
	const auto started = m_started.load(std::memory_order_relaxed);
	if (started != 0) {
		(void)Wait(started - 1, service);
	}
}

void FrontWorker::Run(const std::stop_token& stop) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	pthread_setname_np(pthread_self(), "GpuFront");
#endif
	uint64_t next = 0;
	while (!stop.stop_requested()) {
		const auto spin_start = std::chrono::steady_clock::now();
		for (uint32_t spins = 0; m_started.load(std::memory_order_acquire) == next; spins++) {
			Pause();
			if ((spins & 1023u) == 1023u &&
			    std::chrono::steady_clock::now() - spin_start > WorkerSpin) {
				std::unique_lock lock(m_mutex);
				m_sleeping.store(true, std::memory_order_seq_cst);
				m_wake.wait(lock, stop, [this, next] {
					return m_started.load(std::memory_order_seq_cst) != next;
				});
				m_sleeping.store(false, std::memory_order_relaxed);
				break;
			}
		}
		if (stop.stop_requested()) {
			break;
		}
		auto& slot = m_slots[next % Capacity];
		slot.reads.clear();
		t_in_job  = true;
		t_aborted = false;
		t_reads   = &slot.reads;
		slot.job();
		slot.job     = nullptr;
		t_reads      = nullptr;
		t_in_job     = false;
		slot.aborted = t_aborted;
		m_done.store(++next, std::memory_order_release);
	}
}

} // namespace Libs::Graphics
