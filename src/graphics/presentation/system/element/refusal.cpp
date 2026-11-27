#include "graphics/presentation/system/element/refusal.h"

#include "common/logging/log.h"

#include <atomic>
#include <cassert>
#include <limits>

namespace Libs::Graphics::SystemUi {

namespace {

// One bit for each refusal reason already logged this run.
std::atomic<uint32_t> g_reported_refusals {0};

static_assert(static_cast<uint32_t>(ElementError::DuplicateKey) <
                  std::numeric_limits<uint32_t>::digits,
              "every reason needs a bit of its own");

// The innermost scope recording refusals on this thread, or null when none is open. Per thread, so
// a refusal on one thread never lands in another thread's scope.
thread_local RefusalScope* t_current_scope = nullptr;

// The name of `why` for the log.
const char* RefusalName(ElementError why) {
	switch (why) {
		case ElementError::None: return "None";
		case ElementError::Stale: return "Stale";
		case ElementError::OutOfRange: return "OutOfRange";
		case ElementError::AlreadyParented: return "AlreadyParented";
		case ElementError::NotBegun: return "NotBegun";
		case ElementError::DuplicateKey: return "DuplicateKey";
	}
	return "unknown";
}

} // namespace

bool ReportRefusal(ElementError why) {
	if (why == ElementError::None) {
		return false;
	}
	const uint32_t bit = 1u << static_cast<uint32_t>(why);
	if ((g_reported_refusals.fetch_or(bit) & bit) != 0) {
		return false;
	}
	LOGF("[SystemUi] an element was refused: %s. Later refusals for this reason are not logged.\n",
	     RefusalName(why));
	return true;
}

void RefuseBuild(ElementError why) {
	(void)ReportRefusal(why);
	RefusalScope* const scope = t_current_scope;
	if (scope != nullptr && !scope->m_first.has_value()) {
		scope->m_first = why;
	}
}

RefusalScope::RefusalScope(): m_previous(t_current_scope) {
	t_current_scope = this;
}

RefusalScope::~RefusalScope() {
	assert(t_current_scope == this && "RefusalScopes must end in reverse order");
	t_current_scope = m_previous;
}

} // namespace Libs::Graphics::SystemUi
