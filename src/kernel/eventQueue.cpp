#include "kernel/eventQueue.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <functional>
#include <limits>
#include <list>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel::EventQueue {

LIB_NAME("libkernel", "libkernel");

constexpr uint16_t EV_ADD     = 0x01;
constexpr uint16_t EV_ONESHOT = 0x10;
constexpr uint16_t EV_CLEAR   = 0x20;
constexpr uint16_t EV_ERROR   = 0x4000;

static uint64_t MonotonicTimeNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

static std::unordered_map<KernelEqueue, KernelEqueueRef> g_equeues;
static Common::Mutex                                     g_equeues_mutex;
static uint64_t                                          g_next_equeue = 1;

class KernelEqueuePrivate {
public:
	explicit KernelEqueuePrivate(KernelEqueue handle): m_handle(handle) {}
	virtual ~KernelEqueuePrivate();

	KYTY_CLASS_NO_COPY(KernelEqueuePrivate);

	[[nodiscard]] const std::string& GetName() const { return m_name; }
	void                             SetName(const std::string& m_name) { this->m_name = m_name; }

	int AddEvent(const KernelEqueueEvent& event);
	int TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data);
	int DeleteEvent(uintptr_t ident, int16_t filter);

	int  GetTriggeredEvents(KernelEvent* ev, int num);
	int  WaitForEvents(KernelEvent* ev, int num, uint32_t micros);
	void Close();

private:
	struct EventKey {
		uintptr_t ident;
		int16_t   filter;

		/// Compares the complete guest-visible identity of two registrations.
		bool operator==(const EventKey&) const = default;
	};

	struct EventKeyHash {
		/// Hashes an event identifier and filter for average constant-time lookup.
		size_t operator()(const EventKey& key) const noexcept {
			const auto ident_hash  = std::hash<uintptr_t> {}(key.ident);
			const auto filter_hash = std::hash<int16_t> {}(key.filter);
			return ident_hash ^
			       (filter_hash + 0x9e3779b9u + (ident_hash << 6u) + (ident_hash >> 2u));
		}
	};

	/// Pairs a registration with the identity it was indexed under.
	///
	/// Filter callbacks receive a mutable `KernelEqueueEvent*` and may rewrite `ident` or
	/// `filter`, so index maintenance has to use the key captured at registration rather than
	/// the event's current fields. Erasing a mutated key would leave the original index entry
	/// behind, pointing at a node that no longer exists.
	struct EventNode {
		EventKey          key;
		KernelEqueueEvent event;
	};

	using EventList     = std::list<EventNode>;
	using EventIterator = EventList::iterator;

	void TriggerExpiredTimers(uint64_t now_ns);
	bool GetNextTimerWaitMicros(uint64_t now_ns, uint32_t* wait_micros) const;

	EventList                                                 m_events;
	std::unordered_map<EventKey, EventIterator, EventKeyHash> m_event_index;
	Common::Mutex                                             m_mutex;
	Common::CondVar                                           m_cond_var;
	std::string                                               m_name;
	KernelEqueue                                              m_handle = KERNEL_EQUEUE_INVALID;
	bool                                                      m_closed = false;
};

/// Closes the queue before its final pinned reference releases the storage.
KernelEqueuePrivate::~KernelEqueuePrivate() {
	Close();
}

/// Marks the queue closed, releases registrations, and wakes every waiter.
void KernelEqueuePrivate::Close() {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return;
	}
	m_closed = true;
	for (auto& node: m_events) {
		auto& event = node.event;
		if (event.filter.delete_event_func != nullptr) {
			auto owner = event.filter.owner;
			event.filter.delete_event_func(m_handle, &event);
		}
	}
	m_events.clear();
	m_event_index.clear();
	m_cond_var.SignalAll();
}

/// Copies up to `num` ready events into `ev` and retires consumed one-shot registrations.
int KernelEqueuePrivate::GetTriggeredEvents(KernelEvent* ev, int num) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);
	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}

	TriggerExpiredTimers(MonotonicTimeNs());

	int ret = 0;

	for (auto it = m_events.begin(); it != m_events.end();) {
		auto& event = it->event;
		bool  erase = false;
		while (event.triggered) {
			ev[ret++] = event.event;
			if ((event.event.flags & EV_ONESHOT) != 0) {
				erase = true;
				break;
			}

			if (event.filter.reset_func != nullptr) {
				event.filter.reset_func(&event);
			} else if ((event.event.flags & EV_CLEAR) != 0) {
				event.triggered    = false;
				event.event.fflags = 0;
				event.event.data   = 0;
			}

			if (!event.pending_events.empty()) {
				event.event = event.pending_events.front();
				event.pending_events.pop_front();
				event.triggered = true;
			}

			if (ret >= num) {
				break;
			}
		}
		if (erase) {
			// Erase by the registered key: a trigger callback may have rewritten the ident or
			// filter stored inside the event since it was added.
			m_event_index.erase(it->key);
			it = m_events.erase(it);
		} else {
			it = std::next(it);
		}
		if (ret >= num) {
			break;
		}
	}

	return ret;
}

void KernelEqueuePrivate::TriggerExpiredTimers(uint64_t now_ns) {
	for (auto& node: m_events) {
		auto& event = node.event;
		if (!event.triggered && event.deadline_ns != 0 && event.deadline_ns <= now_ns) {
			event.triggered = true;
		}
	}
}

bool KernelEqueuePrivate::GetNextTimerWaitMicros(uint64_t now_ns, uint32_t* wait_micros) const {
	EXIT_IF(wait_micros == nullptr);

	uint64_t nearest_deadline = UINT64_MAX;
	for (const auto& node: m_events) {
		const auto& event = node.event;
		if (!event.triggered && event.deadline_ns != 0) {
			nearest_deadline = std::min(nearest_deadline, event.deadline_ns);
		}
	}
	if (nearest_deadline == UINT64_MAX) {
		return false;
	}

	const auto remaining_ns = nearest_deadline > now_ns ? nearest_deadline - now_ns : 0;
	const auto rounded_us   = std::max<uint64_t>(1, (remaining_ns + 999u) / 1000u);
	*wait_micros            = static_cast<uint32_t>(std::min<uint64_t>(rounded_us, UINT32_MAX));
	return true;
}

int KernelEqueuePrivate::WaitForEvents(KernelEvent* ev, int num, uint32_t micros) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);
	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;
	t.Start();

	for (;;) {
		int ret = GetTriggeredEvents(ev, num);

		if (ret != 0 || (elapsed >= micros && micros != 0)) {
			return ret;
		}

		uint32_t   timer_wait = 0;
		const bool has_timer  = GetNextTimerWaitMicros(MonotonicTimeNs(), &timer_wait);
		if (micros == 0 && !has_timer) {
			m_cond_var.Wait(&m_mutex);
		} else {
			const auto external_wait = micros != 0 ? micros - elapsed : UINT32_MAX;
			m_cond_var.WaitFor(&m_mutex,
			                   has_timer ? std::min(external_wait, timer_wait) : external_wait);
		}

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
	}

	return 0;
}

/// Adds a registration or updates the mutable metadata of an existing registration.
int KernelEqueuePrivate::AddEvent(const KernelEqueueEvent& event) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	const EventKey key {event.event.ident, event.event.filter};
	const auto     indexed = m_event_index.find(key);
	if (indexed != m_event_index.end()) {
		auto& existing       = indexed->second->event;
		existing.deadline_ns = event.deadline_ns;
		existing.event.udata = event.event.udata;
		for (auto& pending: existing.pending_events) {
			pending.udata = event.event.udata;
		}
	} else {
		m_events.push_back(EventNode {key, event});
		m_event_index.emplace(key, std::prev(m_events.end()));
	}

	m_cond_var.Signal();
	return OK;
}

/// Marks the registration selected by `ident` and `filter` as ready.
int KernelEqueuePrivate::TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	const auto indexed = m_event_index.find(EventKey {ident, filter});
	if (indexed != m_event_index.end()) {
		auto& event = indexed->second->event;

		if (event.filter.trigger_func != nullptr) {
			event.filter.trigger_func(&event, trigger_data);
		} else {
			event.triggered = true;
		}

		m_cond_var.Signal();

		return OK;
	}

	return KERNEL_ERROR_ENOENT;
}

static void UserEventTriggerFunc(KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);
	event->triggered   = true;
	event->event.data  = reinterpret_cast<intptr_t>(trigger_data);
	event->event.udata = trigger_data;
}

static void AmprEventTriggerFunc(KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);
	auto triggered_event = event->event;
	triggered_event.data = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(trigger_data));
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

static void UserEventResetFunc(KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	if ((event->event.flags & EV_CLEAR) != 0) {
		event->triggered    = false;
		event->event.fflags = 0;
		event->event.data   = 0;
	}
}

/// Removes the registration selected by `ident` and `filter`.
int KernelEqueuePrivate::DeleteEvent(uintptr_t ident, int16_t filter) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	const auto indexed = m_event_index.find(EventKey {ident, filter});
	if (indexed != m_event_index.end()) {
		const auto it    = indexed->second;
		auto&      event = it->event;

		if (event.filter.delete_event_func != nullptr) {
			auto owner = event.filter.owner;
			event.filter.delete_event_func(m_handle, &event);
		}

		m_events.erase(it);
		m_event_index.erase(indexed);

		return OK;
	}

	return KERNEL_ERROR_ENOENT;
}

KernelEqueueRef KernelPinEqueue(KernelEqueue eq) {
	if (eq == KERNEL_EQUEUE_INVALID) {
		return {};
	}

	Common::LockGuard lock(g_equeues_mutex);
	const auto        entry = g_equeues.find(eq);
	return entry != g_equeues.end() ? entry->second : KernelEqueueRef {};
}

int KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name) {
	PRINT_NAME();

	if (eq == nullptr || name == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	{
		Common::LockGuard lock(g_equeues_mutex);
		if (g_next_equeue > static_cast<uint64_t>(std::numeric_limits<KernelEqueue>::max())) {
			EXIT("event queue handle space exhausted\n");
		}
		*eq        = static_cast<KernelEqueue>(g_next_equeue++);
		auto owner = std::make_shared<KernelEqueuePrivate>(*eq);
		owner->SetName(std::string(name));
		g_equeues.emplace(*eq, std::move(owner));
	}

	LOGF("\tEqueue create: %s\n", name);

	return OK;
}

int KYTY_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->AddEvent(event);
}

int KYTY_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter,
                                     void* trigger_data) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->TriggerEvent(ident, filter, trigger_data);
}

int KYTY_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->DeleteEvent(ident, filter);
}

int KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq) {
	PRINT_NAME();

	KernelEqueueRef owner;

	{
		Common::LockGuard lock(g_equeues_mutex);
		const auto        entry = g_equeues.find(eq);
		if (entry == g_equeues.end()) {
			return KERNEL_ERROR_EBADF;
		}
		owner = std::move(entry->second);
		g_equeues.erase(entry);
	}

	LOGF("\tEqueue delete: %s\n", owner->GetName().c_str());
	owner->Close();

	return OK;
}

int KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out,
                                   const KernelUseconds* timo) {
	PRINT_NAME();

	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	if (ev == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (num < 1) {
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(out == nullptr);

	LOGF("\tEqueue wait: %s, caller = 0x%016" PRIx64 ", eq = 0x%016" PRIx64 ", ev = 0x%016" PRIx64
	     ", num = %d, timo = %s, thread_id = %d\n",
	     owner->GetName().c_str(), reinterpret_cast<uint64_t>(__builtin_return_address(0)),
	     static_cast<uint64_t>(eq), reinterpret_cast<uint64_t>(ev), num,
	     (timo == nullptr ? "inf" : fmt::format("{}", *timo).c_str()),
	     Common::Thread::GetThreadIdUnique());

	if (timo == nullptr) {
		*out = owner->WaitForEvents(ev, num, 0);
	}

	if (timo != nullptr) {
		if (*timo == 0) {
			*out = owner->GetTriggeredEvents(ev, num);
		} else {
			*out = owner->WaitForEvents(ev, num, *timo);
		}
	}

	if (*out == KERNEL_ERROR_EBADF) {
		return KERNEL_ERROR_EBADF;
	}
	if (*out == 0) {
		LOGF("\tEqueue wait timedout: %s\n", owner->GetName().c_str());
		return KERNEL_ERROR_ETIMEDOUT;
	}

	LOGF("\tEqueue wait received %u events: ident = 0x%016" PRIx64
	     ", filter = %d, flags = 0x%04" PRIx16 ", fflags = 0x%08" PRIx32 ", data = 0x%016" PRIx64
	     ", udata = 0x%016" PRIx64 "\n",
	     *out, static_cast<uint64_t>(ev[0].ident), ev[0].filter, ev[0].flags, ev[0].fflags,
	     static_cast<uint64_t>(ev[0].data), reinterpret_cast<uint64_t>(ev[0].udata));

	return OK;
}

int KYTY_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event add: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	KernelEqueueEvent event {};
	event.event.ident         = static_cast<uintptr_t>(id);
	event.event.filter        = KERNEL_EVFILT_USER;
	event.event.flags         = EV_ADD;
	event.event.fflags        = 0;
	event.event.data          = 0;
	event.event.udata         = nullptr;
	event.filter.trigger_func = UserEventTriggerFunc;
	event.filter.reset_func   = UserEventResetFunc;

	return KernelAddEvent(eq, event);
}

int KYTY_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event edge add: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	KernelEqueueEvent event {};
	event.event.ident         = static_cast<uintptr_t>(id);
	event.event.filter        = KERNEL_EVFILT_USER;
	event.event.flags         = EV_ADD | EV_CLEAR;
	event.event.fflags        = 0;
	event.event.data          = 0;
	event.event.udata         = nullptr;
	event.filter.trigger_func = UserEventTriggerFunc;
	event.filter.reset_func   = UserEventResetFunc;

	return KernelAddEvent(eq, event);
}

int KYTY_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t user event trigger: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	return KernelTriggerEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_USER, udata);
}

int KYTY_SYSV_ABI KernelTriggerUserEventForAll(int id, void* udata) {
	int                          triggered = 0;
	std::vector<KernelEqueueRef> queues;

	{
		Common::LockGuard lock(g_equeues_mutex);
		queues.reserve(g_equeues.size());
		for (const auto& [handle, queue]: g_equeues) {
			queues.push_back(queue);
		}
	}
	for (const auto& eq: queues) {
		if (eq->TriggerEvent(static_cast<uintptr_t>(id), KERNEL_EVFILT_USER, udata) == OK) {
			triggered++;
		}
	}

	return (triggered > 0 ? OK : KERNEL_ERROR_ENOENT);
}

int KYTY_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	return KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_USER);
}

int KYTY_SYSV_ABI KernelAddHRTimerEvent(KernelEqueue eq, int id, const KernelTimespec* ts,
                                        void* udata) {
	if (ts == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll ||
	    static_cast<uint64_t>(ts->tv_sec) >
	        (UINT64_MAX - static_cast<uint64_t>(ts->tv_nsec)) / 1000000000ull) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto delay_ns =
	    static_cast<uint64_t>(ts->tv_sec) * 1000000000ull + static_cast<uint64_t>(ts->tv_nsec);
	const auto now_ns = MonotonicTimeNs();

	KernelEqueueEvent event {};
	event.deadline_ns  = delay_ns <= UINT64_MAX - now_ns ? now_ns + delay_ns : UINT64_MAX;
	event.event.ident  = static_cast<uintptr_t>(id);
	event.event.filter = KERNEL_EVFILT_HRTIMER;
	event.event.flags  = EV_ADD | EV_ONESHOT;
	event.event.fflags = 0;
	event.event.data   = 0;
	event.event.udata  = udata;
	return KernelAddEvent(eq, event);
}

int KYTY_SYSV_ABI KernelDeleteHRTimerEvent(KernelEqueue eq, int id) {
	return KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_HRTIMER);
}

int KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t AMPR event add: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	if (eq != KERNEL_EQUEUE_INVALID) {
		KernelEqueueEvent event {};
		event.event.ident         = static_cast<uintptr_t>(id);
		event.event.filter        = KERNEL_EVFILT_USER;
		event.event.flags         = EV_ADD | EV_CLEAR;
		event.event.fflags        = 0;
		event.event.data          = 0;
		event.event.udata         = udata;
		event.filter.trigger_func = AmprEventTriggerFunc;
		event.filter.reset_func   = UserEventResetFunc;
		(void)KernelAddEvent(eq, event);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelAddAmprSystemEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t AMPR system event add: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	return KernelAddAmprEvent(eq, id, udata);
}

int KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t AMPR event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	if (eq != KERNEL_EQUEUE_INVALID) {
		(void)KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_USER);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteAmprSystemEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t AMPR system event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq),
	     id);

	return KernelDeleteAmprEvent(eq, id);
}

intptr_t KYTY_SYSV_ABI KernelGetEventData(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->data;
	}

	return 0;
}

intptr_t KYTY_SYSV_ABI KernelGetEventFflags(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->fflags;
	}

	return 0;
}

int KYTY_SYSV_ABI KernelGetEventFilter(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->filter;
	}

	return 0;
}

uintptr_t KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->ident;
	}

	return 0;
}

void* KYTY_SYSV_ABI KernelGetEventUserData(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->udata;
	}

	return nullptr;
}

int KYTY_SYSV_ABI KernelGetEventError(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr && (ev->flags & EV_ERROR) != 0) {
		return static_cast<int>(ev->data);
	}

	return 0;
}

} // namespace Libs::LibKernel::EventQueue
