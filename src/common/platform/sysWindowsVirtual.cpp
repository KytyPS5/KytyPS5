#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#include <windows.h> // IWYU pragma: keep

#include "common/virtualMemory.h"

// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <memoryapi.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::VirtualMemory {

static DWORD GetProtectionFlag(Mode mode) {
	DWORD protect = PAGE_NOACCESS;
	switch (mode) {
		case Mode::Read: protect = PAGE_READONLY; break;

		case Mode::Write:
		case Mode::ReadWrite: protect = PAGE_READWRITE; break;

		case Mode::Execute: protect = PAGE_EXECUTE; break;

		case Mode::ExecuteRead: protect = PAGE_EXECUTE_READ; break;

		case Mode::ExecuteWrite:
		case Mode::ExecuteReadWrite: protect = PAGE_EXECUTE_READWRITE; break;

		case Mode::NoAccess:
		default: protect = PAGE_NOACCESS; break;
	}
	return protect;
}

bool AllocFixed(uint64_t address, uint64_t size, Mode mode) {
	auto ptr = reinterpret_cast<uintptr_t>(VirtualAlloc(
	    reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	    static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE), GetProtectionFlag(mode)));
	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());

		printf("VirtualAlloc() failed: 0x%08" PRIx32 "\n", err);
		return false;
	}

	if (ptr != address) {
		printf("VirtualAlloc() failed: wrong address\n");
		VirtualFree(reinterpret_cast<LPVOID>(ptr), 0, MEM_RELEASE);
		return false;
	}

	return true;
}

bool Free(uint64_t address, uint64_t /*size*/) {
	if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE) ==
	    0) {
		printf("VirtualFree() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool Protect(uint64_t address, uint64_t size, Mode mode) {
	DWORD old_protect = 0;
	if (VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                   GetProtectionFlag(mode), &old_protect) == 0) {
		printf("VirtualProtect() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool FlushInstructionCache(uint64_t address, uint64_t size) {
	if (::FlushInstructionCache(GetCurrentProcess(),
	                            reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)),
	                            size) == 0) {
		printf("FlushInstructionCache() failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

} // namespace Common::VirtualMemory

#endif
