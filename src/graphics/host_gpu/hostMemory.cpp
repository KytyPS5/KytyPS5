#include "graphics/host_gpu/hostMemory.h"

#include <cinttypes>
#include <cstdio>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {
namespace {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool IsAccessible(DWORD protect, HostMemoryAccess access) {
	constexpr DWORD blocked  = PAGE_NOACCESS | PAGE_GUARD;
	constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
	                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if (access == HostMemoryAccess::Mapped) {
		return (protect & PAGE_GUARD) == 0;
	}
	return (protect & blocked) == 0 && (protect & readable) != 0;
}
#endif

} // namespace

bool HostMemoryReadU32(uint64_t address, uint32_t& value) {
	uint32_t word = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SIZE_T copied = 0;
	if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
	                       &word, sizeof(word), &copied) || copied != sizeof(word)) {
		return false;
	}
#elif defined(__APPLE__)
	mach_vm_size_t copied = 0;
	if (mach_vm_read_overwrite(mach_task_self(), address, sizeof(word),
	                           reinterpret_cast<mach_vm_address_t>(&word), &copied) !=
	        KERN_SUCCESS || copied != sizeof(word)) {
		return false;
	}
#else
	const iovec local {&word, sizeof(word)};
	const iovec remote {reinterpret_cast<void*>(address), sizeof(word)};
	if (syscall(SYS_process_vm_readv, getpid(), &local, 1ul, &remote, 1ul, 0ul) !=
	    static_cast<long>(sizeof(word))) {
		return false;
	}
#endif
	value = word;
	return true;
}

bool HostMemoryQueryRange(uint64_t addr, uint64_t requested_size, HostMemoryAccess access,
                          uint64_t& accessible_size) {
	accessible_size = 0;
	if (addr == 0 || requested_size == 0) {
		return false;
	}

	const auto end     = UINT64_MAX - addr < requested_size ? UINT64_MAX : addr + requested_size;
	uint64_t   current = addr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	while (current < end) {
		MEMORY_BASIC_INFORMATION region {};
		if (::VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(current)), &region,
		                   sizeof(region)) == 0) {
			break;
		}
		const auto begin  = reinterpret_cast<uint64_t>(region.BaseAddress);
		auto       finish = begin + region.RegionSize;
		if (finish < begin) {
			finish = UINT64_MAX;
		}
		if (finish <= current || (region.State & MEM_COMMIT) == 0 ||
		    !IsAccessible(region.Protect, access)) {
			break;
		}
		current = finish < end ? finish : end;
	}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	auto* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) {
		return false;
	}
	char line[1024] = {};
	while (current < end && std::fgets(line, sizeof(line), maps) != nullptr) {
		uint64_t begin          = 0;
		uint64_t finish         = 0;
		char     permissions[5] = {};
		if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &begin, &finish, permissions) != 3 ||
		    finish <= current) {
			continue;
		}
		if (begin > current) {
			break;
		}
		const bool allowed = access == HostMemoryAccess::Mapped || permissions[0] == 'r';
		if (!allowed) {
			break;
		}
		current = finish < end ? finish : end;
	}
	std::fclose(maps);
#else
	(void)access;
#endif

	accessible_size = current - addr;
	return accessible_size != 0;
}

bool HostMemoryQueryReadable(uint64_t addr, uint64_t requested_size, uint64_t& readable_size) {
	return HostMemoryQueryRange(addr, requested_size, HostMemoryAccess::Read, readable_size);
}

bool HostMemoryIsReadable(uint64_t addr) {
	return HostMemoryRangeIsReadable(addr, 1);
}

bool HostMemoryRangeIsReadable(uint64_t addr, uint64_t size) {
	if (addr == 0 || size == 0 || UINT64_MAX - addr < size) {
		return false;
	}
	uint64_t readable_size = 0;
	return HostMemoryQueryReadable(addr, size, readable_size) && readable_size >= size;
}

} // namespace Libs::Graphics
