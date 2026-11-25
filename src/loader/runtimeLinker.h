#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/hostException.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "loader/symbolDatabase.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Loader {

// The emulator's host exception handler: emulated instructions, walker loads, GPU-tracked pages.
bool KytyExceptionHandler(const Common::HostException::ExceptionInfo& exception_info);

struct EntryParams {
	int         argc;
	uint32_t    pad;
	const char* argv[1];
};

class Elf64;
struct Elf64_Phdr;
struct Elf64_Sym;
struct Elf64_Rela;
class RuntimeLinker;

using module_func_t                          = KYTY_SYSV_ABI int (*)(size_t args, const void* argp);
using application_heap_free_func_t           = KYTY_SYSV_ABI void (*)(void*);
using application_heap_malloc_func_t         = KYTY_SYSV_ABI void* (*)(uint64_t);
using application_heap_posix_memalign_func_t = KYTY_SYSV_ABI int (*)(void**, uint64_t, uint64_t);

struct ModuleId {
	std::string id;
	std::string name;
};

struct LibraryId {
	std::string id;
	std::string name;
};

struct ThreadLocalStorage {
	uint64_t image_vaddr = 0;
	uint64_t init_size   = 0;
	uint64_t image_size  = 0;
	uint32_t index       = 0; // DTV slot; 0 is the main program's static TLS
};

struct DynamicInfo {
	void*    hash_table      = nullptr;
	uint64_t hash_table_size = 0;

	char*    str_table      = nullptr;
	uint64_t str_table_size = 0;

	Elf64_Sym* symbol_table            = nullptr;
	uint64_t   symbol_table_total_size = 0;
	uint64_t   symbol_table_entry_size = 0;

	uint64_t init_vaddr          = 0;
	uint64_t fini_vaddr          = 0;
	uint64_t init_array_vaddr    = 0;
	uint64_t fini_array_vaddr    = 0;
	uint64_t preinit_array_vaddr = 0;
	uint64_t init_array_size     = 0;
	uint64_t fini_array_size     = 0;
	uint64_t preinit_array_size  = 0;

	Elf64_Rela* jmprela_table      = nullptr;
	uint64_t    jmprela_table_size = 0;

	Elf64_Rela* rela_table            = nullptr;
	uint64_t    rela_table_total_size = 0;
	uint64_t    rela_table_entry_size = 0;

	uint64_t relative_count = 0;

	uint64_t debug   = 0;
	uint64_t textrel = 0;
	uint64_t flags   = 0;

	const char* so_name = nullptr;

	std::vector<ModuleId>  export_modules;
	std::vector<ModuleId>  import_modules;
	std::vector<LibraryId> export_libs;
	std::vector<LibraryId> import_libs;
};

struct Program {
	Program();
	~Program();

	int32_t                      unique_id = -1;
	RuntimeLinker*               rt        = nullptr;
	uint32_t                     load_count = 0; // Successful sceKernelLoadStartModule calls.
	std::filesystem::path        file_name;
	std::unique_ptr<Elf64>       elf;
	std::unique_ptr<DynamicInfo> dynamic_info;
	uint64_t                     base_vaddr        = 0;
	uint64_t                     base_size         = 0;
	uint64_t                     base_size_aligned = 0;
	uint64_t                     mapped_size       = 0;
	uint64_t                     flexible_size     = 0;
	uint64_t instruction_trampoline_size = 0;
	std::unique_ptr<SymbolDatabase> export_symbols;
	std::unique_ptr<SymbolDatabase> import_symbols;
	ThreadLocalStorage              tls;
	bool                            fail_if_global_not_resolved = true;
	bool                            dbg_print_reloc             = false;
	std::vector<uint8_t>            rela_bits;
	uint64_t                        proc_param_vaddr            = 0;
};

class RuntimeLinker {
public:
	RuntimeLinker();
	virtual ~RuntimeLinker();
	void Clear();

	KYTY_CLASS_NO_COPY(RuntimeLinker);

	void DbgDump(const std::string& folder);

	Program* LoadProgram(const std::filesystem::path& elf_name);
	void     SaveMainProgram(const std::filesystem::path& elf_name);
	void     SaveProgram(Program* program, const std::filesystem::path& elf_name);
	void     UnloadProgram(Program* program);

	[[nodiscard]] uint64_t GetEntry();
	[[nodiscard]] uint64_t GetProcParam();

	void RelocateAll();
	void RelocateProgram(Program* program);

	void  Execute(const std::filesystem::path& game_patch = {});
	int   StartModule(Program* program, size_t args, const void* argp, module_func_t func);
	int   StopModule(Program* program, size_t args, const void* argp, module_func_t func);
	void  StopAllModules();
	void  SetApplicationHeapApi(void* const api[10]);
	void* ApplicationHeapMalloc(uint64_t size);
	void* ApplicationHeapMemalign(uint64_t alignment, uint64_t size);

	void Resolve(const std::string& name, SymbolType type, Program* program, SymbolRecord* out_info,
	             bool* bind_self);

	SymbolDatabase* Symbols() { return m_symbols.get(); }

	static uint64_t ReadFromElf(Program* program, uint64_t vaddr);
	Program*        FindProgramByAddr(uint64_t vaddr);
	Program*        FindProgramById(int32_t id);
	Program*        FindProgramByFileName(const std::filesystem::path& elf_name);
	// The ids of the loaded programs in load order.
	std::vector<int32_t> GetProgramIds();

	// CreateThreadTls runs on each guest thread before its first guest instruction.
	static void     CreateThreadTls();
	static void     DeleteThreadTls();
	static void*    GetThreadTcb();
	static void     SetThreadTcb(void* tcb);
	static uint8_t* TlsGetAddr(Program* program);
	static void     PatchGuestCode(uint64_t address, uint64_t size);

	void StackTrace(uint64_t frame_ptr, uint64_t stack_ptr);

private:
	static void LoadProgramToMemory(Program* program);
	static void ParseProgramDynamicInfo(Program* program);
	static void CreateSymbolDatabase(Program* program);
	static void Relocate(Program* program);
	static void DeleteProgram(Program* program);

	static const ModuleId*  FindModule(const Program& program, const std::string& id);
	static const LibraryId* FindLibrary(const Program& program, const std::string& id);

	std::vector<Program*>           m_programs;
	std::unique_ptr<SymbolDatabase> m_symbols;
	Common::Mutex                   m_mutex;

	application_heap_malloc_func_t         m_application_heap_malloc         = nullptr;
	application_heap_free_func_t           m_application_heap_free           = nullptr;
	application_heap_posix_memalign_func_t m_application_heap_posix_memalign = nullptr;
};

uint64_t CalcFlexibleMemorySize(const Elf64_Phdr* phdr, uint16_t phnum);
bool     IsGuestCodeAddress(uint64_t address);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool     TestMainEntryUsesGuestStack();
uint64_t TestMapGuestCode(uint64_t size, uint64_t occupied);
bool TestModuleRelocationUsesWritableHostMapping();
#endif

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_ */
