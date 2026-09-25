#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

#include "common/assert.h"
#include "common/virtualMemory.h"

// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <memoryapi.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>
// IWYU pragma: no_include <winerror.h>
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

void Init() {}

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode) {
	auto ptr = (address == 0 ? AllocAligned(address, size, mode, 1)
	                         : reinterpret_cast<uintptr_t>(VirtualAlloc(
	                               reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                               static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                               GetProtectionFlag(mode))));
	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());

		if (err != ERROR_INVALID_ADDRESS) {
			printf("VirtualAlloc() failed: 0x%08" PRIx32 "\n", err);
		} else {
			return AllocAligned(address, size, mode, 1);
		}
	}
	return ptr;
}

using VirtualAlloc2_func_t = /*WINBASEAPI*/ PVOID WINAPI (*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                                             MEM_EXTENDED_PARAMETER*, ULONG);

static VirtualAlloc2_func_t ResolveVirtualAlloc2() {
	HMODULE h = GetModuleHandle("KernelBase"); // @suppress("Invalid arguments")
	if (h != nullptr) {
		return reinterpret_cast<VirtualAlloc2_func_t>(GetProcAddress(h, "VirtualAlloc2"));
	}
	return nullptr;
}

static uint64_t AlignUp(uint64_t addr, uint64_t alignment) {
	return (addr + alignment - 1) & ~(alignment - 1);
}

uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment) {
	if (alignment == 0) {
		printf("VirtualAlloc2 failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return 0;
	}

	static constexpr uint64_t SYSTEM_MANAGED_MIN = 0x0000040000u;
	static constexpr uint64_t SYSTEM_MANAGED_MAX = 0x07FFFFBFFFu;
	static constexpr uint64_t USER_MIN           = 0x1000000000u;
	static constexpr uint64_t USER_MAX           = 0xFBFFFFFFFFu;

	MEM_ADDRESS_REQUIREMENTS req {};
	MEM_EXTENDED_PARAMETER   param {};
	req.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req.HighestEndingAddress = (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MAX)
	                                         : reinterpret_cast<PVOID>(USER_MAX));
	req.Alignment            = alignment;
	param.Type               = MemExtendedParameterAddressRequirements;
	param.Pointer            = &req;

	MEM_ADDRESS_REQUIREMENTS req2 {};
	MEM_EXTENDED_PARAMETER   param2 {};
	req2.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(USER_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req2.HighestEndingAddress = reinterpret_cast<PVOID>(USER_MAX);
	req2.Alignment            = alignment;
	param2.Type               = MemExtendedParameterAddressRequirements;
	param2.Pointer            = &req2;

	static auto virtual_alloc2 = ResolveVirtualAlloc2();

	EXIT_NOT_IMPLEMENTED(virtual_alloc2 == nullptr);

	auto ptr = reinterpret_cast<uintptr_t>(
	    virtual_alloc2(GetCurrentProcess(), nullptr, size,
	                   static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                   GetProtectionFlag(mode), &param, 1));

	if (ptr == 0) {
		ptr = reinterpret_cast<uintptr_t>(
		    virtual_alloc2(GetCurrentProcess(), nullptr, size,
		                   static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
		                   GetProtectionFlag(mode), &param2, 1));
	}

	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());
		if (err != ERROR_INVALID_PARAMETER) {
			printf("VirtualAlloc2(alignment = 0x%016" PRIx64 ") failed: 0x%08" PRIx32 "\n",
			       alignment, err);
		} else {
			return AllocAligned(address, size, mode, alignment << 1u);
		}
	}
	return ptr;
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

bool Commit(uint64_t address, uint64_t size, Mode mode) {
	auto ptr = reinterpret_cast<uintptr_t>(
	    VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                 static_cast<DWORD>(MEM_COMMIT), GetProtectionFlag(mode)));
	if (ptr == 0) {
		printf("VirtualAlloc(MEM_COMMIT) failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return false;
	}

	if (ptr != address) {
		printf("VirtualAlloc(MEM_COMMIT) failed: wrong address\n");
		return false;
	}

	return true;
}

uint64_t Reserve(uint64_t address, uint64_t size) {
	auto ptr = (address == 0 ? ReserveAligned(address, size, 1)
	                         : reinterpret_cast<uintptr_t>(VirtualAlloc(
	                               reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                               static_cast<DWORD>(MEM_RESERVE), PAGE_NOACCESS)));
	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());

		if (err != ERROR_INVALID_ADDRESS) {
			printf("VirtualAlloc(MEM_RESERVE) failed: 0x%08" PRIx32 "\n", err);
		} else {
			return ReserveAligned(address, size, 1);
		}
	}
	return ptr;
}

uint64_t ReserveAligned(uint64_t address, uint64_t size, uint64_t alignment) {
	if (alignment == 0) {
		printf("VirtualAlloc2(MEM_RESERVE) failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return 0;
	}

	static constexpr uint64_t SYSTEM_MANAGED_MIN = 0x0000040000u;
	static constexpr uint64_t SYSTEM_MANAGED_MAX = 0x07FFFFBFFFu;
	static constexpr uint64_t USER_MIN           = 0x1000000000u;
	static constexpr uint64_t USER_MAX           = 0xFBFFFFFFFFu;

	MEM_ADDRESS_REQUIREMENTS req {};
	MEM_EXTENDED_PARAMETER   param {};
	req.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req.HighestEndingAddress = (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MAX)
	                                         : reinterpret_cast<PVOID>(USER_MAX));
	req.Alignment            = alignment;
	param.Type               = MemExtendedParameterAddressRequirements;
	param.Pointer            = &req;

	MEM_ADDRESS_REQUIREMENTS req2 {};
	MEM_EXTENDED_PARAMETER   param2 {};
	req2.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(USER_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req2.HighestEndingAddress = reinterpret_cast<PVOID>(USER_MAX);
	req2.Alignment            = alignment;
	param2.Type               = MemExtendedParameterAddressRequirements;
	param2.Pointer            = &req2;

	static auto virtual_alloc2 = ResolveVirtualAlloc2();

	EXIT_NOT_IMPLEMENTED(virtual_alloc2 == nullptr);

	auto ptr = reinterpret_cast<uintptr_t>(virtual_alloc2(GetCurrentProcess(), nullptr, size,
	                                                      static_cast<DWORD>(MEM_RESERVE),
	                                                      PAGE_NOACCESS, &param, 1));

	if (ptr == 0) {
		ptr = reinterpret_cast<uintptr_t>(virtual_alloc2(GetCurrentProcess(), nullptr, size,
		                                                 static_cast<DWORD>(MEM_RESERVE),
		                                                 PAGE_NOACCESS, &param2, 1));
	}

	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());
		if (err != ERROR_INVALID_PARAMETER) {
			printf("VirtualAlloc2(MEM_RESERVE, alignment = 0x%016" PRIx64 ") failed: 0x%08" PRIx32
			       "\n",
			       alignment, err);
		} else {
			return ReserveAligned(address, size, alignment << 1u);
		}
	}
	return ptr;
}

bool ReserveFixed(uint64_t address, uint64_t size) {
	auto ptr = reinterpret_cast<uintptr_t>(
	    VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                 static_cast<DWORD>(MEM_RESERVE), PAGE_NOACCESS));
	if (ptr == 0) {
		printf("VirtualAlloc(MEM_RESERVE) failed: address=0x%016" PRIx64 ", size=0x%016" PRIx64
		       ", err=0x%08" PRIx32 "\n",
		       address, size, static_cast<uint32_t>(GetLastError()));
		return false;
	}

	if (ptr != address) {
		printf("VirtualAlloc(MEM_RESERVE) failed: wrong address\n");
		VirtualFree(reinterpret_cast<LPVOID>(ptr), 0, MEM_RELEASE);
		return false;
	}

	return true;
}

bool Decommit(uint64_t address, uint64_t size) {
	if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                MEM_DECOMMIT) == 0) {
		printf("VirtualFree(MEM_DECOMMIT) failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool Free(uint64_t address) {
	if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE) ==
	    0) {
		printf("VirtualFree() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool FreeRange(uint64_t address, uint64_t size) {
	if (address == 0 || size == 0) {
		return false;
	}

	uint64_t current = address;
	while (current - address < size) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) == 0 ||
		    reinterpret_cast<uint64_t>(info.AllocationBase) != address || info.State == MEM_FREE) {
			return false;
		}
		const auto next = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		if (next <= current || next - address > size) {
			return false;
		}
		current = next;
	}
	return current - address == size && Free(address);
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
