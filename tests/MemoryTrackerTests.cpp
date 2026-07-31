#include "common/virtualMemory.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#else
#include <map>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Libs::Graphics::MemoryTracker;
using Libs::Graphics::PageManager;
using Libs::Graphics::PageWatchMode;
using Libs::Graphics::RangeSet;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "MemoryTrackerTests: failed: %s\n", text);
    std::abort();
  }
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
using DWORD = uint32_t;
constexpr uint32_t PAGE_NOACCESS = 1;
constexpr uint32_t PAGE_READONLY = 2;
constexpr uint32_t PAGE_READWRITE = 3;
constexpr uint32_t MEM_RESERVE = 0;
constexpr uint32_t MEM_COMMIT = 0;
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
}

std::map<void *, size_t> &AllocationSizes() {
  static std::map<void *, size_t> sizes;
  return sizes;
}

void *VirtualAlloc(void *address, size_t size, DWORD, uint32_t protection) {
  const int extra = address != nullptr ? MAP_FIXED_NOREPLACE : 0;
  void *raw = ::mmap(address, size, ToHostProt(protection),
                     MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
  if (raw == MAP_FAILED) {
    return nullptr;
  }
  AllocationSizes()[raw] = size;
  return raw;
}

int VirtualFree(void *address, size_t, DWORD) {
  auto &sizes = AllocationSizes();
  auto it = sizes.find(address);
  if (it == sizes.end()) {
    return 0;
  }
  const int ok = ::munmap(address, it->second) == 0 ? 1 : 0;
  sizes.erase(it);
  return ok;
}

int VirtualProtect(void *address, size_t size, uint32_t protection,
                   DWORD *old_protection) {
  if (old_protection != nullptr) {
    *old_protection = Protection(address);
  }
  return ::mprotect(address, size, ToHostProt(protection)) == 0 ? 1 : 0;
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

bool ProtectAddressSpace(uint64_t vaddr, uint64_t size,
                         Common::VirtualMemory::Mode mode) {
  uint32_t protection = PAGE_NOACCESS;
  if (mode == Common::VirtualMemory::Mode::Read) {
    protection = PAGE_READONLY;
  } else if (mode == Common::VirtualMemory::Mode::ReadWrite) {
    protection = PAGE_READWRITE;
  }
  DWORD old_protection = 0;
  return VirtualProtect(reinterpret_cast<void *>(vaddr), size, protection,
                        &old_protection) != 0;
}

struct TrackerHarness {
  explicit TrackerHarness(
      PageWatchMode gpu_watch_mode = PageWatchMode::ReadWrite)
      : tracker(page_manager, gpu_watch_mode) {}

  PageManager page_manager;
  MemoryTracker tracker;
};

uint8_t *Allocate(PageManager &manager, uint64_t pages) {
  constexpr uintptr_t base = 0x0000000200010000ull;
  const auto size = manager.GetPageSize() * pages;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  manager.OnGpuMap(base, size);
  return memory;
}

void Release(PageManager &manager, uint8_t *memory, uint64_t size) {
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuUnmap(address, size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestRangeSet() {
  RangeSet ranges;
  ranges.Add(0x1000, 0x80);
  ranges.Add(0x1080, 0x80);
  ranges.Add(0x1200, 0x40);
  Check(ranges.Contains(0x1010, 0xe0) && !ranges.Contains(0x1010, 0x200),
        "range set containment did not require full coverage");
  auto intersections = ranges.Intersections(0x1070, 0x1b0);
  Check(intersections.size() == 2 && intersections[0].address == 0x1070 &&
            intersections[0].size == 0x90 &&
            intersections[1].address == 0x1200 && intersections[1].size == 0x20,
        "range set did not merge and intersect exact byte ranges");
  ranges.Subtract(0x1040, 0x1e0);
  intersections = ranges.Intersections(0x1000, 0x300);
  Check(intersections.size() == 2 && intersections[0].address == 0x1000 &&
            intersections[0].size == 0x40 &&
            intersections[1].address == 0x1220 && intersections[1].size == 0x20,
        "range set subtraction did not preserve both exact tails");
}

void TestQueriesDoNotRequireMappedOwnership() {
  constexpr uint64_t address = 0x0000000203000000ull;
  TrackerHarness harness;
  const auto page_size = harness.page_manager.GetPageSize();
  Check(harness.tracker.IsRegionCpuModified(address, page_size) &&
            !harness.tracker.IsRegionGpuModified(address, page_size),
        "unowned tracker range did not expose its initial CPU-dirty state");
}

void TestCpuDirtyUpload() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = Allocate(page_manager, 2);
  const auto address = reinterpret_cast<uint64_t>(memory);
  Check(tracker.IsRegionCpuModified(address + 16, 32),
        "new region was not CPU dirty");

  uint32_t ranges = 0;
  bool uploaded = false;
  tracker.ForEachUploadRange(
      address + 16, 32, false,
      [&](uint64_t upload_address, uint64_t upload_size) noexcept {
        Check(upload_address == address && upload_size == page_size,
              "upload range was not page aligned");
        ranges++;
      },
      [&]() noexcept { uploaded = true; });
  Check(ranges == 1 && uploaded &&
            !tracker.IsRegionCpuModified(address, page_size) &&
            Protection(memory) == PAGE_READONLY,
        "upload did not clear CPU dirty state and arm protection");

  tracker.MarkRegionAsCpuModified(address + 16, 32);
  Check(tracker.IsRegionCpuModified(address, page_size) && IsWritable(memory),
        "explicit CPU dirtiness did not release write protection");
  tracker.UntrackMemory(address, page_size * 2);
  Release(page_manager, memory, page_size * 2);
}

void TestRangeInvalidation() {
  constexpr uintptr_t base = 0x0000000201000000ull;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  constexpr uint64_t size = Libs::Graphics::TRACKER_REGION_SIZE * 2;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Check(memory == reinterpret_cast<void *>(base),
        "range invalidation allocation failed");
  const auto address = reinterpret_cast<uint64_t>(memory);
  page_manager.OnGpuMap(address, size);

  tracker.ForEachUploadRange(
      address, size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(tracker.IsRegionGpuModified(address, size) && !IsWritable(memory),
        "range invalidation setup did not establish GPU ownership");
  uint32_t flushes = 0;
  tracker.InvalidateRegion(address + 16, size - 32, [&] {
    flushes++;
    tracker.ForEachDownloadRange<true>(address + 16, size - 32,
                                       [](uint64_t, uint64_t) noexcept {});
  });
  Check(flushes == 1 && !tracker.IsRegionGpuModified(address, size) &&
            tracker.IsRegionCpuModified(address, size) && IsWritable(memory) &&
            IsWritable(memory + size - 1),
        "range invalidation did not batch ownership transfer across regions");
  tracker.InvalidateRegion(address + 16, size - 32, [&] { flushes++; });
  Check(flushes == 1,
        "clean range invalidation unnecessarily requested a GPU flush");
  tracker.UntrackMemory(address, size);
  Release(page_manager, memory, size);
}

void TestGpuDirtyBits() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = Allocate(page_manager, 2);
  const auto address = reinterpret_cast<uint64_t>(memory);

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
  tracker.MarkRegionAsCpuModified(address, page_size);
  tracker.UntrackMemory(address, page_size * 2);
  Release(page_manager, memory, page_size * 2);
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
  tracker.UntrackMemory(address, region_size * 2);
  Release(page_manager, memory, region_size * 2);
}

void TestBackingWritePublication() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = Allocate(page_manager, 1);
  const auto address = reinterpret_cast<uint64_t>(memory);

  tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(tracker.IsRegionGpuModified(address, page_size) &&
            Protection(memory) == PAGE_NOACCESS,
        "backing publication setup did not establish GPU ownership");
  std::vector<RangeSet::Range> dirty{{address, page_size}};
  auto writes = page_manager.ReserveBackingWrites(dirty);
  Check(writes.size() == 1 && Protection(memory) == PAGE_NOACCESS,
        "backing reservation exposed protected guest memory");
  uint32_t downloads = 0;
  tracker.ForEachDownloadRange<true>(
      address, page_size, [&](uint64_t, uint64_t) noexcept { downloads++; });
  tracker.MarkRegionAsCpuModified(address, page_size);
  writes.clear();
  Check(downloads == 1 && !tracker.IsRegionGpuModified(address, page_size) &&
            tracker.IsRegionCpuModified(address, page_size) &&
            IsWritable(memory),
        "backing publication did not restore CPU ownership");
  tracker.UntrackMemory(address, page_size);
  Release(page_manager, memory, page_size);
}

[[noreturn]] void RunDeathCase(const char *name) {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto &page_manager = harness.page_manager;
  const auto page_size = page_manager.GetPageSize();
  auto *memory = Allocate(page_manager, 1);
  const auto address = reinterpret_cast<uint64_t>(memory);
  if (std::strcmp(name, "gpu-dirty-explicit-cpu") == 0) {
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        []() noexcept {});
    tracker.MarkRegionAsCpuModified(address, page_size);
  } else if (std::strcmp(name, "reentrant-upload") == 0) {
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        [&]() noexcept {
          (void)tracker.IsRegionCpuModified(address, page_size);
        });
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
        "MemoryTracker death test timed out");
  DWORD exit_code = 0;
  Check(
      GetExitCodeProcess(process.hProcess, &exit_code) != 0 &&
          (exit_code == 321 || exit_code == EXCEPTION_NONCONTINUABLE_EXCEPTION),
      "MemoryTracker death path used the wrong exit");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
  const pid_t pid = ::fork();
  Check(pid >= 0, "fork failed");
  if (pid == 0) {
    ::execl("/proc/self/exe", "MemoryTrackerTests", "--death", name, nullptr);
    std::_Exit(0x7e);
  }
  int status = 0;
  Check(::waitpid(pid, &status, 0) == pid, "waitpid failed");
  const bool fatal_exit =
      WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff);
  Check(fatal_exit || WIFSIGNALED(status),
        "MemoryTracker death path used the wrong exit");
#endif
}

void TestFatalPaths() {
  for (const char *name : {"gpu-dirty-explicit-cpu", "reentrant-upload"}) {
    CheckDeathCase(name);
  }
}

} // namespace

namespace Libs::LibKernel::Memory {

bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size,
                            Common::VirtualMemory::Mode mode) {
  return ProtectAddressSpace(vaddr, size, mode);
}

} // namespace Libs::LibKernel::Memory

int main(int argc, char **argv) {
  if (argc == 3 && std::strcmp(argv[1], "--death") == 0) {
    RunDeathCase(argv[2]);
  }
  TestRangeSet();
  TestQueriesDoNotRequireMappedOwnership();
  TestCpuDirtyUpload();
  TestRangeInvalidation();
  TestGpuDirtyBits();
  TestCrossRegionUpload();
  TestBackingWritePublication();
  TestFatalPaths();
  std::puts("MemoryTrackerTests: all cases passed");
  return 0;
}
