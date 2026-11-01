#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/pm4.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>

namespace Libs {
void InitLibKernel_1(Loader::SymbolDatabase*);
}

namespace {

namespace Pm4 = Libs::Graphics::Pm4;

TEST(Trinity, ModeQueriesPacketsAndMemoryAgree) {
	static Common::Subsystems subsystems;
	Common::VirtualMemory::Init();
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	Loader::SymbolDatabase symbols;
	Libs::InitLibKernel_1(&symbols);
	const auto* query = symbols.FindByNid("tU5e3f9gSiU", Loader::SymbolType::Func);
	const auto* operation = symbols.FindByNid("NH6xARDOVv8", Loader::SymbolType::Func);
	ASSERT_NE(query, nullptr);
	ASSERT_NE(operation, nullptr);
	const auto is_trinity = reinterpret_cast<int(KYTY_SYSV_ABI*)()>(query->vaddr);
	const auto operation_mode = reinterpret_cast<int(KYTY_SYSV_ABI*)(int*, int*)>(operation->vaddr);

	struct CommandBuffer {
		uint32_t* bottom;
		uint32_t* top;
		uint32_t* cursor_up;
		uint32_t* cursor_down;
		void* callback;
		void* user_data;
		uint32_t reserved_dw;
	};
	const auto path = std::filesystem::temp_directory_path() /
	                  ("kyty_trinity_" + std::to_string(reinterpret_cast<uintptr_t>(&symbols)) + ".json");
	struct Case { bool enabled; uint32_t attribute; bool expected; };
	for (const auto test: {Case {false, 0, false}, Case {false, 1u << 22, false}, Case {true, 0, false},
	                       Case {true, 1u << 21, false}, Case {true, 1u << 22, true}}) {
		options.pro_enabled = test.enabled;
		Config::Load(options);
		{
			std::ofstream file(path);
			file << "{\"attribute3\":" << test.attribute
			     << R"(,"kernel":{"flexibleMemorySize":3221225472,"cpuPageTableSize":134217728,"gpuPageTableSize":0},"amm":{"pagetableMemorySizeInMib":96}})";
			ASSERT_TRUE(file.good());
		}
		testing::internal::CaptureStdout();
		Loader::SystemContentLoadParamSfo(path);
		std::filesystem::remove(path);
		EXPECT_EQ(Loader::SystemContentIsTrinityMode(), test.expected);
		EXPECT_EQ(is_trinity(), test.expected ? 1 : 0);
		int mode = -1, submode = -1;
		EXPECT_EQ(operation_mode(&mode, &submode), 0);
		EXPECT_EQ(mode, test.expected ? 3 : 2);
		EXPECT_EQ(submode, 0);
		uint8_t graphics_mode = 0xff;
		Libs::Graphics::Gen5::AgcGetIsTrinityMode(&graphics_mode);
		EXPECT_EQ(graphics_mode, test.expected ? 1 : 0);
		std::array<uint32_t, 5> packet {};
		CommandBuffer buffer {packet.data(), packet.data() + packet.size(), packet.data(),
		                      packet.data() + packet.size(), nullptr, nullptr, 0};
		Libs::Graphics::Gen5::AgcDcbSetUcRegistersIndirect(
		    reinterpret_cast<Libs::Graphics::Gen5::CommandBuffer*>(&buffer), nullptr, 0);
		EXPECT_EQ(packet[0], KYTY_PM4(5, Pm4::IT_SET_UCONFIG_REG_INDIRECT,
		                              test.expected ? 1 : 0));
		const auto console = testing::internal::GetCapturedStdout();
		if (test.enabled) {
			EXPECT_EQ(console, test.expected ? "[PS5] title Trinity support=yes\n"
			                                 : "[PS5] title Trinity support=no\n");
		} else {
			EXPECT_TRUE(console.empty()) << console;
		}
	}

	using namespace Libs::LibKernel::Memory;
	subsystems.Initialize<Lifecycle>();
	constexpr uint64_t mib = 1024 * 1024;
	constexpr uint64_t backing = (12800 + 1216 - 64 - 96) * mib;
	constexpr uint64_t flexible = 3072 * mib;
	EXPECT_EQ(TestGuestBackingSize(), backing);
	EXPECT_EQ(KernelGetDirectMemorySize(), backing - flexible);
	size_t configured = 0;
	ASSERT_EQ(KernelConfiguredFlexibleMemorySize(&configured), 0);
	EXPECT_EQ(configured, flexible);
	int64_t available_start = -1;
	size_t available_size = 0;
	ASSERT_EQ(KernelAvailableDirectMemorySize(0, backing - flexible, 0x4000,
	                                         &available_start, &available_size), 0);
	EXPECT_EQ(available_start, 0);
	EXPECT_EQ(available_size, backing - flexible);
	int64_t physical = -1;
	ASSERT_EQ(KernelAllocateDirectMemory(backing - flexible - 0x4000, backing - flexible,
	                                     0x4000, 0x4000, 0, &physical), 0);
	void* mapped = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&mapped, 0x4000, 2, 0, physical, 0), 0);
	*static_cast<volatile uint64_t*>(mapped) = 0x123456789abcdef0ull;
	EXPECT_EQ(*static_cast<volatile uint64_t*>(mapped), 0x123456789abcdef0ull);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(mapped), 0x4000), 0);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical, 0x4000), 0);
}

} // namespace

int RunTrinityTests() {
	int argc = 1;
	char name[] = "trinity_tests";
	char* argv[] = {name, nullptr};
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
