#ifndef KYTY_COMMON_CRASH_HANDLER_H_
#define KYTY_COMMON_CRASH_HANDLER_H_

#include "common/common.h"

namespace Common::CrashHandler {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr char const* name = "CrashHandler";
	static void                  initialize() { Initialize(); }
	static void                  shutdown() { Shutdown(); }
	static void                  emergency_shutdown() {}
};

} // namespace Common::CrashHandler

#endif /* KYTY_COMMON_CRASH_HANDLER_H_ */
