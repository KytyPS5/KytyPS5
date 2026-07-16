#include "graphics/host_gpu/hostMemory.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace Libs::Graphics {
namespace {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool IsAccessible(DWORD protect, HostMemoryAccess access) {
	constexpr DWORD blocked  = PAGE_NOACCESS | PAGE_GUARD;
	constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
	                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	constexpr DWORD writable =
	    PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return (protect & blocked) == 0 &&
	       (protect & (access == HostMemoryAccess::Write ? writable : readable)) != 0;
}
#endif

} // namespace

bool HostMemoryQueryRange(uint64_t addr, uint64_t requested_size, HostMemoryAccess access,
                          uint64_t* accessible_size) {
	if (accessible_size == nullptr) {
		return false;
	}
	*accessible_size = 0;
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
		const bool allowed =
		    access == HostMemoryAccess::Write ? permissions[1] == 'w' : permissions[0] == 'r';
		if (!allowed) {
			break;
		}
		current = finish < end ? finish : end;
	}
	std::fclose(maps);
#else
	(void)access;
#endif

	*accessible_size = current - addr;
	return *accessible_size != 0;
}

bool HostMemoryQueryEnd(uint64_t addr, HostMemoryAccess access, uint64_t* end) {
	if (addr == 0 || end == nullptr) {
		return false;
	}
	uint64_t accessible_size = 0;
	if (!HostMemoryQueryRange(addr, UINT64_MAX - addr, access, &accessible_size) ||
	    accessible_size == 0 || UINT64_MAX - addr < accessible_size) {
		return false;
	}
	*end = addr + accessible_size;
	return true;
}

bool HostMemoryQueryReadable(uint64_t addr, uint64_t requested_size, uint64_t* readable_size) {
	return HostMemoryQueryRange(addr, requested_size, HostMemoryAccess::Read, readable_size);
}

bool HostMemoryQueryWritable(uint64_t addr, uint64_t requested_size, uint64_t* writable_size) {
	return HostMemoryQueryRange(addr, requested_size, HostMemoryAccess::Write, writable_size);
}

bool HostMemoryReadDword(void*, uint64_t addr, uint32_t* value) {
	if (value == nullptr) {
		return false;
	}
	uint64_t readable = 0;
	if (!HostMemoryQueryReadable(addr, sizeof(*value), &readable) || readable < sizeof(*value)) {
		return false;
	}
	std::memcpy(value, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), sizeof(*value));
	return true;
}

bool HostMemoryIsReadable(uint64_t addr) {
	return HostMemoryRangeIsReadable(addr, 1);
}

bool HostMemoryRangeIsReadable(uint64_t addr, uint64_t size) {
	if (addr == 0 || size == 0 || UINT64_MAX - addr < size) {
		return false;
	}
	uint64_t readable_size = 0;
	return HostMemoryQueryReadable(addr, size, &readable_size) && readable_size >= size;
}

} // namespace Libs::Graphics
