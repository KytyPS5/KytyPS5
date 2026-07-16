#include "common/commonSubsystem.h"
#include "common/subsystems.h"
#include "common/threads.h"

#include "common/emulatorConfig.h"
#include "kernel/memory.h"
#include "libs/errno.h"
#include "common/logging/log.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using Libs::LibKernel::Memory::VirtualQueryInfo;

// Prospero ABI?
constexpr uint64_t SceKernelPageSize             = 0x4000;
constexpr int      SceKernelProtCpuRead          = 0x01;
constexpr int      SceKernelProtCpuRw            = 0x02;
constexpr int      SceKernelMapFixed             = 0x10;
constexpr int      SceKernelMapNoOverwrite       = 0x80;
constexpr int      SceKernelMapNoCoalesce        = 0x400000;
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
	const auto base = MapNamedFlexible(test, SceKernelPageSize * 3, SceKernelProtCpuRw, "prospero_part");

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
	      "MAP_FIXED|MAP_NO_OVERWRITE should reject an already reserved virtual range");

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
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(&addr, SceKernelPageSize * 4,
	                                                            SceKernelProtCpuRw, 0, phys_addr,
	                                                            SceKernelPageSize, "prospero_direct"),
	        "KernelMapNamedDirectMemory");
	const auto base = reinterpret_cast<uint64_t>(addr);
	const auto phys = static_cast<uint64_t>(phys_addr);
	void*      alias = nullptr;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &alias, SceKernelPageSize * 4, SceKernelProtCpuRw, 0, phys_addr,
	            SceKernelPageSize, "prospero_direct_alias"),
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
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(alias_base, SceKernelPageSize * 4),
	        "KernelMunmap(direct alias cleanup)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize * 4),
	        "KernelReleaseDirectMemory");

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
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize),
	        "KernelMunmap");

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
	        Libs::LibKernel::Memory::KernelReserveVirtualRange(
	            &reserve, SceKernelPageSize * 2, 0, SceKernelPageSize),
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
	            &right, SceKernelPageSize, SceKernelProtCpuRw, SceKernelMapFixed,
	            "adjacent_right"),
	        "KernelMapNamedFlexibleMemory(right)");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize * 2),
	        "KernelMunmap(adjacent mappings)");
	Check(test, AvailableFlexibleMemory(test) == baseline,
	      "multi-range unmap leaked flexible-memory budget");
	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, 0, 0, 0, 0, 0,
	            "adjacent_left");
	ExpectRange(test, Query(test, base + SceKernelPageSize), base + SceKernelPageSize,
	            base + SceKernelPageSize * 2, 0, 0, 0, 0, 0, "adjacent_right");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelPageSize * 2),
	        "KernelMunmap(restored reserve)");

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
	            &first_alias, SceKernelPageSize, SceKernelProtCpuRw, 0, second,
	            SceKernelPageSize, "prospero_nonzero_a"),
	        "KernelMapNamedDirectMemory(first alias)");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &second_alias, SceKernelPageSize, SceKernelProtCpuRw, 0, second,
	            SceKernelPageSize, "prospero_nonzero_b"),
	        "KernelMapNamedDirectMemory(second alias)");

	*reinterpret_cast<uint64_t*>(first_alias) = 0x4b59545931364b42ull; // "KYTY16KB"
	Check(test, *reinterpret_cast<const uint64_t*>(second_alias) == 0x4b59545931364b42ull,
	      "nonzero-offset mappings must share backing storage");

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(
	                  reinterpret_cast<uint64_t>(first_alias), SceKernelPageSize),
	        "KernelMunmap(first alias)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(
	                  reinterpret_cast<uint64_t>(second_alias), SceKernelPageSize),
	        "KernelMunmap(second alias)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(second, SceKernelPageSize),
	        "KernelReleaseDirectMemory(second)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(first, SceKernelPageSize),
	        "KernelReleaseDirectMemory(first)");

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
	const char*        test        = "DirectAlignmentStaysWithinSearchRange";
	constexpr int64_t  search_start = SceKernelPageSize * 2;
	constexpr uint64_t alignment    = SceKernelPageSize * 3;
	const auto         search_end   = Libs::LibKernel::Memory::KernelGetDirectMemorySize();

	int64_t phys_addr = -1;
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
	            search_start, search_end, SceKernelPageSize, alignment, SceKernelMtypeC,
	            &phys_addr),
	        "KernelAllocateDirectMemory(non-power-of-two alignment)");
	Check(test, phys_addr >= search_start, "aligned allocation escaped below search_start");
	Check(test, static_cast<uint64_t>(phys_addr) % alignment == 0,
	      "allocation did not honor the requested alignment");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelReleaseDirectMemory");

	constexpr size_t out_of_range_alignment = UINT64_MAX - (SceKernelPageSize - 1);
	phys_addr                               = -1;
	const int result = Libs::LibKernel::Memory::KernelAllocateDirectMemory(
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
	        Libs::LibKernel::Memory::KernelMapNamedDirectMemory(
	            &address, SceKernelPageSize, SceKernelProtCpuRw, 0, phys_addr,
	            SceKernelPageSize, "system_direct"),
	        "KernelMapNamedDirectMemory");
	Check(test, address != nullptr, "direct mapping returned null");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	constexpr uint64_t SystemManagedMin = 0x0000040000ull;
	constexpr uint64_t SystemManagedMax = 0x07fffeffffull;
	const auto         mapped           = reinterpret_cast<uint64_t>(address);
	Check(test, mapped >= SystemManagedMin &&
	                mapped + SceKernelPageSize - 1 <= SystemManagedMax,
	      "default direct mapping fell outside the system-managed host range");
#endif

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(address),
	                                             SceKernelPageSize),
	        "KernelMunmap");
	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestLargeDirectMapAliasesAcrossChunks() {
	const char*        test = "LargeDirectMapAliasesAcrossChunks";
	constexpr uint64_t size = 0x400000;
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

	auto* first  = static_cast<uint8_t*>(first_alias);
	auto* second = static_cast<uint8_t*>(second_alias);
	*reinterpret_cast<uint64_t*>(first)                = 0x1111222233334444ull;
	*reinterpret_cast<uint64_t*>(first + boundary - 8) = 0x5555666677778888ull;
	*reinterpret_cast<uint64_t*>(first + boundary)     = 0x9999aaaabbbbccccull;
	*reinterpret_cast<uint64_t*>(first + size - 8)     = 0xddddeeeeffff0001ull;
	Check(test, *reinterpret_cast<const uint64_t*>(second) == 0x1111222233334444ull &&
	                *reinterpret_cast<const uint64_t*>(second + boundary - 8) ==
	                    0x5555666677778888ull &&
	                *reinterpret_cast<const uint64_t*>(second + boundary) ==
	                    0x9999aaaabbbbccccull &&
	                *reinterpret_cast<const uint64_t*>(second + size - 8) ==
	                    0xddddeeeeffff0001ull,
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

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys_addr, SceKernelPageSize),
	        "KernelReleaseDirectMemory");

	std::printf("[host]    %-48s ok\n", test);
}

void TestLargeHintedReserveHostsSmallDirectMap() {
	const char* test = "LargeHintedReserveHostsSmallDirectMap";

	constexpr uint64_t arena_base = 0x1000000000ull;
	constexpr uint64_t arena_size  = 0x04000000ull;
	constexpr uint64_t window_size = 0x00200000ull;

	void* arena = reinterpret_cast<void*>(arena_base);
	CheckOk(test, Libs::LibKernel::Memory::KernelReserveVirtualRange(
	                  &arena, arena_size, 0, 0x200000),
	        "KernelReserveVirtualRange(arena)");
	const auto actual_arena = reinterpret_cast<uint64_t>(arena);
	Check(test, actual_arena >= arena_base && (actual_arena & (0x200000 - 1u)) == 0,
	      "large hinted reserve violated its search start or alignment");

	void* window = reinterpret_cast<void*>(arena_base);
	CheckOk(test, Libs::LibKernel::Memory::KernelReserveVirtualRange(
	                  &window, window_size, 0, SceKernelPageSize),
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

	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(
	                  reinterpret_cast<uint64_t>(mapped), SceKernelPageSize * 2),
	        "KernelMunmap(direct)");
	CheckOk(test, Libs::LibKernel::Memory::KernelReleaseDirectMemory(
	                  phys, SceKernelPageSize * 2),
	        "KernelReleaseDirectMemory");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(
	                  reinterpret_cast<uint64_t>(window), window_size),
	        "KernelMunmap(window reserve)");
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(
	                  reinterpret_cast<uint64_t>(arena), arena_size),
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
	Check(test, AvailableFlexibleMemory(test) == flexible_baseline,
	      "pooled commit consumed flexible memory instead of expanded direct "
	      "backing");
	CheckFailed(test,
	            Libs::LibKernel::Memory::KernelReleaseDirectMemory(pool_offset,
	                                                               SceKernelMemoryPoolExpandLen),
	            "KernelReleaseDirectMemory(committed pool expansion)");

	constexpr uint64_t first_value     = 0x504f4f4c4241434bull; // "POOLBACK"
	constexpr uint64_t second_value    = 0x5348415245444d45ull; // "SHAREDME"
	*reinterpret_cast<uint64_t*>(base) = first_value;
	*reinterpret_cast<uint64_t*>(base + SceKernelMemoryPoolCommitLen) = second_value;

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
	CheckOk(test, Libs::LibKernel::Memory::KernelMunmap(base, SceKernelMemoryPoolReserveLen),
	        "KernelMunmap(fragmented reserve cleanup)");

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

void TestProgramMemoryRegistrationAndProtection() {
	const char* test = "ProgramMemoryRegistrationAndProtection";
	const auto  size = SceKernelPageSize * 3;
	const auto  base = Common::VirtualMemory::Alloc(0, size, Common::VirtualMemory::Mode::ReadWrite);
	Check(test, base != 0, "program host allocation failed");

	Libs::LibKernel::Memory::RegisterProgramMemory(base, size,
	                                                Common::VirtualMemory::Mode::ReadWrite,
	                                                "program_test");
	ExpectRange(test, Query(test, base), base, base + size, SceKernelProtCpuRead | SceKernelProtCpuRw,
	            0, 0, 0, 1, "program_test");

	Libs::LibKernel::Memory::UpdateProgramMemoryProtection(
	    base, SceKernelPageSize, Common::VirtualMemory::Mode::Read);
	ExpectRange(test, Query(test, base), base, base + SceKernelPageSize, SceKernelProtCpuRead, 0, 0,
	            0, 1, "program_test");

	CheckOk(test,
	        Libs::LibKernel::Memory::KernelMprotect(
	            reinterpret_cast<void*>(base + SceKernelPageSize - 0x10), 0x20,
	            SceKernelProtCpuRead | SceKernelProtCpuRw),
	        "KernelMprotect(program split span)");
	ExpectRange(test, Query(test, base), base, base + size,
	            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "program_test");

	Libs::LibKernel::Memory::UpdateProgramMemoryProtection(
	    base + SceKernelPageSize * 2, SceKernelPageSize, Common::VirtualMemory::Mode::Read);
	ExpectRange(test, Query(test, base + SceKernelPageSize * 2), base + SceKernelPageSize * 2,
	            base + size, SceKernelProtCpuRead, 0, 0, 0, 1, "program_test");

	Libs::LibKernel::Memory::UnregisterProgramMemory(base, size);
	ExpectUnmapped(test, base);
	Check(test, Common::VirtualMemory::Free(base), "program host free failed");

	std::printf("[host]    %-48s ok\n", test);
}

} // namespace

int main() {
	InitSubsystems();

	RunTest(TestProsperoArgumentAndInfoSizeContracts);
	RunTest(TestFlexibleMapQueryAndWholeMunmap);
	RunTest(TestPartialFlexibleMunmapAndFindNext);
	RunTest(TestReserveMapFixedAndNoOverwrite);
	RunTest(TestFixedNoOverwriteRejectsReservedRange);
	RunTest(TestReleasedReserveCanBeReused);
	RunTest(TestMunmapAcrossAdjacentFlexibleMappings);
	RunTest(TestDirectMapQueryOffsetAndPartialMunmap);
	RunTest(TestNonzeroDirectOffsetAliasesSharedBacking);
	RunTest(TestDirectPhysicalFreeRangeReuseAndCoalescing);
	RunTest(TestDirectAlignmentStaysWithinSearchRange);
	RunTest(TestDefaultDirectMapUsesSystemAddressRange);
	RunTest(TestLargeDirectMapAliasesAcrossChunks);
	RunTest(TestDirectMapUnmapReusesHostAddress);
	RunTest(TestLargeHintedReserveHostsSmallDirectMap);
	RunTest(TestMemoryPoolAlignmentContracts);
	RunTest(TestProsperoSampleMemoryPoolExpandCommit);
	RunTest(TestFragmentedMemoryPoolBacking);
	RunTest(TestMemoryPoolMultiRangeDecommit);
	RunTest(TestMemoryPoolCommitDecommitQueryFlags);
	RunTest(TestProgramMemoryRegistrationAndProtection);

	if (g_failed_tests != 0) {
		std::printf("VirtualMemoryAllocationTests: %d case(s) failed\n", g_failed_tests);
		return 1;
	}

	std::printf("VirtualMemoryAllocationTests: all cases passed\n");
	return 0;
}
