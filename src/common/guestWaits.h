#ifndef KYTY_COMMON_GUEST_WAITS_H_
#define KYTY_COMMON_GUEST_WAITS_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Opt-in hang diagnostics: what each guest thread is blocked on, and since when.
//
// Set KYTY_GUEST_WAIT_DUMP_SECONDS=N before starting the emulator to log a dump of every guest
// thread's wait state every N seconds. When it is unset, each hook costs one relaxed atomic load
// and nothing is recorded.
namespace Common::GuestWaits {

enum class Kind : uint8_t {
	None,
	Mutex,
	Rwlock,
	Cond,
	Join,
	Equeue,
	Sema,
	EventFlag,
	Address,
	Aio,
	Ampr,
	Sleep,
};

const char* KindName(Kind kind);

namespace Detail {
extern std::atomic<bool> g_enabled;
} // namespace Detail

inline bool IsEnabled() {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled);

// Records a wait for the calling thread. Returns false, and records nothing, when the thread is
// already inside a recorded wait: the outermost wait (usually the guest-facing entry point, which
// knows the guest caller) is the one kept.
bool BeginWait(Kind kind, const void* object, const char* object_name, uint64_t caller);
void EndWait();

// Name of the calling thread, shown in dumps.
void SetCurrentThreadName(const char* name);

class Scope {
public:
	Scope(Kind kind, const void* object, const char* object_name, uint64_t caller) {
		if (IsEnabled()) {
			m_active = BeginWait(kind, object, object_name, caller);
		}
	}
	~Scope() {
		if (m_active) {
			EndWait();
		}
	}
	Scope(const Scope&)            = delete;
	Scope(Scope&&)                 = delete;
	Scope& operator=(const Scope&) = delete;
	Scope& operator=(Scope&&)      = delete;

private:
	bool m_active = false;
};

struct ThreadState {
	std::string name;
	uint32_t    id = 0; // Registration order; stable for the thread's lifetime.

	// Current wait; kind is None while the thread is running guest code.
	Kind        kind   = Kind::None;
	uint64_t    object = 0;
	std::string object_name;
	uint64_t    caller     = 0;
	uint64_t    waiting_ns = 0;

	// Most recent finished wait. A thread polling in a loop shows a large completed_waits count
	// with the same last_caller in every dump.
	Kind     last_kind       = Kind::None;
	uint64_t last_object     = 0;
	uint64_t last_caller     = 0;
	uint64_t since_last_ns   = 0;
	uint64_t completed_waits = 0;
};

uint64_t                 NowNs();
std::vector<ThreadState> Snapshot(uint64_t now_ns);
std::string              Format(const std::vector<ThreadState>& states, uint64_t min_wait_ns);

// Parses KYTY_GUEST_WAIT_DUMP_SECONDS. Returns 0 (disabled) for a missing or invalid value.
uint32_t ParseDumpSeconds(const char* value);

// Reads KYTY_GUEST_WAIT_DUMP_SECONDS and, when it is set, enables tracking and starts a background
// thread that logs a dump at that interval.
void InitFromEnvironment();

} // namespace Common::GuestWaits

// Records the enclosing HLE call as a guest wait, with the guest return address as the caller.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_GUEST_WAIT_SCOPE(kind, object, object_name)                                           \
	const ::Common::GuestWaits::Scope kyty_guest_wait_scope(                                       \
	    (kind), (object), (object_name), reinterpret_cast<uint64_t>(__builtin_return_address(0)))

#endif /* KYTY_COMMON_GUEST_WAITS_H_ */
