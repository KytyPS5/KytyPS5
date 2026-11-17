#include "kernel/eventFlag.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <chrono>
#include <thread>

namespace Libs::LibKernel::EventFlag {

LIB_NAME("libkernel", "libkernel");

class KernelEventFlagPrivate {
public:
	enum class Result { Ok, AlreadyWaiting, TimedOut, Canceled, Deleted };

	enum class ClearMode { None, All, Bits };

	enum class WaitMode { And, Or };

	KernelEventFlagPrivate(const std::string& name, bool single, bool /*fifo*/, uint64_t bits)
	    : m_name(name), m_single_thread(single), m_bits(bits) {}
	virtual ~KernelEventFlagPrivate();

	KYTY_CLASS_NO_COPY(KernelEventFlagPrivate);

	void   Set(uint64_t bits);
	void   Clear(uint64_t bits);
	void   Cancel(uint64_t bits, int* num_waiting_threads);
	Result Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result,
	            uint32_t* ptr_micros);

	Result Poll(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result) {
		uint32_t micros = 0;
		return Wait(bits, wait_mode, clear_mode, result, &micros);
	}

	[[nodiscard]] const std::string& GetName() const { return m_name; }

private:
	enum class Status { Set, Canceled, Deleted };

	Common::Mutex   m_mutex;
	Common::CondVar m_cond_var;
	Status          m_status          = Status::Set;
	int             m_waiting_threads = 0;
	std::string     m_name;
	bool            m_single_thread = false;
	uint64_t        m_bits          = 0;
};

namespace {

struct EventFlagWaitMode {
	KernelEventFlagPrivate::WaitMode  wait  = KernelEventFlagPrivate::WaitMode::And;
	KernelEventFlagPrivate::ClearMode clear = KernelEventFlagPrivate::ClearMode::None;
};

EventFlagWaitMode DecodeEventFlagWaitMode(uint32_t wait_mode) {
	EventFlagWaitMode mode;

	switch (wait_mode & 0xfu) {
		case 0x01: mode.wait = KernelEventFlagPrivate::WaitMode::And; break;
		case 0x02: mode.wait = KernelEventFlagPrivate::WaitMode::Or; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	switch (wait_mode & 0xf0u) {
		case 0x00: mode.clear = KernelEventFlagPrivate::ClearMode::None; break;
		case 0x10: mode.clear = KernelEventFlagPrivate::ClearMode::All; break;
		case 0x20: mode.clear = KernelEventFlagPrivate::ClearMode::Bits; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	return mode;
}

// Guests poll event flags (zero-timeout waits) in loops. On the console every poll is a system
// call; here a failed poll returns in about 0.1 us, so such a loop spins, and the job threads it
// feeds never get idle long enough to sleep. Wwise's audio thread does this while it waits for its
// next tick. Once one thread has failed to poll one flag many times back to back, give up the CPU
// after each further failed poll: first yield, then sleep briefly. A successful poll or a
// different flag resets the count. So does a gap longer than POLL_LOOP_GAP between two failed
// polls: such a loop does real work between its polls and is not slowed down.
constexpr uint32_t POLL_SPIN_FAILURES  = 16;
constexpr uint32_t POLL_YIELD_FAILURES = 64;
constexpr uint32_t POLL_SLEEP_MICROS   = 50;
constexpr auto     POLL_LOOP_GAP       = std::chrono::microseconds(100);

struct PollBackoff {
	const KernelEventFlagPrivate*         flag     = nullptr;
	uint32_t                              failures = 0;
	std::chrono::steady_clock::time_point last_failure;
	bool                                  logged = false;
};

thread_local PollBackoff t_poll_backoff;

void OnEventFlagPoll(const KernelEventFlagPrivate* flag, bool success) {
	auto& state = t_poll_backoff;
	if (success) {
		state.failures = 0;
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	const bool same_loop =
	    state.failures > 0 && state.flag == flag && now - state.last_failure <= POLL_LOOP_GAP;
	state.flag         = flag;
	state.failures     = same_loop ? state.failures + 1 : 1;
	state.last_failure = now;
	if (state.failures <= POLL_SPIN_FAILURES || !Config::PollBackoffEnabled()) {
		return;
	}
	if (state.failures <= POLL_YIELD_FAILURES) {
		std::this_thread::yield();
	} else {
		if (!state.logged) {
			state.logged = true;
			LOGF("\tEventFlag poll backoff: thread %d sleeps between failed polls of %s\n",
			     Common::Thread::GetThreadIdUnique(), flag->GetName().c_str());
		}
		Common::Thread::SleepMicro(POLL_SLEEP_MICROS);
	}
	// The loop's next poll comes after this pause; measure the gap from its end.
	state.last_failure = std::chrono::steady_clock::now();
}

} // namespace

KernelEventFlagPrivate::~KernelEventFlagPrivate() {
	Common::LockGuard lock(m_mutex);

	while (m_status != Status::Set) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_status = Status::Deleted;

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Wait(uint64_t bits, WaitMode wait_mode,
                                                            ClearMode clear_mode, uint64_t* result,
                                                            uint32_t* ptr_micros) {
	Common::LockGuard lock(m_mutex);

	uint32_t micros     = 0;
	bool     infinitely = true;
	if (ptr_micros != nullptr) {
		micros     = *ptr_micros;
		infinitely = false;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;

	auto update_timeout = [&]() {
		if (ptr_micros != nullptr) {
			*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
		}
	};

	if (m_single_thread && m_waiting_threads > 0) {
		return Result::AlreadyWaiting;
	}

	while (!((wait_mode == WaitMode::And && (m_bits & bits) == bits) ||
	         (wait_mode == WaitMode::Or && (m_bits & bits) != 0))) {
		if ((elapsed >= micros && !infinitely)) {
			if (result != nullptr) {
				*result = m_bits;
			}
			update_timeout();
			return Result::TimedOut;
		}

		m_waiting_threads++;

		if (infinitely) {
			m_cond_var.Wait(&m_mutex);
		} else {
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
		}

		m_waiting_threads--;

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);

		switch (m_status) {
			case Status::Canceled:
				if (result != nullptr) {
					*result = m_bits;
				}
				update_timeout();
				return Result::Canceled;
			case Status::Deleted:
				if (result != nullptr) {
					*result = m_bits;
				}
				update_timeout();
				return Result::Deleted;
			case Status::Set: break;
		}
	}

	if (result != nullptr) {
		*result = m_bits;
	}
	update_timeout();

	switch (clear_mode) {
		case ClearMode::All: m_bits = 0; break;
		case ClearMode::Bits: m_bits &= ~bits; break;
		case ClearMode::None: break;
	}

	return Result::Ok;
}

void KernelEventFlagPrivate::Set(uint64_t bits) {
	Common::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_bits |= bits;

	m_cond_var.SignalAll();
}

void KernelEventFlagPrivate::Clear(uint64_t bits) {
	Common::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_bits &= bits;
}

void KernelEventFlagPrivate::Cancel(uint64_t bits, int* num_waiting_threads) {
	Common::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	if (num_waiting_threads != nullptr) {
		*num_waiting_threads = m_waiting_threads;
	}

	m_status = Status::Canceled;
	m_bits   = bits;

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_status = Status::Set;
}

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr,
                                        uint64_t init_pattern, const void* param) {
	PRINT_NAME();

	if (ef == nullptr || name == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	if (param != nullptr || (attr & ~0x33u) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	bool single = true;
	bool fifo   = true;

	switch (attr & 0x0fu) {
		case 0x00:
		case 0x01: fifo = true; break;
		case 0x02: fifo = false; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	switch (attr & 0xf0u) {
		case 0x00:
		case 0x10: single = true; break;
		case 0x20: single = false; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	*ef = new KernelEventFlagPrivate(std::string(name), single, fifo, init_pattern);

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

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto mode   = DecodeEventFlagWaitMode(wait_mode);
	const bool poll   = timeout != nullptr && *timeout == 0;
	auto       result = ef->Wait(bit_pattern, mode.wait, mode.clear, result_pat, timeout);
	if (poll) {
		OnEventFlagPoll(ef, result == KernelEventFlagPrivate::Result::Ok);
	}

	int ret = OK;

	switch (result) {
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelEventFlagPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto mode   = DecodeEventFlagWaitMode(wait_mode);
	auto       result = ef->Poll(bit_pattern, mode.wait, mode.clear, result_pat);
	OnEventFlagPoll(ef, result == KernelEventFlagPrivate::Result::Ok);

	int ret = OK;

	switch (result) {
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut:
		case KernelEventFlagPrivate::Result::Canceled:
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
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

	ef->Cancel(set_pattern, num_wait_threads);

	return OK;
}

} // namespace Libs::LibKernel::EventFlag
