#include "kernel/memory.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
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
constexpr uint64_t DEFAULT_FLEXIBLE_MEMORY_SIZE = 4ull * 1024ull * 1024ull * 1024ull;

static uint64_t g_flexible_memory_size = DEFAULT_FLEXIBLE_MEMORY_SIZE;

static Graphics::GpuResourceManager& GetGpuResources() {
	return *Graphics::g_render_ctx->GetGpuResources();
}

static bool IsGpuAddressRange(uint64_t vaddr, uint64_t size) {
	constexpr uint64_t GPU_ADDRESS_LIMIT = 1ull << 40u;
	return vaddr != 0 && size != 0 && vaddr < GPU_ADDRESS_LIMIT &&
	       size <= GPU_ADDRESS_LIMIT - vaddr;
}

static void MapGpuRange(uint64_t vaddr, uint64_t size, GpuAccessMode mode) {
	if (mode == GpuAccessMode::NoAccess) {
		return;
	}
	if (!IsGpuAddressRange(vaddr, size)) {
		EXIT("invalid GPU map range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr, size);
	}
	const auto access = mode == GpuAccessMode::Read    ? Graphics::GpuAccess::Read
	                    : mode == GpuAccessMode::Write ? Graphics::GpuAccess::Write
	                                                   : Graphics::GpuAccess::ReadWrite;
	GetGpuResources().MapMemory(vaddr, size, access);
}

static void UnmapGpuRange(uint64_t vaddr, uint64_t size, GpuAccessMode mode) {
	if (mode == GpuAccessMode::NoAccess) {
		return;
	}
	if (!IsGpuAddressRange(vaddr, size)) {
		EXIT("invalid GPU unmap range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr, size);
	}
	auto&                               resources = GetGpuResources();
	Graphics::GraphicsRunSubmissionLock submission_lock;
	const auto access = mode == GpuAccessMode::Read    ? Graphics::GpuAccess::Read
	                    : mode == GpuAccessMode::Write ? Graphics::GpuAccess::Write
	                                                   : Graphics::GpuAccess::ReadWrite;
	resources.UnmapMemory(vaddr, size, access);
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

static GpuAccessMode GetGpuAccessMode(int prot) {
	VirtualMemory::Mode mode {};
	GpuAccessMode       gpu_mode {};
	if (!DecodeMemoryProtection(prot, &mode, &gpu_mode)) {
		EXIT("unsupported GPU memory protection: 0x%08x\n", prot);
	}
	return gpu_mode;
}

static bool ProtectCommittedHostMemory(uint64_t start, uint64_t size, VirtualMemory::Mode mode,
                                       VirtualMemory::Mode* old_mode) {
	if (size == 0) {
		if (old_mode != nullptr) {
			*old_mode = VirtualMemory::Mode::NoAccess;
		}
		return true;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto end           = start + size;
	auto       cur           = start;
	bool       protected_any = false;

	while (cur < end) {
		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) == 0) {
			return false;
		}

		const auto region_start = reinterpret_cast<uint64_t>(mbi.BaseAddress);
		const auto region_end   = region_start + static_cast<uint64_t>(mbi.RegionSize);
		const auto chunk_start  = std::max(cur, region_start);
		const auto chunk_end    = std::min(end, region_end);

		if (chunk_end <= chunk_start) {
			return false;
		}

		if (mbi.State == MEM_COMMIT) {
			VirtualMemory::Mode chunk_old {};
			if (!VirtualMemory::Protect(chunk_start, chunk_end - chunk_start, mode, &chunk_old)) {
				return false;
			}

			if (!protected_any && old_mode != nullptr) {
				*old_mode = chunk_old;
			}
			protected_any = true;
		}

		cur = chunk_end;
	}

	if (!protected_any && old_mode != nullptr) {
		*old_mode = VirtualMemory::Mode::NoAccess;
	}

	return true;
#else
	return VirtualMemory::Protect(start, size, mode, old_mode);
#endif
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

static bool CommitFixedHostRange(uint64_t start, uint64_t size, VirtualMemory::Mode mode) {
	constexpr uint64_t PAGE_SIZE = 0x4000;

	for (uint64_t addr = start; addr < start + size; addr += PAGE_SIZE) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &info, sizeof(info)) != 0) {
			if (info.State == MEM_COMMIT) {
				if (!VirtualMemory::Protect(addr, PAGE_SIZE, mode)) {
					return false;
				}
				continue;
			}
			if (info.State == MEM_RESERVE) {
				if (!VirtualMemory::Commit(addr, PAGE_SIZE, mode)) {
					return false;
				}
				continue;
			}
		}
#endif
		if (VirtualMemory::AllocFixed(addr, PAGE_SIZE, mode)) {
			continue;
		}
		if (VirtualMemory::Commit(addr, PAGE_SIZE, mode)) {
			continue;
		}
		if (VirtualMemory::Protect(addr, PAGE_SIZE, mode)) {
			continue;
		}
		return false;
	}

	return true;
}

#include "memoryAddressSpace.inc"

enum class VirtualRangeType {
	Reserved,
	PoolReserved,
	Direct,
	Flexible,
	Pooled,
	Stack,
	Code,
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

class VirtualRanges {
public:
	struct Range {
		uint64_t         start                   = 0;
		uint64_t         size                    = 0;
		uint64_t         offset                  = 0;
		int              protection              = 0;
		int              memory_type             = 0;
		VirtualRangeType type                    = VirtualRangeType::Reserved;
		bool             committed_from_reserved = false;
		// Reserved placeholders need OS-specific release/commit paths; plain reserves still use
		// Common::VirtualMemory.
		bool placeholder_backed = false;
		char name[KERNEL_MAXIMUM_NAME_LENGTH];
	};

	bool Add(uint64_t start, uint64_t size, uint64_t offset, int protection, int memory_type,
	         VirtualRangeType type, const char* name, bool committed_from_reserved = false,
	         bool placeholder_backed = false) {
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
		r.start                   = start;
		r.size                    = size;
		r.offset                  = offset;
		r.protection              = protection;
		r.memory_type             = memory_type;
		r.type                    = type;
		r.committed_from_reserved = committed_from_reserved;
		r.placeholder_backed      = placeholder_backed;
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

	bool HasGpuAccess(uint64_t start, uint64_t size) {
		Common::LockGuard lock(m_mutex);

		return std::any_of(m_ranges.begin(), m_ranges.end(), [start, size](const auto& range) {
			return VirtualRangesOverlap(start, size, range.start, range.size) &&
			       (range.protection & (PROT_GPU_READ | PROT_GPU_WRITE)) != 0;
		});
	}

	bool ReleaseReserved(uint64_t start, uint64_t size) {
		Common::LockGuard lock(m_mutex);

		for (size_t index = 0; index < m_ranges.size(); index++) {
			auto& r = m_ranges[index];
			if (r.start == start && r.size == size && IsReservedRangeType(r.type)) {
				m_ranges.erase(m_ranges.begin() + static_cast<std::ptrdiff_t>(index));
				return VirtualMemory::Free(start);
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
		if (left.type == VirtualRangeType::Direct || right.type == VirtualRangeType::Direct) {
			return false;
		}

		return left.type == right.type && left.protection == right.protection &&
		       left.memory_type == right.memory_type &&
		       left.committed_from_reserved == right.committed_from_reserved &&
		       left.placeholder_backed == right.placeholder_backed &&
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
	virtual ~PhysicalMemory() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PhysicalMemory);

	static uint64_t Size() { return static_cast<uint64_t>(13824) * 1024 * 1024; }

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
	std::vector<AllocatedBlock> FindMappings(uint64_t phys_addr, size_t len);
	void                        SetVirtualRangeName(uint64_t vaddr, uint64_t len, const char* name);
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
		uint64_t            host_vaddr;
		uint64_t            host_size;
		int                 prot;
		VirtualMemory::Mode mode;
		GpuAccessMode       gpu_mode;
		char                name[KERNEL_MAXIMUM_NAME_LENGTH];
	};

	FlexibleMemory() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~FlexibleMemory() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(FlexibleMemory);

	static uint64_t Size() { return g_flexible_memory_size; }
	uint64_t        Available();

	bool Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, GpuAccessMode gpu_mode,
	         const char* name);
	bool Unmap(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode,
	           uint64_t* host_vaddr_to_release = nullptr);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot,
	          VirtualMemory::Mode* mode, GpuAccessMode* gpu_mode);
	void SetVirtualRangeName(uint64_t vaddr, uint64_t len, const char* name);

	[[nodiscard]] Common::Mutex&                     GetMutex() { return m_mutex; }
	[[nodiscard]] const std::vector<AllocatedBlock>& GetBlocks() const { return m_allocated; }

private:
	std::vector<AllocatedBlock> m_allocated;
	uint64_t                    m_allocated_total = 0;
	Common::Mutex               m_mutex;
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

static PhysicalMemory*          g_physical_memory           = nullptr;
static FlexibleMemory*          g_flexible_memory           = nullptr;
static PooledMemory*            g_pooled_memory             = nullptr;
static VirtualRanges*           g_virtual_ranges            = nullptr;
static DirectMemoryBacking*     g_direct_memory_backing     = nullptr;
static PlaceholderAddressSpace* g_placeholder_address_space = nullptr;
static callback_func_t          g_alloc_callback            = nullptr;
static callback_func_t          g_free_callback             = nullptr;
static std::atomic<uint64_t>    g_memory_pool_committed     = 0;
static void                     MemoryPoolSubtractCommitted(uint64_t len);
// Keep host mappings, physical blocks, placeholders, and virtual ranges in step.
static std::recursive_mutex g_memory_operation_mutex;

bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
	return g_direct_memory_backing->TryWriteBacking(vaddr, data, size);
}

bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
	return g_direct_memory_backing->TryReadBacking(vaddr, data, size);
}

void WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept {
	if (!TryWriteBacking(vaddr, data, size)) {
		EXIT("Memory: required direct-backing write failed, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
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

static bool IsInPrtAperture(uint64_t address) {
	Common::LockGuard lock(g_prt_aperture_mutex);

	for (const auto& aperture: g_prt_apertures) {
		if (aperture.size != 0 && address >= aperture.address &&
		    address < aperture.address + aperture.size) {
			return true;
		}
	}

	return false;
}

static void SelfTestSub64SharedPlaceholderAlias() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	constexpr uint64_t PageSize    = 0x4000;
	const auto         granularity = g_placeholder_address_space->GetGranularity();
	if (granularity < PageSize * 2u) {
		LOGF_COLOR(
		    Log::Color::Yellow,
		    "\t direct-memory sub-64K placeholder self-test skipped: granularity too small\n");
		return;
	}

	const auto base = g_placeholder_address_space->ReserveAligned(0, granularity, granularity);
	if (base == 0) {
		LOGF_COLOR(Log::Color::Yellow,
		           "\t direct-memory sub-64K placeholder self-test skipped: reserve unavailable\n");
		return;
	}

	const auto alias          = base + PageSize;
	bool       ok             = false;
	auto       failure_reason = DirectMemoryBacking::FailureReason::None;
	const bool consumed       = g_placeholder_address_space->Consume(alias, PageSize);

	if (consumed &&
	    g_direct_memory_backing->MapExistingPlaceholderFixed(
	        alias, PageSize, PageSize, VirtualMemory::Mode::ReadWrite, &failure_reason)) {
		auto* ptr = reinterpret_cast<uint64_t*>(alias);
		*ptr      = 0x4b59545953553634ull; // "KYTYSU64"
		ok        = (*ptr == 0x4b59545953553634ull);
		std::memset(ptr, 0, PageSize);

		bool placeholder_preserved = false;
		ok = g_direct_memory_backing->Unmap(alias, PageSize, true, &placeholder_preserved) &&
		     placeholder_preserved && ok;
		if (placeholder_preserved) {
			g_placeholder_address_space->AddFree(alias, PageSize);
		}
	} else if (consumed) {
		g_placeholder_address_space->AddFree(alias, PageSize);
	}

	const bool released = g_placeholder_address_space->ReleaseFree(base, granularity);
	LOGF_COLOR(ok && released ? Log::Color::Green : Log::Color::Red,
	           "\t direct-memory sub-64K placeholder self-test: %s%s%s\n",
	           ok && released ? "ok" : "failed", ok ? "" : ", reason = ",
	           ok ? "" : DirectMemoryBacking::GetFailureReasonName(failure_reason));
#endif
}

static bool RestoreCommittedPlaceholderOrProtect(uint64_t vaddr, uint64_t size) {
	if (g_placeholder_address_space->ReleaseCommitted(vaddr, size)) {
		return true;
	}
	VirtualMemory::Protect(vaddr, size, VirtualMemory::Mode::NoAccess);
	return false;
}

static bool ReleaseReservedRange(uint64_t vaddr, uint64_t size) {
	VirtualRanges::Range range {};
	if (g_virtual_ranges->Query(vaddr, 0, &range) && range.start == vaddr && range.size == size &&
	    IsReservedRangeType(range.type) && range.placeholder_backed) {
		if (!g_virtual_ranges->ConsumeReserved(vaddr, size, range.type)) {
			return false;
		}
		if (g_placeholder_address_space->ReleaseFree(vaddr, size)) {
			return true;
		}
		if (VirtualMemory::Free(vaddr)) {
			return true;
		}
		g_virtual_ranges->Add(vaddr, size, 0, 0, 0, range.type, range.name, false, true);
		return false;
	}
	return g_virtual_ranges->ReleaseReserved(vaddr, size);
}

static bool ReplaceFixedRangeWithReserved(uint64_t start, uint64_t size, bool* placeholder_backed);

KYTY_SUBSYSTEM_INIT(Memory) {
	g_flexible_memory_size      = DEFAULT_FLEXIBLE_MEMORY_SIZE;
	g_physical_memory           = new PhysicalMemory;
	g_flexible_memory           = new FlexibleMemory;
	g_pooled_memory             = new PooledMemory;
	g_virtual_ranges            = new VirtualRanges;
	g_direct_memory_backing     = new DirectMemoryBacking(PhysicalMemory::Size());
	g_placeholder_address_space = new PlaceholderAddressSpace;

	VirtualMemory::Init();
	EXIT_IF(!g_direct_memory_backing->SelfTest());
	g_placeholder_address_space->SelfTest();
	SelfTestSub64SharedPlaceholderAlias();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Memory) {}

KYTY_SUBSYSTEM_DESTROY(Memory) {
	delete g_placeholder_address_space;
	g_placeholder_address_space = nullptr;
	delete g_direct_memory_backing;
	g_direct_memory_backing = nullptr;
	delete g_pooled_memory;
	g_pooled_memory = nullptr;
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

	auto next = m_physical.upper_bound(phys_addr);
	if (next == m_physical.begin()) {
		return false;
	}
	const auto& block = std::prev(next)->second;
	if (block.pool_expansion || phys_addr < block.start_addr ||
	    phys_addr >= block.start_addr + block.size ||
	    len > block.start_addr + block.size - phys_addr) {
		return false;
	}

	AllocatedBlock mapping = block;
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

	auto next = m_physical.upper_bound(phys_addr);
	if (next == m_physical.begin()) {
		return false;
	}
	const auto& block = std::prev(next)->second;
	return !block.pool_expansion && phys_addr >= block.start_addr &&
	       phys_addr < block.start_addr + block.size &&
	       len <= block.start_addr + block.size - phys_addr;
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

bool PhysicalMemory::Unmap(uint64_t vaddr, uint64_t size, GpuAccessMode* gpu_mode,
                           uint64_t* host_vaddr_to_release) {
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

	AllocatedBlock b {};
	b.map_vaddr  = vaddr;
	b.map_size   = len;
	b.host_vaddr = vaddr;
	b.host_size  = len;
	b.prot       = prot;
	b.mode       = mode;
	b.gpu_mode   = gpu_mode;
	CopyVirtualRangeName(b.name, name);

	m_allocated.push_back(b);
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

	auto set_host_release_if_last = [this, host_vaddr_to_release](uint64_t host_vaddr,
	                                                              uint64_t host_size) {
		if (host_vaddr_to_release == nullptr || host_vaddr == 0 || host_size == 0) {
			return;
		}
		const bool still_mapped = std::any_of(
		    m_allocated.begin(), m_allocated.end(), [host_vaddr, host_size](const auto& block) {
			    return block.host_vaddr == host_vaddr && block.host_size == host_size;
		    });
		if (!still_mapped) {
			*host_vaddr_to_release = host_vaddr;
		}
	};

	size_t index = 0;
	for (auto& b: m_allocated) {
		if (b.map_vaddr == vaddr && b.map_size == size) {
			*gpu_mode             = b.gpu_mode;
			const auto host_vaddr = b.host_vaddr;
			const auto host_size  = b.host_size;

			m_allocated.erase(m_allocated.begin() + static_cast<std::ptrdiff_t>(index));
			m_allocated_total -= size;
			set_host_release_if_last(host_vaddr, host_size);
			return true;
		}
		if (vaddr > b.map_vaddr && vaddr + size < b.map_vaddr + b.map_size) {
			*gpu_mode = b.gpu_mode;

			AllocatedBlock right = b;
			right.map_size       = b.map_vaddr + b.map_size - (vaddr + size);
			right.map_vaddr      = vaddr + size;

			b.map_size = vaddr - b.map_vaddr;
			m_allocated.push_back(right);
			m_allocated_total -= size;
			return true;
		}
		if (vaddr == b.map_vaddr && size < b.map_size) {
			*gpu_mode = b.gpu_mode;

			b.map_vaddr += size;
			b.map_size -= size;
			m_allocated_total -= size;
			return true;
		}
		if (vaddr > b.map_vaddr && vaddr + size == b.map_vaddr + b.map_size) {
			*gpu_mode = b.gpu_mode;

			b.map_size = vaddr - b.map_vaddr;
			m_allocated_total -= size;
			return true;
		}
		index++;
	}

	return false;
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
	struct RemovedMapping {
		PooledMemory::Mapping mapping;
		bool                  placeholder_preserved;
	};

	std::vector<RemovedMapping> removed;
	for (const auto& mapping: mappings) {
		bool placeholder_preserved = false;
		if (!g_direct_memory_backing->Unmap(mapping.vaddr, mapping.size, true,
		                                    &placeholder_preserved)) {
			for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
				auto       failure_reason = DirectMemoryBacking::FailureReason::None;
				const bool restored = it->placeholder_preserved
				                          ? g_direct_memory_backing->MapExistingPlaceholderFixed(
				                                it->mapping.vaddr, it->mapping.size,
				                                it->mapping.phys_addr, mode, &failure_reason)
				                          : g_direct_memory_backing->MapFixed(
				                                it->mapping.vaddr, it->mapping.size,
				                                it->mapping.phys_addr, mode, &failure_reason);
				if (!restored) {
					EXIT("pooled-memory unmap rollback failed: %s\n",
					     DirectMemoryBacking::GetFailureReasonName(failure_reason));
				}
			}
			return false;
		}
		removed.push_back({mapping, placeholder_preserved});
	}

	for (const auto& entry: removed) {
		if (entry.placeholder_preserved) {
			g_placeholder_address_space->AddFree(entry.mapping.vaddr, entry.mapping.size);
		} else if (!g_placeholder_address_space->ReserveFixed(entry.mapping.vaddr,
		                                                      entry.mapping.size)) {
			EXIT("failed to reserve pooled-memory placeholder\n");
		}
	}
	return true;
}

int32_t KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot,
                                                   int flags, const char* name) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	EXIT_NOT_IMPLEMENTED(addr_in_out == nullptr);

	constexpr size_t   PAGE_SIZE         = 0x4000;
	constexpr size_t   MAXIMUM_NAME_SIZE = 32;
	constexpr uint64_t DEFAULT_PS5_BASE  = 0x200000000;
	constexpr int      MAP_FIXED         = 0x10;
	constexpr int      MAP_SHARED        = 0x01;
	constexpr int      MAP_PRIVATE       = 0x02;
	constexpr int      MAP_NO_OVERWRITE  = 0x80;
	constexpr int      MAP_VOID          = 0x100;
	constexpr int      MAP_STACK         = 0x400;
	constexpr int      MAP_NO_SYNC       = 0x800;
	constexpr int      MAP_ANON          = 0x1000;
	constexpr int      MAP_UNKNOWN_8000  = 0x8000;
	constexpr int      MAP_NO_CORE       = 0x20000;
	constexpr int      MAP_NO_COALESCE   = 0x400000;
	constexpr int SUPPORTED_MAP_BITS     = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_NO_OVERWRITE |
	                                       MAP_VOID | MAP_STACK | MAP_NO_SYNC | MAP_ANON |
	                                       MAP_UNKNOWN_8000 | MAP_NO_CORE | MAP_NO_COALESCE;

	if (len == 0 || (len & (PAGE_SIZE - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	if (name == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (std::strlen(name) >= MAXIMUM_NAME_SIZE) {
		return KERNEL_ERROR_ENAMETOOLONG;
	}

	if ((flags & ~SUPPORTED_MAP_BITS) != 0) {
		LOGF_COLOR(Log::Color::Red, "\t unsupported flags = 0x%08" PRIx32 "\n",
		           static_cast<uint32_t>(flags & ~SUPPORTED_MAP_BITS));
		return KERNEL_ERROR_EINVAL;
	}

	VirtualMemory::Mode mode     = VirtualMemory::Mode::NoAccess;
	GpuAccessMode       gpu_mode = GpuAccessMode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode, &gpu_mode)) {
		EXIT("unknown prot: %d\n", prot);
	}

	auto                 in_addr                 = reinterpret_cast<uint64_t>(*addr_in_out);
	uint64_t             out_addr                = 0;
	bool                 committed_from_reserved = false;
	bool                 consumed_reserved       = false;
	VirtualRanges::Range consumed_range {};

	if ((flags & MAP_FIXED) != 0) {
		if (in_addr == 0 || (in_addr & (PAGE_SIZE - 1)) != 0) {
			return KERNEL_ERROR_EINVAL;
		}
		if ((flags & MAP_NO_OVERWRITE) != 0 && g_virtual_ranges->HasOverlap(in_addr, len)) {
			return KERNEL_ERROR_ENOMEM;
		}
		if (g_virtual_ranges->Query(in_addr, 0, &consumed_range) &&
		    consumed_range.type == VirtualRangeType::Reserved &&
		    g_virtual_ranges->ConsumeReserved(in_addr, len)) {
			consumed_reserved = true;
			if (g_placeholder_address_space->Commit(in_addr, len, mode) ||
			    CommitFixedHostRange(in_addr, len, mode)) {
				out_addr                = in_addr;
				committed_from_reserved = true;
			}
		} else if (!ReleaseReservedRange(in_addr, len)) {
			return KERNEL_ERROR_ENOMEM;
		} else if (VirtualMemory::AllocFixed(in_addr, len, mode)) {
			out_addr = in_addr;
		}
	} else {
		const auto search_addr = (in_addr != 0 ? in_addr : DEFAULT_PS5_BASE);
		out_addr               = VirtualMemory::AllocAligned(search_addr, len, mode, PAGE_SIZE);
	}

	*addr_in_out = reinterpret_cast<void*>(out_addr);

	if (out_addr == 0) {
		if (consumed_reserved) {
			g_virtual_ranges->Add(in_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                      consumed_range.name, false, consumed_range.placeholder_backed);
		}
		return KERNEL_ERROR_ENOMEM;
	}

	if (!g_flexible_memory->Map(out_addr, len, prot, mode, gpu_mode, name)) {
		LOGF_COLOR(Log::Color::Red, "\t [Fail]\n");
		if (committed_from_reserved) {
			const bool placeholder_backed = RestoreCommittedPlaceholderOrProtect(out_addr, len);
			g_virtual_ranges->Add(out_addr, len, 0, 0, 0, VirtualRangeType::Reserved, name, false,
			                      placeholder_backed);
		} else {
			VirtualMemory::Free(out_addr);
		}
		return KERNEL_ERROR_ENOMEM;
	}

	const auto range_type =
	    ((flags & MAP_STACK) != 0 ? VirtualRangeType::Stack : VirtualRangeType::Flexible);
	if (!g_virtual_ranges->Add(out_addr, len, 0, prot, 0, range_type, name,
	                           committed_from_reserved)) {
		GpuAccessMode rollback_gpu_mode = GpuAccessMode::NoAccess;
		g_flexible_memory->Unmap(out_addr, len, &rollback_gpu_mode);
		if (committed_from_reserved) {
			const bool placeholder_backed = RestoreCommittedPlaceholderOrProtect(out_addr, len);
			g_virtual_ranges->Add(out_addr, len, 0, 0, 0, VirtualRangeType::Reserved, name, false,
			                      placeholder_backed);
		} else {
			VirtualMemory::Free(out_addr);
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

	MapGpuRange(out_addr, len, gpu_mode);

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

	const auto address = reinterpret_cast<uint64_t>(addr);

	LOGF("\t index = %d\n"
	     "\t addr  = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     index, address, static_cast<uint64_t>(len));

	if (index < 0 || index > PRT_APERTURE_MAX_INDEX) {
		return KERNEL_ERROR_EINVAL;
	}

	if (len == 0) {
		Common::LockGuard lock(g_prt_aperture_mutex);
		const auto        old = g_prt_apertures[static_cast<size_t>(index)];
		if (old.size != 0) {
			UnmapGpuRange(old.address, old.size, GpuAccessMode::ReadWrite);
		}
		g_prt_apertures[static_cast<size_t>(index)] = {};
		LOGF_COLOR(Log::Color::Green, "\t[Ok]\n");
		return OK;
	}

	if (address == 0 || (address & (PRT_PAGE_SIZE - 1u)) != 0 ||
	    (len & (PRT_PAGE_SIZE - 1u)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	if (address < PRT_APERTURE_START || len > PRT_APERTURE_END - address) {
		return KERNEL_ERROR_EINVAL;
	}

	{
		Common::LockGuard lock(g_prt_aperture_mutex);
		const auto        old = g_prt_apertures[static_cast<size_t>(index)];
		if (old.size != 0) {
			UnmapGpuRange(old.address, old.size, GpuAccessMode::ReadWrite);
		}
		MapGpuRange(address, len, GpuAccessMode::ReadWrite);
		g_prt_apertures[static_cast<size_t>(index)] = {address, static_cast<uint64_t>(len)};
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

int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t start = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     vaddr, len);

	if (len == 0 || UINT64_MAX - vaddr < len) {
		return KERNEL_ERROR_EINVAL;
	}

	VirtualRanges::Range range {};
	if (!g_virtual_ranges->Query(vaddr, 0, &range)) {
		return KERNEL_ERROR_EACCES;
	}
	const auto chunk_len = std::min<uint64_t>(len, range.size - (vaddr - range.start));
	if (chunk_len < len) {
		const int ret = KernelMunmap(vaddr, chunk_len);
		return ret == OK ? KernelMunmap(vaddr + chunk_len, len - chunk_len) : ret;
	}
	if (IsReservedRangeType(range.type)) {
		const bool released = range.placeholder_backed
		                          ? g_placeholder_address_space->ReleaseFree(vaddr, len)
		                          : VirtualMemory::FreeRange(vaddr, len);
		if (!released) {
			return KERNEL_ERROR_EACCES;
		}
		g_virtual_ranges->Remove(vaddr, len);
		return OK;
	}
	UnmapGpuRange(vaddr, len, GetGpuAccessMode(range.protection));

	size_t        backend_len      = 0;
	uint64_t      backend_base     = 0;
	GpuAccessMode gpu_mode         = GpuAccessMode::NoAccess;
	const bool    pooled_backend   = range.type == VirtualRangeType::Pooled;
	bool          backend_found    = false;
	bool          exact_host_range = backend_found && backend_base == vaddr && backend_len == len;
	bool          backend_unmapped = false;
	bool          shared_unmapped  = false;
	bool          placeholder_restored     = false;
	bool          flexible_backend         = false;
	uint64_t      physical_host_to_release = 0;
	uint64_t      flexible_host_to_release = 0;
	const bool    can_restore_placeholder =
	    range.committed_from_reserved && range.start == vaddr && range.size == len;

	if (pooled_backend) {
		std::vector<PooledMemory::Mapping> mappings;
		backend_found    = g_pooled_memory->Query(vaddr, len, &mappings);
		exact_host_range = backend_found;
		if (backend_found) {
			VirtualMemory::Mode mode        = VirtualMemory::Mode::NoAccess;
			GpuAccessMode       decoded_gpu = GpuAccessMode::NoAccess;
			shared_unmapped = DecodeMemoryProtection(range.protection, &mode, &decoded_gpu) &&
			                  UnmapPooledBackingTransactional(mappings, mode);
			if (shared_unmapped && !g_pooled_memory->Release(vaddr, len, &gpu_mode)) {
				EXIT("failed to release unmapped pooled-memory range\n");
			}
			backend_unmapped     = shared_unmapped;
			placeholder_restored = shared_unmapped;
		}
		if (!backend_unmapped) {
			EXIT("pooled-memory unmap failed: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr,
			     len);
			return KERNEL_ERROR_EACCES;
		}
	} else if ((backend_found = g_physical_memory->Find(vaddr, &backend_base, &backend_len, nullptr,
	                                                    nullptr, &gpu_mode))) {
		exact_host_range = backend_base == vaddr && backend_len == len;
		backend_unmapped =
		    g_physical_memory->Unmap(vaddr, len, &gpu_mode, &physical_host_to_release);
		if (backend_unmapped) {
			const bool direct_contains = g_direct_memory_backing->Contains(vaddr, len);
			if (direct_contains) {
				shared_unmapped = g_direct_memory_backing->Unmap(
				    vaddr, len, can_restore_placeholder, &placeholder_restored);
			}
			if (placeholder_restored) {
				g_placeholder_address_space->AddFree(vaddr, len);
			}
		}
	} else {
		flexible_backend = true;
		backend_found    = g_flexible_memory->Find(vaddr, &backend_base, &backend_len, nullptr,
		                                           nullptr, &gpu_mode);
		exact_host_range = backend_found && backend_base == vaddr && backend_len == len;
		backend_unmapped = (backend_found ? g_flexible_memory->Unmap(vaddr, len, &gpu_mode,
		                                                             &flexible_host_to_release)
		                                  : false);
		if (backend_unmapped && can_restore_placeholder) {
			placeholder_restored = g_placeholder_address_space->ReleaseCommitted(vaddr, len);
		}
	}
	if (!backend_unmapped) {
		EXIT("memory backend unmap failed: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr,
		     len);
	}

	g_virtual_ranges->Remove(vaddr, len);

	const bool host_range_released_or_shared =
	    flexible_backend ? flexible_host_to_release != 0
	                     : (shared_unmapped || physical_host_to_release != 0);
	const bool can_release_host_range = exact_host_range && backend_unmapped &&
	                                    !range.committed_from_reserved &&
	                                    host_range_released_or_shared;
	if (can_release_host_range && (vaddr != 0 || len != 0)) {
		if (!shared_unmapped) {
			VirtualMemory::Free(flexible_backend ? flexible_host_to_release
			                                     : physical_host_to_release);
		}
	} else if (IsCommittedRangeType(range.type)) {
		constexpr uint64_t PAGE_SIZE    = 0x4000;
		auto               aligned_addr = vaddr & ~(PAGE_SIZE - 1);
		auto aligned_len = (len + (vaddr - aligned_addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		if (placeholder_restored) {
			// Exact placeholder-backed unmaps already returned this range to the placeholder
			// manager.
		} else if (!shared_unmapped) {
			VirtualMemory::Protect(aligned_addr, aligned_len, VirtualMemory::Mode::NoAccess);
		} else if (range.committed_from_reserved) {
			VirtualMemory::ReserveFixed(aligned_addr, aligned_len);
		}
		if (range.committed_from_reserved) {
			g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::Reserved, range.name,
			                      false, placeholder_restored);
		}
	}

	if (g_free_callback != nullptr && IsCommittedRangeType(range.type)) {
		g_free_callback(vaddr, len);
	}
	if (pooled_backend) {
		MemoryPoolSubtractCommitted(len);
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

	if (search_start < 0 || search_end <= search_start || len == 0 || phys_addr_out == nullptr) {
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

	if (len == 0 || phys_addr_out == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(0, PhysicalMemory::Size(), len, alignment, &addr, memory_type)) {
		LOGF_COLOR(Log::Color::Red, "\t[Fail]\n");
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	LOGF_COLOR(Log::Color::Green, "\tphys_addr    = %016" PRIx64 "\n\t[Ok]\n", addr);

	return OK;
}

int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	LOGF("\t start = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     start, len);

	if (start < 0 || len == 0) {
		return KERNEL_ERROR_EINVAL;
	}
	if (g_pooled_memory->ReleaseExpansion(static_cast<uint64_t>(start), len)) {
		if (!g_physical_memory->ReleasePoolExpansion(static_cast<uint64_t>(start), len)) {
			EXIT("failed to release physical pool expansion\n");
		}
		return OK;
	}

	uint64_t      vaddr    = 0;
	uint64_t      size     = 0;
	GpuAccessMode gpu_mode = GpuAccessMode::NoAccess;

	PhysicalMemory::AllocatedBlock block {};
	bool                           found = g_physical_memory->Find(start, false, &block);
	bool                           exact_host_range =
	    found && block.start_addr == static_cast<uint64_t>(start) && block.size == len;
	VirtualRanges::Range range {};
	const bool range_found = (found ? g_virtual_ranges->Query(block.map_vaddr, 0, &range) : false);
	bool       shared_unmapped      = false;
	bool       placeholder_restored = false;
	const auto mapped_aliases       = g_physical_memory->FindMappings(start, len);
	for (const auto& alias: mapped_aliases) {
		UnmapGpuRange(alias.map_vaddr, alias.map_size, alias.gpu_mode);
	}

	bool result = g_physical_memory->Release(start, len, &vaddr, &size, &gpu_mode);

	if (!result) {
		if (!mapped_aliases.empty()) {
			EXIT("physical-memory release failed after GPU alias unmap: addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " aliases=%zu\n",
			     start, len, mapped_aliases.size());
		}
		return KERNEL_ERROR_EACCES;
	}

	for (const auto& alias: mapped_aliases) {
		GpuAccessMode alias_gpu_mode        = GpuAccessMode::NoAccess;
		uint64_t      alias_host_to_release = 0;
		if (!g_physical_memory->Unmap(alias.map_vaddr, alias.map_size, &alias_gpu_mode,
		                              &alias_host_to_release)) {
			EXIT("physical-memory alias unmap failed: addr=0x%016" PRIx64 " size=0x%016" PRIx64
			     "\n",
			     alias.map_vaddr, alias.map_size);
		}

		VirtualRanges::Range alias_range {};
		const bool alias_range_found = g_virtual_ranges->Query(alias.map_vaddr, 0, &alias_range);
		g_virtual_ranges->Remove(alias.map_vaddr, alias.map_size);

		bool alias_shared_unmapped      = false;
		bool alias_placeholder_restored = false;
		if (g_direct_memory_backing->Contains(alias.map_vaddr, alias.map_size)) {
			const bool can_restore_placeholder =
			    alias_range_found && alias_range.committed_from_reserved &&
			    alias_range.start == alias.map_vaddr && alias_range.size == alias.map_size;
			alias_shared_unmapped = g_direct_memory_backing->Unmap(alias.map_vaddr, alias.map_size,
			                                                       can_restore_placeholder,
			                                                       &alias_placeholder_restored);
			if (alias_placeholder_restored) {
				g_placeholder_address_space->AddFree(alias.map_vaddr, alias.map_size);
			}
		}

		if (alias_range_found && alias_range.committed_from_reserved) {
			g_virtual_ranges->Add(alias.map_vaddr, alias.map_size, 0, 0, 0,
			                      VirtualRangeType::Reserved, alias_range.name, false,
			                      alias_placeholder_restored);
		} else if (!alias_shared_unmapped && alias_host_to_release != 0) {
			VirtualMemory::Free(alias_host_to_release);
		} else if (!alias_shared_unmapped) {
			VirtualMemory::Protect(alias.map_vaddr, alias.map_size, VirtualMemory::Mode::NoAccess);
		}
	}

	if (vaddr != 0 || size != 0) {
		g_virtual_ranges->Remove(vaddr, size);
		const bool can_restore_placeholder = range_found && range.committed_from_reserved &&
		                                     range.start == vaddr && range.size == size;
		shared_unmapped = g_direct_memory_backing->Unmap(vaddr, size, can_restore_placeholder,
		                                                 &placeholder_restored);
		if (placeholder_restored) {
			g_placeholder_address_space->AddFree(vaddr, size);
		}
	}

	if (exact_host_range && (!range_found || !range.committed_from_reserved) &&
	    (vaddr != 0 || size != 0)) {
		if (!shared_unmapped) {
			VirtualMemory::Free(vaddr);
		}
	} else if (vaddr != 0 || size != 0) {
		if (placeholder_restored) {
			// Exact placeholder-backed unmaps already returned this range to the placeholder
			// manager.
		} else if (!shared_unmapped) {
			VirtualMemory::Protect(vaddr, size, VirtualMemory::Mode::NoAccess);
		} else if (range_found && range.committed_from_reserved) {
			VirtualMemory::ReserveFixed(vaddr, size);
		}
		if (range_found && range.committed_from_reserved) {
			g_virtual_ranges->Add(vaddr, size, 0, 0, 0, VirtualRangeType::Reserved, range.name,
			                      false, placeholder_restored);
		}
	}

	if (g_free_callback != nullptr) {
		g_free_callback(vaddr, len);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len) {
	PRINT_NAME();

	LOGF("\t start = 0x%016" PRIx64 "\n"
	     "\t len   = 0x%016" PRIx64 "\n",
	     start, len);

	if (start < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	constexpr size_t PAGE_SIZE = 0x4000;
	if ((static_cast<uint64_t>(start) & (PAGE_SIZE - 1)) != 0 || (len & (PAGE_SIZE - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	if (len == 0) {
		return OK;
	}

	return KernelReleaseDirectMemory(start, len);
}

int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        int64_t direct_memory_start, size_t alignment) {
	PRINT_NAME();

	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	constexpr int MAP_FIXED        = 0x10;
	constexpr int MAP_NO_OVERWRITE = 0x80;

	bool fixed        = ((flags & MAP_FIXED) != 0);
	bool no_overwrite = ((flags & MAP_NO_OVERWRITE) != 0);

	VirtualMemory::Mode mode     = VirtualMemory::Mode::NoAccess;
	GpuAccessMode       gpu_mode = GpuAccessMode::NoAccess;

	if (!DecodeMemoryProtection(prot, &mode, &gpu_mode)) {
		EXIT("unknown prot: %d\n", prot);
	}
	if (direct_memory_start < 0 || len == 0 ||
	    !g_physical_memory->CanMapDirect(static_cast<uint64_t>(direct_memory_start), len)) {
		return KERNEL_ERROR_ENOMEM;
	}

	auto                 in_addr                 = reinterpret_cast<uint64_t>(*addr);
	uint64_t             out_addr                = 0;
	bool                 committed_from_reserved = false;
	bool                 shared_backing          = false;
	bool                 consumed_reserved       = false;
	VirtualRanges::Range consumed_range {};
	auto                 shared_failure = DirectMemoryBacking::FailureReason::None;
	// Direct mappings must remain views of the single backing object. Anonymous fallbacks break
	// aliasing and lose direct-memory contents when a range is unmapped and mapped again.
	auto map_shared_fixed = [&](uint64_t target_addr) -> bool {
		const bool placeholder_ready = g_placeholder_address_space->Consume(target_addr, len);
		if (placeholder_ready) {
			if (g_direct_memory_backing->MapExistingPlaceholderFixed(
			        target_addr, len, direct_memory_start, mode, &shared_failure)) {
				return true;
			}
			g_placeholder_address_space->AddFree(target_addr, len);
			return false;
		}

		return g_direct_memory_backing->MapFixed(target_addr, len, direct_memory_start, mode,
		                                         &shared_failure);
	};
	auto map_consumed_reserved_fixed = [&]() {
		consumed_reserved = true;
		if (map_shared_fixed(in_addr)) {
			out_addr                = in_addr;
			committed_from_reserved = true;
			shared_backing          = true;
		}
	};

	if (fixed) {
		EXIT_NOT_IMPLEMENTED(in_addr == 0);
		EXIT_NOT_IMPLEMENTED(alignment != 0 && (in_addr & (alignment - 1)) != 0);
		if (no_overwrite && g_virtual_ranges->HasOverlap(in_addr, len)) {
			return KERNEL_ERROR_ENOMEM;
		}

		VirtualRanges::Range same_range {};
		if (g_virtual_ranges->Query(in_addr, 0, &same_range) &&
		    same_range.type == VirtualRangeType::Direct &&
		    len <= same_range.start + same_range.size - in_addr &&
		    same_range.offset + (in_addr - same_range.start) ==
		        static_cast<uint64_t>(direct_memory_start) &&
		    same_range.protection == prot) {
			*addr = reinterpret_cast<void*>(in_addr);
			LOGF_COLOR(Log::Color::Green,
			           "\t in_addr  = 0x%016" PRIx64 "\n"
			           "\t out_addr = 0x%016" PRIx64 "\n"
			           "\t dmem     = 0x%016" PRIx64 "\n"
			           "\t size     = 0x%016" PRIx64 "\n"
			           "\t mode     = %s\n"
			           "\t flags    = 0x%08" PRIx32 "\n"
			           "\t align    = 0x%016" PRIx64 "\n"
			           "\t gpu_mode = %s\n"
			           "\t shared   = %s\n"
			           "\t reason   = already-mapped\n"
			           "\t [Ok]\n",
			           in_addr, in_addr, static_cast<uint64_t>(direct_memory_start), len,
			           Common::EnumName(mode).c_str(), static_cast<uint32_t>(flags), alignment,
			           Common::EnumName(gpu_mode).c_str(),
			           g_direct_memory_backing->Contains(in_addr, len) ? "yes" : "no");
			return OK;
		}

		if (g_virtual_ranges->ConsumeReservedSpan(in_addr, len, &consumed_range)) {
			map_consumed_reserved_fixed();
		} else {
			bool placeholder_backed = false;
			if (ReplaceFixedRangeWithReserved(in_addr, len, &placeholder_backed) &&
			    g_virtual_ranges->ConsumeReservedSpan(in_addr, len, &consumed_range)) {
				map_consumed_reserved_fixed();
			} else {
				VirtualRanges::Range existing_range {};
				if (g_virtual_ranges->Query(in_addr, 0, &existing_range)) {
					GpuAccessMode old_gpu_mode = GpuAccessMode::NoAccess;
					g_physical_memory->Unmap(in_addr, len, &old_gpu_mode);
					g_flexible_memory->Unmap(in_addr, len, &old_gpu_mode);
					g_virtual_ranges->Remove(in_addr, len);
					const bool old_shared = g_direct_memory_backing->Contains(in_addr, len);

					bool old_placeholder_restored = false;
					g_direct_memory_backing->Unmap(in_addr, len,
					                               existing_range.committed_from_reserved,
					                               &old_placeholder_restored);
					if (old_placeholder_restored) {
						g_placeholder_address_space->AddFree(in_addr, len);
					}

					if (old_shared && g_direct_memory_backing->Contains(in_addr, len)) {
						out_addr = 0;
					} else if (map_shared_fixed(in_addr)) {
						out_addr                = in_addr;
						committed_from_reserved = existing_range.committed_from_reserved ||
						                          existing_range.start != in_addr ||
						                          existing_range.size != len;
						shared_backing          = true;
					}
				} else if (!ReleaseReservedRange(in_addr, len)) {
					return KERNEL_ERROR_ENOMEM;
				} else if (map_shared_fixed(in_addr)) {
					out_addr       = in_addr;
					shared_backing = true;
				}
			}
		}
	} else {
		constexpr size_t DEFAULT_ALIGNMENT = 0x4000;
		alignment                          = (alignment != 0 ? alignment : DEFAULT_ALIGNMENT);
		if (in_addr != 0 && g_virtual_ranges->Query(in_addr, 0, &consumed_range) &&
		    consumed_range.type == VirtualRangeType::Reserved &&
		    g_virtual_ranges->ConsumeReserved(in_addr, len)) {
			consumed_reserved = true;
			if (map_shared_fixed(in_addr)) {
				out_addr                = in_addr;
				committed_from_reserved = true;
				shared_backing          = true;
			}
		} else {
			out_addr = g_direct_memory_backing->MapAligned(in_addr, len, direct_memory_start, mode,
			                                               alignment, &shared_failure);
			shared_backing = out_addr != 0;
		}
	}

	*addr = reinterpret_cast<void*>(out_addr);

	const char* shared_reason = "n/a";
	if (!shared_backing) {
		shared_reason = DirectMemoryBacking::GetFailureReasonName(shared_failure);
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
		if (consumed_reserved) {
			g_virtual_ranges->Add(in_addr, len, 0, 0, 0, VirtualRangeType::Reserved,
			                      consumed_range.name, false, consumed_range.placeholder_backed);
		}
		return KERNEL_ERROR_ENOMEM;
	}

	if (!g_physical_memory->Map(out_addr, direct_memory_start, len, prot, mode, gpu_mode)) {
		LOGF_COLOR(Log::Color::Red, "\t [Fail]\n");
		bool placeholder_restored = false;
		g_direct_memory_backing->Unmap(out_addr, len, committed_from_reserved,
		                               &placeholder_restored);
		if (placeholder_restored) {
			g_placeholder_address_space->AddFree(out_addr, len);
		}

		KYTY_NOT_IMPLEMENTED;

		return KERNEL_ERROR_EBUSY;
	}

	PhysicalMemory::AllocatedBlock mapped_block {};
	g_physical_memory->Find(direct_memory_start, false, &mapped_block);
	if (!g_virtual_ranges->Add(out_addr, len, direct_memory_start, prot, mapped_block.memory_type,
	                           VirtualRangeType::Direct, "", committed_from_reserved)) {
		GpuAccessMode rollback_gpu_mode = GpuAccessMode::NoAccess;
		g_physical_memory->Unmap(out_addr, len, &rollback_gpu_mode);
		bool placeholder_restored = false;
		g_direct_memory_backing->Unmap(out_addr, len, committed_from_reserved,
		                               &placeholder_restored);
		if (placeholder_restored) {
			g_placeholder_address_space->AddFree(out_addr, len);
		}
		return KERNEL_ERROR_EBUSY;
	}

	MapGpuRange(out_addr, len, gpu_mode);

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

static bool ReserveFixedHostRange(uint64_t start, uint64_t size) {
	constexpr uint64_t PAGE_SIZE = 0x4000;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SYSTEM_INFO system_info {};
	GetSystemInfo(&system_info);
	const uint64_t granularity = system_info.dwAllocationGranularity;
	if ((start & (granularity - 1u)) == 0 && (size & (granularity - 1u)) == 0) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(start), &info, sizeof(info)) != 0 &&
		    info.State == MEM_FREE && info.RegionSize >= size &&
		    VirtualMemory::ReserveFixed(start, size)) {
			return true;
		}
	}
#endif

	for (uint64_t addr = start; addr < start + size; addr += PAGE_SIZE) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &info, sizeof(info)) != 0) {
			if (info.State == MEM_COMMIT) {
				if (!VirtualMemory::Decommit(addr, PAGE_SIZE)) {
					LOGF_COLOR(Log::Color::Red,
					           "\t reserve-fixed replace: decommit failed at 0x%016" PRIx64 "\n",
					           addr);
					return false;
				}
				continue;
			}
			if (info.State == MEM_RESERVE) {
				continue;
			}
		}
#endif
		if (!VirtualMemory::ReserveFixed(addr, PAGE_SIZE)) {
			LOGF_COLOR(Log::Color::Red,
			           "\t reserve-fixed replace: reserve failed at 0x%016" PRIx64 "\n", addr);
			return false;
		}
	}

	return true;
}

static bool ReplaceFixedRangeWithReserved(uint64_t start, uint64_t size, bool* placeholder_backed) {
	EXIT_IF(placeholder_backed == nullptr);

	*placeholder_backed = false;

	struct ReplacedChunk {
		VirtualRanges::Range range {};
		VirtualMemory::Mode  mode           = VirtualMemory::Mode::NoAccess;
		GpuAccessMode        gpu_mode       = GpuAccessMode::NoAccess;
		bool                 shared_backing = false;
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
		replaced.shared_backing = (range.type == VirtualRangeType::Direct &&
		                           g_direct_memory_backing->Contains(current, chunk));
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
				host_restored =
				    (chunk.shared_backing
				         ? g_direct_memory_backing->MapFixed(chunk.range.start, chunk.range.size,
				                                             chunk.range.offset, chunk.mode)
				         : CommitFixedHostRange(chunk.range.start, chunk.range.size, chunk.mode));
				backend_restored =
				    g_physical_memory->Map(chunk.range.start, chunk.range.offset, chunk.range.size,
				                           chunk.range.protection, chunk.mode, chunk.gpu_mode);
			} else if (chunk.range.type == VirtualRangeType::Flexible ||
			           chunk.range.type == VirtualRangeType::Stack ||
			           chunk.range.type == VirtualRangeType::Pooled) {
				host_restored =
				    CommitFixedHostRange(chunk.range.start, chunk.range.size, chunk.mode);
				backend_restored = g_flexible_memory->Map(chunk.range.start, chunk.range.size,
				                                          chunk.range.protection, chunk.mode,
				                                          chunk.gpu_mode, chunk.range.name);
			}

			const bool range_restored = g_virtual_ranges->Add(
			    chunk.range.start, chunk.range.size, chunk.range.offset, chunk.range.protection,
			    chunk.range.memory_type, chunk.range.type, chunk.range.name,
			    chunk.range.committed_from_reserved, chunk.range.placeholder_backed);
			ok = host_restored && backend_restored && range_restored && ok;
		}
		return ok;
	};

	for (const auto& chunk: chunks) {
		if (chunk.range.type == VirtualRangeType::Pooled) {
			EXIT("reserve-fixed replacement of pooled memory is unsupported: addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     chunk.range.start, chunk.range.size);
		}
	}
	const bool gpu_unmapped = std::any_of(chunks.begin(), chunks.end(), [](const auto& chunk) {
		return IsCommittedRangeType(chunk.range.type) &&
		       IsGpuAddressRange(chunk.range.start, chunk.range.size);
	});
	for (const auto& chunk: chunks) {
		if (IsCommittedRangeType(chunk.range.type)) {
			UnmapGpuRange(chunk.range.start, chunk.range.size, chunk.gpu_mode);
		}
	}

	g_virtual_ranges->Remove(start, size);

	for (auto& chunk: chunks) {
		GpuAccessMode gpu_mode = GpuAccessMode::NoAccess;
		bool          unmapped = true;

		if (chunk.range.type == VirtualRangeType::Direct) {
			if (chunk.shared_backing) {
				bool chunk_placeholder_restored = false;
				if (!g_direct_memory_backing->Unmap(chunk.range.start, chunk.range.size, false,
				                                    &chunk_placeholder_restored)) {
					unmapped = false;
				} else if (chunk_placeholder_restored) {
					g_placeholder_address_space->AddFree(chunk.range.start, chunk.range.size);
				}
			}
			if (unmapped) {
				unmapped = g_physical_memory->Unmap(chunk.range.start, chunk.range.size, &gpu_mode);
				chunk.gpu_mode = gpu_mode;
			}
		} else if (chunk.range.type == VirtualRangeType::Flexible ||
		           chunk.range.type == VirtualRangeType::Stack ||
		           chunk.range.type == VirtualRangeType::Pooled) {
			unmapped = g_flexible_memory->Unmap(chunk.range.start, chunk.range.size, &gpu_mode);
			chunk.gpu_mode = gpu_mode;
		}

		if (!unmapped) {
			if (gpu_unmapped) {
				EXIT("reserve-fixed backend unmap failed after GPU unmap: addr=0x%016" PRIx64
				     " size=0x%016" PRIx64 "\n",
				     chunk.range.start, chunk.range.size);
			}
			LOGF_COLOR(Log::Color::Red,
			           "\t reserve-fixed replace: backend unmap failed at 0x%016" PRIx64
			           ", size=0x%016" PRIx64 ", type=%s\n",
			           chunk.range.start, chunk.range.size,
			           Common::EnumName(chunk.range.type).c_str());
			restore_chunks();
			return false;
		}
	}

	if (g_placeholder_address_space->ReserveFixed(start, size)) {
		*placeholder_backed = true;
	} else if (!ReserveFixedHostRange(start, size)) {
		if (gpu_unmapped) {
			EXIT("reserve-fixed host reservation failed after GPU unmap: addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     start, size);
		}
		restore_chunks();
		return false;
	}

	if (!g_virtual_ranges->Add(start, size, 0, 0, 0, VirtualRangeType::Reserved, "anon", false,
	                           *placeholder_backed)) {
		if (gpu_unmapped) {
			EXIT("reserve-fixed range registration failed after GPU unmap: addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     start, size);
		}
		if (*placeholder_backed) {
			g_placeholder_address_space->ReleaseFree(start, size);
		} else {
			VirtualMemory::Free(start);
		}
		LOGF_COLOR(Log::Color::Red,
		           "\t reserve-fixed replace: range add failed at 0x%016" PRIx64
		           ", size=0x%016" PRIx64 "\n",
		           start, size);
		restore_chunks();
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

	constexpr size_t PAGE_SIZE        = 0x4000;
	constexpr int    MAP_FIXED        = 0x10;
	constexpr int    MAP_NO_OVERWRITE = 0x80;

	if (addr == nullptr || len == 0 || (len & (PAGE_SIZE - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}
	if (alignment != 0 && (alignment & (alignment - 1)) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t out_addr            = 0;
	bool     range_already_added = false;
	bool     placeholder_backed  = false;
	if ((flags & MAP_FIXED) != 0) {
		if (in_addr == 0 || (in_addr & (PAGE_SIZE - 1)) != 0) {
			return KERNEL_ERROR_EINVAL;
		}
		if ((flags & MAP_NO_OVERWRITE) != 0 && g_virtual_ranges->HasOverlap(in_addr, len)) {
			return KERNEL_ERROR_ENOMEM;
		}
		if (ReplaceFixedRangeWithReserved(in_addr, len, &placeholder_backed)) {
			out_addr            = in_addr;
			range_already_added = true;
		}
	} else {
		alignment          = (alignment != 0 ? alignment : PAGE_SIZE);
		out_addr           = g_placeholder_address_space->ReserveAligned(in_addr, len, alignment);
		placeholder_backed = (out_addr != 0);
		if (out_addr == 0) {
			out_addr           = VirtualMemory::ReserveAligned(in_addr, len, alignment);
			placeholder_backed = false;
		}
	}

	if (out_addr == 0) {
		return KERNEL_ERROR_ENOMEM;
	}

	if (!range_already_added &&
	    !g_virtual_ranges->Add(out_addr, len, 0, 0, 0, VirtualRangeType::Reserved, "anon", false,
	                           placeholder_backed)) {
		if (!placeholder_backed || !g_placeholder_address_space->ReleaseFree(out_addr, len)) {
			VirtualMemory::Free(out_addr);
		}
		return KERNEL_ERROR_EBUSY;
	}

	*addr = reinterpret_cast<void*>(out_addr);

	LOGF("\t out_addr  = 0x%016" PRIx64 "\n"
	     "\t placeholder = %s\n",
	     out_addr, placeholder_backed ? "yes" : "no");

	return OK;
}

bool KernelHandleReservedRangeAccessViolation(uint64_t vaddr) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	VirtualRanges::Range range {};
	if (!g_virtual_ranges->Query(vaddr, 0, &range) ||
	    std::strncmp(range.name, "AMM", KERNEL_MAXIMUM_NAME_LENGTH) != 0) {
		return false;
	}
	EXIT("AMM virtual-memory unmap is unsupported: addr=0x%016" PRIx64 "\n", vaddr);
}

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

	EXIT_NOT_IMPLEMENTED(size == nullptr);

	*size = g_flexible_memory->Available();

	LOGF("\t *size = 0x%016" PRIx64 "\n", *size);

	return OK;
}

int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(uint64_t* size) {
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

static std::vector<VirtualRanges::Range> RequireProgramMemory(uint64_t vaddr, uint64_t size) {
	std::vector<VirtualRanges::Range> ranges;
	if (g_virtual_ranges == nullptr || !g_virtual_ranges->QuerySpan(vaddr, size, &ranges) ||
	    std::any_of(ranges.begin(), ranges.end(),
	                [](const auto& range) { return range.type != VirtualRangeType::Code; })) {
		EXIT("program-memory range is not fully mapped: addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     vaddr, size);
	}
	return ranges;
}

void RegisterProgramMemory(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode,
                           const char* name) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	if (g_virtual_ranges == nullptr || vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr ||
	    name == nullptr ||
	    !g_virtual_ranges->Add(vaddr, size, 0, ProgramProtection(mode), 0, VirtualRangeType::Code,
	                           name)) {
		EXIT("failed to register program memory: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
}

void UpdateProgramMemoryProtection(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);
	RequireProgramMemory(vaddr, size);
	g_virtual_ranges->Protect(vaddr, size, ProgramProtection(mode));
}

void UnregisterProgramMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard<std::recursive_mutex> memory_operation_lock(g_memory_operation_mutex);

	for (const auto& range: RequireProgramMemory(vaddr, size)) {
		const auto gpu_mode = GetGpuAccessMode(range.protection);
		if (gpu_mode != GpuAccessMode::NoAccess) {
			if (!GetGpuResources().IsMapped(range.start, range.size)) {
				EXIT("program GPU range is not tracked: addr=0x%016" PRIx64 " size=0x%016" PRIx64
				     "\n",
				     range.start, range.size);
			}
			UnmapGpuRange(range.start, range.size, gpu_mode);
		}
	}
	if (!g_virtual_ranges->Remove(vaddr, size)) {
		EXIT("failed to unregister program memory: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
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
	for (const auto& old_range: old_ranges) {
		const auto old_gpu_mode = GetGpuAccessMode(old_range.protection);
		if (old_gpu_mode == GpuAccessMode::NoAccess) {
			continue;
		}
		if ((old_range.type != VirtualRangeType::Stack &&
		     old_range.type != VirtualRangeType::Code) ||
		    !GetGpuResources().IsMapped(old_range.start, old_range.size)) {
			EXIT("GPU protection transition requires tracked stack/code memory: addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " type=%s\n",
			     old_range.start, old_range.size, Common::EnumName(old_range.type).c_str());
		}
		UnmapGpuRange(old_range.start, old_range.size, old_gpu_mode);
	}

	VirtualMemory::Mode old_mode {};
	bool                ok = ProtectCommittedHostMemory(aligned_addr, aligned_len, mode, &old_mode);

	if (!ok) {
		EXIT("host memory-protection update failed: addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " prot=0x%08x\n",
		     aligned_addr, aligned_len, prot);
	}
	g_virtual_ranges->Protect(aligned_addr, aligned_len, prot);
	g_direct_memory_backing->UpdateProtection(aligned_addr, aligned_len, mode);
	MapGpuRange(aligned_addr, aligned_len, gpu_mode);

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
	constexpr int MAP_FIXED = 0x10;
	return KernelBatchMap2(entries, num_entries, num_entries_out, MAP_FIXED);
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
		                           reserved_range.name, false, reserved_range.placeholder_backed)) {
			g_virtual_ranges->Add(out_vaddr, len, reserved_range.offset, reserved_range.protection,
			                      reserved_range.memory_type, reserved_range.type,
			                      reserved_range.name, reserved_range.committed_from_reserved,
			                      reserved_range.placeholder_backed);
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
		g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved, old_range.name,
		                      false, old_range.placeholder_backed);
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
		g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved, old_range.name,
		                      false, old_range.placeholder_backed);
	};

	for (const auto& mapping: mappings) {
		auto       failure_reason = DirectMemoryBacking::FailureReason::None;
		const bool placeholder_ready =
		    g_placeholder_address_space->Consume(mapping.vaddr, mapping.size);
		const bool ok =
		    placeholder_ready
		        ? g_direct_memory_backing->MapExistingPlaceholderFixed(
		              mapping.vaddr, mapping.size, mapping.phys_addr, mode, &failure_reason)
		        : g_direct_memory_backing->MapFixed(mapping.vaddr, mapping.size, mapping.phys_addr,
		                                            mode, &failure_reason);
		if (!ok) {
			if (placeholder_ready) {
				g_placeholder_address_space->AddFree(mapping.vaddr, mapping.size);
			}
			LOGF_COLOR(Log::Color::Red, "\t pool backing map failed: %s\n",
			           DirectMemoryBacking::GetFailureReasonName(failure_reason));
			rollback();
			return KERNEL_ERROR_ENOMEM;
		}
		mapped.push_back(mapping);
	}

	if (!g_virtual_ranges->Add(vaddr, len, 0, prot, type, VirtualRangeType::Pooled, old_range.name,
	                           true, old_range.placeholder_backed)) {
		rollback();
		return KERNEL_ERROR_EBUSY;
	}

	MapGpuRange(vaddr, len, gpu_mode);

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

	VirtualRanges::Range old_range {};
	if (!g_virtual_ranges->Query(vaddr, 0, &old_range)) {
		return KERNEL_ERROR_EACCES;
	}
	const auto chunk_len = std::min<uint64_t>(len, old_range.size - (vaddr - old_range.start));
	if (old_range.type == VirtualRangeType::PoolReserved) {
		return chunk_len < len
		           ? KernelMemoryPoolDecommit(reinterpret_cast<void*>(vaddr + chunk_len),
		                                      len - chunk_len, flags)
		           : OK;
	}
	if (old_range.type != VirtualRangeType::Pooled) {
		return KERNEL_ERROR_EACCES;
	}
	if (chunk_len < len) {
		const int ret = KernelMemoryPoolDecommit(addr, chunk_len, flags);
		return ret == OK ? KernelMemoryPoolDecommit(reinterpret_cast<void*>(vaddr + chunk_len),
		                                            len - chunk_len, flags)
		                 : ret;
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
	UnmapGpuRange(vaddr, len, decoded_gpu);
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
	old_range.placeholder_backed = true;
	g_virtual_ranges->Add(vaddr, len, 0, 0, 0, VirtualRangeType::PoolReserved, old_range.name,
	                      false, old_range.placeholder_backed);

	if (g_free_callback != nullptr) {
		g_free_callback(vaddr, len);
	}

	MemoryPoolSubtractCommitted(len);

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

} // namespace Libs::LibKernel::Memory
