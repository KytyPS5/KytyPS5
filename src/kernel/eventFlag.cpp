#include "kernel/eventFlag.h"

#include "common/logging/log.h"
#include "common/threads.h"
#include "common/timer.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Libs::LibKernel::EventFlag {

LIB_NAME("libkernel", "libkernel");

constexpr uint32_t ATTR_TH_PRIO = 0x02;
constexpr uint32_t ATTR_MULTI   = 0x20;
constexpr uint32_t WAIT_AND     = 0x01;
constexpr uint32_t CLEAR_ALL    = 0x10;
constexpr uint32_t CLEAR_PAT    = 0x20;

class KernelEventFlagPrivate {
public:
	KernelEventFlagPrivate(uint32_t attr, uint64_t bits): m_attr(attr), m_bits(bits) {}
	~KernelEventFlagPrivate();

	KYTY_CLASS_NO_COPY(KernelEventFlagPrivate);

	int  Wait(uint64_t bits, uint32_t mode, uint64_t* result, KernelUseconds* timeout);
	int  Poll(uint64_t bits, uint32_t mode, uint64_t* result);
	void Set(uint64_t bits);
	void Clear(uint64_t bits);
	int  Cancel(uint64_t bits);

private:
	// A waiting thread, on its stack. Whoever releases it decides its outcome.
	struct Waiter {
		uint64_t        bits     = 0;
		uint32_t        mode     = 0;
		int             priority = 0;
		uint64_t        result   = 0;
		int             error    = OK;
		bool            released = false;
		Common::CondVar cond_var;

		void Release(int ret) {
			error    = ret;
			released = true;
			cond_var.Signal();
		}
	};

	int  TakeLocked(uint64_t bits, uint32_t mode, uint64_t* result);
	void ReleaseAll(int error);

	Common::Mutex        m_mutex;
	Common::CondVar      m_last_waiter_left;
	std::vector<Waiter*> m_queue;
	uint32_t             m_attr;
	uint64_t             m_bits;
	// Queued and released threads that have not returned yet.
	int  m_waiters = 0;
	bool m_deleted = false;
};

namespace {

bool Satisfied(uint64_t pattern, uint64_t bits, uint32_t mode) {
	return (mode & WAIT_AND) != 0 ? (pattern & bits) == bits : (pattern & bits) != 0;
}

uint64_t Cleared(uint64_t pattern, uint64_t bits, uint32_t mode) {
	if ((mode & CLEAR_ALL) != 0) {
		return 0;
	}
	return (mode & CLEAR_PAT) != 0 ? pattern & ~bits : pattern;
}

// Exactly one of AND and OR, at most one clear mode and a non-empty pattern.
bool ValidWait(uint64_t bits, uint32_t mode) {
	return bits != 0 && (mode & 0x03u) != 0 && (mode & 0x03u) != 0x03u && (mode & 0x30u) != 0x30u;
}

// A timed wait sleeps ceil(timeout / tick) + 1 ticks of 1 ms from the last tick: at least the
// timeout, and up to the next tick even for a zero timeout.
uint64_t TickDeadline(uint64_t now_ns, uint32_t micros) {
	constexpr uint64_t TICK_NS = 1000000;
	return (now_ns / TICK_NS + (uint64_t {micros} + 999) / 1000 + 1) * TICK_NS;
}

} // namespace

KernelEventFlagPrivate::~KernelEventFlagPrivate() {
	Common::LockGuard lock(m_mutex);

	m_deleted = true;
	ReleaseAll(KERNEL_ERROR_EACCES);
	while (m_waiters != 0) {
		m_last_waiter_left.Wait(&m_mutex);
	}
}

// Takes the bits when the wait condition holds; EBUSY when the thread would have to wait.
int KernelEventFlagPrivate::TakeLocked(uint64_t bits, uint32_t mode, uint64_t* result) {
	if (m_deleted) {
		return KERNEL_ERROR_EACCES;
	}
	if ((m_attr & ATTR_MULTI) == 0 && m_waiters != 0) {
		return KERNEL_ERROR_EPERM;
	}
	*result = m_bits;
	if (!Satisfied(m_bits, bits, mode)) {
		return KERNEL_ERROR_EBUSY;
	}
	m_bits = Cleared(m_bits, bits, mode);
	return OK;
}

int KernelEventFlagPrivate::Poll(uint64_t bits, uint32_t mode, uint64_t* result) {
	Common::LockGuard lock(m_mutex);

	return TakeLocked(bits, mode, result);
}

int KernelEventFlagPrivate::Wait(uint64_t bits, uint32_t mode, uint64_t* result,
                                 KernelUseconds* timeout) {
	Common::LockGuard lock(m_mutex);

	if (int ret = TakeLocked(bits, mode, result); ret != KERNEL_ERROR_EBUSY) {
		return ret;
	}

	Waiter waiter {.bits = bits, .mode = mode};
	if ((m_attr & ATTR_TH_PRIO) != 0) {
		waiter.priority = PthreadGetCurrentPriorityForKernel();
	}
	// Behind every waiter of the same or a higher priority; FIFO waiters all have priority 0.
	m_queue.insert(std::find_if(m_queue.begin(), m_queue.end(),
	                            [&](const Waiter* w) { return w->priority > waiter.priority; }),
	               &waiter);
	m_waiters++;

	const uint64_t start    = Common::SteadyNs();
	const uint64_t deadline = (timeout != nullptr ? TickDeadline(start, *timeout) : UINT64_MAX);
	for (uint64_t now = start; !waiter.released && now < deadline; now = Common::SteadyNs()) {
		if (timeout == nullptr) {
			waiter.cond_var.Wait(&m_mutex);
		} else {
			waiter.cond_var.WaitFor(&m_mutex, static_cast<uint32_t>((deadline - now + 999) / 1000));
		}
	}
	if (!waiter.released) {
		std::erase(m_queue, &waiter);
		waiter.result = m_bits;
		waiter.error  = KERNEL_ERROR_ETIMEDOUT;
	}
	if (timeout != nullptr) {
		const uint64_t elapsed = (Common::SteadyNs() - start) / 1000;
		*timeout = (elapsed < *timeout ? *timeout - static_cast<KernelUseconds>(elapsed) : 0);
	}
	if (--m_waiters == 0 && m_deleted) {
		m_last_waiter_left.Signal();
	}

	*result = waiter.result;
	return waiter.error;
}

void KernelEventFlagPrivate::Set(uint64_t bits) {
	Common::LockGuard lock(m_mutex);

	m_bits |= bits;
	// In queue order: a released waiter's clear applies before the next one is tested.
	for (auto it = m_queue.begin(); it != m_queue.end();) {
		auto* waiter = *it;
		if (!Satisfied(m_bits, waiter->bits, waiter->mode)) {
			++it;
			continue;
		}
		waiter->result = m_bits;
		m_bits         = Cleared(m_bits, waiter->bits, waiter->mode);
		waiter->Release(OK);
		it = m_queue.erase(it);
	}
}

void KernelEventFlagPrivate::Clear(uint64_t bits) {
	Common::LockGuard lock(m_mutex);

	m_bits &= bits;
}

int KernelEventFlagPrivate::Cancel(uint64_t bits) {
	Common::LockGuard lock(m_mutex);

	m_bits = bits;
	ReleaseAll(KERNEL_ERROR_ECANCELED);
	return m_waiters;
}

// The released waiters report a zero pattern.
void KernelEventFlagPrivate::ReleaseAll(int error) {
	for (auto* waiter: m_queue) {
		waiter->Release(error);
	}
	m_queue.clear();
}

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr,
                                        uint64_t init_pattern, const void* param) {
	PRINT_NAME();

	// Known attributes only, at most one queue order and at most one waiter count.
	if (ef == nullptr || name == nullptr || param != nullptr || (attr & ~0x33u) != 0 ||
	    (attr & 0x03u) == 0x03u || (attr & 0x30u) == 0x30u) {
		return KERNEL_ERROR_EINVAL;
	}
	if (strnlen(name, 32) == 32) {
		return KERNEL_ERROR_ENAMETOOLONG;
	}

	*ef = new KernelEventFlagPrivate(attr, init_pattern);

	LOGF("\tEventFlag create: %s\n", name);

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag ef) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	delete ef;

	return OK;
}

int KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat, KernelUseconds* timeout) {
	PRINT_NAME();

	if (!ValidWait(bit_pattern, wait_mode)) {
		return KERNEL_ERROR_EINVAL;
	}
	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	uint64_t  pattern = 0;
	const int ret     = ef->Wait(bit_pattern, wait_mode, &pattern, timeout);
	if (result_pat != nullptr) {
		*result_pat = pattern;
	}
	return ret;
}

int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat) {
	PRINT_NAME();

	if (!ValidWait(bit_pattern, wait_mode)) {
		return KERNEL_ERROR_EINVAL;
	}
	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	uint64_t  pattern = 0;
	const int ret     = ef->Poll(bit_pattern, wait_mode, &pattern);
	if (result_pat != nullptr) {
		*result_pat = pattern;
	}
	return ret;
}

int KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	ef->Set(bit_pattern);

	return OK;
}

int KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	ef->Clear(bit_pattern);

	return OK;
}

int KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern,
                                        int* num_wait_threads) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	const int waiters = ef->Cancel(set_pattern);
	if (num_wait_threads != nullptr) {
		*num_wait_threads = waiters;
	}

	return OK;
}

} // namespace Libs::LibKernel::EventFlag
