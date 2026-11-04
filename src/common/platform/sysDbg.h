#ifndef KYTY_COMMON_PLATFORM_SYSDBG_H_
#define KYTY_COMMON_PLATFORM_SYSDBG_H_

#include "common/common.h"

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_dbg_stack_info_t {
	uintptr_t reserved_addr; // lowest address of the calling thread's stack reservation
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t guard_addr; // guard page just below the committed stack
	size_t    guard_size;
#endif
};

sys_dbg_stack_info_t SysStackUsage();

#endif /* KYTY_COMMON_PLATFORM_SYSDBG_H_ */
