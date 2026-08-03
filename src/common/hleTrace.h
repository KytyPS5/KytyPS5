#ifndef KYTY_COMMON_HLETRACE_H_
#define KYTY_COMMON_HLETRACE_H_

#include "common/common.h"

// Diagnostic instrumentation for locating guest stalls.
//
// Compiled in only when the CMake option KYTY_ENABLE_HLE_TRACE is ON, which defines
// KYTY_HLE_TRACE. In a normal build every entry point below is either absent from the
// translation unit (the PRINT_NAME() breadcrumb) or an empty inline no-op, so neither the
// Windows nor the Linux release build changes.
//
// The instrument answers one question: when the guest appears frozen, what is every guest
// thread doing? A breadcrumb is recorded on entry to and exit from each HLE function that
// carries PRINT_NAME(); a watchdog thread periodically writes a per-thread snapshot showing
// whether a thread is parked inside a call (blocked) or churning through calls (spinning).

namespace Common::HleTrace {

#ifdef KYTY_HLE_TRACE
inline constexpr bool ENABLED = true;
#else
inline constexpr bool ENABLED = false;
#endif

#ifdef KYTY_HLE_TRACE

// Records that the calling thread has entered/left an HLE function. `module` and `func` must
// have static storage duration; only the pointers are kept.
void Enter(const char* module, const char* func);
void Leave();

// Appends a line to the trace's event ring buffer, dumped with each snapshot. Use for
// low-frequency, high-value events (a file being opened, a module being loaded).
void Note(const char* category, const char* text);

// Names the calling thread, or any thread by its Common::Thread unique id. A thread that has
// not yet executed an instrumented call has no slot yet, so naming it by id is best-effort.
void NameCurrentThread(const char* name);
void SetThreadName(int unique_id, const char* name);

// Starts the watchdog thread. Safe to call more than once; only the first call has an effect.
// Output goes to $KYTY_HLE_TRACE_FILE, defaulting to kyty_hle_trace.log in the working
// directory. The snapshot period defaults to 5000 ms and can be overridden with
// $KYTY_HLE_TRACE_PERIOD_MS.
void Start();

class ScopedCall {
public:
	ScopedCall(const char* module, const char* func) { Enter(module, func); }
	~ScopedCall() { Leave(); }

	KYTY_CLASS_NO_COPY(ScopedCall);
};

// Breadcrumb without the log line, for functions deliberately left out of PRINT_NAME() because
// they are too hot to log. Requires g_module to be in scope, same as PRINT_NAME().
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define TRACE_NAME() Common::HleTrace::ScopedCall trace_name_scope(g_module, __func__)

#else

inline void Enter(const char* /*module*/, const char* /*func*/) {}
inline void Leave() {}
inline void Note(const char* /*category*/, const char* /*text*/) {}
inline void NameCurrentThread(const char* /*name*/) {}
inline void SetThreadName(int /*unique_id*/, const char* /*name*/) {}
inline void Start() {}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define TRACE_NAME() ((void)0)

#endif

} // namespace Common::HleTrace

#endif /* KYTY_COMMON_HLETRACE_H_ */
