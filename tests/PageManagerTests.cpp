#include "common/virtualMemory.h"
#include "graphics/host_gpu/pageManager.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

using Libs::Graphics::PageManager;
using Libs::Graphics::PageWatchMode;

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

uint64_t g_protection_calls = 0;

bool ProtectAddressSpace(uint64_t vaddr, uint64_t size,
                         Common::VirtualMemory::Mode mode) {
  uint32_t protection = PAGE_NOACCESS;
  if (mode == Common::VirtualMemory::Mode::Read) {
    protection = PAGE_READONLY;
  } else if (mode == Common::VirtualMemory::Mode::ReadWrite) {
    protection = PAGE_READWRITE;
  }
  DWORD old_protection = 0;
  g_protection_calls++;
  return VirtualProtect(reinterpret_cast<void *>(vaddr), size, protection,
                        &old_protection) != 0;
}

uint8_t *Allocate(uint64_t size, uint32_t protection = PAGE_READWRITE) {
  constexpr uintptr_t test_address = 0x0000000200010000ull;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(test_address), size,
                   MEM_RESERVE | MEM_COMMIT, protection));
  Check(memory == reinterpret_cast<void *>(test_address),
        "fixed low VirtualAlloc failed");
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

void TestWatchAndUnwatch() {
  g_protection_calls = 0;
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size * 2);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size * 2);
  manager.UpdatePageWatchers(true, address, page_size);
  Check(Protection(memory) == PAGE_READONLY && IsWritable(memory + page_size),
        "write watch installed incorrect protections");
  Check(g_protection_calls != 0,
        "watch protection bypassed the address-space owner callback");
  manager.UpdatePageWatchers(false, address, page_size);
  Check(IsWritable(memory), "write unwatch did not restore access");
  manager.OnGpuUnmap(address, page_size * 2);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestSharedWatcherCounts() {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address + 8, 32);
  manager.UpdatePageWatchers(true, address + 128, 64);
  manager.UpdatePageWatchers(false, address + 8, 32);
  Check(Protection(memory) == PAGE_READONLY,
        "first unwatch released a shared watcher");
  manager.UpdatePageWatchers(false, address + 128, 64);
  Check(IsWritable(memory), "last unwatch did not restore access");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestMixedWatcherModes() {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size, PageWatchMode::Write);
  manager.UpdatePageWatchers(true, address, page_size,
                             PageWatchMode::ReadWrite);
  Check(Protection(memory) == PAGE_NOACCESS,
        "read/write watcher did not deny access");
  manager.UpdatePageWatchers(false, address, page_size, PageWatchMode::Write);
  Check(Protection(memory) == PAGE_NOACCESS,
        "write unwatch released a read/write watcher");
  manager.UpdatePageWatchers(false, address, page_size,
                             PageWatchMode::ReadWrite);
  Check(IsWritable(memory), "read/write unwatch did not restore access");
  manager.OnGpuUnmap(address, page_size);
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

  manager.OnGpuMap(base, region_size * 2);
  manager.UpdatePageWatchers(true, boundary - page_size, page_size * 2);
  Check(!IsWritable(reinterpret_cast<void *>(boundary - page_size)) &&
            !IsWritable(reinterpret_cast<void *>(boundary)),
        "cross-region watch did not protect both pages");
  manager.UpdatePageWatchers(false, boundary - page_size, page_size * 2);
  Check(IsWritable(reinterpret_cast<void *>(boundary - page_size)) &&
            IsWritable(reinterpret_cast<void *>(boundary)),
        "cross-region unwatch did not restore both pages");
  manager.OnGpuUnmap(base, region_size * 2);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestBatchedWatcherRanges() {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  constexpr uint64_t region_size = 4ull * 1024ull * 1024ull;
  constexpr uint64_t allocation_size = region_size * 3;
  auto *memory = Allocate(allocation_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, allocation_size);
  manager.UpdatePageWatchers(true, address + page_size, page_size);
  manager.UpdatePageWatchers(true, address + page_size * 3, page_size);
  manager.UpdatePageWatchers(true, address, page_size * 5);
  manager.UpdatePageWatchers(false, address, page_size * 5);
  Check(IsWritable(memory) && Protection(memory + page_size) == PAGE_READONLY &&
            IsWritable(memory + page_size * 2) &&
            Protection(memory + page_size * 3) == PAGE_READONLY &&
            IsWritable(memory + page_size * 4),
        "fragmented unwatch lost overlapping watcher counts");
  manager.UpdatePageWatchers(false, address + page_size, page_size);
  manager.UpdatePageWatchers(false, address + page_size * 3, page_size);

  g_protection_calls = 0;
  manager.UpdatePageWatchers(true, address, allocation_size);
  Check(g_protection_calls == 4 && Protection(memory) == PAGE_READONLY &&
            Protection(memory + region_size) == PAGE_READONLY &&
            Protection(memory + region_size * 2) == PAGE_READONLY &&
            Protection(memory + allocation_size - page_size) == PAGE_READONLY,
        "large watch was not batched and protected by tracking region");
  manager.UpdatePageWatchers(false, address, allocation_size);
  Check(IsWritable(memory) && IsWritable(memory + region_size) &&
            IsWritable(memory + region_size * 2) &&
            IsWritable(memory + allocation_size - page_size),
        "large cross-region unwatch did not restore the full range");

  manager.OnGpuUnmap(address, allocation_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

[[noreturn]] void RunDeathCase(const char *name) {
  PageManager manager;
  const auto page_size = manager.GetPageSize();
  if (std::strcmp(name, "invalid-range") == 0) {
    manager.UpdatePageWatchers(true, (1ull << 40u) - 1, 2);
  } else if (std::strcmp(name, "unknown-untrack") == 0) {
    manager.UpdatePageWatchers(false, 0x1000, page_size);
  } else if (std::strcmp(name, "destructor-watch") == 0) {
    auto doomed = std::make_unique<PageManager>();
    auto *memory = Allocate(page_size);
    const auto address = reinterpret_cast<uint64_t>(memory);
    doomed->OnGpuMap(address, page_size);
    doomed->UpdatePageWatchers(true, address, page_size);
    doomed.reset();
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
  const pid_t pid = ::fork();
  Check(pid >= 0, "fork failed");
  if (pid == 0) {
    ::execl("/proc/self/exe", "PageManagerTests", "--death", name, nullptr);
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
       {"invalid-range", "unknown-untrack", "destructor-watch"}) {
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
  TestWatchAndUnwatch();
  TestSharedWatcherCounts();
  TestMixedWatcherModes();
  TestCrossRegionRange();
  TestBatchedWatcherRanges();
  TestFatalPaths();
  std::puts("PageManagerTests: all cases passed");
  return 0;
}
