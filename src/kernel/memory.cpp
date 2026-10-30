#include "kernel/memory.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <magic_enum.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::LibKernel::Memory {

namespace VirtualMemory = Common::VirtualMemory;

LIB_NAME("libkernel", "libkernel");

constexpr int PROT_CPU_READ  = 0x01;
constexpr int PROT_CPU_WRITE = 0x02;
constexpr int PROT_CPU_EXEC  = 0x04;
constexpr int PROT_GPU_READ  = 0x10;
constexpr int PROT_GPU_WRITE = 0x20;

constexpr uint64_t PAGE_TABLE_GRANULARITY       = 2ull * 1024ull * 1024ull;
constexpr uint64_t DEFAULT_FLEXIBLE_MEMORY_SIZE = 448ull * 1024ull * 1024ull;
// A game gets 12.5 GiB. A fixed 64 MiB cache and mapping table memory past the 64 MiB included
// for each of the CPU, GPU and AMM tables come off the top; flexible and direct memory share what
// is left. One 4 KiB table maps 2 MiB.
constexpr uint64_t GUEST_MEMORY_BUDGET   = 12800ull * 1024ull * 1024ull;
constexpr uint64_t RESERVED_CACHE_SIZE   = 64ull * 1024ull * 1024ull;
constexpr uint64_t INCLUDED_TABLE_MEMORY = 64ull * 1024ull * 1024ull;
constexpr uint64_t MAPPING_TABLE_SIZE    = 4ull * 1024ull;

static uint64_t                 g_guest_memory_size    = GUEST_MEMORY_BUDGET - RESERVED_CACHE_SIZE;
static uint64_t                 g_flexible_memory_size = DEFAULT_FLEXIBLE_MEMORY_SIZE;
static uint64_t                 g_cpu_table_memory     = INCLUDED_TABLE_MEMORY;
static uint64_t                 g_gpu_table_memory     = INCLUDED_TABLE_MEMORY;
static Graphics::RenderContext* g_gpu_resources        = nullptr;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static void (*g_test_gpu_range)(bool, uint64_t, size_t) = nullptr;
#endif

static Graphics::RenderContext& GetGpuResources() {
	EXIT_IF(g_gpu_resources == nullptr);
	return *g_gpu_resources;
}

static bool IsGpuAddressRange(uint64_t vaddr, uint64_t size) {
	return Graphics::GuestRange {vaddr, size}.Valid();
}

static void MapGpuRange(uint64_t vaddr, uint64_t size) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_gpu_range != nullptr) g_test_gpu_range(true, vaddr, size);
#endif
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) {
		return;
	}
	GetGpuResources().MapMemory(vaddr, size);
}

static void UnmapGpuRange(uint64_t vaddr, uint64_t size) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_gpu_range != nullptr) g_test_gpu_range(false, vaddr, size);
#endif
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) {
		return;
	}
	GetGpuResources().UnmapMemory(vaddr, size);
}

static bool DecodeMemoryProtection(int prot, VirtualMemory::Mode* mode) {
	EXIT_IF(mode == nullptr);

	bool cpu_read  = (prot & PROT_CPU_READ) != 0;
	bool cpu_write = (prot & PROT_CPU_WRITE) != 0;
	bool cpu_exec  = (prot & PROT_CPU_EXEC) != 0;
	bool gpu_read  = (prot & PROT_GPU_READ) != 0;
	bool gpu_write = (prot & PROT_GPU_WRITE) != 0;

	if ((prot &
	     (PROT_CPU_READ | PROT_CPU_WRITE | PROT_CPU_EXEC | PROT_GPU_READ | PROT_GPU_WRITE)) == 0 &&
	    prot != 0) {
		return false;
	}

	bool host_read  = cpu_read || gpu_read;
	bool host_write = cpu_write || gpu_write;

	if (host_write) {
		host_read = true;
	}

	*mode = VirtualMemory::Mode::NoAccess;
	if (cpu_exec) {
		*mode = (host_write ? VirtualMemory::Mode::ExecuteReadWrite
		                    : (host_read ? VirtualMemory::Mode::ExecuteRead
		                                 : VirtualMemory::Mode::Execute));
	} else if (host_write) {
		*mode = VirtualMemory::Mode::ReadWrite;
	} else if (host_read) {
		*mode = VirtualMemory::Mode::Read;
	}

	return true;
}

static void CopyVirtualRangeName(char* dst, const char* name) {
	EXIT_IF(dst == nullptr);

	std::memset(dst, 0, KERNEL_MAXIMUM_NAME_LENGTH);

	if (name != nullptr) {
		std::strncpy(dst, name, KERNEL_MAXIMUM_NAME_LENGTH - 1);
	}
}

static bool VirtualRangesOverlap(uint64_t left_start, uint64_t left_size, uint64_t right_start,
                                 uint64_t right_size) {
	if (left_size == 0 || right_size == 0) {
		return false;
	}

	auto left_end = (UINT64_MAX - left_start < left_size ? UINT64_MAX : left_start + left_size);
	auto right_end =
	    (UINT64_MAX - right_start < right_size ? UINT64_MAX : right_start + right_size);

	return left_start < right_end && right_start < left_end;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static uint32_t              g_test_backing_store_unmaps_before_failure = UINT32_MAX;
static callback_func_t       g_test_before_backing_map                  = nullptr;
static callback_func_t       g_test_backing_read                        = nullptr;
static uint64_t              g_test_backing_bytes_zeroed                = 0;
static std::atomic<uint64_t> g_test_free_range_visits {0};
#endif

#include "memoryAddressSpace.inc"

enum class VirtualRangeType {
	Reserved,
	PoolReserved,
	Direct,
	Flexible,
	Pooled,
	Stack,
	Code,
	Runtime,
};

static bool IsReservedRangeType(VirtualRangeType type) {
	return type == VirtualRangeType::Reserved || type == VirtualRangeType::PoolReserved;
}

static bool IsPooledRangeType(VirtualRangeType type) {
	return type == VirtualRangeType::Pooled || type == VirtualRangeType::PoolReserved;
}

static bool IsCommittedRangeType(VirtualRangeType type) {
	return !IsReservedRangeType(type);
}

static bool IsPrivateCommittedRangeType(VirtualRangeType type) {
	return type == VirtualRangeType::Stack || type == VirtualRangeType::Code ||
	       type == VirtualRangeType::Runtime;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static bool g_test_fail_next_range_replace = false;
#endif

class VirtualRanges {
public:
	struct Range {
		uint64_t         start          = 0;
		uint64_t         size           = 0;
		uint64_t         offset         = 0;
		int              protection     = 0;
		int              memory_type    = 0;
		VirtualRangeType type           = VirtualRangeType::Reserved;
		bool             disallow_merge = false;
		char             name[KERNEL_MAXIMUM_NAME_LENGTH];
	};

	bool Add(uint64_t start, uint64_t size, uint64_t offset, int protection, int memory_type,
	         VirtualRangeType type, const char* name, bool disallow_merge = false) {
		Common::LockGuard lock(m_mutex);

		if (start == 0 || size == 0 || size > UINT64_MAX - start) {
			return false;
		}
		auto position = LowerBound(start);
		if ((position != m_ranges.end() &&
		     VirtualRangesOverlap(start, size, position->second.start, position->second.size)) ||
		    (position != m_ranges.begin() &&
		     VirtualRangesOverlap(start, size, std::prev(position)->second.start,
		                          std::prev(position)->second.size))) {
			return false;
		}

		Range r {};
		r.start          = start;
		r.size           = size;
		r.offset         = offset;
		r.protection     = protection;
		r.memory_type    = memory_type;
		r.type           = type;
		r.disallow_merge = disallow_merge;
		CopyVirtualRangeName(r.name, name);
		auto inserted = m_ranges.emplace_hint(position, start, r);
		UpdatePageTableEntries(inserted->second, true);
		MergeAroundUnlocked(inserted);
		return true;
	}

	bool Remove(uint64_t start, uint64_t size) {
		Common::LockGuard lock(m_mutex);

		auto position = m_ranges.find(start);
		if (position != m_ranges.end() && position->second.size == size) {
			EraseUnlocked(position);
			return true;
		}
		return RemoveUnlocked(start, size);
	}

	bool HasOverlap(uint64_t start, uint64_t size) {
		Common::LockGuard lock(m_mutex);

		return FindOverlap(start, size) != nullptr;
	}

	bool QueryOverlap(uint64_t start, uint64_t size, Range* out) {
		EXIT_IF(out == nullptr);
		Common::LockGuard lock(m_mutex);

		const auto* overlap = FindOverlap(start, size);
		if (overlap == nullptr) {
			return false;
		}
		*out = *overlap;
		return true;
	}

	bool ReleaseReserved(uint64_t start, uint64_t size) {
		Common::LockGuard lock(m_mutex);

		auto position = m_ranges.find(start);
		if (position != m_ranges.end() && position->second.size == size &&
		    IsReservedRangeType(position->second.type)) {
			EraseUnlocked(position);
		}
		return true;
	}

	bool ReplaceSpan(uint64_t start, uint64_t size, VirtualRangeType expected_type, uint64_t offset,
	                 int protection, int memory_type, VirtualRangeType type, const char* name,
	                 bool disallow_merge = false) {
		Common::LockGuard lock(m_mutex);

		if (start == 0 || size == 0 || size > UINT64_MAX - start) {
			return false;
		}

		auto       current = start;
		const auto end     = start + size;
		for (auto candidate = FindContaining(start); current < end; ++candidate) {
			if (candidate == m_ranges.end() || candidate->second.start > current ||
			    candidate->second.type != expected_type) {
				return false;
			}
			current = std::min(end, End(candidate->second.start, candidate->second.size));
		}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_fail_next_range_replace) {
			g_test_fail_next_range_replace = false;
			return false;
		}
#endif
		Range replacement {};
		replacement.start          = start;
		replacement.size           = size;
		replacement.offset         = offset;
		replacement.protection     = protection;
		replacement.memory_type    = memory_type;
		replacement.type           = type;
		replacement.disallow_merge = disallow_merge;
		CopyVirtualRangeName(replacement.name, name);
		// Validation precedes all mutations. Keep the first node for the replacement so
		// replacing an existing arena does not allocate another tree node.
		auto first = SplitAtUnlocked(start);
		auto last  = SplitAtUnlocked(end);
		for (auto it = std::next(first); it != last;) {
			it = EraseUnlocked(it);
		}
		UpdatePageTableEntries(first->second, false);
		first->second = replacement;
		UpdatePageTableEntries(first->second, true);
		MergeAroundUnlocked(first);
		return true;
	}

	void Rename(uint64_t start, uint64_t size, const char* name) {
		Common::LockGuard lock(m_mutex);

		auto position = m_ranges.find(start);
		if (position != m_ranges.end() && position->second.size == size) {
			CopyVirtualRangeName(position->second.name, name);
			MergeAroundUnlocked(position);
			return;
		}
		EditUnlocked(start, size, [name](Range* r) { CopyVirtualRangeName(r->name, name); });
	}

	void Protect(uint64_t start, uint64_t size, int protection) {
		Common::LockGuard lock(m_mutex);

		EditUnlocked(start, size, [protection](Range* r) { r->protection = protection; });
	}

	bool Query(uint64_t addr, int flags, Range* out) {
		EXIT_IF(out == nullptr);

		Common::LockGuard lock(m_mutex);

		auto next = m_ranges.upper_bound(addr);
		if (next != m_ranges.begin()) {
			auto current = std::prev(next);
			if (addr < End(current->second.start, current->second.size)) {
				*out = current->second;
				return true;
			}
		}
		if (flags != 1 || next == m_ranges.end()) {
			return false;
		}

		*out = next->second;
		return true;
	}

	bool QuerySpan(uint64_t start, uint64_t size, std::vector<Range>* out) {
		EXIT_IF(out == nullptr);

		Common::LockGuard lock(m_mutex);
		out->clear();
		if (start == 0 || size == 0 || size > UINT64_MAX - start) {
			return false;
		}

		const auto end     = start + size;
		auto       current = start;
		for (auto it = FindContaining(start); it != m_ranges.end(); ++it) {
			const auto& range     = it->second;
			const auto  range_end = End(range.start, range.size);
			if (range.start > current) {
				break;
			}

			Range part = range;
			part.start = current;
			part.size  = std::min(end, range_end) - current;
			if (part.type == VirtualRangeType::Direct) {
				part.offset += current - range.start;
			}
			out->push_back(part);
			current += part.size;
			if (current == end) {
				return true;
			}
		}

		out->clear();
		return false;
	}

	uint64_t ClampRangeSize(uint64_t virtual_addr, uint64_t size) {
		Common::LockGuard lock(m_mutex);

		if (virtual_addr == 0 || size == 0 || size > UINT64_MAX - virtual_addr) {
			return 0;
		}

		auto vma = FindContaining(virtual_addr);
		if (vma == m_ranges.end() || !IsCommittedRangeType(vma->second.type)) {
			return 0;
		}
		const auto vma_end = End(vma->second.start, vma->second.size);

		uint64_t clamped_size = std::min(size, vma_end - virtual_addr);
		uint64_t expected     = virtual_addr + clamped_size;
		++vma;

		while (vma != m_ranges.end() && vma->second.start == expected &&
		       IsCommittedRangeType(vma->second.type) && clamped_size < size) {
			const auto chunk = std::min(size - clamped_size, vma->second.size);
			clamped_size += chunk;
			expected += chunk;
			++vma;
		}

		return clamped_size;
	}

	uint64_t CountPageTableEntries(bool gpu) {
		Common::LockGuard lock(m_mutex);
		return gpu ? m_gpu_page_table_entries : m_cpu_page_table_entries;
	}

private:
	static uint64_t End(uint64_t start, uint64_t size) {
		return (UINT64_MAX - start < size ? UINT64_MAX : start + size);
	}

	static bool SameMergeKey(const Range& left, const Range& right) {
		if (left.disallow_merge || right.disallow_merge || left.type == VirtualRangeType::Direct ||
		    right.type == VirtualRangeType::Direct) {
			return false;
		}

		return left.type == right.type && left.protection == right.protection &&
		       left.memory_type == right.memory_type &&
		       std::strncmp(left.name, right.name, KERNEL_MAXIMUM_NAME_LENGTH) == 0;
	}

	// Keys are range starts; ranges are nonempty, non-overlapping and locally coalesced.
	// A tree avoids shifting unrelated ranges when streaming edits split or remove an arena.
	using RangeMap = std::map<uint64_t, Range>;
	using Iterator = RangeMap::iterator;

	Iterator LowerBound(uint64_t start) { return m_ranges.lower_bound(start); }

	Iterator FindContaining(uint64_t address) {
		auto next = m_ranges.upper_bound(address);
		if (next == m_ranges.begin()) {
			return m_ranges.end();
		}
		auto current = std::prev(next);
		return address < End(current->second.start, current->second.size) ? current
		                                                                  : m_ranges.end();
	}

	static bool CanMerge(const Range& left, const Range& right) {
		return End(left.start, left.size) == right.start && SameMergeKey(left, right);
	}

	void UpdatePageTableEntries(const Range& range, bool add) {
		if (!IsCommittedRangeType(range.type)) {
			return;
		}
		// Preserve the statistics API's per-range accounting: splitting within a page-table
		// entry counts it for each fragment, and merging removes that duplicate contribution.
		const auto entries = (End(range.start, range.size) - 1) / PAGE_TABLE_GRANULARITY -
		                     range.start / PAGE_TABLE_GRANULARITY + 1;
		if ((range.protection & (PROT_CPU_READ | PROT_CPU_WRITE | PROT_CPU_EXEC)) != 0) {
			m_cpu_page_table_entries =
			    add ? m_cpu_page_table_entries + entries : m_cpu_page_table_entries - entries;
		}
		if ((range.protection & (PROT_GPU_READ | PROT_GPU_WRITE)) != 0) {
			m_gpu_page_table_entries =
			    add ? m_gpu_page_table_entries + entries : m_gpu_page_table_entries - entries;
		}
	}

	Iterator EraseUnlocked(Iterator position) {
		UpdatePageTableEntries(position->second, false);
		return m_ranges.erase(position);
	}

	Iterator MergeNextUnlocked(Iterator left, Iterator right) {
		UpdatePageTableEntries(left->second, false);
		left->second.size += right->second.size;
		auto next = EraseUnlocked(right);
		UpdatePageTableEntries(left->second, true);
		return next;
	}

	void MergeAroundUnlocked(Iterator current) {
		if (current != m_ranges.begin()) {
			auto previous = std::prev(current);
			if (CanMerge(previous->second, current->second)) {
				MergeNextUnlocked(previous, current);
				current = previous;
			}
		}
		for (auto next = std::next(current); next != m_ranges.end();) {
			if (!CanMerge(current->second, next->second)) {
				break;
			}
			next = MergeNextUnlocked(current, next);
		}
	}

	Iterator SplitAtUnlocked(uint64_t address) {
		auto position = LowerBound(address);
		if (position != m_ranges.begin()) {
			auto&      previous     = std::prev(position)->second;
			const auto previous_end = End(previous.start, previous.size);
			if (previous.start < address && address < previous_end) {
				Range right = previous;
				right.start = address;
				right.size  = previous_end - address;
				if (right.type == VirtualRangeType::Direct) {
					right.offset += address - previous.start;
				}
				position = m_ranges.emplace_hint(position, address, right);
				UpdatePageTableEntries(previous, false);
				previous.size = address - previous.start;
				UpdatePageTableEntries(previous, true);
				UpdatePageTableEntries(right, true);
			}
		}
		return position;
	}

	void MergeSpanUnlocked(Iterator first, Iterator last) {
		if (first == last) {
			return;
		}
		// Include both neighbors. The right neighbor itself can be erased by a merge,
		// so the sentinel must be the node after it, which this loop never touches.
		const auto stop = last == m_ranges.end() ? last : std::next(last);
		if (first != m_ranges.begin()) {
			--first;
		}
		for (auto next = std::next(first); next != stop;) {
			if (CanMerge(first->second, next->second)) {
				next = MergeNextUnlocked(first, next);
			} else {
				first = next++;
			}
		}
	}

	template <typename EditFunc>
	void EditUnlocked(uint64_t start, uint64_t size, EditFunc edit) {
		if (size == 0) {
			return;
		}
		auto first = SplitAtUnlocked(start);
		auto last  = SplitAtUnlocked(End(start, size));
		for (auto it = first; it != last; ++it) {
			UpdatePageTableEntries(it->second, false);
			edit(&it->second);
			UpdatePageTableEntries(it->second, true);
		}
		MergeSpanUnlocked(first, last);
	}

	bool RemoveUnlocked(uint64_t start, uint64_t size) {
		if (size == 0) {
			return false;
		}
		auto first = SplitAtUnlocked(start);
		auto last  = SplitAtUnlocked(End(start, size));
		if (first == last) {
			return false;
		}
		while (first != last) {
			first = EraseUnlocked(first);
		}
		// Removal leaves an address gap, so it cannot make surviving ranges mergeable.
		return true;
	}

	Range* FindOverlap(uint64_t start, uint64_t size) {
		auto position = LowerBound(start);
		if (position != m_ranges.end() &&
		    VirtualRangesOverlap(start, size, position->second.start, position->second.size)) {
			return &position->second;
		}
		if (position != m_ranges.begin()) {
			auto previous = std::prev(position);
			if (VirtualRangesOverlap(start, size, previous->second.start, previous->second.size)) {
				return &previous->second;
			}
		}
		return nullptr;
	}

	RangeMap      m_ranges;
	Common::Mutex m_mutex;
	uint64_t      m_cpu_page_table_entries = 0;
	uint64_t      m_gpu_page_table_entries = 0;
};

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static uint32_t g_test_physical_memory_unmaps_before_failure = UINT32_MAX;
static bool     g_test_fail_next_fixed_reserve_range_add     = false;
#endif

static void RemoveFreeRange(std::map<uint64_t, uint64_t>& ranges, uint64_t start, uint64_t size) {
	const auto end   = start + size;
	auto       range = ranges.upper_bound(start);
	if (range != ranges.begin()) {
		--range;
	}
	while (range != ranges.end() && range->first < end) {
		const auto range_start = range->first;
		const auto range_end   = range_start + range->second;
		if (range_end <= start) {
			++range;
			continue;
		}
		range = ranges.erase(range);
		if (range_start < start) {
			ranges.emplace(range_start, start - range_start);
		}
		if (end < range_end) {
			ranges.emplace(end, range_end - end);
		}
	}
}

static void AddFreeRange(std::map<uint64_t, uint64_t>& ranges, uint64_t start, uint64_t size) {
	auto end  = start + size;
	auto next = ranges.lower_bound(start);
	if (next != ranges.begin()) {
		auto previous = std::prev(next);
		if (previous->first + previous->second >= start) {
			start = previous->first;
			end   = std::max(end, previous->first + previous->second);
			next  = ranges.erase(previous);
		}
	}
	while (next != ranges.end() && next->first <= end) {
		end  = std::max(end, next->first + next->second);
		next = ranges.erase(next);
	}
	ranges.emplace(start, end - start);
}

class PhysicalMemory {
public:
	enum class AllocationKind { Direct, Pooled, Automatic };
	using PhysicalRanges = std::vector<std::pair<uint64_t, uint64_t>>;

	struct AllocatedBlock {
		uint64_t       start_addr;
		uint64_t       size;
		int            memory_type;
		AllocationKind kind;
	};

	struct Mapping {
		uint64_t start_addr;
		uint64_t size;
	};

	PhysicalMemory() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		m_free.emplace(0, Size());
	}

	KYTY_CLASS_NO_COPY(PhysicalMemory);

	static uint64_t TotalSize() { return g_guest_memory_size; }
	static uint64_t Size() { return TotalSize() - g_flexible_memory_size; }

	bool Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment,
	           uint64_t* phys_addr_out, int memory_type,
	           AllocationKind kind = AllocationKind::Direct);
	bool ReserveAutomatic(uint64_t size, PhysicalRanges* ranges);
	void RestoreAutomatic(const PhysicalRanges& ranges);
	bool Available(uint64_t search_start, uint64_t search_end, size_t alignment,
	               uint64_t* phys_addr_out, uint64_t* size_out);
	bool Release(uint64_t start, size_t len);
	bool Map(uint64_t vaddr, uint64_t phys_addr, size_t len);
	bool Unmap(uint64_t vaddr, uint64_t size);
	bool Find(uint64_t phys_addr, PhysicalMemory::AllocatedBlock* out);
	bool CanMapDirect(uint64_t phys_addr, size_t len);
	bool ReleasePoolExpansion(uint64_t phys_addr, size_t len);
	bool GetAllocatedSpan(uint64_t phys_addr, size_t len, bool skip_gaps,
	                      std::vector<AllocatedBlock>* blocks);
	std::vector<VirtualRanges::Range> FindMappings(uint64_t phys_addr, size_t len);

	[[nodiscard]] Common::Mutex&                            GetMutex() { return m_mutex; }
	[[nodiscard]] const std::map<uint64_t, AllocatedBlock>& GetPhysicalBlocks() const {
		return m_physical;
	}
	[[nodiscard]] const std::map<uint64_t, Mapping>& GetMappings() const { return m_mappings; }

private:
	void                                  ReclaimAutomatic(uint64_t start, uint64_t size);
	std::map<uint64_t, Mapping>::iterator SplitMapping(uint64_t vaddr);

	std::map<uint64_t, AllocatedBlock> m_physical;
	std::map<uint64_t, uint64_t>       m_free;
	std::map<uint64_t, uint64_t>       m_automatic_free;
	// Track backing ownership while a transaction changes views before publishing VirtualRanges.
	// Names, protections and memory types live only in VirtualRanges. Physical offsets may alias.
	std::map<uint64_t, Mapping> m_mappings;
	Common::Mutex               m_mutex;
};

class FlexibleMemory {
public:
	struct Mapping {
		uint64_t vaddr;
		uint64_t size;
		uint64_t backing_offset;
	};

	FlexibleMemory() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		m_free.emplace(PhysicalMemory::Size(), Size());
	}

	KYTY_CLASS_NO_COPY(FlexibleMemory);

	static uint64_t Size() { return g_flexible_memory_size; }
	uint64_t        Available();
	void            Charge(uint64_t size);
	void            Release(uint64_t size);

	bool Map(uint64_t vaddr, size_t len, VirtualMemory::Mode mode);
	bool Unmap(uint64_t vaddr, uint64_t size);
	bool Snapshot(uint64_t vaddr, uint64_t size, std::vector<Mapping>* blocks);
	bool Restore(const std::vector<Mapping>& blocks, VirtualMemory::Mode mode);

	[[nodiscard]] Common::Mutex&                     GetMutex() { return m_mutex; }
	[[nodiscard]] const std::map<uint64_t, Mapping>& GetMappings() const { return m_mappings; }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	uint64_t TestBackingHighWater() const { return m_backing_high_water; }
#endif

private:
	std::map<uint64_t, Mapping>  m_mappings;
	std::map<uint64_t, uint64_t> m_free;
	uint64_t                     m_allocated_total = 0;
	// Only never-issued backing is guaranteed to retain its initial zero contents.
	uint64_t      m_backing_high_water = PhysicalMemory::Size();
	Common::Mutex m_mutex;
};

// All access is serialized by g_memory_operation_mutex.
class PooledMemory {
public:
	struct Mapping {
		uint64_t vaddr;
		uint64_t size;
		uint64_t phys_addr;
	};

	void                   Expand(uint64_t phys_addr, uint64_t size);
	bool                   ReleaseExpansion(uint64_t phys_addr, uint64_t size);
	bool                   Allocate(uint64_t vaddr, uint64_t size, std::vector<Mapping>* mappings);
	bool                   Release(uint64_t vaddr, uint64_t size);
	[[nodiscard]] uint64_t Available();
	[[nodiscard]] const std::map<uint64_t, Mapping>& GetMappings() const { return m_mappings; }

private:
	std::map<uint64_t, uint64_t> m_free;
	std::map<uint64_t, Mapping>  m_mappings;
};

static std::unique_ptr<PhysicalMemory>    g_physical_memory;
static std::unique_ptr<FlexibleMemory>    g_flexible_memory;
static std::unique_ptr<PooledMemory>      g_pooled_memory;
static std::unique_ptr<VirtualRanges>     g_virtual_ranges;
static std::unique_ptr<GuestAddressSpace> g_guest_address_space;
static callback_func_t                    g_alloc_callback        = nullptr;
static callback_func_t                    g_free_callback         = nullptr;
static std::atomic<uint64_t>              g_memory_pool_committed = 0;
static void                               MemoryPoolSubtractCommitted(uint64_t len);
// Keep host mappings, physical blocks, placeholders, and virtual ranges in step.
static std::recursive_mutex g_memory_operation_mutex;

// The base address the PS5 kernel hands out for hint-less user mappings. Guest code can
// assume mappings it did not place explicitly are at or above this (Sony's libc rejects a
// heap below it), so hint-less searches must not fall back to the low system-managed range.
static constexpr uint64_t GUEST_DEFAULT_MAP_BASE = 0x200000000ull;

static uint64_t FindGuestFreeRange(uint64_t search_addr, uint64_t size, uint64_t alignment) {
	EXIT_IF(g_guest_address_space == nullptr || g_virtual_ranges == nullptr);

	auto find_in = [&](uint64_t begin, uint64_t end) {
		auto current = begin;
		while (current < end && size <= end - current) {
			const auto candidate =
			    g_guest_address_space->FindFreeAligned(current, end, size, alignment);
			if (candidate == 0) {
				return uint64_t {0};
			}
			VirtualRanges::Range overlap {};
			if (!g_virtual_ranges->QueryOverlap(candidate, size, &overlap)) {
				return candidate;
			}
			const auto overlap_end = overlap.start + overlap.size;
			if (overlap_end <= current) {
				return uint64_t {0};
			}
			current = overlap_end;
		}
		return uint64_t {0};
	};

	if (search_addr != 0) {
		return find_in(search_addr, HOST_USER_MAX + 1u);
	}
	auto addr = find_in(GUEST_DEFAULT_MAP_BASE, HOST_SYSTEM_MANAGED_MAX + 1u);
	if (addr == 0) {
		addr = find_in(HOST_USER_MIN, HOST_USER_MAX + 1u);
	}
	return addr;
}

bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
	return g_guest_address_space != nullptr &&
	       g_guest_address_space->TryWriteBacking(vaddr, data, size);
}

bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
	return g_guest_address_space != nullptr &&
	       g_guest_address_space->TryReadBacking(vaddr, data, size);
}

bool TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size) {
	if (g_gpu_resources != nullptr && IsGpuAddressRange(vaddr, size)) {
		if (!Graphics::GuestGpu::IsGpuThread()) {
			return false;
		}
		auto& buffers = GetGpuResources().GetBufferCache();
		if (buffers.HasGpuDirtyBytes(vaddr, size)) {
			buffers.ReadMemory(vaddr, size);
		}
	}
	return TryReadBacking(vaddr, data, size);
}

uint64_t ClampRangeSize(uint64_t vaddr, uint64_t size) {
	EXIT_IF(g_virtual_ranges == nullptr);

	const auto clamped_size = g_virtual_ranges->ClampRangeSize(vaddr, size);
	if (clamped_size == 0) {
		EXIT("Memory: attempted to access invalid address 0x%016" PRIx64 " with size 0x%016" PRIx64
		     "\n",
		     vaddr, size);
	}
	if (clamped_size != size) {
		LOGF("Memory: clamped buffer range addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " to 0x%016" PRIx64 "\n",
		     vaddr, size, clamped_size);
	}
	return clamped_size;
}

void WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept {
	if (!TryWriteBacking(vaddr, data, size)) {
		EXIT("Memory: required direct-backing write failed, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
}

void InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (size == 0) {
		return;
	}
	(void)GetGpuResources().InvalidateMemory(vaddr, size);
}

void InstallGpuResources(Graphics::RenderContext* resources) noexcept {
	EXIT_IF(resources != nullptr && g_gpu_resources != nullptr);
	g_gpu_resources = resources;
}

bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	return g_gpu_resources != nullptr && g_gpu_resources->HandleFault(access, fault_vaddr);
}

struct PrtAperture {
	uint64_t address = 0;
	uint64_t size    = 0;
};

constexpr int      PRT_APERTURE_MAX_INDEX = 2;
constexpr uint64_t PRT_PAGE_SIZE          = 0x4000;
constexpr uint64_t PRT_APERTURE_START     = 0x0f00000000ull;
constexpr uint64_t PRT_APERTURE_END       = 0xfc00000000ull;

static std::array<PrtAperture, PRT_APERTURE_MAX_INDEX + 1> g_prt_apertures {};
static Common::Mutex                                       g_prt_aperture_mutex;

static bool IsInPrtAperture(uint64_t address, uint64_t size = 1) {
	if (size == 0 || UINT64_MAX - address < size) {
		return false;
	}
	Common::LockGuard lock(g_prt_aperture_mutex);

	for (const auto& aperture: g_prt_apertures) {
		if (address >= aperture.address) {
			const auto offset = address - aperture.address;
			if (offset < aperture.size && size <= aperture.size - offset) {
				return true;
			}
		}
	}

	return false;
}

bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size) {
	return g_guest_address_space->CopySparseMemory<false>(vaddr, data, size);
}

bool CopySparseMemory(uint64_t vaddr, void* data, uint64_t size) {
	return g_guest_address_space->CopySparseMemory<true>(vaddr, data, size);
}

static bool SelfTestSub64SharedPlaceholderAlias() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	constexpr uint64_t PageSize    = 0x4000;
	const auto         granularity = g_guest_address_space->GetGranularity();
	if (granularity < PageSize * 2u) {
		LOGF_COLOR(
		    Log::Color::Yellow,
		    "\t direct-memory sub-64K placeholder self-test skipped: granularity too small\n");
		return true;
	}

	const auto base = FindGuestFreeRange(0, granularity, granularity);
	if (base == 0) {
		LOGF_COLOR(Log::Color::Red,
		           "\t direct-memory sub-64K placeholder self-test: reserve unavailable\n");
		return false;
	}

	const auto alias          = base + PageSize;
	bool       ok             = false;
	auto       failure_reason = GuestBackingStore::FailureReason::None;
	if (g_guest_address_space->MapBacking(alias, PageSize, PageSize, VirtualMemory::Mode::ReadWrite,
	                                      &failure_reason)) {
		auto* ptr = reinterpret_cast<uint64_t*>(alias);
		*ptr      = 0x4b59545953553634ull; // "KYTYSU64"
		ok        = (*ptr == 0x4b59545953553634ull);
		std::memset(ptr, 0, PageSize);

		ok = g_guest_address_space->UnmapBacking(alias, PageSize) && ok;
	}

	LOGF_COLOR(
	    ok ? Log::Color::Green : Log::Color::Red,
	    "\t direct-memory sub-64K placeholder self-test: %s%s%s\n", ok ? "ok" : "failed",
	    ok ? "" : ", reason = ", ok ? "" : GuestBackingStore::GetFailureReasonName(failure_reason));
	return ok;
#else
	return true;
#endif
}

static bool ReplaceFixedRangeWithReserved(uint64_t start, uint64_t size);

void Initialize() {
	const auto params = Loader::SystemContentGetMemoryParams();
	if (params.flexible_size != 0) {
		g_flexible_memory_size = params.flexible_size;
	}
	g_cpu_table_memory = std::max(params.cpu_table_memory, INCLUDED_TABLE_MEMORY);
	g_gpu_table_memory = std::max(params.gpu_table_memory, INCLUDED_TABLE_MEMORY);

	const auto table_overhead = g_cpu_table_memory + g_gpu_table_memory +
	                            std::max(params.amm_table_memory, INCLUDED_TABLE_MEMORY) -
	                            3 * INCLUDED_TABLE_MEMORY;
	const auto memory_budget = GUEST_MEMORY_BUDGET +
	                           (Loader::SystemContentIsTrinityMode() ? 1216ull * 1024ull * 1024ull : 0);
	EXIT_IF(g_flexible_memory_size + table_overhead >= memory_budget - RESERVED_CACHE_SIZE);
	g_guest_memory_size = memory_budget - RESERVED_CACHE_SIZE - table_overhead;
	LOGF("\t flexible memory size = %" PRIu64 " MiB, direct memory size = %" PRIu64 " MiB\n",
	     g_flexible_memory_size / (1024ull * 1024ull),
	     (g_guest_memory_size - g_flexible_memory_size) / (1024ull * 1024ull));

	VirtualMemory::Init();
	g_guest_address_space = std::make_unique<GuestAddressSpace>(PhysicalMemory::TotalSize());
	g_physical_memory     = std::make_unique<PhysicalMemory>();
	g_flexible_memory     = std::make_unique<FlexibleMemory>();
	g_pooled_memory       = std::make_unique<PooledMemory>();
	g_virtual_ranges      = std::make_unique<VirtualRanges>();
	EXIT_IF(!g_guest_address_space->SelfTest());
	EXIT_IF(!SelfTestSub64SharedPlaceholderAlias());
}

void Shutdown() {
	g_pooled_memory.reset();
	g_flexible_memory.reset();
	g_physical_memory.reset();
	g_virtual_ranges.reset();
	g_guest_address_space.reset();
}

struct AlignedPos {
	uint64_t value = 0;
	bool     valid = false;
};

static constexpr AlignedPos GetAlignedPos(uint64_t pos, size_t alignment) {
	if (alignment == 0) {
		return {pos, true};
	}

	const auto remainder = pos % alignment;
	const auto increment = (remainder != 0 ? alignment - remainder : 0);
	if (increment > UINT64_MAX - pos) {
		return {};
	}

	return {pos + increment, true};
}

static_assert(!GetAlignedPos(UINT64_MAX - 1, 4).valid);

void RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func) {
	EXIT_IF(g_alloc_callback != nullptr || g_free_callback != nullptr);
	EXIT_IF(alloc_func == nullptr || free_func == nullptr);

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	g_alloc_callback = alloc_func;
	g_free_callback  = free_func;

	g_physical_memory->GetMutex().Lock();
	for (const auto& [vaddr, mapping]: g_physical_memory->GetMappings()) {
		g_alloc_callback(vaddr, mapping.size);
	}
	g_physical_memory->GetMutex().Unlock();

	g_flexible_memory->GetMutex().Lock();
	for (const auto& [_, block]: g_flexible_memory->GetMappings()) {
		g_alloc_callback(block.vaddr, block.size);
	}
	g_flexible_memory->GetMutex().Unlock();

	for (const auto& [_, mapping]: g_pooled_memory->GetMappings()) {
		g_alloc_callback(mapping.vaddr, mapping.size);
	}
}

bool PhysicalMemory::Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment,
                           uint64_t* phys_addr_out, int memory_type, AllocationKind kind) {
	if (phys_addr_out == nullptr) {
		return false;
	}

	Common::LockGuard lock(m_mutex);

	search_end = std::min<uint64_t>(search_end, Size());
	if (search_start >= search_end) {
		return false;
	}

	auto range = m_free.upper_bound(search_start);
	if (range != m_free.begin()) {
		range--;
	}
	for (; range != m_free.end() && range->first < search_end; ++range) {
		const auto range_end   = std::min<uint64_t>(range->first + range->second, search_end);
		const auto lower_bound = std::max(range->first, search_start);
		const auto aligned     = GetAlignedPos(lower_bound, alignment);
		const auto free_pos    = aligned.value;
		if (!aligned.valid || free_pos < lower_bound || free_pos > range_end ||
		    len > range_end - free_pos) {
			continue;
		}

		AllocatedBlock b {};
		b.size           = len;
		b.start_addr     = free_pos;
		b.memory_type    = memory_type;
		b.kind           = kind;

		RemoveFreeRange(m_free, free_pos, len);
		EXIT_IF(!m_physical.emplace(b.start_addr, b).second);
		if (kind == AllocationKind::Automatic) {
			AddFreeRange(m_automatic_free, free_pos, len);
		}

		*phys_addr_out = free_pos;
		return true;
	}

	return false;
}

bool PhysicalMemory::Available(uint64_t search_start, uint64_t search_end, size_t alignment,
                               uint64_t* phys_addr_out, uint64_t* size_out) {
	if (phys_addr_out == nullptr || size_out == nullptr) {
		return false;
	}

	Common::LockGuard lock(m_mutex);

	search_end = std::min<uint64_t>(search_end, Size());
	if (search_start >= search_end) {
		return false;
	}

	uint64_t best_addr = 0;
	uint64_t best_size = 0;

	auto range = m_free.upper_bound(search_start);
	if (range != m_free.begin()) --range;
	for (; range != m_free.end(); ++range) {
		const auto [range_start, range_size] = *range;
		if (range_start >= search_end) {
			break;
		}
		const auto range_end   = std::min<uint64_t>(range_start + range_size, search_end);
		const auto lower_bound = std::max(range_start, search_start);
		const auto aligned     = GetAlignedPos(lower_bound, alignment);
		const auto free_pos    = aligned.value;
		if (aligned.valid && free_pos >= lower_bound && free_pos < range_end &&
		    range_end - free_pos > best_size) {
			best_addr = free_pos;
			best_size = range_end - free_pos;
		}
	}

	if (best_size == 0) {
		return false;
	}

	*phys_addr_out = best_addr;
	*size_out      = best_size;
	return true;
}

bool PhysicalMemory::ReserveAutomatic(uint64_t size, PhysicalRanges* ranges) {
	Common::LockGuard lock(m_mutex);
	for (const auto& [start, available]: m_automatic_free) {
		const auto length = std::min(size, available);
		ranges->emplace_back(start, length);
		size -= length;
		if (size == 0) {
			for (const auto& [offset, length]: *ranges) {
				RemoveFreeRange(m_automatic_free, offset, length);
			}
			return true;
		}
	}
	ranges->clear();
	return false;
}

void PhysicalMemory::RestoreAutomatic(const PhysicalRanges& ranges) {
	Common::LockGuard lock(m_mutex);
	for (const auto& [start, size]: ranges) {
		AddFreeRange(m_automatic_free, start, size);
	}
}

void PhysicalMemory::ReclaimAutomatic(uint64_t start, uint64_t size) {
	const auto end   = start + size;
	auto       owner = m_physical.upper_bound(start);
	if (owner != m_physical.begin()) {
		--owner;
	}
	bool automatic = false;
	for (; owner != m_physical.end() && owner->first < end; ++owner) {
		const auto& block = owner->second;
		if (block.kind != AllocationKind::Automatic || block.start_addr + block.size <= start) {
			continue;
		}
		const auto first = std::max(start, block.start_addr);
		const auto last  = std::min(end, block.start_addr + block.size);
		AddFreeRange(m_automatic_free, first, last - first);
		automatic = true;
	}
	if (automatic) {
		for (const auto& [vaddr, mapping]: m_mappings) {
			const auto first = std::max(start, mapping.start_addr);
			const auto last  = std::min(end, mapping.start_addr + mapping.size);
			if (first < last) {
				RemoveFreeRange(m_automatic_free, first, last - first);
			}
		}
	}
}

bool PhysicalMemory::Release(uint64_t start, size_t len) {
	Common::LockGuard lock(m_mutex);

	auto next = m_physical.upper_bound(start);
	if (next == m_physical.begin()) {
		return false;
	}
	auto       it    = std::prev(next);
	const auto block = it->second;
	if (len == 0 || block.kind == AllocationKind::Pooled || start < block.start_addr ||
	    start >= block.start_addr + block.size || len > block.start_addr + block.size - start) {
		return false;
	}

	if (block.kind == AllocationKind::Automatic) {
		RemoveFreeRange(m_automatic_free, start, len);
	}
	if (block.start_addr < start) {
		it->second.size = start - block.start_addr;
	} else {
		m_physical.erase(it);
	}
	if (start + len < block.start_addr + block.size) {
		auto right       = block;
		right.start_addr = start + len;
		right.size       = block.start_addr + block.size - right.start_addr;
		m_physical.emplace(right.start_addr, right);
	}
	AddFreeRange(m_free, start, len);
	return true;
}

bool PhysicalMemory::Map(uint64_t vaddr, uint64_t phys_addr, size_t len) {
	Common::LockGuard lock(m_mutex);

	if (vaddr == 0 || len == 0 || UINT64_MAX - vaddr < len || !CanMapDirect(phys_addr, len)) {
		return false;
	}
	auto next = m_mappings.lower_bound(vaddr);
	if ((next != m_mappings.end() && next->first < vaddr + len) ||
	    (next != m_mappings.begin() &&
	     std::prev(next)->first + std::prev(next)->second.size > vaddr)) {
		return false;
	}

	m_mappings.emplace_hint(next, vaddr, Mapping {phys_addr, len});
	RemoveFreeRange(m_automatic_free, phys_addr, len);
	return true;
}

bool PhysicalMemory::CanMapDirect(uint64_t phys_addr, size_t len) {
	return GetAllocatedSpan(phys_addr, len, false, nullptr);
}

bool PhysicalMemory::ReleasePoolExpansion(uint64_t phys_addr, size_t len) {
	Common::LockGuard lock(m_mutex);
	const auto        it = m_physical.find(phys_addr);
	if (it == m_physical.end() || it->second.kind != AllocationKind::Pooled ||
	    it->second.size != len) {
		return false;
	}
	m_physical.erase(it);
	AddFreeRange(m_free, phys_addr, len);
	return true;
}

bool PhysicalMemory::GetAllocatedSpan(uint64_t phys_addr, size_t len, bool skip_gaps,
                                      std::vector<AllocatedBlock>* blocks) {
	if (blocks != nullptr) {
		blocks->clear();
	}
	if (len == 0 || UINT64_MAX - phys_addr < len) {
		return false;
	}

	Common::LockGuard lock(m_mutex);
	const auto        end     = phys_addr + len;
	auto              current = phys_addr;
	auto              block   = m_physical.upper_bound(phys_addr);
	if (block != m_physical.begin()) {
		--block;
	}
	for (; block != m_physical.end() && block->second.start_addr < end; ++block) {
		const auto block_end = block->second.start_addr + block->second.size;
		if (block_end <= current) {
			continue;
		}
		if (block->second.kind == AllocationKind::Pooled ||
		    (!skip_gaps && current < block->second.start_addr)) {
			return false;
		}
		auto part       = block->second;
		part.start_addr = std::max(current, part.start_addr);
		current         = std::min(end, block_end);
		part.size       = current - part.start_addr;
		if (blocks != nullptr) {
			blocks->push_back(part);
		}
	}
	return skip_gaps || current == end;
}

// Return the first mapping beginning at or after vaddr, splitting a containing interval.
std::map<uint64_t, PhysicalMemory::Mapping>::iterator PhysicalMemory::SplitMapping(uint64_t vaddr) {
	auto next = m_mappings.lower_bound(vaddr);
	if (next != m_mappings.end() && next->first == vaddr) {
		return next;
	}
	if (next != m_mappings.begin()) {
		auto       previous = std::prev(next);
		const auto offset   = vaddr - previous->first;
		if (offset < previous->second.size) {
			const Mapping right {previous->second.start_addr + offset,
			                     previous->second.size - offset};
			next                  = m_mappings.emplace_hint(next, vaddr, right);
			previous->second.size = offset;
		}
	}
	return next;
}

bool PhysicalMemory::Unmap(uint64_t vaddr, uint64_t size) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_physical_memory_unmaps_before_failure == 0) {
		g_test_physical_memory_unmaps_before_failure = UINT32_MAX;
		return false;
	}
	if (g_test_physical_memory_unmaps_before_failure != UINT32_MAX) {
		g_test_physical_memory_unmaps_before_failure--;
	}
#endif

	Common::LockGuard lock(m_mutex);
	if (size == 0 || UINT64_MAX - vaddr < size) {
		return false;
	}
	const auto end   = vaddr + size;
	auto       first = m_mappings.upper_bound(vaddr);
	if (first != m_mappings.begin()) {
		--first;
	}
	// Rollback can leave several ownership intervals inside one semantic virtual range.
	// Validate the complete span before changing any ownership records.
	auto current = vaddr;
	for (auto it = first; current < end; ++it) {
		if (it == m_mappings.end() || it->first > current ||
		    current >= it->first + it->second.size) {
			return false;
		}
		current = std::min(end, it->first + it->second.size);
	}
	const auto last = SplitMapping(end);
	first           = SplitMapping(vaddr);
	while (first != last) {
		const auto removed = first->second;
		first              = m_mappings.erase(first);
		ReclaimAutomatic(removed.start_addr, removed.size);
	}
	return true;
}

bool PhysicalMemory::Find(uint64_t phys_addr, AllocatedBlock* out) {
	EXIT_IF(out == nullptr);

	Common::LockGuard lock(m_mutex);

	auto following = m_physical.upper_bound(phys_addr);
	if (following != m_physical.begin()) {
		const auto& block = std::prev(following)->second;
		if (phys_addr < block.start_addr + block.size) {
			*out = block;
			return true;
		}
	}

	return false;
}

std::vector<VirtualRanges::Range> PhysicalMemory::FindMappings(uint64_t phys_addr, size_t len) {
	Common::LockGuard lock(m_mutex);

	std::vector<VirtualRanges::Range> mappings;
	std::vector<VirtualRanges::Range> parts;
	for (const auto& [vaddr, mapping]: m_mappings) {
		if (!VirtualRangesOverlap(phys_addr, len, mapping.start_addr, mapping.size)) {
			continue;
		}
		const auto overlap_start = std::max(phys_addr, mapping.start_addr);
		const auto overlap_end   = std::min(phys_addr + len, mapping.start_addr + mapping.size);
		EXIT_IF(!g_virtual_ranges->QuerySpan(vaddr + overlap_start - mapping.start_addr,
		                                     overlap_end - overlap_start, &parts));
		for (const auto& part: parts) {
			EXIT_IF(part.type != VirtualRangeType::Direct);
			mappings.push_back(part);
		}
	}
	return mappings;
}

bool FlexibleMemory::Map(uint64_t vaddr, size_t len, VirtualMemory::Mode mode) {
	Common::LockGuard lock(m_mutex);
	if (len == 0 || len > Size() - std::min(Size(), m_allocated_total)) {
		return false;
	}

	std::vector<Mapping> blocks;
	auto                 current   = vaddr;
	auto                 remaining = static_cast<uint64_t>(len);
	for (const auto& [backing_offset, free_size]: m_free) {
		if (remaining == 0) break;
		const auto chunk = std::min(remaining, free_size);
		blocks.push_back({current, chunk, backing_offset});
		current += chunk;
		remaining -= chunk;
	}
	if (remaining != 0) return false;

	size_t mapped = 0;
	for (const auto& block: blocks) {
		const auto reused = block.backing_offset < m_backing_high_water
		                        ? std::min(block.size, m_backing_high_water - block.backing_offset)
		                        : 0;
		const bool cleared =
		    reused == 0 || g_guest_address_space->ZeroBacking(block.backing_offset, reused);
		// Never roll this back: a successfully exposed view can be written before a later failure.
		m_backing_high_water = std::max(m_backing_high_water, block.backing_offset + block.size);
		if (!cleared || !g_guest_address_space->MapBacking(block.vaddr, block.size,
		                                                   block.backing_offset, mode)) {
			while (mapped != 0) {
				const auto& previous = blocks[--mapped];
				EXIT_IF(!g_guest_address_space->UnmapBacking(previous.vaddr, previous.size));
			}
			return false;
		}
		++mapped;
	}

	for (const auto& block: blocks) {
		RemoveFreeRange(m_free, block.backing_offset, block.size);
		EXIT_IF(!m_mappings.emplace(block.vaddr, block).second);
	}
	m_allocated_total += len;
	return true;
}

bool FlexibleMemory::Unmap(uint64_t vaddr, uint64_t size) {
	Common::LockGuard lock(m_mutex);
	if (size == 0 || UINT64_MAX - vaddr < size) return false;

	const auto end   = vaddr + size;
	auto       first = m_mappings.upper_bound(vaddr);
	if (first == m_mappings.begin()) return false;
	--first;
	auto current = vaddr;
	for (auto it = first; it != m_mappings.end() && current < end; ++it) {
		const auto& block = it->second;
		if (block.vaddr > current || current >= block.vaddr + block.size) return false;
		current = std::min(end, block.vaddr + block.size);
	}
	if (current != end || !g_guest_address_space->UnmapBacking(vaddr, size)) return false;

	for (auto it = first; it != m_mappings.end() && it->first < end;) {
		const auto block     = it->second;
		const auto block_end = block.vaddr + block.size;
		const auto cut_start = std::max(vaddr, block.vaddr);
		const auto cut_end   = std::min(end, block_end);
		it                   = m_mappings.erase(it);
		AddFreeRange(m_free, block.backing_offset + cut_start - block.vaddr, cut_end - cut_start);
		if (block.vaddr < cut_start) {
			m_mappings.emplace(
			    block.vaddr, Mapping {block.vaddr, cut_start - block.vaddr, block.backing_offset});
		}
		if (cut_end < block_end) {
			m_mappings.emplace(cut_end, Mapping {cut_end, block_end - cut_end,
			                                     block.backing_offset + cut_end - block.vaddr});
		}
	}
	EXIT_IF(size > m_allocated_total);
	m_allocated_total -= size;
	return true;
}

bool FlexibleMemory::Snapshot(uint64_t vaddr, uint64_t size, std::vector<Mapping>* blocks) {
	EXIT_IF(blocks == nullptr);
	Common::LockGuard lock(m_mutex);
	blocks->clear();
	if (size == 0 || UINT64_MAX - vaddr < size) return false;

	auto it = m_mappings.upper_bound(vaddr);
	if (it == m_mappings.begin()) return false;
	--it;
	const auto end     = vaddr + size;
	auto       current = vaddr;
	for (; it != m_mappings.end() && current < end; ++it) {
		const auto& block = it->second;
		if (block.vaddr > current || current >= block.vaddr + block.size) break;
		const auto part_end = std::min(end, block.vaddr + block.size);
		blocks->push_back(
		    {current, part_end - current, block.backing_offset + current - block.vaddr});
		current = part_end;
	}
	if (current == end) return true;
	blocks->clear();
	return false;
}

bool FlexibleMemory::Restore(const std::vector<Mapping>& blocks, VirtualMemory::Mode mode) {
	Common::LockGuard lock(m_mutex);
	if (blocks.empty()) return false;

	for (const auto& block: blocks) {
		auto next = m_free.upper_bound(block.backing_offset);
		if (next == m_free.begin()) return false;
		const auto range = std::prev(next);
		if (!GuestFreeRangeContains(range->first, range->second, block.backing_offset,
		                            block.size)) {
			return false;
		}
	}

	size_t mapped = 0;
	for (const auto& block: blocks) {
		if (!g_guest_address_space->MapBacking(block.vaddr, block.size, block.backing_offset,
		                                       mode)) {
			while (mapped != 0) {
				const auto& previous = blocks[--mapped];
				EXIT_IF(!g_guest_address_space->UnmapBacking(previous.vaddr, previous.size));
			}
			return false;
		}
		++mapped;
	}
	for (const auto& block: blocks) {
		RemoveFreeRange(m_free, block.backing_offset, block.size);
		EXIT_IF(!m_mappings.emplace(block.vaddr, block).second);
		m_allocated_total += block.size;
	}
	return true;
}

uint64_t FlexibleMemory::Available() {
	Common::LockGuard lock(m_mutex);

	return (Size() >= m_allocated_total ? Size() - m_allocated_total : 0);
}

void FlexibleMemory::Charge(uint64_t size) {
	Common::LockGuard lock(m_mutex);

	m_allocated_total += size;
}

void FlexibleMemory::Release(uint64_t size) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(size > m_allocated_total);
	m_allocated_total -= size;
}

void PooledMemory::Expand(uint64_t phys_addr, uint64_t size) {
	AddFreeRange(m_free, phys_addr, size);
}

bool PooledMemory::ReleaseExpansion(uint64_t phys_addr, uint64_t size) {
	auto next = m_free.upper_bound(phys_addr);
	if (next == m_free.begin()) return false;
	const auto range = std::prev(next);
	if (!GuestFreeRangeContains(range->first, range->second, phys_addr, size)) return false;
	RemoveFreeRange(m_free, phys_addr, size);
	return true;
}

bool PooledMemory::Allocate(uint64_t vaddr, uint64_t size, std::vector<Mapping>* mappings) {
	EXIT_IF(mappings == nullptr);
	mappings->clear();
	if (vaddr == 0 || size == 0 || UINT64_MAX - vaddr < size) return false;

	auto current   = vaddr;
	auto remaining = size;
	for (const auto& [start, free_size]: m_free) {
		if (remaining == 0) break;
		const auto part_size = std::min(free_size, remaining);
		mappings->push_back({current, part_size, start});
		current += part_size;
		remaining -= part_size;
	}
	if (remaining != 0) {
		mappings->clear();
		return false;
	}
	for (const auto& mapping: *mappings) {
		RemoveFreeRange(m_free, mapping.phys_addr, mapping.size);
		EXIT_IF(!m_mappings.emplace(mapping.vaddr, mapping).second);
	}
	return true;
}

bool PooledMemory::Release(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || UINT64_MAX - vaddr < size) return false;

	const auto end   = vaddr + size;
	auto       first = m_mappings.upper_bound(vaddr);
	if (first == m_mappings.begin()) return false;
	--first;
	auto current = vaddr;
	for (auto it = first; it != m_mappings.end() && current < end; ++it) {
		const auto& mapping = it->second;
		if (mapping.vaddr > current || current >= mapping.vaddr + mapping.size) return false;
		current = std::min(end, mapping.vaddr + mapping.size);
	}
	if (current != end) return false;

	for (auto it = first; it != m_mappings.end() && it->first < end;) {
		const auto mapping     = it->second;
		const auto mapping_end = mapping.vaddr + mapping.size;
		const auto cut_start   = std::max(vaddr, mapping.vaddr);
		const auto cut_end     = std::min(end, mapping_end);
		it                     = m_mappings.erase(it);
		AddFreeRange(m_free, mapping.phys_addr + cut_start - mapping.vaddr, cut_end - cut_start);
		if (mapping.vaddr < vaddr) {
			m_mappings.emplace(mapping.vaddr,
			                   Mapping {mapping.vaddr, vaddr - mapping.vaddr, mapping.phys_addr});
		}
		if (end < mapping_end) {
			m_mappings.emplace(
			    end, Mapping {end, mapping_end - end, mapping.phys_addr + end - mapping.vaddr});
		}
	}
	return true;
}

uint64_t PooledMemory::Available() {
	uint64_t available = 0;
	for (const auto& [_, size]: m_free)
		available += size;
	return available;
}

// The ABI entry points and batch operations hold g_memory_operation_mutex.
static int MapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags,
                             const char* name) {
	EXIT_NOT_IMPLEMENTED(addr_in_out == nullptr);

	constexpr size_t   PAGE_SIZE                = 0x4000;
	constexpr size_t   MAXIMUM_NAME_SIZE        = 32;
	constexpr uint64_t DEFAULT_PS5_BASE         = 0x200000000;
	constexpr uint32_t GUEST_MAP_FIXED          = 0x10;
	constexpr uint32_t GUEST_MAP_NO_OVERWRITE   = 0x80;
	constexpr uint32_t GUEST_MAP_DMEM_COMPAT    = 0x400;
	constexpr uint32_t GUEST_MAP_UNKNOWN_8000   = 0x8000;
	constexpr uint32_t GUEST_MAP_NO_COALESCE    = 0x400000;
	constexpr uint32_t GUEST_MAP_ALIGNMENT_MASK = 0xff000000;
	constexpr uint32_t SUPPORTED_MAP_BITS       = GUEST_MAP_FIXED | GUEST_MAP_NO_OVERWRITE |
	                                              GUEST_MAP_DMEM_COMPAT | GUEST_MAP_UNKNOWN_8000 |
	                                              GUEST_MAP_NO_COALESCE | GUEST_MAP_ALIGNMENT_MASK;

	if (len == 0 || (len & (PAGE_SIZE - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}
	if (len > g_flexible_memory->Available()) {
		return KERNEL_ERROR_ENOMEM;
	}

	if (name == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (std::strlen(name) >= MAXIMUM_NAME_SIZE) {
		return KERNEL_ERROR_ENAMETOOLONG;
	}

	const auto map_flags       = static_cast<uint32_t>(flags);
	const auto alignment_shift = (map_flags & GUEST_MAP_ALIGNMENT_MASK) >> 24u;
	if ((map_flags & ~SUPPORTED_MAP_BITS) != 0 ||
	    (alignment_shift != 0 && (alignment_shift < 14 || alignment_shift > 31))) {
		LOGF_COLOR(Log::Color::Red, "\t unsupported flags = 0x%08" PRIx32 "\n", map_flags);
		return KERNEL_ERROR_EINVAL;
	}
	const uint64_t map_alignment =
	    alignment_shift != 0 ? uint64_t {1} << alignment_shift : PAGE_SIZE;

	VirtualMemory::Mode mode     = VirtualMemory::Mode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode)) {
		EXIT("unknown prot: %d\n", prot);
	}

	auto     in_addr         = reinterpret_cast<uint64_t>(*addr_in_out);
	uint64_t out_addr        = 0;
	bool     reserved_target = false;

	if ((flags & GUEST_MAP_FIXED) != 0) {
		if (in_addr == 0 || (in_addr & (PAGE_SIZE - 1)) != 0 ||
		    (in_addr & (map_alignment - 1u)) != 0) {
			return KERNEL_ERROR_EINVAL;
		}
		if ((flags & GUEST_MAP_NO_OVERWRITE) != 0 && g_virtual_ranges->HasOverlap(in_addr, len)) {
			return KERNEL_ERROR_ENOMEM;
		}
		std::vector<VirtualRanges::Range> reserved_ranges;
		if (g_virtual_ranges->QuerySpan(in_addr, len, &reserved_ranges) &&
		    std::all_of(reserved_ranges.begin(), reserved_ranges.end(), [](const auto& range) {
			    return range.type == VirtualRangeType::Reserved;
		    })) {
			UnmapGpuRange(in_addr, len);
			reserved_target = true;
			out_addr        = in_addr;
		}
		if (!reserved_target && ReplaceFixedRangeWithReserved(in_addr, len)) {
			reserved_target = true;
			out_addr        = in_addr;
		}
	} else {
		const auto search_addr = (in_addr != 0 ? in_addr : DEFAULT_PS5_BASE);
		out_addr               = FindGuestFreeRange(search_addr, len, map_alignment);
		if (out_addr != 0) {
			UnmapGpuRange(out_addr, len);
		}
	}

	*addr_in_out = reinterpret_cast<void*>(out_addr);

	if (out_addr == 0) {
		return KERNEL_ERROR_ENOMEM;
	}

	if (!g_flexible_memory->Map(out_addr, len, mode)) {
		LOGF_COLOR(Log::Color::Red, "\t [Fail]\n");
		return KERNEL_ERROR_ENOMEM;
	}

	const bool published = reserved_target
	                           ? g_virtual_ranges->ReplaceSpan(
	                                 out_addr, len, VirtualRangeType::Reserved, 0, prot, 0,
	                                 VirtualRangeType::Flexible, name,
	                                 (map_flags & GUEST_MAP_NO_COALESCE) != 0)
	                           : g_virtual_ranges->Add(out_addr, len, 0, prot, 0,
	                                                   VirtualRangeType::Flexible, name,
	                                                   (map_flags & GUEST_MAP_NO_COALESCE) != 0);
	if (!published) {
		EXIT_IF(!g_flexible_memory->Unmap(out_addr, len));
		return KERNEL_ERROR_EBUSY;
	}

	LOGF("\t in_addr  = 0x%016" PRIx64 "\n"
	     "\t out_addr = 0x%016" PRIx64 "\n"
	     "\t size     = %" PRIu64 "\n"
	     "\t mode     = %s\n"
	     "\t flags    = 0x%08" PRIx32 "\n"
	     "\t name     = %s\n",
	     in_addr, out_addr, len, magic_enum::enum_name(mode), static_cast<uint32_t>(flags), name);

	MapGpuRange(out_addr, len);

	if (g_alloc_callback != nullptr) {
		g_alloc_callback(out_addr, len);
	}

	return OK;
}

int32_t KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot,
                                                   int flags, const char* name) {
	PRINT_NAME();
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	return MapFlexibleMemory(addr_in_out, len, prot, flags, name);
}

int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags) {
	PRINT_NAME();
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	return MapFlexibleMemory(addr_in_out, len, prot, flags, "");
}

int KYTY_SYSV_ABI KernelSetPrtAperture(int index, void* addr, size_t len) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	const auto address = reinterpret_cast<uint64_t>(addr);

	LOGF("\t index = %d\n"
	     "\t addr  = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     index, address, static_cast<uint64_t>(len));

	if (index < 0 || index > PRT_APERTURE_MAX_INDEX) {
		return KERNEL_ERROR_EINVAL;
	}

	if (len != 0 && (address == 0 || (address & (PRT_PAGE_SIZE - 1u)) != 0 ||
	                 (len & (PRT_PAGE_SIZE - 1u)) != 0 || address < PRT_APERTURE_START ||
	                 len > PRT_APERTURE_END - address)) {
		return KERNEL_ERROR_EINVAL;
	}

	PrtAperture old {};
	{
		Common::LockGuard lock(g_prt_aperture_mutex);
		old = g_prt_apertures[static_cast<size_t>(index)];
	}
	if (old.size != 0) {
		UnmapGpuRange(old.address, old.size);
	}
	{
		Common::LockGuard lock(g_prt_aperture_mutex);
		g_prt_apertures[static_cast<size_t>(index)] =
		    len == 0 ? PrtAperture {} : PrtAperture {address, static_cast<uint64_t>(len)};
	}
	if (len != 0) {
		MapGpuRange(address, len);
	}

	LOGF_COLOR(Log::Color::Green, "\t[Ok]\n");

	return OK;
}

int KYTY_SYSV_ABI KernelGetPrtAperture(int index, void** addr, size_t* len) {
	PRINT_NAME();

	LOGF("\t index = %d\n"
	     "\t addr  = %p\n"
	     "\t len   = %p\n",
	     index, static_cast<void*>(addr), static_cast<void*>(len));

	if (index < 0 || index > PRT_APERTURE_MAX_INDEX) {
		return KERNEL_ERROR_EINVAL;
	}

	if (addr == nullptr || len == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	PrtAperture aperture {};
	{
		Common::LockGuard lock(g_prt_aperture_mutex);
		aperture = g_prt_apertures[static_cast<size_t>(index)];
	}

	*addr = reinterpret_cast<void*>(aperture.address);
	*len  = static_cast<size_t>(aperture.size);

	LOGF_COLOR(Log::Color::Green,
	           "\t *addr = 0x%016" PRIx64 "\n"
	           "\t *len  = 0x%016" PRIx64 "\n"
	           "\t[Ok]\n",
	           aperture.address, aperture.size);

	return OK;
}

int KYTY_SYSV_ABI KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	auto vaddr = reinterpret_cast<uint64_t>(addr);

	LOGF("\t addr = 0x%016" PRIx64 "\n"
	     "\t len  = 0x%016" PRIx64 "\n"
	     "\t name = %s\n",
	     vaddr, len, name != nullptr ? name : "(null)");

	if (name == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (std::strlen(name) >= KERNEL_MAXIMUM_NAME_LENGTH) {
		return KERNEL_ERROR_ENAMETOOLONG;
	}

	g_virtual_ranges->Rename(vaddr, len, name);

	return OK;
}

int KYTY_SYSV_ABI KernelClearVirtualRangeName(const void* addr, uint64_t len) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	auto vaddr = reinterpret_cast<uint64_t>(addr);

	LOGF("\t addr = 0x%016" PRIx64 "\n"
	     "\t len  = 0x%016" PRIx64 "\n",
	     vaddr, len);

	g_virtual_ranges->Rename(vaddr, len, "");

	return OK;
}

static bool FreeGuestMemoryOwner(uint64_t vaddr, uint64_t size) {
	return g_guest_address_space->ReleaseCommitted(vaddr, size) &&
	       g_virtual_ranges->Remove(vaddr, size);
}

static int UnmapMemoryOwner(const VirtualRanges::Range& range, uint64_t vaddr, size_t len,
                            bool keep_pool_reserved = false) {
	if (IsReservedRangeType(range.type)) {
		if (!g_guest_address_space->ReleaseFree(vaddr, len)) {
			return KERNEL_ERROR_EACCES;
		}
		g_virtual_ranges->Remove(vaddr, len);
		return OK;
	}
	if (range.type == VirtualRangeType::Code || range.type == VirtualRangeType::Runtime) {
		return FreeGuestMemoryOwner(vaddr, len) ? OK : KERNEL_ERROR_EACCES;
	}

	bool owner_unmapped = false;
	if (range.type == VirtualRangeType::Pooled) {
		owner_unmapped = g_guest_address_space->UnmapBacking(vaddr, len);
		if (owner_unmapped && !g_pooled_memory->Release(vaddr, len)) {
			EXIT("failed to release unmapped pooled-memory range\n");
		}
		if (!owner_unmapped) {
			return KERNEL_ERROR_EACCES;
		}
	} else if (range.type == VirtualRangeType::Direct) {
		VirtualMemory::Mode direct_mode = VirtualMemory::Mode::NoAccess;

		const auto direct_offset = range.offset + vaddr - range.start;
		if (DecodeMemoryProtection(range.protection, &direct_mode) &&
		    g_guest_address_space->BackingContains(vaddr, len) &&
		    g_guest_address_space->UnmapBacking(vaddr, len)) {
			if (g_physical_memory->Unmap(vaddr, len)) {
				owner_unmapped = true;
			} else {
				EXIT_IF(!g_guest_address_space->MapBacking(vaddr, len, direct_offset, direct_mode));
			}
		}
	} else if (range.type == VirtualRangeType::Stack) {
		owner_unmapped = g_guest_address_space->ReleaseCommitted(vaddr, len);
	} else {
		owner_unmapped = g_flexible_memory->Unmap(vaddr, len);
	}
	if (!owner_unmapped) {
		return KERNEL_ERROR_EACCES;
	}

	g_virtual_ranges->Remove(vaddr, len);
	if (keep_pool_reserved) {
		EXIT_IF(!g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved,
		                               range.name));
	}

	if (g_free_callback != nullptr && IsCommittedRangeType(range.type)) {
		g_free_callback(vaddr, len);
	}
	if (range.type == VirtualRangeType::Pooled) {
		MemoryPoolSubtractCommitted(len);
	}

	return OK;
}

static int UnmapMemoryRange(const VirtualRanges::Range& range, uint64_t vaddr, size_t len) {
	UnmapGpuRange(vaddr, len);
	const int result = UnmapMemoryOwner(range, vaddr, len);
	if (result != OK && IsCommittedRangeType(range.type)) {
		VirtualMemory::Mode mode = VirtualMemory::Mode::NoAccess;
		EXIT_IF(!DecodeMemoryProtection(range.protection, &mode));
		EXIT_IF(!g_guest_address_space->Protect(vaddr, len, mode));
		MapGpuRange(vaddr, len);
	}
	return result;
}

// The ABI entry point and batch operations hold g_memory_operation_mutex.
static int UnmapMemory(uint64_t vaddr, size_t len) {
	LOGF("\t start = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     vaddr, len);

	if (len == 0 || UINT64_MAX - vaddr < len) {
		return KERNEL_ERROR_EINVAL;
	}
	std::vector<VirtualRanges::Range> ranges;
	if (!g_virtual_ranges->QuerySpan(vaddr, len, &ranges)) {
		return KERNEL_ERROR_EACCES;
	}
	for (const auto& range: ranges) {
		const int result = UnmapMemoryRange(range, range.start, range.size);
		if (result != OK) return result;
	}
	return OK;
}

int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len) {
	PRINT_NAME();
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	return UnmapMemory(vaddr, len);
}

static bool IsAmmMemoryRange(uint64_t vaddr, size_t size) {
	constexpr uint64_t page_size = 0x4000;
	return size != 0 && ((vaddr | size) & (page_size - 1)) == 0 &&
	       vaddr >= kExtendedMemoryBase && vaddr < kExtendedMemoryBase + kExtendedMemorySize &&
	       size <= kExtendedMemoryBase + kExtendedMemorySize - vaddr;
}

int UnmapAmmMemory(uint64_t vaddr, size_t size) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	if (!IsAmmMemoryRange(vaddr, size)) return KERNEL_ERROR_EINVAL;

	const auto end = vaddr + size;
	while (vaddr < end) {
		VirtualRanges::Range range {};
		// AMM unmap accepts holes, including a completely unmapped range.
		if (!g_virtual_ranges->Query(vaddr, 1, &range) || range.start >= end) break;
		vaddr = std::max(vaddr, range.start);
		const auto chunk = std::min(end, range.start + range.size) - vaddr;
		const int result = UnmapMemoryRange(range, vaddr, chunk);
		if (result != OK) return result;
		vaddr += chunk;
	}
	return OK;
}

size_t KYTY_SYSV_ABI KernelGetDirectMemorySize() {
	PRINT_NAME();

	return PhysicalMemory::Size();
}

int KYTY_SYSV_ABI KernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                                  size_t alignment, int64_t* phys_addr_out,
                                                  size_t* size_out) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t search_start = 0x%016" PRIx64 "\n"
	     "\t search_end   = 0x%016" PRIx64 "\n"
	     "\t alignment    = 0x%016" PRIx64 "\n",
	     search_start, search_end, static_cast<uint64_t>(alignment));

	if (phys_addr_out == nullptr || size_out == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*phys_addr_out = 0;
	*size_out      = 0;

	if (search_start < 0 || search_end < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	if (search_end <= search_start) {
		LOGF_COLOR(Log::Color::Red, "\t[Fail]\n");
		return KERNEL_ERROR_ENOMEM;
	}

	uint64_t phys_addr = 0;
	uint64_t size      = 0;
	if (!g_physical_memory->Available(static_cast<uint64_t>(search_start),
	                                  static_cast<uint64_t>(search_end), alignment, &phys_addr,
	                                  &size)) {
		LOGF_COLOR(Log::Color::Red, "\t[Fail]\n");
		return KERNEL_ERROR_ENOMEM;
	}

	*phys_addr_out = static_cast<int64_t>(phys_addr);
	*size_out      = static_cast<size_t>(size);

	LOGF_COLOR(Log::Color::Green,
	           "\t phys_addr = 0x%016" PRIx64 "\n"
	           "\t size      = 0x%016" PRIx64 "\n"
	           "\t[Ok]\n",
	           phys_addr, size);

	return OK;
}

int KYTY_SYSV_ABI KernelGetPageTableStats(int* cpu_total, int* cpu_available, int* gpu_total,
                                          int* gpu_available) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	if (cpu_total == nullptr || cpu_available == nullptr || gpu_total == nullptr ||
	    gpu_available == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	const auto cpu_used =
	    (g_virtual_ranges != nullptr ? g_virtual_ranges->CountPageTableEntries(false) : 0);
	const auto gpu_used =
	    (g_virtual_ranges != nullptr ? g_virtual_ranges->CountPageTableEntries(true) : 0);

	const auto cpu_tables = g_cpu_table_memory / MAPPING_TABLE_SIZE;
	const auto gpu_tables = g_gpu_table_memory / MAPPING_TABLE_SIZE;
	*cpu_total            = static_cast<int>(cpu_tables);
	*gpu_total            = static_cast<int>(gpu_tables);
	*cpu_available        = static_cast<int>(cpu_tables - std::min(cpu_used, cpu_tables));
	*gpu_available        = static_cast<int>(gpu_tables - std::min(gpu_used, gpu_tables));

	LOGF_COLOR(Log::Color::Green,
	           "\t cpu_total     = %d\n"
	           "\t cpu_available = %d\n"
	           "\t gpu_total     = %d\n"
	           "\t gpu_available = %d\n"
	           "\t[Ok]\n",
	           *cpu_total, *cpu_available, *gpu_total, *gpu_available);

	return OK;
}

int KYTY_SYSV_ABI KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t offset    = 0x%016" PRIx64 "\n"
	     "\t flags     = 0x%08" PRIx32 "\n"
	     "\t info_size = 0x%016" PRIx64 "\n",
	     offset, flags, info_size);

	struct QueryInfo {
		int64_t start;
		int64_t end;
		int     memory_type;
	};

	if (offset < 0 || (flags != 0 && flags != 1) || info_size != sizeof(QueryInfo) ||
	    info == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto* query_info = static_cast<QueryInfo*>(info);

	PhysicalMemory::AllocatedBlock block {};
	{
		Common::LockGuard lock(g_physical_memory->GetMutex());
		const auto&       blocks  = g_physical_memory->GetPhysicalBlocks();
		auto              current = blocks.upper_bound(static_cast<uint64_t>(offset));
		if (current != blocks.begin()) {
			auto previous = std::prev(current);
			if (static_cast<uint64_t>(offset) <
			    previous->second.start_addr + previous->second.size) {
				current = previous;
			}
		}
		if (current == blocks.end() ||
		    (flags == 0 && (static_cast<uint64_t>(offset) < current->second.start_addr ||
		                    static_cast<uint64_t>(offset) >=
		                        current->second.start_addr + current->second.size))) {
			if (flags == 1 && static_cast<uint64_t>(offset) < PhysicalMemory::Size()) {
				query_info->start       = static_cast<int64_t>(PhysicalMemory::Size());
				query_info->end         = static_cast<int64_t>(PhysicalMemory::Size());
				query_info->memory_type = 0;
				LOGF_COLOR(Log::Color::Green, "\t terminal    = true\n\t[Ok]\n");
				return OK;
			}

			LOGF_COLOR(Log::Color::Red, "\t[Fail]\n");
			return KERNEL_ERROR_EACCES;
		}

		block        = current->second;
		uint64_t end = block.start_addr + block.size;
		for (auto following = std::next(current);
		     following != blocks.end() && following->second.start_addr == end &&
		     following->second.memory_type == block.memory_type;
		     ++following) {
			end += following->second.size;
		}
		block.size = end - block.start_addr;
	}

	query_info->start       = static_cast<int64_t>(block.start_addr);
	query_info->end         = static_cast<int64_t>(block.start_addr + block.size);
	query_info->memory_type = block.memory_type;

	LOGF_COLOR(Log::Color::Green,
	           "\t start       = %016" PRIx64 "\n"
	           "\t end         = %016" PRIx64 "\n"
	           "\t memory_type = %d\n"
	           "\t[Ok]\n",
	           query_info->start, query_info->end, query_info->memory_type);

	return OK;
}

int AllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment,
                         int memory_type, int64_t* phys_addr_out, bool automatic) {

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t search_start = 0x%016" PRIx64 "\n"
	     "\t search_end   = 0x%016" PRIx64 "\n"
	     "\t len          = 0x%016" PRIx64 "\n"
	     "\t alignment    = 0x%016" PRIx64 "\n"
	     "\t memory_type  = %d\n",
	     search_start, search_end, len, alignment, memory_type);

	constexpr uint64_t PAGE_SIZE = 0x4000;
	if (search_start < 0 || search_end <= search_start || len == 0 ||
	    (len & (PAGE_SIZE - 1u)) != 0 || (alignment != 0 && (alignment & (PAGE_SIZE - 1u)) != 0) ||
	    phys_addr_out == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t addr = 0;
	const auto kind = automatic ? PhysicalMemory::AllocationKind::Automatic
	                            : PhysicalMemory::AllocationKind::Direct;
	if (!g_physical_memory->Alloc(search_start, search_end, len, alignment, &addr, memory_type,
	                              kind)) {
		LOGF_COLOR(Log::Color::Red, "\t[Fail]\n");
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	LOGF_COLOR(Log::Color::Green, "\tphys_addr    = %016" PRIx64 "\n\t[Ok]\n", addr);

	return OK;
}

int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                             size_t alignment, int memory_type,
                                             int64_t* phys_addr_out) {
	PRINT_NAME();
	return AllocateDirectMemory(search_start, search_end, len, alignment, memory_type,
	                            phys_addr_out);
}

int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type,
                                                 int64_t* phys_addr_out) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t len          = 0x%016" PRIx64 "\n"
	     "\t alignment    = 0x%016" PRIx64 "\n"
	     "\t memory_type  = %d\n",
	     len, alignment, memory_type);

	return KernelAllocateDirectMemory(0, static_cast<int64_t>(PhysicalMemory::Size()), len,
	                                  alignment, memory_type, phys_addr_out);
}

static int ReleaseDirectMemoryInternal(int64_t start, size_t len, bool skip_gaps) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	PhysicalMemory::AllocatedBlock allocation {};
	if (g_physical_memory->Find(static_cast<uint64_t>(start), &allocation) &&
	    allocation.start_addr == static_cast<uint64_t>(start) && allocation.size == len &&
	    allocation.kind == PhysicalMemory::AllocationKind::Pooled) {
		if (!g_pooled_memory->ReleaseExpansion(static_cast<uint64_t>(start), len)) {
			return KERNEL_ERROR_EACCES;
		}
		if (!g_physical_memory->ReleasePoolExpansion(static_cast<uint64_t>(start), len)) {
			EXIT("failed to release physical pool expansion\n");
		}
		return OK;
	}

	std::vector<PhysicalMemory::AllocatedBlock> allocated_span;
	if (!g_physical_memory->GetAllocatedSpan(static_cast<uint64_t>(start), len, skip_gaps,
	                                         &allocated_span)) {
		return KERNEL_ERROR_EACCES;
	}

	const auto mapped_aliases = g_physical_memory->FindMappings(start, len);
	for (const auto& alias: mapped_aliases) {
		if (!g_guest_address_space->BackingContains(alias.start, alias.size)) {
			EXIT("direct-memory alias escaped guest address-space ownership\n");
		}
	}

	for (const auto& alias: mapped_aliases) {
		UnmapGpuRange(alias.start, alias.size);
	}

	auto restore_gpu_aliases = [&mapped_aliases]() {
		for (const auto& alias: mapped_aliases) {
			MapGpuRange(alias.start, alias.size);
		}
	};
	auto restore_owner_aliases = [](std::span<const VirtualRanges::Range> aliases) {
		for (auto it = aliases.rbegin(); it != aliases.rend(); ++it) {
			VirtualMemory::Mode mode = VirtualMemory::Mode::NoAccess;

			EXIT_IF(!DecodeMemoryProtection(it->protection, &mode));
			EXIT_IF(!g_guest_address_space->MapBacking(it->start, it->size, it->offset, mode));
		}
	};

	size_t owner_unmapped = 0;
	for (const auto& alias: mapped_aliases) {
		if (!g_guest_address_space->UnmapBacking(alias.start, alias.size)) {
			restore_owner_aliases(std::span(mapped_aliases).first(owner_unmapped));
			restore_gpu_aliases();
			return KERNEL_ERROR_EACCES;
		}
		++owner_unmapped;
	}

	size_t metadata_unmapped = 0;
	for (const auto& alias: mapped_aliases) {
		if (!g_physical_memory->Unmap(alias.start, alias.size)) {
			for (const auto& removed: std::span(mapped_aliases).first(metadata_unmapped)) {
				EXIT_IF(!g_physical_memory->Map(removed.start, removed.offset, removed.size));
			}
			restore_owner_aliases(std::span(mapped_aliases).first(owner_unmapped));
			restore_gpu_aliases();
			return KERNEL_ERROR_EACCES;
		}
		++metadata_unmapped;
	}

	for (const auto& alias: mapped_aliases) {
		EXIT_IF(!g_virtual_ranges->Remove(alias.start, alias.size));
	}

	for (const auto& block: allocated_span) {
		EXIT_IF(!g_physical_memory->Release(block.start_addr, block.size));
	}

	if (g_free_callback != nullptr) {
		for (const auto& alias: mapped_aliases) {
			g_free_callback(alias.start, alias.size);
		}
	}

	return OK;
}

static int ValidateDirectReleaseRange(int64_t start, size_t len) {
	constexpr uint64_t PAGE_SIZE = 0x4000;
	return start < 0 || (static_cast<uint64_t>(start) & (PAGE_SIZE - 1u)) != 0 ||
	               (len & (PAGE_SIZE - 1u)) != 0
	           ? KERNEL_ERROR_EINVAL
	           : OK;
}

int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len) {
	PRINT_NAME();

	LOGF("\t start = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     start, len);

	const int validation = ValidateDirectReleaseRange(start, len);
	if (validation != OK) {
		return validation;
	}
	if (len != 0) {
		// An unchecked release frees the allocated parts and skips the unallocated ones.
		(void)ReleaseDirectMemoryInternal(start, len, true);
	}
	return OK;
}

int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len) {
	PRINT_NAME();

	LOGF("\t start = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     start, len);

	const int validation = ValidateDirectReleaseRange(start, len);
	if (validation != OK || len == 0) {
		return validation;
	}

	const int result = ReleaseDirectMemoryInternal(start, len, false);
	return result == KERNEL_ERROR_EACCES ? KERNEL_ERROR_ENOENT : result;
}

// The ABI entry points and batch operations hold g_memory_operation_mutex.
static int MapDirectMemory(void** addr, size_t len, int prot, int flags,
                           int64_t direct_memory_start, size_t alignment, const char* name,
                           int type = -1) {
	if (addr == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	constexpr uint64_t PAGE_SIZE              = 0x4000;
	constexpr int      GUEST_MAP_FIXED        = 0x10;
	constexpr int      GUEST_MAP_NO_OVERWRITE = 0x80;

	if (len == 0 || (len & (PAGE_SIZE - 1u)) != 0 || direct_memory_start < 0 ||
	    (static_cast<uint64_t>(direct_memory_start) & (PAGE_SIZE - 1u)) != 0 ||
	    (alignment != 0 && (alignment & (alignment - 1u)) != 0 &&
	     (alignment & (PAGE_SIZE - 1u)) != 0)) {
		return KERNEL_ERROR_EINVAL;
	}
	if ((prot & PROT_CPU_EXEC) != 0) {
		return KERNEL_ERROR_EACCES;
	}

	bool fixed        = ((flags & GUEST_MAP_FIXED) != 0);
	bool no_overwrite = ((flags & GUEST_MAP_NO_OVERWRITE) != 0);

	VirtualMemory::Mode mode     = VirtualMemory::Mode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode)) {
		return KERNEL_ERROR_EINVAL;
	}
	if (!g_physical_memory->CanMapDirect(static_cast<uint64_t>(direct_memory_start), len)) {
		return KERNEL_ERROR_ENOMEM;
	}

	auto     in_addr         = reinterpret_cast<uint64_t>(*addr);
	uint64_t out_addr        = 0;
	bool     shared_backing  = false;
	bool     reserved_target = false;
	auto     shared_failure  = GuestBackingStore::FailureReason::None;
	// Direct mappings must remain views of the single backing object. Anonymous fallbacks break
	// aliasing and lose direct-memory contents when a range is unmapped and mapped again.
	auto map_shared_fixed = [&](uint64_t target_addr) -> bool {
		return g_guest_address_space->MapBacking(target_addr, len, direct_memory_start, mode,
		                                         &shared_failure);
	};
	std::vector<VirtualRanges::Range> replaced_mappings;
	auto restore_mappings = [&](size_t count, bool restore_views) {
		for (size_t index = 0; index < count; ++index) {
			const auto& old = replaced_mappings[index];
			if (restore_views) {
				VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;

				EXIT_IF(!DecodeMemoryProtection(old.protection, &old_mode));
				EXIT_IF(
				    !g_guest_address_space->MapBacking(old.start, old.size, old.offset, old_mode));
			}
			EXIT_IF(!g_physical_memory->Map(old.start, old.offset, old.size));
		}
		MapGpuRange(in_addr, len);
	};
	if (fixed) {
		if (in_addr == 0 || (in_addr & (PAGE_SIZE - 1u)) != 0 ||
		    (alignment != 0 && in_addr % alignment != 0)) {
			return KERNEL_ERROR_EINVAL;
		}
		if (no_overwrite && g_virtual_ranges->HasOverlap(in_addr, len)) {
			return KERNEL_ERROR_ENOMEM;
		}

		std::vector<VirtualRanges::Range> reserved_ranges;
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		if (g_virtual_ranges->QuerySpan(in_addr, len, &reserved_ranges) &&
		    std::all_of(reserved_ranges.begin(), reserved_ranges.end(), [](const auto& range) {
			    return range.type == VirtualRangeType::Direct;
		    })) {
			replaced_mappings = std::move(reserved_ranges);
			UnmapGpuRange(in_addr, len);
			for (size_t index = 0; index < replaced_mappings.size(); ++index) {
				const auto& old = replaced_mappings[index];
				if (!g_physical_memory->Unmap(old.start, old.size)) {
					restore_mappings(index, false);
					return KERNEL_ERROR_EBUSY;
				}
			}
			// Keep the old view accessible until mmap atomically replaces it. In particular,
			// do not publish a temporary reserved/PROT_NONE range between two direct views.
			if (!map_shared_fixed(in_addr)) {
				restore_mappings(replaced_mappings.size(), true);
				return KERNEL_ERROR_ENOMEM;
			}
			out_addr       = in_addr;
			shared_backing = true;
		} else
#endif
		{
			if (g_virtual_ranges->QuerySpan(in_addr, len, &reserved_ranges) &&
			    std::all_of(reserved_ranges.begin(), reserved_ranges.end(), [](const auto& range) {
				    return range.type == VirtualRangeType::Reserved;
			    })) {
				UnmapGpuRange(in_addr, len);
				reserved_target = true;
			}
			if (!reserved_target && ReplaceFixedRangeWithReserved(in_addr, len)) {
				reserved_target = true;
			}
			if (!reserved_target) {
				return KERNEL_ERROR_ENOMEM;
			}
			if (map_shared_fixed(in_addr)) {
				out_addr       = in_addr;
				shared_backing = true;
			}
		}
	} else {
		constexpr size_t DEFAULT_ALIGNMENT = 0x4000;
		alignment                          = (alignment != 0 ? alignment : DEFAULT_ALIGNMENT);
		std::vector<VirtualRanges::Range> reserved_ranges;
		if (in_addr != 0 && g_virtual_ranges->QuerySpan(in_addr, len, &reserved_ranges) &&
		    std::all_of(reserved_ranges.begin(), reserved_ranges.end(), [](const auto& range) {
			    return range.type == VirtualRangeType::Reserved;
		    })) {
			UnmapGpuRange(in_addr, len);
			reserved_target = true;
			if (map_shared_fixed(in_addr)) {
				out_addr       = in_addr;
				shared_backing = true;
			}
		}
		if (!reserved_target) {
			out_addr = FindGuestFreeRange(in_addr, len, alignment);
			if (out_addr != 0) {
				UnmapGpuRange(out_addr, len);
				shared_backing = map_shared_fixed(out_addr);
				if (!shared_backing) {
					out_addr = 0;
				}
			}
		}
	}

	*addr = reinterpret_cast<void*>(out_addr);

	const char* shared_reason = "n/a";
	if (!shared_backing) {
		shared_reason = GuestBackingStore::GetFailureReasonName(shared_failure);
	}

	LOGF("\t in_addr  = 0x%016" PRIx64 "\n"
	     "\t out_addr = 0x%016" PRIx64 "\n"
	     "\t dmem     = 0x%016" PRIx64 "\n"
	     "\t size     = 0x%016" PRIx64 "\n"
	     "\t mode     = %s\n"
	     "\t flags    = 0x%08" PRIx32 "\n"
	     "\t align    = 0x%016" PRIx64 "\n"
	     "\t shared   = %s\n"
	     "\t reason   = %s\n",
	     in_addr, out_addr, static_cast<uint64_t>(direct_memory_start), len,
	     magic_enum::enum_name(mode), static_cast<uint32_t>(flags), alignment,
	     shared_backing ? "yes" : "no", shared_reason);

	if (out_addr == 0) {
		return KERNEL_ERROR_ENOMEM;
	}

	PhysicalMemory::AllocatedBlock mapped_block {};
	g_physical_memory->Find(direct_memory_start, &mapped_block);
	const int memory_type = type == -1 ? mapped_block.memory_type : type;
	if (!g_physical_memory->Map(out_addr, direct_memory_start, len)) {
		LOGF_COLOR(Log::Color::Red, "\t [Fail]\n");
		if (replaced_mappings.empty()) {
			EXIT_IF(!g_guest_address_space->UnmapBacking(out_addr, len));
		} else {
			restore_mappings(replaced_mappings.size(), true);
		}
		return KERNEL_ERROR_EBUSY;
	}

	const bool published = !replaced_mappings.empty() || reserved_target
	                           ? g_virtual_ranges->ReplaceSpan(
	                                 out_addr, len, replaced_mappings.empty()
	                                                    ? VirtualRangeType::Reserved
	                                                    : VirtualRangeType::Direct,
	                                 direct_memory_start,
	                                 prot, memory_type, VirtualRangeType::Direct, name)
	                           : g_virtual_ranges->Add(out_addr, len, direct_memory_start, prot,
	                                                   memory_type,
	                                                   VirtualRangeType::Direct, name);
	if (!published) {
		EXIT_IF(!g_physical_memory->Unmap(out_addr, len));
		if (replaced_mappings.empty()) {
			EXIT_IF(!g_guest_address_space->UnmapBacking(out_addr, len));
		} else {
			restore_mappings(replaced_mappings.size(), true);
		}
		return KERNEL_ERROR_EBUSY;
	}

	MapGpuRange(out_addr, len);

	if (g_alloc_callback != nullptr) {
		g_alloc_callback(out_addr, len);
	}

	LOGF_COLOR(Log::Color::Green, "\t [Ok]\n");

	return OK;
}

int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        int64_t direct_memory_start, size_t alignment) {
	PRINT_NAME();
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	return MapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment, "");
}

int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags,
                                         int64_t direct_memory_start, size_t alignment) {
	PRINT_NAME();
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	return MapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment, "", type);
}

int MapAutomaticMemory(uint64_t vaddr, size_t size, int type, int prot) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	constexpr uint64_t                    page_size = 0x4000;
	if (!IsAmmMemoryRange(vaddr, size)) return KERNEL_ERROR_EINVAL;

	PhysicalMemory::PhysicalRanges ranges;
	if (!g_physical_memory->ReserveAutomatic(size, &ranges)) {
		return KERNEL_ERROR_EAGAIN;
	}
	uint64_t mapped = 0;
	for (const auto& [offset, length]: ranges) {
		void*     address = reinterpret_cast<void*>(vaddr + mapped);
		const int result  = MapDirectMemory(&address, length, prot, 0x10,
		                                    static_cast<int64_t>(offset), page_size, "", type);
		if (result != OK) {
			if (mapped != 0) {
				EXIT_IF(UnmapMemory(vaddr, mapped) != OK);
			}
			g_physical_memory->RestoreAutomatic(ranges);
			return result;
		}
		mapped += length;
	}
	return OK;
}

int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags,
                                             int64_t direct_memory_start, size_t alignment,
                                             const char* name) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t name = %s\n", name != nullptr ? name : "(null)");

	if (name == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (std::strlen(name) >= KERNEL_MAXIMUM_NAME_LENGTH) {
		return KERNEL_ERROR_ENAMETOOLONG;
	}

	return MapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment, name);
}

int KYTY_SYSV_ABI KernelIsAddressSanitizerEnabled() {
	PRINT_NAME();

	return 0;
}

int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);

	VirtualRanges::Range range {};
	if (!g_virtual_ranges->Query(reinterpret_cast<uint64_t>(addr), 0, &range)) {
		return KERNEL_ERROR_EACCES;
	}

	if (start != nullptr) {
		*start = reinterpret_cast<void*>(range.start);
	}
	if (end != nullptr) {
		*end = reinterpret_cast<void*>(range.start + range.size);
	}
	if (prot != nullptr) {
		*prot = range.protection;
	}

	return OK;
}

static bool ReplaceFixedRangeWithReserved(uint64_t start, uint64_t size) {

	struct ReplacedChunk {
		VirtualRanges::Range                        range {};
		VirtualMemory::Mode                         mode     = VirtualMemory::Mode::NoAccess;

		std::vector<FlexibleMemory::Mapping>        flexible_blocks;
		bool                                        host_unmapped    = false;
		bool                                        backend_unmapped = false;
	};

	std::vector<ReplacedChunk> chunks;
	const auto                 end     = start + size;
	auto                       current = start;

	while (current < end) {
		VirtualRanges::Range range {};
		if (!g_virtual_ranges->Query(current, 1, &range)) {
			break;
		}
		if (range.start >= end) {
			break;
		}
		if (current < range.start) {
			current = std::min<uint64_t>(end, range.start);
			continue;
		}

		const auto range_end = range.start + range.size;
		const auto chunk     = std::min<uint64_t>(end, range_end) - current;

		ReplacedChunk replaced {};
		replaced.range       = range;
		replaced.range.start = current;
		replaced.range.size  = chunk;
		if (range.type == VirtualRangeType::Direct) {
			replaced.range.offset += current - range.start;
		}
		DecodeMemoryProtection(replaced.range.protection, &replaced.mode);
		if (range.type == VirtualRangeType::Direct &&
		    !g_guest_address_space->BackingContains(current, chunk)) {
			return false;
		}
		if (range.type == VirtualRangeType::Flexible &&
		    !g_flexible_memory->Snapshot(current, chunk, &replaced.flexible_blocks)) {
			return false;
		}
		if (range.type == VirtualRangeType::Pooled) {
			EXIT("reserve-fixed replacement of pooled memory is unsupported: addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     replaced.range.start, replaced.range.size);
		}
		chunks.push_back(std::move(replaced));

		current += chunk;
	}

	auto restore_chunks = [&chunks]() -> bool {
		bool ok = true;
		for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
			const auto& chunk            = *it;
			bool        host_restored    = true;
			bool        backend_restored = true;

			if (chunk.range.type == VirtualRangeType::Direct) {
				if (chunk.host_unmapped) {
					host_restored = g_guest_address_space->MapBacking(
					    chunk.range.start, chunk.range.size, chunk.range.offset, chunk.mode);
				}
				if (chunk.backend_unmapped) {
					backend_restored = g_physical_memory->Map(chunk.range.start, chunk.range.offset,
					                                          chunk.range.size);
				}
			} else if (chunk.range.type == VirtualRangeType::Flexible && chunk.backend_unmapped) {
				host_restored = backend_restored =
				    g_flexible_memory->Restore(chunk.flexible_blocks, chunk.mode);
			} else if (IsPrivateCommittedRangeType(chunk.range.type) && chunk.host_unmapped) {
				host_restored =
				    g_guest_address_space->Commit(chunk.range.start, chunk.range.size, chunk.mode);
			}
			ok = host_restored && backend_restored && ok;
		}

		for (const auto& chunk: chunks) {
			const bool range_restored = g_virtual_ranges->Add(
			    chunk.range.start, chunk.range.size, chunk.range.offset, chunk.range.protection,
			    chunk.range.memory_type, chunk.range.type, chunk.range.name,
			    chunk.range.disallow_merge);
			ok = range_restored && ok;
		}
		if (ok) {
			for (const auto& chunk: chunks) {
				if (IsCommittedRangeType(chunk.range.type)) {
					MapGpuRange(chunk.range.start, chunk.range.size);
				}
			}
		}
		return ok;
	};

	UnmapGpuRange(start, size);
	g_virtual_ranges->Remove(start, size);

	for (auto& chunk: chunks) {
		bool unmapped = true;

		if (chunk.range.type == VirtualRangeType::Direct) {
			if (!g_guest_address_space->UnmapBacking(chunk.range.start, chunk.range.size)) {
				unmapped = false;
			} else {
				chunk.host_unmapped = true;
			}
			if (unmapped) {
				unmapped = g_physical_memory->Unmap(chunk.range.start, chunk.range.size);
				chunk.backend_unmapped = unmapped;
			}
		} else if (chunk.range.type == VirtualRangeType::Flexible) {
			unmapped               = g_flexible_memory->Unmap(chunk.range.start, chunk.range.size);
			chunk.host_unmapped    = unmapped;
			chunk.backend_unmapped = unmapped;
		} else if (IsPrivateCommittedRangeType(chunk.range.type)) {
			unmapped = g_guest_address_space->ReleaseCommitted(chunk.range.start, chunk.range.size);
			chunk.host_unmapped    = unmapped;
			chunk.backend_unmapped = unmapped;
		} else if (IsReservedRangeType(chunk.range.type)) {
			unmapped = g_guest_address_space->ReleaseFree(chunk.range.start, chunk.range.size);
		} else {
			unmapped = false;
		}
		if (!unmapped) {
			LOGF_COLOR(Log::Color::Red,
			           "\t reserve-fixed replace: backend unmap failed at 0x%016" PRIx64
			           ", size=0x%016" PRIx64 ", type=%s\n",
			           chunk.range.start, chunk.range.size,
			           magic_enum::enum_name(chunk.range.type));
			if (!restore_chunks()) {
				EXIT("reserve-fixed backend-unmap rollback failed\n");
			}
			return false;
		}
	}

	if (!g_guest_address_space->ReserveFixed(start, size)) {
		if (!restore_chunks()) {
			EXIT("reserve-fixed host-reservation rollback failed\n");
		}
		return false;
	}

	bool range_added = false;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_fail_next_fixed_reserve_range_add) {
		g_test_fail_next_fixed_reserve_range_add = false;
	} else
#endif
	{
		range_added =
		    g_virtual_ranges->Add(start, size, 0, 0, 0, VirtualRangeType::Reserved, "anon");
	}
	if (!range_added) {
		LOGF_COLOR(Log::Color::Red,
		           "\t reserve-fixed replace: range add failed at 0x%016" PRIx64
		           ", size=0x%016" PRIx64 "\n",
		           start, size);
		if (!restore_chunks()) {
			EXIT("reserve-fixed range-registration rollback failed\n");
		}
		auto free_start = start;
		for (const auto& chunk: chunks) {
			if (free_start < chunk.range.start &&
			    !g_guest_address_space->ReleaseFree(free_start, chunk.range.start - free_start)) {
				EXIT("reserve-fixed range-registration gap cleanup failed\n");
			}
			free_start = chunk.range.start + chunk.range.size;
		}
		if (free_start < start + size &&
		    !g_guest_address_space->ReleaseFree(free_start, start + size - free_start)) {
			EXIT("reserve-fixed range-registration tail cleanup failed\n");
		}
		return false;
	}

	for (const auto& chunk: chunks) {
		if (g_free_callback != nullptr && IsCommittedRangeType(chunk.range.type)) {
			g_free_callback(chunk.range.start, chunk.range.size);
		}
	}

	return true;
}

int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	const auto in_addr = (addr != nullptr ? reinterpret_cast<uint64_t>(*addr) : 0);

	LOGF("\t in_addr   = 0x%016" PRIx64 "\n"
	     "\t len       = 0x%016" PRIx64 "\n"
	     "\t flags     = 0x%08" PRIx32 "\n"
	     "\t alignment = 0x%016" PRIx64 "\n",
	     in_addr, len, flags, alignment);

	constexpr size_t PAGE_SIZE              = 0x4000;
	constexpr int    GUEST_MAP_FIXED        = 0x10;
	constexpr int    GUEST_MAP_NO_OVERWRITE = 0x80;

	if (addr == nullptr || len == 0 || (len & (PAGE_SIZE - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}
	if (alignment != 0 && (alignment & (alignment - 1)) != 0 &&
	    (alignment & (PAGE_SIZE - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t out_addr            = 0;
	bool     range_already_added = false;
	if ((flags & GUEST_MAP_FIXED) != 0) {
		if (in_addr == 0 || (in_addr & (PAGE_SIZE - 1)) != 0) {
			return KERNEL_ERROR_EINVAL;
		}
		if ((flags & GUEST_MAP_NO_OVERWRITE) != 0 && g_virtual_ranges->HasOverlap(in_addr, len)) {
			return KERNEL_ERROR_ENOMEM;
		}
		if (ReplaceFixedRangeWithReserved(in_addr, len)) {
			out_addr            = in_addr;
			range_already_added = true;
		}
	} else {
		alignment = (alignment != 0 ? alignment : PAGE_SIZE);
		out_addr  = FindGuestFreeRange(in_addr, len, alignment);
		if (out_addr != 0) {
			UnmapGpuRange(out_addr, len);
		}
	}

	if (out_addr == 0) {
		return KERNEL_ERROR_ENOMEM;
	}

	if (!range_already_added &&
	    !g_virtual_ranges->Add(out_addr, len, 0, 0, 0, VirtualRangeType::Reserved, "anon")) {
		return KERNEL_ERROR_EBUSY;
	}

	*addr = reinterpret_cast<void*>(out_addr);

	LOGF("\t out_addr  = 0x%016" PRIx64 "\n", out_addr);

	return OK;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void TestSetGpuRangeCallback(void (*callback)(bool, uint64_t, size_t)) {
	g_test_gpu_range = callback;
}

void TestBeforeNextBackingMap(callback_func_t callback) {
	g_test_before_backing_map = callback;
}

void TestSetBackingReadCallback(callback_func_t callback) {
	g_test_backing_read = callback;
}

void TestFailNextPhysicalMemoryUnmap() {
	TestFailPhysicalMemoryUnmapAfter(0);
}

void TestFailPhysicalMemoryUnmapAfter(uint32_t successful_unmaps) {
	g_test_physical_memory_unmaps_before_failure = successful_unmaps;
}

void TestFailGuestBackingStoreUnmapAfter(uint32_t successful_unmaps) {
	g_test_backing_store_unmaps_before_failure = successful_unmaps;
}

void TestFailNextFixedReserveRangeRegistration() {
	g_test_fail_next_fixed_reserve_range_add = true;
}

void TestFailNextVirtualRangeReplacement() {
	g_test_fail_next_range_replace = true;
}

bool TestPlaceholderRangeIsFree(uint64_t vaddr, uint64_t size) {
	return g_guest_address_space->TestContainsFree(vaddr, size);
}

bool TestGuestAddressRangeIsOwned(uint64_t vaddr, uint64_t size) {
	return g_guest_address_space->Owns(vaddr, size);
}

bool TestGuestBackingOutsideAddressSpace() {
	return !g_guest_address_space->OverlapsOwned(g_guest_address_space->GetBackingBase(),
	                                             g_guest_address_space->GetBackingSize());
}

uint64_t TestGuestFreeRangeVisits() {
	return g_test_free_range_visits.load(std::memory_order_relaxed);
}

uint64_t TestDirectMappingCount() {
	std::lock_guard lock(g_memory_operation_mutex);
	return g_physical_memory->GetMappings().size();
}

uint64_t TestFlexibleMappingCount() {
	std::lock_guard lock(g_memory_operation_mutex);
	return g_flexible_memory->GetMappings().size();
}

uint64_t TestFlexibleBackingHighWater() {
	std::lock_guard lock(g_memory_operation_mutex);
	return g_flexible_memory->TestBackingHighWater();
}

uint64_t TestBackingBytesZeroed() {
	std::lock_guard lock(g_memory_operation_mutex);
	return g_test_backing_bytes_zeroed;
}

uint64_t TestGuestBackingSize() {
	return g_guest_address_space->GetBackingSize();
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool TestWindowsBackingViewModes() {
	constexpr uint64_t size = 0x10000;
	GuestBackingStore backing(size);
	void* placeholder = VirtualAlloc2(GetCurrentProcess(), nullptr, size,
	                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
	                                  nullptr, 0);
	if (placeholder == nullptr) {
		return false;
	}
	const auto address = reinterpret_cast<uint64_t>(placeholder);
	bool valid = true;
	for (auto [mode, protection]: std::array {
	         std::pair {VirtualMemory::Mode::NoAccess, DWORD {PAGE_NOACCESS}},
	         std::pair {VirtualMemory::Mode::Read, DWORD {PAGE_READONLY}},
	         std::pair {VirtualMemory::Mode::ReadWrite, DWORD {PAGE_READWRITE}},
	         std::pair {VirtualMemory::Mode::ExecuteReadWrite, DWORD {PAGE_EXECUTE_READWRITE}}}) {
		if (!backing.MapFixed(address, size, 0, mode)) {
			valid = false;
			break;
		}
		MEMORY_BASIC_INFORMATION info {};
		valid = VirtualQuery(placeholder, &info, sizeof(info)) != 0 &&
		        info.Type == MEM_MAPPED && info.Protect == protection && valid;
		EXIT_IF(!backing.Unmap(address, size));
	}
	return VirtualFree(placeholder, 0, MEM_RELEASE) != 0 && valid;
}
#endif

bool TestGuestFreeRangeBounds() {
	return GuestFreeRangeContains(0x10000, 0x20000, 0x18000, 0x4000) &&
	       !GuestFreeRangeContains(0x10000, 0x20000, 0x40000, 0x4000) &&
	       !GuestFreeRangeContains(UINT64_MAX - 0x1000, 0x2000, UINT64_MAX - 0x800, 0x400);
}
#endif

int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info,
                                     uint64_t info_size) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	auto vaddr = reinterpret_cast<uint64_t>(addr);

	LOGF("\t addr      = 0x%016" PRIx64 "\n"
	     "\t flags     = 0x%08" PRIx32 "\n"
	     "\t info_size = 0x%016" PRIx64 "\n",
	     vaddr, flags, info_size);

	if (info == nullptr || info_size != sizeof(VirtualQueryInfo) || (flags != 0 && flags != 1)) {
		return KERNEL_ERROR_EINVAL;
	}

	VirtualRanges::Range candidate {};
	if (!g_virtual_ranges->Query(vaddr, flags, &candidate)) {
		return KERNEL_ERROR_EACCES;
	}

	std::memset(info, 0, sizeof(VirtualQueryInfo));
	info->start        = candidate.start;
	info->end          = candidate.start + candidate.size;
	info->offset       = candidate.offset;
	info->protection   = candidate.protection;
	info->memory_type  = candidate.memory_type;
	info->is_flexible  = (candidate.type == VirtualRangeType::Flexible ? 1 : 0);
	info->is_direct    = (candidate.type == VirtualRangeType::Direct ? 1 : 0);
	info->is_stack     = (candidate.type == VirtualRangeType::Stack ? 1 : 0);
	info->is_pooled    = (IsPooledRangeType(candidate.type) ? 1 : 0);
	info->is_committed = (IsCommittedRangeType(candidate.type) ? 1 : 0);
	info->is_gpu_prt   = IsInPrtAperture(vaddr);
	CopyVirtualRangeName(info->name, candidate.name);

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1) < 64) {
		LOGF("\t start       = 0x%016" PRIx64 "\n"
		     "\t end         = 0x%016" PRIx64 "\n"
		     "\t offset      = 0x%016" PRIx64 "\n"
		     "\t protection  = 0x%08" PRIx32 "\n"
		     "\t memory_type = %d\n"
		     "\t flexible    = %d\n"
		     "\t direct      = %d\n"
		     "\t name        = %s\n",
		     static_cast<uint64_t>(info->start), static_cast<uint64_t>(info->end), info->offset,
		     info->protection, info->memory_type, static_cast<int>(info->is_flexible),
		     static_cast<int>(info->is_direct), info->name);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelIsStack(void* addr, void** start, void** end) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	auto vaddr = reinterpret_cast<uint64_t>(addr);

	LOGF("\t addr = 0x%016" PRIx64 "\n", vaddr);

	VirtualRanges::Range candidate {};
	if (!g_virtual_ranges->Query(vaddr, 0, &candidate)) {
		return KERNEL_ERROR_EACCES;
	}

	uint64_t stack_start = 0;
	uint64_t stack_end   = 0;
	if (candidate.type == VirtualRangeType::Stack) {
		stack_start = candidate.start;
		stack_end   = candidate.start + candidate.size;
	}

	if (start != nullptr) {
		*start = reinterpret_cast<void*>(stack_start);
	}

	if (end != nullptr) {
		*end = reinterpret_cast<void*>(stack_end);
	}

	LOGF("\t start = 0x%016" PRIx64 "\n"
	     "\t end   = 0x%016" PRIx64 "\n",
	     stack_start, stack_end);

	return OK;
}

int KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	if (size == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*size = g_flexible_memory->Available();

	LOGF("\t *size = 0x%016" PRIx64 "\n", *size);

	return OK;
}

int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(size_t* size) {
	PRINT_NAME();

	if (size == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	*size = FlexibleMemory::Size();

	LOGF("\t *size = 0x%016" PRIx64 "\n", *size);

	return OK;
}

static int ProgramProtection(VirtualMemory::Mode mode) {
	const auto protection = static_cast<int>(mode);
	if ((protection & ~(PROT_CPU_READ | PROT_CPU_WRITE | PROT_CPU_EXEC)) != 0) {
		EXIT("unsupported program-memory protection: 0x%08x\n", protection);
	}
	return protection;
}

static std::vector<VirtualRanges::Range> RequireGuestRuntimeMemory(uint64_t vaddr, uint64_t size) {
	std::vector<VirtualRanges::Range> ranges;
	if (g_virtual_ranges == nullptr || !g_virtual_ranges->QuerySpan(vaddr, size, &ranges) ||
	    std::any_of(ranges.begin(), ranges.end(), [](const auto& range) {
		    return range.type != VirtualRangeType::Code && range.type != VirtualRangeType::Runtime;
	    })) {
		EXIT("guest runtime range is not fully mapped: addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     vaddr, size);
	}
	return ranges;
}

static uint64_t AllocateGuestRuntimeMemory(uint64_t search_addr, uint64_t size,
                                           VirtualMemory::Mode mode, const char* name,
                                           VirtualRangeType type, bool fixed) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	constexpr uint64_t GuestPageSize = 0x4000;
	if (size == 0 || size > UINT64_MAX - (GuestPageSize - 1u) || name == nullptr ||
	    (fixed && (search_addr == 0 || (search_addr & (GuestPageSize - 1u)) != 0))) {
		return 0;
	}
	const auto mapped_size = (size + GuestPageSize - 1u) & ~(GuestPageSize - 1u);
	const auto vaddr =
	    fixed ? search_addr : FindGuestFreeRange(search_addr, mapped_size, GuestPageSize);
	if (vaddr == 0 || g_virtual_ranges->HasOverlap(vaddr, mapped_size)) {
		return 0;
	}
	UnmapGpuRange(vaddr, mapped_size);
	if (!g_guest_address_space->Commit(vaddr, mapped_size, mode)) {
		return 0;
	}
	if (!g_virtual_ranges->Add(vaddr, mapped_size, 0, ProgramProtection(mode), 0, type, name)) {
		EXIT_IF(!g_guest_address_space->ReleaseCommitted(vaddr, mapped_size));
		return 0;
	}
	MapGpuRange(vaddr, mapped_size);
	return vaddr;
}

uint64_t AllocateProgramMemory(uint64_t search_addr, uint64_t size, VirtualMemory::Mode mode,
                               const char* name) {
	return AllocateGuestRuntimeMemory(search_addr, size, mode, name, VirtualRangeType::Code, false);
}

void ChargeFlexibleMemory(uint64_t size) {
	g_flexible_memory->Charge(size);
}

void ReleaseFlexibleMemory(uint64_t size) {
	g_flexible_memory->Release(size);
}

void SetProgramMemoryProtection(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	const auto ranges = RequireGuestRuntimeMemory(vaddr, size);
	if (std::any_of(ranges.begin(), ranges.end(),
	                [](const auto& range) { return range.type != VirtualRangeType::Code; })) {
		EXIT("program-memory range is not fully mapped: addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     vaddr, size);
	}

	const auto host_mode = VirtualMemory::IsExecute(mode) ? VirtualMemory::Mode::ExecuteReadWrite
	                                                      : VirtualMemory::Mode::ReadWrite;
	EXIT_IF(!g_guest_address_space->Protect(vaddr, size, host_mode));
	g_virtual_ranges->Protect(vaddr, size, ProgramProtection(mode));
}

uint64_t AllocateRuntimeMemory(uint64_t search_addr, uint64_t size, VirtualMemory::Mode mode,
                               const char* name, bool fixed) {
	return AllocateGuestRuntimeMemory(search_addr, size, mode, name, VirtualRangeType::Runtime,
	                                  fixed);
}

uint64_t AllocateGuestStackMemory(uint64_t search_addr, uint64_t size, VirtualMemory::Mode mode,
                                  const char* name) {
	const auto vaddr =
	    AllocateGuestRuntimeMemory(search_addr, size, mode, name, VirtualRangeType::Stack, false);
	if (vaddr != 0 && g_alloc_callback != nullptr) {
		g_alloc_callback(vaddr, size);
	}
	return vaddr;
}

bool ProtectGuestMemory(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode,
                        VirtualMemory::Mode* old_mode) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	constexpr uint64_t                    GuestPageSize = 0x4000;
	if (vaddr == 0 || size == 0 || size > UINT64_MAX - (vaddr & (GuestPageSize - 1u))) {
		return false;
	}
	const auto aligned_addr = vaddr & ~(GuestPageSize - 1u);
	const auto aligned_size =
	    (size + (vaddr - aligned_addr) + GuestPageSize - 1u) & ~(GuestPageSize - 1u);
	const auto ranges = RequireGuestRuntimeMemory(aligned_addr, aligned_size);
	if (old_mode != nullptr) {
		*old_mode = static_cast<VirtualMemory::Mode>(
		    ranges.front().protection & (PROT_CPU_READ | PROT_CPU_WRITE | PROT_CPU_EXEC));
	}
	if (!g_guest_address_space->Protect(aligned_addr, aligned_size, mode)) {
		return false;
	}
	g_virtual_ranges->Protect(aligned_addr, aligned_size, ProgramProtection(mode));
	return true;
}

bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
	return g_guest_address_space != nullptr &&
	       g_guest_address_space->ProtectTransient(vaddr, size, mode);
}

bool FreeGuestMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	constexpr uint64_t GuestPageSize = 0x4000;
	if (vaddr == 0 || size == 0 || size > UINT64_MAX - (GuestPageSize - 1u)) {
		return false;
	}
	const auto mapped_size = (size + GuestPageSize - 1u) & ~(GuestPageSize - 1u);
	(void)RequireGuestRuntimeMemory(vaddr, mapped_size);
	UnmapGpuRange(vaddr, mapped_size);
	return FreeGuestMemoryOwner(vaddr, mapped_size);
}

// The ABI entry points and batch operations hold g_memory_operation_mutex.
static int ProtectMemory(const void* addr, size_t len, int prot) {
	auto vaddr = reinterpret_cast<uint64_t>(addr);

	LOGF("\t addr = 0x%016" PRIx64 "\n"
	     "\t len  = 0x%016" PRIx64 "\n",
	     vaddr, static_cast<uint64_t>(len));

	constexpr uint64_t PAGE_SIZE    = 0x4000;
	auto               aligned_addr = vaddr & ~(PAGE_SIZE - 1);
	const auto         page_offset  = vaddr - aligned_addr;
	if (len > UINT64_MAX - page_offset || len + page_offset > UINT64_MAX - (PAGE_SIZE - 1)) {
		EXIT("memory-protection range overflows: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, static_cast<uint64_t>(len));
	}
	auto aligned_len = (len + page_offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	if (aligned_len == 0) {
		return OK;
	}

	VirtualMemory::Mode mode     = VirtualMemory::Mode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode)) {
		return KERNEL_ERROR_EINVAL;
	}
	std::vector<VirtualRanges::Range> old_ranges;
	if (!g_virtual_ranges->QuerySpan(aligned_addr, aligned_len, &old_ranges) ||
	    std::any_of(old_ranges.begin(), old_ranges.end(),
	                [](const auto& range) { return !IsCommittedRangeType(range.type); })) {
		EXIT("memory-protection range is not fully mapped: addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     aligned_addr, aligned_len);
	}
	const auto old_mode = static_cast<VirtualMemory::Mode>(
	    old_ranges.front().protection & (PROT_CPU_READ | PROT_CPU_WRITE | PROT_CPU_EXEC));
	bool ok = g_guest_address_space->Protect(aligned_addr, aligned_len, mode);

	if (!ok) {
		EXIT("host memory-protection update failed: addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " prot=0x%08x\n",
		     aligned_addr, aligned_len, prot);
	}
	g_virtual_ranges->Protect(aligned_addr, aligned_len, prot);

	LOGF("\t prot: %s -> %s\n", magic_enum::enum_name(old_mode), magic_enum::enum_name(mode));

	return OK;
}

int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot) {
	PRINT_NAME();
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	return ProtectMemory(addr, len, prot);
}

int KYTY_SYSV_ABI KernelMtypeprotect(const void* addr, size_t len, int type, int prot) {
	PRINT_NAME();
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	LOGF("\t type = %d\n", type);
	return ProtectMemory(addr, len, prot);
}

int KYTY_SYSV_ABI KernelBatchMap2(KernelBatchMapEntry* entries, int num_entries,
                                  int* num_entries_out, int flags) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t entries         = %p\n"
	     "\t num_entries     = %d\n"
	     "\t num_entries_out = %p\n"
	     "\t flags           = 0x%08" PRIx32 "\n",
	     static_cast<void*>(entries), num_entries, static_cast<void*>(num_entries_out),
	     static_cast<uint32_t>(flags));

	enum MemoryOpTypes {
		MAP_OP_MAP_DIRECT   = 0,
		MAP_OP_UNMAP        = 1,
		MAP_OP_PROTECT      = 2,
		MAP_OP_MAP_FLEXIBLE = 3,
		MAP_OP_TYPE_PROTECT = 4,
	};

	if (entries == nullptr || num_entries < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	int processed = 0;

	for (int i = 0; i < num_entries; i++, processed++) {
		auto* entry = &entries[i];

		LOGF("\t [%d] start = %p, offset = 0x%016" PRIx64 ", length = 0x%016" PRIx64
		     ", prot = 0x%02" PRIx32 ", type = 0x%02" PRIx32 ", op = %d\n",
		     i, entry->start, entry->offset, entry->length,
		     static_cast<uint32_t>(entry->protection), static_cast<uint32_t>(entry->type),
		     entry->operation);

		if (entry->length == 0 || entry->operation < MAP_OP_MAP_DIRECT ||
		    entry->operation > MAP_OP_TYPE_PROTECT) {
			break;
		}

		int result = OK;
		switch (entry->operation) {
			case MAP_OP_MAP_DIRECT:
				result = MapDirectMemory(&entry->start, entry->length, entry->protection, flags,
				                         static_cast<int64_t>(entry->offset), 0, "anon");
				break;
			case MAP_OP_UNMAP:
				result = UnmapMemory(reinterpret_cast<uint64_t>(entry->start), entry->length);
				break;
			case MAP_OP_PROTECT:
			case MAP_OP_TYPE_PROTECT:
				result = ProtectMemory(entry->start, entry->length, entry->protection);
				break;
			case MAP_OP_MAP_FLEXIBLE:
				result = MapFlexibleMemory(&entry->start, entry->length, entry->protection, flags,
				                           "anon");
				break;
			default: result = KERNEL_ERROR_EINVAL; break;
		}

		if (result != OK) {
			if (num_entries_out != nullptr) {
				*num_entries_out = processed;
			}
			return result;
		}
	}

	if (num_entries_out != nullptr) {
		*num_entries_out = processed;
	}

	return (processed == num_entries ? OK : KERNEL_ERROR_EINVAL);
}

int KYTY_SYSV_ABI KernelBatchMap(KernelBatchMapEntry* entries, int num_entries,
                                 int* num_entries_out) {
	constexpr int GUEST_MAP_FIXED = 0x10;
	return KernelBatchMap2(entries, num_entries, num_entries_out, GUEST_MAP_FIXED);
}

static bool IsAligned(uint64_t value, uint64_t alignment) {
	return alignment == 0 || (value & (alignment - 1u)) == 0;
}

static void MemoryPoolSubtractCommitted(uint64_t len) {
	// Writers are serialized by g_memory_operation_mutex; debug stats read without that lock.
	const auto current = g_memory_pool_committed.load(std::memory_order_relaxed);
	EXIT_IF(len > current);
	g_memory_pool_committed.store(current - len, std::memory_order_relaxed);
}

int KYTY_SYSV_ABI KernelMemoryPoolExpand(int64_t search_start, int64_t search_end, size_t len,
                                         size_t alignment, int64_t* phys_addr_out) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	constexpr uint64_t POOL_PAGE_SIZE = 0x10000;
	if (search_start < 0 || search_end <= search_start || len == 0 ||
	    (len & (POOL_PAGE_SIZE - 1u)) != 0 || phys_addr_out == nullptr ||
	    (alignment != 0 &&
	     (!std::has_single_bit(alignment) || (alignment & (POOL_PAGE_SIZE - 1u)) != 0))) {
		return KERNEL_ERROR_EINVAL;
	}
	if (static_cast<uint64_t>(search_end - search_start) < len) {
		return KERNEL_ERROR_ENOMEM;
	}

	const auto effective_alignment = (alignment != 0 ? alignment : POOL_PAGE_SIZE);
	uint64_t   phys_addr           = 0;
	if (!g_physical_memory->Alloc(static_cast<uint64_t>(search_start),
	                              static_cast<uint64_t>(search_end), len, effective_alignment,
	                              &phys_addr, 0, PhysicalMemory::AllocationKind::Pooled)) {
		return KERNEL_ERROR_ENOMEM;
	}

	g_pooled_memory->Expand(phys_addr, len);
	*phys_addr_out = static_cast<int64_t>(phys_addr);

	LOGF("\t search_start = 0x%016" PRIx64 "\n"
	     "\t search_end   = 0x%016" PRIx64 "\n"
	     "\t len          = 0x%016" PRIx64 "\n"
	     "\t alignment    = 0x%016" PRIx64 "\n"
	     "\t phys_addr    = 0x%016" PRIx64 "\n",
	     search_start, search_end, static_cast<uint64_t>(len), static_cast<uint64_t>(alignment),
	     phys_addr);
	return OK;
}

int KYTY_SYSV_ABI KernelMemoryPoolReserve(void* addr_in, size_t len, size_t alignment, int flags,
                                          void** addr_out) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t addr_in   = 0x%016" PRIx64 "\n"
	     "\t len       = 0x%016" PRIx64 "\n"
	     "\t alignment = 0x%016" PRIx64 "\n"
	     "\t flags     = 0x%08" PRIx32 "\n"
	     "\t addr_out  = %p\n",
	     reinterpret_cast<uint64_t>(addr_in), static_cast<uint64_t>(len),
	     static_cast<uint64_t>(alignment), static_cast<uint32_t>(flags),
	     static_cast<void*>(addr_out));

	constexpr uint64_t POOL_RESERVE_ALIGNMENT = 0x200000;

	if (addr_out == nullptr || len == 0 || !IsAligned(len, POOL_RESERVE_ALIGNMENT)) {
		return KERNEL_ERROR_EINVAL;
	}
	if (alignment != 0 &&
	    (!std::has_single_bit(alignment) || !IsAligned(alignment, POOL_RESERVE_ALIGNMENT))) {
		return KERNEL_ERROR_EINVAL;
	}

	void*      out_addr          = addr_in;
	const auto reserve_alignment = (alignment != 0 ? alignment : POOL_RESERVE_ALIGNMENT);
	const int  ret = KernelReserveVirtualRange(&out_addr, len, flags, reserve_alignment);
	if (ret == OK) {
		const auto           out_vaddr = reinterpret_cast<uint64_t>(out_addr);
		VirtualRanges::Range reserved_range {};
		if (!g_virtual_ranges->Query(out_vaddr, 0, &reserved_range) ||
		    reserved_range.start != out_vaddr || reserved_range.size != len ||
		    !IsReservedRangeType(reserved_range.type)) {
			return KERNEL_ERROR_EBUSY;
		}
		if (!g_virtual_ranges->ReplaceSpan(out_vaddr, len, VirtualRangeType::Reserved, 0, 0, 0,
		                                   VirtualRangeType::PoolReserved, reserved_range.name)) {
			return KERNEL_ERROR_EBUSY;
		}
		*addr_out = out_addr;
		LOGF("\t out_addr  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out_addr));
	}

	return ret;
}

int KYTY_SYSV_ABI KernelMemoryPoolCommit(void* addr, size_t len, int type, int prot, int flags) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t addr  = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n"
	     "\t type  = 0x%08" PRIx32 "\n"
	     "\t prot  = 0x%08" PRIx32 "\n"
	     "\t flags = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(addr), static_cast<uint64_t>(len), static_cast<uint32_t>(type),
	     static_cast<uint32_t>(prot), static_cast<uint32_t>(flags));

	constexpr uint64_t POOL_COMMIT_ALIGNMENT = 0x10000;
	constexpr int      PROT_CPU_EXEC         = 0x04;

	if (addr == nullptr || len == 0 || !IsAligned(len, POOL_COMMIT_ALIGNMENT) ||
	    (prot & PROT_CPU_EXEC) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	VirtualMemory::Mode mode     = VirtualMemory::Mode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode)) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto vaddr = reinterpret_cast<uint64_t>(addr);

	VirtualRanges::Range old_range {};
	if (!g_virtual_ranges->Query(vaddr, 0, &old_range) ||
	    old_range.type != VirtualRangeType::PoolReserved ||
	    len > old_range.size - (vaddr - old_range.start)) {
		return KERNEL_ERROR_EACCES;
	}

	std::vector<PooledMemory::Mapping> mappings;
	if (!g_pooled_memory->Allocate(vaddr, len, &mappings)) {
		return KERNEL_ERROR_ENOMEM;
	}

	size_t mapped_size = 0;
	auto   rollback    = [&]() {
		if (mapped_size != 0 && !g_guest_address_space->UnmapBacking(vaddr, mapped_size)) {
			EXIT("failed to roll back pooled-memory backing maps\n");
		}
		if (!g_pooled_memory->Release(vaddr, len)) {
			EXIT("failed to release pooled-memory rollback allocation\n");
		}
	};

	for (const auto& mapping: mappings) {
		auto       failure_reason = GuestBackingStore::FailureReason::None;
		const bool ok = g_guest_address_space->MapBacking(mapping.vaddr, mapping.size,
		                                                  mapping.phys_addr, mode, &failure_reason);
		if (!ok) {
			LOGF_COLOR(Log::Color::Red, "\t pool backing map failed: %s\n",
			           GuestBackingStore::GetFailureReasonName(failure_reason));
			rollback();
			return KERNEL_ERROR_ENOMEM;
		}
		mapped_size += mapping.size;
	}

	if (!g_virtual_ranges->ReplaceSpan(vaddr, len, VirtualRangeType::PoolReserved, 0, prot, type,
	                                   VirtualRangeType::Pooled, old_range.name)) {
		rollback();
		return KERNEL_ERROR_EBUSY;
	}

	MapGpuRange(vaddr, len);

	if (g_alloc_callback != nullptr) {
		g_alloc_callback(vaddr, len);
	}

	g_memory_pool_committed.fetch_add(len, std::memory_order_relaxed);

	return OK;
}

int KYTY_SYSV_ABI KernelMemoryPoolDecommit(void* addr, size_t len, int flags) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t addr  = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n"
	     "\t flags = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(addr), static_cast<uint64_t>(len),
	     static_cast<uint32_t>(flags));

	constexpr uint64_t POOL_COMMIT_ALIGNMENT = 0x10000;

	if (addr == nullptr || len == 0 || !IsAligned(len, POOL_COMMIT_ALIGNMENT)) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto vaddr = reinterpret_cast<uint64_t>(addr);
	if (UINT64_MAX - vaddr < len) {
		return KERNEL_ERROR_EINVAL;
	}
	std::vector<VirtualRanges::Range> ranges;
	if (!g_virtual_ranges->QuerySpan(vaddr, len, &ranges) ||
	    std::any_of(ranges.begin(), ranges.end(),
	                [](const auto& range) { return !IsPooledRangeType(range.type); })) {
		return KERNEL_ERROR_EACCES;
	}

	UnmapGpuRange(vaddr, len);
	for (const auto& range: ranges) {
		if (range.type == VirtualRangeType::Pooled &&
		    UnmapMemoryOwner(range, range.start, range.size, true) != OK) {
			EXIT("pooled-memory backing transaction failed after GPU unmap: addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     range.start, range.size);
		}
	}
	return OK;
}

int KYTY_SYSV_ABI KernelMemoryPoolBatch(const KernelMemoryPoolBatchEntry* entries, int num_entries,
                                        int* num_entries_out, int flags) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	if (entries == nullptr || num_entries < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	enum MemoryPoolOp {
		POOL_OP_COMMIT       = 1,
		POOL_OP_DECOMMIT     = 2,
		POOL_OP_PROTECT      = 3,
		POOL_OP_TYPE_PROTECT = 4,
		POOL_OP_MOVE         = 5,
	};

	int processed = 0;
	int result    = OK;

	for (int i = 0; i < num_entries; i++, processed++) {
		const auto& entry = entries[i];
		switch (entry.op) {
			case POOL_OP_COMMIT:
				result = KernelMemoryPoolCommit(entry.commit.addr, entry.commit.len,
				                                entry.commit.type, entry.commit.prot, entry.flags);
				break;
			case POOL_OP_DECOMMIT:
				result =
				    KernelMemoryPoolDecommit(entry.decommit.addr, entry.decommit.len, entry.flags);
				break;
			case POOL_OP_PROTECT:
				result = KernelMprotect(entry.protect.addr, entry.protect.len, entry.protect.prot);
				break;
			case POOL_OP_TYPE_PROTECT:
				result = KernelMtypeprotect(entry.type_protect.addr, entry.type_protect.len,
				                            entry.type_protect.type, entry.type_protect.prot);
				break;
			case POOL_OP_MOVE:
			default: result = KERNEL_ERROR_EINVAL; break;
		}

		if (result != OK) {
			break;
		}
	}

	if (num_entries_out != nullptr) {
		*num_entries_out = processed;
	}

	(void)flags;
	return result;
}

int KYTY_SYSV_ABI KernelMemoryPoolGetBlockStats(KernelMemoryPoolBlockStats* output,
                                                size_t                      output_size) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	if (output == nullptr && output_size != 0) {
		return KERNEL_ERROR_EFAULT;
	}

	KernelMemoryPoolBlockStats stats {};
	constexpr uint64_t         BLOCK_SIZE = 0x10000;
	const uint64_t             committed  = g_memory_pool_committed.load(std::memory_order_relaxed);
	const uint64_t available = (g_pooled_memory != nullptr ? g_pooled_memory->Available() : 0);

	stats.available_flushed_blocks = static_cast<int32_t>(available / BLOCK_SIZE);
	stats.available_cached_blocks  = 0;
	stats.allocated_flushed_blocks = static_cast<int32_t>(committed / BLOCK_SIZE);
	stats.allocated_cached_blocks  = 0;

	const auto copy_size = std::min(output_size, sizeof(stats));
	if (copy_size != 0) {
		std::memcpy(output, &stats, copy_size);
	}

	return OK;
}

DebugStats GetDebugStats() {
	DebugStats stats;
	stats.direct_total       = PhysicalMemory::Size();
	stats.flexible_total     = FlexibleMemory::Size();
	stats.pool_committed     = g_memory_pool_committed.load(std::memory_order_relaxed);
	stats.cpu_page_entries_total = g_cpu_table_memory / MAPPING_TABLE_SIZE;
	stats.gpu_page_entries_total = g_gpu_table_memory / MAPPING_TABLE_SIZE;
	if (g_physical_memory != nullptr) {
		Common::LockGuard lock(g_physical_memory->GetMutex());
		for (const auto& [start, block]: g_physical_memory->GetPhysicalBlocks()) {
			switch (block.kind) {
				case PhysicalMemory::AllocationKind::Direct:
					stats.direct_allocated += block.size;
					break;
				case PhysicalMemory::AllocationKind::Pooled:
					stats.pooled_allocated += block.size;
					break;
				case PhysicalMemory::AllocationKind::Automatic:
					stats.automatic_allocated += block.size;
					break;
			}
		}
		for (const auto& [vaddr, mapping]: g_physical_memory->GetMappings()) {
			stats.direct_mapped += mapping.size;
		}
	}
	if (g_flexible_memory != nullptr) {
		stats.flexible_used = stats.flexible_total - g_flexible_memory->Available();
	}
	if (g_virtual_ranges != nullptr) {
		stats.cpu_page_entries = g_virtual_ranges->CountPageTableEntries(false);
		stats.gpu_page_entries = g_virtual_ranges->CountPageTableEntries(true);
	}
	return stats;
}

} // namespace Libs::LibKernel::Memory
