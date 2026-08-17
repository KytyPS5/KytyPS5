#include "kernel/memory.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
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

enum class GpuAccessMode { NoAccess, Read, Write, ReadWrite };

constexpr uint64_t PAGE_TABLE_POOL_SIZE   = 4ull * 1024ull * 1024ull * 1024ull;
constexpr uint64_t PAGE_TABLE_GRANULARITY = 2ull * 1024ull * 1024ull;
constexpr int      PAGE_TABLE_POOL_ENTRIES =
    static_cast<int>(PAGE_TABLE_POOL_SIZE / PAGE_TABLE_GRANULARITY);
constexpr uint64_t DEFAULT_FLEXIBLE_MEMORY_SIZE = 1ull * 1024ull * 1024ull * 1024ull;

static uint64_t                      g_flexible_memory_size        = DEFAULT_FLEXIBLE_MEMORY_SIZE;
static bool                          g_flexible_memory_size_frozen = false;
static Graphics::GpuResourceManager* g_gpu_resources               = nullptr;

static Graphics::GpuResourceManager& GetGpuResources() {
	EXIT_IF(g_gpu_resources == nullptr);
	return *g_gpu_resources;
}

static bool IsGpuAddressRange(uint64_t vaddr, uint64_t size) {
	constexpr uint64_t GPU_ADDRESS_LIMIT = 1ull << 40u;
	return vaddr != 0 && size != 0 && vaddr < GPU_ADDRESS_LIMIT && size < GPU_ADDRESS_LIMIT - vaddr;
}

static void MapGpuRange(uint64_t vaddr, uint64_t size) {
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) {
		return;
	}
	GetGpuResources().MapMemory(vaddr, size);
}

static void UnmapGpuRange(uint64_t vaddr, uint64_t size) {
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) {
		return;
	}
	GetGpuResources().UnmapMemory(vaddr, size);
}

static bool DecodeMemoryProtection(int prot, VirtualMemory::Mode* mode, GpuAccessMode* gpu_mode) {
	EXIT_IF(mode == nullptr);
	EXIT_IF(gpu_mode == nullptr);

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

	if (gpu_read && gpu_write) {
		*gpu_mode = GpuAccessMode::ReadWrite;
	} else if (gpu_read) {
		*gpu_mode = GpuAccessMode::Read;
	} else if (gpu_write) {
		*gpu_mode = GpuAccessMode::Write;
	} else {
		*gpu_mode = GpuAccessMode::NoAccess;
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
static uint32_t g_test_backing_store_unmaps_before_failure = UINT32_MAX;
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

		if (start == 0 || size == 0) {
			return false;
		}
		auto position = LowerBound(start);
		if ((position != m_ranges.end() &&
		     VirtualRangesOverlap(start, size, position->start, position->size)) ||
		    (position != m_ranges.begin() &&
		     VirtualRangesOverlap(start, size, std::prev(position)->start,
		                          std::prev(position)->size))) {
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
		const auto index = static_cast<size_t>(position - m_ranges.begin());
		m_ranges.insert(position, r);
		MergeAroundUnlocked(index);
		return true;
	}

	bool Remove(uint64_t start, uint64_t size) {
		Common::LockGuard lock(m_mutex);

		auto position = LowerBound(start);
		if (position != m_ranges.end() && position->start == start && position->size == size) {
			m_ranges.erase(position);
			return true;
		}
		auto removed = RemoveUnlocked(start, size);
		MergeUnlocked();
		return removed;
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

		for (size_t index = 0; index < m_ranges.size(); index++) {
			auto& r = m_ranges[index];
			if (r.start == start && r.size == size && IsReservedRangeType(r.type)) {
				m_ranges.erase(m_ranges.begin() + static_cast<std::ptrdiff_t>(index));
				return true;
			}
		}
		return true;
	}

	bool ConsumeReserved(uint64_t start, uint64_t size,
	                     VirtualRangeType type = VirtualRangeType::Reserved) {
		Common::LockGuard lock(m_mutex);

		auto end = End(start, size);
		for (const auto& r: m_ranges) {
			if (r.type == type && start >= r.start && end <= End(r.start, r.size)) {
				RemoveUnlocked(start, size);
				MergeUnlocked();
				return true;
			}
		}

		return false;
	}

	bool ConsumeReservedSpan(uint64_t start, uint64_t size, Range* first_range = nullptr,
	                         VirtualRangeType type = VirtualRangeType::Reserved) {
		Common::LockGuard lock(m_mutex);

		if (size == 0) {
			return false;
		}

		auto current = start;
		auto end     = End(start, size);
		while (current < end) {
			const Range* candidate = nullptr;
			for (const auto& r: m_ranges) {
				if (r.type == type && current >= r.start && current < End(r.start, r.size)) {
					candidate = &r;
					break;
				}
			}
			if (candidate == nullptr) {
				return false;
			}
			if (current == start && first_range != nullptr) {
				*first_range = *candidate;
			}
			current = std::min(end, End(candidate->start, candidate->size));
		}

		RemoveUnlocked(start, size);
		MergeUnlocked();
		return true;
	}

	void Rename(uint64_t start, uint64_t size, const char* name) {
		Common::LockGuard lock(m_mutex);

		auto position = LowerBound(start);
		if (position != m_ranges.end() && position->start == start && position->size == size) {
			CopyVirtualRangeName(position->name, name);
			MergeAroundUnlocked(static_cast<size_t>(position - m_ranges.begin()));
			return;
		}
		EditUnlocked(start, size, [name](Range* r) { CopyVirtualRangeName(r->name, name); });
	}

	void Protect(uint64_t start, uint64_t size, int protection) {
		Common::LockGuard lock(m_mutex);

		EditUnlocked(start, size, [protection](Range* r) { r->protection = protection; });
	}

	void SetMemoryType(uint64_t start, uint64_t size, int memory_type) {
		Common::LockGuard lock(m_mutex);

		EditUnlocked(start, size, [memory_type](Range* r) { r->memory_type = memory_type; });
	}

	bool Query(uint64_t addr, int flags, Range* out) {
		EXIT_IF(out == nullptr);

		Common::LockGuard lock(m_mutex);

		auto next = std::upper_bound(
		    m_ranges.begin(), m_ranges.end(), addr,
		    [](uint64_t value, const Range& range) { return value < range.start; });
		if (next != m_ranges.begin()) {
			auto current = std::prev(next);
			if (addr < End(current->start, current->size)) {
				*out = *current;
				return true;
			}
		}
		if (flags != 1 || next == m_ranges.end()) {
			return false;
		}

		*out = *next;
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
		for (const auto& range: m_ranges) {
			const auto range_end = End(range.start, range.size);
			if (range_end <= current) {
				continue;
			}
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

		auto vma = std::upper_bound(
		    m_ranges.begin(), m_ranges.end(), virtual_addr,
		    [](uint64_t value, const Range& range) { return value < range.start; });
		if (vma == m_ranges.begin()) {
			return 0;
		}
		--vma;

		const auto vma_end = End(vma->start, vma->size);
		if (virtual_addr < vma->start || virtual_addr >= vma_end ||
		    !IsCommittedRangeType(vma->type)) {
			return 0;
		}

		uint64_t clamped_size = std::min(size, vma_end - virtual_addr);
		uint64_t expected     = virtual_addr + clamped_size;
		++vma;

		while (vma != m_ranges.end() && vma->start == expected && IsCommittedRangeType(vma->type) &&
		       clamped_size < size) {
			const auto chunk = std::min(size - clamped_size, vma->size);
			clamped_size += chunk;
			expected += chunk;
			++vma;
		}

		return clamped_size;
	}

	uint64_t CountPageTableEntries(bool gpu) {
		Common::LockGuard lock(m_mutex);

		uint64_t used = 0;
		for (const auto& r: m_ranges) {
			if (!IsCommittedRangeType(r.type) || r.size == 0) {
				continue;
			}

			const bool has_cpu_access =
			    (r.protection & (PROT_CPU_READ | PROT_CPU_WRITE | PROT_CPU_EXEC)) != 0;
			const bool has_gpu_access = (r.protection & (PROT_GPU_READ | PROT_GPU_WRITE)) != 0;
			if (gpu ? !has_gpu_access : !has_cpu_access) {
				continue;
			}

			const auto end         = End(r.start, r.size);
			const auto first_entry = r.start / PAGE_TABLE_GRANULARITY;
			const auto last_entry  = (end - 1) / PAGE_TABLE_GRANULARITY;
			used += last_entry - first_entry + 1;
		}

		return used;
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

	static void AddPiece(std::vector<Range>* ranges, const Range& source, uint64_t start,
	                     uint64_t end) {
		EXIT_IF(ranges == nullptr);

		if (end <= start) {
			return;
		}

		Range piece = source;
		piece.start = start;
		piece.size  = end - start;
		if (piece.type == VirtualRangeType::Direct) {
			piece.offset += start - source.start;
		}
		ranges->push_back(piece);
	}

	std::vector<Range>::iterator LowerBound(uint64_t start) {
		return std::lower_bound(
		    m_ranges.begin(), m_ranges.end(), start,
		    [](const Range& range, uint64_t value) { return range.start < value; });
	}

	void MergeAroundUnlocked(size_t index) {
		if (index >= m_ranges.size()) {
			return;
		}
		if (index != 0) {
			auto& previous = m_ranges[index - 1];
			auto& current  = m_ranges[index];
			if (End(previous.start, previous.size) == current.start &&
			    SameMergeKey(previous, current)) {
				previous.size += current.size;
				m_ranges.erase(m_ranges.begin() + static_cast<std::ptrdiff_t>(index));
				index--;
			}
		}
		while (index + 1 < m_ranges.size()) {
			auto& current = m_ranges[index];
			auto& next    = m_ranges[index + 1];
			if (End(current.start, current.size) != next.start || !SameMergeKey(current, next)) {
				break;
			}
			current.size += next.size;
			m_ranges.erase(m_ranges.begin() + static_cast<std::ptrdiff_t>(index + 1));
		}
	}

	template <typename EditFunc>
	void EditUnlocked(uint64_t start, uint64_t size, EditFunc edit) {
		if (size == 0) {
			return;
		}

		std::vector<Range> out;
		auto               edit_end = End(start, size);

		for (const auto& r: m_ranges) {
			auto r_end = End(r.start, r.size);
			if (!VirtualRangesOverlap(start, size, r.start, r.size)) {
				out.push_back(r);
				continue;
			}

			auto mid_start = std::max(start, r.start);
			auto mid_end   = std::min(edit_end, r_end);

			AddPiece(&out, r, r.start, mid_start);

			Range mid = r;
			mid.start = mid_start;
			mid.size  = mid_end - mid_start;
			if (mid.type == VirtualRangeType::Direct) {
				mid.offset += mid_start - r.start;
			}
			edit(&mid);
			out.push_back(mid);

			AddPiece(&out, r, mid_end, r_end);
		}

		m_ranges = out;
		MergeUnlocked();
	}

	bool RemoveUnlocked(uint64_t start, uint64_t size) {
		if (size == 0) {
			return false;
		}

		std::vector<Range> out;
		bool               removed = false;
		auto               rem_end = End(start, size);

		for (const auto& r: m_ranges) {
			auto r_end = End(r.start, r.size);
			if (!VirtualRangesOverlap(start, size, r.start, r.size)) {
				out.push_back(r);
				continue;
			}

			removed = true;
			AddPiece(&out, r, r.start, std::max(start, r.start));
			AddPiece(&out, r, std::min(rem_end, r_end), r_end);
		}

		m_ranges = out;
		return removed;
	}

	void MergeUnlocked() {
		if (m_ranges.size() < 2) {
			return;
		}

		std::sort(m_ranges.begin(), m_ranges.end(),
		          [](const Range& left, const Range& right) { return left.start < right.start; });

		std::vector<Range> merged;
		for (const auto& r: m_ranges) {
			if (!merged.empty()) {
				auto& last = merged[merged.size() - 1];
				if (End(last.start, last.size) == r.start && SameMergeKey(last, r)) {
					last.size += r.size;
					continue;
				}
			}
			merged.push_back(r);
		}
		m_ranges = merged;
	}

	Range* FindOverlap(uint64_t start, uint64_t size) {
		auto position = LowerBound(start);
		if (position != m_ranges.end() &&
		    VirtualRangesOverlap(start, size, position->start, position->size)) {
			return &*position;
		}
		if (position != m_ranges.begin()) {
			auto previous = std::prev(position);
			if (VirtualRangesOverlap(start, size, previous->start, previous->size)) {
				return &*previous;
			}
		}
		return nullptr;
	}

	std::vector<Range> m_ranges;
	Common::Mutex      m_mutex;
};

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static uint32_t g_test_physical_memory_unmaps_before_failure = UINT32_MAX;
static bool     g_test_fail_next_fixed_reserve_range_add     = false;
#endif

class PhysicalMemory {
public:
	struct AllocatedBlock {
		uint64_t            start_addr;
		uint64_t            size;
		uint64_t            map_vaddr;
		uint64_t            map_size;
		uint64_t            host_vaddr;
		uint64_t            host_size;
		int                 prot;
		VirtualMemory::Mode mode;
		GpuAccessMode       gpu_mode;
		int                 memory_type;
		bool                pool_expansion;
		char                name[KERNEL_MAXIMUM_NAME_LENGTH];
	};

	PhysicalMemory() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		m_free.emplace(0, Size());
	}
	virtual ~PhysicalMemory() = default;

	KYTY_CLASS_NO_COPY(PhysicalMemory);

	static constexpr uint64_t TotalSize() { return static_cast<uint64_t>(13824) * 1024 * 1024; }
	static uint64_t           Size() {
		EXIT_IF(g_flexible_memory_size >= TotalSize());
		return TotalSize() - g_flexible_memory_size;
	}

	bool Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment,
	           uint64_t* phys_addr_out, int memory_type, bool pool_expansion = false);
	bool Available(uint64_t search_start, uint64_t search_end, size_t alignment,
	               uint64_t* phys_addr_out, uint64_t* size_out);
	bool Release(uint64_t start, size_t len, uint64_t* vaddr, uint64_t* size,
	             GpuAccessMode* gpu_mode);
	bool Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode,
	         GpuAccessMode gpu_mode);
	bool Unmap(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode,
	           uint64_t* host_vaddr_to_release = nullptr);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot,
	          VirtualMemory::Mode* mode, GpuAccessMode* gpu_mode);
	bool Find(uint64_t phys_addr, bool next, PhysicalMemory::AllocatedBlock* out);
	bool CanMapDirect(uint64_t phys_addr, size_t len);
	bool ReleasePoolExpansion(uint64_t phys_addr, size_t len);
	bool GetAllocatedSpan(uint64_t phys_addr, size_t len, std::vector<AllocatedBlock>* blocks);
	std::vector<AllocatedBlock> FindMappings(uint64_t phys_addr, size_t len);
	void ProtectMapping(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode,
	                    GpuAccessMode gpu_mode);
	void SetVirtualRangeName(uint64_t vaddr, uint64_t len, const char* name);
	void SetVirtualRangeMemoryType(uint64_t vaddr, uint64_t len, int memory_type);

	[[nodiscard]] Common::Mutex&                            GetMutex() { return m_mutex; }
	[[nodiscard]] const std::map<uint64_t, AllocatedBlock>& GetPhysicalBlocks() const {
		return m_physical;
	}
	[[nodiscard]] const std::vector<AllocatedBlock>& GetMappings() const { return m_mappings; }

private:
	void ConsumeFreeRange(std::map<uint64_t, uint64_t>::iterator range, uint64_t start,
	                      uint64_t size);
	void AddFreeRange(uint64_t start, uint64_t size);

	std::map<uint64_t, AllocatedBlock> m_physical;
	std::map<uint64_t, uint64_t>       m_free;
	std::vector<AllocatedBlock>        m_mappings;
	Common::Mutex                      m_mutex;
};

class FlexibleMemory {
public:
	struct AllocatedBlock {
		uint64_t            map_vaddr;
		uint64_t            map_size;
		uint64_t            backing_offset;
		uint64_t            host_vaddr;
		uint64_t            host_size;
		int                 prot;
		VirtualMemory::Mode mode;
		GpuAccessMode       gpu_mode;
		char                name[KERNEL_MAXIMUM_NAME_LENGTH];
	};

	FlexibleMemory() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		m_free.emplace(PhysicalMemory::Size(), Size());
	}
	virtual ~FlexibleMemory() = default;

	KYTY_CLASS_NO_COPY(FlexibleMemory);

	static uint64_t Size() { return g_flexible_memory_size; }
	uint64_t        Available();

	bool Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, GpuAccessMode gpu_mode,
	         const char* name);
	bool Unmap(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode,
	           uint64_t* host_vaddr_to_release = nullptr);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot,
	          VirtualMemory::Mode* mode, GpuAccessMode* gpu_mode);
	bool Snapshot(uint64_t vaddr, uint64_t size, std::vector<AllocatedBlock>* blocks);
	bool Restore(const std::vector<AllocatedBlock>& blocks);
	void Protect(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode,
	             GpuAccessMode gpu_mode);
	void SetVirtualRangeName(uint64_t vaddr, uint64_t len, const char* name);

	[[nodiscard]] Common::Mutex&                     GetMutex() { return m_mutex; }
	[[nodiscard]] const std::vector<AllocatedBlock>& GetBlocks() const { return m_allocated; }

private:
	void ConsumeFreeRange(std::map<uint64_t, uint64_t>::iterator range, uint64_t start,
	                      uint64_t size);
	void AddFreeRange(uint64_t start, uint64_t size);

	std::vector<AllocatedBlock>  m_allocated;
	std::map<uint64_t, uint64_t> m_free;
	uint64_t                     m_allocated_total = 0;
	Common::Mutex                m_mutex;
};

class PooledMemory {
public:
	struct Mapping {
		uint64_t      vaddr;
		uint64_t      size;
		uint64_t      phys_addr;
		GpuAccessMode gpu_mode;
	};

	void                   Expand(uint64_t phys_addr, uint64_t size);
	bool                   ReleaseExpansion(uint64_t phys_addr, uint64_t size);
	bool                   Allocate(uint64_t vaddr, uint64_t size, GpuAccessMode gpu_mode,
	                                std::vector<Mapping>* mappings);
	bool                   Query(uint64_t vaddr, uint64_t size, std::vector<Mapping>* mappings);
	bool                   Release(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode);
	[[nodiscard]] uint64_t Available();
	[[nodiscard]] std::vector<Mapping> GetMappings();

private:
	struct PhysicalRange {
		uint64_t start;
		uint64_t size;
	};

	void AddFreeUnlocked(uint64_t start, uint64_t size);
	bool QueryUnlocked(uint64_t vaddr, uint64_t size, std::vector<Mapping>* mappings) const;

	std::vector<PhysicalRange> m_free;
	std::vector<PhysicalRange> m_expansions;
	std::vector<Mapping>       m_mappings;
	Common::Mutex              m_mutex;
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

bool TryReadGpuCleanBacking(uint64_t vaddr, void* data, uint64_t size) {
	if (g_gpu_resources != nullptr && IsGpuAddressRange(vaddr, size)) {
		if (!Graphics::GuestGpu::IsGpuThread() ||
		    GetGpuResources().GetBufferCache().HasGpuDirtyBytes(vaddr, size) ||
		    GetGpuResources().GetTextureCache().QueryRegion(vaddr, size).gpu_image_bytes) {
			return false;
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

void InstallGpuResources(Graphics::GpuResourceManager* resources) noexcept {
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

bool TryReadPrtBacking(uint64_t vaddr, void* data, uint64_t size) {
	std::vector<VirtualRanges::Range> ranges;
	if (g_guest_address_space == nullptr || g_virtual_ranges == nullptr ||
	    !IsInPrtAperture(vaddr, size) || !g_virtual_ranges->QuerySpan(vaddr, size, &ranges)) {
		return false;
	}
	if (std::any_of(ranges.begin(), ranges.end(), [](const auto& range) {
		    return !IsReservedRangeType(range.type) &&
		           !g_guest_address_space->BackingContains(range.start, range.size);
	    })) {
		return false;
	}
	return g_guest_address_space->TryReadSparseBacking(vaddr, data, size);
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
	g_flexible_memory_size_frozen = true;
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
	for (const auto& b: g_physical_memory->GetMappings()) {
		if (b.map_vaddr != 0 && b.map_size != 0) {
			g_alloc_callback(b.map_vaddr, b.map_size);
		}
	}
	g_physical_memory->GetMutex().Unlock();

	g_flexible_memory->GetMutex().Lock();
	for (const auto& b: g_flexible_memory->GetBlocks()) {
		g_alloc_callback(b.map_vaddr, b.map_size);
	}
	g_flexible_memory->GetMutex().Unlock();

	for (const auto& mapping: g_pooled_memory->GetMappings()) {
		g_alloc_callback(mapping.vaddr, mapping.size);
	}
}

void SetFlexibleMemorySize(uint64_t size) {
	constexpr uint64_t GuestPageSize = 0x4000;
	EXIT_IF(g_flexible_memory_size_frozen || g_guest_address_space != nullptr);
	EXIT_IF(size == 0 || (size & (GuestPageSize - 1u)) != 0 || size >= PhysicalMemory::TotalSize());
	g_flexible_memory_size = size;
	LOGF("\t flexible memory size = 0x%016" PRIx64 " (%" PRIu64 " MiB)\n", size,
	     size / (1024ull * 1024ull));
}

bool PhysicalMemory::Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment,
                           uint64_t* phys_addr_out, int memory_type, bool pool_expansion) {
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
		b.gpu_mode       = GpuAccessMode::NoAccess;
		b.map_size       = 0;
		b.map_vaddr      = 0;
		b.prot           = 0;
		b.mode           = VirtualMemory::Mode::NoAccess;
		b.memory_type    = memory_type;
		b.pool_expansion = pool_expansion;

		ConsumeFreeRange(range, free_pos, len);
		EXIT_IF(!m_physical.emplace(b.start_addr, b).second);

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

	for (const auto& [range_start, range_size]: m_free) {
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

void PhysicalMemory::ConsumeFreeRange(std::map<uint64_t, uint64_t>::iterator range, uint64_t start,
                                      uint64_t size) {
	const auto range_start = range->first;
	const auto range_end   = range->first + range->second;
	m_free.erase(range);
	if (range_start < start) {
		m_free.emplace(range_start, start - range_start);
	}
	if (start + size < range_end) {
		m_free.emplace(start + size, range_end - start - size);
	}
}

void PhysicalMemory::AddFreeRange(uint64_t start, uint64_t size) {
	auto end  = start + size;
	auto next = m_free.lower_bound(start);
	if (next != m_free.begin()) {
		auto previous = std::prev(next);
		if (previous->first + previous->second >= start) {
			start = previous->first;
			end   = std::max(end, previous->first + previous->second);
			next  = m_free.erase(previous);
		}
	}
	while (next != m_free.end() && next->first <= end) {
		end  = std::max(end, next->first + next->second);
		next = m_free.erase(next);
	}
	m_free.emplace(start, end - start);
}

bool PhysicalMemory::Release(uint64_t start, size_t len, uint64_t* vaddr, uint64_t* size,
                             GpuAccessMode* gpu_mode) {
	EXIT_IF(vaddr == nullptr);
	EXIT_IF(size == nullptr);
	EXIT_IF(gpu_mode == nullptr);

	Common::LockGuard lock(m_mutex);

	auto next = m_physical.upper_bound(start);
	if (next == m_physical.begin()) {
		return false;
	}
	auto  it = std::prev(next);
	auto& b  = it->second;
	if (b.pool_expansion || start < b.start_addr || start >= b.start_addr + b.size ||
	    len > b.start_addr + b.size - start) {
		return false;
	}

	if (start == b.start_addr && len == b.size) {
		*vaddr    = b.map_vaddr;
		*size     = b.map_size;
		*gpu_mode = b.gpu_mode;

		m_physical.erase(it);
		AddFreeRange(start, len);
		return true;
	}
	if (start > b.start_addr && start + len < b.start_addr + b.size) {
		auto old_start = b.start_addr;
		auto old_end   = b.start_addr + b.size;

		*vaddr    = (b.map_vaddr != 0 ? b.map_vaddr + (start - old_start) : 0);
		*size     = (b.map_vaddr != 0 ? len : 0);
		*gpu_mode = b.gpu_mode;

		AllocatedBlock right = b;
		right.start_addr     = start + len;
		right.size           = old_end - right.start_addr;
		if (right.map_vaddr != 0) {
			right.map_vaddr += right.start_addr - old_start;
			right.map_size = right.size;
		}

		b.size = start - old_start;
		if (b.map_vaddr != 0) {
			b.map_size = b.size;
		}

		m_physical.emplace(right.start_addr, right);
		AddFreeRange(start, len);
		return true;
	}
	if (start == b.start_addr && len < b.size) {
		*vaddr    = b.map_vaddr;
		*size     = (b.map_vaddr != 0 ? len : 0);
		*gpu_mode = b.gpu_mode;

		AllocatedBlock remaining = b;
		m_physical.erase(it);
		remaining.start_addr += len;
		remaining.size -= len;
		if (remaining.map_vaddr != 0) {
			remaining.map_vaddr += len;
			remaining.map_size -= len;
		}
		m_physical.emplace(remaining.start_addr, remaining);
		AddFreeRange(start, len);
		return true;
	}
	if (start > b.start_addr && start + len == b.start_addr + b.size) {
		*vaddr    = (b.map_vaddr != 0 ? b.map_vaddr + (start - b.start_addr) : 0);
		*size     = (b.map_vaddr != 0 ? len : 0);
		*gpu_mode = b.gpu_mode;

		b.size = start - b.start_addr;
		if (b.map_vaddr != 0) {
			b.map_size = b.size;
		}
		AddFreeRange(start, len);
		return true;
	}

	return false;
}

bool PhysicalMemory::Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot,
                         VirtualMemory::Mode mode, GpuAccessMode gpu_mode) {
	Common::LockGuard lock(m_mutex);

	if (len == 0 || UINT64_MAX - phys_addr < len) {
		return false;
	}

	auto current = phys_addr;
	auto next    = m_physical.upper_bound(current);
	if (next == m_physical.begin()) {
		return false;
	}
	auto first = std::prev(next);
	while (current < phys_addr + len) {
		auto block = m_physical.upper_bound(current);
		if (block == m_physical.begin()) {
			return false;
		}
		--block;
		const auto block_end = block->second.start_addr + block->second.size;
		if (block->second.pool_expansion || current < block->second.start_addr ||
		    current >= block_end) {
			return false;
		}
		current = std::min<uint64_t>(phys_addr + len, block_end);
		if (current < phys_addr + len) {
			const auto following = std::next(block);
			if (following == m_physical.end() || following->second.start_addr != current) {
				return false;
			}
		}
	}

	AllocatedBlock mapping = first->second;
	mapping.start_addr     = phys_addr;
	mapping.size           = len;
	mapping.map_vaddr      = vaddr;
	mapping.map_size       = len;
	mapping.host_vaddr     = vaddr;
	mapping.host_size      = len;
	mapping.prot           = prot;
	mapping.mode           = mode;
	mapping.gpu_mode       = gpu_mode;
	m_mappings.push_back(mapping);

	return true;
}

bool PhysicalMemory::CanMapDirect(uint64_t phys_addr, size_t len) {
	Common::LockGuard lock(m_mutex);

	if (len == 0 || UINT64_MAX - phys_addr < len) {
		return false;
	}

	const auto end     = phys_addr + len;
	auto       current = phys_addr;
	while (current < end) {
		auto block = m_physical.upper_bound(current);
		if (block == m_physical.begin()) {
			return false;
		}
		--block;
		const auto block_end = block->second.start_addr + block->second.size;
		if (block->second.pool_expansion || current < block->second.start_addr ||
		    current >= block_end) {
			return false;
		}
		current = std::min(end, block_end);
		if (current < end) {
			const auto following = std::next(block);
			if (following == m_physical.end() || following->second.start_addr != current) {
				return false;
			}
		}
	}
	return true;
}

bool PhysicalMemory::ReleasePoolExpansion(uint64_t phys_addr, size_t len) {
	Common::LockGuard lock(m_mutex);
	const auto        it = m_physical.find(phys_addr);
	if (it == m_physical.end() || !it->second.pool_expansion || it->second.size != len) {
		return false;
	}
	m_physical.erase(it);
	AddFreeRange(phys_addr, len);
	return true;
}

bool PhysicalMemory::GetAllocatedSpan(uint64_t phys_addr, size_t len,
                                      std::vector<AllocatedBlock>* blocks) {
	EXIT_IF(blocks == nullptr);
	blocks->clear();
	if (len == 0 || UINT64_MAX - phys_addr < len) {
		return false;
	}

	Common::LockGuard lock(m_mutex);
	const auto        end     = phys_addr + len;
	auto              current = phys_addr;
	while (current < end) {
		auto block = m_physical.upper_bound(current);
		if (block == m_physical.begin()) {
			blocks->clear();
			return false;
		}
		--block;
		const auto block_end = block->second.start_addr + block->second.size;
		if (block->second.pool_expansion || current < block->second.start_addr ||
		    current >= block_end) {
			blocks->clear();
			return false;
		}

		auto part       = block->second;
		part.start_addr = current;
		part.size       = std::min(end, block_end) - current;
		blocks->push_back(part);
		current += part.size;
		if (current < end) {
			const auto following = std::next(block);
			if (following == m_physical.end() || following->second.start_addr != current) {
				blocks->clear();
				return false;
			}
		}
	}
	return true;
}

bool PhysicalMemory::Unmap(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode,
                           uint64_t* host_vaddr_to_release) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (g_test_physical_memory_unmaps_before_failure == 0) {
		g_test_physical_memory_unmaps_before_failure = UINT32_MAX;
		return false;
	}
	if (g_test_physical_memory_unmaps_before_failure != UINT32_MAX) {
		g_test_physical_memory_unmaps_before_failure--;
	}
#endif

	EXIT_IF(gpu_mode == nullptr);

	Common::LockGuard lock(m_mutex);

	if (host_vaddr_to_release != nullptr) {
		*host_vaddr_to_release = 0;
	}

	auto set_host_release_if_last = [this, host_vaddr_to_release](uint64_t host_vaddr,
	                                                              uint64_t host_size) {
		if (host_vaddr_to_release == nullptr || host_vaddr == 0 || host_size == 0) {
			return;
		}
		const bool still_mapped = std::any_of(
		    m_mappings.begin(), m_mappings.end(), [host_vaddr, host_size](const auto& block) {
			    return block.host_vaddr == host_vaddr && block.host_size == host_size;
		    });
		if (!still_mapped) {
			*host_vaddr_to_release = host_vaddr;
		}
	};

	size_t index = 0;
	for (auto& b: m_mappings) {
		if (b.map_vaddr == vaddr && b.map_size == size) {
			*gpu_mode             = b.gpu_mode;
			const auto host_vaddr = b.host_vaddr;
			const auto host_size  = b.host_size;

			m_mappings.erase(m_mappings.begin() + static_cast<std::ptrdiff_t>(index));
			set_host_release_if_last(host_vaddr, host_size);

			return true;
		}
		if (vaddr > b.map_vaddr && vaddr + size < b.map_vaddr + b.map_size) {
			*gpu_mode = b.gpu_mode;

			AllocatedBlock right = b;
			right.start_addr += (vaddr + size) - b.map_vaddr;
			right.size      = b.map_vaddr + b.map_size - (vaddr + size);
			right.map_size  = right.size;
			right.map_vaddr = vaddr + size;

			b.size     = vaddr - b.map_vaddr;
			b.map_size = b.size;
			m_mappings.push_back(right);
			return true;
		}
		if (vaddr == b.map_vaddr && size < b.map_size) {
			*gpu_mode = b.gpu_mode;

			b.start_addr += size;
			b.size -= size;
			b.map_vaddr += size;
			b.map_size -= size;
			return true;
		}
		if (vaddr > b.map_vaddr && vaddr + size == b.map_vaddr + b.map_size) {
			*gpu_mode = b.gpu_mode;

			b.size     = vaddr - b.map_vaddr;
			b.map_size = b.size;
			return true;
		}
		index++;
	}

	return false;
}

void PhysicalMemory::ProtectMapping(uint64_t vaddr, uint64_t size, int prot,
                                    VirtualMemory::Mode mode, GpuAccessMode gpu_mode) {
	Common::LockGuard lock(m_mutex);
	if (size == 0 || UINT64_MAX - vaddr < size) {
		return;
	}

	const auto                  end = vaddr + size;
	std::vector<AllocatedBlock> updated;
	updated.reserve(m_mappings.size() + 2);
	for (const auto& block: m_mappings) {
		const auto block_end = block.map_vaddr + block.map_size;
		if (!VirtualRangesOverlap(vaddr, size, block.map_vaddr, block.map_size)) {
			updated.push_back(block);
			continue;
		}
		const auto overlap_start = std::max(vaddr, block.map_vaddr);
		const auto overlap_end   = std::min(end, block_end);
		if (block.map_vaddr < overlap_start) {
			auto left     = block;
			left.size     = overlap_start - block.map_vaddr;
			left.map_size = left.size;
			updated.push_back(left);
		}
		auto middle       = block;
		middle.start_addr = block.start_addr + overlap_start - block.map_vaddr;
		middle.size       = overlap_end - overlap_start;
		middle.map_vaddr  = overlap_start;
		middle.map_size   = middle.size;
		middle.prot       = prot;
		middle.mode       = mode;
		middle.gpu_mode   = gpu_mode;
		updated.push_back(middle);
		if (overlap_end < block_end) {
			auto right       = block;
			right.start_addr = block.start_addr + overlap_end - block.map_vaddr;
			right.size       = block_end - overlap_end;
			right.map_vaddr  = overlap_end;
			right.map_size   = right.size;
			updated.push_back(right);
		}
	}
	m_mappings = std::move(updated);
}

bool PhysicalMemory::Find(uint64_t phys_addr, bool next, AllocatedBlock* out) {
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

	if (next && following != m_physical.end()) {
		*out = following->second;
		return true;
	}

	return false;
}

std::vector<PhysicalMemory::AllocatedBlock> PhysicalMemory::FindMappings(uint64_t phys_addr,
                                                                         size_t   len) {
	Common::LockGuard lock(m_mutex);

	std::vector<AllocatedBlock> mappings;
	for (const auto& m: m_mappings) {
		if (!VirtualRangesOverlap(phys_addr, len, m.start_addr, m.size)) {
			continue;
		}

		const uint64_t overlap_start = std::max<uint64_t>(phys_addr, m.start_addr);
		const uint64_t overlap_end   = std::min<uint64_t>(phys_addr + len, m.start_addr + m.size);
		AllocatedBlock part          = m;
		part.start_addr              = overlap_start;
		part.size                    = overlap_end - overlap_start;
		part.map_vaddr += overlap_start - m.start_addr;
		part.map_size = part.size;
		mappings.push_back(part);
	}

	std::sort(mappings.begin(), mappings.end(),
	          [](const auto& a, const auto& b) { return a.map_vaddr < b.map_vaddr; });
	return mappings;
}

bool PhysicalMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot,
                          VirtualMemory::Mode* mode, GpuAccessMode* gpu_mode) {
	Common::LockGuard lock(m_mutex);

	return std::any_of(m_mappings.begin(), m_mappings.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b) {
		                   if (vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size) {
			                   if (base_addr != nullptr) {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr) {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr) {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr) {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr) {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

void PhysicalMemory::SetVirtualRangeName(uint64_t vaddr, uint64_t len, const char* name) {
	Common::LockGuard lock(m_mutex);

	for (auto& b: m_mappings) {
		if (VirtualRangesOverlap(vaddr, len, b.map_vaddr, b.map_size)) {
			CopyVirtualRangeName(b.name, name);
		}
	}
}

bool FlexibleMemory::Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode,
                         GpuAccessMode gpu_mode, const char* name) {
	Common::LockGuard lock(m_mutex);

	const auto available = (Size() >= m_allocated_total ? Size() - m_allocated_total : 0);
	if (len == 0 || len > available) {
		LOGF_COLOR(Log::Color::Red,
		           "\t flexible memory exhausted: configured = 0x%016" PRIx64
		           ", allocated = 0x%016" PRIx64 ", available = 0x%016" PRIx64
		           ", requested = 0x%016" PRIx64 "\n",
		           Size(), m_allocated_total, available, static_cast<uint64_t>(len));
		return false;
	}

	std::vector<AllocatedBlock> blocks;
	auto                        current   = vaddr;
	auto                        remaining = static_cast<uint64_t>(len);
	for (const auto& [backing_offset, free_size]: m_free) {
		if (remaining == 0) {
			break;
		}
		const auto     chunk = std::min(remaining, free_size);
		AllocatedBlock block {};
		block.map_vaddr      = current;
		block.map_size       = chunk;
		block.backing_offset = backing_offset;
		block.host_vaddr     = vaddr;
		block.host_size      = len;
		block.prot           = prot;
		block.mode           = mode;
		block.gpu_mode       = gpu_mode;
		CopyVirtualRangeName(block.name, name);
		blocks.push_back(block);
		current += chunk;
		remaining -= chunk;
	}
	if (remaining != 0) {
		return false;
	}

	std::vector<AllocatedBlock> mapped;
	for (const auto& block: blocks) {
		if (!g_guest_address_space->ZeroBacking(block.backing_offset, block.map_size)) {
			for (auto it = mapped.rbegin(); it != mapped.rend(); ++it) {
				EXIT_IF(!g_guest_address_space->UnmapBacking(it->map_vaddr, it->map_size));
			}
			return false;
		}
		if (!g_guest_address_space->MapBacking(block.map_vaddr, block.map_size,
		                                       block.backing_offset, block.mode)) {
			for (auto it = mapped.rbegin(); it != mapped.rend(); ++it) {
				EXIT_IF(!g_guest_address_space->UnmapBacking(it->map_vaddr, it->map_size));
			}
			return false;
		}
		mapped.push_back(block);
	}

	for (const auto& block: blocks) {
		auto next = m_free.upper_bound(block.backing_offset);
		EXIT_IF(next == m_free.begin());
		auto range = std::prev(next);
		EXIT_IF(block.backing_offset < range->first ||
		        block.map_size > range->first + range->second - block.backing_offset);
		ConsumeFreeRange(range, block.backing_offset, block.map_size);
		m_allocated.push_back(block);
	}
	std::sort(m_allocated.begin(), m_allocated.end(),
	          [](const auto& left, const auto& right) { return left.map_vaddr < right.map_vaddr; });
	m_allocated_total += len;

	return true;
}

bool FlexibleMemory::Unmap(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode,
                           uint64_t* host_vaddr_to_release) {
	EXIT_IF(gpu_mode == nullptr);

	Common::LockGuard lock(m_mutex);

	if (host_vaddr_to_release != nullptr) {
		*host_vaddr_to_release = 0;
	}
	if (size == 0 || UINT64_MAX - vaddr < size) {
		return false;
	}

	const auto end     = vaddr + size;
	auto       current = vaddr;
	bool       found   = false;
	for (const auto& block: m_allocated) {
		if (block.map_vaddr + block.map_size <= current) {
			continue;
		}
		if (block.map_vaddr > current || block.map_vaddr >= end) {
			break;
		}
		if (!found) {
			*gpu_mode = block.gpu_mode;
			found     = true;
		}
		current = std::min(end, block.map_vaddr + block.map_size);
		if (current == end) {
			break;
		}
	}
	if (!found || current != end || !g_guest_address_space->UnmapBacking(vaddr, size)) {
		return false;
	}

	std::vector<AllocatedBlock> remaining;
	remaining.reserve(m_allocated.size() + 1);
	uint64_t removed = 0;
	for (const auto& block: m_allocated) {
		const auto block_end = block.map_vaddr + block.map_size;
		if (!VirtualRangesOverlap(vaddr, size, block.map_vaddr, block.map_size)) {
			remaining.push_back(block);
			continue;
		}
		const auto overlap_start = std::max(vaddr, block.map_vaddr);
		const auto overlap_end   = std::min(end, block_end);
		const auto overlap_size  = overlap_end - overlap_start;
		AddFreeRange(block.backing_offset + overlap_start - block.map_vaddr, overlap_size);
		removed += overlap_size;

		if (block.map_vaddr < overlap_start) {
			auto left     = block;
			left.map_size = overlap_start - block.map_vaddr;
			remaining.push_back(left);
		}
		if (overlap_end < block_end) {
			auto right           = block;
			right.map_vaddr      = overlap_end;
			right.map_size       = block_end - overlap_end;
			right.backing_offset = block.backing_offset + overlap_end - block.map_vaddr;
			remaining.push_back(right);
		}
	}
	EXIT_IF(removed != size || removed > m_allocated_total);
	m_allocated = std::move(remaining);
	m_allocated_total -= removed;
	return true;
}

void FlexibleMemory::ConsumeFreeRange(std::map<uint64_t, uint64_t>::iterator range, uint64_t start,
                                      uint64_t size) {
	const auto range_start = range->first;
	const auto range_end   = range->first + range->second;
	m_free.erase(range);
	if (range_start < start) {
		m_free.emplace(range_start, start - range_start);
	}
	if (start + size < range_end) {
		m_free.emplace(start + size, range_end - start - size);
	}
}

void FlexibleMemory::AddFreeRange(uint64_t start, uint64_t size) {
	auto end  = start + size;
	auto next = m_free.lower_bound(start);
	if (next != m_free.begin()) {
		auto previous = std::prev(next);
		if (previous->first + previous->second >= start) {
			start = previous->first;
			end   = std::max(end, previous->first + previous->second);
			next  = m_free.erase(previous);
		}
	}
	while (next != m_free.end() && next->first <= end) {
		end  = std::max(end, next->first + next->second);
		next = m_free.erase(next);
	}
	m_free.emplace(start, end - start);
}

bool FlexibleMemory::Snapshot(uint64_t vaddr, uint64_t size, std::vector<AllocatedBlock>* blocks) {
	EXIT_IF(blocks == nullptr);
	Common::LockGuard lock(m_mutex);
	blocks->clear();
	if (size == 0 || UINT64_MAX - vaddr < size) {
		return false;
	}

	const auto end     = vaddr + size;
	auto       current = vaddr;
	for (const auto& block: m_allocated) {
		const auto block_end = block.map_vaddr + block.map_size;
		if (block_end <= current) {
			continue;
		}
		if (block.map_vaddr > current || block.map_vaddr >= end) {
			break;
		}
		const auto part_end = std::min(end, block_end);
		auto       part     = block;
		part.map_vaddr      = current;
		part.map_size       = part_end - current;
		part.backing_offset += current - block.map_vaddr;
		blocks->push_back(part);
		current = part_end;
		if (current == end) {
			return true;
		}
	}
	blocks->clear();
	return false;
}

bool FlexibleMemory::Restore(const std::vector<AllocatedBlock>& blocks) {
	Common::LockGuard lock(m_mutex);
	if (blocks.empty()) {
		return false;
	}

	for (const auto& block: blocks) {
		auto next = m_free.upper_bound(block.backing_offset);
		if (next == m_free.begin()) {
			return false;
		}
		const auto range = std::prev(next);
		if (block.backing_offset < range->first ||
		    block.map_size > range->first + range->second - block.backing_offset) {
			return false;
		}
	}

	std::vector<AllocatedBlock> mapped;
	for (const auto& block: blocks) {
		if (!g_guest_address_space->MapBacking(block.map_vaddr, block.map_size,
		                                       block.backing_offset, block.mode)) {
			for (auto it = mapped.rbegin(); it != mapped.rend(); ++it) {
				EXIT_IF(!g_guest_address_space->UnmapBacking(it->map_vaddr, it->map_size));
			}
			return false;
		}
		mapped.push_back(block);
	}

	for (const auto& block: blocks) {
		auto next = m_free.upper_bound(block.backing_offset);
		EXIT_IF(next == m_free.begin());
		auto range = std::prev(next);
		ConsumeFreeRange(range, block.backing_offset, block.map_size);
		m_allocated.push_back(block);
		m_allocated_total += block.map_size;
	}
	std::sort(m_allocated.begin(), m_allocated.end(),
	          [](const auto& left, const auto& right) { return left.map_vaddr < right.map_vaddr; });
	return true;
}

void FlexibleMemory::Protect(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode,
                             GpuAccessMode gpu_mode) {
	Common::LockGuard lock(m_mutex);
	if (size == 0 || UINT64_MAX - vaddr < size) {
		return;
	}

	const auto                  end = vaddr + size;
	std::vector<AllocatedBlock> updated;
	updated.reserve(m_allocated.size() + 2);
	for (const auto& block: m_allocated) {
		const auto block_end = block.map_vaddr + block.map_size;
		if (!VirtualRangesOverlap(vaddr, size, block.map_vaddr, block.map_size)) {
			updated.push_back(block);
			continue;
		}
		const auto overlap_start = std::max(vaddr, block.map_vaddr);
		const auto overlap_end   = std::min(end, block_end);
		if (block.map_vaddr < overlap_start) {
			auto left     = block;
			left.map_size = overlap_start - block.map_vaddr;
			updated.push_back(left);
		}
		auto middle           = block;
		middle.map_vaddr      = overlap_start;
		middle.map_size       = overlap_end - overlap_start;
		middle.backing_offset = block.backing_offset + overlap_start - block.map_vaddr;
		middle.prot           = prot;
		middle.mode           = mode;
		middle.gpu_mode       = gpu_mode;
		updated.push_back(middle);
		if (overlap_end < block_end) {
			auto right           = block;
			right.map_vaddr      = overlap_end;
			right.map_size       = block_end - overlap_end;
			right.backing_offset = block.backing_offset + overlap_end - block.map_vaddr;
			updated.push_back(right);
		}
	}
	m_allocated = std::move(updated);
}

bool FlexibleMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot,
                          VirtualMemory::Mode* mode, GpuAccessMode* gpu_mode) {
	Common::LockGuard lock(m_mutex);

	return std::any_of(m_allocated.begin(), m_allocated.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b) {
		                   if (vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size) {
			                   if (base_addr != nullptr) {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr) {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr) {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr) {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr) {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

void FlexibleMemory::SetVirtualRangeName(uint64_t vaddr, uint64_t len, const char* name) {
	Common::LockGuard lock(m_mutex);

	for (auto& b: m_allocated) {
		if (VirtualRangesOverlap(vaddr, len, b.map_vaddr, b.map_size)) {
			CopyVirtualRangeName(b.name, name);
		}
	}
}

void PhysicalMemory::SetVirtualRangeMemoryType(uint64_t vaddr, uint64_t len, int memory_type) {
	Common::LockGuard lock(m_mutex);

	for (auto& b: m_mappings) {
		if (VirtualRangesOverlap(vaddr, len, b.map_vaddr, b.map_size)) {
			b.memory_type = memory_type;
		}
	}
}

uint64_t FlexibleMemory::Available() {
	Common::LockGuard lock(m_mutex);

	return (Size() >= m_allocated_total ? Size() - m_allocated_total : 0);
}

void PooledMemory::AddFreeUnlocked(uint64_t start, uint64_t size) {
	if (size == 0) {
		return;
	}

	m_free.push_back({start, size});
	std::sort(m_free.begin(), m_free.end(),
	          [](const auto& left, const auto& right) { return left.start < right.start; });

	std::vector<PhysicalRange> merged;
	for (const auto& range: m_free) {
		if (!merged.empty() && range.start <= merged.back().start + merged.back().size) {
			const auto end =
			    std::max(merged.back().start + merged.back().size, range.start + range.size);
			merged.back().size = end - merged.back().start;
		} else {
			merged.push_back(range);
		}
	}
	m_free = std::move(merged);
}

void PooledMemory::Expand(uint64_t phys_addr, uint64_t size) {
	Common::LockGuard lock(m_mutex);
	m_expansions.push_back({phys_addr, size});
	AddFreeUnlocked(phys_addr, size);
}

bool PooledMemory::ReleaseExpansion(uint64_t phys_addr, uint64_t size) {
	Common::LockGuard lock(m_mutex);
	const auto        expansion = std::find_if(m_expansions.begin(), m_expansions.end(),
	                                           [phys_addr, size](const auto& range) {
		                                    return range.start == phys_addr && range.size == size;
	                                           });
	if (expansion == m_expansions.end()) {
		return false;
	}
	if (std::any_of(m_mappings.begin(), m_mappings.end(), [phys_addr, size](const auto& mapping) {
		    return VirtualRangesOverlap(phys_addr, size, mapping.phys_addr, mapping.size);
	    })) {
		return false;
	}

	const auto end = phys_addr + size;
	const auto free_range =
	    std::find_if(m_free.begin(), m_free.end(), [phys_addr, end](const auto& r) {
		    return phys_addr >= r.start && end <= r.start + r.size;
	    });
	if (free_range == m_free.end()) {
		return false;
	}

	const auto old        = *free_range;
	const auto old_end    = old.start + old.size;
	const auto left_size  = phys_addr - old.start;
	const auto right_size = old_end - end;
	m_free.erase(free_range);
	if (left_size != 0) {
		m_free.push_back({old.start, left_size});
	}
	if (right_size != 0) {
		m_free.push_back({end, right_size});
	}
	std::sort(m_free.begin(), m_free.end(),
	          [](const auto& left, const auto& right) { return left.start < right.start; });
	m_expansions.erase(expansion);
	return true;
}

bool PooledMemory::Allocate(uint64_t vaddr, uint64_t size, GpuAccessMode gpu_mode,
                            std::vector<Mapping>* mappings) {
	EXIT_IF(mappings == nullptr);
	mappings->clear();
	if (vaddr == 0 || size == 0 || UINT64_MAX - vaddr < size) {
		return false;
	}

	Common::LockGuard lock(m_mutex);
	auto              free      = m_free;
	auto              current   = vaddr;
	auto              remaining = size;
	for (auto& range: free) {
		if (remaining == 0 || range.size == 0) {
			continue;
		}
		const auto part_size = std::min(range.size, remaining);
		mappings->push_back({current, part_size, range.start, gpu_mode});
		range.start += part_size;
		range.size -= part_size;
		current += part_size;
		remaining -= part_size;
	}
	if (remaining != 0) {
		mappings->clear();
		return false;
	}

	free.erase(
	    std::remove_if(free.begin(), free.end(), [](const auto& range) { return range.size == 0; }),
	    free.end());
	m_free = std::move(free);
	m_mappings.insert(m_mappings.end(), mappings->begin(), mappings->end());
	return true;
}

bool PooledMemory::QueryUnlocked(uint64_t vaddr, uint64_t size,
                                 std::vector<Mapping>* mappings) const {
	EXIT_IF(mappings == nullptr);
	mappings->clear();
	if (vaddr == 0 || size == 0 || UINT64_MAX - vaddr < size) {
		return false;
	}

	const auto end     = vaddr + size;
	auto       current = vaddr;
	while (current < end) {
		const auto it =
		    std::find_if(m_mappings.begin(), m_mappings.end(), [current](const auto& m) {
			    return current >= m.vaddr && current < m.vaddr + m.size;
		    });
		if (it == m_mappings.end()) {
			mappings->clear();
			return false;
		}
		const auto part_size = std::min(end, it->vaddr + it->size) - current;
		mappings->push_back(
		    {current, part_size, it->phys_addr + (current - it->vaddr), it->gpu_mode});
		current += part_size;
	}
	return true;
}

bool PooledMemory::Query(uint64_t vaddr, uint64_t size, std::vector<Mapping>* mappings) {
	Common::LockGuard lock(m_mutex);
	return QueryUnlocked(vaddr, size, mappings);
}

bool PooledMemory::Release(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode) {
	EXIT_IF(gpu_mode == nullptr);

	Common::LockGuard    lock(m_mutex);
	std::vector<Mapping> released;
	if (!QueryUnlocked(vaddr, size, &released)) {
		return false;
	}

	*gpu_mode                = released.front().gpu_mode;
	const auto           end = vaddr + size;
	std::vector<Mapping> kept;
	for (const auto& mapping: m_mappings) {
		const auto mapping_end = mapping.vaddr + mapping.size;
		const auto cut_start   = std::max(vaddr, mapping.vaddr);
		const auto cut_end     = std::min(end, mapping_end);
		if (cut_start >= cut_end) {
			kept.push_back(mapping);
			continue;
		}
		if (mapping.vaddr < cut_start) {
			auto left = mapping;
			left.size = cut_start - mapping.vaddr;
			kept.push_back(left);
		}
		if (cut_end < mapping_end) {
			auto right      = mapping;
			right.vaddr     = cut_end;
			right.size      = mapping_end - cut_end;
			right.phys_addr = mapping.phys_addr + (cut_end - mapping.vaddr);
			kept.push_back(right);
		}
	}
	m_mappings = std::move(kept);
	for (const auto& mapping: released) {
		AddFreeUnlocked(mapping.phys_addr, mapping.size);
	}
	return true;
}

uint64_t PooledMemory::Available() {
	Common::LockGuard lock(m_mutex);
	uint64_t          available = 0;
	for (const auto& range: m_free) {
		available += range.size;
	}
	return available;
}

std::vector<PooledMemory::Mapping> PooledMemory::GetMappings() {
	Common::LockGuard lock(m_mutex);
	return m_mappings;
}

static bool UnmapPooledBackingTransactional(const std::vector<PooledMemory::Mapping>& mappings,
                                            VirtualMemory::Mode                       mode) {
	std::vector<PooledMemory::Mapping> removed;
	for (const auto& mapping: mappings) {
		if (!g_guest_address_space->UnmapBacking(mapping.vaddr, mapping.size)) {
			for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
				auto       failure_reason = GuestBackingStore::FailureReason::None;
				const bool restored       = g_guest_address_space->MapBacking(
				    it->vaddr, it->size, it->phys_addr, mode, &failure_reason);
				if (!restored) {
					EXIT("pooled-memory unmap rollback failed: %s\n",
					     GuestBackingStore::GetFailureReasonName(failure_reason));
				}
			}
			return false;
		}
		removed.push_back(mapping);
	}
	return true;
}

int32_t KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot,
                                                   int flags, const char* name) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

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
	GpuAccessMode       gpu_mode = GpuAccessMode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode, &gpu_mode)) {
		EXIT("unknown prot: %d\n", prot);
	}

	auto                 in_addr              = reinterpret_cast<uint64_t>(*addr_in_out);
	uint64_t             out_addr             = 0;
	bool                 consumed_reservation = false;
	VirtualRanges::Range consumed_range {};

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
			consumed_range = reserved_ranges.front();
			if (g_virtual_ranges->ConsumeReservedSpan(in_addr, len)) {
				consumed_reservation = true;
				out_addr             = in_addr;
			}
		}
		if (!consumed_reservation && ReplaceFixedRangeWithReserved(in_addr, len) &&
		    g_virtual_ranges->ConsumeReservedSpan(in_addr, len, &consumed_range)) {
			consumed_reservation = true;
			out_addr             = in_addr;
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
		if (consumed_reservation) {
			g_virtual_ranges->Add(in_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                      consumed_range.name);
		}
		return KERNEL_ERROR_ENOMEM;
	}

	if (!g_flexible_memory->Map(out_addr, len, prot, mode, gpu_mode, name)) {
		LOGF_COLOR(Log::Color::Red, "\t [Fail]\n");
		if (consumed_reservation) {
			EXIT_IF(!g_virtual_ranges->Add(out_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                               consumed_range.name));
		}
		return KERNEL_ERROR_ENOMEM;
	}

	if (!g_virtual_ranges->Add(out_addr, len, 0, prot, 0, VirtualRangeType::Flexible, name,
	                           (map_flags & GUEST_MAP_NO_COALESCE) != 0)) {
		GpuAccessMode rollback_gpu_mode = GpuAccessMode::NoAccess;
		EXIT_IF(!g_flexible_memory->Unmap(out_addr, len, &rollback_gpu_mode));
		if (consumed_reservation) {
			EXIT_IF(!g_virtual_ranges->Add(out_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                               consumed_range.name));
		}
		return KERNEL_ERROR_EBUSY;
	}

	LOGF("\t in_addr  = 0x%016" PRIx64 "\n"
	     "\t out_addr = 0x%016" PRIx64 "\n"
	     "\t size     = %" PRIu64 "\n"
	     "\t mode     = %s\n"
	     "\t flags    = 0x%08" PRIx32 "\n"
	     "\t name     = %s\n"
	     "\t gpu_mode = %s\n",
	     in_addr, out_addr, len, Common::EnumName(mode).c_str(), static_cast<uint32_t>(flags), name,
	     Common::EnumName(gpu_mode).c_str());

	MapGpuRange(out_addr, len);

	if (g_alloc_callback != nullptr) {
		g_alloc_callback(out_addr, len);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags) {
	return KernelMapNamedFlexibleMemory(addr_in_out, len, prot, flags, "");
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

	g_physical_memory->SetVirtualRangeName(vaddr, len, name);
	g_flexible_memory->SetVirtualRangeName(vaddr, len, name);
	g_virtual_ranges->Rename(vaddr, len, name);

	return OK;
}

static bool FreeGuestMemoryOwner(uint64_t vaddr, uint64_t size) {
	return g_guest_address_space->ReleaseCommitted(vaddr, size) &&
	       g_virtual_ranges->Remove(vaddr, size);
}

static int UnmapMemoryRange(uint64_t vaddr, size_t len) {
	if (len == 0 || UINT64_MAX - vaddr < len) {
		return KERNEL_ERROR_EINVAL;
	}

	VirtualRanges::Range range {};
	if (!g_virtual_ranges->Query(vaddr, 0, &range)) {
		return KERNEL_ERROR_EACCES;
	}
	const auto chunk_len = std::min<uint64_t>(len, range.size - (vaddr - range.start));
	if (chunk_len < len) {
		const int ret = UnmapMemoryRange(vaddr, chunk_len);
		return ret == OK ? UnmapMemoryRange(vaddr + chunk_len, len - chunk_len) : ret;
	}
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

	GpuAccessMode gpu_mode       = GpuAccessMode::NoAccess;
	bool          owner_unmapped = false;
	if (range.type == VirtualRangeType::Pooled) {
		std::vector<PooledMemory::Mapping> mappings;
		if (g_pooled_memory->Query(vaddr, len, &mappings)) {
			VirtualMemory::Mode mode        = VirtualMemory::Mode::NoAccess;
			GpuAccessMode       decoded_gpu = GpuAccessMode::NoAccess;
			owner_unmapped = DecodeMemoryProtection(range.protection, &mode, &decoded_gpu) &&
			                 UnmapPooledBackingTransactional(mappings, mode);
			if (owner_unmapped && !g_pooled_memory->Release(vaddr, len, &gpu_mode)) {
				EXIT("failed to release unmapped pooled-memory range\n");
			}
		}
		if (!owner_unmapped) {
			return KERNEL_ERROR_EACCES;
		}
	} else if (range.type == VirtualRangeType::Direct) {
		VirtualMemory::Mode direct_mode   = VirtualMemory::Mode::NoAccess;
		GpuAccessMode       direct_gpu    = GpuAccessMode::NoAccess;
		const auto          direct_offset = range.offset + vaddr - range.start;
		if (DecodeMemoryProtection(range.protection, &direct_mode, &direct_gpu) &&
		    g_guest_address_space->BackingContains(vaddr, len) &&
		    g_guest_address_space->UnmapBacking(vaddr, len)) {
			uint64_t ignored_host = 0;
			if (g_physical_memory->Unmap(vaddr, len, &gpu_mode, &ignored_host)) {
				owner_unmapped = true;
			} else {
				EXIT_IF(!g_guest_address_space->MapBacking(vaddr, len, direct_offset, direct_mode));
			}
		}
	} else if (range.type == VirtualRangeType::Stack) {
		owner_unmapped = g_guest_address_space->ReleaseCommitted(vaddr, len);
	} else {
		uint64_t ignored_host = 0;
		owner_unmapped        = g_flexible_memory->Unmap(vaddr, len, &gpu_mode, &ignored_host);
	}
	if (!owner_unmapped) {
		return KERNEL_ERROR_EACCES;
	}

	g_virtual_ranges->Remove(vaddr, len);

	if (g_free_callback != nullptr && IsCommittedRangeType(range.type)) {
		g_free_callback(vaddr, len);
	}
	if (range.type == VirtualRangeType::Pooled) {
		MemoryPoolSubtractCommitted(len);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

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
	UnmapGpuRange(vaddr, len);
	return UnmapMemoryRange(vaddr, len);
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

	*cpu_total     = PAGE_TABLE_POOL_ENTRIES;
	*gpu_total     = PAGE_TABLE_POOL_ENTRIES;
	*cpu_available = PAGE_TABLE_POOL_ENTRIES -
	                 static_cast<int>(std::min<uint64_t>(cpu_used, PAGE_TABLE_POOL_ENTRIES));
	*gpu_available = PAGE_TABLE_POOL_ENTRIES -
	                 static_cast<int>(std::min<uint64_t>(gpu_used, PAGE_TABLE_POOL_ENTRIES));

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

int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                             size_t alignment, int memory_type,
                                             int64_t* phys_addr_out) {
	PRINT_NAME();

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
	if (!g_physical_memory->Alloc(search_start, search_end, len, alignment, &addr, memory_type)) {
		LOGF_COLOR(Log::Color::Red, "\t[Fail]\n");
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	LOGF_COLOR(Log::Color::Green, "\tphys_addr    = %016" PRIx64 "\n\t[Ok]\n", addr);

	return OK;
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

static int ReleaseDirectMemoryInternal(int64_t start, size_t len) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	if (g_pooled_memory->ReleaseExpansion(static_cast<uint64_t>(start), len)) {
		if (!g_physical_memory->ReleasePoolExpansion(static_cast<uint64_t>(start), len)) {
			EXIT("failed to release physical pool expansion\n");
		}
		return OK;
	}

	std::vector<PhysicalMemory::AllocatedBlock> allocated_span;
	if (!g_physical_memory->GetAllocatedSpan(static_cast<uint64_t>(start), len, &allocated_span)) {
		return KERNEL_ERROR_EACCES;
	}

	const auto mapped_aliases = g_physical_memory->FindMappings(start, len);
	for (const auto& alias: mapped_aliases) {
		VirtualRanges::Range range {};
		if (!g_guest_address_space->BackingContains(alias.map_vaddr, alias.map_size) ||
		    !g_virtual_ranges->Query(alias.map_vaddr, 0, &range) ||
		    range.type != VirtualRangeType::Direct ||
		    alias.map_size > range.start + range.size - alias.map_vaddr) {
			EXIT("direct-memory alias escaped guest address-space ownership\n");
		}
	}

	for (const auto& alias: mapped_aliases) {
		UnmapGpuRange(alias.map_vaddr, alias.map_size);
	}

	auto restore_gpu_aliases = [&mapped_aliases]() {
		for (const auto& alias: mapped_aliases) {
			MapGpuRange(alias.map_vaddr, alias.map_size);
		}
	};
	auto restore_owner_aliases = [](const std::vector<PhysicalMemory::AllocatedBlock>& aliases) {
		for (auto it = aliases.rbegin(); it != aliases.rend(); ++it) {
			EXIT_IF(!g_guest_address_space->MapBacking(it->map_vaddr, it->map_size, it->start_addr,
			                                           it->mode));
		}
	};

	std::vector<PhysicalMemory::AllocatedBlock> owner_unmapped;
	for (const auto& alias: mapped_aliases) {
		if (!g_guest_address_space->UnmapBacking(alias.map_vaddr, alias.map_size)) {
			restore_owner_aliases(owner_unmapped);
			restore_gpu_aliases();
			return KERNEL_ERROR_EACCES;
		}
		owner_unmapped.push_back(alias);
	}

	std::vector<PhysicalMemory::AllocatedBlock> metadata_unmapped;
	for (const auto& alias: mapped_aliases) {
		GpuAccessMode alias_gpu_mode = GpuAccessMode::NoAccess;
		if (!g_physical_memory->Unmap(alias.map_vaddr, alias.map_size, &alias_gpu_mode)) {
			for (const auto& removed: metadata_unmapped) {
				EXIT_IF(!g_physical_memory->Map(removed.map_vaddr, removed.start_addr,
				                                removed.map_size, removed.prot, removed.mode,
				                                removed.gpu_mode));
			}
			restore_owner_aliases(owner_unmapped);
			restore_gpu_aliases();
			return KERNEL_ERROR_EACCES;
		}
		metadata_unmapped.push_back(alias);
	}

	for (const auto& alias: mapped_aliases) {
		EXIT_IF(!g_virtual_ranges->Remove(alias.map_vaddr, alias.map_size));
	}

	for (const auto& block: allocated_span) {
		uint64_t      unused_vaddr = 0;
		uint64_t      unused_size  = 0;
		GpuAccessMode unused_gpu   = GpuAccessMode::NoAccess;
		EXIT_IF(!g_physical_memory->Release(block.start_addr, block.size, &unused_vaddr,
		                                    &unused_size, &unused_gpu));
	}

	if (g_free_callback != nullptr) {
		for (const auto& alias: mapped_aliases) {
			g_free_callback(alias.map_vaddr, alias.map_size);
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
		(void)ReleaseDirectMemoryInternal(start, len);
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

	const int result = ReleaseDirectMemoryInternal(start, len);
	return result == KERNEL_ERROR_EACCES ? KERNEL_ERROR_ENOENT : result;
}

int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        int64_t direct_memory_start, size_t alignment) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

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
	GpuAccessMode       gpu_mode = GpuAccessMode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode, &gpu_mode)) {
		return KERNEL_ERROR_EINVAL;
	}
	if (!g_physical_memory->CanMapDirect(static_cast<uint64_t>(direct_memory_start), len)) {
		return KERNEL_ERROR_ENOMEM;
	}

	auto                 in_addr              = reinterpret_cast<uint64_t>(*addr);
	uint64_t             out_addr             = 0;
	bool                 shared_backing       = false;
	bool                 consumed_reservation = false;
	VirtualRanges::Range consumed_range {};
	auto                 shared_failure = GuestBackingStore::FailureReason::None;
	// Direct mappings must remain views of the single backing object. Anonymous fallbacks break
	// aliasing and lose direct-memory contents when a range is unmapped and mapped again.
	auto map_shared_fixed = [&](uint64_t target_addr) -> bool {
		return g_guest_address_space->MapBacking(target_addr, len, direct_memory_start, mode,
		                                         &shared_failure);
	};
	auto map_consumed_reserved_fixed = [&]() {
		if (map_shared_fixed(in_addr)) {
			out_addr       = in_addr;
			shared_backing = true;
		}
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
		if (g_virtual_ranges->QuerySpan(in_addr, len, &reserved_ranges) &&
		    std::all_of(reserved_ranges.begin(), reserved_ranges.end(), [](const auto& range) {
			    return range.type == VirtualRangeType::Reserved;
		    })) {
			UnmapGpuRange(in_addr, len);
			consumed_range = reserved_ranges.front();
			if (g_virtual_ranges->ConsumeReservedSpan(in_addr, len)) {
				consumed_reservation = true;
				map_consumed_reserved_fixed();
			}
		}
		if (!consumed_reservation && ReplaceFixedRangeWithReserved(in_addr, len) &&
		    g_virtual_ranges->ConsumeReservedSpan(in_addr, len, &consumed_range)) {
			consumed_reservation = true;
			map_consumed_reserved_fixed();
		}
		if (!consumed_reservation) {
			return KERNEL_ERROR_ENOMEM;
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
			consumed_range = reserved_ranges.front();
			if (g_virtual_ranges->ConsumeReservedSpan(in_addr, len)) {
				consumed_reservation = true;
				if (map_shared_fixed(in_addr)) {
					out_addr       = in_addr;
					shared_backing = true;
				}
			}
		}
		if (!consumed_reservation) {
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
	     "\t gpu_mode = %s\n"
	     "\t shared   = %s\n"
	     "\t reason   = %s\n",
	     in_addr, out_addr, static_cast<uint64_t>(direct_memory_start), len,
	     Common::EnumName(mode).c_str(), static_cast<uint32_t>(flags), alignment,
	     Common::EnumName(gpu_mode).c_str(), shared_backing ? "yes" : "no", shared_reason);

	if (out_addr == 0) {
		if (consumed_reservation) {
			g_virtual_ranges->Add(in_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                      consumed_range.name);
		}
		return KERNEL_ERROR_ENOMEM;
	}

	if (!g_physical_memory->Map(out_addr, direct_memory_start, len, prot, mode, gpu_mode)) {
		LOGF_COLOR(Log::Color::Red, "\t [Fail]\n");
		EXIT_IF(!g_guest_address_space->UnmapBacking(out_addr, len));
		if (consumed_reservation) {
			EXIT_IF(!g_virtual_ranges->Add(in_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                               consumed_range.name));
		}
		return KERNEL_ERROR_EBUSY;
	}

	PhysicalMemory::AllocatedBlock mapped_block {};
	g_physical_memory->Find(direct_memory_start, false, &mapped_block);
	if (!g_virtual_ranges->Add(out_addr, len, direct_memory_start, prot, mapped_block.memory_type,
	                           VirtualRangeType::Direct, "")) {
		GpuAccessMode rollback_gpu_mode = GpuAccessMode::NoAccess;
		EXIT_IF(!g_physical_memory->Unmap(out_addr, len, &rollback_gpu_mode));
		EXIT_IF(!g_guest_address_space->UnmapBacking(out_addr, len));
		if (consumed_reservation) {
			EXIT_IF(!g_virtual_ranges->Add(in_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                               consumed_range.name));
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

int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags,
                                         int64_t direct_memory_start, size_t alignment) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t type = %d\n", type);

	auto ret = KernelMapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment);
	if (ret == OK && addr != nullptr && *addr != nullptr) {
		const auto out_addr = reinterpret_cast<uint64_t>(*addr);
		g_physical_memory->SetVirtualRangeMemoryType(out_addr, len, type);
		g_virtual_ranges->SetMemoryType(out_addr, len, type);
	}

	return ret;
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

	auto ret = KernelMapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment);
	if (ret == OK && addr != nullptr) {
		g_physical_memory->SetVirtualRangeName(reinterpret_cast<uint64_t>(*addr), len, name);
		g_virtual_ranges->Rename(reinterpret_cast<uint64_t>(*addr), len, name);
	}

	return ret;
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
		GpuAccessMode                               gpu_mode = GpuAccessMode::NoAccess;
		std::vector<FlexibleMemory::AllocatedBlock> flexible_blocks;
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
		DecodeMemoryProtection(replaced.range.protection, &replaced.mode, &replaced.gpu_mode);
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
		chunks.push_back(replaced);

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
					backend_restored = g_physical_memory->Map(
					    chunk.range.start, chunk.range.offset, chunk.range.size,
					    chunk.range.protection, chunk.mode, chunk.gpu_mode);
				}
			} else if (chunk.range.type == VirtualRangeType::Flexible && chunk.backend_unmapped) {
				host_restored = backend_restored =
				    g_flexible_memory->Restore(chunk.flexible_blocks);
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
		GpuAccessMode gpu_mode = GpuAccessMode::NoAccess;
		bool          unmapped = true;

		if (chunk.range.type == VirtualRangeType::Direct) {
			if (!g_guest_address_space->UnmapBacking(chunk.range.start, chunk.range.size)) {
				unmapped = false;
			} else {
				chunk.host_unmapped = true;
			}
			if (unmapped) {
				unmapped = g_physical_memory->Unmap(chunk.range.start, chunk.range.size, &gpu_mode);
				chunk.gpu_mode         = gpu_mode;
				chunk.backend_unmapped = unmapped;
			}
		} else if (chunk.range.type == VirtualRangeType::Flexible) {
			unmapped = g_flexible_memory->Unmap(chunk.range.start, chunk.range.size, &gpu_mode);
			chunk.host_unmapped    = unmapped;
			chunk.backend_unmapped = unmapped;
			chunk.gpu_mode         = gpu_mode;
		} else if (IsPrivateCommittedRangeType(chunk.range.type)) {
			unmapped = g_guest_address_space->ReleaseCommitted(chunk.range.start, chunk.range.size);
			chunk.host_unmapped    = unmapped;
			chunk.backend_unmapped = unmapped;
		} else if (IsReservedRangeType(chunk.range.type)) {
			unmapped = g_guest_address_space->ReleaseFree(chunk.range.start, chunk.range.size);
		} else {
			unmapped = false;
		}
		if (chunk.range.type == VirtualRangeType::Direct) {
			chunk.gpu_mode         = gpu_mode;
			chunk.backend_unmapped = chunk.backend_unmapped && unmapped;
		}

		if (!unmapped) {
			LOGF_COLOR(Log::Color::Red,
			           "\t reserve-fixed replace: backend unmap failed at 0x%016" PRIx64
			           ", size=0x%016" PRIx64 ", type=%s\n",
			           chunk.range.start, chunk.range.size,
			           Common::EnumName(chunk.range.type).c_str());
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

uint64_t TestGuestBackingSize() {
	return g_guest_address_space->GetBackingSize();
}

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

int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

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
	GpuAccessMode       gpu_mode = GpuAccessMode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode, &gpu_mode)) {
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
	for (const auto& old_range: old_ranges) {
		if (old_range.type == VirtualRangeType::Direct) {
			g_physical_memory->ProtectMapping(old_range.start, old_range.size, prot, mode,
			                                  gpu_mode);
		} else if (old_range.type == VirtualRangeType::Flexible) {
			g_flexible_memory->Protect(old_range.start, old_range.size, prot, mode, gpu_mode);
		}
	}
	g_virtual_ranges->Protect(aligned_addr, aligned_len, prot);

	LOGF("\t prot: %s -> %s\n", Common::EnumName(old_mode).c_str(), Common::EnumName(mode).c_str());

	return OK;
}

int KYTY_SYSV_ABI KernelMtypeprotect(const void* addr, size_t len, int type, int prot) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t addr = 0x%016" PRIx64 "\n"
	     "\t len  = 0x%016" PRIx64 "\n"
	     "\t type = 0x%08" PRIx32 "\n"
	     "\t prot = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(addr), static_cast<uint64_t>(len), static_cast<uint32_t>(type),
	     static_cast<uint32_t>(prot));

	return KernelMprotect(addr, len, prot);
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
		     static_cast<uint32_t>(static_cast<unsigned char>(entry->protection)),
		     static_cast<uint32_t>(static_cast<unsigned char>(entry->type)), entry->operation);

		if (entry->length == 0 || entry->operation < MAP_OP_MAP_DIRECT ||
		    entry->operation > MAP_OP_TYPE_PROTECT) {
			break;
		}

		int result = OK;
		switch (entry->operation) {
			case MAP_OP_MAP_DIRECT:
				result = KernelMapNamedDirectMemory(&entry->start, entry->length, entry->protection,
				                                    flags, static_cast<int64_t>(entry->offset), 0,
				                                    "anon");
				break;
			case MAP_OP_UNMAP:
				result = KernelMunmap(reinterpret_cast<uint64_t>(entry->start), entry->length);
				break;
			case MAP_OP_PROTECT:
				result = KernelMprotect(entry->start, entry->length, entry->protection);
				break;
			case MAP_OP_MAP_FLEXIBLE:
				result = KernelMapNamedFlexibleMemory(&entry->start, entry->length,
				                                      entry->protection, flags, "anon");
				break;
			case MAP_OP_TYPE_PROTECT:
				result =
				    KernelMtypeprotect(entry->start, entry->length, entry->type, entry->protection);
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

static bool IsPowerOfTwo(uint64_t value) {
	return value != 0 && (value & (value - 1u)) == 0;
}

static void MemoryPoolSubtractCommitted(uint64_t len) {
	auto current = g_memory_pool_committed.load(std::memory_order_relaxed);
	while (current != 0) {
		const auto next = (current > len ? current - len : 0);
		if (g_memory_pool_committed.compare_exchange_weak(current, next,
		                                                  std::memory_order_relaxed)) {
			return;
		}
	}
}

int KYTY_SYSV_ABI KernelMemoryPoolExpand(int64_t search_start, int64_t search_end, size_t len,
                                         size_t alignment, int64_t* phys_addr_out) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	constexpr uint64_t POOL_PAGE_SIZE = 0x10000;
	if (search_start < 0 || search_end <= search_start || len == 0 ||
	    (len & (POOL_PAGE_SIZE - 1u)) != 0 || phys_addr_out == nullptr ||
	    (alignment != 0 &&
	     (!IsPowerOfTwo(alignment) || (alignment & (POOL_PAGE_SIZE - 1u)) != 0))) {
		return KERNEL_ERROR_EINVAL;
	}
	if (static_cast<uint64_t>(search_end - search_start) < len) {
		return KERNEL_ERROR_ENOMEM;
	}

	const auto effective_alignment = (alignment != 0 ? alignment : POOL_PAGE_SIZE);
	uint64_t   phys_addr           = 0;
	if (!g_physical_memory->Alloc(static_cast<uint64_t>(search_start),
	                              static_cast<uint64_t>(search_end), len, effective_alignment,
	                              &phys_addr, 0, true)) {
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
	    (!IsPowerOfTwo(alignment) || !IsAligned(alignment, POOL_RESERVE_ALIGNMENT))) {
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
		g_virtual_ranges->Remove(out_vaddr, len);
		if (!g_virtual_ranges->Add(out_vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved,
		                           reserved_range.name)) {
			g_virtual_ranges->Add(out_vaddr, len, reserved_range.offset, reserved_range.protection,
			                      reserved_range.memory_type, reserved_range.type,
			                      reserved_range.name);
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
	GpuAccessMode       gpu_mode = GpuAccessMode::NoAccess;
	if (!DecodeMemoryProtection(prot, &mode, &gpu_mode)) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto vaddr = reinterpret_cast<uint64_t>(addr);

	VirtualRanges::Range old_range {};
	if (!g_virtual_ranges->Query(vaddr, 0, &old_range) ||
	    old_range.type != VirtualRangeType::PoolReserved) {
		return KERNEL_ERROR_EACCES;
	}

	if (!g_virtual_ranges->ConsumeReserved(vaddr, len, VirtualRangeType::PoolReserved)) {
		return KERNEL_ERROR_EACCES;
	}

	std::vector<PooledMemory::Mapping> mappings;
	if (!g_pooled_memory->Allocate(vaddr, len, gpu_mode, &mappings)) {
		g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved, old_range.name);
		return KERNEL_ERROR_ENOMEM;
	}

	std::vector<PooledMemory::Mapping> mapped;
	auto                               rollback = [&]() {
		if (!UnmapPooledBackingTransactional(mapped, mode)) {
			EXIT("failed to roll back pooled-memory backing maps\n");
		}
		GpuAccessMode rollback_gpu_mode = GpuAccessMode::NoAccess;
		if (!g_pooled_memory->Release(vaddr, len, &rollback_gpu_mode)) {
			EXIT("failed to release pooled-memory rollback allocation\n");
		}
		g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved, old_range.name);
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
		mapped.push_back(mapping);
	}

	if (!g_virtual_ranges->Add(vaddr, len, 0, prot, type, VirtualRangeType::Pooled,
	                           old_range.name)) {
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

static int DecommitMemoryPoolRange(uint64_t vaddr, size_t len) {
	VirtualRanges::Range old_range {};
	if (!g_virtual_ranges->Query(vaddr, 0, &old_range)) {
		return KERNEL_ERROR_EACCES;
	}
	const auto chunk_len = std::min<uint64_t>(len, old_range.size - (vaddr - old_range.start));
	if (old_range.type == VirtualRangeType::PoolReserved) {
		return chunk_len < len ? DecommitMemoryPoolRange(vaddr + chunk_len, len - chunk_len) : OK;
	}
	if (old_range.type != VirtualRangeType::Pooled) {
		return KERNEL_ERROR_EACCES;
	}
	if (chunk_len < len) {
		const int ret = DecommitMemoryPoolRange(vaddr, chunk_len);
		return ret == OK ? DecommitMemoryPoolRange(vaddr + chunk_len, len - chunk_len) : ret;
	}

	std::vector<PooledMemory::Mapping> mappings;
	if (!g_pooled_memory->Query(vaddr, len, &mappings)) {
		return KERNEL_ERROR_EACCES;
	}

	VirtualMemory::Mode mode        = VirtualMemory::Mode::NoAccess;
	GpuAccessMode       decoded_gpu = GpuAccessMode::NoAccess;
	if (!DecodeMemoryProtection(old_range.protection, &mode, &decoded_gpu)) {
		return KERNEL_ERROR_EACCES;
	}
	if (!UnmapPooledBackingTransactional(mappings, mode)) {
		EXIT("pooled-memory backing transaction failed after GPU unmap: addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, len);
	}

	GpuAccessMode gpu_mode = GpuAccessMode::NoAccess;
	if (!g_pooled_memory->Release(vaddr, len, &gpu_mode)) {
		EXIT("failed to release decommitted pooled-memory range\n");
	}

	g_virtual_ranges->Remove(vaddr, len);
	g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved, old_range.name);

	if (g_free_callback != nullptr) {
		g_free_callback(vaddr, len);
	}

	MemoryPoolSubtractCommitted(len);
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
	const auto end  = vaddr + len;
	auto       scan = vaddr;
	while (scan < end) {
		VirtualRanges::Range scan_range {};
		if (!g_virtual_ranges->Query(scan, 0, &scan_range) ||
		    (scan_range.type != VirtualRangeType::Pooled &&
		     scan_range.type != VirtualRangeType::PoolReserved)) {
			return KERNEL_ERROR_EACCES;
		}
		const auto next = std::min(end, scan_range.start + scan_range.size);
		if (next <= scan) {
			return KERNEL_ERROR_EACCES;
		}
		scan = next;
	}

	UnmapGpuRange(vaddr, len);
	return DecommitMemoryPoolRange(vaddr, len);
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

} // namespace Libs::LibKernel::Memory
