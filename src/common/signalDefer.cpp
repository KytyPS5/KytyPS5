#include "common/signalDefer.h"

#include <atomic>

namespace Common {

namespace {

thread_local uint32_t   t_depth    = 0;
thread_local bool       t_deferred = false;
std::atomic<void (*)()> g_redeliver {nullptr};

} // namespace

SignalDeferScope::SignalDeferScope() noexcept {
	t_depth++;
}

SignalDeferScope::~SignalDeferScope() {
	if (--t_depth != 0 || !t_deferred) {
		return;
	}
	t_deferred = false;
	if (auto* redeliver = g_redeliver.load(std::memory_order_acquire)) {
		redeliver();
	}
}

bool DeferSignalInSection() noexcept {
	if (t_depth == 0) {
		return false;
	}
	t_deferred = true;
	return true;
}

void SetSignalRedelivery(void (*redeliver)()) {
	g_redeliver.store(redeliver, std::memory_order_release);
}

} // namespace Common
