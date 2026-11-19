#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/memory.h"
#include "libs/errno.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace Libs {
void InitLibKernel_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

namespace {

using namespace Loader;

struct SegmentInfo {
	uint64_t address;
	uint32_t size;
	int32_t  prot;
};

struct ModuleInfo2 {
	uint64_t    st_size;
	char        name[256];
	SegmentInfo segments[4];
	uint32_t    segment_count;
	uint8_t     fingerprint[20];
};
static_assert(sizeof(ModuleInfo2) == 0x160);

struct Image {
	Elf64_Ehdr                ehdr;
	std::array<Elf64_Phdr, 8> phdrs;
	std::array<Elf64_Dyn, 4>  dyn;
	char                      strings[16];
};

constexpr Elf64_Ehdr MODULE_EHDR {
    .e_ident     = {0x7f, 'E', 'L', 'F', ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_FREEBSD, 2},
    .e_type      = ET_DYNAMIC,
    .e_machine   = EM_X86_64,
    .e_version   = EV_CURRENT,
    .e_phoff     = offsetof(Image, phdrs),
    .e_phentsize = sizeof(Elf64_Phdr),
    .e_phnum     = sizeof(Image::phdrs) / sizeof(Elf64_Phdr),
};

// Five mapped segments (X, RELRO R, RW, R, RW) and a SONAME that differs from the file name.
constexpr Image MODULE_IMAGE {
    .ehdr    = MODULE_EHDR,
    .phdrs   = {{
        {PT_LOAD, PF_X, 0, 0x0000, 0x0000, 0, 0x1ac, 0x4000},
        {PT_LOAD, PF_R, 0, 0x4000, 0x4000, 0, 0, 0x4000},
        {PT_OS_RELRO, PF_R, 0, 0x4000, 0x4000, 0, 0x83, 0x4000},
        {PT_DYNAMIC, PF_R, offsetof(Image, dyn), 0, 0, sizeof(Image::dyn), sizeof(Image::dyn), 8},
        {PT_OS_DYNLIBDATA, PF_R, offsetof(Image, strings), 0, 0, sizeof(Image::strings), 0, 8},
        {PT_LOAD, PF_R | PF_W, 0, 0x8000, 0x8000, 0, 0x338, 0x4000},
        {PT_LOAD, PF_R, 0, 0xc000, 0xc000, 0, 0x21, 0x4000},
        {PT_LOAD, PF_R | PF_W, 0, 0x10000, 0x10000, 0, 0x550, 0x4000},
    }},
    .dyn     = {{{DT_PLTREL, {DT_RELA}}, {DT_OS_STRTAB, {0}}, {DT_SONAME, {1}}, {DT_NULL, {0}}}},
    .strings = "\0libsoname.prx",
};

Program* LoadModule(const char* name) {
	const auto path = std::filesystem::temp_directory_path() / name;
	std::ofstream(path, std::ios::binary)
	    .write(reinterpret_cast<const char*>(&MODULE_IMAGE), sizeof(MODULE_IMAGE));
	return Common::Singleton<RuntimeLinker>::Instance()->LoadProgram(path);
}

TEST(KernelModuleInfo, ListsLoadedModulesInLoadOrderAndDescribesTheirMappedSegments) {
	SymbolDatabase symbols;
	Libs::InitLibKernel_1(&symbols);
	const auto* list_symbol = symbols.FindByNid("ZzzC3ZGVAkc", SymbolType::Func);
	const auto* info_symbol = symbols.FindByNid("QgsKEUfkqMA", SymbolType::Func);
	ASSERT_NE(list_symbol, nullptr);
	ASSERT_NE(info_symbol, nullptr);
	const auto get_module_list2 =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(int32_t*, uint64_t, uint64_t*)>(list_symbol->vaddr);
	const auto get_module_info2 =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(int32_t, ModuleInfo2*)>(info_symbol->vaddr);
	const auto* first  = LoadModule("kyty_module_info_first.prx");
	const auto* second = LoadModule("kyty_module_info_second.prx");

	std::array<int32_t, 3> handles {-1, -1, -1};
	uint64_t               count = 99;
	EXPECT_EQ(get_module_list2(handles.data(), 1, &count), Libs::LibKernel::KERNEL_ERROR_ENOMEM);
	EXPECT_EQ(handles[0], -1);
	EXPECT_EQ(count, 99u);
	EXPECT_EQ(get_module_list2(nullptr, 3, &count), Libs::LibKernel::KERNEL_ERROR_EFAULT);
	EXPECT_EQ(get_module_list2(handles.data(), 3, nullptr), Libs::LibKernel::KERNEL_ERROR_EFAULT);
	ASSERT_EQ(get_module_list2(handles.data(), 2, &count), 0);
	EXPECT_EQ(count, 2u);
	EXPECT_EQ(handles[0], first->unique_id);
	EXPECT_EQ(handles[1], second->unique_id);
	EXPECT_EQ(handles[2], -1);

	ModuleInfo2 info {};
	EXPECT_EQ(get_module_info2(second->unique_id, nullptr), Libs::LibKernel::KERNEL_ERROR_EFAULT);
	info.st_size = sizeof(info) - 1;
	EXPECT_EQ(get_module_info2(second->unique_id, &info), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	info.st_size = sizeof(info) + 1;
	EXPECT_EQ(get_module_info2(second->unique_id, &info), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	info.st_size = sizeof(info);
	EXPECT_EQ(get_module_info2(second->unique_id + 1, &info), Libs::LibKernel::KERNEL_ERROR_ESRCH);

	info.segment_count  = 77;
	info.fingerprint[0] = 0xcd;
	ASSERT_EQ(get_module_info2(second->unique_id, &info), 0);
	EXPECT_EQ(info.st_size, sizeof(info));
	EXPECT_STREQ(second->dynamic_info->so_name, "libsoname.prx");
	EXPECT_STREQ(info.name, "kyty_module_info_second.prx");
	ASSERT_EQ(info.segment_count, 4u);
	const std::array<SegmentInfo, 4> expected {{
	    {second->base_vaddr + 0x0000, 0x1ac, 4},
	    {second->base_vaddr + 0x4000, 0x83, 1},
	    {second->base_vaddr + 0x8000, 0x338, 3},
	    {second->base_vaddr + 0xc000, 0x21, 1},
	}};
	for (size_t i = 0; i < expected.size(); i++) {
		EXPECT_EQ(info.segments[i].address, expected[i].address) << i;
		EXPECT_EQ(info.segments[i].size, expected[i].size) << i;
		EXPECT_EQ(info.segments[i].prot, expected[i].prot) << i;
	}
	EXPECT_EQ(info.fingerprint[0], 0);
}

} // namespace

int main(int argc, char** argv) {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	testing::InitGoogleTest(&argc, argv);
	const int result = RUN_ALL_TESTS();
	subsystems.Destroy();
	return result;
}
