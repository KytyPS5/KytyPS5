#include "graphics/host_gpu/pageManager.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

using Libs::Graphics::GpuAccess;
using Libs::Graphics::PageFaultAccess;
using Libs::Graphics::PageManager;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "PageManagerTests: failed: %s\n", text);
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

struct FaultContext {
  PageManager *manager = nullptr;
  bool result = true;
  bool reenter = false;
  uint64_t reenter_address = 0;
  bool block = false;
  std::atomic_uint32_t calls{0};
  std::atomic_bool entered{false};
  std::atomic_bool release{false};
};

std::atomic<PageManager *> g_native_fault_manager{nullptr};
std::atomic_bool g_delay_native_fault{false};
std::atomic_bool g_native_fault_entered{false};
std::atomic_bool g_release_native_fault{false};

LONG CALLBACK NativeFaultHandler(EXCEPTION_POINTERS *exception) {
  if (exception == nullptr || exception->ExceptionRecord == nullptr ||
      exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto operation = exception->ExceptionRecord->ExceptionInformation[0];
  const auto access = operation == 0   ? PageFaultAccess::Read
                      : operation == 1 ? PageFaultAccess::Write
                      : operation == 8 ? PageFaultAccess::Execute
                                       : PageFaultAccess::Unknown;
  auto *manager = g_native_fault_manager.load(std::memory_order_acquire);
  if (manager == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_delay_native_fault.load(std::memory_order_acquire)) {
    g_native_fault_entered.store(true, std::memory_order_release);
    while (!g_release_native_fault.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  return manager->HandleFault(
             access, exception->ExceptionRecord->ExceptionInformation[1])
             ? EXCEPTION_CONTINUE_EXECUTION
             : EXCEPTION_CONTINUE_SEARCH;
}

bool InvalidateFault(void *context, Libs::Graphics::PageFaultAccess, uint64_t vaddr, uint64_t size,
                     Libs::Graphics::PageFaultPhase phase) noexcept {
  auto *fault = static_cast<FaultContext *>(context);
  Check(fault != nullptr && fault->manager != nullptr, "invalid fault context");
  if (phase != Libs::Graphics::PageFaultPhase::Invalidate) {
    return true;
  }
  fault->calls.fetch_add(1, std::memory_order_relaxed);
  if (fault->reenter) {
    const auto address =
        fault->reenter_address != 0 ? fault->reenter_address : vaddr;
    (void)fault->manager->HandleFault(PageFaultAccess::Write, address);
  }
  if (fault->block) {
    fault->entered.store(true, std::memory_order_release);
    while (!fault->release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  (void)size;
  return fault->result;
}

uint8_t *Allocate(uint64_t size, uint32_t protection = PAGE_READWRITE) {
  constexpr uintptr_t test_address = 0x0000000200010000ull;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(test_address), size,
                   MEM_RESERVE | MEM_COMMIT, protection));
  Check(memory == reinterpret_cast<void *>(test_address),
        "fixed low VirtualAlloc failed");
  return memory;
}

void TestWatchFaultAndUnwatch() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size * 2);

  manager.OnGpuMap(reinterpret_cast<uint64_t>(memory), page_size * 2);
  manager.UpdatePageWatchers(true, reinterpret_cast<uint64_t>(memory),
                             page_size);
  Check(manager.IsTracked(reinterpret_cast<uint64_t>(memory)) &&
            !IsWritable(memory),
        "watch did not protect the page");
  Check(manager.HandleFault(PageFaultAccess::Write,
                            reinterpret_cast<uint64_t>(memory + 32)),
        "tracked write fault was not handled");
  Check(!manager.IsTracked(reinterpret_cast<uint64_t>(memory)) &&
            IsWritable(memory),
        "fault invalidation did not remove the watcher");
  Check(manager.HandleFault(PageFaultAccess::Write,
                            reinterpret_cast<uint64_t>(memory)),
        "single delayed write fault was not coalesced");
  Check(manager.HandleFault(PageFaultAccess::Write,
                            reinterpret_cast<uint64_t>(memory)),
        "second delayed write fault was not coalesced");
  manager.OnGpuUnmap(reinterpret_cast<uint64_t>(memory), page_size * 2);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestSharedWatcherFault() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address + 8, 32);
  manager.UpdatePageWatchers(true, address + 128, 64);
  Check(manager.HandleFault(PageFaultAccess::Write, address + 16),
        "shared-watcher fault was not handled");
  Check(!manager.IsTracked(address) && IsWritable(memory),
        "fault callback did not clear every shared watcher");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestReadWriteWatcherFault() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size,
                             Libs::Graphics::PageWatchMode::Write);
  manager.UpdatePageWatchers(true, address, page_size,
                             Libs::Graphics::PageWatchMode::ReadWrite);
  manager.UpdatePageWatchers(false, address, page_size,
                             Libs::Graphics::PageWatchMode::Write);
  Check(Protection(memory) == PAGE_NOACCESS,
        "read/write watcher did not install no-access protection");
  Check(manager.HandleFault(PageFaultAccess::Read, address + 8),
        "tracked read fault was not handled");
  Check(Protection(memory) == PAGE_READWRITE && !manager.IsTracked(address),
        "read fault did not release every watcher");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestPermittedMappedLateFaultsResume() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size);
  manager.UpdatePageWatchers(false, address, page_size);
  Check(manager.HandleFault(PageFaultAccess::Write, address),
        "first delayed mapped write was not accepted");
  Check(manager.HandleFault(PageFaultAccess::Write, address),
        "second delayed mapped write was not accepted");
  Check(manager.HandleFault(PageFaultAccess::Read, address),
        "delayed mapped read was not accepted on readable backing");
  DWORD old_protection = 0;
  Check(VirtualProtect(memory, page_size, PAGE_READONLY, &old_protection) != 0 &&
            old_protection == PAGE_READWRITE,
        "failed to prepare intentional read-only protection");
  Check(!manager.HandleFault(PageFaultAccess::Write, address),
        "intentional read-only mapping accepted a write fault");
  Check(VirtualProtect(memory, page_size, PAGE_READWRITE, &old_protection) != 0,
        "failed to restore writable protection");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestPartialMappingUnmapPreservesTokens() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size);
  manager.OnGpuMap(address + 8, 16);
  manager.UpdatePageWatchers(true, address, page_size,
                             Libs::Graphics::PageWatchMode::ReadWrite);
  manager.UpdatePageWatchers(false, address, page_size,
                             Libs::Graphics::PageWatchMode::ReadWrite);
  manager.OnGpuUnmap(address + 8, 16);
  Check(manager.HandleFault(PageFaultAccess::Read, address),
        "partial mapping unmap erased delayed read ownership");
  Check(manager.HandleFault(PageFaultAccess::Write, address),
        "partial mapping unmap erased delayed write ownership");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestNativeDelayedReadAfterModeDowngrade() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  memory[0] = 0x6d;
  const auto address = reinterpret_cast<uint64_t>(memory);

  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size);
  manager.UpdatePageWatchers(true, address, page_size,
                             Libs::Graphics::PageWatchMode::ReadWrite);
  manager.UpdatePageWatchers(false, address, page_size);
  Check(Protection(memory) == PAGE_NOACCESS,
        "read/write ownership did not install no-access protection");

  void *handler = AddVectoredExceptionHandler(1, NativeFaultHandler);
  Check(handler != nullptr, "AddVectoredExceptionHandler failed");
  Check(g_native_fault_manager.exchange(&manager, std::memory_order_acq_rel) ==
            nullptr,
        "native fault manager already installed");
  g_native_fault_entered.store(false, std::memory_order_release);
  g_release_native_fault.store(false, std::memory_order_release);
  g_delay_native_fault.store(true, std::memory_order_release);

  uint8_t value = 0;
  std::thread reader(
      [&] { value = *static_cast<volatile uint8_t *>(memory); });
  while (!g_native_fault_entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  manager.UpdatePageWatchers(true, address, page_size);
  manager.UpdatePageWatchers(false, address, page_size,
                             Libs::Graphics::PageWatchMode::ReadWrite);
  Check(Protection(memory) == PAGE_READONLY,
        "mode downgrade did not restore readable protection");
  manager.UpdatePageWatchers(true, address, page_size);

  context.block = true;
  bool write_handled = false;
  std::thread writer([&] {
    write_handled = manager.HandleFault(PageFaultAccess::Write, address);
  });
  while (!context.entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  g_release_native_fault.store(true, std::memory_order_release);
  reader.join();
  context.release.store(true, std::memory_order_release);
  writer.join();

  g_delay_native_fault.store(false, std::memory_order_release);
  Check(g_native_fault_manager.exchange(nullptr, std::memory_order_acq_rel) ==
            &manager,
        "native fault manager publication changed");
  Check(RemoveVectoredExceptionHandler(handler) != 0,
        "RemoveVectoredExceptionHandler failed");
  Check(value == 0x6d && write_handled &&
            context.calls.load(std::memory_order_relaxed) == 1,
        "delayed read was not coalesced across write ownership/resolution");

  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestDelayedFaultAfterExplicitUnwatch() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size);

  // Models a store that already raised an AV before another thread published
  // explicit CPU dirtiness and removed the watcher.
  manager.UpdatePageWatchers(false, address, page_size);
  Check(manager.HandleFault(PageFaultAccess::Write, address),
        "delayed watched write was not accepted after explicit unwatch");
  Check(manager.HandleFault(PageFaultAccess::Write, address) &&
            context.calls.load(std::memory_order_relaxed) == 0,
        "second explicit-unwatch fault was not coalesced or dispatched invalidation");

  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestNativeAccessViolation() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size);

  void *handler = AddVectoredExceptionHandler(1, NativeFaultHandler);
  Check(handler != nullptr, "AddVectoredExceptionHandler failed");
  Check(g_native_fault_manager.exchange(&manager, std::memory_order_acq_rel) ==
            nullptr,
        "native fault manager already installed");
  *static_cast<volatile uint8_t *>(memory) = 0x5a;
  Check(g_native_fault_manager.exchange(nullptr, std::memory_order_acq_rel) ==
            &manager,
        "native fault manager publication changed");
  Check(RemoveVectoredExceptionHandler(handler) != 0,
        "RemoveVectoredExceptionHandler failed");

  Check(memory[0] == 0x5a && !manager.IsTracked(address) &&
            context.calls.load(std::memory_order_relaxed) == 1,
        "native access violation did not invalidate and resume the store");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestInvalidLateWriteTokenIsConsumed() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size);
  Check(manager.HandleFault(PageFaultAccess::Write, address),
        "initial write fault was not handled");
  DWORD old_protection = 0;
  Check(VirtualProtect(memory, page_size, PAGE_READONLY, &old_protection) !=
                0 &&
            old_protection == PAGE_READWRITE,
        "failed to create invalid late-write protection state");
  Check(!manager.HandleFault(PageFaultAccess::Write, address) &&
            !manager.HandleFault(PageFaultAccess::Write, address),
        "invalid late-write token was accepted or retained");
  Check(VirtualProtect(memory, page_size, PAGE_READWRITE, &old_protection) != 0,
        "failed to restore test protection");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestCrossRegionRange() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
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

[[noreturn]] void RunDeathCase(const char *name) {
  FaultContext context;
  auto manager = std::make_unique<PageManager>(InvalidateFault, &context);
  context.manager = manager.get();
  const auto page_size = manager->GetPageSize();
  if (std::strcmp(name, "invalid-range") == 0) {
    manager->UpdatePageWatchers(true, (1ull << 40u) - 1, 2);
  } else if (std::strcmp(name, "unknown-untrack") == 0) {
    manager->UpdatePageWatchers(false, 0x1000, page_size);
  } else {
    const bool two_pages = std::strcmp(name, "cross-reentrant") == 0;
    auto *memory = Allocate(
        two_pages ? page_size * 2 : page_size,
        std::strcmp(name, "protection") == 0 ? PAGE_READONLY : PAGE_READWRITE);
    const auto address = reinterpret_cast<uint64_t>(memory);
    manager->OnGpuMap(address, two_pages ? page_size * 2 : page_size);
    manager->UpdatePageWatchers(true, address, page_size);
    if (two_pages) {
      manager->UpdatePageWatchers(true, address + page_size, page_size);
    }
    if (std::strcmp(name, "destructor-watch") == 0) {
      manager.reset();
    } else if (std::strcmp(name, "non-write") == 0) {
      (void)manager->HandleFault(PageFaultAccess::Read, address);
    } else if (std::strcmp(name, "callback-false") == 0) {
      context.result = false;
      (void)manager->HandleFault(PageFaultAccess::Write, address);
    } else if (std::strcmp(name, "reentrant") == 0) {
      context.reenter = true;
      (void)manager->HandleFault(PageFaultAccess::Write, address);
    } else if (std::strcmp(name, "cross-reentrant") == 0) {
      context.reenter = true;
      context.reenter_address = address + page_size;
      (void)manager->HandleFault(PageFaultAccess::Write, address);
    } else if (std::strcmp(name, "concurrent-non-write") == 0) {
      context.block = true;
      std::thread first(
          [&] { (void)manager->HandleFault(PageFaultAccess::Write, address); });
      while (!context.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      (void)manager->HandleFault(PageFaultAccess::Read, address);
      first.join();
    } else if (std::strcmp(name, "watched-unmap") == 0) {
      manager->OnGpuUnmap(address, page_size);
    } else if (std::strcmp(name, "protection") != 0) {
      std::_Exit(0x7f);
    }
  }
  std::_Exit(0x7f);
}

void CheckDeathCase(const char *name) {
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
}

void TestFatalPaths() {
  for (const char *name :
       {"invalid-range", "unknown-untrack", "destructor-watch", "non-write",
        "callback-false", "reentrant", "cross-reentrant",
        "concurrent-non-write", "watched-unmap", "protection"}) {
    CheckDeathCase(name);
  }
}

void TestConcurrentFault() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  context.block = true;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size);
  bool first_result = false;
  bool second_result = false;
  std::thread first([&] {
    first_result = manager.HandleFault(PageFaultAccess::Write, address);
  });
  while (!context.entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::thread second([&] {
    second_result = manager.HandleFault(PageFaultAccess::Write, address);
  });
  context.release.store(true, std::memory_order_release);
  first.join();
  second.join();
  Check(first_result && second_result &&
            context.calls.load(std::memory_order_relaxed) == 1,
        "concurrent faults dispatched invalidation more than once");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestExternalDirtyTransferDuringResolution() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  context.block = true;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuMap(address, page_size);
  manager.UpdatePageWatchers(true, address, page_size);
  bool handled = false;
  std::thread fault(
      [&] { handled = manager.HandleFault(PageFaultAccess::Write, address); });
  while (!context.entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  manager.UpdatePageWatchers(false, address, page_size);
  context.release.store(true, std::memory_order_release);
  fault.join();
  Check(handled && !manager.IsTracked(address) && IsWritable(memory),
        "external dirty transfer did not satisfy active resolution");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestMappingDoesNotRequireCpuWriteAccess() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  DWORD old_protection = 0;
  Check(VirtualProtect(memory, page_size, PAGE_NOACCESS, &old_protection) != 0 &&
            old_protection == PAGE_READWRITE,
        "failed to prepare CPU-inaccessible mapping");
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuMap(address, page_size);
  Check(manager.IsMapped(address, page_size),
        "CPU-inaccessible committed range was not GPU mapped");
  manager.OnGpuUnmap(address, page_size);
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void TestGpuAccessPermissions() {
  FaultContext context;
  PageManager manager(InvalidateFault, &context);
  context.manager = &manager;
  const auto page_size = manager.GetPageSize();
  auto *memory = Allocate(page_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  manager.OnGpuMap(address, page_size, GpuAccess::Read);
  Check(manager.HasGpuAccess(address, page_size, GpuAccess::Read) &&
            !manager.HasGpuAccess(address, page_size, GpuAccess::Write),
        "read-only GPU mapping granted write access");
  manager.OnGpuMap(address, page_size, GpuAccess::Write);
  Check(manager.HasGpuAccess(address, page_size, GpuAccess::ReadWrite),
        "overlapping GPU mappings did not combine permissions");
  manager.OnGpuUnmap(address, page_size, GpuAccess::Read);
  Check(!manager.HasGpuAccess(address, page_size, GpuAccess::Read) &&
            manager.HasGpuAccess(address, page_size, GpuAccess::Write),
        "GPU read unmap removed the wrong permission");
  manager.OnGpuUnmap(address, page_size, GpuAccess::Write);
  Check(!manager.IsMapped(address, page_size),
        "GPU permission mappings were not fully balanced");
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}
#endif

} // namespace

int main(int argc, char **argv) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  if (argc == 3 && std::strcmp(argv[1], "--death") == 0) {
    RunDeathCase(argv[2]);
  }
  TestWatchFaultAndUnwatch();
  TestSharedWatcherFault();
  TestReadWriteWatcherFault();
  TestPermittedMappedLateFaultsResume();
  TestPartialMappingUnmapPreservesTokens();
  TestNativeDelayedReadAfterModeDowngrade();
  TestDelayedFaultAfterExplicitUnwatch();
  TestNativeAccessViolation();
  TestInvalidLateWriteTokenIsConsumed();
  TestCrossRegionRange();
  TestConcurrentFault();
  TestExternalDirtyTransferDuringResolution();
  TestMappingDoesNotRequireCpuWriteAccess();
  TestGpuAccessPermissions();
  TestFatalPaths();
  std::puts("PageManagerTests: all cases passed");
  return 0;
#else
  (void)argc;
  (void)argv;
  std::fputs("PageManagerTests: unsupported platform\n", stderr);
  return 1;
#endif
}
