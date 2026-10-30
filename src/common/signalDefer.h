#ifndef KYTY_COMMON_SIGNALDEFER_H_
#define KYTY_COMMON_SIGNALDEFER_H_

#include "common/common.h"

namespace Common {

// A host section that a guest signal handler must not interrupt. The handler runs guest code on
// the interrupted thread, and that code may need what the section holds: a game that suspends
// its threads with a signal (a garbage collector) waits in the handler for another thread, which
// then waits for the lock the suspended thread holds. A signal for the thread is delivered when
// its outermost section ends.
class SignalDeferScope final {
public:
	SignalDeferScope() noexcept;
	~SignalDeferScope();
	KYTY_CLASS_NO_COPY(SignalDeferScope);
};

// In the host signal handler: true when the thread is in a section; the signal stays pending and
// is delivered again (SetSignalRedelivery) when the section ends.
[[nodiscard]] bool DeferSignalInSection() noexcept;
// What delivers a deferred signal again on the current thread.
void SetSignalRedelivery(void (*redeliver)());

} // namespace Common

#endif /* KYTY_COMMON_SIGNALDEFER_H_ */
