#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "loader/redZonePatcher.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include "loader/x64InstructionEmulator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <csignal>
#include <immintrin.h>
#include <xbyak/xbyak.h>
#endif

#if defined(__linux__)
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

namespace Libs::Fiber {
struct FiberObject;
struct FiberOptParam;
using FiberEntry = KYTY_SYSV_ABI void (*)(uint64_t, uint64_t);
int32_t KYTY_SYSV_ABI FiberInitialize(FiberObject*, const char*, FiberEntry, uint64_t, void*,
                                     uint64_t, const FiberOptParam*, uint32_t);
int32_t KYTY_SYSV_ABI FiberFinalize(FiberObject*);
int32_t KYTY_SYSV_ABI FiberRun(FiberObject*, uint64_t, uint64_t*);
int32_t KYTY_SYSV_ABI FiberSwitch(FiberObject*, uint64_t, uint64_t*);
int32_t KYTY_SYSV_ABI FiberGetSelf(FiberObject**);
int32_t KYTY_SYSV_ABI FiberReturnToThread(uint64_t, uint64_t*);
} // namespace Libs::Fiber

namespace {

using Libs::LibKernel::Memory::VirtualQueryInfo;

// Prospero ABI?
constexpr uint64_t SceKernelPageSize             = 0x4000;
constexpr uint64_t SceKernelTotalPhysicalSize    = 13824ull * 1024ull * 1024ull;
constexpr uint64_t TestFlexibleMemorySize        = 3072ull * 1024ull * 1024ull;
constexpr int      SceKernelProtCpuRead          = 0x01;
constexpr int      SceKernelProtCpuRw            = 0x02;
constexpr int      SceKernelProtCpuExec          = 0x04;
constexpr int      SceKernelMapFixed             = 0x10;
constexpr int      SceKernelMapNoOverwrite       = 0x80;
constexpr int      SceKernelMapDmemCompat        = 0x400;
constexpr int      SceKernelMapNoCoalesce        = 0x400000;
constexpr int      SceKernelMapAligned64Kb       = 16 << 24;
constexpr int      SceKernelVqFindNext           = 1;
constexpr int      SceKernelMtypeC               = 11;
constexpr uint64_t SceKernelDirectMemoryStart    = 0;
constexpr uint64_t SceKernelMemoryPoolReserveLen = 0x200000;
constexpr uint64_t SceKernelMemoryPoolCommitLen  = 0x10000;
constexpr uint64_t SceKernelMemoryPoolExpandLen  = 0x400000;
constexpr uint64_t SceKernelMemoryPoolAlignment  = 0x10000;
constexpr int      ErrorAccess                   = Libs::LibKernel::KERNEL_ERROR_EACCES;

struct TestFailure {};

int g_failed_tests = 0;

[[noreturn]] void Fail(const char* test, const std::string& message) {
	std::fflush(stdout);
	std::fprintf(stderr, "VirtualMemoryAllocationTests: %s failed: %s\n", test, message.c_str());
	g_failed_tests++;
	throw TestFailure {};
}

void Check(const char* test, bool value, const std::string& message) {
	if (!value) {
		Fail(test, message);
	}
}

void CheckOk(const char* test, int result, const char* action) {
	if (result != OK) {
		char buffer[256] = {};
		std::snprintf(buffer, sizeof(buffer), "%s returned 0x%08" PRIx32, action,
		              static_cast<uint32_t>(result));
		Fail(test, buffer);
	}
}

void CheckFailed(const char* test, int result, const char* action) {
	if (result >= OK) {
		char buffer[256] = {};
		std::snprintf(buffer, sizeof(buffer), "%s returned success, expected negative error",
		              action);
		Fail(test, buffer);
	}
}

void InitSubsystems() {
	static bool initialized = false;
	if (initialized) {
		return;
	}

	static Common::Subsystems subsystems;
	Common::VirtualMemory::Init();
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();

	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);

	subsystems.Initialize<Log::Lifecycle>();

	const auto param_json = std::filesystem::temp_directory_path() /
	                        ("kyty_virtual_memory_" +
	                         std::to_string(reinterpret_cast<uintptr_t>(&initialized)) + ".json");
	constexpr char json[] = R"({"kernel":{"flexibleMemorySize":3221225472}})";
	Common::File   param_file;
	Check("InitSubsystems", param_file.Create(param_json), "failed to create temporary param.json");
	uint32_t bytes_written = 0;
	param_file.Write(json, sizeof(json) - 1, &bytes_written);
	param_file.Close();
	Check("InitSubsystems", bytes_written == sizeof(json) - 1,
	      "failed to write temporary param.json");

	Loader::SystemContentLoadParamSfo(param_json);
	const auto flexible_memory_size = Loader::SystemContentGetFlexibleMemorySize();
	Check("InitSubsystems", Common::File::DeleteFile(param_json),
	      "failed to remove temporary param.json");
	Check("InitSubsystems", flexible_memory_size == TestFlexibleMemorySize,
	      "failed to read flexible memory size from param.json");
	Libs::LibKernel::Memory::SetFlexibleMemorySize(flexible_memory_size);

	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();

	initialized = true;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
void* g_red_zone_fault_page = nullptr;

LONG CALLBACK RedZoneFaultHandler(EXCEPTION_POINTERS* exception) {
	if (exception == nullptr || exception->ExceptionRecord == nullptr ||
	    exception->ContextRecord == nullptr ||
	    exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
	    reinterpret_cast<void*>(exception->ExceptionRecord->ExceptionInformation[1]) !=
	        g_red_zone_fault_page) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Model the Windows exception stack footprint that prompted the static patch:
	// data below the interrupted RSP is not part of the Windows ABI contract.
	*reinterpret_cast<uint64_t*>(exception->ContextRecord->Rsp - 0x18) = 0;
	DWORD old_protection = 0;
	if (VirtualProtect(g_red_zone_fault_page, 0x1000, PAGE_READWRITE, &old_protection) == FALSE) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

void TestWindowsGuestRedZoneStaticPatcher() {
	const char* test = "WindowsGuestRedZoneStaticPatcher";
	constexpr uint64_t SENTINEL = 0x1122334455667788ull;
	constexpr uint64_t CODE_SIZE = 0x4000;
	constexpr uint64_t TRAMPOLINE_SIZE = 0x4000;
	const auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x0000000902000000ull, CODE_SIZE + TRAMPOLINE_SIZE,
	    Common::VirtualMemory::Mode::ExecuteReadWrite, "red_zone_patcher_test");
	Check(test, mapping != 0, "failed to allocate patch test code");

	std::vector<uint8_t> code;
	const auto emit = [&code](std::initializer_list<uint8_t> bytes) {
		code.insert(code.end(), bytes.begin(), bytes.end());
	};
	const auto emit64 = [&code](uint64_t value) {
		const auto offset = code.size();
		code.resize(offset + sizeof(value));
		std::memcpy(code.data() + offset, &value, sizeof(value));
	};
	emit({0x48, 0xb8});
	emit64(SENTINEL);                         // movabs rax, sentinel
	emit({0x48, 0x89, 0x44, 0x24, 0xe8});     // mov [rsp-0x18], rax
	emit({0x48, 0x8b, 0x07});                 // mov rax, [rdi] (faultable, 3 bytes)
	emit({0x48, 0x8b, 0x44, 0x24, 0xe8});     // mov rax, [rsp-0x18]
	emit({0x48, 0xb9});
	emit64(SENTINEL);                         // movabs rcx, sentinel
	emit({0x48, 0x39, 0xc8});                 // cmp rax, rcx
	emit({0x0f, 0x94, 0xc0});                 // sete al
	emit({0x0f, 0xb6, 0xc0, 0xc3});           // movzx eax, al; ret
	Check(test, code.size() < CODE_SIZE, "generated patch test code is too large");
	std::memcpy(reinterpret_cast<void*>(mapping), code.data(), code.size());
	Check(test, Common::VirtualMemory::FlushInstructionCache(mapping, code.size()),
	      "failed to flush generated test code");

	g_red_zone_fault_page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
	Check(test, g_red_zone_fault_page != nullptr, "failed to allocate fault page");
	auto* handler = AddVectoredExceptionHandler(1, RedZoneFaultHandler);
	Check(test, handler != nullptr, "failed to install test exception handler");

	using GuestFunction = uint64_t(KYTY_SYSV_ABI*)(const uint64_t*);
	const auto function = reinterpret_cast<GuestFunction>(mapping);
	const bool unpatched_was_corrupted = function(static_cast<const uint64_t*>(g_red_zone_fault_page)) == 0;

	DWORD old_protection = 0;
	Check(test, VirtualProtect(g_red_zone_fault_page, 0x1000, PAGE_NOACCESS, &old_protection) != FALSE,
	      "failed to reset fault page protection");
	Loader::RegisterRedZonePatchModule(reinterpret_cast<void*>(mapping), CODE_SIZE,
	                                   reinterpret_cast<void*>(mapping + CODE_SIZE),
	                                   TRAMPOLINE_SIZE);
	const std::array<uintptr_t, 1> function_starts = {static_cast<uintptr_t>(mapping)};
	const auto result = Loader::PatchGuestInstructions(
	    mapping, code.size(), function_starts, true, false);
	const bool patched_preserved = function(static_cast<const uint64_t*>(g_red_zone_fault_page)) == 1;

	Loader::UnregisterRedZonePatchModule(reinterpret_cast<void*>(mapping));
	RemoveVectoredExceptionHandler(handler);
	VirtualFree(g_red_zone_fault_page, 0, MEM_RELEASE);
	g_red_zone_fault_page = nullptr;
	const bool freed = Libs::LibKernel::Memory::FreeGuestMemory(mapping, CODE_SIZE + TRAMPOLINE_SIZE);

	Check(test, unpatched_was_corrupted, "test harness did not reproduce red-zone corruption");
	Check(test, result.red_zone_function_count == 1 && result.memory_instruction_count >= 1 &&
	                result.patched_memory_instruction_count >= 1 &&
	                result.unrelocatable_memory_instruction_count == 0,
	      "static patcher did not cover the faultable instruction");
	Check(test, patched_preserved, "patched fault still corrupted the guest red zone");
	Check(test, freed, "failed to free patch test code");
	std::printf("[host]    %-48s ok\n", test);
}
#else
void TestWindowsGuestRedZoneStaticPatcher() {
	std::printf("[host]    %-48s skipped\n", "WindowsGuestRedZoneStaticPatcher");
}
#endif

void RunTest(void (*test_func)()) {
	if (g_failed_tests != 0) {
		return;
	}
	try {
		test_func();
	} catch (const TestFailure&) {
	}
}

VirtualQueryInfo Query(const char* test, uint64_t addr, int flags = 0) {
	VirtualQueryInfo info {};
	const int ret = Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(addr),
	                                                            flags, &info, sizeof(info));
	CheckOk(test, ret, "KernelVirtualQuery");
	return info;
}

int QueryResult(uint64_t addr, int flags = 0) {
	VirtualQueryInfo info {};
	return Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(addr), flags,
	                                                   &info, sizeof(info));
}

size_t AvailableFlexibleMemory(const char* test) {
	size_t    size = 0;
	const int ret  = Libs::LibKernel::Memory::KernelAvailableFlexibleMemorySize(&size);
	CheckOk(test, ret, "KernelAvailableFlexibleMemorySize");
	return size;
}

size_t ConfiguredFlexibleMemory(const char* test) {
	size_t size = 0;
	CheckOk(test, Libs::LibKernel::Memory::KernelConfiguredFlexibleMemorySize(&size),
	        "KernelConfiguredFlexibleMemorySize");
	return size;
}

uint64_t MapNamedFlexible(const char* test, uint64_t size, int prot, const char* name) {
	void*     addr = nullptr;
	const int ret =
	    Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(&addr, size, prot, 0, name);
	CheckOk(test, ret, "KernelMapNamedFlexibleMemory");
	Check(test, addr != nullptr, "flexible mapping returned null");
	return reinterpret_cast<uint64_t>(addr);
}

void ExpectRange(const char* test, const VirtualQueryInfo& info, uint64_t start, uint64_t end,
                 int prot, uint32_t flexible, uint32_t direct, uint32_t pooled, uint32_t committed,
                 const char* name = nullptr, uint64_t offset = 0) {
	Check(test, info.start == start, "unexpected range start");
	Check(test, info.end == end, "unexpected range end");
	Check(test, info.protection == prot, "unexpected range protection");
	Check(test, info.is_flexible == flexible, "unexpected flexible flag");
	Check(test, info.is_direct == direct, "unexpected direct flag");
	Check(test, info.is_pooled == pooled, "unexpected pooled flag");
	Check(test, info.is_committed == committed, "unexpected committed flag");
	Check(test, info.offset == offset, "unexpected range offset");
	if (name != nullptr) {
		Check(test,
		      std::strncmp(info.name, name, Libs::LibKernel::Memory::KERNEL_MAXIMUM_NAME_LENGTH) ==
		          0,
		      "unexpected range name");
	}
}

void ExpectUnmapped(const char* test, uint64_t addr) {
	const int ret = QueryResult(addr);
	if (ret != ErrorAccess) {
		char buffer[256] = {};
		std::snprintf(buffer, sizeof(buffer), "KernelVirtualQuery(unmapped) returned 0x%08" PRIx32,
		              static_cast<uint32_t>(ret));
		Fail(test, buffer);
	}
}

void TestProsperoArgumentAndInfoSizeContracts() {
	const char* test = "ProsperoArgumentAndInfoSizeContracts";
	void*       addr = nullptr;

	Check(test, sizeof(VirtualQueryInfo) == 72, "SceKernelVirtualQueryInfo layout drifted");
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(&addr, 0, SceKernelProtCpuRw,
	                                                                  0, "zero_len"),
	            "KernelMapNamedFlexibleMemory(len=0)");
	CheckFailed(test, QueryResult(0), "KernelVirtualQuery(null)");

	VirtualQueryInfo info {};
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelVirtualQuery(nullptr, 0, &info, sizeof(info) - 1),
	            "KernelVirtualQuery(short info)");
	CheckFailed(test, Libs::LibKernel::Memory::KernelVirtualQuery(nullptr, 2, &info, sizeof(info)),
	            "KernelVirtualQuery(unknown flags)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestGuestAddressSpaceOwnsReservationsBeforeBacking() {
	const char* test = "GuestAddressSpaceOwnsReservationsBeforeBacking";
	void*       addr = nullptr;

	Check(test, Libs::LibKernel::Memory::TestGuestBackingOutsideAddressSpace(),
	      "boot-time shared backing alias overlaps an owned guest interval");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, SceKernelPageSize, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(addr);
	Check(test, Libs::LibKernel::Memory::TestGuestAddressRangeIsOwned(base, SceKernelPageSize),
	      "guest reservation was allocated outside the early owner");
	Check(test, Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(base, SceKernelPageSize),
	      "semantic reservation replaced the owner's placeholder");
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestHostMemory(base, SceKernelPageSize,
	                                                      Common::VirtualMemory::Mode::NoAccess),
	      "owner rejected a sparse placeholder protection no-op");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize), "KernelMunmap");
	Check(test, Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(base, SceKernelPageSize),
	      "released semantic reservation escaped owner control");

	std::printf("[host]    %-48s ok\n", test);
}

void TestPrtBackingReadPreservesSparseResidency() {
	const char*        test         = "PrtBackingReadPreservesSparseResidency";
	constexpr uint64_t commit_size  = SceKernelMemoryPoolCommitLen;
	constexpr uint64_t aperture_len = commit_size * 3;
	int64_t            pool_offset  = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolExpand(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), commit_size * 2,
	            SceKernelMemoryPoolAlignment, &pool_offset),
	        "KernelMemoryPoolExpand");

	void* arena = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolReserve(
	            reinterpret_cast<void*>(0x1000000000ull), SceKernelMemoryPoolReserveLen, 0, 0,
	            &arena),
	        "KernelMemoryPoolReserve");
	const auto base = reinterpret_cast<uint64_t>(arena);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(
	            arena, commit_size, SceKernelMtypeC, SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit(first)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(
	            reinterpret_cast<void*>(base + commit_size * 2), commit_size, SceKernelMtypeC,
	            SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit(third)");
	std::memset(reinterpret_cast<void*>(base), 0x3c, commit_size);
	std::memset(reinterpret_cast<void*>(base + commit_size * 2), 0xa7, commit_size);

	CheckOk(test, Libs::LibKernel::Memory::KernelSetPrtAperture(2, arena, aperture_len),
	        "KernelSetPrtAperture");
	std::vector<uint8_t> bytes(aperture_len, 0x5a);
	Check(test, !Libs::LibKernel::Memory::TryReadBacking(base, bytes.data(), bytes.size()),
	      "dense backing read accepted a nonresident span");
	Check(test, std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0x5a; }),
	      "failed dense backing read modified its destination");
	Check(test, Libs::LibKernel::Memory::TryReadPrtBacking(base, bytes.data(), bytes.size()),
	      "PRT backing read rejected a valid sparse aperture range");
	Check(test,
	      std::all_of(bytes.begin(), bytes.begin() + commit_size,
	                  [](uint8_t value) { return value == 0x3c; }) &&
	          std::all_of(bytes.begin() + commit_size, bytes.begin() + commit_size * 2,
	                      [](uint8_t value) { return value == 0; }) &&
	          std::all_of(bytes.begin() + commit_size * 2, bytes.end(),
	                      [](uint8_t value) { return value == 0xa7; }),
	      "PRT backing read did not copy resident pages and zero nonresident pages");
	Check(test,
	      !Libs::LibKernel::Memory::TryReadPrtBacking(base + commit_size * 2, bytes.data(),
	                                                  commit_size * 2),
	      "PRT backing read crossed the registered aperture");

	constexpr uint64_t unowned_prt = 0x5000000000ull;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelSetPrtAperture(
	            2, reinterpret_cast<void*>(unowned_prt), commit_size),
	        "KernelSetPrtAperture(unowned)");
	Check(test,
	      !Libs::LibKernel::Memory::TryReadPrtBacking(unowned_prt, bytes.data(), commit_size),
	      "PRT backing read accepted an unowned virtual range");
	CheckOk(test, Libs::LibKernel::Memory::KernelSetPrtAperture(2, nullptr, 0),
	        "KernelSetPrtAperture(clear)");

	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, commit_size, 0),
	        "KernelMemoryPoolDecommit(first)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolDecommit(
	            reinterpret_cast<void*>(base + commit_size * 2), commit_size, 0),
	        "KernelMemoryPoolDecommit(third)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelMemoryPoolReserveLen),
	        "KernelMunmap(pool reserve)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(pool_offset, commit_size * 2),
	        "KernelReleaseDirectMemory(pool expansion)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestPrtReadDuringDirectCommit() {
	const char*        test       = "PrtReadDuringDirectCommit";
	constexpr uint64_t chunk_size = SceKernelMemoryPoolCommitLen;
	constexpr uint64_t chunks     = 32;
	constexpr uint64_t size       = chunk_size * chunks;
	int64_t            physical   = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), size, chunk_size,
	            SceKernelMtypeC, &physical),
	        "KernelAllocateDirectMemory");
	void* arena = reinterpret_cast<void*>(0x1000000000ull);
	CheckOk(test, Libs::LibKernel::Memory::KernelReserveVirtualRange(&arena, size, 0, size),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(arena);
	CheckOk(test, Libs::LibKernel::Memory::KernelSetPrtAperture(2, arena, size),
	        "KernelSetPrtAperture");
	const auto map_chunk = [&](uint64_t index) {
		void* address = reinterpret_cast<void*>(base + index * chunk_size);
		return Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
		    &address, chunk_size, SceKernelProtCpuRw, SceKernelMapFixed,
		    physical + static_cast<int64_t>(index * chunk_size), chunk_size, "prt_direct");
	};
	CheckOk(test, map_chunk(0), "KernelMapNamedDirectMemory(first)");
	CheckOk(test, map_chunk(chunks - 1), "KernelMapNamedDirectMemory(last)");
	std::memset(reinterpret_cast<void*>(base), 0x3c, chunk_size);
	std::memset(reinterpret_cast<void*>(base + (chunks - 1) * chunk_size), 0xa7, chunk_size);

	std::vector<uint8_t> bytes(size, 0x5a);
	Check(test, Libs::LibKernel::Memory::TryReadPrtBacking(base, bytes.data(), size),
	      "PRT read rejected a partly committed direct mapping");
	Check(test, bytes.front() == 0x3c && bytes[chunk_size] == 0 && bytes.back() == 0xa7,
	      "PRT read lost resident bytes or sparse zeros");
	Libs::LibKernel::Memory::TestFailNextVirtualRangeReplacement();
	CheckFailed(test, map_chunk(1), "KernelMapNamedDirectMemory(injected replacement failure)");
	ExpectRange(test, Query(test, base + chunk_size), base + chunk_size,
	            base + (chunks - 1) * chunk_size, 0, 0, 0, 0, 0, "anon");
	Check(test, !Libs::LibKernel::Memory::TryReadBacking(base + chunk_size, bytes.data(),
	                                                    chunk_size) &&
	                Libs::LibKernel::Memory::TryReadPrtBacking(base, bytes.data(), size) &&
	                bytes[chunk_size] == 0,
	      "failed direct publication did not restore sparse reservation and backing");

	std::atomic<bool>     started {false};
	std::atomic<bool>     stop {false};
	std::atomic<bool>     read_failed {false};
	std::atomic<uint32_t> reads {0};
	std::thread reader([&] {
		std::vector<uint8_t> snapshot(size);
		started.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_acquire)) {
			if (!Libs::LibKernel::Memory::TryReadPrtBacking(base, snapshot.data(), size) ||
			    snapshot.front() != 0x3c || snapshot.back() != 0xa7) {
				read_failed.store(true, std::memory_order_relaxed);
			}
			reads.fetch_add(1, std::memory_order_relaxed);
		}
	});
	while (!started.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	int map_result = OK;
	for (uint64_t index = 1; index + 1 < chunks; ++index) {
		map_result = map_chunk(index);
		if (map_result != OK) {
			break;
		}
	}
	stop.store(true, std::memory_order_release);
	reader.join();

	CheckOk(test, Libs::LibKernel::Memory::KernelSetPrtAperture(2, nullptr, 0),
	        "KernelSetPrtAperture(clear)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, size), "KernelMunmap");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(physical, size),
	        "KernelReleaseDirectMemory");
	CheckOk(test, map_result, "KernelMapNamedDirectMemory(middle)");
	Check(test, reads.load(std::memory_order_relaxed) != 0 &&
	                !read_failed.load(std::memory_order_relaxed),
	      "PRT read observed a gap while direct backing was committed");
	std::printf("[host]    %-48s ok\n", test);
}

void TestGuestAddressSpaceHasNoFixedFallback() {
	const char* test            = "GuestAddressSpaceHasNoFixedFallback";
	const auto  unowned_address = reinterpret_cast<void*>(0x10000);
	void*       addr            = unowned_address;

	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelReserveVirtualRange(
	                &addr, SceKernelPageSize, SceKernelMapFixed | SceKernelMapNoOverwrite,
	                SceKernelPageSize),
	            "KernelReserveVirtualRange(unowned fixed address)");
	Check(test, reinterpret_cast<uint64_t>(addr) == 0x10000,
	      "failed fixed reservation unexpectedly moved");

	addr = unowned_address;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	                &addr, SceKernelPageSize, SceKernelProtCpuRw,
	                SceKernelMapFixed | SceKernelMapNoOverwrite, "unowned_flexible"),
	            "KernelMapNamedFlexibleMemory(unowned fixed address)");

	int64_t phys_addr = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize,
	            SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");
	addr = unowned_address;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	                &addr, SceKernelPageSize, SceKernelProtCpuRw,
	                SceKernelMapFixed | SceKernelMapNoOverwrite, phys_addr, SceKernelPageSize,
	                "unowned_direct"),
	            "KernelMapNamedDirectMemory(unowned fixed address)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelCheckedReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestGuestFreeRangeSearchDoesNotUnderflow() {
	const char* test = "GuestFreeRangeSearchDoesNotUnderflow";

	Check(test, Libs::LibKernel::Memory::TestGuestFreeRangeBounds(),
	      "free-range containment accepted a candidate beyond the range end");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFlexibleMemoryCapacityIsBootFixed() {
	const char* test       = "FlexibleMemoryCapacityIsBootFixed";
	const auto  configured = ConfiguredFlexibleMemory(test);
	const auto  baseline   = AvailableFlexibleMemory(test);
	const auto  backing    = Libs::LibKernel::Memory::TestGuestBackingSize();

	Check(test, configured == TestFlexibleMemorySize,
	      "boot flexible pool did not use the param.json value");
	Check(test, configured == baseline, "boot flexible pool did not start at configured capacity");
	Check(test, backing == SceKernelTotalPhysicalSize,
	      "boot backing is not the single 13.5 GiB physical file");
	Check(test, backing == Libs::LibKernel::Memory::KernelGetDirectMemorySize() + configured,
	      "direct and flexible regions do not partition the boot backing");

	const auto address =
	    MapNamedFlexible(test, SceKernelPageSize, SceKernelProtCpuRw, "boot_fixed_flexible");
	Check(test, ConfiguredFlexibleMemory(test) == configured,
	      "configured flexible capacity changed after allocation");
	Check(test, Libs::LibKernel::Memory::TestGuestBackingSize() == backing,
	      "shared backing size changed after allocation");
	Check(test, AvailableFlexibleMemory(test) == baseline - SceKernelPageSize,
	      "flexible allocation did not consume the boot-time pool");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(address, SceKernelPageSize),
	        "KernelMunmap");
	Check(test, ConfiguredFlexibleMemory(test) == configured,
	      "configured flexible capacity changed after release");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "flexible release did not restore the boot-time pool");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFlexibleMemoryUsesSharedBacking() {
	const char* test     = "FlexibleMemoryUsesSharedBacking";
	const auto  baseline = AvailableFlexibleMemory(test);
	void*       address  = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &address, SceKernelPageSize * 2, SceKernelProtCpuRw, 0, "shared_flexible"),
	        "KernelMapNamedFlexibleMemory");
	const auto base = reinterpret_cast<uint64_t>(address);
	Check(test, Libs::LibKernel::Memory::TestGuestAddressRangeIsOwned(base, SceKernelPageSize * 2),
	      "flexible mapping escaped the guest owner");

	constexpr uint64_t first_value     = 0x464c45584241434bull; // "FLEXBACK"
	constexpr uint64_t second_value    = 0x534841524544464cull; // "SHAREDFL"
	*reinterpret_cast<uint64_t*>(base) = first_value;
	uint64_t value                     = 0;
	Check(test, Libs::LibKernel::Memory::TryReadBacking(base, &value, sizeof(value)),
	      "TryReadBacking did not resolve flexible memory");
	Check(test, value == first_value, "backing did not observe a flexible-memory CPU write");
	Check(test,
	      Libs::LibKernel::Memory::TryWriteBacking(base + SceKernelPageSize, &second_value,
	                                               sizeof(second_value)),
	      "TryWriteBacking did not resolve flexible memory");
	Check(test, *reinterpret_cast<uint64_t*>(base + SceKernelPageSize) == second_value,
	      "flexible-memory view did not observe a backing write");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize * 2),
	        "KernelMunmap");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "flexible backing offsets were not returned to the boot-time pool");
	Check(test, !Libs::LibKernel::Memory::TryReadBacking(base, &value, sizeof(value)),
	      "unmapped flexible memory remained registered in the backing owner");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFlexibleDmemCompatAndAlignmentFlags() {
	const char* test     = "FlexibleDmemCompatAndAlignmentFlags";
	const auto  baseline = AvailableFlexibleMemory(test);
	void*       address  = nullptr;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &address, SceKernelPageSize, SceKernelProtCpuRw,
	            SceKernelMapDmemCompat | SceKernelMapAligned64Kb, "dmem_compat"),
	        "KernelMapNamedFlexibleMemory(DMEM_COMPAT|ALIGNED_64KB)");
	const auto base = reinterpret_cast<uint64_t>(address);
	Check(test, (base & (0x10000 - 1u)) == 0, "Requested alignment flag was not honored");
	const auto info = Query(test, base);
	Check(test, info.is_flexible == 1 && info.is_stack == 0,
	      "SCE_KERNEL_MAP_DMEM_COMPAT was misclassified as MAP_STACK");
	Check(test, AvailableFlexibleMemory(test) + SceKernelPageSize == baseline,
	      "DMEM_COMPAT mapping did not consume boot-time flexible backing");

	void* stack_start = reinterpret_cast<void*>(UINT64_MAX);
	void* stack_end   = reinterpret_cast<void*>(UINT64_MAX);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelIsStack(reinterpret_cast<void*>(base), &stack_start,
	                                               &stack_end),
	        "KernelIsStack");
	Check(test, stack_start == nullptr && stack_end == nullptr,
	      "DMEM_COMPAT flexible mapping was reported as a stack");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize), "KernelMunmap");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "DMEM_COMPAT cleanup did not restore flexible capacity");

	void* opaque = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &opaque, SceKernelPageSize, SceKernelProtCpuRw, 0x8000, "opaque_runtime_flag"),
	        "KernelMapNamedFlexibleMemory(opaque runtime flag)");
	Check(test, AvailableFlexibleMemory(test) + SceKernelPageSize == baseline,
	      "opaque runtime flag mapping did not consume boot-time flexible backing");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(opaque),
	                                              SceKernelPageSize),
	        "KernelMunmap(opaque runtime flag)");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "opaque runtime flag cleanup did not restore flexible capacity");

	void* invalid_flag = nullptr;
	CheckFailed(
	    test,
	    Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	        &invalid_flag, SceKernelPageSize, SceKernelProtCpuRw, 0x10000, "unsupported_flag"),
	    "KernelMapNamedFlexibleMemory(unsupported flag)");

	void* invalid_alignment = nullptr;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	                &invalid_alignment, SceKernelPageSize, SceKernelProtCpuRw, 13 << 24,
	                "invalid_alignment"),
	            "KernelMapNamedFlexibleMemory(invalid alignment)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFlexibleNoCoalescePreservesBoundaries() {
	const char* test     = "FlexibleNoCoalescePreservesBoundaries";
	const auto  baseline = AvailableFlexibleMemory(test);
	void*       reserve  = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&reserve, SceKernelPageSize * 2, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(reserve);

	void* left = reinterpret_cast<void*>(base);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &left, SceKernelPageSize, SceKernelProtCpuRw,
	            SceKernelMapFixed | SceKernelMapNoCoalesce, "no_coalesce"),
	        "KernelMapNamedFlexibleMemory(left)");
	void* right = reinterpret_cast<void*>(base + SceKernelPageSize);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &right, SceKernelPageSize, SceKernelProtCpuRw,
	            SceKernelMapFixed | SceKernelMapNoCoalesce, "no_coalesce"),
	        "KernelMapNamedFlexibleMemory(right)");

	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, SceKernelProtCpuRw, 1, 0,
	            0, 1, "no_coalesce");
	ExpectRange(test, Query(test, base + SceKernelPageSize), base + SceKernelPageSize,
	            base + SceKernelPageSize * 2, SceKernelProtCpuRw, 1, 0, 0, 1, "no_coalesce");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize * 2),
	        "KernelMunmap");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "NO_COALESCE cleanup did not restore flexible capacity");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFlexibleMemoryReuseIsZeroFilled() {
	const char* test     = "FlexibleMemoryReuseIsZeroFilled";
	const auto  baseline = AvailableFlexibleMemory(test);
	const auto  first =
	    MapNamedFlexible(test, SceKernelPageSize, SceKernelProtCpuRw, "flexible_zero_source");
	std::memset(reinterpret_cast<void*>(first), 0xa5, SceKernelPageSize);
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(first, SceKernelPageSize),
	        "KernelMunmap(source)");

	const auto reused =
	    MapNamedFlexible(test, SceKernelPageSize, SceKernelProtCpuRw, "flexible_zero_reuse");
	const auto* bytes = reinterpret_cast<const uint8_t*>(reused);
	Check(test,
	      std::all_of(bytes, bytes + SceKernelPageSize, [](uint8_t value) { return value == 0; }),
	      "reused flexible backing exposed stale bytes");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(reused, SceKernelPageSize),
	        "KernelMunmap(reuse)");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "zero-fill test leaked flexible backing capacity");

	std::printf("[host]    %-48s ok\n", test);
}

void TestSmallerFlexibleMapReusesReleasedHole() {
	const char*        test       = "SmallerFlexibleMapReusesReleasedHole";
	const auto         baseline   = AvailableFlexibleMemory(test);
	constexpr uint64_t SmallSize  = SceKernelPageSize * 12;
	constexpr uint64_t LargeSize  = SceKernelPageSize * 16;
	constexpr uint64_t BlockSize  = SceKernelPageSize * 4;
	constexpr int      RuntimeMap = 0x8000;

	// Leave a 0x30000 hole before an allocated backing block. The large mapping must then be
	// assembled from two flexible-backing extents, matching the allocation that exposed the
	// PPSA30528 regression.
	void* seed_hole = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &seed_hole, SmallSize, SceKernelProtCpuRw, RuntimeMap, "backing_seed_hole"),
	        "KernelMapNamedFlexibleMemory(seed hole)");
	void* seed_blocker = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &seed_blocker, BlockSize, SceKernelProtCpuRw, RuntimeMap, "backing_seed_blocker"),
	        "KernelMapNamedFlexibleMemory(seed blocker)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(seed_hole), SmallSize),
	        "KernelMunmap(seed hole)");
	void* virtual_blocker = seed_hole;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&virtual_blocker, SmallSize, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange(seed virtual blocker)");
	Check(test, virtual_blocker == seed_hole, "seed virtual hole was not reserved in place");

	void* large = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &large, LargeSize, SceKernelProtCpuRw, RuntimeMap, "released_large_hole"),
	        "KernelMapNamedFlexibleMemory(large)");
	void* blocker = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &blocker, SmallSize, SceKernelProtCpuRw, RuntimeMap, "adjacent_blocker"),
	        "KernelMapNamedFlexibleMemory(blocker)");
	const auto large_base   = reinterpret_cast<uint64_t>(large);
	const auto blocker_base = reinterpret_cast<uint64_t>(blocker);
	Check(test, blocker_base == large_base + LargeSize,
	      "hint-less flexible maps were not adjacent");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(large_base, LargeSize),
	        "KernelMunmap(large)");

	void* reused = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &reused, SmallSize, SceKernelProtCpuRw, RuntimeMap, "smaller_hole_reuse"),
	        "KernelMapNamedFlexibleMemory(smaller reuse)");
	Check(test, reinterpret_cast<uint64_t>(reused) == large_base,
	      "smaller flexible map did not reuse the released hole");
	*reinterpret_cast<uint64_t*>(reused) = 0x4b59545952455553ull; // "KYTYREUS"
	Check(test, *reinterpret_cast<const uint64_t*>(reused) == 0x4b59545952455553ull,
	      "reused flexible hole is not writable");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(reused), SmallSize),
	        "KernelMunmap(reused)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(blocker_base, SmallSize),
	        "KernelMunmap(blocker)");
	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(seed_blocker), BlockSize),
	    "KernelMunmap(seed blocker)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(virtual_blocker),
	                                              SmallSize),
	        "KernelMunmap(seed virtual blocker)");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "released-hole reuse leaked flexible memory capacity");

	std::printf("[host]    %-48s ok\n", test);
}

void TestGuestStackUsesPrivateOwnerMemoryAndCache() {
	const char* test     = "GuestStackUsesPrivateOwnerMemoryAndCache";
	const auto  baseline = AvailableFlexibleMemory(test);
	uint64_t    first    = 0;
	uint64_t    second   = 0;
	uint64_t    map_size = 0;

	Check(test, Libs::LibKernel::TestGuestStackOwnerLifecycle(&first, &second, &map_size),
	      "guest stack owner lifecycle failed");
	Check(test, first != 0 && first == second, "guest stack cache did not reuse its owner mapping");
	Check(test, map_size != 0 && (map_size & (SceKernelPageSize - 1u)) == 0,
	      "guest stack mapping is not 16 KiB aligned");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "private guest stack changed flexible backing capacity");

	std::printf("[host]    %-48s ok\n", test);
}

void TestMainEntryUsesGuestStackAndDisablesHostChecks() {
	const char* test = "MainEntryUsesGuestStackAndDisablesHostChecks";

	Check(test, Loader::TestMainEntryUsesGuestStack(),
	      "main-entry stack switch did not preserve the guest/host stack invariants");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFragmentedBackingUnmapRollback() {
	const char* test     = "FragmentedBackingUnmapRollback";
	const auto  baseline = AvailableFlexibleMemory(test);
	const auto  left =
	    MapNamedFlexible(test, SceKernelPageSize, SceKernelProtCpuRw, "backing_hole_left");
	const auto blocker =
	    MapNamedFlexible(test, SceKernelPageSize, SceKernelProtCpuRw, "backing_blocker");
	const auto right =
	    MapNamedFlexible(test, SceKernelPageSize, SceKernelProtCpuRw, "backing_hole_right");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(left, SceKernelPageSize),
	        "KernelMunmap(left hole)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(right, SceKernelPageSize),
	        "KernelMunmap(right hole)");

	const auto fragmented =
	    MapNamedFlexible(test, SceKernelPageSize * 2, SceKernelProtCpuRw, "fragmented_backing");
	auto* first_word = reinterpret_cast<uint64_t*>(fragmented);
	auto* last_word =
	    reinterpret_cast<uint64_t*>(fragmented + SceKernelPageSize * 2 - sizeof(uint64_t));
	*first_word = 0x465241474c454654ull; // "FRAGLEFT"
	*last_word  = 0x4652414752474854ull; // "FRAGRGHT"

	Libs::LibKernel::Memory::TestFailGuestBackingStoreUnmapAfter(1);
	CheckFailed(test, Libs::LibKernel::Memory::KernelMunmap(fragmented, SceKernelPageSize * 2),
	            "KernelMunmap(injected second-view failure)");
	ExpectRange(test, Query(test, fragmented), fragmented, fragmented + SceKernelPageSize * 2,
	            SceKernelProtCpuRw, 1, 0, 0, 1, "fragmented_backing");
	Check(test, *first_word == 0x465241474c454654ull && *last_word == 0x4652414752474854ull,
	      "transactional backing-unmap rollback lost mapped contents");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(fragmented, SceKernelPageSize * 2),
	        "KernelMunmap(retry)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(blocker, SceKernelPageSize),
	        "KernelMunmap(blocker)");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "fragmented backing rollback test leaked flexible capacity");

	std::printf("[host]    %-48s ok\n", test);
}

void TestRuntimeMemoryOwnerLifecycle() {
	const char* test = "RuntimeMemoryOwnerLifecycle";
	Check(test,
	      Libs::LibKernel::Memory::AllocateRuntimeMemory(0x10000, SceKernelPageSize,
	                                                     Common::VirtualMemory::Mode::ReadWrite,
	                                                     "runtime_outside_owner", true) == 0,
	      "fixed runtime allocation escaped the guest owner");

	const auto base = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, SceKernelPageSize * 2, Common::VirtualMemory::Mode::ReadWrite, "runtime_lifecycle");
	Check(test, base != 0, "runtime allocation failed");
	Check(test, Libs::LibKernel::Memory::TestGuestAddressRangeIsOwned(base, SceKernelPageSize * 2),
	      "runtime allocation is outside the owner");
	*reinterpret_cast<uint64_t*>(base) = 0x52554e54494d454full; // "RUNTIMEO"
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestMemory(base, SceKernelPageSize,
	                                                  Common::VirtualMemory::Mode::Read),
	      "runtime protection failed");
	Check(test, Libs::LibKernel::Memory::FreeGuestMemory(base, SceKernelPageSize * 2),
	      "runtime free failed");
	Check(test, Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(base, SceKernelPageSize * 2),
	      "runtime free did not restore the owner placeholder");

	const auto reused = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    base, SceKernelPageSize * 2, Common::VirtualMemory::Mode::ReadWrite, "runtime_reuse", true);
	Check(test, reused == base, "fixed runtime allocation did not reuse the owner placeholder");
	Check(test, Libs::LibKernel::Memory::FreeGuestMemory(reused, SceKernelPageSize * 2),
	      "reused runtime free failed");

	const auto adjacent_first = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, SceKernelPageSize, Common::VirtualMemory::Mode::ReadWrite, "runtime_adjacent_first");
	Check(test, adjacent_first != 0, "first adjacent runtime allocation failed");
	const auto adjacent_second = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    adjacent_first + SceKernelPageSize, SceKernelPageSize,
	    Common::VirtualMemory::Mode::ReadWrite, "runtime_adjacent_second", true);
	Check(test, adjacent_second == adjacent_first + SceKernelPageSize,
	      "second adjacent runtime allocation failed");
	Check(test, Libs::LibKernel::Memory::FreeGuestMemory(adjacent_first, SceKernelPageSize * 2),
	      "combined adjacent runtime free failed");
	Check(
	    test,
	    Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(adjacent_first, SceKernelPageSize * 2),
	    "combined adjacent runtime free did not restore one owner placeholder");
	const auto adjacent_reused = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    adjacent_first, SceKernelPageSize, Common::VirtualMemory::Mode::ReadWrite,
	    "runtime_adjacent_reuse", true);
	Check(test, adjacent_reused == adjacent_first,
	      "combined adjacent runtime placeholder could not be split for reuse");
	Check(test, Libs::LibKernel::Memory::FreeGuestMemory(adjacent_reused, SceKernelPageSize),
	      "adjacent runtime reuse cleanup failed");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFlexibleMapQueryAndWholeMunmap() {
	const char* test     = "FlexibleMapQueryAndWholeMunmap";
	const auto  baseline = AvailableFlexibleMemory(test);
	const auto  size     = SceKernelPageSize * 2;
	const auto  base     = MapNamedFlexible(test, size, SceKernelProtCpuRw, "prospero_flex");

	ExpectRange(test, Query(test, base), base, base + size, SceKernelProtCpuRw, 1, 0, 0, 1,
	            "prospero_flex");
	Check(test, AvailableFlexibleMemory(test) + size == baseline,
	      "flexible allocation should consume Prospero-reported flexible budget");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, size), "KernelMunmap");
	ExpectUnmapped(test, base);
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "whole munmap should return flexible memory to Prospero-reported budget");

	std::printf("[host]    %-48s ok\n", test);
}

void TestPartialFlexibleMunmapAndFindNext() {
	const char* test     = "PartialFlexibleMunmapAndFindNext";
	const auto  baseline = AvailableFlexibleMemory(test);
	const auto  base =
	    MapNamedFlexible(test, SceKernelPageSize * 3, SceKernelProtCpuRw, "prospero_part");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize, SceKernelPageSize),
	        "KernelMunmap(middle page)");

	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, SceKernelProtCpuRw, 1, 0,
	            0, 1, "prospero_part");
	ExpectUnmapped(test, base + SceKernelPageSize);
	ExpectRange(test, Query(test, base + SceKernelPageSize, SceKernelVqFindNext),
	            base + SceKernelPageSize * 2, base + SceKernelPageSize * 3, SceKernelProtCpuRw, 1,
	            0, 0, 1, "prospero_part");
	Check(test, AvailableFlexibleMemory(test) + SceKernelPageSize * 2 == baseline,
	      "partial munmap should return only the unmapped flexible page");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
	        "KernelMunmap(left cleanup)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize * 2, SceKernelPageSize),
	        "KernelMunmap(right cleanup)");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "cleanup should return all flexible memory to Prospero-reported budget");

	std::printf("[host]    %-48s ok\n", test);
}

void TestReserveMapFixedAndNoOverwrite() {
	const char* test = "ReserveMapFixedAndNoOverwrite";
	void*       addr = nullptr;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, SceKernelPageSize * 3, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(addr);

	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize * 3, 0, 0, 0, 0, 0);

	void* fixed = reinterpret_cast<void*>(base + SceKernelPageSize);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &fixed, SceKernelPageSize, SceKernelProtCpuRead, SceKernelMapFixed, "fixed_mid"),
	        "KernelMapNamedFlexibleMemory(fixed)");
	Check(test, reinterpret_cast<uint64_t>(fixed) == base + SceKernelPageSize,
	      "MAP_FIXED mapping moved");

	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, 0, 0, 0, 0, 0);
	ExpectRange(test, Query(test, base + SceKernelPageSize), base + SceKernelPageSize,
	            base + SceKernelPageSize * 2, SceKernelProtCpuRead, 1, 0, 0, 1, "fixed_mid");
	ExpectRange(test, Query(test, base + SceKernelPageSize * 2), base + SceKernelPageSize * 2,
	            base + SceKernelPageSize * 3, 0, 0, 0, 0, 0);

	void* blocked = reinterpret_cast<void*>(base + SceKernelPageSize);
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	                &blocked, SceKernelPageSize, SceKernelProtCpuRw,
	                SceKernelMapFixed | SceKernelMapNoOverwrite, "blocked"),
	            "KernelMapNamedFlexibleMemory(MAP_FIXED|MAP_NO_OVERWRITE)");
	ExpectRange(test, Query(test, base + SceKernelPageSize), base + SceKernelPageSize,
	            base + SceKernelPageSize * 2, SceKernelProtCpuRead, 1, 0, 0, 1, "fixed_mid");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
	        "KernelMunmap(left reserve cleanup)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize, SceKernelPageSize),
	        "KernelMunmap(fixed cleanup)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize * 2, SceKernelPageSize),
	        "KernelMunmap(right reserve cleanup)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFixedNoOverwriteRejectsReservedRange() {
	const char* test = "FixedNoOverwriteRejectsReservedRange";
	void*       addr = nullptr;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, SceKernelPageSize, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(addr);

	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, 0, 0, 0, 0, 0);

	void*     fixed = reinterpret_cast<void*>(base);
	const int ret   = Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	    &fixed, SceKernelPageSize, SceKernelProtCpuRw, SceKernelMapFixed | SceKernelMapNoOverwrite,
	    "reserved_blocked");
	const bool rejected = ret < OK;

	if (ret == OK) {
		CheckOk(test,
		        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(fixed),
		                                              SceKernelPageSize),
		        "KernelMunmap(unexpected fixed map cleanup)");
		if (reinterpret_cast<uint64_t>(fixed) != base) {
			CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
			        "KernelMunmap(reserve cleanup)");
		}
	} else {
		CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
		        "KernelMunmap(reserve cleanup)");
	}

	Check(test, rejected,
	      "MAP_FIXED|MAP_NO_OVERWRITE should reject an already reserved virtual "
	      "range");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectMapQueryOffsetAndPartialMunmap() {
	const char* test = "DirectMapQueryOffsetAndPartialMunmap";

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            SceKernelPageSize * 4, SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	void* addr = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &addr, SceKernelPageSize * 4, SceKernelProtCpuRw, 0, phys_addr, SceKernelPageSize,
	            "prospero_direct"),
	        "KernelMapNamedDirectMemory");
	const auto base = reinterpret_cast<uint64_t>(addr);
	const auto phys = static_cast<uint64_t>(phys_addr);
	Check(test, Libs::LibKernel::Memory::TestGuestAddressRangeIsOwned(base, SceKernelPageSize * 4),
	      "direct mapping escaped the guest owner");
	void* alias = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &alias, SceKernelPageSize * 4, SceKernelProtCpuRw, 0, phys_addr, SceKernelPageSize,
	            "prospero_direct_alias"),
	        "KernelMapNamedDirectMemory(alias)");
	const auto alias_base = reinterpret_cast<uint64_t>(alias);

	constexpr uint64_t alias_test_value = 0x4b595459444d454dull; // "KYTYDMEM"
	*reinterpret_cast<uint64_t*>(base)  = alias_test_value;
	Check(test, *reinterpret_cast<const uint64_t*>(alias_base) == alias_test_value,
	      "direct mappings of the same physical offset must share backing storage");
	uint64_t backing_read = 0;
	Check(test, Libs::LibKernel::Memory::TryReadBacking(base, &backing_read, sizeof(backing_read)),
	      "TryReadBacking should resolve a direct mapping");
	Check(test, backing_read == alias_test_value,
	      "TryReadBacking should observe the physical backing bytes");
	constexpr uint64_t backing_write = 0x524541444241434bull; // "READBACK"
	Check(test,
	      Libs::LibKernel::Memory::TryWriteBacking(alias_base + sizeof(uint64_t), &backing_write,
	                                               sizeof(backing_write)),
	      "TryWriteBacking should resolve a direct alias");
	backing_read = 0;
	Check(test,
	      Libs::LibKernel::Memory::TryReadBacking(base + sizeof(uint64_t), &backing_read,
	                                              sizeof(backing_read)),
	      "TryReadBacking should resolve an aliased physical offset");
	Check(test, backing_read == backing_write,
	      "backing reads and writes should preserve direct-memory aliasing");

	auto info = Query(test, base);
	ExpectRange(test, info, base, base + SceKernelPageSize * 4, SceKernelProtCpuRw, 0, 1, 0, 1,
	            "prospero_direct", phys);
	Check(test, info.memory_type == SceKernelMtypeC, "unexpected direct memory type");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize, SceKernelPageSize),
	        "KernelMunmap(direct middle page)");
	ExpectUnmapped(test, base + SceKernelPageSize);

	constexpr uint64_t transaction_sentinel = 0x5452414e53414354ull; // "TRANSACT"
	constexpr uint64_t rejected_write       = 0x4e4f504152544941ull; // "NOPARTIA"
	const auto         crossing_address     = base + SceKernelPageSize - sizeof(uint32_t);
	std::memcpy(reinterpret_cast<void*>(alias_base + SceKernelPageSize - sizeof(uint32_t)),
	            &transaction_sentinel, sizeof(transaction_sentinel));
	Check(test,
	      !Libs::LibKernel::Memory::TryWriteBacking(crossing_address, &rejected_write,
	                                                sizeof(rejected_write)),
	      "TryWriteBacking should reject a range crossing an unmapped span");
	uint64_t backing_after_rejected_write = 0;
	std::memcpy(&backing_after_rejected_write,
	            reinterpret_cast<const void*>(alias_base + SceKernelPageSize - sizeof(uint32_t)),
	            sizeof(backing_after_rejected_write));
	Check(test, backing_after_rejected_write == transaction_sentinel,
	      "failed backing writes must not modify a validated prefix");
	uint64_t rejected_read = transaction_sentinel;
	Check(test,
	      !Libs::LibKernel::Memory::TryReadBacking(crossing_address, &rejected_read,
	                                               sizeof(rejected_read)),
	      "TryReadBacking should reject a range crossing an unmapped span");
	Check(test, rejected_read == transaction_sentinel,
	      "failed backing reads must not modify a destination prefix");
	Check(test,
	      Libs::LibKernel::Memory::ClampRangeSize(base + SceKernelPageSize - 0xf30, 0x1560) ==
	          0xf30,
	      "ClampRangeSize did not stop at an unmapped span");

	info = Query(test, base + SceKernelPageSize, SceKernelVqFindNext);
	ExpectRange(test, info, base + SceKernelPageSize * 2, base + SceKernelPageSize * 4,
	            SceKernelProtCpuRw, 0, 1, 0, 1, "prospero_direct", phys + SceKernelPageSize * 2);
	Check(test, info.memory_type == SceKernelMtypeC, "unexpected right direct memory type");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
	        "KernelMunmap(direct left cleanup)");
	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize * 2, SceKernelPageSize * 2),
	    "KernelMunmap(direct right cleanup)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(alias_base, SceKernelPageSize * 4),
	        "KernelMunmap(direct alias cleanup)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize * 4),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectPartialProtectUnmapPreservesNeighbors() {
	const char* test      = "DirectPartialProtectUnmapPreservesNeighbors";
	const auto  size      = SceKernelPageSize * 3;
	int64_t     phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), size, SceKernelPageSize,
	            SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	void* address = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(&address, size, SceKernelProtCpuRw,
	                                                            0, phys_addr, SceKernelPageSize,
	                                                            "partial_protect_direct"),
	        "KernelMapNamedDirectMemory");
	const auto base = reinterpret_cast<uint64_t>(address);
	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelMprotect(reinterpret_cast<void*>(base + SceKernelPageSize),
	                                            SceKernelPageSize, SceKernelProtCpuRead),
	    "KernelMprotect(middle)");
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestHostMemory(base, size,
	                                                      Common::VirtualMemory::Mode::Read),
	      "owner could not protect fragmented backing views");
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestHostMemory(base, size,
	                                                      Common::VirtualMemory::Mode::ReadWrite),
	      "owner could not restore fragmented backing views");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize, SceKernelPageSize),
	        "KernelMunmap(middle)");

	// Access the neighbors without changing their host permissions first.
	auto* left  = reinterpret_cast<volatile uint64_t*>(base);
	auto* right = reinterpret_cast<volatile uint64_t*>(base + SceKernelPageSize * 2);
	*left       = 0x4c45465450524f54ull; // "LEFTPROT"
	*right      = 0x5247485450524f54ull; // "RGHTPROT"
	Check(test, *left == 0x4c45465450524f54ull, "partial unmap changed the left neighbor access");
	Check(test, *right == 0x5247485450524f54ull, "partial unmap changed the right neighbor access");

	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, size),
	        "KernelReleaseDirectMemory");
	ExpectUnmapped(test, base);
	ExpectUnmapped(test, base + SceKernelPageSize * 2);

	std::printf("[host]    %-48s ok\n", test);
}

#if defined(__linux__)
void TestPartialUnmapPreservesHostPermissions() {
	const char* test = "PartialUnmapPreservesHostPermissions";
	const auto base = MapNamedFlexible(test, SceKernelPageSize * 3, SceKernelProtCpuRw,
	                                   "unmap_host_permissions");
	using Common::VirtualMemory::Mode;
	Check(test, Libs::LibKernel::Memory::ProtectGuestHostMemory(base, SceKernelPageSize, Mode::Read),
	      "could not protect the left survivor from writes");
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestHostMemory(base + SceKernelPageSize * 2,
	                                                      SceKernelPageSize, Mode::NoAccess),
	      "could not protect the right survivor from reads");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize, SceKernelPageSize),
	        "KernelMunmap(middle)");

	// Probe actual host access without taking a signal or changing the page protections.
	uint64_t value = 0;
	iovec local {&value, sizeof(value)};
	iovec left {reinterpret_cast<void*>(base), sizeof(value)};
	iovec right {reinterpret_cast<void*>(base + SceKernelPageSize * 2), sizeof(value)};
	Check(test, process_vm_readv(getpid(), &local, 1, &left, 1, 0) == sizeof(value),
	      "partial unmap removed read access to the left survivor");
	Check(test, process_vm_writev(getpid(), &local, 1, &left, 1, 0) == -1,
	      "partial unmap removed the left survivor's write protection");
	Check(test, process_vm_readv(getpid(), &local, 1, &right, 1, 0) == -1,
	      "partial unmap removed the right survivor's read protection");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
	        "KernelMunmap(left cleanup)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelPageSize * 2, SceKernelPageSize),
	        "KernelMunmap(right cleanup)");
	std::printf("[host]    %-48s ok\n", test);
}
#endif

void TestDirectMapValidationBeforeOwnerMutation() {
	const char* test    = "DirectMapValidationBeforeOwnerMutation";
	int64_t     invalid = -1;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	                0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize + 1,
	                SceKernelPageSize, SceKernelMtypeC, &invalid),
	            "KernelAllocateDirectMemory(unaligned size)");
	Check(test, invalid == -1, "invalid direct allocation changed the output address");
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	                0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize,
	                0x1000, SceKernelMtypeC, &invalid),
	            "KernelAllocateDirectMemory(sub-page alignment)");
	Check(test, invalid == -1, "invalid alignment changed the output address");

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize * 2,
	            SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	auto expect_invalid = [&](size_t len, int prot, int flags, int64_t phys, size_t alignment,
	                          const char* action) {
		void* address = nullptr;
		CheckFailed(test,
		            Libs::LibKernel::Memory::KernelMapDirectMemory(&address, len, prot, flags, phys,
		                                                           alignment),
		            action);
		Check(test, address == nullptr, "invalid direct map changed the output address");
	};
	expect_invalid(SceKernelPageSize + 1, SceKernelProtCpuRw, 0, phys_addr, SceKernelPageSize,
	               "KernelMapDirectMemory(unaligned size)");
	expect_invalid(SceKernelPageSize, SceKernelProtCpuRw, 0, phys_addr + 1, SceKernelPageSize,
	               "KernelMapDirectMemory(unaligned physical address)");
	expect_invalid(SceKernelPageSize, SceKernelProtCpuExec, 0, phys_addr, SceKernelPageSize,
	               "KernelMapDirectMemory(executable)");

	void* aligned = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapDirectMemory(
	            &aligned, SceKernelPageSize, SceKernelProtCpuRw, 0, phys_addr, 0xc000),
	        "KernelMapDirectMemory(16K-multiple alignment)");
	Check(test, reinterpret_cast<uint64_t>(aligned) % 0xc000 == 0,
	      "non-power-of-two 16K alignment was not honored");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(aligned),
	                                              SceKernelPageSize),
	        "KernelMunmap(16K-multiple alignment)");

	void* ignored_flag = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapDirectMemory(&ignored_flag, SceKernelPageSize,
	                                                       SceKernelProtCpuRw, 0x08, phys_addr,
	                                                       SceKernelPageSize),
	        "KernelMapDirectMemory(ignored flag)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(ignored_flag),
	                                              SceKernelPageSize),
	        "KernelMunmap(ignored flag)");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize * 2),
	        "KernelReleaseDirectMemory");
	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectReleaseRollbackRestoresOwnerMapping() {
	const char* test      = "DirectReleaseRollbackRestoresOwnerMapping";
	int64_t     phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize,
	            SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");
	void* address = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &address, SceKernelPageSize, SceKernelProtCpuRw, 0, phys_addr, SceKernelPageSize,
	            "release_rollback"),
	        "KernelMapNamedDirectMemory");
	const auto base                    = reinterpret_cast<uint64_t>(address);
	*reinterpret_cast<uint64_t*>(base) = 0x52454c524f4c4c42ull; // "RELROLLB"

	Libs::LibKernel::Memory::TestFailNextPhysicalMemoryUnmap();
	CheckFailed(
	    test,
	    Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(phys_addr, SceKernelPageSize),
	    "KernelCheckedReleaseDirectMemory(injected failure)");
	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, SceKernelProtCpuRw, 0, 1,
	            0, 1, "release_rollback", static_cast<uint64_t>(phys_addr));
	Check(test, *reinterpret_cast<uint64_t*>(base) == 0x52454c524f4c4c42ull,
	      "release rollback lost the shared-backing contents");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelCheckedReleaseDirectMemory(retry)");
	ExpectUnmapped(test, base);
	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectReleaseContracts() {
	const char* test = "DirectReleaseContracts";
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(0, 0),
	        "KernelReleaseDirectMemory(zero length)");
	CheckOk(test, Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(0, 0),
	        "KernelCheckedReleaseDirectMemory(zero length)");
	CheckFailed(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(1, SceKernelPageSize),
	            "KernelReleaseDirectMemory(unaligned start)");
	CheckFailed(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(0, SceKernelPageSize + 1),
	            "KernelReleaseDirectMemory(unaligned size)");

	const auto free_offset = static_cast<int64_t>(
	    Libs::LibKernel::Memory::KernelGetDirectMemorySize() - SceKernelPageSize);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(free_offset, SceKernelPageSize),
	        "KernelReleaseDirectMemory(unallocated range)");
	Check(test,
	      Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(
	          free_offset, SceKernelPageSize) == Libs::LibKernel::KERNEL_ERROR_ENOENT,
	      "checked release did not report an unallocated range");

	std::printf("[host]    %-48s ok\n", test);
}

void TestReleasedReserveCanBeReused() {
	const char* test = "ReleasedReserveCanBeReused";
	void*       addr = nullptr;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, SceKernelPageSize, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(addr);
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize), "KernelMunmap");

	void* reused = reinterpret_cast<void*>(base);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(
	            &reused, SceKernelPageSize, SceKernelMapFixed | SceKernelMapNoOverwrite,
	            SceKernelPageSize),
	        "KernelReserveVirtualRange(reuse)");
	Check(test, reinterpret_cast<uint64_t>(reused) == base,
	      "released host reservation was not reusable at the same address");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
	        "KernelMunmap(reuse cleanup)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestMunmapAcrossAdjacentFlexibleMappings() {
	const char* test     = "MunmapAcrossAdjacentFlexibleMappings";
	const auto  baseline = AvailableFlexibleMemory(test);
	void*       reserve  = nullptr;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&reserve, SceKernelPageSize * 2, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(reserve);

	void* left  = reinterpret_cast<void*>(base);
	void* right = reinterpret_cast<void*>(base + SceKernelPageSize);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &left, SceKernelPageSize, SceKernelProtCpuRw, SceKernelMapFixed, "adjacent_left"),
	        "KernelMapNamedFlexibleMemory(left)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &right, SceKernelPageSize, SceKernelProtCpuRw, SceKernelMapFixed, "adjacent_right"),
	        "KernelMapNamedFlexibleMemory(right)");

	Check(test,
	      Libs::LibKernel::Memory::ClampRangeSize(base + SceKernelPageSize - 0x100, 0x200) == 0x200,
	      "ClampRangeSize did not cross adjacent committed mappings");
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestHostMemory(base, SceKernelPageSize * 2,
	                                                      Common::VirtualMemory::Mode::Read),
	      "owner could not protect adjacent backing mappings");
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestHostMemory(base, SceKernelPageSize * 2,
	                                                      Common::VirtualMemory::Mode::ReadWrite),
	      "owner could not restore adjacent backing mappings");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize * 2),
	        "KernelMunmap(adjacent mappings)");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "multi-range unmap leaked flexible-memory budget");
	ExpectUnmapped(test, base);
	ExpectUnmapped(test, base + SceKernelPageSize);

	std::printf("[host]    %-48s ok\n", test);
}

void TestNonzeroDirectOffsetAliasesSharedBacking() {
	const char* test = "NonzeroDirectOffsetAliasesSharedBacking";

	int64_t first  = 0;
	int64_t second = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize,
	            SceKernelPageSize, SceKernelMtypeC, &first),
	        "KernelAllocateDirectMemory(first)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize,
	            SceKernelPageSize, SceKernelMtypeC, &second),
	        "KernelAllocateDirectMemory(second)");
	Check(test, second == first + static_cast<int64_t>(SceKernelPageSize),
	      "second allocation should use a nonzero 16 KiB offset");

	void* first_alias  = nullptr;
	void* second_alias = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &first_alias, SceKernelPageSize, SceKernelProtCpuRw, 0, second, SceKernelPageSize,
	            "prospero_nonzero_a"),
	        "KernelMapNamedDirectMemory(first alias)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &second_alias, SceKernelPageSize, SceKernelProtCpuRw, 0, second, SceKernelPageSize,
	            "prospero_nonzero_b"),
	        "KernelMapNamedDirectMemory(second alias)");

	*reinterpret_cast<uint64_t*>(first_alias) = 0x4b59545931364b42ull; // "KYTY16KB"
	Check(test, *reinterpret_cast<const uint64_t*>(second_alias) == 0x4b59545931364b42ull,
	      "nonzero-offset mappings must share backing storage");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(first_alias),
	                                              SceKernelPageSize),
	        "KernelMunmap(first alias)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(second_alias),
	                                              SceKernelPageSize),
	        "KernelMunmap(second alias)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(second, SceKernelPageSize),
	        "KernelReleaseDirectMemory(second)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(first, SceKernelPageSize),
	        "KernelReleaseDirectMemory(first)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectMapAcrossContiguousAllocations() {
	const char* test   = "DirectMapAcrossContiguousAllocations";
	const auto  end    = Libs::LibKernel::Memory::KernelGetDirectMemorySize();
	int64_t     first  = 0;
	int64_t     second = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, end, SceKernelPageSize, SceKernelPageSize, SceKernelMtypeC, &first),
	        "KernelAllocateDirectMemory(first)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, end, SceKernelPageSize, SceKernelPageSize, SceKernelMtypeC, &second),
	        "KernelAllocateDirectMemory(second)");
	Check(test, second == first + static_cast<int64_t>(SceKernelPageSize),
	      "test allocations are not physically contiguous");

	void* mapping = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &mapping, SceKernelPageSize * 2, SceKernelProtCpuRw, 0, first, SceKernelPageSize,
	            "contiguous_allocations"),
	        "KernelMapNamedDirectMemory");
	auto* words = reinterpret_cast<uint64_t*>(mapping);
	words[0]    = 0x434f4e5449474c46ull; // "CONTIGLF"
	*reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(mapping) + SceKernelPageSize) =
	    0x434f4e5449475254ull; // "CONTIGRT"

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(first, SceKernelPageSize * 2),
	        "KernelCheckedReleaseDirectMemory(contiguous span)");
	ExpectUnmapped(test, reinterpret_cast<uint64_t>(mapping));

	int64_t reclaimed = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, end, SceKernelPageSize * 2, SceKernelPageSize, SceKernelMtypeC, &reclaimed),
	        "KernelAllocateDirectMemory(reclaimed)");
	Check(test, reclaimed == first, "released contiguous span was not coalesced");
	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(reclaimed, SceKernelPageSize * 2),
	    "KernelCheckedReleaseDirectMemory(reclaimed)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectPhysicalFreeRangeReuseAndCoalescing() {
	const char* test = "DirectPhysicalFreeRangeReuseAndCoalescing";
	const auto  end  = Libs::LibKernel::Memory::KernelGetDirectMemorySize();

	int64_t first = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, end, SceKernelPageSize * 3, SceKernelPageSize, SceKernelMtypeC, &first),
	        "KernelAllocateDirectMemory(first)");
	const auto middle = first + static_cast<int64_t>(SceKernelPageSize);
	const auto last   = middle + static_cast<int64_t>(SceKernelPageSize);

	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(middle, SceKernelPageSize),
	        "KernelReleaseDirectMemory(middle split)");
	int64_t reused = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, end, SceKernelPageSize, SceKernelPageSize, SceKernelMtypeC, &reused),
	        "KernelAllocateDirectMemory(reused)");
	Check(test, reused == middle, "released physical gap was not reused");

	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(first, SceKernelPageSize),
	        "KernelReleaseDirectMemory(left split)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(reused, SceKernelPageSize),
	        "KernelReleaseDirectMemory(reused)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(last, SceKernelPageSize),
	        "KernelReleaseDirectMemory(right split)");

	int64_t coalesced = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, end, SceKernelPageSize * 3, SceKernelPageSize, SceKernelMtypeC, &coalesced),
	        "KernelAllocateDirectMemory(coalesced)");
	Check(test, coalesced == first, "adjacent released physical ranges were not coalesced");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(coalesced, SceKernelPageSize * 3),
	        "KernelReleaseDirectMemory(coalesced)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectAlignmentStaysWithinSearchRange() {
	const char*        test         = "DirectAlignmentStaysWithinSearchRange";
	constexpr int64_t  search_start = SceKernelPageSize * 2;
	constexpr uint64_t alignment    = SceKernelPageSize * 3;
	const auto         search_end   = Libs::LibKernel::Memory::KernelGetDirectMemorySize();

	int64_t phys_addr = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(search_start, search_end,
	                                                            SceKernelPageSize, alignment,
	                                                            SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory(non-power-of-two alignment)");
	Check(test, phys_addr >= search_start, "aligned allocation escaped below search_start");
	Check(test, static_cast<uint64_t>(phys_addr) % alignment == 0,
	      "allocation did not honor the requested alignment");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelReleaseDirectMemory");

	constexpr size_t out_of_range_alignment = UINT64_MAX - (SceKernelPageSize - 1);
	phys_addr                               = -1;
	const int result                        = Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	    search_start, search_end, SceKernelPageSize, out_of_range_alignment, SceKernelMtypeC,
	    &phys_addr);
	CheckFailed(test, result, "KernelAllocateDirectMemory(out-of-range alignment)");
	Check(test, phys_addr == -1, "failed allocation modified physAddrOut");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDefaultDirectMapUsesSystemAddressRange() {
	const char* test = "DefaultDirectMapUsesSystemAddressRange";

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize,
	            SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	void* address = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(&address, SceKernelPageSize,
	                                                            SceKernelProtCpuRw, 0, phys_addr,
	                                                            SceKernelPageSize, "system_direct"),
	        "KernelMapNamedDirectMemory");
	Check(test, address != nullptr, "direct mapping returned null");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	constexpr uint64_t SystemManagedMin = 0x0000040000ull;
	constexpr uint64_t SystemManagedMax = 0x07fffeffffull;
	const auto         mapped           = reinterpret_cast<uint64_t>(address);
	Check(test, mapped >= SystemManagedMin && mapped + SceKernelPageSize - 1 <= SystemManagedMax,
	      "default direct mapping fell outside the system-managed host range");
#endif

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(address),
	                                              SceKernelPageSize),
	        "KernelMunmap");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestLargeDirectMapAliasesAcrossChunks() {
	const char*        test     = "LargeDirectMapAliasesAcrossChunks";
	constexpr uint64_t size     = 0x400000;
	constexpr uint64_t boundary = 0x200000;

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), size, 0x10000,
	            SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	void* first_alias  = nullptr;
	void* second_alias = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &first_alias, size, SceKernelProtCpuRw, 0, phys_addr, 0x10000, "large_direct_a"),
	        "KernelMapNamedDirectMemory(first alias)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &second_alias, size, SceKernelProtCpuRw, 0, phys_addr, 0x10000, "large_direct_b"),
	        "KernelMapNamedDirectMemory(second alias)");

	auto* first                                        = static_cast<uint8_t*>(first_alias);
	auto* second                                       = static_cast<uint8_t*>(second_alias);
	*reinterpret_cast<uint64_t*>(first)                = 0x1111222233334444ull;
	*reinterpret_cast<uint64_t*>(first + boundary - 8) = 0x5555666677778888ull;
	*reinterpret_cast<uint64_t*>(first + boundary)     = 0x9999aaaabbbbccccull;
	*reinterpret_cast<uint64_t*>(first + size - 8)     = 0xddddeeeeffff0001ull;
	Check(test,
	      *reinterpret_cast<const uint64_t*>(second) == 0x1111222233334444ull &&
	          *reinterpret_cast<const uint64_t*>(second + boundary - 8) == 0x5555666677778888ull &&
	          *reinterpret_cast<const uint64_t*>(second + boundary) == 0x9999aaaabbbbccccull &&
	          *reinterpret_cast<const uint64_t*>(second + size - 8) == 0xddddeeeeffff0001ull,
	      "large direct aliases diverged at a mapping chunk boundary");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(first_alias), size),
	        "KernelMunmap(first alias)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(second_alias), size),
	        "KernelMunmap(second alias)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, size),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

/**
 * @brief Tests on-demand commitment of physical direct memory and placeholder retention on unmap.
 */
void TestOnDemandPhysicalMemoryCommitment() {
	const char*        test = "OnDemandPhysicalMemoryCommitment";
	constexpr uint64_t size = 0x200000; // 2 MiB

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), size, 0x10000,
	            SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	void* address = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &address, size, SceKernelProtCpuRw, 0, phys_addr, 0x10000, "on_demand_direct"),
	        "KernelMapNamedDirectMemory");

	// Verify write and read work correctly through the on-demand mapped view
	auto* ptr = static_cast<uint64_t*>(address);
	ptr[0] = 0xdeadbeefcafebabeull;
	ptr[size / sizeof(uint64_t) - 1] = 0x0123456789abcdefull;
	Check(test, ptr[0] == 0xdeadbeefcafebabeull, "written value at start did not match");
	Check(test, ptr[size / sizeof(uint64_t) - 1] == 0x0123456789abcdefull,
	      "written value at end did not match");

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Verify mapped view is committed
	MEMORY_BASIC_INFORMATION mbi {};
	Check(test, VirtualQuery(address, &mbi, sizeof(mbi)) != 0, "VirtualQuery(mapped) failed");
	Check(test, mbi.State == MEM_COMMIT, "mapped memory is not in MEM_COMMIT state");
#endif

	// Unmap and verify that placeholder is restored to reserved state
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(address), size),
	        "KernelMunmap");

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Check(test, VirtualQuery(address, &mbi, sizeof(mbi)) != 0, "VirtualQuery(unmapped) failed");
	Check(test, mbi.State == MEM_RESERVE, "unmapped memory placeholder is not in MEM_RESERVE state");
#endif

	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, size),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestHintlessDirectMapUsesCanonicalGuestBase() {
	// Mirrors the allocation Sony's libc.prx makes for its internal heap: 4 MiB of
	// direct memory, 2 MiB aligned, mapped with no address hint. The PS5 kernel never
	// places hint-less user mappings below 0x200000000 and guest code relies on that
	// (libc fails its mspace setup for a lower heap address, and the first malloc then
	// dereferences a null mspace). Writes through the mapping must also stick.
	const char* test = "HintlessDirectMapUsesCanonicalGuestBase";

	constexpr uint64_t Len   = 0x400000;
	constexpr uint64_t Align = 0x200000;

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(0, 0x260000000ull, Len, Align, 12,
	                                                            &phys_addr),
	        "KernelAllocateDirectMemory");

	void* address = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(&address, Len, SceKernelProtCpuRw,
	                                                            0, phys_addr, Align, "libc_heap"),
	        "KernelMapNamedDirectMemory");
	const auto base = reinterpret_cast<uint64_t>(address);
	{
		char message[128] = {};
		std::snprintf(message, sizeof(message),
		              "hint-less direct map landed below the PS5 base: 0x%016" PRIx64, base);
		Check(test, base >= 0x200000000ull, message);
	}

	auto* header = reinterpret_cast<uint64_t*>(base);
	header[0]    = 0x4d53504143453030ull; // "MSPACE00"
	header[7]    = 0x58585858ull;         // magic at +0x38, like the libc mspace
	*reinterpret_cast<uint64_t*>(base + Len - 8) = 0x454e444d41524bull;

	Check(test, header[0] == 0x4d53504143453030ull, "immediate readback of header[0] failed");
	Check(test, header[7] == 0x58585858ull, "immediate readback of header[7] failed");
	Check(test, *reinterpret_cast<const uint64_t*>(base + Len - 8) == 0x454e444d41524bull,
	      "immediate readback of tail failed");

	uint64_t backing = 0;
	Check(test, Libs::LibKernel::Memory::TryReadBacking(base + 0x38, &backing, sizeof(backing)),
	      "TryReadBacking(header+0x38)");
	Check(test, backing == 0x58585858ull, "backing store does not see the guest write at +0x38");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, Len), "KernelMunmap");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, Len),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectMemoryContentPersistsAcrossRemap() {
	const char* test = "DirectMemoryContentPersistsAcrossRemap";

	constexpr uint64_t MapSize = SceKernelPageSize * 4;

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            MapSize, SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	// Direct memory is physical: contents must survive unmapping and remapping, including
	// a remap of a sub-range at a nonzero physical offset.
	void* address = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(&address, MapSize,
	                                                            SceKernelProtCpuRw, 0, phys_addr,
	                                                            SceKernelPageSize, "persist_a"),
	        "KernelMapNamedDirectMemory(first)");
	const auto base = reinterpret_cast<uint64_t>(address);
	for (uint64_t offset = 0; offset < MapSize; offset += sizeof(uint64_t)) {
		*reinterpret_cast<uint64_t*>(base + offset) = offset ^ 0x4b5954595045525aull; // "KYTYPERZ"
	}
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, MapSize), "KernelMunmap(first)");

	void* remap = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(&remap, MapSize,
	                                                            SceKernelProtCpuRw, 0, phys_addr,
	                                                            SceKernelPageSize, "persist_b"),
	        "KernelMapNamedDirectMemory(remap)");
	const auto remap_base = reinterpret_cast<uint64_t>(remap);
	for (uint64_t offset = 0; offset < MapSize; offset += sizeof(uint64_t)) {
		const auto expected = offset ^ 0x4b5954595045525aull;
		const auto actual   = *reinterpret_cast<const uint64_t*>(remap_base + offset);
		if (actual != expected) {
			char message[160] = {};
			std::snprintf(message, sizeof(message),
			              "content lost across remap at offset 0x%" PRIx64 ": expected 0x%016" PRIx64
			              ", read 0x%016" PRIx64,
			              offset, expected, actual);
			Fail(test, message);
		}
	}
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(remap_base, MapSize), "KernelMunmap(remap)");

	// Sub-range remap at a nonzero physical offset: page 2 of the original allocation.
	void* partial = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &partial, SceKernelPageSize, SceKernelProtCpuRw, 0,
	            phys_addr + static_cast<int64_t>(SceKernelPageSize * 2), SceKernelPageSize,
	            "persist_c"),
	        "KernelMapNamedDirectMemory(partial)");
	const auto partial_base = reinterpret_cast<uint64_t>(partial);
	for (uint64_t offset = 0; offset < SceKernelPageSize; offset += sizeof(uint64_t)) {
		const auto expected = (SceKernelPageSize * 2 + offset) ^ 0x4b5954595045525aull;
		const auto actual   = *reinterpret_cast<const uint64_t*>(partial_base + offset);
		if (actual != expected) {
			char message[160] = {};
			std::snprintf(message, sizeof(message),
			              "content lost in partial remap at offset 0x%" PRIx64
			              ": expected 0x%016" PRIx64 ", read 0x%016" PRIx64,
			              offset, expected, actual);
			Fail(test, message);
		}
	}
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(partial_base, SceKernelPageSize),
	        "KernelMunmap(partial)");

	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, MapSize),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestDirectMapUnmapReusesHostAddress() {
	const char* test = "DirectMapUnmapReusesHostAddress";

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            SceKernelPageSize, SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	uint64_t first_address = 0;
	for (int iteration = 0; iteration < 64; iteration++) {
		void* address = nullptr;
		CheckOk(test,
		        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
		            &address, SceKernelPageSize, SceKernelProtCpuRw, 0, phys_addr,
		            SceKernelPageSize, "reuse_direct"),
		        "KernelMapNamedDirectMemory");
		const auto current_address = reinterpret_cast<uint64_t>(address);
		if (iteration == 0) {
			first_address = current_address;
		} else {
			char message[160] = {};
			std::snprintf(message, sizeof(message),
			              "direct map address changed from 0x%016" PRIx64 " to 0x%016" PRIx64,
			              first_address, current_address);
			Check(test, current_address == first_address, message);
		}
		CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(current_address, SceKernelPageSize),
		        "KernelMunmap");
	}

	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFixedReserveReplacesPartialDirectMapping() {
	const char*        test         = "FixedReserveReplacesPartialDirectMapping";
	constexpr uint64_t page_count   = 13;
	constexpr uint64_t keep_pages   = 5;
	constexpr uint64_t total_size   = SceKernelPageSize * page_count;
	constexpr uint64_t keep_size    = SceKernelPageSize * keep_pages;
	constexpr uint64_t replace_size = total_size - keep_size;

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            total_size, SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");
	void* alias = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &alias, total_size, SceKernelProtCpuRw, 0, phys_addr, SceKernelPageSize,
	            "partial_replace_alias"),
	        "KernelMapNamedDirectMemory(alias)");
	*reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(alias) + keep_size) =
	    0x4b595459414c4941ull; // "KYTYALIA"

	void* reserve = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&reserve, total_size, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange(container)");
	const auto base = reinterpret_cast<uint64_t>(reserve);

	void* mapped = reserve;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &mapped, total_size, SceKernelProtCpuRw, SceKernelMapFixed | SceKernelMapNoCoalesce,
	            phys_addr, SceKernelPageSize, "partial_replace_direct"),
	        "KernelMapNamedDirectMemory");
	Check(test, mapped == reserve, "fixed direct mapping moved");
	*reinterpret_cast<uint64_t*>(base) = 0x4b5954594b454550ull; // "KYTYKEEP"

	void* replacement = reinterpret_cast<void*>(base + keep_size);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(
	            &replacement, replace_size, SceKernelMapFixed | SceKernelMapNoCoalesce,
	            SceKernelPageSize),
	        "KernelReserveVirtualRange(partial replacement)");
	Check(test, reinterpret_cast<uint64_t>(replacement) == base + keep_size,
	      "partial fixed reservation moved");
	Check(test, *reinterpret_cast<uint64_t*>(base) == 0x4b5954594b454550ull,
	      "partial replacement damaged the neighboring direct mapping");
	ExpectRange(test, Query(test, base), base, base + keep_size, SceKernelProtCpuRw, 0, 1, 0, 1,
	            "partial_replace_direct");
	ExpectRange(test, Query(test, base + keep_size), base + keep_size, base + total_size, 0, 0, 0,
	            0, 0);

	void* remapped = replacement;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &remapped, replace_size, SceKernelProtCpuRw,
	            SceKernelMapFixed | SceKernelMapNoCoalesce, phys_addr + keep_size,
	            SceKernelPageSize, "partial_replace_remap"),
	        "KernelMapNamedDirectMemory(replacement reuse)");
	Check(test, remapped == replacement, "replacement reservation was not reusable in place");
	Check(test, *reinterpret_cast<uint64_t*>(remapped) == 0x4b595459414c4941ull,
	      "replacement remap did not preserve its direct-memory backing offset");
	*reinterpret_cast<uint64_t*>(remapped) = 0x4b59545952455553ull; // "KYTYREUS"
	Check(test,
	      *reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(alias) + keep_size) ==
	          0x4b59545952455553ull,
	      "replacement remap did not alias the original direct-memory backing");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, keep_size),
	        "KernelMunmap(direct remainder)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base + keep_size, replace_size),
	        "KernelMunmap(reused replacement)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(alias), total_size),
	        "KernelMunmap(alias)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, total_size),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFixedReserveRollbackSkipsUntouchedChunks() {
	const char*        test       = "FixedReserveRollbackSkipsUntouchedChunks";
	constexpr uint64_t part_size  = SceKernelPageSize * 2;
	constexpr uint64_t total_size = part_size * 2;
	int64_t            left_phys  = 0;
	int64_t            right_phys = 0;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            part_size, SceKernelPageSize, SceKernelMtypeC, &left_phys),
	        "KernelAllocateDirectMemory(left)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            part_size, SceKernelPageSize, SceKernelMtypeC, &right_phys),
	        "KernelAllocateDirectMemory(right)");

	void* reserve = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&reserve, total_size, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base  = reinterpret_cast<uint64_t>(reserve);
	void*      left  = reserve;
	void*      right = reinterpret_cast<void*>(base + part_size);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &left, part_size, SceKernelProtCpuRw, SceKernelMapFixed | SceKernelMapNoCoalesce,
	            left_phys, SceKernelPageSize, "rollback_left"),
	        "KernelMapNamedDirectMemory(left)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &right, part_size, SceKernelProtCpuRw, SceKernelMapFixed | SceKernelMapNoCoalesce,
	            right_phys, SceKernelPageSize, "rollback_right"),
	        "KernelMapNamedDirectMemory(right)");
	*reinterpret_cast<uint64_t*>(left)  = 0x4b5954594c454654ull; // "KYTYLEFT"
	*reinterpret_cast<uint64_t*>(right) = 0x4b59545952474854ull; // "KYTYRGHT"

	Libs::LibKernel::Memory::TestFailPhysicalMemoryUnmapAfter(1);
	void* replacement = reserve;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelReserveVirtualRange(
	                &replacement, total_size, SceKernelMapFixed | SceKernelMapNoCoalesce,
	                SceKernelPageSize),
	            "KernelReserveVirtualRange(second-chunk rollback)");
	Check(test, *reinterpret_cast<uint64_t*>(left) == 0x4b5954594c454654ull,
	      "rollback did not restore the mutated first chunk");
	Check(test, *reinterpret_cast<uint64_t*>(right) == 0x4b59545952474854ull,
	      "rollback damaged the failing second chunk");
	Check(test, !Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(base, part_size),
	      "first restored mapping remained recorded as a free placeholder");
	Check(test, !Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(base + part_size, part_size),
	      "second restored mapping remained recorded as a free placeholder");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, part_size), "KernelMunmap(left)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base + part_size, part_size),
	        "KernelMunmap(right)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(left_phys, part_size),
	        "KernelReleaseDirectMemory(left)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(right_phys, part_size),
	        "KernelReleaseDirectMemory(right)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFixedReserveRollbackConsumesRestoredPlaceholder() {
	const char*        test       = "FixedReserveRollbackConsumesRestoredPlaceholder";
	constexpr uint64_t total_size = SceKernelPageSize * 4;

	int64_t phys_addr = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            total_size, SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");

	void* reserve = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&reserve, total_size, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange");
	const auto base = reinterpret_cast<uint64_t>(reserve);

	void* mapped = reserve;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &mapped, total_size, SceKernelProtCpuRw, SceKernelMapFixed | SceKernelMapNoCoalesce,
	            phys_addr, SceKernelPageSize, "rollback_direct"),
	        "KernelMapNamedDirectMemory");
	*reinterpret_cast<uint64_t*>(base) = 0x4b595459524f4c4cull; // "KYTYROLL"

	Libs::LibKernel::Memory::TestFailNextPhysicalMemoryUnmap();
	void* replacement = reserve;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelReserveVirtualRange(
	                &replacement, total_size, SceKernelMapFixed | SceKernelMapNoCoalesce,
	                SceKernelPageSize),
	            "KernelReserveVirtualRange(injected rollback)");
	Check(test, *reinterpret_cast<uint64_t*>(base) == 0x4b595459524f4c4cull,
	      "rollback did not restore direct-memory contents");
	Check(test, !Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(base, total_size),
	      "rollback left a mapped direct range recorded as a free placeholder");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, total_size), "KernelMunmap");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, total_size),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFixedReserveRangeAddRollbackKeepsPlaceholder() {
	const char*        test      = "FixedReserveRangeAddRollbackKeepsPlaceholder";
	constexpr uint64_t size      = SceKernelPageSize * 4;
	int64_t            phys_addr = 0;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            SceKernelDirectMemoryStart, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            size, SceKernelPageSize, SceKernelMtypeC, &phys_addr),
	        "KernelAllocateDirectMemory");
	void* reserve = nullptr;
	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelReserveVirtualRange(&reserve, size, 0, SceKernelPageSize),
	    "KernelReserveVirtualRange");
	const auto base   = reinterpret_cast<uint64_t>(reserve);
	void*      mapped = reserve;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &mapped, size, SceKernelProtCpuRw, SceKernelMapFixed | SceKernelMapNoCoalesce,
	            phys_addr, SceKernelPageSize, "range_add_rollback"),
	        "KernelMapNamedDirectMemory");
	*reinterpret_cast<uint64_t*>(mapped) = 0x4b59545952414e47ull; // "KTYRANG"

	Libs::LibKernel::Memory::TestFailNextFixedReserveRangeRegistration();
	void* replacement = mapped;
	CheckFailed(
	    test,
	    Libs::LibKernel::Memory::KernelReserveVirtualRange(
	        &replacement, size, SceKernelMapFixed | SceKernelMapNoCoalesce, SceKernelPageSize),
	    "KernelReserveVirtualRange(range-add rollback)");
	Check(test, *reinterpret_cast<uint64_t*>(mapped) == 0x4b59545952414e47ull,
	      "range-add rollback did not restore direct-memory contents");
	Check(test, !Libs::LibKernel::Memory::TestPlaceholderRangeIsFree(base, size),
	      "range-add rollback left the restored mapping recorded as free");
	ExpectRange(test, Query(test, base), base, base + size, SceKernelProtCpuRw, 0, 1, 0, 1,
	            "range_add_rollback");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, size), "KernelMunmap");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, size),
	        "KernelReleaseDirectMemory");
	std::printf("[host]    %-48s ok\n", test);
}

void TestLargeHintedReserveHostsSmallDirectMap() {
	const char* test = "LargeHintedReserveHostsSmallDirectMap";

	constexpr uint64_t arena_base  = 0x1000000000ull;
	constexpr uint64_t arena_size  = 0x04000000ull;
	constexpr uint64_t window_size = 0x00200000ull;

	void* arena = reinterpret_cast<void*>(arena_base);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&arena, arena_size, 0, 0x200000),
	        "KernelReserveVirtualRange(arena)");
	const auto actual_arena = reinterpret_cast<uint64_t>(arena);
	Check(test, actual_arena >= arena_base && (actual_arena & (0x200000 - 1u)) == 0,
	      "large hinted reserve violated its search start or alignment");

	void* window = reinterpret_cast<void*>(arena_base);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(&window, window_size, 0,
	                                                           SceKernelPageSize),
	        "KernelReserveVirtualRange(window)");
	Check(test, reinterpret_cast<uint64_t>(window) >= actual_arena + arena_size,
	      "second hinted reserve overlaps the large arena");

	int64_t phys = 0;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize * 2,
	            SceKernelPageSize, SceKernelMtypeC, &phys),
	        "KernelAllocateDirectMemory");

	void* mapped = window;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &mapped, SceKernelPageSize * 2, SceKernelProtCpuRw,
	            SceKernelMapFixed | SceKernelMapNoCoalesce, phys, 0, "prospero_large_reserve"),
	        "KernelMapNamedDirectMemory");
	Check(test, mapped == window, "fixed direct mapping moved away from the reserved window");
	*reinterpret_cast<uint64_t*>(mapped) = 0x4b59545952455356ull; // "KYTYRESV"

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(mapped),
	                                              SceKernelPageSize * 2),
	        "KernelMunmap(direct)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys, SceKernelPageSize * 2),
	        "KernelReleaseDirectMemory");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(window) +
	                                                  SceKernelPageSize * 2,
	                                              window_size - SceKernelPageSize * 2),
	        "KernelMunmap(window reserve remainder)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(arena), arena_size),
	        "KernelMunmap(arena reserve)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestMemoryPoolAlignmentContracts() {
	const char* test = "MemoryPoolAlignmentContracts";
	void*       addr = nullptr;

	CheckFailed(
	    test,
	    Libs::LibKernel::Memory::KernelMemoryPoolReserve(nullptr, SceKernelPageSize, 0, 0, &addr),
	    "KernelMemoryPoolReserve(16KiB len)");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolReserve(nullptr, SceKernelMemoryPoolReserveLen,
	                                                         0, 0, &addr),
	        "KernelMemoryPoolReserve");
	const auto base = reinterpret_cast<uint64_t>(addr);

	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMemoryPoolCommit(reinterpret_cast<void*>(base),
	                                                            SceKernelPageSize, SceKernelMtypeC,
	                                                            SceKernelProtCpuRw, 0),
	            "KernelMemoryPoolCommit(16KiB len)");
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMemoryPoolDecommit(reinterpret_cast<void*>(base),
	                                                              SceKernelPageSize, 0),
	            "KernelMemoryPoolDecommit(16KiB len)");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelMemoryPoolReserveLen),
	        "KernelMunmap(pool reserve cleanup)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestProsperoSampleMemoryPoolExpandCommit() {
	const char* test = "ProsperoSampleMemoryPoolExpandCommit";

	int64_t pool_offset = -1;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMemoryPoolExpand(
	                0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), SceKernelPageSize,
	                SceKernelMemoryPoolAlignment, &pool_offset),
	            "KernelMemoryPoolExpand(16KiB len)");
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMemoryPoolExpand(
	                0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	                SceKernelMemoryPoolExpandLen, SceKernelPageSize, &pool_offset),
	            "KernelMemoryPoolExpand(16KiB alignment)");
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMemoryPoolExpand(
	                0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	                SceKernelMemoryPoolExpandLen, SceKernelMemoryPoolAlignment * 3, &pool_offset),
	            "KernelMemoryPoolExpand(non-power-of-two alignment)");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolExpand(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            SceKernelMemoryPoolExpandLen, SceKernelMemoryPoolAlignment, &pool_offset),
	        "KernelMemoryPoolExpand");
	Check(test,
	      pool_offset >= 0 &&
	          (static_cast<uint64_t>(pool_offset) & (SceKernelMemoryPoolAlignment - 1u)) == 0,
	      "expanded physical range is not 64 KiB aligned");
	void* direct_alias = nullptr;
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMapDirectMemory(
	                &direct_alias, SceKernelMemoryPoolCommitLen, SceKernelProtCpuRw, 0, pool_offset,
	                SceKernelMemoryPoolAlignment),
	            "KernelMapDirectMemory(pool expansion)");

	Libs::LibKernel::Memory::KernelMemoryPoolBlockStats stats {};
	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolGetBlockStats(&stats, sizeof(stats)),
	        "KernelMemoryPoolGetBlockStats(expanded)");
	Check(test,
	      stats.available_flushed_blocks ==
	          static_cast<int32_t>(SceKernelMemoryPoolExpandLen / SceKernelMemoryPoolAlignment),
	      "expanded pages were not added to the pool budget");

	void* arena = reinterpret_cast<void*>(0x1000000000ull);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolReserve(arena, SceKernelMemoryPoolReserveLen,
	                                                         0, 0, &arena),
	        "KernelMemoryPoolReserve");
	const auto base              = reinterpret_cast<uint64_t>(arena);
	const auto flexible_baseline = AvailableFlexibleMemory(test);
	const auto commit_len        = SceKernelMemoryPoolCommitLen * 2;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(arena, commit_len, SceKernelMtypeC,
	                                                        SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit");
	ExpectRange(test, Query(test, base), base, base + commit_len, SceKernelProtCpuRw, 0, 0, 1, 1);
	Check(test, Libs::LibKernel::Memory::TestGuestAddressRangeIsOwned(base, commit_len),
	      "pooled commit escaped the guest owner");
	Check(test, AvailableFlexibleMemory(test) == flexible_baseline,
	      "pooled commit consumed flexible memory instead of expanded direct "
	      "backing");
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelCheckedReleaseDirectMemory(
	                pool_offset, SceKernelMemoryPoolExpandLen),
	            "KernelCheckedReleaseDirectMemory(committed pool expansion)");

	constexpr uint64_t first_value     = 0x504f4f4c4241434bull; // "POOLBACK"
	constexpr uint64_t second_value    = 0x5348415245444d45ull; // "SHAREDME"
	*reinterpret_cast<uint64_t*>(base) = first_value;
	*reinterpret_cast<uint64_t*>(base + SceKernelMemoryPoolCommitLen) = second_value;
	uint64_t backing_read                                             = 0;
	Check(test, Libs::LibKernel::Memory::TryReadBacking(base, &backing_read, sizeof(backing_read)),
	      "TryReadBacking did not resolve pooled memory");
	Check(test, backing_read == first_value,
	      "shared backing did not observe a pooled-memory CPU write");

	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, SceKernelMemoryPoolCommitLen, 0),
	    "KernelMemoryPoolDecommit(first page)");
	ExpectRange(test, Query(test, base), base, base + SceKernelMemoryPoolCommitLen, 0, 0, 0, 1, 0);
	ExpectRange(test, Query(test, base + SceKernelMemoryPoolCommitLen),
	            base + SceKernelMemoryPoolCommitLen, base + commit_len, SceKernelProtCpuRw, 0, 0, 1,
	            1);
	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolGetBlockStats(&stats, sizeof(stats)),
	        "KernelMemoryPoolGetBlockStats(partially decommitted)");
	Check(test,
	      stats.available_flushed_blocks ==
	              static_cast<int32_t>(SceKernelMemoryPoolExpandLen / SceKernelMemoryPoolAlignment -
	                                   1) &&
	          stats.allocated_flushed_blocks == 1,
	      "partial decommit returned the wrong number of pages to the expanded "
	      "pool");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(arena, SceKernelMemoryPoolCommitLen,
	                                                        SceKernelMtypeC, SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit(first-page recommit)");
	Check(test,
	      *reinterpret_cast<const uint64_t*>(base) == first_value &&
	          *reinterpret_cast<const uint64_t*>(base + SceKernelMemoryPoolCommitLen) ==
	              second_value,
	      "partially recommitted pooled pages did not retain shared-backing "
	      "contents");

	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, commit_len, 0),
	        "KernelMemoryPoolDecommit(cleanup)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelMemoryPoolReserveLen),
	        "KernelMunmap(pool reserve cleanup)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(pool_offset,
	                                                           SceKernelMemoryPoolExpandLen),
	        "KernelReleaseDirectMemory(pool expansion)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolGetBlockStats(&stats, sizeof(stats)),
	        "KernelMemoryPoolGetBlockStats(released)");
	Check(test, stats.available_flushed_blocks == 0 && stats.allocated_flushed_blocks == 0,
	      "released expansion remained in the pool budget");

	std::printf("[host]    %-48s ok\n", test);
}

void TestFragmentedMemoryPoolBacking() {
	const char* test = "FragmentedMemoryPoolBacking";

	int64_t    first_pool  = -1;
	int64_t    direct_gap  = -1;
	int64_t    second_pool = -1;
	const auto direct_end =
	    static_cast<int64_t>(Libs::LibKernel::Memory::KernelGetDirectMemorySize());
	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelMemoryPoolExpand(0, direct_end, SceKernelMemoryPoolCommitLen,
	                                                    SceKernelMemoryPoolAlignment, &first_pool),
	    "KernelMemoryPoolExpand(first)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            0, direct_end, SceKernelMemoryPoolCommitLen, SceKernelMemoryPoolAlignment,
	            SceKernelMtypeC, &direct_gap),
	        "KernelAllocateDirectMemory(gap)");
	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelMemoryPoolExpand(0, direct_end, SceKernelMemoryPoolCommitLen,
	                                                    SceKernelMemoryPoolAlignment, &second_pool),
	    "KernelMemoryPoolExpand(second)");
	Check(test,
	      first_pool + static_cast<int64_t>(SceKernelMemoryPoolCommitLen) == direct_gap &&
	          direct_gap + static_cast<int64_t>(SceKernelMemoryPoolCommitLen) == second_pool,
	      "test setup did not create nonadjacent pool expansions");

	void* arena = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolReserve(nullptr, SceKernelMemoryPoolReserveLen,
	                                                         0, 0, &arena),
	        "KernelMemoryPoolReserve");
	const auto base       = reinterpret_cast<uint64_t>(arena);
	const auto commit_len = SceKernelMemoryPoolCommitLen * 2;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(arena, commit_len, SceKernelMtypeC,
	                                                        SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit(fragmented)");

	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelMemoryPoolCommit(
	                reinterpret_cast<void*>(base + commit_len), SceKernelMemoryPoolCommitLen,
	                SceKernelMtypeC, SceKernelProtCpuRw, 0),
	            "KernelMemoryPoolCommit(exhausted)");
	ExpectRange(test, Query(test, base + commit_len), base + commit_len,
	            base + SceKernelMemoryPoolReserveLen, 0, 0, 0, 1, 0);

	*reinterpret_cast<uint64_t*>(base) = 0x465241474d454e54ull; // "FRAGMENT"
	*reinterpret_cast<uint64_t*>(base + SceKernelMemoryPoolCommitLen) = 0x504f4f4c50414745ull;
	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, commit_len, 0),
	        "KernelMemoryPoolDecommit(fragmented)");

	Libs::LibKernel::Memory::KernelMemoryPoolBlockStats stats {};
	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolGetBlockStats(&stats, sizeof(stats)),
	        "KernelMemoryPoolGetBlockStats(fragmented decommit)");
	Check(test, stats.available_flushed_blocks == 2 && stats.allocated_flushed_blocks == 0,
	      "fragmented decommit did not restore both pool pages");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(arena, commit_len, SceKernelMtypeC,
	                                                        SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit(fragmented recommit)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, commit_len),
	        "KernelMunmap(fragmented commit)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + commit_len,
	                                              SceKernelMemoryPoolReserveLen - commit_len),
	        "KernelMunmap(fragmented reserve remainder)");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(first_pool,
	                                                           SceKernelMemoryPoolCommitLen),
	        "KernelReleaseDirectMemory(first pool)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(second_pool,
	                                                           SceKernelMemoryPoolCommitLen),
	        "KernelReleaseDirectMemory(second pool)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_gap,
	                                                           SceKernelMemoryPoolCommitLen),
	        "KernelReleaseDirectMemory(gap)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestMemoryPoolMultiRangeDecommit() {
	const char* test        = "MemoryPoolMultiRangeDecommit";
	const auto  expand_len  = SceKernelMemoryPoolCommitLen * 2;
	int64_t     pool_offset = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolExpand(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), expand_len,
	            SceKernelMemoryPoolAlignment, &pool_offset),
	        "KernelMemoryPoolExpand");

	void* arena = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolReserve(nullptr, SceKernelMemoryPoolReserveLen,
	                                                         0, 0, &arena),
	        "KernelMemoryPoolReserve");
	const auto base = reinterpret_cast<uint64_t>(arena);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(arena, SceKernelMemoryPoolCommitLen,
	                                                        SceKernelMtypeC, SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit(read-write)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(
	            reinterpret_cast<void*>(base + SceKernelMemoryPoolCommitLen),
	            SceKernelMemoryPoolCommitLen, SceKernelMtypeC, SceKernelProtCpuRead, 0),
	        "KernelMemoryPoolCommit(read-only)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, expand_len, 0),
	        "KernelMemoryPoolDecommit(different protections)");
	const auto decommitted = Query(test, base);
	Check(test,
	      decommitted.start <= base && decommitted.end >= base + expand_len &&
	          decommitted.is_pooled == 1 && decommitted.is_committed == 0,
	      "multi-range decommit did not restore the reserved pool span");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(
	            reinterpret_cast<void*>(base + SceKernelMemoryPoolCommitLen),
	            SceKernelMemoryPoolCommitLen, SceKernelMtypeC, SceKernelProtCpuRead, 0),
	        "KernelMemoryPoolCommit(mixed span)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, expand_len, 0),
	        "KernelMemoryPoolDecommit(reserved and committed span)");
	const auto mixed_decommitted = Query(test, base);
	Check(test,
	      mixed_decommitted.start <= base && mixed_decommitted.end >= base + expand_len &&
	          mixed_decommitted.is_pooled == 1 && mixed_decommitted.is_committed == 0,
	      "mixed reserved/committed decommit left committed pages behind");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(arena, SceKernelMemoryPoolCommitLen,
	                                                        SceKernelMtypeC, SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit(preflight prefix)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(base + SceKernelMemoryPoolCommitLen,
	                                              SceKernelMemoryPoolCommitLen),
	        "KernelMunmap(preflight tail reserve)");
	void* flexible_tail = reinterpret_cast<void*>(base + SceKernelMemoryPoolCommitLen);
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(
	            &flexible_tail, SceKernelMemoryPoolCommitLen, SceKernelProtCpuRead,
	            SceKernelMapFixed, "pool_invalid_tail"),
	        "KernelMapNamedFlexibleMemory(preflight tail)");
	CheckFailed(test, Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, expand_len, 0),
	            "KernelMemoryPoolDecommit(invalid tail)");
	ExpectRange(test, Query(test, base), base, base + SceKernelMemoryPoolCommitLen,
	            SceKernelProtCpuRw, 0, 0, 1, 1);
	ExpectRange(test, Query(test, base + SceKernelMemoryPoolCommitLen),
	            base + SceKernelMemoryPoolCommitLen, base + expand_len, SceKernelProtCpuRead, 1, 0,
	            0, 1, "pool_invalid_tail");

	CheckOk(
	    test,
	    Libs::LibKernel::Memory::KernelMemoryPoolDecommit(arena, SceKernelMemoryPoolCommitLen, 0),
	    "KernelMemoryPoolDecommit(preflight prefix cleanup)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelMemoryPoolReserveLen),
	        "KernelMunmap(pool reserve cleanup)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(pool_offset, expand_len),
	        "KernelReleaseDirectMemory(pool expansion)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestMemoryPoolCommitDecommitQueryFlags() {
	const char* test        = "MemoryPoolCommitDecommitQueryFlags";
	void*       addr        = nullptr;
	int64_t     pool_offset = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolExpand(
	            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
	            SceKernelMemoryPoolCommitLen, SceKernelMemoryPoolAlignment, &pool_offset),
	        "KernelMemoryPoolExpand");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolReserve(nullptr, SceKernelMemoryPoolReserveLen,
	                                                         0, 0, &addr),
	        "KernelMemoryPoolReserve");
	const auto base = reinterpret_cast<uint64_t>(addr);

	const auto reserved    = Query(test, base);
	const bool reserved_ok = reserved.start <= base && base < reserved.end &&
	                         reserved.is_pooled == 1 && reserved.is_committed == 0;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolCommit(reinterpret_cast<void*>(base),
	                                                        SceKernelMemoryPoolCommitLen,
	                                                        SceKernelMtypeC, SceKernelProtCpuRw, 0),
	        "KernelMemoryPoolCommit");
	const auto committed    = Query(test, base);
	const bool committed_ok = committed.start == base &&
	                          committed.end == base + SceKernelMemoryPoolCommitLen &&
	                          committed.protection == SceKernelProtCpuRw &&
	                          committed.is_pooled == 1 && committed.is_committed == 1;

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMemoryPoolDecommit(reinterpret_cast<void*>(base),
	                                                          SceKernelMemoryPoolCommitLen, 0),
	        "KernelMemoryPoolDecommit");
	const auto decommitted    = Query(test, base);
	const bool decommitted_ok = decommitted.start <= base && base < decommitted.end &&
	                            decommitted.is_pooled == 1 && decommitted.is_committed == 0;

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelMemoryPoolReserveLen),
	        "KernelMunmap(pool reserve cleanup)");
	Check(test, reserved_ok, "pool reserve should query as pooled/uncommitted");
	Check(test, committed_ok, "pool commit should query as pooled/committed");
	Check(test, decommitted_ok, "pool decommit should return to pooled/uncommitted");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(pool_offset,
	                                                           SceKernelMemoryPoolCommitLen),
	        "KernelReleaseDirectMemory(pool expansion)");

	std::printf("[host]    %-48s ok\n", test);
}

void TestProgramMemoryAllocationAndProtection() {
	const char* test = "ProgramMemoryAllocationAndProtection";
	const auto  size = SceKernelPageSize * 3;
	const auto  base = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x900000000, size, Common::VirtualMemory::Mode::ReadWrite, "program_test");
	Check(test, base != 0, "program guest allocation failed");
	Check(test, Libs::LibKernel::Memory::TestGuestAddressRangeIsOwned(base, size),
	      "program allocation escaped the guest owner");
	ExpectRange(test, Query(test, base), base, base + size,
	            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "program_test");

	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestMemory(base, SceKernelPageSize,
	                                                  Common::VirtualMemory::Mode::Read),
	      "ProtectGuestMemory(first page) failed");
	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, SceKernelProtCpuRead, 0, 0,
	            0, 1, "program_test");

	Common::VirtualMemory::Mode previous_mode = Common::VirtualMemory::Mode::NoAccess;
	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestMemory(
	          base, SceKernelPageSize, Common::VirtualMemory::Mode::ReadWrite, &previous_mode),
	      "ProtectGuestMemory(tracked restore) failed");
	Check(test, previous_mode == Common::VirtualMemory::Mode::Read,
	      "semantic guest protection did not preserve its tracked old mode");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMprotect(
	            reinterpret_cast<void*>(base + SceKernelPageSize - 0x10), 0x20,
	            SceKernelProtCpuRead | SceKernelProtCpuRw),
	        "KernelMprotect(program split span)");
	ExpectRange(test, Query(test, base), base, base + size,
	            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "program_test");

	Check(test,
	      Libs::LibKernel::Memory::ProtectGuestMemory(
	          base + SceKernelPageSize * 2, SceKernelPageSize, Common::VirtualMemory::Mode::Read),
	      "ProtectGuestMemory(last page) failed");
	ExpectRange(test, Query(test, base + SceKernelPageSize * 2), base + SceKernelPageSize * 2,
	            base + size, SceKernelProtCpuRead, 0, 0, 0, 1, "program_test");

	Check(test, Libs::LibKernel::Memory::FreeGuestMemory(base, size), "program guest free failed");
	ExpectUnmapped(test, base);

	std::printf("[host]    %-48s ok\n", test);
}

void TestModuleRelocationUsesWritableHostMapping() {
	const char* test = "ModuleRelocationUsesWritableHostMapping";
	Check(test, Loader::TestModuleRelocationUsesWritableHostMapping(),
	      "module relocation did not retain writable host memory and semantic guest protection");
	std::printf("[host]    %-48s ok\n", test);
}

#if defined(__linux__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
volatile sig_atomic_t g_rsqrt_traps = 0;

bool EmulateReciprocalSquareRootContext(void* context) {
	const auto host_mxcsr = _mm_getcsr();
	_mm_setcsr(0x7fe1); // Exercise the handler under a distinct rounding mode and raised flags.
	const bool emulated        = Loader::X64InstructionEmulator::TryEmulate(context);
	const bool preserved_mxcsr = _mm_getcsr() == 0x7fe1;
	_mm_setcsr(host_mxcsr);
	if (!emulated || !preserved_mxcsr) {
		return false;
	}
	g_rsqrt_traps = g_rsqrt_traps + 1;
	return true;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
LONG CALLBACK ReciprocalSquareRootHandler(EXCEPTION_POINTERS* exception) {
	if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION ||
	    !EmulateReciprocalSquareRootContext(exception->ContextRecord)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}
#else
void ReciprocalSquareRootHandler(int, siginfo_t*, void* context) {
	if (!EmulateReciprocalSquareRootContext(context)) {
		_exit(190);
	}
}
#endif

void TestPackedReciprocalSquareRoot() {
	const char* test = "PackedReciprocalSquareRoot";
	constexpr uint64_t code_size = 0x4000;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	constexpr uint64_t allocation_size = code_size * 2;
#else
	constexpr uint64_t allocation_size = code_size;
#endif
	const auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x902000000, allocation_size, Common::VirtualMemory::Mode::ExecuteReadWrite, "rsqrt_test");
	Check(test, mapping != 0, "failed to allocate instruction test code");
	struct RestoreState {
		uint64_t mapping;
		uint64_t size;
		uint32_t mxcsr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		void* handler = nullptr;
#else
		struct sigaction previous {};
		bool installed = false;
#endif
		~RestoreState() {
			_mm_setcsr(mxcsr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (handler != nullptr) {
				RemoveVectoredExceptionHandler(handler);
			}
			Loader::UnregisterRedZonePatchModule(reinterpret_cast<void*>(mapping));
#else
			if (installed) {
				sigaction(SIGILL, &previous, nullptr);
			}
#endif
			Libs::LibKernel::Memory::FreeGuestMemory(mapping, size);
		}
	} restore {mapping, allocation_size, _mm_getcsr()};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	restore.handler = AddVectoredExceptionHandler(1, ReciprocalSquareRootHandler);
	Check(test, restore.handler != nullptr, "failed to install instruction handler");
#else
	struct sigaction action {};
	action.sa_sigaction = ReciprocalSquareRootHandler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	restore.installed = sigaction(SIGILL, &action, &restore.previous) == 0;
	Check(test, restore.installed, "failed to install instruction handler");
#endif
	using GuestFunction = void(KYTY_SYSV_ABI*)(const uint32_t*, uint32_t*);
	const auto function = reinterpret_cast<GuestFunction>(mapping);
	const auto refine = [](float estimate) {
		return (estimate * 0.5f) * std::fma(-estimate, estimate, 3.0f);
	};
	constexpr uint64_t red_zone_sentinel = 0x1122334455667788ull;
	std::array<uint32_t, 16> input {};
	std::array<uint32_t, 48> output {};
	input.fill(0x3f800000);
	for (const auto registers: {std::array {2, 1}, std::array {10, 9}, std::array {1, 1}}) {
		const auto destination = registers[0];
		const auto source = registers[1];
		input[0] = 0x3f800000;
		uint32_t expected = 0x3f800000;
		if (source == 9) {
			input[0] = 0x40800000;
			expected = 0x3f000000;
		}
		Xbyak::CodeGenerator code(code_size, reinterpret_cast<void*>(mapping));
		if (source == 9) {
			code.vmovups(Xbyak::Ymm(1), code.ptr[code.rdi + 32]);
		}
		code.vmovups(Xbyak::Ymm(source), code.ptr[code.rdi]);
		if (destination != source) {
			code.vmovups(Xbyak::Ymm(destination), code.ptr[code.rdi + 32]);
		}
		for (uint32_t offset = 8; offset <= 128; offset += 8) {
			code.mov(code.rax, red_zone_sentinel ^ offset);
			code.mov(code.qword[code.rsp - offset], code.rax);
		}
		code.vrsqrtps(Xbyak::Xmm(destination), Xbyak::Xmm(source));
		for (uint32_t offset = 8; offset <= 128; offset += 8) {
			code.mov(code.rax, code.qword[code.rsp - offset]);
			code.mov(code.qword[code.rsi + 64 + offset - 8], code.rax);
		}
		code.vmovups(code.ptr[code.rsi], Xbyak::Ymm(destination));
		code.vmovups(code.ptr[code.rsi + 32], Xbyak::Ymm(source));
		code.vzeroupper();
		code.ret();
#if defined(__linux__)
		const std::vector<uint8_t> original(code.getCode(), code.getCurr());
#endif
		Check(test, Common::VirtualMemory::FlushInstructionCache(mapping, code.getSize()),
		      "failed to flush generated instruction test code");
		function(input.data(), output.data());
		const float native = std::bit_cast<float>(output[0]);
		const auto red_zone_intact = [&] {
			for (uint32_t offset = 8; offset <= 128; offset += 8) {
				const auto index = 16 + (offset - 8) / sizeof(uint32_t);
				const auto value = static_cast<uint64_t>(output[index]) |
				                   (static_cast<uint64_t>(output[index + 1]) << 32);
				if (value != (red_zone_sentinel ^ offset)) {
					return false;
				}
			}
			return true;
		};
		Check(test, red_zone_intact(), "native instruction corrupted the guest red zone");
		Check(test, std::isfinite(native) && std::abs(native - std::bit_cast<float>(expected)) < 0.001f,
		      "native reciprocal root is outside its error bound");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		Loader::RegisterRedZonePatchModule(reinterpret_cast<void*>(mapping), code_size,
		                                   reinterpret_cast<void*>(mapping + code_size), code_size);
		const std::array<uintptr_t, 1> function_starts {mapping};
		// The extended-register case also relocates ordinary memory accesses while
		// red-zone data is live, exercising both enabled patchers in one function.
		const bool protect_memory = source == 9;
		const auto patched = Loader::PatchGuestInstructions(
		    mapping, code.getSize(), function_starts, protect_memory, true);
		Check(test, patched.reciprocal_sqrt_instruction_count == 1 &&
		                patched.unrelocatable_memory_instruction_count == 0 &&
		                (protect_memory ? patched.patched_memory_instruction_count > 0
		                                : patched.patched_memory_instruction_count == 0),
		      "instruction pass lost reciprocal root or memory patch coverage");
#else
		Check(test, Loader::X64InstructionEmulator::PatchReciprocalSquareRoots(mapping, code.getSize()) == 1,
		      "instruction pass did not patch exactly one packed reciprocal root");
		const auto* patched = reinterpret_cast<const uint8_t*>(mapping);
		size_t changed = 0;
		for (size_t i = 0; i < original.size(); ++i) {
			changed += original[i] != patched[i];
		}
		Check(test, changed == 1, "instruction pass changed unrelated code bytes");
		Check(test, Common::VirtualMemory::FlushInstructionCache(mapping, code.getSize()),
		      "failed to flush patched instruction test code");
#endif
		const auto before = g_rsqrt_traps;
		function(input.data(), output.data());
		Check(test, g_rsqrt_traps == before + 1, "patched instruction did not execute its handler");
		Check(test, red_zone_intact(), "patched instruction corrupted the guest red zone");
		Check(test, output[0] == expected, "patched instruction read or wrote the wrong register");
		if (destination != source) {
			Check(test, std::equal(input.begin(), input.begin() + 4, output.begin() + 8),
			      "source lower XMM lanes were corrupted");
		}
		if (source != 9) {
			Check(test, refine(refine(std::bit_cast<float>(output[0]))) == 1.0f,
			      "identity quaternion normalization drifted below one");
		}
		for (size_t i = 0; i < 4; ++i) {
			Check(test, output[4 + i] == 0, "128-bit VEX destination retained upper YMM lanes");
			const auto expected_upper = destination == source ? 0u : input[4 + i];
			Check(test, output[12 + i] == expected_upper, "source upper YMM lanes were corrupted");
		}
		std::printf("[host]    rsqrt xmm%d,xmm%d native=%08x patched=%08x\n",
		            destination, source, std::bit_cast<uint32_t>(native), output[0]);
		if (destination != 2) {
			continue;
		}
		constexpr std::array<std::array<uint32_t, 8>, 4> cases {{
		    {0x00000000, 0x80000000, 0x00000001, 0x80000001,
		     0x7f800000, 0xff800000, 0x7f800000, 0xff800000},
		    {0x7f800000, 0xff800000, 0xbf800000, 0x7fc12345,
		     0x00000000, 0xffc00000, 0xffc00000, 0x7fc12345},
		    {0x7f812345, 0xff812345, 0x40800000, 0x40000000,
		     0x7fc12345, 0xffc12345, 0x3f000000, 0x3f3504f3},
		    {0x00800000, 0x7f7fffff, 0x3f800000, 0x41800000,
		     0x5f000000, 0x1f800000, 0x3f800000, 0x3e800000},
		}};
		for (const auto& values: cases) {
			std::copy_n(values.begin(), 4, input.begin());
			for (uint32_t controls = 0; controls < 8; ++controls) {
				const uint32_t mxcsr = 0x1fa1u | ((controls & 3u) << 13u) | ((controls & 4u) << 4u);
				_mm_setcsr(mxcsr);
				function(input.data(), output.data());
				const auto result_mxcsr = _mm_getcsr();
				_mm_setcsr(restore.mxcsr);
				Check(test, red_zone_intact(), "special-value trap corrupted the guest red zone");
				Check(test, result_mxcsr == mxcsr, "instruction changed MXCSR controls or exception flags");
				Check(test, std::equal(output.begin(), output.begin() + 4, values.begin() + 4),
				      "special values or round-independent reciprocal roots differ from ISA semantics");
			}
		}
		input.fill(0x3f800000);
	}
#if defined(__linux__)
	std::array<uint8_t, 16> unknown {0x0f, 0x0b};
	ucontext_t context {};
	context.uc_mcontext.gregs[REG_RIP] = reinterpret_cast<greg_t>(unknown.data());
	Check(test, !Loader::X64InstructionEmulator::TryEmulate(&context), "unrecognized UD2 was swallowed");
	_libc_fpstate fpstate {};
	context.uc_mcontext.fpregs = &fpstate;
	const auto emulate = [&](std::array<uint8_t, 16> instruction, size_t length) {
		context.uc_mcontext.gregs[REG_RIP] = reinterpret_cast<greg_t>(instruction.data());
		Check(test, Loader::X64InstructionEmulator::TryEmulate(&context), "existing instruction emulation was rejected");
		Check(test, context.uc_mcontext.gregs[REG_RIP] == reinterpret_cast<greg_t>(instruction.data() + length),
		      "existing instruction emulation advanced RIP incorrectly");
	};
	fpstate._xmm[8].element[0] = 0x1234;
	fpstate._xmm[8].element[2] = 0xdeadbeef;
	emulate({0x66, 0x41, 0x0f, 0x78, 0xc0, 8, 4}, 7); // extrq xmm8, 8, 4
	Check(test, fpstate._xmm[8].element[0] == 0x23 && fpstate._xmm[8].element[2] == 0,
	      "SSE4a extraction lost its extended register or upper-half semantics");
	fpstate._xmm[1].element[0] = 0x1234;
	fpstate._xmm[8].element[0] = 0xffffc4c8;
	emulate({0x66, 0x41, 0x0f, 0x79, 0xc8}, 5); // observed fault: extrq xmm1, xmm8
	Check(test, fpstate._xmm[1].element[0] == 0x23 && fpstate._xmm[8].element[0] == 0xffffc4c8,
	      "register EXTRQ lost its source controls, ignored bits, or separate destination");
	fpstate._xmm[11].element[0] = 0x89abcdef;
	fpstate._xmm[11].element[1] = 0x01234567;
	fpstate._xmm[0].element[0] = 0x2010;
	emulate({0x66, 0x44, 0x0f, 0x79, 0xd8}, 5); // extrq xmm11, xmm0
	Check(test, fpstate._xmm[11].element[0] == 0x4567 && fpstate._xmm[11].element[1] == 0,
	      "register EXTRQ lost its extended destination or 64-bit extraction");
	fpstate._xmm[8].element[0] = 0x89abcdef;
	fpstate._xmm[8].element[1] = 0x01234567;
	fpstate._xmm[9].element[0] = 0;
	emulate({0x66, 0x45, 0x0f, 0x79, 0xc1}, 5); // extrq xmm8, xmm9
	Check(test, fpstate._xmm[8].element[0] == 0x89abcdef && fpstate._xmm[8].element[1] == 0x01234567,
	      "register EXTRQ did not interpret zero length as 64 bits");
	fpstate._xmm[3].element[0] = 0xab0408;
	emulate({0x66, 0x0f, 0x79, 0xdb}, 4); // extrq xmm3, xmm3
	Check(test, fpstate._xmm[3].element[0] == 0x40,
	      "register EXTRQ overwrote aliased controls before reading them");
	fpstate._xmm[8].element[0] = 0x1111;
	fpstate._xmm[8].element[2] = 0xdeadbeef;
	fpstate._xmm[9].element[0] = 0xab;
	emulate({0xf2, 0x45, 0x0f, 0x78, 0xc1, 8, 4}, 7); // insertq xmm8, xmm9, 8, 4
	Check(test, fpstate._xmm[8].element[0] == 0x1ab1 && fpstate._xmm[8].element[2] == 0xdeadbeef,
	      "SSE4a insertion corrupted its destination lanes");
	std::array<uint32_t, 8> sha_source {0, 0, 0, 0, 5, 6, 7, 8};
	context.uc_mcontext.gregs[REG_RDI] = reinterpret_cast<greg_t>(sha_source.data());
	for (uint32_t lane = 0; lane < 4; ++lane) {
		fpstate._xmm[8].element[lane] = lane + 1;
	}
	emulate({0x44, 0x0f, 0x38, 0xc9, 0x47, 0x10}, 6); // sha1msg1 xmm8, [rdi+16]
	constexpr std::array<uint32_t, 4> sha_expected {6, 10, 2, 6};
	Check(test, std::equal(sha_expected.begin(), sha_expected.end(), fpstate._xmm[8].element),
	      "SHA emulation lost its memory operand or extended destination register");
	emulate({0x0f, 0x01, 0xfa}, 3); // monitorx
#endif
	std::printf("[host]    %-48s ok\n", test);
}
#endif

#if defined(__x86_64__) || defined(_M_X64)
constexpr int32_t FiberErrorState = -2141650938; // SCE_FIBER_ERROR_STATE
constexpr int32_t FiberErrorPermission = -2141650939;

struct FiberRoundTrip {
	Libs::Fiber::FiberObject* first;
	Libs::Fiber::FiberObject* second;
	std::atomic<bool> running {false};
	std::atomic<bool> release {false};
	int first_errors = 0;
	int second_errors = 0;
	uint32_t first_visits = 0;
	uint32_t second_visits = 0;
	uint64_t first_arg = 0;
	uint64_t second_arg = 0;
};

[[noreturn]] void KYTY_SYSV_ABI FirstFiberEntry(uint64_t initial, uint64_t arg) {
	auto& data = *reinterpret_cast<FiberRoundTrip*>(initial);
	for (;;) {
		Libs::Fiber::FiberObject* self = nullptr;
		data.first_errors |= Libs::Fiber::FiberGetSelf(&self);
		data.first_errors |= self != data.first;
		data.first_errors |= Libs::Fiber::FiberSwitch(self, 0, nullptr) != FiberErrorState;
		if (data.first_errors != 0) {
			__builtin_trap();
		}
		++data.first_visits;
		data.first_arg = arg;
		if (arg == 20) {
			data.running.store(true, std::memory_order_release);
			while (!data.release.load(std::memory_order_acquire)) {
				asm volatile("pause");
			}
		}
		data.first_errors |= Libs::Fiber::FiberSwitch(data.second, arg + 1, &arg);
		if (data.first_errors != 0) {
			__builtin_trap();
		}
		data.first_errors |= Libs::Fiber::FiberReturnToThread(arg + 1, &arg);
	}
}

[[noreturn]] void KYTY_SYSV_ABI SecondFiberEntry(uint64_t initial, uint64_t arg) {
	auto& data = *reinterpret_cast<FiberRoundTrip*>(initial);
	for (;;) {
		Libs::Fiber::FiberObject* self = nullptr;
		data.second_errors |= Libs::Fiber::FiberGetSelf(&self);
		data.second_errors |= self != data.second;
		if (data.second_errors != 0) {
			__builtin_trap();
		}
		++data.second_visits;
		data.second_arg = arg;
		data.second_errors |= Libs::Fiber::FiberSwitch(data.first, arg + 1, &arg);
		if (data.second_errors != 0) {
			__builtin_trap();
		}
	}
}

void TestSmallFiberStacksAndMigration() {
	const char* test = "SmallFiberStacksAndMigration";
	// 256-byte objects, 8-byte object alignment, 16-byte context
	// alignment, and a 512-byte minimum context. The game supplies 2048 bytes.
	for (const size_t stack_size: {512u, 2048u}) {
		alignas(8) std::array<uint8_t, 256> first_object {};
		alignas(8) std::array<uint8_t, 256> second_object {};
		constexpr size_t GuardSize = 4096;
		alignas(16) std::array<uint8_t, GuardSize + 2048 + 64> first_stack;
		alignas(16) std::array<uint8_t, GuardSize + 2048 + 64> second_stack;
		first_stack.fill(0xa5);
		second_stack.fill(0xa5);
		FiberRoundTrip data {reinterpret_cast<Libs::Fiber::FiberObject*>(first_object.data()),
		                     reinterpret_cast<Libs::Fiber::FiberObject*>(second_object.data())};
		CheckOk(test, Libs::Fiber::FiberInitialize(data.first, "first", FirstFiberEntry,
		    reinterpret_cast<uint64_t>(&data), first_stack.data() + GuardSize, stack_size,
		    nullptr, 0x0a000000), "initialize first fiber");
		CheckOk(test, Libs::Fiber::FiberInitialize(data.second, "second", SecondFiberEntry,
		    reinterpret_cast<uint64_t>(&data), second_stack.data() + GuardSize, stack_size,
		    nullptr, 0x0a000000), "initialize second fiber");
		const auto thread_context_is_clear = [&] {
			Libs::Fiber::FiberObject* self = data.first;
			return Libs::Fiber::FiberGetSelf(&self) == OK && self == nullptr &&
			       Libs::Fiber::FiberSwitch(data.first, 0, nullptr) == FiberErrorPermission &&
			       Libs::Fiber::FiberReturnToThread(0, nullptr) == FiberErrorPermission;
		};
		Check(test, thread_context_is_clear(), "thread retained a fiber context before its first run");
		uint64_t first_base = 0;
		uint64_t second_base = 0;
		std::memcpy(&first_base, first_stack.data() + GuardSize, sizeof(first_base));
		std::memcpy(&second_base, second_stack.data() + GuardSize, sizeof(second_base));
		const auto check_stacks = [&] {
			for (const auto* stack: {&first_stack, &second_stack}) {
				const auto is_guard = [](uint8_t byte) { return byte == 0xa5; };
				Check(test, std::all_of(stack->begin(), stack->begin() + GuardSize, is_guard) &&
				                std::all_of(stack->begin() + GuardSize + stack_size, stack->end(), is_guard),
				      "fiber wrote outside its supplied stack");
			}
			Check(test, std::memcmp(&first_base, first_stack.data() + GuardSize, sizeof(first_base)) == 0 &&
			                std::memcmp(&second_base, second_stack.data() + GuardSize, sizeof(second_base)) == 0,
			      "fiber overwrote the bottom of its stack");
		};
		uint64_t returned = 0;
		CheckOk(test, Libs::Fiber::FiberRun(data.first, 10, &returned), "run first fiber");
		Check(test, returned == 13 && data.first_visits == 1 && data.second_visits == 1 &&
		                data.first_arg == 10 && data.second_arg == 11,
		      "switch/return arguments were not preserved");
		Check(test, thread_context_is_clear(), "run retained its thread context after returning");
		check_stacks();

		std::atomic<bool> done {false};
		int worker_result = -1;
		int worker_repeat_result = -1;
		bool worker_context_clear = false;
		uint64_t worker_returned = 0;
		uint64_t worker_repeat_returned = 0;
		std::thread worker([&] {
			worker_context_clear = thread_context_is_clear();
			worker_result = Libs::Fiber::FiberRun(data.first, 20, &worker_returned);
			worker_context_clear = thread_context_is_clear() && worker_context_clear;
			worker_repeat_result = Libs::Fiber::FiberRun(data.first, 30, &worker_repeat_returned);
			worker_context_clear = thread_context_is_clear() && worker_context_clear;
			done.store(true, std::memory_order_release);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!data.running.load(std::memory_order_acquire) &&
		       !done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::yield();
		}
		const bool running = data.running.load(std::memory_order_acquire);
		Libs::Fiber::FiberObject* main_self = data.first;
		const int main_self_result = Libs::Fiber::FiberGetSelf(&main_self);
		const int busy_run = running ? Libs::Fiber::FiberRun(data.first, 30, nullptr) : 0;
		const int busy_finalize = running ? Libs::Fiber::FiberFinalize(data.first) : 0;
		data.release.store(true, std::memory_order_release);
		worker.join();
		Check(test, thread_context_is_clear() && main_self_result == OK && main_self == nullptr,
		      "migrated fiber changed the original thread's current fiber");
		Check(test, running && busy_run == FiberErrorState && busy_finalize == FiberErrorState,
		      "running fiber was not exclusively owned");
		CheckOk(test, worker_result, "resume fiber on another thread");
		CheckOk(test, worker_repeat_result, "repeat run on another thread");
		Check(test, worker_context_clear, "worker retained a context outside a fiber run");
		Check(test, worker_returned == 23 && worker_repeat_returned == 33 &&
		                data.first_visits == 3 && data.second_visits == 3 &&
		                data.first_arg == 30 && data.second_arg == 31 &&
		                data.first_errors == 0 && data.second_errors == 0,
		      "migration lost fiber identity, arguments, or resumable contexts");
		CheckOk(test, Libs::Fiber::FiberRun(data.first, 40, &returned), "migrate fiber back to original thread");
		Check(test, returned == 43 && data.first_visits == 4 && data.second_visits == 4 &&
		                data.first_arg == 40 && data.second_arg == 41 &&
		                data.first_errors == 0 && data.second_errors == 0,
		      "return migration reused an expired thread context");
		Check(test, thread_context_is_clear(), "repeated run retained its thread context after returning");
		check_stacks();
		CheckOk(test, Libs::Fiber::FiberFinalize(data.first), "finalize first suspended fiber");
		CheckOk(test, Libs::Fiber::FiberFinalize(data.second), "finalize second suspended fiber");
		std::printf("[host]    %s stack=%zu ok\n", test, stack_size);
	}
}
#endif

} // namespace

int main(int argc, char** argv) {
	InitSubsystems();
#if defined(__x86_64__) || defined(_M_X64)
	if (argc == 2 && std::strcmp(argv[1], "--fiber-only") == 0) {
		RunTest(TestSmallFiberStacksAndMigration);
		return g_failed_tests == 0 ? 0 : 1;
	}
#endif
#if defined(__linux__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (argc == 2 && std::strcmp(argv[1], "--rsqrt-only") == 0) {
		RunTest(TestPackedReciprocalSquareRoot);
		return g_failed_tests == 0 ? 0 : 1;
	}
#endif
	if (argc == 2 && std::strcmp(argv[1], "--red-zone-patcher-only") == 0) {
		RunTest(TestWindowsGuestRedZoneStaticPatcher);
		return g_failed_tests == 0 ? 0 : 1;
	}

#if defined(__x86_64__) || defined(_M_X64)
	RunTest(TestSmallFiberStacksAndMigration);
#endif
#if defined(__linux__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	RunTest(TestPackedReciprocalSquareRoot);
#endif
	RunTest(TestWindowsGuestRedZoneStaticPatcher);
	RunTest(TestProsperoArgumentAndInfoSizeContracts);
	RunTest(TestGuestAddressSpaceOwnsReservationsBeforeBacking);
	RunTest(TestPrtBackingReadPreservesSparseResidency);
	RunTest(TestPrtReadDuringDirectCommit);
	RunTest(TestGuestAddressSpaceHasNoFixedFallback);
	RunTest(TestGuestFreeRangeSearchDoesNotUnderflow);
	RunTest(TestFlexibleMemoryCapacityIsBootFixed);
	RunTest(TestFlexibleMemoryUsesSharedBacking);
	RunTest(TestFlexibleDmemCompatAndAlignmentFlags);
	RunTest(TestFlexibleNoCoalescePreservesBoundaries);
	RunTest(TestFlexibleMemoryReuseIsZeroFilled);
	RunTest(TestSmallerFlexibleMapReusesReleasedHole);
	RunTest(TestGuestStackUsesPrivateOwnerMemoryAndCache);
	RunTest(TestMainEntryUsesGuestStackAndDisablesHostChecks);
	RunTest(TestFragmentedBackingUnmapRollback);
	RunTest(TestRuntimeMemoryOwnerLifecycle);
	RunTest(TestFlexibleMapQueryAndWholeMunmap);
	RunTest(TestPartialFlexibleMunmapAndFindNext);
	RunTest(TestReserveMapFixedAndNoOverwrite);
	RunTest(TestFixedNoOverwriteRejectsReservedRange);
	RunTest(TestReleasedReserveCanBeReused);
	RunTest(TestMunmapAcrossAdjacentFlexibleMappings);
	RunTest(TestDirectMapQueryOffsetAndPartialMunmap);
	RunTest(TestDirectPartialProtectUnmapPreservesNeighbors);
#if defined(__linux__)
	RunTest(TestPartialUnmapPreservesHostPermissions);
#endif
	RunTest(TestDirectMapValidationBeforeOwnerMutation);
	RunTest(TestDirectReleaseRollbackRestoresOwnerMapping);
	RunTest(TestDirectReleaseContracts);
	RunTest(TestNonzeroDirectOffsetAliasesSharedBacking);
	RunTest(TestDirectMapAcrossContiguousAllocations);
	RunTest(TestDirectPhysicalFreeRangeReuseAndCoalescing);
	RunTest(TestDirectAlignmentStaysWithinSearchRange);
	RunTest(TestDefaultDirectMapUsesSystemAddressRange);
	RunTest(TestLargeDirectMapAliasesAcrossChunks);
	RunTest(TestOnDemandPhysicalMemoryCommitment);
	RunTest(TestHintlessDirectMapUsesCanonicalGuestBase);
	RunTest(TestDirectMemoryContentPersistsAcrossRemap);
	RunTest(TestDirectMapUnmapReusesHostAddress);
	RunTest(TestFixedReserveReplacesPartialDirectMapping);
	RunTest(TestFixedReserveRollbackConsumesRestoredPlaceholder);
	RunTest(TestFixedReserveRollbackSkipsUntouchedChunks);
	RunTest(TestFixedReserveRangeAddRollbackKeepsPlaceholder);
	RunTest(TestLargeHintedReserveHostsSmallDirectMap);
	RunTest(TestMemoryPoolAlignmentContracts);
	RunTest(TestProsperoSampleMemoryPoolExpandCommit);
	RunTest(TestFragmentedMemoryPoolBacking);
	RunTest(TestMemoryPoolMultiRangeDecommit);
	RunTest(TestMemoryPoolCommitDecommitQueryFlags);
	RunTest(TestProgramMemoryAllocationAndProtection);
	RunTest(TestModuleRelocationUsesWritableHostMapping);

	if (g_failed_tests != 0) {
		std::printf("VirtualMemoryAllocationTests: %d case(s) failed\n", g_failed_tests);
		return 1;
	}

	std::printf("VirtualMemoryAllocationTests: all cases passed\n");
	return 0;
}
