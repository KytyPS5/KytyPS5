#include "common/virtualMemory.h"
#include "graphics/host_gpu/pageManager.h"

#include <algorithm>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#elif defined(__APPLE__)
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <map>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <map>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Libs::Graphics::PageManager;
using Libs::Graphics::RegionBits;
using Libs::Graphics::TRACKER_PAGE_SIZE;
using Libs::Graphics::TRACKER_REGION_SIZE;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "PageManagerTests: failed: %s\n", text);
    std::abort();
  }
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
using DWORD = uint32_t;
constexpr uint32_t PAGE_NOACCESS = 1;
constexpr uint32_t PAGE_READONLY = 2;
constexpr uint32_t PAGE_READWRITE = 3;
constexpr uint32_t MEM_RELEASE = 0;

int ToHostProt(uint32_t protection) {
  switch (protection) {
  case PAGE_NOACCESS:
    return PROT_NONE;
  case PAGE_READONLY:
    return PROT_READ;
  default:
    return PROT_READ | PROT_WRITE;
  }
}

uint32_t Protection(const void *address) {
#if defined(__APPLE__)
  mach_vm_address_t region_address =
      reinterpret_cast<mach_vm_address_t>(address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info{};
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  Check(mach_vm_region(mach_task_self(), &region_address, &region_size,
                       VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &info_count,
                       &object_name) == KERN_SUCCESS,
        "mach_vm_region failed");
  if (object_name != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), object_name);
  }
  return (info.protection & VM_PROT_WRITE) != 0
             ? PAGE_READWRITE
             : (info.protection & VM_PROT_READ) != 0 ? PAGE_READONLY
                                                     : PAGE_NOACCESS;
#else
  const auto addr = reinterpret_cast<uintptr_t>(address);
  std::FILE *maps = std::fopen("/proc/self/maps", "r");
  Check(maps != nullptr, "open /proc/self/maps failed");
  char line[512];
  uint32_t result = 0;
  while (std::fgets(line, sizeof(line), maps) != nullptr) {
    unsigned long start = 0;
    unsigned long end = 0;
    char perms[8]{};
    if (std::sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) {
      continue;
    }
    if (addr >= start && addr < end) {
      result = perms[1] == 'w'   ? PAGE_READWRITE
               : perms[0] == 'r' ? PAGE_READONLY
                                 : PAGE_NOACCESS;
      break;
    }
  }
  std::fclose(maps);
  return result;
#endif
}

std::map<void *, size_t> &AllocationSizes() {
  static std::map<void *, size_t> sizes;
  return sizes;
}

int VirtualFree(void *address, size_t, DWORD) {
  auto &sizes = AllocationSizes();
  auto it = sizes.find(address);
  if (it == sizes.end()) {
    return 0;
  }
#if defined(__APPLE__)
  const int ok = mach_vm_deallocate(mach_task_self(),
                                    reinterpret_cast<mach_vm_address_t>(address),
                                    it->second) == KERN_SUCCESS
                     ? 1
                     : 0;
#else
  const int ok = ::munmap(address, it->second) == 0 ? 1 : 0;
#endif
  sizes.erase(it);
  return ok;
}

int VirtualProtect(void *address, size_t size, uint32_t protection,
                   DWORD *old_protection) {
  if (old_protection != nullptr) {
    *old_protection = Protection(address);
  }
#if defined(__APPLE__)
  return mach_vm_protect(mach_task_self(),
                         reinterpret_cast<mach_vm_address_t>(address), size,
                         false, static_cast<vm_prot_t>(ToHostProt(protection))) ==
                 KERN_SUCCESS
             ? 1
             : 0;
#else
  return ::mprotect(address, size, ToHostProt(protection)) == 0 ? 1 : 0;
#endif
}
#else
uint32_t Protection(const void *address) {
  MEMORY_BASIC_INFORMATION info{};
  Check(VirtualQuery(address, &info, sizeof(info)) != 0, "VirtualQuery failed");
  return info.Protect;
}
#endif

bool IsWritable(const void *address) {
  return Protection(address) == PAGE_READWRITE;
}

uint64_t g_protection_calls = 0;
struct ProtectionCall {
  uint64_t address;
  uint64_t size;
  bool access_changed = true;
};
std::vector<ProtectionCall> g_protection_ranges;
std::mutex g_protection_log_mutex;

bool ProtectAddressSpace(uint64_t vaddr, uint64_t size,
                         Common::VirtualMemory::Mode mode,
                         bool access_changed = true) {
  uint32_t protection = PAGE_NOACCESS;
  if (mode == Common::VirtualMemory::Mode::Read) {
    protection = PAGE_READONLY;
  } else if (mode == Common::VirtualMemory::Mode::ReadWrite) {
    protection = PAGE_READWRITE;
  }
  DWORD old_protection = 0;
  {
    std::lock_guard lock(g_protection_log_mutex);
    g_protection_calls++;
    g_protection_ranges.push_back({vaddr, size, access_changed});
  }
  return VirtualProtect(reinterpret_cast<void *>(vaddr), size, protection,
                        &old_protection) != 0;
}

uint8_t *Allocate(uint64_t size, uint32_t protection = PAGE_READWRITE,
                  uintptr_t test_address = 0x0000000200010000ull) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(test_address), size,
                   MEM_RESERVE | MEM_COMMIT, protection));
  Check(memory == reinterpret_cast<void *>(test_address),
        "fixed low VirtualAlloc failed");
#elif defined(__APPLE__)
  mach_vm_address_t raw = 0;
  bool allocated = false;
  constexpr uintptr_t stride = 0x0000000100000000ull;
  for (uintptr_t candidate = test_address;
       Libs::Graphics::GuestRange{candidate, size}.Valid(); candidate += stride) {
    raw = candidate;
    if (mach_vm_allocate(mach_task_self(), &raw, size, VM_FLAGS_FIXED) !=
        KERN_SUCCESS) {
      continue;
    }
    if (mach_vm_protect(mach_task_self(), raw, size, false,
                        static_cast<vm_prot_t>(ToHostProt(protection))) ==
        KERN_SUCCESS) {
      allocated = true;
      break;
    }
    mach_vm_deallocate(mach_task_self(), raw, size);
  }
  Check(allocated, "no free fixed guest address found");
  auto *memory = reinterpret_cast<uint8_t *>(raw);
  AllocationSizes()[memory] = static_cast<size_t>(size);
#else
  void *raw = ::mmap(reinterpret_cast<void *>(test_address), size,
                     ToHostProt(protection),
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  Check(raw == reinterpret_cast<void *>(test_address), "fixed low mmap failed");
  auto *memory = static_cast<uint8_t *>(raw);
  AllocationSizes()[raw] = static_cast<size_t>(size);
#endif
  return memory;
}

void TestWatchAndUnwatch(uint64_t base = 0x0000000200010000ull) {
  g_protection_calls = 0;
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size * 2, PAGE_READWRITE, base);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.UpdatePageWatchers<true>(address, page_size);
  Check(Protection(memory) == PAGE_READONLY && IsWritable(memory + page_size),
        "write watch installed incorrect protections");
  Check(g_protection_calls != 0,
        "watch protection bypassed the address-space owner callback");
  manager.UpdatePageWatchers<false>(address, page_size);
  Check(IsWritable(memory), "write unwatch did not restore access");
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestSharedWatcherCounts() {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.UpdatePageWatchers<true>(address + 8, 32);
  manager.UpdatePageWatchers<true>(address + 128, 64);
  manager.UpdatePageWatchers<false>(address + 8, 32);
  Check(Protection(memory) == PAGE_READONLY,
        "first unwatch released a shared watcher");
  manager.UpdatePageWatchers<false>(address + 128, 64);
  Check(IsWritable(memory), "last unwatch did not restore access");
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestCrossRegionRange() {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  constexpr uint64_t region_size = 4ull * 1024ull * 1024ull;
  auto *memory = Allocate(region_size * 2);
  const auto base = reinterpret_cast<uint64_t>(memory);
  const auto boundary = (base + region_size - 1) & ~(region_size - 1);
  Check(boundary >= base + page_size &&
            boundary + page_size <= base + region_size * 2,
        "test allocation does not contain a region boundary");

  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchers<true>(boundary - page_size, page_size * 2);
  Check(g_protection_calls == 2 && g_protection_ranges.size() == 2 &&
            g_protection_ranges[0].address == boundary - page_size &&
            g_protection_ranges[0].size == page_size &&
            g_protection_ranges[1].address == boundary &&
            g_protection_ranges[1].size == page_size,
        "cross-region watch was not split only at the region boundary");
  Check(!IsWritable(reinterpret_cast<void *>(boundary - page_size)) &&
            !IsWritable(reinterpret_cast<void *>(boundary)),
        "cross-region watch did not protect both pages");
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchers<false>(boundary - page_size, page_size * 2);
  Check(g_protection_calls == 2 && g_protection_ranges.size() == 2 &&
            g_protection_ranges[0].address == boundary - page_size &&
            g_protection_ranges[0].size == page_size &&
            g_protection_ranges[1].address == boundary &&
            g_protection_ranges[1].size == page_size,
        "cross-region unwatch was not split only at the region boundary");
  Check(IsWritable(reinterpret_cast<void *>(boundary - page_size)) &&
            IsWritable(reinterpret_cast<void *>(boundary)),
        "cross-region unwatch did not restore both pages");
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestBatchedWatcherRanges() {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  constexpr uint64_t region_size = 4ull * 1024ull * 1024ull;
  constexpr uint64_t allocation_size = region_size * 3;
  auto *memory = Allocate(allocation_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.UpdatePageWatchers<true>(address + page_size, page_size);
  manager.UpdatePageWatchers<true>(address + page_size * 3, page_size);
  manager.UpdatePageWatchers<true>(address, page_size * 5);
  manager.UpdatePageWatchers<false>(address, page_size * 5);
  Check(IsWritable(memory) && Protection(memory + page_size) == PAGE_READONLY &&
            IsWritable(memory + page_size * 2) &&
            Protection(memory + page_size * 3) == PAGE_READONLY &&
            IsWritable(memory + page_size * 4),
        "fragmented unwatch lost overlapping watcher counts");
  manager.UpdatePageWatchers<false>(address + page_size, page_size);
  manager.UpdatePageWatchers<false>(address + page_size * 3, page_size);

  g_protection_calls = 0;
  manager.UpdatePageWatchers<true>(address, allocation_size);
  Check(g_protection_calls == 4 && Protection(memory) == PAGE_READONLY &&
            Protection(memory + region_size) == PAGE_READONLY &&
            Protection(memory + region_size * 2) == PAGE_READONLY &&
            Protection(memory + allocation_size - page_size) == PAGE_READONLY,
        "large watch was not batched and protected by tracking region");
  manager.UpdatePageWatchers<false>(address, allocation_size);
  Check(IsWritable(memory) && IsWritable(memory + region_size) &&
            IsWritable(memory + region_size * 2) &&
            IsWritable(memory + allocation_size - page_size),
        "large cross-region unwatch did not restore the full range");

  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestRegionMaskWatcherRanges() {
  PageManager manager;
  constexpr auto page_size = TRACKER_PAGE_SIZE;
  constexpr auto region_size = TRACKER_REGION_SIZE;
  auto *memory = Allocate(region_size * 2);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  Check(region_base + region_size <= allocation_base + region_size * 2,
        "test allocation does not contain a complete tracking region");

  RegionBits full_mask;
  full_mask.Fill();
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true>(region_base, full_mask);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].address == region_base &&
            g_protection_ranges[0].size == region_size,
        "full region mask did not use one protection span");
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false>(region_base, full_mask);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].address == region_base &&
            g_protection_ranges[0].size == region_size,
        "full region unmask did not use one protection span");

  RegionBits sparse_mask;
  sparse_mask.Set(1);
  sparse_mask.Set(3);
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true>(region_base, sparse_mask);
  Check(g_protection_calls == 2 &&
            Protection(reinterpret_cast<void *>(region_base + page_size)) ==
                PAGE_READONLY &&
            IsWritable(reinterpret_cast<void *>(region_base + page_size * 2)) &&
            Protection(reinterpret_cast<void *>(region_base + page_size * 3)) ==
                PAGE_READONLY,
        "sparse region mask installed incorrect write watchers");
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false>(region_base, sparse_mask);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].address == region_base + page_size &&
            g_protection_ranges[0].size == page_size * 3,
        "sparse unmask did not bridge a compatible gap");

  manager.UpdatePageWatchersForRegion<true>(region_base, sparse_mask);
  g_protection_calls = 0;
  manager.UpdatePageWatchersForRegion<true>(region_base, sparse_mask);
  Check(g_protection_calls == 0,
        "duplicate sparse watch changed an already protected range");
  manager.UpdatePageWatchersForRegion<false>(region_base, sparse_mask);
  Check(g_protection_calls == 0,
        "first sparse unwatch released a duplicate watcher");
  manager.UpdatePageWatchersForRegion<false>(region_base, sparse_mask);

  manager.UpdatePageWatchers<true>(region_base + page_size * 2, page_size);
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true>(region_base, sparse_mask);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].address == region_base + page_size &&
            g_protection_ranges[0].size == page_size * 3,
        "sparse mask did not bridge a compatible protected gap");
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false>(region_base, sparse_mask);
  Check(g_protection_calls == 2 && g_protection_ranges.size() == 2 &&
            g_protection_ranges[0].address == region_base + page_size &&
            g_protection_ranges[0].size == page_size &&
            g_protection_ranges[1].address == region_base + page_size * 3 &&
            g_protection_ranges[1].size == page_size,
        "sparse unmask crossed an incompatible protected gap");
  manager.UpdatePageWatchers<false>(region_base + page_size * 2, page_size);

  g_protection_calls = 0;
  manager.UpdatePageWatchersForRegion<true, true>(region_base, sparse_mask);
  Check(g_protection_calls == 2 &&
            Protection(reinterpret_cast<void *>(region_base + page_size)) ==
                PAGE_NOACCESS &&
            Protection(reinterpret_cast<void *>(region_base + page_size * 3)) ==
                PAGE_NOACCESS,
        "sparse read mask did not deny access");
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false, true>(region_base, sparse_mask);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].size == page_size * 3,
        "sparse read unmask did not bridge a compatible gap");

  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestWriteWatchesKeepAccess() {
  PageManager manager;
  constexpr auto page_size = TRACKER_PAGE_SIZE;
  constexpr auto region_size = TRACKER_REGION_SIZE;
  auto *memory = Allocate(region_size * 2);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  RegionBits mask;
  mask.Set(1);
  const auto only_access = [](bool changed) {
    std::lock_guard lock(g_protection_log_mutex);
    return !g_protection_ranges.empty() &&
           std::all_of(g_protection_ranges.begin(), g_protection_ranges.end(),
                       [changed](const ProtectionCall &call) {
                         return call.access_changed == changed;
                       });
  };

  // Write watches move pages between read-only and read-write only.
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true>(region_base, mask);
  const bool write_watch = only_access(false);
  // Access watches add or remove read access.
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true, true>(region_base, mask);
  const bool access_watch = only_access(true) &&
                            Protection(reinterpret_cast<void *>(
                                region_base + page_size)) == PAGE_NOACCESS;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false, true>(region_base, mask);
  const bool access_unwatch = only_access(true) &&
                              Protection(reinterpret_cast<void *>(
                                  region_base + page_size)) == PAGE_READONLY;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false>(region_base, mask);
  const bool write_unwatch =
      only_access(false) &&
      IsWritable(reinterpret_cast<void *>(region_base + page_size));
  Check(write_watch && write_unwatch,
        "write watch changes reported an access change");
  Check(access_watch && access_unwatch,
        "access watch changes did not report an access change");
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestTrackingFaults() {
  using Libs::Graphics::PageFaultAccess;
  PageManager manager;
  constexpr auto page_size = TRACKER_PAGE_SIZE;
  constexpr auto region_size = TRACKER_REGION_SIZE;
  auto *memory = Allocate(region_size * 2);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  const auto page = region_base + page_size;
  const auto *page_ptr = reinterpret_cast<const void *>(page);

  Check(manager.IsTrackingFault(page, PageFaultAccess::Write),
        "a write fault on a never-tracked writable page was not claimed");
  manager.UpdatePageWatchers<true>(page, page_size);
  Check(manager.IsTrackingFault(page, PageFaultAccess::Write),
        "a write fault on a write-watched page was not claimed");
  manager.UpdatePageWatchers<false>(page, page_size);
  // Released since the fault: the retry passes.
  Check(IsWritable(page_ptr) &&
            manager.IsTrackingFault(page, PageFaultAccess::Write),
        "a write fault on a released writable page was not claimed");
#if !defined(__APPLE__)
  // The page's own protection forbids the access: the retry would fault again.
  Check(VirtualProtect(const_cast<void *>(page_ptr), page_size, PAGE_READONLY,
                       nullptr) != 0,
        "VirtualProtect failed");
  Check(!manager.IsTrackingFault(page, PageFaultAccess::Write),
        "a write fault on an unwatched read-only page was claimed");
  Check(manager.IsTrackingFault(page, PageFaultAccess::Read),
        "a read fault on a released readable page was not claimed");
  Check(VirtualProtect(const_cast<void *>(page_ptr), page_size, PAGE_NOACCESS,
                       nullptr) != 0,
        "VirtualProtect failed");
  Check(!manager.IsTrackingFault(page, PageFaultAccess::Read),
        "a read fault on an unwatched inaccessible page was claimed");
#endif
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestRegionEndpointBatching() {
  PageManager manager;
  constexpr auto page_size = TRACKER_PAGE_SIZE;
  constexpr auto region_size = TRACKER_REGION_SIZE;
  constexpr auto last_page = region_size / page_size - 1;
  auto *memory = Allocate(region_size * 2);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  Check(region_base + region_size <= allocation_base + region_size * 2,
        "test allocation does not contain a complete tracking region");

  RegionBits endpoints;
  endpoints.Set(0);
  endpoints.Set(last_page);
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true>(region_base, endpoints);
  Check(g_protection_calls == 2 && g_protection_ranges.size() == 2 &&
            g_protection_ranges[0].address == region_base &&
            g_protection_ranges[0].size == page_size &&
            g_protection_ranges[1].address ==
                region_base + region_size - page_size &&
            g_protection_ranges[1].size == page_size,
        "endpoint watch did not protect only the selected pages");

  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false>(region_base, endpoints);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].address == region_base &&
            g_protection_ranges[0].size == region_size,
        "endpoint unwatch did not coalesce the compatible 4 MiB span");

  RegionBits full_mask;
  full_mask.Fill();
  manager.UpdatePageWatchersForRegion<true>(region_base, full_mask);
  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true>(region_base, full_mask);
  Check(g_protection_calls == 0,
        "duplicate full-region watch issued a redundant protection call");
  manager.UpdatePageWatchersForRegion<false>(region_base, full_mask);
  Check(g_protection_calls == 0,
        "first full-region unwatch released overlapping watcher counts");
  manager.UpdatePageWatchersForRegion<false>(region_base, full_mask);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].address == region_base &&
            g_protection_ranges[0].size == region_size,
        "last full-region unwatch did not use one 4 MiB protection call");

  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestReadWriteWatcherInteractions() {
  PageManager manager;
  constexpr auto page_size = TRACKER_PAGE_SIZE;
  constexpr auto region_size = TRACKER_REGION_SIZE;
  auto *memory = Allocate(region_size * 2);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  Check(region_base + region_size <= allocation_base + region_size * 2,
        "test allocation does not contain a complete tracking region");

  RegionBits write_mask;
  write_mask.SetRange(10, 15);
  RegionBits read_mask;
  read_mask.Set(11);
  read_mask.Set(13);

  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true>(region_base, write_mask);
  Check(g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
            g_protection_ranges[0].address == region_base + page_size * 10 &&
            g_protection_ranges[0].size == page_size * 5,
        "contiguous write watch was not batched");

  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<true, true>(region_base, read_mask);
  Check(
      g_protection_calls == 2 &&
          Protection(reinterpret_cast<void *>(region_base + page_size * 11)) ==
              PAGE_NOACCESS &&
          Protection(reinterpret_cast<void *>(region_base + page_size * 12)) ==
              PAGE_READONLY &&
          Protection(reinterpret_cast<void *>(region_base + page_size * 13)) ==
              PAGE_NOACCESS,
      "read watchers did not compose with write-only watchers");

  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false>(region_base, write_mask);
  Check(
      g_protection_calls == 3 &&
          IsWritable(reinterpret_cast<void *>(region_base + page_size * 10)) &&
          Protection(reinterpret_cast<void *>(region_base + page_size * 11)) ==
              PAGE_NOACCESS &&
          IsWritable(reinterpret_cast<void *>(region_base + page_size * 12)) &&
          Protection(reinterpret_cast<void *>(region_base + page_size * 13)) ==
              PAGE_NOACCESS &&
          IsWritable(reinterpret_cast<void *>(region_base + page_size * 14)),
      "write unwatch changed pages still owned by read watchers");

  g_protection_calls = 0;
  g_protection_ranges.clear();
  manager.UpdatePageWatchersForRegion<false, true>(region_base, read_mask);
  Check(
      g_protection_calls == 1 && g_protection_ranges.size() == 1 &&
          g_protection_ranges[0].address == region_base + page_size * 11 &&
          g_protection_ranges[0].size == page_size * 3 &&
          IsWritable(reinterpret_cast<void *>(region_base + page_size * 11)) &&
          IsWritable(reinterpret_cast<void *>(region_base + page_size * 13)),
      "read unwatch did not coalesce through compatible writable state");

  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestConcurrentSharedWatchers() {
  constexpr size_t worker_count = 8;
  PageManager manager;
  auto *memory = Allocate(TRACKER_PAGE_SIZE);
  const auto address = reinterpret_cast<uint64_t>(memory);
  bool watched = false;
  std::barrier phase(worker_count, [&]() noexcept {
    watched = !watched;
    Check(Protection(memory) == (watched ? PAGE_READONLY : PAGE_READWRITE),
          "concurrent watchers lost protection or failed to restore writes");
  });
  std::vector<std::jthread> workers;
  for (size_t i = 0; i < worker_count; i++) {
    workers.emplace_back([&] {
      for (size_t round = 0; round < 100; round++) {
        manager.UpdatePageWatchers<true>(address, TRACKER_PAGE_SIZE);
        phase.arrive_and_wait();
        manager.UpdatePageWatchers<false>(address, TRACKER_PAGE_SIZE);
        phase.arrive_and_wait();
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

[[noreturn]] void RunDeathCase(const char *name) {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  if (std::strcmp(name, "invalid-range") == 0) {
    manager.UpdatePageWatchers<true>((1ull << 40u) - 1, 2);
  } else if (std::strcmp(name, "unknown-untrack") == 0) {
    manager.UpdatePageWatchers<false>(0x1000, page_size);
  } else if (std::strcmp(name, "destructor-watch") == 0) {
    auto doomed = std::make_unique<PageManager>();
    auto *memory = Allocate(page_size);
    const auto address = reinterpret_cast<uint64_t>(memory);
    doomed->UpdatePageWatchers<true>(address, page_size);
    doomed.reset();
  } else if (std::strcmp(name, "known-write-underflow") == 0) {
    auto *memory = Allocate(page_size);
    const auto address = reinterpret_cast<uint64_t>(memory);
    manager.UpdatePageWatchers<true>(address, page_size);
    manager.UpdatePageWatchers<false>(address, page_size);
    manager.UpdatePageWatchers<false>(address, page_size);
  } else if (std::strcmp(name, "read-overflow") == 0) {
    auto *memory = Allocate(page_size);
    const auto address = reinterpret_cast<uint64_t>(memory);
    RegionBits mask;
    const auto region_base = address & ~(TRACKER_REGION_SIZE - 1);
    const auto page = static_cast<size_t>((address - region_base) / page_size);
    mask.Set(page);
    manager.UpdatePageWatchersForRegion<true, true>(region_base, mask);
    manager.UpdatePageWatchersForRegion<true, true>(region_base, mask);
  } else if (std::strcmp(name, "write-overflow") == 0) {
    auto *memory = Allocate(page_size);
    const auto address = reinterpret_cast<uint64_t>(memory);
    for (uint32_t count = 0; count < 128; count++) {
      manager.UpdatePageWatchers<true>(address, page_size);
    }
  }
  std::_Exit(0x7f);
}

void CheckDeathCase(const char *name) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  char path[MAX_PATH]{};
  Check(GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
        "GetModuleFileName failed");
  std::string command = std::string("\"") + path + "\" --death " + name;
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  Check(CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) != 0,
        "CreateProcess failed");
  Check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
        "death test timed out");
  DWORD exit_code = 0;
  Check(
      GetExitCodeProcess(process.hProcess, &exit_code) != 0 &&
          (exit_code == 322 || exit_code == EXCEPTION_NONCONTINUABLE_EXCEPTION),
      "death case did not use the PageManager fatal exit");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
#if defined(__APPLE__)
  std::vector<char> path(PATH_MAX);
  uint32_t path_size = static_cast<uint32_t>(path.size());
  if (_NSGetExecutablePath(path.data(), &path_size) != 0) {
    path.resize(path_size);
    Check(_NSGetExecutablePath(path.data(), &path_size) == 0,
          "_NSGetExecutablePath failed");
  }
#endif
  const pid_t pid = ::fork();
  Check(pid >= 0, "fork failed");
  if (pid == 0) {
#if defined(__APPLE__)
    ::execl(path.data(), "PageManagerTests", "--death", name, nullptr);
#else
    ::execl("/proc/self/exe", "PageManagerTests", "--death", name, nullptr);
#endif
    std::_Exit(0x7e);
  }
  int status = 0;
  Check(::waitpid(pid, &status, 0) == pid, "waitpid failed");
  const bool fatal_exit =
      WIFEXITED(status) && WEXITSTATUS(status) == (322 & 0xff);
  const bool fatal_signal = WIFSIGNALED(status);
  Check(fatal_exit || fatal_signal,
        "death case did not use the PageManager fatal exit");
#endif
}

void TestFatalPaths() {
  for (const char *name :
       {"invalid-range", "unknown-untrack", "destructor-watch",
        "known-write-underflow", "read-overflow", "write-overflow"}) {
    CheckDeathCase(name);
  }
}

} // namespace

namespace Libs::LibKernel::Memory {

bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size,
                            Common::VirtualMemory::Mode mode,
                            bool access_changed) {
  return ProtectAddressSpace(vaddr, size, mode, access_changed);
}

} // namespace Libs::LibKernel::Memory

int main(int argc, char **argv) {
  if (argc == 3 && std::strcmp(argv[1], "--death") == 0) {
    RunDeathCase(argv[2]);
  }
  TestWatchAndUnwatch();
  TestWatchAndUnwatch(Libs::LibKernel::Memory::kExtendedMemoryBase);
  TestSharedWatcherCounts();
  TestCrossRegionRange();
  TestBatchedWatcherRanges();
  TestRegionMaskWatcherRanges();
  TestWriteWatchesKeepAccess();
  TestTrackingFaults();
  TestRegionEndpointBatching();
  TestReadWriteWatcherInteractions();
  TestConcurrentSharedWatchers();
  TestFatalPaths();
  std::puts("PageManagerTests: all cases passed");
  return 0;
}
