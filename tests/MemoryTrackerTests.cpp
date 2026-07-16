#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "common/assert.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#endif

namespace {

using Libs::Graphics::DirtySource;
using Libs::Graphics::CpuFaultAction;
using Libs::Graphics::MemoryTracker;
using Libs::Graphics::PageFaultAccess;
using Libs::Graphics::PageFaultPhase;
using Libs::Graphics::PageManager;
using Libs::Graphics::PageWatchMode;
using Libs::Graphics::RegionManager;
using Libs::Graphics::RangeSet;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "MemoryTrackerTests: failed: %s\n", text);
    std::abort();
  }
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool IsWritable(const void *address) {
  MEMORY_BASIC_INFORMATION info{};
  Check(VirtualQuery(address, &info, sizeof(info)) != 0, "VirtualQuery failed");
  return info.Protect == PAGE_READWRITE;
}

uint32_t Protection(const void *address) {
  MEMORY_BASIC_INFORMATION info{};
  Check(VirtualQuery(address, &info, sizeof(info)) != 0, "VirtualQuery failed");
  return info.Protect;
}

class SharedPage final {
 public:
  SharedPage(uintptr_t address, uint64_t size) {
    mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  static_cast<DWORD>(size >> 32u),
                                  static_cast<DWORD>(size), nullptr);
    Check(mapping_ != nullptr, "CreateFileMapping failed");
    guest = static_cast<uint8_t *>(MapViewOfFileEx(
        mapping_, FILE_MAP_ALL_ACCESS, 0, 0, size,
        reinterpret_cast<void *>(address)));
    Check(guest == reinterpret_cast<void *>(address),
          "fixed shared view failed");
    backing = static_cast<uint8_t *>(
        MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, size));
    Check(backing != nullptr, "shared backing view failed");
  }

  ~SharedPage() {
    Check(UnmapViewOfFile(backing) != 0, "shared backing unmap failed");
    Check(UnmapViewOfFile(guest) != 0, "shared guest unmap failed");
    Check(CloseHandle(mapping_) != 0, "shared mapping close failed");
  }

  SharedPage(const SharedPage &) = delete;
  SharedPage &operator=(const SharedPage &) = delete;

  uint8_t *guest = nullptr;
  uint8_t *backing = nullptr;

 private:
  HANDLE mapping_ = nullptr;
};

bool DummyFault(void *, PageFaultAccess, uint64_t, uint64_t, PageFaultPhase) noexcept {
  return true;
}

struct TrackerHarness {
  static bool Fault(void *context, PageFaultAccess access, uint64_t vaddr, uint64_t size,
                    PageFaultPhase phase) noexcept {
    auto *self = static_cast<TrackerHarness *>(context);
    if (self == nullptr || self->target == nullptr) {
      EXIT("memory-tracker test fault has no target\n");
    }
    return self->discard_virtual
               ? self->target->InvalidateVirtualGpuWrite(access, vaddr, size, phase)
               : self->target->InvalidateRegion(vaddr, size, phase);
  }

  explicit TrackerHarness(
      PageWatchMode gpu_watch_mode = PageWatchMode::ReadWrite)
      : page_manager(Fault, this), tracker(page_manager, gpu_watch_mode) {
    target = &tracker;
  }

  MemoryTracker *target = nullptr;
  bool discard_virtual = false;
  PageManager page_manager;
  MemoryTracker tracker;
};

struct SharedTrackerHarness {
  static bool Fault(void *context, PageFaultAccess, uint64_t vaddr, uint64_t size,
                    PageFaultPhase phase) noexcept {
    auto *self = static_cast<SharedTrackerHarness *>(context);
    if (self == nullptr) {
      EXIT("shared memory-tracker test fault has no harness\n");
    }
    const bool first = self->first.InvalidateRegion(vaddr, size, phase);
    const bool second = self->second.InvalidateRegion(vaddr, size, phase);
    return first || second;
  }

  SharedTrackerHarness()
      : page_manager(Fault, this), first(page_manager), second(page_manager) {}

  PageManager page_manager;
  MemoryTracker first;
  MemoryTracker second;
};

struct SharedMetadataImageHarness {
  static bool Fault(void *context, PageFaultAccess access, uint64_t vaddr,
                    uint64_t size, PageFaultPhase phase) noexcept {
    auto *self = static_cast<SharedMetadataImageHarness *>(context);
    if (self == nullptr ||
        (access != PageFaultAccess::Read && access != PageFaultAccess::Write)) {
      EXIT("shared metadata/image test received an invalid fault\n");
    }
    const bool metadata = access == PageFaultAccess::Write &&
                          self->metadata.InvalidateVirtualGpuWrite(
                              access, vaddr, size, phase);
    const bool image = self->image.InvalidateRegion(vaddr, size, phase);
    return metadata || image;
  }

  SharedMetadataImageHarness()
      : page_manager(Fault, this), image(page_manager),
        metadata(page_manager, PageWatchMode::Write) {}

  PageManager page_manager;
  MemoryTracker image;
  MemoryTracker metadata;
};

struct SplitTrackerHarness {
  static bool Fault(void *context, PageFaultAccess access, uint64_t vaddr,
                    uint64_t size, PageFaultPhase phase) noexcept {
    auto *self = static_cast<SplitTrackerHarness *>(context);
    if (self == nullptr) {
      EXIT("split memory-tracker test fault has no harness\n");
    }
    if (phase == PageFaultPhase::Release) {
      return true;
    }
    const bool buffer = self->buffer.InvalidateRegion(vaddr, size, phase);
    const bool metadata = self->metadata.InvalidateVirtualGpuWrite(
        access, vaddr, size, phase);
    const bool image = self->image.InvalidateRegion(vaddr, size, phase);
    if (static_cast<unsigned>(buffer) + static_cast<unsigned>(metadata) +
            static_cast<unsigned>(image) !=
        1) {
      EXIT("split memory-tracker fault matched multiple owners\n");
    }
    return true;
  }

  SplitTrackerHarness()
      : page_manager(Fault, this), buffer(page_manager), image(page_manager),
        metadata(page_manager, PageWatchMode::Write) {}

  PageManager page_manager;
  MemoryTracker buffer;
  MemoryTracker image;
  MemoryTracker metadata;
};

struct DownloadTrackerHarness {
  static bool Fault(void *context, PageFaultAccess access, uint64_t vaddr,
                    uint64_t size, PageFaultPhase phase) noexcept {
    auto *self = static_cast<DownloadTrackerHarness *>(context);
    if (self == nullptr) {
      EXIT("download memory-tracker test fault has no harness\n");
    }
    if (phase == PageFaultPhase::Invalidate) {
      if (self->pending_access != PageFaultAccess::Unknown) {
        EXIT("download memory-tracker test has an overlapping request\n");
      }
      const auto action = self->tracker.BeginCpuFault(vaddr, size);
      if (action == CpuFaultAction::Download) {
        self->pending_access = access;
      }
      return action != CpuFaultAction::Untracked;
    }
	if (phase == PageFaultPhase::Release) {
		return true;
	}
    const bool downloaded = self->pending_access != PageFaultAccess::Unknown;
    if (downloaded) {
      if (self->pending_access != access || self->download_data.empty()) {
        EXIT("download memory-tracker test has invalid completion state\n");
      }
      if (self->backing == nullptr ||
          self->download_address < self->guest_address) {
        EXIT("download memory-tracker test has no backing alias\n");
      }
      std::memcpy(self->backing + self->download_address - self->guest_address,
                  self->download_data.data(), self->download_data.size());
    }
    const bool completed =
        self->tracker.CompleteCpuFault(vaddr, size, access, downloaded);
    self->pending_access = PageFaultAccess::Unknown;
    return completed;
  }

  DownloadTrackerHarness() : page_manager(Fault, this), tracker(page_manager) {}

  PageFaultAccess pending_access = PageFaultAccess::Unknown;
  uint64_t download_address = 0;
  uint64_t guest_address = 0;
  uint8_t *backing = nullptr;
  std::vector<uint8_t> download_data;
  PageManager page_manager;
  MemoryTracker tracker;
};

std::atomic<PageManager *> g_native_page_manager{nullptr};
std::atomic_bool g_native_fault_entered{false};
std::atomic_bool g_unmap_contended{false};

void UnmapContended() noexcept {
  g_unmap_contended.store(true, std::memory_order_release);
}

LONG CALLBACK NativeTrackerFaultHandler(EXCEPTION_POINTERS *exception) {
  if (exception == nullptr || exception->ExceptionRecord == nullptr ||
      exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto operation = exception->ExceptionRecord->ExceptionInformation[0];
  const auto access = operation == 0   ? PageFaultAccess::Read
                      : operation == 1 ? PageFaultAccess::Write
                      : operation == 8 ? PageFaultAccess::Execute
                                       : PageFaultAccess::Unknown;
  auto *page_manager = g_native_page_manager.load(std::memory_order_acquire);
  if (page_manager == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  g_native_fault_entered.store(true, std::memory_order_release);
  return page_manager->HandleFault(
             access, exception->ExceptionRecord->ExceptionInformation[1])
             ? EXCEPTION_CONTINUE_EXECUTION
             : EXCEPTION_CONTINUE_SEARCH;
}

void TestPendingFaultBlocksUploadConsumption() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t region_size = 4ull * 1024ull * 1024ull;
  PageManager page_manager(DummyFault, nullptr);
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size);
  RegionManager region(page_manager, address & ~(region_size - 1));

  Libs::Graphics::RegionBits changed;
  {
    std::scoped_lock lock(region.lock);
    region.Track(address, page_size);
    changed = region.ForEachModifiedRange<DirtySource::Cpu, true>(
        address, page_size, [](uint64_t, uint64_t) noexcept {});
    changed = region.ChangeState<DirtySource::Cpu, true>(address, page_size);
    Check(region.BeginCpuFault(address, 1) == CpuFaultAction::Continue,
          "fault ownership rejected an already CPU-dirty page");
  }
  page_manager.UpdatePageWatchers(false, address, page_size);

  uint32_t ranges = 0;
  {
    std::scoped_lock lock(region.lock);
    const auto pending_change =
        region.ForEachModifiedRange<DirtySource::Cpu, true>(
            address, page_size, [&](uint64_t, uint64_t) noexcept { ranges++; });
    Check(ranges == 0 && pending_change.none() &&
              region.IsModified<DirtySource::Cpu>(address - region.GetCpuAddr(),
                                                  page_size),
          "pending fault page was consumed by upload");
    Check(region.CompleteCpuFault(address, 1, PageFaultAccess::Write, false),
          "fault completion was not recorded");
    changed = region.ForEachModifiedRange<DirtySource::Cpu, true>(
        address, page_size, [&](uint64_t, uint64_t) noexcept { ranges++; });
  }
  Check(ranges == 1, "completed fault page was not available to upload");
  {
    std::scoped_lock lock(region.lock);
    changed = region.ChangeState<DirtySource::Cpu, true>(address, page_size);
  }
  region.ApplyProtection(changed, false);
  page_manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestCleanReadFaultPreservesCpuState() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  PageManager page_manager(DummyFault, nullptr);
  MemoryTracker tracker(page_manager);
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(VirtualAlloc(
      reinterpret_cast<void *>(base), page_size, MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size);
  tracker.ForEachUploadRange(address, page_size, false,
                             [](uint64_t, uint64_t) noexcept {},
                             []() noexcept {});
  Check(!tracker.IsRegionCpuModified(address, page_size) &&
            !tracker.IsRegionGpuModified(address, page_size),
        "clean read-fault setup retained dirty ownership");
  Check(tracker.BeginCpuFault(address, 1, PageFaultAccess::Read) ==
            CpuFaultAction::Continue,
        "clean tracked read fault was not accepted");
  Check(tracker.CompleteCpuFault(address, 1, PageFaultAccess::Read, false),
        "clean tracked read fault did not complete");
  Check(!tracker.IsRegionCpuModified(address, page_size) &&
            !tracker.IsRegionGpuModified(address, page_size),
        "clean read fault incorrectly transferred write ownership to the CPU");
  tracker.UntrackMemory(address, page_size);
  page_manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestGpuDownloadFaultOwnership() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  DownloadTrackerHarness harness;
  const auto page_size = harness.page_manager.GetPageSize();
  SharedPage shared(base, page_size);
  auto *memory = shared.guest;
  const auto address = reinterpret_cast<uint64_t>(memory);
  harness.guest_address = address;
  harness.backing = shared.backing;
  harness.page_manager.OnGpuMap(address, page_size);
  harness.tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  harness.download_address = address + 32;
  harness.download_data = {0x11, 0x22, 0x33, 0x44};
  Check(harness.page_manager.HandleFault(PageFaultAccess::Read, address + 32),
        "GPU-dirty read fault was not handled");
  Check(std::memcmp(memory + 32, harness.download_data.data(), 4) == 0 &&
            !harness.tracker.IsRegionGpuModified(address, page_size) &&
            !harness.tracker.IsRegionCpuModified(address, page_size) &&
            Protection(memory) == PAGE_READONLY &&
            harness.page_manager.IsTracked(address),
        "GPU readback did not leave a clean write-watched page");

  Check(harness.page_manager.HandleFault(PageFaultAccess::Write, address + 32),
        "write watcher was not preserved after GPU readback");
  Check(harness.tracker.IsRegionCpuModified(address, page_size) && IsWritable(memory),
        "post-read CPU write did not claim CPU ownership");

  harness.tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  harness.download_data = {0xaa, 0xbb, 0xcc, 0xdd};
  Check(harness.page_manager.HandleFault(PageFaultAccess::Write, address + 33),
        "GPU-dirty write fault was not handled");
  Check(std::memcmp(memory + 32, harness.download_data.data(), 4) == 0 &&
            !harness.tracker.IsRegionGpuModified(address, page_size) &&
            harness.tracker.IsRegionCpuModified(address, page_size) && IsWritable(memory),
        "GPU write fault did not download before granting CPU ownership");
  harness.tracker.UnmapMemory(address, page_size);
}

void TestVirtualGpuWriteDiscard() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness(PageWatchMode::Write);
  harness.discard_virtual = true;
  const auto page_size = harness.page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  memory[8] = 0x5a;
  harness.page_manager.OnGpuMap(address, page_size);
  harness.tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(Protection(memory) == PAGE_READONLY && memory[8] == 0x5a &&
            harness.tracker.IsRegionGpuModified(address, page_size),
        "virtual GPU ownership did not preserve authoritative backing reads");
  Check(harness.page_manager.HandleFault(PageFaultAccess::Write, address + 8),
        "virtual GPU write fault was not discarded");
  Check(!harness.tracker.IsRegionGpuModified(address, page_size) &&
            harness.tracker.IsRegionCpuModified(address, page_size) && IsWritable(memory),
        "virtual GPU discard did not transfer the page to CPU ownership");
  harness.tracker.UnmapMemory(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestSameSlabTrackerArbitration() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  SplitTrackerHarness harness;
  const auto page_size = harness.page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(VirtualAlloc(
      reinterpret_cast<void *>(base), page_size * 3,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  harness.page_manager.OnGpuMap(address, page_size * 3);

  harness.buffer.ForEachUploadRange(
      address, page_size, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  harness.image.ForEachUploadRange(
      address + page_size, page_size, false,
      [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
  harness.metadata.ForEachUploadRange(
      address + page_size * 2, page_size, true,
      [](uint64_t, uint64_t) noexcept {}, []() noexcept {});

  Check(harness.buffer.BeginCpuFault(address + page_size * 2, 1) ==
            CpuFaultAction::Untracked &&
            harness.image.BeginCpuFault(address + page_size * 2, 1) ==
                CpuFaultAction::Untracked,
        "unrelated same-slab trackers claimed the metadata page");
  Check(harness.page_manager.HandleFault(PageFaultAccess::Write,
                                         address + page_size * 2 + 8),
        "metadata write fault was not exclusively handled");
  Check(harness.page_manager.HandleFault(PageFaultAccess::Write, address + 8),
        "buffer write fault was not exclusively handled");
  Check(harness.page_manager.HandleFault(PageFaultAccess::Write,
                                         address + page_size + 8),
        "image write fault was not exclusively handled");
  Check(harness.buffer.IsRegionCpuModified(address, page_size) &&
            harness.image.IsRegionCpuModified(address + page_size, page_size) &&
            harness.metadata.IsRegionCpuModified(address + page_size * 2,
                                                  page_size) &&
            IsWritable(memory) && IsWritable(memory + page_size) &&
            IsWritable(memory + page_size * 2),
        "exclusive same-slab faults did not transfer exact CPU ownership");

  harness.buffer.UntrackMemory(address, page_size);
  harness.image.UntrackMemory(address + page_size, page_size);
  harness.metadata.UntrackMemory(address + page_size * 2, page_size);
  harness.page_manager.OnGpuUnmap(address, page_size * 3);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestSharedMetadataAndImagePageFault() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  SharedMetadataImageHarness harness;
  const auto page_size = harness.page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(VirtualAlloc(
      reinterpret_cast<void *>(base), page_size, MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  harness.page_manager.OnGpuMap(address, page_size);
  harness.metadata.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  harness.image.ForEachUploadRange(
      address, page_size, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(Protection(memory) == PAGE_READONLY &&
            harness.metadata.IsRegionGpuModified(address, page_size) &&
            !harness.image.IsRegionCpuModified(address, page_size),
        "shared metadata/image page was not write-watched");
  Check(harness.page_manager.HandleFault(PageFaultAccess::Write, address + 8),
        "shared metadata/image write fault was not handled");
  Check(!harness.metadata.IsRegionGpuModified(address, page_size) &&
            harness.metadata.IsRegionCpuModified(address, page_size) &&
            harness.image.IsRegionCpuModified(address, page_size) &&
            IsWritable(memory),
        "shared write fault did not invalidate both native trackers");
  harness.metadata.UntrackMemory(address, page_size);
  harness.image.UntrackMemory(address, page_size);
  harness.page_manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestRangeSet() {
  RangeSet ranges;
  ranges.Add(0x1000, 0x80);
  ranges.Add(0x1080, 0x80);
  ranges.Add(0x1200, 0x40);
  auto intersections = ranges.Intersections(0x1070, 0x1b0);
  Check(intersections.size() == 2 && intersections[0].address == 0x1070 &&
            intersections[0].size == 0x90 && intersections[1].address == 0x1200 &&
            intersections[1].size == 0x20,
        "range set did not merge and intersect exact byte ranges");
  ranges.Subtract(0x1040, 0x1e0);
  intersections = ranges.Intersections(0x1000, 0x300);
  Check(intersections.size() == 2 && intersections[0].address == 0x1000 &&
            intersections[0].size == 0x40 && intersections[1].address == 0x1220 &&
            intersections[1].size == 0x20,
        "range set subtraction did not preserve both exact tails");
}

void TestCpuDirtyUploadAndFault() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size * 2,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size * 2);
  Check(tracker.IsRegionCpuModified(address + 16, 32),
        "new region was not CPU dirty");

  uint32_t ranges = 0;
  bool uploaded = false;
  tracker.ForEachUploadRange(
      address + 16, 32, false,
      [&](uint64_t upload_addr, uint64_t upload_size) noexcept {
        Check(upload_addr == address && upload_size == page_size,
              "upload range was not page aligned");
        ranges++;
      },
      [&]() noexcept { uploaded = true; });
  Check(ranges == 1 && uploaded &&
            !tracker.IsRegionCpuModified(address + 16, 32) &&
            !IsWritable(memory),
        "upload did not clear CPU dirty state and arm protection");

  Check(page_manager.HandleFault(PageFaultAccess::Write, address + 24),
        "tracked CPU write fault was not handled");
  Check(tracker.IsRegionCpuModified(address + 16, 32) && IsWritable(memory),
        "fault did not restore CPU dirty state and write access");
  tracker.ForEachUploadRange(
      address + 16, 32, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(!tracker.IsRegionCpuModified(address + 16, 32) && !IsWritable(memory),
        "fault owner state did not support a balanced rearm");
  tracker.MarkRegionAsCpuModified(address + 16, 32);
  Check(IsWritable(memory),
        "explicit CPU dirty transition did not release the rearmed watch");

  tracker.UnmapMemory(address, page_size * 2);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestFaultDuringUploadRemainsDirty() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size);
  tracker.ForEachUploadRange(
      address, page_size, false, [](uint64_t, uint64_t) noexcept {},
      [&]() noexcept {
        bool handled = false;
        std::thread fault([&] {
          handled = page_manager.HandleFault(PageFaultAccess::Write, address);
        });
        fault.join();
        Check(handled, "concurrent write racing upload was not handled");
      });
  Check(tracker.IsRegionCpuModified(address, page_size) && IsWritable(memory),
        "upload completion erased a racing CPU dirty transition");
  tracker.UnmapMemory(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestNativeStoreDuringRangeEnumeration() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size);

  void *handler = AddVectoredExceptionHandler(1, NativeTrackerFaultHandler);
  Check(handler != nullptr, "AddVectoredExceptionHandler failed");
  Check(g_native_page_manager.exchange(&page_manager, std::memory_order_acq_rel) ==
            nullptr,
        "native page manager already installed");
  g_native_fault_entered.store(false, std::memory_order_release);

  std::thread writer;
  tracker.ForEachUploadRange(
      address, page_size, false,
      [&](uint64_t, uint64_t) noexcept {
        writer = std::thread(
            [&] { *static_cast<volatile uint8_t *>(memory) = 0x6b; });
        while (!g_native_fault_entered.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      },
      [&]() noexcept { writer.join(); });

  Check(g_native_page_manager.exchange(nullptr, std::memory_order_acq_rel) ==
            &page_manager,
        "native page manager publication changed");
  Check(RemoveVectoredExceptionHandler(handler) != 0,
        "RemoveVectoredExceptionHandler failed");
  Check(memory[0] == 0x6b && tracker.IsRegionCpuModified(address, page_size) &&
            IsWritable(memory),
        "native store during range enumeration was lost");

  tracker.UnmapMemory(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestFaultDuringDownloadSynchronization() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  SharedPage shared(base, page_size * 3);
  auto *memory = shared.guest;
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size * 3);
  tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  tracker.ForEachUploadRange(
      address + page_size * 2, page_size, true,
      [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
  memory[page_size] = 0x31;

  void *handler = AddVectoredExceptionHandler(1, NativeTrackerFaultHandler);
  Check(handler != nullptr, "AddVectoredExceptionHandler failed");
  Check(g_native_page_manager.exchange(&page_manager, std::memory_order_acq_rel) ==
            nullptr,
        "native page manager already installed");
  g_native_fault_entered.store(false, std::memory_order_release);

  uint32_t ranges = 0;
  std::vector<uint8_t> download(page_size, 0x5a);
  std::thread writer;
  {
    PageManager::BackingWrite first(page_manager, address, page_size);
    PageManager::BackingWrite third(page_manager, address + page_size * 2,
                                    page_size);
    tracker.ForEachDownloadRange<true>(
        address, page_size * 3,
        [&](uint64_t download_address, uint64_t download_size) noexcept {
          Check((download_address == address ||
                 download_address == address + page_size * 2) &&
                    download_size == page_size,
                "download transaction reported the wrong range");
          ranges++;
          if (download_address == address) {
            writer = std::thread(
                [&] { *static_cast<volatile uint8_t *>(memory) = 0x7c; });
            while (!g_native_fault_entered.load(std::memory_order_acquire)) {
              std::this_thread::yield();
            }
          }
          std::memcpy(shared.backing + download_address - address,
                      download.data(), download_size);
          Check(Protection(reinterpret_cast<void *>(download_address)) ==
                    PAGE_NOACCESS,
                "backing alias exposed the protected guest page");
          Check(Protection(memory + page_size) == PAGE_READWRITE,
                "clean middle page was reserved or protected");
        });
    Check(ranges == 2 && Protection(memory) == PAGE_NOACCESS &&
              Protection(memory + page_size) == PAGE_READWRITE &&
              Protection(memory + page_size * 2) == PAGE_NOACCESS,
          "download completion released protection before reservation");
  }
  writer.join();

  Check(g_native_page_manager.exchange(nullptr, std::memory_order_acq_rel) ==
            &page_manager,
        "native page manager publication changed");
  Check(RemoveVectoredExceptionHandler(handler) != 0,
        "RemoveVectoredExceptionHandler failed");
  Check(ranges == 2, "partial download did not enumerate both dirty ranges");
  Check(memory[0] == 0x7c && memory[1] == 0x5a &&
            memory[page_size] == 0x31 && memory[page_size * 2] == 0x5a,
        "reserved backing write raced or lost downloaded data");
  Check(!tracker.IsRegionGpuModified(address, page_size * 3) &&
            tracker.IsRegionCpuModified(address, page_size * 3),
        "partial download left incorrect tracker ownership");
  Check(IsWritable(memory), "first dirty page did not restore write access");
  Check(IsWritable(memory + page_size), "clean page lost write access");
  Check(Protection(memory + page_size * 2) == PAGE_READONLY &&
            page_manager.IsTracked(address + page_size * 2),
        "uncontended dirty page did not retain its clean write watch");

  tracker.UnmapMemory(address, page_size * 3);
}

void TestFaultAndExplicitDirtyRace() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size);
  for (uint32_t iteration = 0; iteration < 64; iteration++) {
    tracker.ForEachUploadRange(
        address, page_size, false, [](uint64_t, uint64_t) noexcept {},
        []() noexcept {});
    std::atomic_bool start{false};
    bool handled = false;
    std::thread fault([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      handled = page_manager.HandleFault(PageFaultAccess::Write, address);
    });
    std::thread dirty([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      tracker.MarkRegionAsCpuModified(address, page_size);
    });
    start.store(true, std::memory_order_release);
    fault.join();
    dirty.join();
    Check(handled && tracker.IsRegionCpuModified(address, page_size) &&
              IsWritable(memory),
          "fault/explicit-dirty race lost dirty state or write access");
  }
  tracker.UnmapMemory(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestSharedTrackersAndConcurrentPageFaults() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  SharedTrackerHarness harness;
  const auto page_size = harness.page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size * 2,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  harness.page_manager.OnGpuMap(address, page_size * 2);

  for (auto *tracker : {&harness.first, &harness.second}) {
    tracker->ForEachUploadRange(
        address, page_size * 2, false, [](uint64_t, uint64_t) noexcept {},
        []() noexcept {});
  }
  Check(!IsWritable(memory) && !IsWritable(memory + page_size),
        "shared trackers did not arm both pages");

  std::atomic_bool start{false};
  bool first_handled = false;
  bool second_handled = false;
  std::thread first_fault([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    first_handled = harness.page_manager.HandleFault(PageFaultAccess::Write,
                                                     address + 8);
  });
  std::thread second_fault([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    second_handled = harness.page_manager.HandleFault(
        PageFaultAccess::Write, address + page_size + 8);
  });
  start.store(true, std::memory_order_release);
  first_fault.join();
  second_fault.join();

  Check(first_handled && second_handled,
        "concurrent faults were not handled by shared trackers");
  for (auto *tracker : {&harness.first, &harness.second}) {
    Check(tracker->IsRegionCpuModified(address, page_size * 2),
          "a shared tracker lost concurrent CPU dirtiness");
  }

  harness.first.UntrackMemory(address, page_size * 2);
  harness.second.UnmapMemory(address, page_size * 2);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestGpuDirtyBits() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), page_size * 2,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, page_size * 2);
  tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(tracker.IsRegionGpuModified(address, page_size) &&
            !tracker.IsRegionGpuModified(address + page_size, page_size) &&
            Protection(memory) == PAGE_NOACCESS,
        "GPU dirty state escaped the requested range");
  tracker.UnmarkRegionAsGpuModified(address, page_size);
  Check(!tracker.IsRegionGpuModified(address, page_size) &&
            Protection(memory) == PAGE_READONLY,
        "GPU dirty state did not restore write-only tracking");
  tracker.MarkRegionAsGpuModified(address, page_size);
  Check(tracker.IsRegionGpuModified(address, page_size) &&
            Protection(memory) == PAGE_NOACCESS,
        "explicit GPU dirty transition did not trap CPU access");
  tracker.UnmarkRegionAsGpuModified(address, page_size);
  tracker.MarkRegionAsCpuModified(address, page_size);
  tracker.UnmapMemory(address, page_size * 2);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestCrossRegionUpload() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t region_size = 4ull * 1024ull * 1024ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), region_size * 2,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  const auto boundary = (address + region_size - 1) & ~(region_size - 1);
  page_manager.OnGpuMap(address, region_size * 2);
  uint32_t ranges = 0;
  tracker.ForEachUploadRange(
      boundary - page_size, page_size * 2, false,
      [&](uint64_t, uint64_t) noexcept { ranges++; }, []() noexcept {});
  Check(ranges == 2 &&
            !tracker.IsRegionCpuModified(boundary - page_size, page_size * 2) &&
            !IsWritable(reinterpret_cast<void *>(boundary - page_size)) &&
            !IsWritable(reinterpret_cast<void *>(boundary)),
        "cross-region upload did not clear and protect both regions");
  tracker.MarkRegionAsCpuModified(boundary - page_size, page_size * 2);
  tracker.ForEachUploadRange(
      boundary - page_size, page_size * 2, true,
      [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
  Check(tracker.IsRegionGpuModified(boundary - page_size, page_size * 2),
        "cross-region written upload did not mark GPU dirty state");
  tracker.UnmarkRegionAsGpuModified(boundary - page_size, page_size * 2);
  tracker.MarkRegionAsCpuModified(boundary - page_size, page_size * 2);
  tracker.UnmapMemory(address, region_size * 2);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

[[noreturn]] void RunDeathCase(const char *name) {
  constexpr uintptr_t base = 0x0000000200010000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  if (std::strcmp(name, "unmapped") == 0) {
    (void)tracker.IsRegionCpuModified(base, page_size);
  }
  const auto allocation_size =
      std::strcmp(name, "missing-download-bytes") == 0 ? page_size * 2
                                                       : page_size;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), allocation_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, allocation_size);
  if (std::strcmp(name, "gpu-dirty-fault") == 0 ||
      std::strcmp(name, "gpu-dirty-read") == 0 ||
      std::strcmp(name, "gpu-dirty-explicit-cpu") == 0 ||
      std::strcmp(name, "virtual-gpu-read") == 0) {
    harness.discard_virtual = std::strcmp(name, "virtual-gpu-read") == 0;
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        []() noexcept {});
    if (std::strcmp(name, "gpu-dirty-explicit-cpu") == 0) {
      tracker.MarkRegionAsCpuModified(address, page_size);
    } else {
      (void)page_manager.HandleFault(
          (std::strcmp(name, "gpu-dirty-read") == 0 ||
           std::strcmp(name, "virtual-gpu-read") == 0)
              ? PageFaultAccess::Read
              : PageFaultAccess::Write,
          address);
    }
  } else if (std::strcmp(name, "reentrant-upload") == 0) {
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        [&]() noexcept {
          (void)tracker.IsRegionCpuModified(address, page_size);
        });
  } else if (std::strcmp(name, "writable-upload-race") == 0) {
    std::atomic_bool start{false};
    std::atomic_bool entered{false};
    std::thread fault([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      entered.store(true, std::memory_order_release);
      (void)page_manager.HandleFault(PageFaultAccess::Write, address);
    });
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        [&]() noexcept {
          start.store(true, std::memory_order_release);
          while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
    });
    fault.join();
  } else if (std::strcmp(name, "gpu-dirty-unmap-race") == 0) {
    g_unmap_contended.store(false, std::memory_order_release);
    MemoryTracker::SetUnmapContentionHook(UnmapContended);
    std::thread unmap;
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        [&]() noexcept {
          unmap = std::thread(
              [&] { tracker.UnmapMemory(address, page_size); });
          while (!g_unmap_contended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
        });
    unmap.join();
  } else if (std::strcmp(name, "missing-download-bytes") == 0) {
    tracker.ForEachUploadRange(
        address, allocation_size, true, [](uint64_t, uint64_t) noexcept {},
        []() noexcept {});
    RangeSet dirty_bytes;
    dirty_bytes.Add(address, 1);
    PageManager::BackingWrite backing(page_manager, address, page_size);
    tracker.ForEachDownloadRange<true>(
        address, allocation_size,
        [&](uint64_t dirty_address, uint64_t dirty_size) noexcept {
          for (auto page = dirty_address; page < dirty_address + dirty_size;
               page += page_size) {
            if (dirty_bytes.Intersections(page, page_size).empty()) {
              EXIT("GPU-dirty test page has no dirty byte record\n");
            }
          }
        },
        [](uint64_t, uint64_t) noexcept {});
  }
  std::_Exit(0x7f);
}

void TestFatalPaths() {
  char path[MAX_PATH]{};
  Check(GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
        "GetModuleFileName failed");
  for (const char *name : {"gpu-dirty-fault", "gpu-dirty-read", "virtual-gpu-read",
                           "gpu-dirty-explicit-cpu",
                           "unmapped", "reentrant-upload",
                           "writable-upload-race", "gpu-dirty-unmap-race",
                           "missing-download-bytes"}) {
    std::string command = std::string("\"") + path + "\" --death " + name;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Check(CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                         FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &process) != 0,
          "CreateProcess failed");
    Check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
          "MemoryTracker death test timed out");
    DWORD exit_code = 0;
    Check(GetExitCodeProcess(process.hProcess, &exit_code) != 0 &&
              (exit_code == 321 ||
               exit_code == EXCEPTION_NONCONTINUABLE_EXCEPTION),
          "MemoryTracker death path used the wrong exit");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}
#endif

} // namespace

int main(int argc, char **argv) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  if (argc == 3 && std::strcmp(argv[1], "--death") == 0) {
    RunDeathCase(argv[2]);
  }
  TestCpuDirtyUploadAndFault();
  TestPendingFaultBlocksUploadConsumption();
  TestCleanReadFaultPreservesCpuState();
  TestGpuDownloadFaultOwnership();
  TestVirtualGpuWriteDiscard();
  TestSameSlabTrackerArbitration();
  TestSharedMetadataAndImagePageFault();
  TestRangeSet();
  TestGpuDirtyBits();
  TestCrossRegionUpload();
  TestFaultDuringUploadRemainsDirty();
  TestNativeStoreDuringRangeEnumeration();
  TestFaultDuringDownloadSynchronization();
  TestFaultAndExplicitDirtyRace();
  TestSharedTrackersAndConcurrentPageFaults();
  TestFatalPaths();
  std::puts("MemoryTrackerTests: all cases passed");
  return 0;
#else
  (void)argc;
  (void)argv;
  std::fputs("MemoryTrackerTests: unsupported platform\n", stderr);
  return 1;
#endif
}
