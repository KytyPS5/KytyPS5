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
#define KYTY_SYNC_ON_ADDRESS_LINUX 1
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define KYTY_SYNC_ON_ADDRESS_WINDOWS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#define KYTY_SYNC_ON_ADDRESS_PORTABLE 1
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

[[nodiscard]] WaitDeadline MakeDeadline(std::chrono::nanoseconds timeout) {
	const auto now = Clock::now();
	const auto remaining = Clock::time_point::max() - now;
	return {true, timeout >= remaining ? Clock::time_point::max() : now + timeout};
}

[[nodiscard]] WaitDeadline MakeDeadline(const uint32_t* timeout_micros) {
	if (timeout_micros == nullptr) {
		return {};
	}
	return MakeDeadline(std::chrono::microseconds(*timeout_micros));
}

[[nodiscard]] uint32_t GetWaitSliceMicros(const WaitDeadline& deadline, bool first_wait,
                                          bool has_signal_poll) {
	if (!deadline.finite) {
		return has_signal_poll ? SIGNAL_POLL_MICROS : UINT32_MAX - 1u;
	}

	const auto now = Clock::now();
	if (now >= deadline.end) {
		return first_wait ? 0u : UINT32_MAX;
	}

	const auto remaining =
	    std::chrono::ceil<std::chrono::microseconds>(deadline.end - now).count();
	if (!has_signal_poll) {
		return static_cast<uint32_t>(
		    std::min<int64_t>(remaining, UINT32_MAX - 1u));
	}
	return static_cast<uint32_t>(std::min<int64_t>(remaining, SIGNAL_POLL_MICROS));
}

void PollSignals(signal_poll_func_t signal_poll) {
	if (signal_poll != nullptr) {
		signal_poll();
	}
}

#if defined(KYTY_SYNC_ON_ADDRESS_LINUX)

template <typename T>
int WaitLinux(volatile T* address, T expected, const WaitDeadline& deadline,
              signal_poll_func_t signal_poll) {
	bool       first_wait = true;

	for (;;) {
		if (ReadWord(address) != expected) {
			return OK;
		}
		const auto slice_micros =
		    GetWaitSliceMicros(deadline, first_wait, signal_poll != nullptr);
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
	// The legacy FUTEX_WAKE path may wake one waiter when count is zero.
	if (count == 0) {
		return OK;
	}

	const auto result = syscall(SYS_futex, const_cast<void*>(address), FUTEX_WAKE_PRIVATE, count,
	                            nullptr, nullptr, 0);
	return result < 0 ? KERNEL_ERROR_EINVAL : OK;
}

#endif

#if defined(KYTY_SYNC_ON_ADDRESS_WINDOWS)

template <typename T>
int WaitWindows(volatile T* address, T expected, const WaitDeadline& deadline,
                signal_poll_func_t signal_poll) {
	bool first_wait = true;

	for (;;) {
		if (ReadWord(address) != expected) {
			return OK;
		}
		const auto slice_micros =
		    GetWaitSliceMicros(deadline, first_wait, signal_poll != nullptr);
		if (slice_micros == UINT32_MAX) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}

		DWORD ms = 0;
		if (!deadline.finite && signal_poll == nullptr) {
			ms = INFINITE;
		} else if (slice_micros == 0) {
			ms = 0;
		} else {
			ms = static_cast<DWORD>((static_cast<uint64_t>(slice_micros) + 999u) / 1000u);
		}

		T compare_value = expected;
		const BOOL ok = WaitOnAddress(address, &compare_value, sizeof(T), ms);
		if (ok) {
			return OK;
		}

		const DWORD err = GetLastError();
		if (err != ERROR_TIMEOUT) {
			return KERNEL_ERROR_EINVAL;
		}

		if (ReadWord(address) != expected) {
			return OK;
		}

		PollSignals(signal_poll);
		if (deadline.finite && Clock::now() >= deadline.end) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}
		first_wait = false;
	}
}

int WakeWindows(volatile void* address, int32_t count) {
	if (count == 0) {
		return OK;
	}
	if (count == 1) {
		WakeByAddressSingle(const_cast<void*>(address));
	} else if (count == INT_MAX) {
		WakeByAddressAll(const_cast<void*>(address));
	} else {
		for (int32_t i = 0; i < count; ++i) {
			WakeByAddressSingle(const_cast<void*>(address));
		}
	}
	return OK;
}

#endif

#if defined(KYTY_SYNC_ON_ADDRESS_PORTABLE)

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
int WaitPortable(volatile T* address, T expected, const WaitDeadline& deadline,
                 signal_poll_func_t signal_poll) {
	PortableWaiter waiter;
	auto           entry      = RegisterPortableWaiter(address, &waiter);
	bool           first_wait = true;
	int            result     = OK;

	while (ReadWord(address) == expected && !waiter.wake_requested) {
		const auto slice_micros =
		    GetWaitSliceMicros(deadline, first_wait, signal_poll != nullptr);
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

#endif

template <typename T>
int WaitImpl(volatile T* address, T expected, const WaitDeadline& deadline,
             signal_poll_func_t signal_poll) {
	if (!IsValidWaitAddress(address)) {
		return KERNEL_ERROR_EINVAL;
	}

	int result = OK;
#if defined(KYTY_SYNC_ON_ADDRESS_LINUX)
	result = WaitLinux(address, expected, deadline, signal_poll);
#elif defined(KYTY_SYNC_ON_ADDRESS_WINDOWS)
	result = WaitWindows(address, expected, deadline, signal_poll);
#elif defined(KYTY_SYNC_ON_ADDRESS_PORTABLE)
	result = WaitPortable(address, expected, deadline, signal_poll);
#endif
	PollSignals(signal_poll);
	return result;
}

} // namespace

int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout_micros), signal_poll);
}

int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout_micros), signal_poll);
}

int Wait64(volatile uint64_t* address, uint64_t expected, std::chrono::nanoseconds timeout,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout), signal_poll);
}

int Wake(volatile void* address, int32_t count) {
	if (!IsValidWakeAddress(address) || count < 0) {
		return KERNEL_ERROR_EINVAL;
	}

#if defined(KYTY_SYNC_ON_ADDRESS_LINUX)
	return WakeLinux(address, count);
#elif defined(KYTY_SYNC_ON_ADDRESS_WINDOWS)
	return WakeWindows(address, count);
#elif defined(KYTY_SYNC_ON_ADDRESS_PORTABLE)
	return WakePortable(address, count);
#endif
}

} // namespace Libs::LibKernel::SyncOnAddress
