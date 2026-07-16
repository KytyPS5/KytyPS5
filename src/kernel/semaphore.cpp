#include "kernel/semaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel {
void KernelDispatchPendingSignalForCurrentThread();
} // namespace Libs::LibKernel

namespace Libs::LibKernel::Semaphore {

LIB_NAME("libkernel", "libkernel");

constexpr uint32_t SIGNAL_APC_POLL_MICROS = 10000;

class KernelSemaPrivate {
public:
	enum class Result { Ok, TimedOut, Canceled, Deleted, InvalCount };

	KernelSemaPrivate(const std::string& name, bool fifo, int init_count, int max_count)
	    : m_name(name), m_fifo_order(fifo), m_count(init_count), m_init_count(init_count),
	      m_max_count(max_count) {};
	virtual ~KernelSemaPrivate();

	KYTY_CLASS_NO_COPY(KernelSemaPrivate);

	Result Cancel(int set_count, int* num_waiting_threads);
	Result Signal(int signal_count);
	Result Wait(int need_count, uint32_t* ptr_micros);

	Result Poll(int need_count) {
		uint32_t micros = 0;
		return Wait(need_count, &micros);
	}

	[[nodiscard]] const std::string& GetName() const { return m_name; }

private:
	enum class Status { Set, Deleted };

	struct WaitingThread {
		int    id         = 0;
		int    priority   = 700;
		int    need_count = 0;
		Result result     = Result::TimedOut;
		bool   ready      = false;
	};

	void AddWaiter(WaitingThread* waiter);
	void RemoveWaiter(WaitingThread* waiter);
	void WakeWaiters();

	Common::Mutex               m_mutex;
	Common::CondVar             m_cond_var;
	std::string                 m_name;
	Status                      m_status = Status::Set;
	std::vector<WaitingThread*> m_waiting_threads;
	bool                        m_fifo_order;
	int                         m_count;
	int                         m_init_count;
	int                         m_max_count;
};

KernelSemaPrivate::~KernelSemaPrivate() {
	Common::LockGuard lock(m_mutex);

	while (m_status != Status::Set) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_status = Status::Deleted;

	for (auto* waiter: m_waiting_threads) {
		if (!waiter->ready) {
			waiter->result = Result::Deleted;
			waiter->ready  = true;
		}
	}

	m_cond_var.SignalAll();

	while (!m_waiting_threads.empty()) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}
}

KernelSemaPrivate::Result KernelSemaPrivate::Cancel(int set_count, int* num_waiting_threads) {
	Common::LockGuard lock(m_mutex);

	if (set_count > m_max_count) {
		return Result::InvalCount;
	}

	if (m_status == Status::Deleted) {
		return Result::Deleted;
	}

	if (num_waiting_threads != nullptr) {
		*num_waiting_threads = static_cast<int>(
		    std::count_if(m_waiting_threads.begin(), m_waiting_threads.end(),
		                  [](const WaitingThread* waiter) { return !waiter->ready; }));
	}

	m_count = (set_count < 0 ? m_init_count : set_count);

	for (auto* waiter: m_waiting_threads) {
		if (!waiter->ready) {
			waiter->result = Result::Canceled;
			waiter->ready  = true;
		}
	}

	m_cond_var.SignalAll();

	return Result::Ok;
}

KernelSemaPrivate::Result KernelSemaPrivate::Signal(int signal_count) {
	Common::LockGuard lock(m_mutex);

	if (m_status == Status::Deleted) {
		return Result::Deleted;
	}

	if (m_count + signal_count > m_max_count) {
		return Result::InvalCount;
	}

	m_count += signal_count;

	WakeWaiters();

	m_cond_var.SignalAll();

	return Result::Ok;
}

void KernelSemaPrivate::AddWaiter(WaitingThread* waiter) {
	if (m_fifo_order) {
		m_waiting_threads.push_back(waiter);
		return;
	}

	auto it = m_waiting_threads.begin();
	while (it != m_waiting_threads.end() && (*it)->priority <= waiter->priority) {
		++it;
	}
	m_waiting_threads.insert(it, waiter);
}

void KernelSemaPrivate::RemoveWaiter(WaitingThread* waiter) {
	auto it = std::find(m_waiting_threads.begin(), m_waiting_threads.end(), waiter);
	if (it != m_waiting_threads.end()) {
		m_waiting_threads.erase(it);
	}
}

void KernelSemaPrivate::WakeWaiters() {
	for (auto* waiter: m_waiting_threads) {
		if (!waiter->ready && waiter->need_count <= m_count) {
			m_count -= waiter->need_count;
			waiter->result = Result::Ok;
			waiter->ready  = true;
		}
	}
}

KernelSemaPrivate::Result KernelSemaPrivate::Wait(int need_count, uint32_t* ptr_micros) {
	Common::LockGuard lock(m_mutex);

	if (need_count < 1 || need_count > m_max_count) {
		return Result::InvalCount;
	}

	if (m_status == Status::Deleted) {
		return Result::Deleted;
	}

	uint32_t micros     = 0;
	bool     infinitely = true;
	if (ptr_micros != nullptr) {
		micros     = *ptr_micros;
		infinitely = false;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;
	t.Start();

	int id = Common::Thread::GetThreadIdUnique();

	if (m_count >= need_count) {
		m_count -= need_count;
		if (ptr_micros != nullptr) {
			*ptr_micros = micros;
		}
		return Result::Ok;
	}

	if (!infinitely && micros == 0) {
		return Result::TimedOut;
	}

	WaitingThread waiter {};
	waiter.id         = id;
	waiter.priority   = (m_fifo_order ? 0 : Libs::LibKernel::PthreadGetCurrentPriorityForKernel());
	waiter.need_count = need_count;
	AddWaiter(&waiter);

	while (!waiter.ready) {
		if ((elapsed >= micros && !infinitely)) {
			RemoveWaiter(&waiter);
			*ptr_micros = 0;
			return Result::TimedOut;
		}

		if (infinitely) {
			m_cond_var.WaitFor(&m_mutex, SIGNAL_APC_POLL_MICROS);
		} else {
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
		}

		m_mutex.Unlock();
		LibKernel::KernelDispatchPendingSignalForCurrentThread();
		m_mutex.Lock();

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
	}

	RemoveWaiter(&waiter);

	if (ptr_micros != nullptr) {
		*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
	}

	return waiter.result;
}

int KYTY_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init,
                                   int max, void* opt) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(sem == nullptr);

	if (name == nullptr || attr > 2 || init < 0 || max <= 0 || init > max) {
		return KERNEL_ERROR_EINVAL;
	}

	bool fifo = false;

	switch (attr) {
		case 0x01: fifo = true; break;
		case 0x02:
		default: fifo = false; break;
	}

	*sem = new KernelSemaPrivate(std::string(name), fifo, init, max);

	LOGF("\t Semaphore create: %s, ptr=0x%016" PRIx64 ", init=%d, max=%d\n", name,
	     reinterpret_cast<uint64_t>(*sem), init, max);

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteSema(KernelSema sem) {
	PRINT_NAME();

	if (sem == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	delete sem;

	return OK;
}

int KYTY_SYSV_ABI KernelWaitSema(KernelSema sem, int need, KernelUseconds* time) {
	if (sem == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	auto result = sem->Wait(need, time);

	int ret = OK;

	switch (result) {
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount: ret = KERNEL_ERROR_EINVAL; break;
		case KernelSemaPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelSemaPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelPollSema(KernelSema sem, int need) {
	PRINT_NAME();

	if (sem == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	LOGF("\t Semaphore poll: %s, %d\n", sem->GetName().c_str(), need);

	auto result = sem->Poll(need);

	int ret = OK;

	switch (result) {
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount: ret = KERNEL_ERROR_EINVAL; break;
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelSignalSema(KernelSema sem, int count) {
	if (sem == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	auto result = sem->Signal(count);

	int ret = OK;

	switch (result) {
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount:
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EINVAL; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelCancelSema(KernelSema sem, int count, int* threads) {
	PRINT_NAME();

	if (sem == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	auto result = sem->Cancel(count, threads);

	int ret = OK;

	switch (result) {
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount:
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EINVAL; break;
	}

	return ret;
}

} // namespace Libs::LibKernel::Semaphore

namespace Libs::Posix {

LIB_NAME("Posix", "libkernel");

namespace {

constexpr uint16_t POSIX_SEM_MAGIC        = 0x09fa;
constexpr int      POSIX_SEM_VALUE_MAX    = 0x7fffffff;
constexpr uint32_t SIGNAL_APC_POLL_MICROS = 10000;

struct PosixSemGuest {
	uint16_t          magic;
	uint16_t          nameid;
	volatile uint32_t has_waiters;
	volatile uint32_t count;
	uint32_t          flags;
};

static_assert(sizeof(PosixSemGuest) == 16);

class PosixSemPrivate {
public:
	PosixSemPrivate(PosixSemGuest* guest, unsigned int value, uint32_t flags)
	    : m_guest(guest), m_count(static_cast<int>(value)) {
		SyncGuest(flags);
	}

	int Wait(uint32_t* micros) {
		Common::LockGuard lock(m_mutex);

		uint32_t      elapsed    = 0;
		const bool    timed_wait = (micros != nullptr);
		Common::Timer timer;
		timer.Start();

		while (m_count <= 0) {
			if (timed_wait && elapsed >= *micros) {
				SyncGuest();
				return POSIX_ETIMEDOUT;
			}

			m_waiters++;
			SyncGuest();

			if (timed_wait) {
				m_cond_var.WaitFor(&m_mutex, *micros - elapsed);
			} else {
				m_cond_var.WaitFor(&m_mutex, SIGNAL_APC_POLL_MICROS);
			}

			m_mutex.Unlock();
			LibKernel::KernelDispatchPendingSignalForCurrentThread();
			m_mutex.Lock();

			m_waiters--;
			elapsed = static_cast<uint32_t>(timer.GetTimeS() * 1000000.0);
		}

		m_count--;
		SyncGuest();

		return OK;
	}

	int TryWait() {
		Common::LockGuard lock(m_mutex);

		if (m_count <= 0) {
			SyncGuest();
			return POSIX_EAGAIN;
		}

		m_count--;
		SyncGuest();

		return OK;
	}

	int Post() {
		Common::LockGuard lock(m_mutex);

		if (m_count == POSIX_SEM_VALUE_MAX) {
			return POSIX_EOVERFLOW;
		}

		m_count++;
		SyncGuest();
		m_cond_var.Signal();

		return OK;
	}

	int GetValue() {
		Common::LockGuard lock(m_mutex);

		return m_count;
	}

private:
	void SyncGuest(uint32_t flags = std::numeric_limits<uint32_t>::max()) {
		if (m_guest != nullptr) {
			m_guest->magic       = POSIX_SEM_MAGIC;
			m_guest->nameid      = 0;
			m_guest->has_waiters = (m_waiters > 0 ? 1u : 0u);
			m_guest->count       = static_cast<uint32_t>(m_count);
			if (flags != std::numeric_limits<uint32_t>::max()) {
				m_guest->flags = flags;
			}
		}
	}

	Common::Mutex   m_mutex;
	Common::CondVar m_cond_var;
	PosixSemGuest*  m_guest   = nullptr;
	int             m_count   = 0;
	uint32_t        m_waiters = 0;
};

Common::Mutex                                        g_posix_sem_mutex;
std::unordered_map<PosixSemGuest*, PosixSemPrivate*> g_posix_sems;

static int PosixToKernel(int posix_errno) {
	return (posix_errno >= 0 && posix_errno <= 102 ? posix_errno - 2147352576
	                                               : LibKernel::KERNEL_ERROR_EINVAL);
}

static PosixSemPrivate* GetSem(void* sem) {
	if (sem == nullptr) {
		return nullptr;
	}

	Common::LockGuard lock(g_posix_sem_mutex);

	const auto it = g_posix_sems.find(static_cast<PosixSemGuest*>(sem));
	return (it != g_posix_sems.end() ? it->second : nullptr);
}

static int SetErrnoReturn(int posix_errno) {
	*GetErrorAddr() = posix_errno;
	return -1;
}

static int SemInitImpl(void* sem, int pshared, unsigned int value, const char* name) {
	if (sem == nullptr || value > POSIX_SEM_VALUE_MAX) {
		return POSIX_EINVAL;
	}

	auto* guest = static_cast<PosixSemGuest*>(sem);
	auto* obj   = new PosixSemPrivate(guest, value, static_cast<uint32_t>(pshared));

	{
		Common::LockGuard lock(g_posix_sem_mutex);

		if (auto it = g_posix_sems.find(guest); it != g_posix_sems.end()) {
			delete it->second;
			it->second = obj;
		} else {
			g_posix_sems.insert({guest, obj});
		}
	}

	static std::atomic<uint32_t> log_count {0};
	const auto                   count = log_count.fetch_add(1);
	if (count < 32) {
		LOGF("\t POSIX semaphore init: %s, value = %u, pshared = %d\n",
		     (name != nullptr ? name : "sem"), value, pshared);
	}

	return OK;
}

static int SemDestroyImpl(void* sem) {
	if (sem == nullptr) {
		return POSIX_EINVAL;
	}

	auto*            guest = static_cast<PosixSemGuest*>(sem);
	PosixSemPrivate* obj   = nullptr;

	{
		Common::LockGuard lock(g_posix_sem_mutex);

		const auto it = g_posix_sems.find(guest);
		if (it == g_posix_sems.end()) {
			return POSIX_EINVAL;
		}

		obj = it->second;
		g_posix_sems.erase(it);
	}

	delete obj;
	guest->magic       = 0;
	guest->has_waiters = 0;
	guest->count       = 0;
	guest->flags       = 0;

	return OK;
}

static int SemTimedwaitImpl(void* sem, uint32_t* micros) {
	auto* obj = GetSem(sem);
	if (obj == nullptr) {
		return POSIX_EINVAL;
	}

	return obj->Wait(micros);
}

static bool AbsTimespecToMicros(const LibKernel::KernelTimespec* abstime, uint32_t* micros) {
	if (abstime == nullptr || micros == nullptr || abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
	    abstime->tv_nsec >= 1000000000) {
		return false;
	}

	LibKernel::KernelTimespec now {};
	if (LibKernel::KernelClockGettime(0, &now) != OK) {
		return false;
	}

	const int64_t sec_delta  = abstime->tv_sec - now.tv_sec;
	const int64_t nsec_delta = abstime->tv_nsec - now.tv_nsec;
	const int64_t usec_delta = sec_delta * 1000000 + nsec_delta / 1000;

	if (usec_delta <= 0) {
		*micros = 0;
	} else if (usec_delta > std::numeric_limits<uint32_t>::max()) {
		*micros = std::numeric_limits<uint32_t>::max();
	} else {
		*micros = static_cast<uint32_t>(usec_delta);
	}

	return true;
}

} // namespace

int KYTY_SYSV_ABI sem_init(void* sem, int pshared, unsigned int value) {
	PRINT_NAME();

	const int result = SemInitImpl(sem, pshared, value, nullptr);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int KYTY_SYSV_ABI sem_destroy(void* sem) {
	PRINT_NAME();

	const int result = SemDestroyImpl(sem);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int KYTY_SYSV_ABI sem_wait(void* sem) {
	const int result = SemTimedwaitImpl(sem, nullptr);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int KYTY_SYSV_ABI sem_trywait(void* sem) {
	auto* obj = GetSem(sem);
	if (obj == nullptr) {
		return SetErrnoReturn(POSIX_EINVAL);
	}

	const int result = obj->TryWait();
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int KYTY_SYSV_ABI sem_reltimedwait_np(void* sem, uint32_t usec) {
	const int result = SemTimedwaitImpl(sem, &usec);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int KYTY_SYSV_ABI sem_timedwait(void* sem, const LibKernel::KernelTimespec* abstime) {
	uint32_t micros = 0;
	if (!AbsTimespecToMicros(abstime, &micros)) {
		return SetErrnoReturn(POSIX_EINVAL);
	}

	const int result = SemTimedwaitImpl(sem, &micros);
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int KYTY_SYSV_ABI sem_post(void* sem) {
	auto* obj = GetSem(sem);
	if (obj == nullptr) {
		return SetErrnoReturn(POSIX_EINVAL);
	}

	const int result = obj->Post();
	return (result == OK ? 0 : SetErrnoReturn(result));
}

int KYTY_SYSV_ABI sem_getvalue(void* sem, int* value) {
	auto* obj = GetSem(sem);
	if (obj == nullptr || value == nullptr) {
		return SetErrnoReturn(POSIX_EINVAL);
	}

	*value = obj->GetValue();

	return 0;
}

} // namespace Libs::Posix

namespace Libs::LibKernel::Semaphore {

int KYTY_SYSV_ABI PthreadSemInit(void* sem, int flag, unsigned int value, const char* name) {
	PRINT_NAME();

	if (flag != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const int result = Posix::SemInitImpl(sem, 0, value, name);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int KYTY_SYSV_ABI PthreadSemDestroy(void* sem) {
	PRINT_NAME();

	const int result = Posix::SemDestroyImpl(sem);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int KYTY_SYSV_ABI PthreadSemWait(void* sem) {
	const int result = Posix::SemTimedwaitImpl(sem, nullptr);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int KYTY_SYSV_ABI PthreadSemTrywait(void* sem) {
	auto* obj = Posix::GetSem(sem);
	if (obj == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	const int result = obj->TryWait();
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int KYTY_SYSV_ABI PthreadSemTimedwait(void* sem, KernelUseconds usec) {
	uint32_t  micros = usec;
	const int result = Posix::SemTimedwaitImpl(sem, &micros);
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int KYTY_SYSV_ABI PthreadSemPost(void* sem) {
	auto* obj = Posix::GetSem(sem);
	if (obj == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	const int result = obj->Post();
	return (result == OK ? OK : Posix::PosixToKernel(result));
}

int KYTY_SYSV_ABI PthreadSemGetvalue(void* sem, int* value) {
	auto* obj = Posix::GetSem(sem);
	if (obj == nullptr || value == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*value = obj->GetValue();

	return OK;
}

} // namespace Libs::LibKernel::Semaphore
