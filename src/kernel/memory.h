#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/virtualMemory.h"

#include <array>
#include <cstdint>
#include <string>

namespace Libs::Graphics {
class RenderContext;
enum class PageFaultAccess;
} // namespace Libs::Graphics

namespace Libs::LibKernel::Memory {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Memory";
	static constexpr auto        initialize = Libs::LibKernel::Memory::Initialize;
	static constexpr auto        shutdown   = Libs::LibKernel::Memory::Shutdown;
};

using callback_func_t = void (*)(uintptr_t addr, size_t size);

constexpr uint32_t KERNEL_MAXIMUM_NAME_LENGTH = 32;
constexpr uint64_t kExtendedMemoryBase       = 0x080000000000ull;
constexpr uint64_t kExtendedMemorySize       = 512ull * 1024 * 1024 * 1024;

struct VirtualQueryInfo {
	uintptr_t start;
	uintptr_t end;
	uint64_t  offset;
	int32_t   protection;
	int32_t   memory_type;
	uint32_t  is_flexible  : 1;
	uint32_t  is_direct    : 1;
	uint32_t  is_stack     : 1;
	uint32_t  is_pooled    : 1;
	uint32_t  is_committed : 1;
	uint32_t  is_gpu_prt   : 1;
	uint32_t  amm_usage    : 1;
	uint32_t  reserved     : 1;
	char      name[KERNEL_MAXIMUM_NAME_LENGTH];
	uint8_t   gpu_mask_id;
	uint8_t   reserved2;
};

static_assert(sizeof(VirtualQueryInfo) == 72, "VirtualQueryInfo struct size is incorrect");

struct KernelBatchMapEntry {
	void*         start;
	uint64_t      offset;
	uint64_t      length;
	unsigned char protection;
	unsigned char type;
	int16_t       reserved;
	int32_t       operation;
};

static_assert(sizeof(KernelBatchMapEntry) == 32, "KernelBatchMapEntry struct size is incorrect");

struct KernelMemoryPoolBatchEntry {
	uint32_t op;
	uint32_t flags;
	union {
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} commit;
		struct {
			void*    addr;
			uint64_t len;
		} decommit;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
		} protect;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} type_protect;
		struct {
			void*    dst;
			void*    src;
			uint64_t len;
		} move;
		uintptr_t padding[3];
	};
};

static_assert(sizeof(KernelMemoryPoolBatchEntry) == 32,
              "KernelMemoryPoolBatchEntry struct size is incorrect");

struct KernelMemoryPoolBlockStats {
	int32_t available_flushed_blocks;
	int32_t available_cached_blocks;
	int32_t allocated_flushed_blocks;
	int32_t allocated_cached_blocks;
};

static_assert(sizeof(KernelMemoryPoolBlockStats) == 16,
              "KernelMemoryPoolBlockStats struct size is incorrect");

void                   RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func);
void                   SetFlexibleMemorySize(uint64_t size);
int AllocateDirectMemory(int64_t search_start, int64_t search_end, size_t size, size_t alignment,
                         int memory_type, int64_t* phys_addr_out, bool automatic = false);
int MapAutomaticMemory(uint64_t vaddr, size_t size, int type, int prot);
bool                   TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size);
bool                   TryReadBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   TryReadGpuCleanBacking(uint64_t vaddr, void* data, uint64_t size);
using BackingVisitor = void (*)(void* context, const uint8_t* data, uint64_t size);
// TryReadGpuCleanBacking, handing each backing piece to `visit` in place instead of copying it.
bool TryVisitGpuCleanBacking(uint64_t vaddr, uint64_t size, BackingVisitor visit, void* context);
bool                   TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   HasGpuOwnedBytes(uint64_t vaddr, uint64_t size);
bool                   TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size);
// Zero-fills the destination, copies back whatever the guest has committed over the span, and
// returns how many bytes were real. For a caller that cannot refuse, such as an image upload.
uint64_t                  ReadBackingPartial(uint64_t vaddr, void* data, uint64_t size);
// The same for any mapped guest span; a concurrent unmap leaves zeros instead of a host fault.
uint64_t ReadGuestMemoryPartial(uint64_t vaddr, void* data, uint64_t size);
// The committed prefix, whether a PRT aperture covers it, and the covering plus neighbouring
// virtual ranges. For failure reports only.
[[nodiscard]] std::string DescribeGuestRange(uint64_t vaddr, uint64_t size);
// Which of TryReadGpuCleanBacking's conditions refuses this range, for a caller that has to
// report why a descriptor read failed. Never null; says so when the range reads back now.
[[nodiscard]] const char* DescribeGpuBackingRefusal(uint64_t vaddr, uint64_t size);
// Clips to the contiguous committed mapping at vaddr; 0 when nothing is mapped there.
[[nodiscard]] uint64_t ClampRangeSize(uint64_t vaddr, uint64_t size);
// Clips to the end of the last committed mapping inside the range, holes included; 0 when
// nothing is mapped at vaddr.
[[nodiscard]] uint64_t ClampSparseRangeSize(uint64_t vaddr, uint64_t size);
// The committed virtual range holding vaddr, as one mapping call left it; false when nothing
// committed is mapped there.
[[nodiscard]] bool QueryCommittedRange(uint64_t vaddr, uint64_t* start, uint64_t* size);

// How often a short or unmapped buffer range is worth reporting. A resource descriptor is re-bound
// every draw, so one guest allocation that outlives its committed extent repeats the same outcome
// hundreds of thousands of times. Reporting only the first would hide the recurrence that
// separates a stale descriptor from a one-off, so repeats are reported at exponentially sparser
// intervals with their count.
class BufferRangeReportPolicy {
public:
	struct Decision {
		bool     report      = false;
		uint64_t occurrences = 0;
	};

	// Set-associative and self-evicting, so the table never allocates on the draw path; an evicted
	// range simply reports as new again.
	static constexpr uint32_t WAY_COUNT  = 4;
	static constexpr uint32_t SET_COUNT  = 64;
	static constexpr uint32_t SLOT_COUNT = WAY_COUNT * SET_COUNT;

	Decision Observe(uint64_t vaddr, uint64_t requested, uint64_t clamped) {
		const uint32_t first  = Index(vaddr, requested) * WAY_COUNT;
		Slot*          victim = nullptr;
		Slot*          match  = nullptr;
		for (uint32_t way = 0; way < WAY_COUNT; way++) {
			auto& candidate = m_slots.at(first + way);
			if (candidate.occurrences != 0 && candidate.vaddr == vaddr &&
			    candidate.requested == requested) {
				match = &candidate;
				break;
			}
			if (victim == nullptr || candidate.occurrences == 0 ||
			    (victim->occurrences != 0 && candidate.last_use < victim->last_use)) {
				victim = &candidate;
			}
		}
		const uint64_t now = ++m_clock;
		if (match == nullptr || match->clamped != clamped) {
			auto& slot = match != nullptr ? *match : *victim;
			slot       = {vaddr, requested, clamped, 1, FIRST_INTERVAL, now};
			return {true, 1};
		}
		auto& slot    = *match;
		slot.last_use = now;
		slot.occurrences++;
		if (slot.occurrences < slot.next_report) {
			return {false, slot.occurrences};
		}
		if (slot.next_report <= UINT64_MAX / INTERVAL_GROWTH) {
			slot.next_report *= INTERVAL_GROWTH;
		}
		return {true, slot.occurrences};
	}

private:
	static constexpr uint64_t FIRST_INTERVAL  = 2;
	static constexpr uint64_t INTERVAL_GROWTH = 4;

	struct Slot {
		uint64_t vaddr       = 0;
		uint64_t requested   = 0;
		uint64_t clamped     = 0;
		uint64_t occurrences = 0;
		uint64_t next_report = 0;
		uint64_t last_use    = 0;
	};

	static uint32_t Index(uint64_t vaddr, uint64_t requested) {
		uint64_t mix = vaddr * 0x9e3779b97f4a7c15ull + requested;
		mix ^= mix >> 29u;
		mix *= 0xbf58476d1ce4e5b9ull;
		mix ^= mix >> 32u;
		return static_cast<uint32_t>(mix % SET_COUNT);
	}

	std::array<Slot, SLOT_COUNT> m_slots {};
	uint64_t                     m_clock = 0;
};

void                   WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept;
void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
void                   InstallGpuResources(Graphics::RenderContext* renderer) noexcept;
[[nodiscard]] bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept;

int KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags,
                                               const char* name);
int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags);
int KYTY_SYSV_ABI KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name);
int KYTY_SYSV_ABI KernelClearVirtualRangeName(const void* addr, uint64_t len);
int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len);
size_t KYTY_SYSV_ABI KernelGetDirectMemorySize();
int KYTY_SYSV_ABI    KernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                                     size_t alignment, int64_t* phys_addr_out,
                                                     size_t* size_out);
int KYTY_SYSV_ABI    KernelGetPageTableStats(int* cpu_total, int* cpu_available, int* gpu_total,
                                             int* gpu_available);
int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                             size_t alignment, int memory_type,
                                             int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type,
                                                 int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags,
                                         int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags,
                                             int64_t direct_memory_start, size_t alignment,
                                             const char* name);
int KYTY_SYSV_ABI KernelSetPrtAperture(int index, void* addr, size_t len);
int KYTY_SYSV_ABI KernelGetPrtAperture(int index, void** addr, size_t* len);
int KYTY_SYSV_ABI KernelIsAddressSanitizerEnabled();
int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);
int KYTY_SYSV_ABI KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size);
int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info,
                                     uint64_t info_size);
int KYTY_SYSV_ABI KernelIsStack(void* addr, void** start, void** end);
int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
int KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot);
int KYTY_SYSV_ABI KernelMtypeprotect(const void* addr, size_t len, int type, int prot);
int KYTY_SYSV_ABI KernelBatchMap(KernelBatchMapEntry* entries, int num_entries,
                                 int* num_entries_out);
int KYTY_SYSV_ABI KernelBatchMap2(KernelBatchMapEntry* entries, int num_entries,
                                  int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolExpand(int64_t search_start, int64_t search_end, size_t len,
                                         size_t alignment, int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolReserve(void* addr_in, size_t len, size_t alignment, int flags,
                                          void** addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolCommit(void* addr, size_t len, int type, int prot, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolDecommit(void* addr, size_t len, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolBatch(const KernelMemoryPoolBatchEntry* entries, int num_entries,
                                        int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolGetBlockStats(KernelMemoryPoolBlockStats* output,
                                                size_t                      output_size);

uint64_t AllocateProgramMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name);
void SetProgramMemoryProtection(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode);
uint64_t AllocateRuntimeMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name,
                               bool fixed = false);
uint64_t AllocateGuestStackMemory(uint64_t search_addr, uint64_t size,
                                  Common::VirtualMemory::Mode mode, const char* name);
bool     ProtectGuestMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode,
                            Common::VirtualMemory::Mode* old_mode = nullptr);
// Transient PageManager watch state; does not change the guest mapping's semantic protection.
bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode);
bool FreeGuestMemory(uint64_t vaddr, uint64_t size);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void     TestBeforeNextBackingMap(callback_func_t callback);
void     TestSetBackingReadCallback(callback_func_t callback);
void     TestFailNextPhysicalMemoryUnmap();
void     TestFailPhysicalMemoryUnmapAfter(uint32_t successful_unmaps);
void     TestFailGuestBackingStoreUnmapAfter(uint32_t successful_unmaps);
void     TestFailNextFixedReserveRangeRegistration();
void     TestFailNextVirtualRangeReplacement();
bool     TestPlaceholderRangeIsFree(uint64_t vaddr, uint64_t size);
bool     TestGuestAddressRangeIsOwned(uint64_t vaddr, uint64_t size);
bool     TestGuestBackingOutsideAddressSpace();
uint64_t TestGuestBackingSize();
bool     TestGuestFreeRangeBounds();
#endif

} // namespace Libs::LibKernel::Memory

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_ */
