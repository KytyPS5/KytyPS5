#include "kernel/syncOnAddress.h"

#include "common/threads.h"
#include "libs/errno.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::LibKernel::SyncOnAddress {

namespace {

constexpr uint32_t SIGNAL_POLL_MICROS = 10000;

using Clock = std::chrono::steady_clock;

template <typename T>
[[nodiscard]] bool IsValidWaitAddress(const volatile T* address) {
	return address != nullptr && (reinterpret_cast<uintptr_t>(address) & (alignof(T) - 1u)) == 0;
}

[[nodiscard]] bool IsValidWakeAddress(const volatile void* address) {
	return address != nullptr &&
	       (reinterpret_cast<uintptr_t>(address) & (alignof(uint32_t) - 1u)) == 0;
}

template <typename T>
[[nodiscard]] T ReadWord(const volatile T* address) {
	return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

struct WaitDeadline {
	bool              finite = false;
	Clock::time_point end {};
};

[[nodiscard]] WaitDeadline MakeDeadline(const uint32_t* timeout_micros) {
	if (timeout_micros == nullptr) {
		return {};
	}
	return {true, Clock::now() + std::chrono::microseconds(*timeout_micros)};
}

[[nodiscard]] uint32_t GetWaitSliceMicros(const WaitDeadline& deadline, bool first_wait) {
	if (!deadline.finite) {
		return SIGNAL_POLL_MICROS;
	}

	const auto now = Clock::now();
	if (now >= deadline.end) {
		return first_wait ? 0u : UINT32_MAX;
	}

	const auto remaining =
	    std::chrono::duration_cast<std::chrono::microseconds>(deadline.end - now).count();
	return static_cast<uint32_t>(std::min<int64_t>(remaining, SIGNAL_POLL_MICROS));
}

void PollSignals(signal_poll_func_t signal_poll) {
	if (signal_poll != nullptr) {
		signal_poll();
	}
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)

template <typename T>
int WaitLinux(volatile T* address, T expected, const uint32_t* timeout_micros,
              signal_poll_func_t signal_poll) {
	const auto deadline   = MakeDeadline(timeout_micros);
	bool       first_wait = true;

	for (;;) {
		if (ReadWord(address) != expected) {
			return OK;
		}
		const auto slice_micros = GetWaitSliceMicros(deadline, first_wait);
		if (slice_micros == UINT32_MAX) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}
		const timespec timeout = {
		    .tv_sec  = static_cast<time_t>(slice_micros / 1000000u),
		    .tv_nsec = static_cast<long>(slice_micros % 1000000u) * 1000L,
		};

		long result     = 0;
		int  wait_error = 0;
		result          = syscall(SYS_futex, const_cast<T*>(address), FUTEX_WAIT_PRIVATE,
		                          static_cast<uint32_t>(expected), &timeout, nullptr, 0);
		if (result != 0) {
			wait_error = errno;
		}
		if (result == 0 || wait_error == EAGAIN) {
			return OK;
		}
		if (wait_error != ETIMEDOUT && wait_error != EINTR) {
			return KERNEL_ERROR_EINVAL;
		}

		PollSignals(signal_poll);
		if (deadline.finite && Clock::now() >= deadline.end) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}
		first_wait = false;
	}
}

int WakeLinux(volatile void* address, int32_t count) {
	// A count of 0 must release no waiters, but FUTEX_WAKE does not implement it that way: the
	// kernel's wake loop tests `if (++ret >= nr_wake) break` *after* waking, so a zero count still
	// releases one waiter. WakePortable already short-circuits this case, and the two backends of
	// the same ABI have to agree.
	if (count == 0) {
		return OK;
	}

	const auto result = syscall(SYS_futex, const_cast<void*>(address), FUTEX_WAKE_PRIVATE, count,
	                            nullptr, nullptr, 0);
	return result < 0 ? KERNEL_ERROR_EINVAL : OK;
}

#endif

struct PortableWaiter {
	Common::CondVar condition;
	bool            wake_requested = false;
};

struct PortableAddressEntry {
	Common::Mutex              mutex;
	std::list<PortableWaiter*> waiters;
};

struct PortableAddressRegistry {
	std::mutex                                                           mutex;
	std::unordered_map<uintptr_t, std::shared_ptr<PortableAddressEntry>> entries;
};

PortableAddressRegistry& GetPortableRegistry() {
	static PortableAddressRegistry registry;
	return registry;
}

std::shared_ptr<PortableAddressEntry> RegisterPortableWaiter(volatile void*  address,
                                                             PortableWaiter* waiter) {
	auto&           registry = GetPortableRegistry();
	std::lock_guard registry_lock(registry.mutex);
	auto&           entry = registry.entries[reinterpret_cast<uintptr_t>(address)];
	if (!entry) {
		entry = std::make_shared<PortableAddressEntry>();
	}
	entry->mutex.Lock();
	entry->waiters.push_back(waiter);
	return entry;
}

void UnregisterPortableWaiter(volatile void*                               address,
                              const std::shared_ptr<PortableAddressEntry>& entry,
                              PortableWaiter*                              waiter) {
	entry->waiters.remove(waiter);
	const bool empty = entry->waiters.empty();
	entry->mutex.Unlock();
	if (!empty) {
		return;
	}

	auto&           registry = GetPortableRegistry();
	std::lock_guard registry_lock(registry.mutex);
	entry->mutex.Lock();
	const auto it = registry.entries.find(reinterpret_cast<uintptr_t>(address));
	if (it != registry.entries.end() && it->second == entry && entry->waiters.empty()) {
		registry.entries.erase(it);
	}
	entry->mutex.Unlock();
}

template <typename T>
int WaitPortable(volatile T* address, T expected, const uint32_t* timeout_micros,
                 signal_poll_func_t signal_poll) {
	PortableWaiter waiter;
	auto           entry      = RegisterPortableWaiter(address, &waiter);
	const auto     deadline   = MakeDeadline(timeout_micros);
	bool           first_wait = true;
	int            result     = OK;

	while (ReadWord(address) == expected && !waiter.wake_requested) {
		const auto slice_micros = GetWaitSliceMicros(deadline, first_wait);
		if (slice_micros == UINT32_MAX) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}
		if (slice_micros == 0) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}

		(void)waiter.condition.WaitFor(&entry->mutex, slice_micros);
		entry->mutex.Unlock();
		PollSignals(signal_poll);
		entry->mutex.Lock();

		if (deadline.finite && Clock::now() >= deadline.end && ReadWord(address) == expected &&
		    !waiter.wake_requested) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}
		first_wait = false;
	}

	UnregisterPortableWaiter(address, entry, &waiter);
	return result;
}

int WakePortable(volatile void* address, int32_t count) {
	if (count == 0) {
		return OK;
	}
	auto&            registry = GetPortableRegistry();
	std::unique_lock registry_lock(registry.mutex);
	const auto       it = registry.entries.find(reinterpret_cast<uintptr_t>(address));
	if (it == registry.entries.end()) {
		return OK;
	}
	auto entry = it->second;
	entry->mutex.Lock();
	registry_lock.unlock();

	int32_t remaining = count;
	for (auto* waiter: entry->waiters) {
		if (!waiter->wake_requested) {
			waiter->wake_requested = true;
			waiter->condition.Signal();
			if (remaining != INT_MAX && --remaining == 0) {
				break;
			}
		}
	}
	entry->mutex.Unlock();
	return OK;
}

template <typename T>
int WaitImpl(volatile T* address, T expected, const uint32_t* timeout_micros,
             signal_poll_func_t signal_poll) {
	if (!IsValidWaitAddress(address)) {
		return KERNEL_ERROR_EINVAL;
	}

	int result = OK;
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	result = WaitLinux(address, expected, timeout_micros, signal_poll);
#else
	result = WaitPortable(address, expected, timeout_micros, signal_poll);
#endif
	PollSignals(signal_poll);
	return result;
}

} // namespace

int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, timeout_micros, signal_poll);
}

int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, timeout_micros, signal_poll);
}

int Wake(volatile void* address, int32_t count) {
	if (!IsValidWakeAddress(address) || count < 0) {
		return KERNEL_ERROR_EINVAL;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	return WakeLinux(address, count);
#endif
	return WakePortable(address, count);
}

} // namespace Libs::LibKernel::SyncOnAddress
