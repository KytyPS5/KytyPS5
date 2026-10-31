#ifndef KYTY_COMMON_SPSCQUEUE_H_
#define KYTY_COMMON_SPSCQUEUE_H_

#include "common/assert.h"
#include "common/common.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace Common {

inline void SpinPause() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
	_mm_pause();
#endif
}

// How long a pipeline thread spins on an empty or full handoff before it falls back to a kernel
// wait: KYTY_GPU_SPIN_US microseconds, 2 ms by default. Within a busy frame the stages never
// wait that long, so they never enter the kernel; an idle stage stops burning its core.
inline std::chrono::nanoseconds SpinBudget() noexcept {
	static const auto budget = [] {
		uint64_t micros = 2000;
		if (const char* value = std::getenv("KYTY_GPU_SPIN_US"); value != nullptr) {
			micros = std::strtoull(value, nullptr, 10);
		}
		return std::chrono::nanoseconds(micros * 1000u);
	}();
	return budget;
}

// Busy-waiting with an exponential _mm_pause backoff (1, 2, 4 ... 64 pauses per round) that
// gives up once the spin budget is spent. The clock is read every few rounds only.
class SpinWait {
public:
	SpinWait() noexcept: m_budget(SpinBudget()) {}

	// Pauses once more; false once the budget is spent and the caller should sleep.
	bool Spin() noexcept {
		for (uint32_t i = 0; i < m_pauses; i++) {
			SpinPause();
		}
		m_iterations += m_pauses;
		if (m_pauses < MaxPauses) {
			m_pauses <<= 1u;
		}
		if ((++m_rounds & ClockMask) != 0) {
			return true;
		}
		const auto now = std::chrono::steady_clock::now();
		if (m_rounds == ClockMask + 1) {
			m_start = now;
			return true;
		}
		return now - m_start < m_budget;
	}

	[[nodiscard]] uint64_t Iterations() const noexcept { return m_iterations; }

private:
	static constexpr uint32_t MaxPauses = 64;
	static constexpr uint32_t ClockMask = 15;

	std::chrono::nanoseconds              m_budget;
	std::chrono::steady_clock::time_point m_start {};
	uint32_t                              m_pauses     = 1;
	uint32_t                              m_rounds     = 0;
	uint64_t                              m_iterations = 0;
};

// A monotonically increasing counter one thread advances and any threads wait on.
//
// A waiter spins (SpinWait) and only then sleeps. A wake-up is only issued when the value
// reaches the lowest target a sleeping thread registered. WaitFor() registers its target (it
// lowers the shared one) and then reads the value, seq_cst; Notify() issues a seq_cst fence and
// then reads the target. Either the waiter sees a value published before the fence, or Notify()
// sees the target, takes the registration out and wakes every sleeper; a sleeper whose own
// target is not reached yet registers it again and sleeps on. Publish() alone is a plain release
// store: a sleeper it satisfies wakes at the next Notify().
//
// Sleepers wait on a count of the wake-ups, which a waiter reads before it registers, and not on
// the value: the registration it finds may be a lower target, even one already reached that the
// next Notify() takes out at the value the sleeper read. A sleeper on the value would not see that
// wake-up and would sleep on with no target registered, and no later Notify() would wake it.
class ProgressCounter {
public:
	ProgressCounter() = default;
	KYTY_CLASS_NO_COPY(ProgressCounter);

	[[nodiscard]] uint64_t Load() const noexcept { return m_value.load(std::memory_order_acquire); }

	void Publish(uint64_t value) noexcept { m_value.store(value, std::memory_order_release); }

	void Notify() noexcept {
		std::atomic_thread_fence(std::memory_order_seq_cst);
		auto registered = m_wait_target.load(std::memory_order_relaxed);
		if (registered <= m_value.load(std::memory_order_relaxed)) {
			// The registered target is reached: every sleeper wakes and registers again. A
			// lower target registered meanwhile stays.
			(void)m_wait_target.compare_exchange_strong(registered, NoTarget,
			                                            std::memory_order_seq_cst);
			m_wakeups.fetch_add(1, std::memory_order_seq_cst);
			m_wakeups.notify_all();
		}
	}

	void Advance(uint64_t value) noexcept {
		Publish(value);
		Notify();
	}

	// Waits until the counter reaches `target`; the wait acquires every write the advancing
	// thread made before it reached `target`.
	void WaitFor(uint64_t target) noexcept {
		if (m_value.load(std::memory_order_acquire) >= target) {
			return;
		}
		SpinWait spin;
		while (spin.Spin()) {
			if (m_value.load(std::memory_order_acquire) >= target) {
				return;
			}
		}
		for (;;) {
			// A Notify() that takes out the registration below from now on changes the count, so
			// the wait returns and the target is registered again.
			const auto wakeups = m_wakeups.load(std::memory_order_seq_cst);
			// Lower the registered target to ours: another thread may sleep on a lower one.
			auto registered = m_wait_target.load(std::memory_order_seq_cst);
			while (registered > target &&
			       !m_wait_target.compare_exchange_weak(registered, target,
			                                            std::memory_order_seq_cst)) {
			}
			if (m_value.load(std::memory_order_seq_cst) >= target) {
				break;
			}
			m_wakeups.wait(wakeups, std::memory_order_seq_cst);
		}
	}

private:
	static constexpr uint64_t NoTarget = ~uint64_t {0};

	alignas(64) std::atomic_uint64_t m_value {0};
	alignas(64) std::atomic_uint64_t m_wait_target {NoTarget};
	std::atomic_uint32_t             m_wakeups {0};
};

// A bounded lock-free single-producer single-consumer ring. Push() waits while the ring is full
// and Pop() waits while it is empty: they spin (SpinWait) and only then sleep.
//
// The producer constructs an element in its slot and then publishes it with a release store of
// m_tail; the consumer acquires m_tail before it reads the slot. The consumer moves the element
// out, destroys it and then releases the slot with a release store of m_head; the producer
// acquires m_head before it reuses the slot. The indices live on separate cache lines, and each
// side keeps a cached copy of the other side's index so it only reads the shared line when the
// cached one says the ring is full or empty.
//
// A side that sleeps announces itself and then re-reads the index it waits on, seq_cst. The
// producer looks for a sleeping consumer behind a seq_cst fence after every push. The consumer
// does so every CheckInterval pops and before it sleeps itself: a producer only sleeps on a full
// ring, so a consumer that keeps popping reaches a check soon, and one that stops popping is
// about to sleep. Either way one side sees the other.
template <typename T>
class SpscQueue {
public:
	explicit SpscQueue(size_t capacity)
	    : m_capacity(std::bit_ceil(capacity)), m_mask(m_capacity - 1),
	      m_slots(std::make_unique<Slot[]>(m_capacity)) {
		EXIT_IF(capacity == 0);
	}
	~SpscQueue() {
		const auto tail = m_tail.load(std::memory_order_acquire);
		for (auto head = m_head.load(std::memory_order_acquire); head != tail; head++) {
			Element(head)->~T();
		}
	}
	KYTY_CLASS_NO_COPY(SpscQueue);

	void Push(T&& value) {
		const auto tail = m_tail.load(std::memory_order_relaxed);
		if (tail - m_producer_head >= m_capacity) {
			m_producer_head = m_head.load(std::memory_order_acquire);
			if (tail - m_producer_head >= m_capacity) {
				WaitForSpace(tail);
			}
		}
		Construct(tail, std::move(value));
	}

	// Returns false (and keeps `value`) when the ring is full.
	bool TryPush(T& value) {
		const auto tail = m_tail.load(std::memory_order_relaxed);
		if (tail - m_producer_head >= m_capacity) {
			m_producer_head = m_head.load(std::memory_order_acquire);
			if (tail - m_producer_head >= m_capacity) {
				return false;
			}
		}
		Construct(tail, std::move(value));
		return true;
	}

	// Returns nothing once Stop() was called and the ring is empty, or when Interrupt() was called
	// while the ring is empty.
	[[nodiscard]] std::optional<T> Pop() {
		const auto head = m_head.load(std::memory_order_relaxed);
		if (m_consumer_tail == head) {
			m_consumer_tail = m_tail.load(std::memory_order_acquire);
			if (m_consumer_tail == head && !WaitForElement(head)) {
				return std::nullopt;
			}
		}
		return Take(head);
	}

	// Returns nothing when the ring is empty.
	[[nodiscard]] std::optional<T> TryPop() {
		const auto head = m_head.load(std::memory_order_relaxed);
		if (m_consumer_tail == head) {
			m_consumer_tail = m_tail.load(std::memory_order_acquire);
			if (m_consumer_tail == head) {
				return std::nullopt;
			}
		}
		return Take(head);
	}

	// Any thread: wakes the consumer, whose Pop() returns nothing once the ring is empty.
	void Stop() {
		m_stopped.store(true, std::memory_order_seq_cst);
		m_consumer_signal.fetch_add(1, std::memory_order_seq_cst);
		m_consumer_signal.notify_all();
	}

	// Any thread: wakes a consumer that waits on an empty ring without stopping the queue; its Pop()
	// returns nothing once, so it can look at other work. An interrupt that finds the consumer busy
	// ends its next wait on an empty ring instead.
	void Interrupt() {
		m_interrupted.store(true, std::memory_order_seq_cst);
		m_consumer_signal.fetch_add(1, std::memory_order_seq_cst);
		m_consumer_signal.notify_all();
	}

	[[nodiscard]] bool IsStopped() const noexcept {
		return m_stopped.load(std::memory_order_acquire);
	}

	[[nodiscard]] size_t Capacity() const noexcept { return m_capacity; }

private:
	static constexpr uint32_t CheckInterval = 16;

	// A slot per cache line at least: the producer writes one while the consumer reads another.
	struct alignas(alignof(T) > 64 ? alignof(T) : 64) Slot {
		std::byte storage[sizeof(T)];
	};

	T* Element(uint64_t index) {
		return std::launder(reinterpret_cast<T*>(m_slots[index & m_mask].storage));
	}

	void Construct(uint64_t tail, T&& value) {
		::new (static_cast<void*>(m_slots[tail & m_mask].storage)) T(std::move(value));
		m_tail.store(tail + 1, std::memory_order_release);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (m_consumer_sleeping.load(std::memory_order_relaxed)) {
			m_consumer_signal.fetch_add(1, std::memory_order_seq_cst);
			m_consumer_signal.notify_one();
		}
	}

	std::optional<T> Take(uint64_t head) {
		auto*            element = Element(head);
		std::optional<T> value(std::move(*element));
		element->~T();
		m_head.store(head + 1, std::memory_order_release);
		if (++m_unchecked_pops >= CheckInterval) {
			WakeProducer();
		}
		return value;
	}

	void WakeProducer() {
		m_unchecked_pops = 0;
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (m_producer_sleeping.load(std::memory_order_relaxed)) {
			m_producer_signal.fetch_add(1, std::memory_order_seq_cst);
			m_producer_signal.notify_one();
		}
	}

	void WaitForSpace(uint64_t tail) {
		SpinWait spin;
		while (spin.Spin()) {
			m_producer_head = m_head.load(std::memory_order_acquire);
			if (tail - m_producer_head < m_capacity) {
				return;
			}
		}
		for (;;) {
			m_producer_sleeping.store(true, std::memory_order_seq_cst);
			const auto signal = m_producer_signal.load(std::memory_order_seq_cst);
			m_producer_head   = m_head.load(std::memory_order_seq_cst);
			if (tail - m_producer_head < m_capacity) {
				break;
			}
			m_producer_signal.wait(signal, std::memory_order_seq_cst);
		}
		m_producer_sleeping.store(false, std::memory_order_relaxed);
	}

	// False when stopped or interrupted with nothing left to pop.
	bool WaitForElement(uint64_t head) {
		// The producer may be asleep on a full ring the consumer drained without a check.
		WakeProducer();
		SpinWait spin;
		while (spin.Spin()) {
			m_consumer_tail = m_tail.load(std::memory_order_acquire);
			if (m_consumer_tail != head) {
				return true;
			}
			if (m_stopped.load(std::memory_order_relaxed) ||
			    m_interrupted.load(std::memory_order_relaxed)) {
				break;
			}
		}
		bool available = false;
		for (;;) {
			m_consumer_sleeping.store(true, std::memory_order_seq_cst);
			const auto signal = m_consumer_signal.load(std::memory_order_seq_cst);
			m_consumer_tail   = m_tail.load(std::memory_order_seq_cst);
			if (m_consumer_tail != head) {
				available = true;
				break;
			}
			if (m_stopped.load(std::memory_order_seq_cst) ||
			    m_interrupted.load(std::memory_order_seq_cst)) {
				break;
			}
			m_consumer_signal.wait(signal, std::memory_order_seq_cst);
		}
		m_consumer_sleeping.store(false, std::memory_order_relaxed);
		if (!available) {
			m_interrupted.store(false, std::memory_order_relaxed);
		}
		return available;
	}

	const uint64_t          m_capacity;
	const uint64_t          m_mask;
	std::unique_ptr<Slot[]> m_slots;

	// Consumer side.
	alignas(64) std::atomic_uint64_t m_head {0};
	uint64_t             m_consumer_tail  = 0;
	uint32_t             m_unchecked_pops = 0;
	std::atomic_bool     m_consumer_sleeping {false};
	std::atomic_uint32_t m_consumer_signal {0};
	std::atomic_bool     m_stopped {false};
	std::atomic_bool     m_interrupted {false};

	// Producer side.
	alignas(64) std::atomic_uint64_t m_tail {0};
	uint64_t             m_producer_head = 0;
	std::atomic_bool     m_producer_sleeping {false};
	std::atomic_uint32_t m_producer_signal {0};
};

} // namespace Common

#endif // KYTY_COMMON_SPSCQUEUE_H_
