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

	// Full stack reservation reported by pthread.
	uintptr_t reserved_addr;
	size_t    reserved_size;
#endif
};

// A fault nothing else handled. The process is already dying when this is reported.
// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_dbg_crash_info_t {
	uint64_t    native_code; // Windows exception code, or the POSIX signal number
	uint64_t    fault_addr;  // the faulting instruction
	uint64_t    access_addr; // data address for a memory fault, 0 otherwise
	const char* description; // "access violation", "illegal instruction", ...
	const char* access;      // "read", "write", "execute", or nullptr
};

using SysCrashHandler = void (*)(const sys_dbg_crash_info_t& info);

// Installs a last-resort reporter for faults that reach nobody else. Returns false when the
// platform has no hook, which leaves the process behaving exactly as it did before.
bool SysInstallCrashHandler(SysCrashHandler handler);

void SysStackWalk(void** stack, int* depth);
void SysStackUsage(sys_dbg_stack_info_t& s);          // NOLINT(google-runtime-references)
void SysStackUsagePrint(sys_dbg_stack_info_t& stack); // NOLINT(google-runtime-references)

#endif /* KYTY_COMMON_PLATFORM_SYSDBG_H_ */
