#include "common/common.h"

// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <memoryapi.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#include <windows.h> // IWYU pragma: keep

#include "common/assert.h"
#include "common/platform/sysDbg.h"

void SysStackWalk(void** stack, int* depth) {
	*depth = static_cast<int>(CaptureStackBackTrace(0, static_cast<DWORD>(*depth), stack, nullptr));
}

void SysStackUsagePrint(sys_dbg_stack_info_t& stack) {
	printf("stack: (0x%" PRIx64 ", %" PRIu64 ") + (0x%" PRIx64 ", %" PRIu64 ") + (0x%" PRIx64
	       ", %" PRIu64 ")\n",
	       static_cast<uint64_t>(stack.reserved_addr), static_cast<uint64_t>(stack.reserved_size),
	       static_cast<uint64_t>(stack.guard_addr), static_cast<uint64_t>(stack.guard_size),
	       static_cast<uint64_t>(stack.commited_addr), static_cast<uint64_t>(stack.commited_size));
}

void SysStackUsage(sys_dbg_stack_info_t& s) {
	MEMORY_BASIC_INFORMATION mbi {};
	[[maybe_unused]] size_t  ss = VirtualQuery(&mbi, &mbi, sizeof(mbi));
	EXIT_IF(ss == 0);
	PVOID reserved = mbi.AllocationBase;
	ss             = VirtualQuery(reserved, &mbi, sizeof(mbi));
	EXIT_IF(ss == 0);
	size_t reserved_size = mbi.RegionSize;
	ss = VirtualQuery(static_cast<char*>(reserved) + reserved_size, &mbi, sizeof(mbi));
	EXIT_IF(ss == 0);
	void*  guard_page      = mbi.BaseAddress;
	size_t guard_page_size = mbi.RegionSize;
	ss = VirtualQuery(static_cast<char*>(guard_page) + guard_page_size, &mbi, sizeof(mbi));
	EXIT_IF(ss == 0);
	void*  commited      = mbi.BaseAddress;
	size_t commited_size = mbi.RegionSize;
	s.reserved_addr      = reinterpret_cast<uintptr_t>(reserved);
	s.reserved_size      = reserved_size;
	s.guard_addr         = reinterpret_cast<uintptr_t>(guard_page);
	s.guard_size         = guard_page_size;
	s.commited_addr      = reinterpret_cast<uintptr_t>(commited);
	s.commited_size      = commited_size;

	s.addr       = s.reserved_addr;
	s.total_size = s.reserved_size + s.guard_size + s.commited_size;
}

#endif
