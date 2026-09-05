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

#include <atomic>

namespace {

std::atomic<SysCrashHandler> g_crash_handler {nullptr};

static_assert(decltype(g_crash_handler)::is_always_lock_free);

const char* ExceptionDescription(DWORD code) {
	switch (code) {
		case EXCEPTION_ACCESS_VIOLATION: return "access violation";
		case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
		case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
		case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
		case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
		case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
		case 0xe06d7363u: return "unhandled C++ exception";
		default: return "unhandled exception";
	}
}

LONG WINAPI CrashFilter(PEXCEPTION_POINTERS pointers) {
	// The reporter itself may fault. Report the first one and let any later fault pass straight
	// through, so a broken reporter cannot turn a crash into a hang.
	static LONG reporting = 0;
	if (InterlockedExchange(&reporting, 1) != 0) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	const auto* record  = pointers->ExceptionRecord;
	const auto  handler = g_crash_handler.load(std::memory_order_acquire);

	if (handler != nullptr && record != nullptr) {
		sys_dbg_crash_info_t info {};
		info.native_code = record->ExceptionCode;
		info.fault_addr  = reinterpret_cast<uint64_t>(record->ExceptionAddress);
		info.description = ExceptionDescription(record->ExceptionCode);

		if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
		     record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
		    record->NumberParameters >= 2) {
			info.access_addr = record->ExceptionInformation[1];
			switch (record->ExceptionInformation[0]) {
				case 0: info.access = "read"; break;
				case 1: info.access = "write"; break;
				case 8: info.access = "execute"; break;
				default: break;
			}
		}

		handler(info);
	}

	// Hand the fault back to the default handling so Windows Error Reporting and any configured
	// dump collection behave exactly as they did before.
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool SysInstallCrashHandler(SysCrashHandler handler) {
	if (handler == nullptr) {
		return false;
	}
	g_crash_handler.store(handler, std::memory_order_release);
	SetUnhandledExceptionFilter(CrashFilter);
	return true;
}

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
