#ifndef KYTY_COMMON_PLATFORM_SYSDBG_H_
#define KYTY_COMMON_PLATFORM_SYSDBG_H_

#include "common/common.h"

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_dbg_stack_info_t {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t addr;

	uintptr_t reserved_addr;
	size_t    reserved_size;
	uintptr_t guard_addr;
	size_t    guard_size;
	uintptr_t commited_addr;
	size_t    commited_size;

	size_t total_size;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	uintptr_t code_addr;
	uintptr_t addr;
	uintptr_t commited_addr;
	size_t    commited_size;
	size_t    total_size;
	size_t    code_size;
#endif
};

void SysStackWalk(void** stack, int* depth);
void SysStackUsage(sys_dbg_stack_info_t& s);          // NOLINT(google-runtime-references)
void SysStackUsagePrint(sys_dbg_stack_info_t& stack); // NOLINT(google-runtime-references)

#endif /* KYTY_COMMON_PLATFORM_SYSDBG_H_ */
