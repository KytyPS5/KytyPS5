#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
// #error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "common/assert.h"
#include "common/platform/sysVirtual.h"
#include "common/virtualMemory.h"

#include <atomic>
#include <map>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

// IWYU pragma: no_include <asm/mman-common.h>
// IWYU pragma: no_include <asm/mman.h>
// IWYU pragma: no_include <bits/pthread_types.h>
// IWYU pragma: no_include <linux/mman.h>

#if defined(MAP_FIXED_NOREPLACE) && KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#define KYTY_FIXED_NOREPLACE
#endif

namespace Common {

static pthread_mutex_t              g_virtual_mutex {};
static std::map<uintptr_t, size_t>* g_allocs   = nullptr;
static std::map<uintptr_t, int>*    g_protects = nullptr;

void SysVirtualInit() {
	pthread_mutexattr_t attr {};

	pthread_mutexattr_init(&attr);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_FAST_NP); // glibc-only fast mutex
#else
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
#endif
	pthread_mutex_init(&g_virtual_mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	g_allocs   = new std::map<uintptr_t, size_t>;
	g_protects = new std::map<uintptr_t, int>;
}

static int get_protection_flag(VirtualMemory::Mode mode) {
	int protect = PROT_NONE;
	switch (mode) {
		case VirtualMemory::Mode::Read: protect = PROT_READ; break;
		case VirtualMemory::Mode::Write:
		case VirtualMemory::Mode::ReadWrite: protect = PROT_READ | PROT_WRITE; break; // NOLINT
		case VirtualMemory::Mode::Execute: protect = PROT_EXEC; break;
		case VirtualMemory::Mode::ExecuteRead: protect = PROT_EXEC | PROT_READ; break; // NOLINT
		case VirtualMemory::Mode::ExecuteWrite:
		case VirtualMemory::Mode::ExecuteReadWrite:
			protect = PROT_EXEC | PROT_WRITE | PROT_READ;
			break; // NOLINT
		case VirtualMemory::Mode::NoAccess:
		default: protect = PROT_NONE; break;
	}
	return protect;
}

static VirtualMemory::Mode get_protection_flag(int mode) {
	switch (mode) {
		case PROT_NONE: return VirtualMemory::Mode::NoAccess;
		case PROT_READ: return VirtualMemory::Mode::Read;
		case PROT_WRITE: return VirtualMemory::Mode::Write;
		case PROT_READ | PROT_WRITE: return VirtualMemory::Mode::ReadWrite; // NOLINT
		case PROT_EXEC: return VirtualMemory::Mode::Execute;
		case PROT_EXEC | PROT_WRITE: return VirtualMemory::Mode::ExecuteWrite; // NOLINT
		case PROT_EXEC | PROT_READ: return VirtualMemory::Mode::ExecuteRead;   // NOLINT
		case PROT_EXEC | PROT_WRITE | PROT_READ:
			return VirtualMemory::Mode::ExecuteReadWrite; // NOLINT
		default: return VirtualMemory::Mode::NoAccess;
	}
}

// Keep automatic mappings inside the guest and GPU-addressable low window.
#ifdef KYTY_FIXED_NOREPLACE
static constexpr uintptr_t LOW_ARENA_LIMIT = 0x000000FC00000000ULL; // libc mspace window ceiling
static constexpr uintptr_t LOW_ARENA_FLOOR = 0x000000A000000000ULL; // 640 GiB
static constexpr uintptr_t LOW_ARENA_GRAIN = 0x0000000000010000ULL; // 64 KiB

static_assert(LOW_ARENA_LIMIT <= 0x0000010000000000ULL,
              "arena must stay inside the GPU page tracker's 1<<40 window");
static_assert(LOW_ARENA_FLOOR < LOW_ARENA_LIMIT, "arena floor must sit below its ceiling");

static std::atomic<uintptr_t> g_low_arena_next {LOW_ARENA_LIMIT};
#endif

// Caller holds g_virtual_mutex.
static void record_alloc(uintptr_t addr, size_t size) {
	auto next = g_allocs->upper_bound(addr);
	if (next != g_allocs->begin()) {
		auto       it         = std::prev(next);
		const auto alloc_addr = it->first;
		const auto alloc_end  = alloc_addr + it->second;
		if (alloc_addr <= addr && addr + size <= alloc_end) {
			g_allocs->erase(it);
			if (alloc_addr < addr) {
				(*g_allocs)[alloc_addr] = addr - alloc_addr;
			}
			if (addr + size < alloc_end) {
				(*g_allocs)[addr + size] = alloc_end - (addr + size);
			}
		}
	}
	(*g_allocs)[addr] = size;
}

#ifdef KYTY_FIXED_NOREPLACE
static uintptr_t align_up_to(uintptr_t addr, uint64_t alignment) {
	return (addr + alignment - 1) & ~(alignment - 1);
}
#endif

// Freed arena addresses are not reused while GPU caches remain keyed by address.
static void* map_anonymous(uintptr_t addr, size_t size, int protect, int flags) {
	if (addr != 0) {
		return mmap(reinterpret_cast<void*>(addr), size, protect, flags, -1, 0); // NOLINT
	}

#ifdef KYTY_FIXED_NOREPLACE
	const auto step = align_up_to(size, LOW_ARENA_GRAIN);
	for (int attempt = 0; attempt < 256; attempt++) {
		const auto top = g_low_arena_next.fetch_sub(step, std::memory_order_relaxed);
		if (top < step || top - step < LOW_ARENA_FLOOR) {
			break;
		}
		const auto hint = (top - step) & ~(LOW_ARENA_GRAIN - 1);
		void*      ptr  = mmap(reinterpret_cast<void*>(hint), size, protect,
		                       flags | MAP_FIXED_NOREPLACE, -1, 0); // NOLINT
		if (ptr != MAP_FAILED) {
			return ptr;
		}
	}
#endif

	return mmap(nullptr, size, protect, flags, -1, 0); // NOLINT
}

uint64_t SysVirtualAlloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode) {
	EXIT_IF(g_allocs == nullptr);

	auto addr = static_cast<uintptr_t>(address);

	int protect = get_protection_flag(mode);

	void* ptr = map_anonymous(addr, size, protect, MAP_PRIVATE | MAP_ANON);

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED) {
		pthread_mutex_lock(&g_virtual_mutex);
		record_alloc(ret_addr, size);
		uintptr_t page_start  = ret_addr >> 12u;
		uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
		for (uintptr_t page = page_start; page <= page_end; page++) {
			(*g_protects)[page] = protect;
		}
		pthread_mutex_unlock(&g_virtual_mutex);
	}

	return ret_addr;
}

static uintptr_t align_up(uintptr_t addr, uint64_t alignment) {
	return (addr + alignment - 1) & ~(alignment - 1);
}

uint64_t SysVirtualAllocAligned(uint64_t address, uint64_t size, VirtualMemory::Mode mode,
                                uint64_t alignment) {
	if (alignment == 0) {
		return 0;
	}

	EXIT_IF(g_allocs == nullptr);

	auto addr    = static_cast<uintptr_t>(address);
	int  protect = get_protection_flag(mode);

	void* ptr = map_anonymous(addr, size, protect, MAP_PRIVATE | MAP_ANON);

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
		munmap(ptr, size);

		ptr      = map_anonymous(addr, size + alignment, protect,
		                         MAP_PRIVATE | MAP_ANON | MAP_NORESERVE);
		ret_addr = reinterpret_cast<uintptr_t>(ptr);
		if (ptr != MAP_FAILED) {
#if defined(__APPLE__)
			// Carve the aligned subrange out of the live mapping with MAP_FIXED (in-place
			// replacement) and trim the slack; never munmap the whole range first, or a
			// concurrent host mapping (dyld, Rosetta, Metal) could claim the hole and be
			// destroyed by the MAP_FIXED. Other platforms keep the original path below.
			auto aligned_addr = align_up(ret_addr, alignment);
			// NOLINTNEXTLINE
			void* fixed = mmap(reinterpret_cast<void*>(aligned_addr), size, protect,
			                   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
			if (fixed == MAP_FAILED) {
				munmap(ptr, size + alignment);
				ret_addr = 0;
				ptr      = MAP_FAILED;
			} else {
				if (aligned_addr > ret_addr) {
					munmap(reinterpret_cast<void*>(ret_addr), aligned_addr - ret_addr);
				}
				const uintptr_t tail_start = aligned_addr + size;
				const uintptr_t resv_end   = ret_addr + size + alignment;
				if (resv_end > tail_start) {
					munmap(reinterpret_cast<void*>(tail_start), resv_end - tail_start);
				}
				ptr      = fixed;
				ret_addr = aligned_addr;
			}
#else
			munmap(ptr, size + alignment);
			auto aligned_addr = align_up(ret_addr, alignment);
#ifdef KYTY_FIXED_NOREPLACE
			// NOLINTNEXTLINE
			ptr = mmap(reinterpret_cast<void*>(aligned_addr), size, protect,
			           MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);
#else
			// NOLINTNEXTLINE
			ptr = mmap(reinterpret_cast<void*>(aligned_addr), size, protect,
			           MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
			ret_addr = reinterpret_cast<uintptr_t>(ptr);
			if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
				munmap(ptr, size);
				ret_addr = 0;
				ptr      = MAP_FAILED;
			}
#endif
		}
	}

	if (ptr == MAP_FAILED) {
		return SysVirtualAllocAligned(address, size, mode, alignment << 1u);
	}

	pthread_mutex_lock(&g_virtual_mutex);
	record_alloc(ret_addr, size);
	uintptr_t page_start  = ret_addr >> 12u;
	uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
	for (uintptr_t page = page_start; page <= page_end; page++) {
		(*g_protects)[page] = protect;
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	return ret_addr;
}

#if defined(__APPLE__)
// macOS has no /proc/self/maps; query the Mach VM map directly. mach_vm_region returns
// the first mapped region at or above `region_addr`; if it begins before the end of the
// requested range, the range overlaps an existing mapping.
static bool is_mapped(void* ptr, size_t length) {
	auto              query_addr  = reinterpret_cast<mach_vm_address_t>(ptr);
	mach_vm_address_t region_addr = query_addr;
	mach_vm_size_t    region_size = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t                    object_name = MACH_PORT_NULL;

	kern_return_t kr =
	    mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
	                   reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);
	if (kr != KERN_SUCCESS) {
		return false; // no region at or above the address → unmapped
	}
	return region_addr < (query_addr + length);
}
#else
static bool is_mapped(void* ptr, size_t length) {
	FILE* file = fopen("/proc/self/maps", "r");
	char  line[1024];
	bool  ret  = false;
	auto  addr = reinterpret_cast<uintptr_t>(ptr);

	while (feof(file) == 0) {
		if (fgets(line, 1024, file) == nullptr) {
			break;
		}
		uint64_t start = 0;
		uint64_t end   = 0;
		// NOLINTNEXTLINE(cert-err34-c)
		if (sscanf(line, "%" SCNx64 "-%" SCNx64, &start, &end) != 2) {
			continue;
		}
		if (addr >= start && addr + length <= end) {
			ret = true;
			break;
		}
	}
	fclose(file);
	return ret;
}
#endif

bool SysVirtualAllocFixed(uint64_t address, uint64_t size, VirtualMemory::Mode mode) {
	EXIT_IF(g_allocs == nullptr);

	auto addr    = static_cast<uintptr_t>(address);
	int  protect = get_protection_flag(mode);

#ifdef KYTY_FIXED_NOREPLACE
	// NOLINTNEXTLINE
	void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect,
	                 MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);
#else
	// NOLINTNEXTLINE
	void* ptr = (is_mapped(reinterpret_cast<void*>(addr), size)
	                 ? MAP_FAILED
	                 : mmap(reinterpret_cast<void*>(addr), size, protect,
	                        MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0));
#endif

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED && ret_addr != addr) {
		munmap(ptr, size);
		ret_addr = 0;
		ptr      = MAP_FAILED;
	}

	if (ptr != MAP_FAILED) {
		pthread_mutex_lock(&g_virtual_mutex);
		record_alloc(ret_addr, size);
		uintptr_t page_start  = ret_addr >> 12u;
		uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
		for (uintptr_t page = page_start; page <= page_end; page++) {
			(*g_protects)[page] = protect;
		}
		pthread_mutex_unlock(&g_virtual_mutex);

		return true;
	}

	return false;
}

bool SysVirtualCommit(uint64_t address, uint64_t size, VirtualMemory::Mode mode) {
	return SysVirtualProtect(address, size, mode);
}

uint64_t SysVirtualReserve(uint64_t address, uint64_t size) {
	return SysVirtualReserveAligned(address, size, 1);
}

uint64_t SysVirtualReserveAligned(uint64_t address, uint64_t size, uint64_t alignment) {
	if (alignment == 0) {
		return 0;
	}

	EXIT_IF(g_allocs == nullptr);

	auto addr = static_cast<uintptr_t>(address);

	void* ptr = map_anonymous(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE);

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
		munmap(ptr, size);

		ptr      = map_anonymous(addr, size + alignment, PROT_NONE,
		                         MAP_PRIVATE | MAP_ANON | MAP_NORESERVE);
		ret_addr = reinterpret_cast<uintptr_t>(ptr);
		if (ptr != MAP_FAILED) {
#if defined(__APPLE__)
			// Carve the aligned subrange out of the live reservation with MAP_FIXED (an
			// in-place replacement), then trim the slack. The range must never be
			// returned to the OS in between: another thread (dyld, Rosetta, Metal,
			// malloc) could claim the hole, and the subsequent MAP_FIXED would silently
			// destroy its mapping. Other platforms keep the original path below.
			auto aligned_addr = align_up(ret_addr, alignment);
			// NOLINTNEXTLINE
			void* fixed = mmap(reinterpret_cast<void*>(aligned_addr), size, PROT_NONE,
			                   MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
			if (fixed == MAP_FAILED) {
				munmap(ptr, size + alignment);
				ret_addr = 0;
				ptr      = MAP_FAILED;
			} else {
				if (aligned_addr > ret_addr) {
					munmap(reinterpret_cast<void*>(ret_addr), aligned_addr - ret_addr);
				}
				const uintptr_t tail_start = aligned_addr + size;
				const uintptr_t resv_end   = ret_addr + size + alignment;
				if (resv_end > tail_start) {
					munmap(reinterpret_cast<void*>(tail_start), resv_end - tail_start);
				}
				ptr      = fixed;
				ret_addr = aligned_addr;
			}
#else
			munmap(ptr, size + alignment);
			auto aligned_addr = align_up(ret_addr, alignment);
#ifdef KYTY_FIXED_NOREPLACE
			// NOLINTNEXTLINE
			ptr = mmap(reinterpret_cast<void*>(aligned_addr), size, PROT_NONE,
			           MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
#else
			// NOLINTNEXTLINE
			ptr = mmap(reinterpret_cast<void*>(aligned_addr), size, PROT_NONE,
			           MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
#endif
			ret_addr = reinterpret_cast<uintptr_t>(ptr);
			if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
				munmap(ptr, size);
				ret_addr = 0;
				ptr      = MAP_FAILED;
			}
#endif
		}
	}

	if (ptr == MAP_FAILED) {
		return SysVirtualReserveAligned(address, size, alignment << 1u);
	}

	pthread_mutex_lock(&g_virtual_mutex);
	record_alloc(ret_addr, size);
	pthread_mutex_unlock(&g_virtual_mutex);

	return ret_addr;
}

bool SysVirtualReserveFixed(uint64_t address, uint64_t size) {
	EXIT_IF(g_allocs == nullptr);

	auto addr = static_cast<uintptr_t>(address);

#ifdef KYTY_FIXED_NOREPLACE
	// NOLINTNEXTLINE
	void* ptr = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
	                 MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
#else
	// NOLINTNEXTLINE
	void* ptr = (is_mapped(reinterpret_cast<void*>(addr), size)
	                 ? MAP_FAILED
	                 : mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
	                        MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0));
#endif

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED && ret_addr != addr) {
		munmap(ptr, size);
		ret_addr = 0;
		ptr      = MAP_FAILED;
	}

	if (ptr != MAP_FAILED) {
		pthread_mutex_lock(&g_virtual_mutex);
		record_alloc(ret_addr, size);
		pthread_mutex_unlock(&g_virtual_mutex);

		return true;
	}

	return false;
}

bool SysVirtualDecommit(uint64_t address, uint64_t size) {
	// Drop physical pages while preserving the reservation.
	if (!SysVirtualProtect(address, size, VirtualMemory::Mode::NoAccess)) {
		return false;
	}

	if (size != 0) {
#if defined(__APPLE__)
		constexpr int RECLAIM_ADVICE = MADV_FREE;
#else
		constexpr int RECLAIM_ADVICE = MADV_DONTNEED;
#endif
		const auto page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
		if (page_size != 0) {
			// Do not discard pages outside the requested range.
			const auto begin = (static_cast<uintptr_t>(address) + page_size - 1) & ~(page_size - 1);
			const auto end   = (static_cast<uintptr_t>(address) + size) & ~(page_size - 1);
			if (end > begin) {
				::madvise(reinterpret_cast<void*>(begin), end - begin, RECLAIM_ADVICE);
			}
		}
	}

	return true;
}

bool SysVirtualFree(uint64_t address) {
	EXIT_IF(g_allocs == nullptr);
	size_t size = 0;

	auto addr = static_cast<uintptr_t>(address & ~static_cast<uint64_t>(0xfffu));

	pthread_mutex_lock(&g_virtual_mutex);
	if (auto s = g_allocs->find(addr); s != g_allocs->end()) {
		size = s->second;
		g_allocs->erase(s);
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	if (size == 0) {
		return false;
	}

	if (munmap(reinterpret_cast<void*>(addr), size) == 0) {
		uintptr_t page_start = addr >> 12u;
		uintptr_t page_end   = (addr + size - 1) >> 12u;
		pthread_mutex_lock(&g_virtual_mutex);
		for (uintptr_t page = page_start; page <= page_end; page++) {
			g_protects->erase(page);
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
}

bool SysVirtualFreeRange(uint64_t address, uint64_t size) {
	EXIT_IF(g_allocs == nullptr);
	if (size == 0 || (address & 0xfffu) != 0 || (size & 0xfffu) != 0) {
		return false;
	}

	const auto addr = static_cast<uintptr_t>(address);
	const auto end  = addr + size;
	if (end < addr) {
		return false;
	}

	pthread_mutex_lock(&g_virtual_mutex);
	auto next = g_allocs->upper_bound(addr);
	if (next == g_allocs->begin()) {
		pthread_mutex_unlock(&g_virtual_mutex);
		return false;
	}

	// A reservation may have been split into several adjacent records.
	auto       first      = std::prev(next);
	const auto alloc_addr = first->first;
	if (addr < alloc_addr || alloc_addr + first->second <= addr) {
		pthread_mutex_unlock(&g_virtual_mutex);
		return false;
	}

	auto      last   = first;
	uintptr_t cursor = alloc_addr + first->second;
	while (cursor < end) {
		auto following = std::next(last);
		if (following == g_allocs->end() || following->first != cursor) {
			pthread_mutex_unlock(&g_virtual_mutex);
			return false;
		}
		last   = following;
		cursor = following->first + following->second;
	}
	const auto alloc_end = cursor;

	if (munmap(reinterpret_cast<void*>(addr), size) != 0) {
		pthread_mutex_unlock(&g_virtual_mutex);
		return false;
	}

	g_allocs->erase(first, std::next(last));
	if (alloc_addr < addr) {
		(*g_allocs)[alloc_addr] = addr - alloc_addr;
	}
	if (end < alloc_end) {
		(*g_allocs)[end] = alloc_end - end;
	}
	for (uintptr_t page = addr >> 12u; page <= (end - 1u) >> 12u; page++) {
		g_protects->erase(page);
	}
	pthread_mutex_unlock(&g_virtual_mutex);
	return true;
}

bool SysVirtualProtect(uint64_t address, uint64_t size, VirtualMemory::Mode mode,
                       VirtualMemory::Mode* old_mode) {
	auto addr = static_cast<uintptr_t>(address);

	pthread_mutex_lock(&g_virtual_mutex);
	if (old_mode != nullptr) {
		if (auto s = g_protects->find(addr >> 12u); s != g_protects->end()) {
			*old_mode = get_protection_flag(s->second);
		} else {
			*old_mode = VirtualMemory::Mode::NoAccess;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	uintptr_t page_start = addr >> 12u;
	uintptr_t page_end   = (addr + size - 1) >> 12u;
	if (mprotect(reinterpret_cast<void*>(page_start << 12u), (page_end - page_start + 1) << 12u,
	             get_protection_flag(mode)) == 0) {
		pthread_mutex_lock(&g_virtual_mutex);
		for (uintptr_t page = page_start; page <= page_end; page++) {
			(*g_protects)[page] = get_protection_flag(mode);
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
}

bool SysVirtualFlushInstructionCache(uint64_t /*address*/, uint64_t /*size*/) {
	return true;
}

} // namespace Common

#endif
