#include "common/commonSubsystem.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

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

	static char  arg0[] = "virtual_memory_allocation_tests";
	static char* argv[] = {arg0};

	auto* slist  = Common::SubsystemsList::Instance();
	auto* core   = Common::CommonSubsystem::Instance();
	auto* config = Config::ConfigSubsystem::Instance();
	auto* log    = Log::LogSubsystem::Instance();
	auto* memory = Libs::LibKernel::Memory::MemorySubsystem::Instance();
	auto* thread = Common::ThreadsSubsystem::Instance();

	slist->SetArgs(1, argv);
	slist->Add(thread, {});
	slist->Add(core, {});
	slist->Add(config, {core});
	Check("InitSubsystems", slist->InitAll(false), "failed to initialize base subsystems");

	Config::ConfigOptions options;
	options.printf_direction = Config::OutputDirection::Silent;
	Config::Load(options);

	slist->Add(log, {core, config});
	Check("InitSubsystems", slist->InitAll(false), "failed to initialize logging subsystem");

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

	slist->Add(memory, {core, log, thread});
	Check("InitSubsystems", slist->InitAll(false), "failed to initialize memory subsystem");

	initialized = true;
}

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
	Check(test, (base & (0x10000 - 1u)) == 0, "SDK alignment flag was not honored");
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

	Common::VirtualMemory::Mode old_left {};
	Common::VirtualMemory::Mode old_right {};
	Check(test,
	      Common::VirtualMemory::Protect(base, SceKernelPageSize,
	                                     Common::VirtualMemory::Mode::ReadWrite, &old_left),
	      "could not inspect left-page protection");
	Check(test,
	      Common::VirtualMemory::Protect(base + SceKernelPageSize * 2, SceKernelPageSize,
	                                     Common::VirtualMemory::Mode::ReadWrite, &old_right),
	      "could not inspect right-page protection");
	Check(test, old_left == Common::VirtualMemory::Mode::ReadWrite,
	      "partial unmap changed the left neighbor protection");
	Check(test, old_right == Common::VirtualMemory::Mode::ReadWrite,
	      "partial unmap changed the right neighbor protection");
	*reinterpret_cast<uint64_t*>(base) = 0x4c45465450524f54ull; // "LEFTPROT"
	*reinterpret_cast<uint64_t*>(base + SceKernelPageSize * 2) =
	    0x5247485450524f54ull; // "RGHTPROT"

	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, size),
	        "KernelReleaseDirectMemory");
	ExpectUnmapped(test, base);
	ExpectUnmapped(test, base + SceKernelPageSize * 2);

	std::printf("[host]    %-48s ok\n", test);
}

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

} // namespace

int main() {
	InitSubsystems();

	RunTest(TestProsperoArgumentAndInfoSizeContracts);
	RunTest(TestGuestAddressSpaceOwnsReservationsBeforeBacking);
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
	RunTest(TestDirectMapValidationBeforeOwnerMutation);
	RunTest(TestDirectReleaseRollbackRestoresOwnerMapping);
	RunTest(TestDirectReleaseContracts);
	RunTest(TestNonzeroDirectOffsetAliasesSharedBacking);
	RunTest(TestDirectMapAcrossContiguousAllocations);
	RunTest(TestDirectPhysicalFreeRangeReuseAndCoalescing);
	RunTest(TestDirectAlignmentStaysWithinSearchRange);
	RunTest(TestDefaultDirectMapUsesSystemAddressRange);
	RunTest(TestLargeDirectMapAliasesAcrossChunks);
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
