#include "loader/runtimeLinker.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hostException.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/platform/sysDbg.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/pageManager.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "loader/elf.h"
#include "loader/gamePatch.h"
#include "loader/jit.h"
#include "loader/redZonePatcher.h"
#include "loader/symbolDatabase.h"
#include "loader/x64InstructionEmulator.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/uio.h>
#include <unistd.h>
#endif
#endif

namespace Libs::LibKernel {
void SetProgName(const std::string& name);
} // namespace Libs::LibKernel

namespace Loader {

Program::Program() = default;

Program::~Program() = default;

static void FreeTlsBlock(ThreadLocalStorage::Block* block) {
	if (block == nullptr || block->ptr == nullptr) {
		return;
	}

	if (block->free_func != nullptr) {
		block->free_func(block->ptr);
	} else if (block->vm_alloc) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(reinterpret_cast<uint64_t>(block->ptr),
		                                                  block->alloc_size));
	} else {
		delete[] block->ptr;
	}

	block->ptr        = nullptr;
	block->free_func  = nullptr;
	block->vm_alloc   = false;
	block->alloc_size = 0;
}

static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
	return alignment != 0 ? (value + alignment - 1) & ~(alignment - 1) : value;
}

ThreadLocalStorage::~ThreadLocalStorage() {
	for (auto& [_, block]: tlss) {
		FreeTlsBlock(&block);
	}
}

#pragma pack(1)

struct EntryParams {
	int         argc;
	uint32_t    pad;
	const char* argv[3];
};

#pragma pack()

using atexit_func_t = KYTY_SYSV_ABI void (*)();
using entry_func_t  = KYTY_SYSV_ABI void (*)(EntryParams* params, atexit_func_t atexit_func);
using module_ini_fini_func_t = KYTY_SYSV_ABI int (*)(size_t args, const void* argp,
                                                     module_func_t func);

enum class BindType { Unknown, Local, Global, Weak };

struct RelocationInfo {
	bool        resolved   = false;
	BindType    bind       = BindType::Unknown;
	SymbolType  type       = SymbolType::Unknown;
	uint64_t    value      = 0;
	uint64_t    vaddr      = 0;
	uint64_t    base_vaddr = 0;
	std::string name;
	std::string dbg_name;
	bool        bind_self = false;
};

struct StubbedImportRecord {
	uint32_t    index       = 0;
	uint64_t    patch_vaddr = 0;
	uint64_t    thunk_vaddr = 0;
	std::string name;
	SymbolType  type = SymbolType::Unknown;
	BindType    bind = BindType::Unknown;
	std::string program;
};

// The structure will be passed via the stack
// since the size of an object is larger than 16 bytes
struct RelocateHandlerStack {
	uint64_t stack[3];
};

static std::vector<StubbedImportRecord> g_stubbed_imports;
static std::atomic_uint32_t             g_unresolved_stub_call_log_count {0};
static std::vector<uint64_t>            g_unresolved_stub_thunk_pages;
static uint64_t                         g_unresolved_stub_thunk_offset = 0;
static constexpr uint64_t               UNRESOLVED_STUB_PAGE_SIZE      = 4096;

static KYTY_SYSV_ABI uint64_t ResolveImportStubWithId(uint64_t record_id);

static bool PatchGuestMemory64(uint64_t vaddr, uint64_t value) {
	auto* ptr     = reinterpret_cast<uint64_t*>(vaddr);
	bool  changed = (*ptr != value);
	std::memcpy(ptr, &value, sizeof(value));
	return changed;
}

static uint64_t AllocateUnresolvedImportThunk(uint64_t record_id) {
	constexpr uint64_t thunk_size = 165;

	if (g_unresolved_stub_thunk_pages.empty() ||
	    g_unresolved_stub_thunk_offset + thunk_size > UNRESOLVED_STUB_PAGE_SIZE) {
		auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, UNRESOLVED_STUB_PAGE_SIZE, Common::VirtualMemory::Mode::ExecuteReadWrite,
		    "unresolved_import_thunk");
		EXIT_NOT_IMPLEMENTED(page == 0);
		g_unresolved_stub_thunk_pages.push_back(page);
		g_unresolved_stub_thunk_offset = 0;
	}

	auto* code = reinterpret_cast<uint8_t*>(g_unresolved_stub_thunk_pages.back() +
	                                        g_unresolved_stub_thunk_offset);
	g_unresolved_stub_thunk_offset += thunk_size;

	const auto target = reinterpret_cast<uint64_t>(ResolveImportStubWithId);
	uint8_t    bytes[thunk_size] {};
	size_t     i      = 0;
	const auto emit   = [&](uint8_t b) { bytes[i++] = b; };
	const auto emit64 = [&](uint64_t v) {
		std::memcpy(bytes + i, &v, sizeof(v));
		i += sizeof(v);
	};
	const auto emit32 = [&](uint32_t v) {
		std::memcpy(bytes + i, &v, sizeof(v));
		i += sizeof(v);
	};
	const auto save_xmm = [&](uint8_t reg, uint8_t offset) {
		emit(0xf3);
		emit(0x0f);
		emit(0x7f);
		if (offset == 0) {
			emit(static_cast<uint8_t>(0x04u | (reg << 3u)));
			emit(0x24);
		} else {
			emit(static_cast<uint8_t>(0x44u | (reg << 3u)));
			emit(0x24);
			emit(offset);
		}
	};
	const auto load_xmm = [&](uint8_t reg, uint8_t offset) {
		emit(0xf3);
		emit(0x0f);
		emit(0x6f);
		if (offset == 0) {
			emit(static_cast<uint8_t>(0x04u | (reg << 3u)));
			emit(0x24);
		} else {
			emit(static_cast<uint8_t>(0x44u | (reg << 3u)));
			emit(0x24);
			emit(offset);
		}
	};

	emit(0x50); // push rax; preserve AL for variadic SysV calls
	emit(0x57); // push rdi
	emit(0x56); // push rsi
	emit(0x52); // push rdx
	emit(0x51); // push rcx
	emit(0x41);
	emit(0x50); // push r8
	emit(0x41);
	emit(0x51); // push r9
	emit(0x48);
	emit(0x81);
	emit(0xec);
	emit32(0x80); // sub rsp, 0x80
	for (uint8_t reg = 0; reg < 8; reg++) {
		save_xmm(reg, static_cast<uint8_t>(reg * 0x10u));
	}
	emit(0x48);
	emit(0xbf);
	emit64(record_id); // mov rdi, record_id
	emit(0x48);
	emit(0xb8);
	emit64(target); // mov rax, ResolveImportStubWithId
	emit(0xff);
	emit(0xd0); // call rax
	emit(0x49);
	emit(0x89);
	emit(0xc3); // mov r11, rax
	for (uint8_t reg = 0; reg < 8; reg++) {
		load_xmm(reg, static_cast<uint8_t>(reg * 0x10u));
	}
	emit(0x48);
	emit(0x81);
	emit(0xc4);
	emit32(0x80); // add rsp, 0x80
	emit(0x41);
	emit(0x59); // pop r9
	emit(0x41);
	emit(0x58); // pop r8
	emit(0x59); // pop rcx
	emit(0x5a); // pop rdx
	emit(0x5e); // pop rsi
	emit(0x5f); // pop rdi
	emit(0x58); // pop rax
	emit(0x4d);
	emit(0x85);
	emit(0xdb); // test r11, r11
	emit(0x74);
	emit(0x03); // jz +3
	emit(0x41);
	emit(0xff);
	emit(0xe3); // jmp r11
	// Match the integer fallback for floating-point return values.
	emit(0x0f);
	emit(0x57);
	emit(0xc0); // xorps xmm0, xmm0
	emit(0x31);
	emit(0xc0); // xor eax, eax
	emit(0xc3); // ret

	EXIT_NOT_IMPLEMENTED(i != thunk_size);
	std::memcpy(code, bytes, sizeof(bytes));
	Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(code), thunk_size);
	return reinterpret_cast<uint64_t>(code);
}

static uint64_t RegisterStubbedImport(uint32_t index, const Program* program,
                                      const RelocationInfo& ri) {
	const auto program_name = program != nullptr ? Common::PathToString(program->file_name) : "";

	for (auto& record: g_stubbed_imports) {
		if (record.patch_vaddr == ri.vaddr) {
			record.index   = index;
			record.name    = ri.name;
			record.type    = ri.type;
			record.bind    = ri.bind;
			record.program = program_name;
			return record.thunk_vaddr;
		}
	}

	StubbedImportRecord record {};
	record.index       = index;
	record.patch_vaddr = ri.vaddr;
	record.name        = ri.name;
	record.type        = ri.type;
	record.bind        = ri.bind;
	record.program     = program_name;
	g_stubbed_imports.push_back(record);
	const auto record_id                     = g_stubbed_imports.size() - 1;
	const auto thunk                         = AllocateUnresolvedImportThunk(record_id);
	g_stubbed_imports[record_id].thunk_vaddr = thunk;
	return thunk;
}

static KYTY_SYSV_ABI uint64_t ResolveImportStubWithId(uint64_t record_id) {
	if (record_id < g_stubbed_imports.size()) {
		auto& record = g_stubbed_imports[record_id];
		auto  nid    = record.name;
		auto  pos    = Common::FindIndex(nid, "[");
		if (Common::IndexValid(nid, pos)) {
			nid = Common::Left(nid, pos);
		}

		SymbolRecord resolved {};
		if (!nid.empty() &&
		    Common::Singleton<RuntimeLinker>::Instance()->ResolveLoadedSymbolByNid(nid, record.type,
		                                                                           &resolved) &&
		    resolved.vaddr != 0 && resolved.vaddr != record.thunk_vaddr) {
			LOGF("Late-resolved import: %s -> %s [0x%016" PRIx64 "]\n", record.name.c_str(),
			     resolved.name.c_str(), resolved.vaddr);

			if (record.patch_vaddr != 0) {
				PatchGuestMemory64(record.patch_vaddr, resolved.vaddr);
			}

			return resolved.vaddr;
		}
	}

	const auto log_index = g_unresolved_stub_call_log_count.fetch_add(1);
	if (log_index < 1024) {
		if (record_id < g_stubbed_imports.size()) {
			const auto& record = g_stubbed_imports[record_id];
			printf("Unresolved import stub called: %s\n", record.name.c_str());
			LOGF("Unresolved import stub called [%u]: patch_vaddr=0x%016" PRIx64
			     " jmprela_index=%" PRIu32 " symbol=%s type=%s bind=%s program=%s\n",
			     log_index, record.patch_vaddr, record.index, record.name.c_str(),
			     Common::EnumName(record.type).c_str(), Common::EnumName(record.bind).c_str(),
			     record.program.c_str());
		} else {
			printf("Unresolved import stub called: <bad-record>\n");
			LOGF("Unresolved import stub called [%u]: record_id=%" PRIu64 " symbol=<bad-record>\n",
			     log_index, record_id);
		}
	}
	return 0;
}

constexpr uint64_t SYSTEM_RESERVED  = 0x800000000u;
constexpr uint64_t CODE_BASE_INCR   = 0x010000000u;
constexpr uint64_t INVALID_OFFSET   = 0x040000000u;
constexpr uint64_t CODE_BASE_OFFSET = 0x100000000u;
constexpr uint64_t INVALID_MEMORY   = SYSTEM_RESERVED + INVALID_OFFSET;

static uint64_t g_desired_base_addr = SYSTEM_RESERVED + CODE_BASE_OFFSET;
static uint64_t g_invalid_memory    = 0;

static Program*              g_tls_main_program        = nullptr;
static thread_local Program* g_tls_cached_main_program = nullptr;
static thread_local uint8_t* g_tls_cached_main_tcb     = nullptr;

static KYTY_SYSV_ABI void RunEntry(uint64_t addr, EntryParams* params, atexit_func_t atexit_func,
                                   void* stack_top) {
#if defined(__x86_64__) || defined(_M_X64)
	auto* func = reinterpret_cast<entry_func_t>(addr);

	if (stack_top != nullptr) {
		const auto aligned_stack_top =
		    reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0x0f);
		const auto guest_rsp = aligned_stack_top - 2u * sizeof(uintptr_t);
		const auto guest_rbp = guest_rsp;

		auto* guest_root_frame = reinterpret_cast<uintptr_t*>(guest_rbp);
		guest_root_frame[0]    = 0;
		guest_root_frame[1]    = 0;

#if defined(__APPLE__)
		// Clang on macOS can allocate plain "r" inputs to r12/r13, which the template
		// clobbers before consuming them. Pin the inputs to registers the SysV guest
		// preserves without changing register allocation on Windows or Linux.
		register entry_func_t func_reg asm("rbx")      = func;
		register uintptr_t    guest_rsp_reg asm("r14") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r15") = guest_rbp;
#endif

#if defined(__APPLE__)
		asm volatile(
		    "pushq %%r12\n\t"
		    "pushq %%r13\n\t"
		    "movq %%rsp, %%r12\n\t"
		    "movq %%rbp, %%r13\n\t"
		    "movq %[guest_rsp], %%rsp\n\t"
		    "movq %[guest_rbp], %%rbp\n\t"
		    "callq *%[func]\n\t"
		    "movq %%r13, %%rbp\n\t"
		    "movq %%r12, %%rsp\n\t"
		    "popq %%r13\n\t"
		    "popq %%r12\n\t"
		    :
		    : [func] "r"(func_reg), "D"(params),
		      "S"(atexit_func), [guest_rsp] "r"(guest_rsp_reg), [guest_rbp] "r"(guest_rbp_reg)
		    : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
		      "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
		      "xmm13", "xmm14", "xmm15");
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Windows stack probes use the TEB stack limits during the guest stack switch.
		// bounds, which describe the host stack and are invalid while RSP is in guest memory.
		register entry_func_t func_reg asm("rbx")     = func;
		register uintptr_t    guest_rsp_reg asm("r8") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r9") = guest_rbp;
		asm volatile("pushq %%r12\n\t"
		             "pushq %%r13\n\t"
		             "pushq %%r14\n\t"
		             "pushq %%r15\n\t"
		             "movq %%gs:0x08, %%r14\n\t"
		             "movq %%gs:0x10, %%r15\n\t"
		             "xorq %%rcx, %%rcx\n\t"
		             "movq %%rcx, %%gs:0x08\n\t"
		             "movq %%rcx, %%gs:0x10\n\t"
		             "movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             "movq %%r14, %%gs:0x08\n\t"
		             "movq %%r15, %%gs:0x10\n\t"
		             "popq %%r15\n\t"
		             "popq %%r14\n\t"
		             "popq %%r13\n\t"
		             "popq %%r12\n\t"
		             : [guest_rsp] "+r"(guest_rsp_reg), [guest_rbp] "+r"(guest_rbp_reg)
		             : [func] "r"(func_reg), "D"(params), "S"(atexit_func)
		             : "cc", "memory", "rax", "rcx", "rdx", "r10", "r11", "xmm0", "xmm1", "xmm2",
		               "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
		               "xmm12", "xmm13", "xmm14", "xmm15");
#else
		// Clobbers prevent inputs from being allocated to r12/r13.
		asm volatile("movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             :
		             : [func] "r"(func), "D"(params),
		               "S"(atexit_func), [guest_rsp] "r"(guest_rsp), [guest_rbp] "r"(guest_rbp)
		             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13",
		               "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
		               "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
		return;
	}

	uintptr_t guest_root_frame[2] = {};

#if defined(__APPLE__)
	register entry_func_t func_reg asm("rbx")      = func;
	register uintptr_t    guest_rbp_reg asm("r14") = reinterpret_cast<uintptr_t>(guest_root_frame);
#endif

#if defined(__APPLE__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("pushq %%r12\n\t"
	             "pushq %%r13\n\t"
	             "movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             "popq %%r13\n\t"
	             "popq %%r12\n\t"
	             :
#if defined(__APPLE__)
	             : [func] "r"(func_reg), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_rbp_reg)
#else
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
#endif
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1",
	               "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
	               "xmm12", "xmm13", "xmm14", "xmm15");
#else
	// Keep inputs out of r12.
	asm volatile("movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             :
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "xmm0",
	               "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
	               "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
#else
	(void)stack_top;
	reinterpret_cast<entry_func_t>(addr)(params, atexit_func);
#endif
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
struct MainEntryStackTestState {
	bool      called = false;
	uintptr_t rsp    = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t teb_stack_base  = UINTPTR_MAX;
	uintptr_t teb_stack_limit = UINTPTR_MAX;
#endif
};

static KYTY_SYSV_ABI void TestMainEntryStackCallback(EntryParams* params,
                                                     atexit_func_t /*atexit_func*/) {
	auto* state = reinterpret_cast<MainEntryStackTestState*>(const_cast<char*>(params->argv[0]));
	asm volatile("pushq %%r15\n\t"
	             "pushq %%r14\n\t"
	             "popq %%r14\n\t"
	             "popq %%r15\n\t"
	             :
	             :
	             : "memory");
	asm volatile("movq %%rsp, %0" : "=r"(state->rsp) : : "memory");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(state->teb_stack_base), "=r"(state->teb_stack_limit)
	             :
	             : "memory");
#endif
	state->called = true;
}

bool TestMainEntryUsesGuestStack() {
	constexpr uint64_t stack_size = 0x10000;
	const auto         stack_base = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, stack_size, Common::VirtualMemory::Mode::ReadWrite, "main_entry_stack_test");
	if (stack_base == 0) {
		return false;
	}

	MainEntryStackTestState state {};
	EntryParams             params {};
	params.argv[0] = reinterpret_cast<const char*>(&state);
	std::memset(reinterpret_cast<void*>(stack_base), 0xcd, stack_size);
	auto* root_frame = reinterpret_cast<const uintptr_t*>(stack_base + stack_size) - 2;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t original_teb_stack_base  = 0;
	uintptr_t original_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(original_teb_stack_base), "=r"(original_teb_stack_limit)
	             :
	             : "memory");
#endif

	RunEntry(reinterpret_cast<uint64_t>(TestMainEntryStackCallback), &params, nullptr,
	         reinterpret_cast<void*>(stack_base + stack_size));

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t restored_teb_stack_base  = 0;
	uintptr_t restored_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(restored_teb_stack_base), "=r"(restored_teb_stack_limit)
	             :
	             : "memory");
	const bool teb_ok = state.teb_stack_base == 0 && state.teb_stack_limit == 0 &&
	                    restored_teb_stack_base == original_teb_stack_base &&
	                    restored_teb_stack_limit == original_teb_stack_limit;
#else
	constexpr bool teb_ok = true;
#endif

	const bool rsp_ok  = state.rsp >= stack_base && state.rsp < stack_base + stack_size;
	const bool root_ok = root_frame[0] == 0 && root_frame[1] == 0;
	const bool freed   = Libs::LibKernel::Memory::FreeGuestMemory(stack_base, stack_size);
	return state.called && rsp_ok && root_ok && teb_ok && freed;
}

bool TestModuleRelocationUsesWritableHostMapping() {
	constexpr uint64_t page_size = 0x4000;
	constexpr uint64_t value     = 0x4b59545950415443;
	const auto         base      = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0, page_size, Common::VirtualMemory::Mode::ReadWrite, "host_only_patch_test");
	if (base == 0) {
		return false;
	}
	Libs::LibKernel::Memory::SetProgramMemoryProtection(base, page_size,
	                                                    Common::VirtualMemory::Mode::Read);

	Libs::LibKernel::Memory::VirtualQueryInfo before {};
	Libs::LibKernel::Memory::VirtualQueryInfo after {};
	const bool                                before_ok =
	    Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(base), 0, &before,
	                                                sizeof(before)) == 0;
	const bool changed  = PatchGuestMemory64(base, value);
	const bool after_ok = Libs::LibKernel::Memory::KernelVirtualQuery(
	                          reinterpret_cast<const void*>(base), 0, &after, sizeof(after)) == 0;
	const bool value_ok = *reinterpret_cast<const uint64_t*>(base) == value;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION mbi {};
	const bool               host_mode_ok =
	    VirtualQuery(reinterpret_cast<const void*>(base), &mbi, sizeof(mbi)) != 0 &&
	    mbi.Protect == PAGE_READWRITE;
#else
	constexpr bool host_mode_ok = true;
#endif
	const bool freed = Libs::LibKernel::Memory::FreeGuestMemory(base, page_size);

	return before_ok && after_ok && changed && value_ok && host_mode_ok && freed &&
	       before.protection == after.protection;
}
#endif

static uint64_t GetAlignedSize(const Elf64_Phdr* p) {
	return (p->p_align != 0 ? (p->p_memsz + (p->p_align - 1)) & ~(p->p_align - 1) : p->p_memsz);
}

static void DbgDumpSymbols(const std::string& folder, Elf64_Sym* symbols, uint64_t size,
                           const char* names) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + "symbols.txt");

	for (auto* sym = symbols;
	     reinterpret_cast<uint8_t*>(sym) < reinterpret_cast<uint8_t*>(symbols) + size; sym++) {
		f.Printf("----\n");
		f.Printf("st_name = %" PRIu32 ", %s\n", sym->st_name, names + sym->st_name);
		f.Printf("st_info = 0x%02" PRIx8 "\n", sym->st_info);
		f.Printf("st_other = 0x%02" PRIx8 "\n", sym->st_other);
		f.Printf("st_shndx = 0x%04" PRIx16 "\n", sym->st_shndx);
		f.Printf("st_value = 0x%016" PRIx64 "\n", sym->st_value);
		f.Printf("st_size = %" PRIu64 "\n", sym->st_size);
	}

	f.Close();
}

static void DbgDumpRela(const std::string& folder, Elf64_Rela* records, uint64_t size,
                        const char* /*names*/, const char* file_name) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + file_name);

	for (auto* r = records;
	     reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++) {
		f.Printf("----\n"
		         "r_offset = 0x%016" PRIx64 "\n"
		         "r_info = 0x%016" PRIx64 "\n"
		         "r_addend = %" PRId64 "\n",
		         r->r_offset, r->r_info, r->r_addend);
	}

	f.Close();
}

static Common::VirtualMemory::Mode GetMode(Elf64_Word flags) {
	switch (flags) {
		case PF_R: return Common::VirtualMemory::Mode::Read;
		case PF_W: return Common::VirtualMemory::Mode::Write;
		case PF_R | PF_W: return Common::VirtualMemory::Mode::ReadWrite;
		case PF_X: return Common::VirtualMemory::Mode::Execute;
		case PF_X | PF_R: return Common::VirtualMemory::Mode::ExecuteRead;
		case PF_X | PF_W: return Common::VirtualMemory::Mode::ExecuteWrite;
		case PF_X | PF_W | PF_R: return Common::VirtualMemory::Mode::ExecuteReadWrite;

		default: return Common::VirtualMemory::Mode::NoAccess;
	}
}

struct GuestStackFrame {
	GuestStackFrame* next;
	uintptr_t        return_address;
};

static bool IsReadableRange(uint64_t addr, uint64_t size);

static int WalkGuestStack(uint64_t rbp, uint64_t rsp, void** stack, int capacity) {
	constexpr uintptr_t STACK_SIZE = 1024u * 1024u;
	const uintptr_t     code_start = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	const uintptr_t     stack_end  = rsp + STACK_SIZE;
	if (stack == nullptr || capacity <= 0 || rsp == 0 || rbp < rsp || stack_end < rsp ||
	    g_desired_base_addr <= code_start) {
		return 0;
	}

	auto* frame = reinterpret_cast<GuestStackFrame*>(rbp);

	int depth = 0;
	while (depth < capacity) {
		const auto frame_addr = reinterpret_cast<uintptr_t>(frame);
		if (frame_addr < rsp || frame_addr > stack_end - sizeof(GuestStackFrame) ||
		    !IsReadableRange(frame_addr, sizeof(GuestStackFrame)) ||
		    frame->return_address < code_start || frame->return_address >= g_desired_base_addr) {
			break;
		}
		stack[depth++] = reinterpret_cast<void*>(frame->return_address);
		if (reinterpret_cast<uintptr_t>(frame->next) <= frame_addr) {
			break;
		}
		frame = frame->next;
	}
	return depth;
}

// Probe diagnostic ranges without raising another fault.
static bool IsReadableRange(uint64_t addr, uint64_t size) {
	if (addr == 0 || size == 0) {
		return false;
	}

	const uint64_t end = addr + size;
	if (end < addr) {
		return false;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uint64_t current = addr;
	while (current < end) {
		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) == 0 ||
		    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
			return false;
		}
		const auto region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (region_end <= current) {
			return false;
		}
		current = std::min(region_end, end);
	}
#elif defined(__APPLE__)
	// Walk the Mach regions covering the range and require read permission. The fatal
	// report dumps memory behind raw register values, and a fault inside the reporter
	// re-enters the signal handler and wedges the reporting thread.
	uint64_t current = addr;
	while (current < end) {
		mach_vm_address_t              region_addr = current;
		mach_vm_size_t                 region_size = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t                    object_name = MACH_PORT_NULL;
		if (mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
		                   reinterpret_cast<vm_region_info_t>(&info), &count,
		                   &object_name) != KERN_SUCCESS ||
		    region_addr > current || (info.protection & VM_PROT_READ) == 0) {
			return false;
		}
		current = region_addr + region_size;
	}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	const auto page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
	if (page_size == 0) {
		return false;
	}

	for (uint64_t current = addr; current < end;) {
		uint8_t probe = 0;

		iovec local {&probe, sizeof(probe)};
		iovec remote {reinterpret_cast<void*>(current), sizeof(probe)};

		if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) !=
		    static_cast<ssize_t>(sizeof(probe))) {
			return false;
		}

		const uint64_t next = (current & ~(page_size - 1)) + page_size;
		if (next <= current) { // wrapped at the top of the address space
			break;
		}
		current = next;
	}
#else
	(void)end;
#endif
	return true;
}

static bool KytyExceptionHandler(const Common::HostException::ExceptionInfo& exception_info) {
	const auto* info = &exception_info;

	if (info->type == Common::HostException::ExceptionType::IllegalInstruction &&
	    Loader::X64InstructionEmulator::TryEmulate(info->native_context)) {
		return true;
	}

	if (info->type == Common::HostException::ExceptionType::AccessViolation) {
		using CoreAccess = Common::HostException::AccessViolationType;
		using GpuAccess  = Libs::Graphics::PageFaultAccess;
		GpuAccess access;
		switch (info->access_violation_type) {
			case CoreAccess::Read: access = GpuAccess::Read; break;
			case CoreAccess::Write: access = GpuAccess::Write; break;
			case CoreAccess::Execute: access = GpuAccess::Execute; break;
			case CoreAccess::Unknown: return false;
		}
		if (Libs::LibKernel::Memory::HandleGpuFault(access, info->access_violation_vaddr)) {
			return true;
		}
	}
	EXIT("Unhandled host exception: type=%u code=%u pc=0x%016" PRIx64
	     " access=%u address=0x%016" PRIx64 "\n",
	     static_cast<unsigned>(info->type), info->native_code, info->exception_address,
	     static_cast<unsigned>(info->access_violation_type), info->access_violation_vaddr);
}

static void EncodeId64(uint16_t in_id, std::string* out_id) {
	static const char* str = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	if (in_id < 0x40u) {
		*out_id += str[in_id];
	} else {
		if (in_id < 0x1000u) {
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		} else {
			*out_id += str[static_cast<uint16_t>(in_id >> 12u) & 0x3fu];
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		}
	}
}

template <class T>
static void GetDynDataOs(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = elf->GetDynamicData<T>(dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynData(Elf64* elf, uint64_t base_vaddr, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = reinterpret_cast<T>(base_vaddr + dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynValue(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_val;
	}
}

template <class T>
static void GetDynValues(Elf64* elf, T* out, Elf64_Sxword tag) {
	for (const auto* dyn: elf->GetDynList(tag)) {
		out->push_back(dyn->d_un.d_val);
	}
}

template <class T>
static void GetDynPtr(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_ptr;
	}
}

static void KYTY_SYSV_ABI ProgramExitHandler() {
	Common::Singleton<RuntimeLinker>::Instance()->StopAllModules();

	LOGF("exit!!!\n");
}

template <class T>
static void GetDynModules(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		ModuleId id {};
		// id.id            = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version_major = static_cast<int>((need >> 40u) & 0xffu);
		id.version_minor = static_cast<int>((need >> 32u) & 0xffu);
		id.name          = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

template <class T>
static void GetDynLibs(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		LibraryId id {};
		// id.id      = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version = static_cast<int>((need >> 32u) & 0xffffu);
		id.name    = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

static RelocationInfo GetRelocationInfo(Elf64_Rela* r, Program* program) {
	KYTY_PROFILER_FUNCTION();

	// KYTY_PROFILER_BLOCK("1");

	RelocationInfo ret;
	// SymbolRecord   sr {};

	// KYTY_PROFILER_END_BLOCK;

	// KYTY_PROFILER_BLOCK("2");

	auto         type    = r->GetType();
	auto         symbol  = r->GetSymbol();
	Elf64_Sxword addend  = r->r_addend;
	auto*        symbols = program->dynamic_info->symbol_table;
	auto*        names   = program->dynamic_info->str_table;
	ret.base_vaddr       = program->base_vaddr;
	ret.vaddr            = ret.base_vaddr + r->r_offset;
	ret.bind_self        = false;

	// KYTY_PROFILER_END_BLOCK;

	// KYTY_PROFILER_BLOCK("3");

	switch (type) {
		case R_X86_64_GLOB_DAT:
		case R_X86_64_JUMP_SLOT: addend = 0; [[fallthrough]];
		case R_X86_64_64: {
			auto         sym          = symbols[symbol];
			auto         bind         = sym.GetBind();
			auto         sym_type     = sym.GetType();
			uint64_t     symbol_vaddr = 0;
			SymbolRecord sr {};
			switch (sym_type) {
				case STT_NOTYPE: ret.type = SymbolType::NoType; break;
				case STT_FUNC: ret.type = SymbolType::Func; break;
				case STT_OBJECT: ret.type = SymbolType::Object; break;
				default: EXIT("unknown symbol type: %d\n", (int)sym_type);
			}
			switch (bind) {
				case STB_LOCAL:
					symbol_vaddr = ret.base_vaddr + sym.st_value;
					ret.bind     = BindType::Local;
					break;
				case STB_GLOBAL: ret.bind = BindType::Global; [[fallthrough]];
				case STB_WEAK: {
					ret.bind = (ret.bind == BindType::Unknown ? BindType::Weak : ret.bind);
					ret.name = names + sym.st_name;
					program->rt->Resolve(ret.name, ret.type, program, &sr, &ret.bind_self);
					symbol_vaddr = sr.vaddr;
				} break;
				default: EXIT("unknown bind: %d\n", (int)bind);
			}
			ret.resolved = (symbol_vaddr != 0);
			ret.value    = (ret.resolved ? symbol_vaddr + addend : 0);
			ret.name     = sr.name;
			ret.dbg_name = sr.dbg_name;
		} break;
		case R_X86_64_RELATIVE:
			ret.value    = ret.base_vaddr + addend;
			ret.resolved = true;
			break;
		case R_X86_64_DTPMOD64:
			ret.value    = reinterpret_cast<uint64_t>(program);
			ret.resolved = true;
			ret.type     = SymbolType::TlsModule;
			ret.bind     = BindType::Local;
			ret.dbg_name = Common::PathToString(program->file_name);
			break;
		default: EXIT("unknown type: %d\n", (int)type);
	}

	// KYTY_PROFILER_END_BLOCK;

	return ret;
}

static void RelocateRecord(uint32_t index, Elf64_Rela* r, Program* program, bool jmprela_table,
                           bool imports_only, std::vector<std::string>* unresolved) {
	KYTY_PROFILER_FUNCTION();

	auto ri = GetRelocationInfo(r, program);

	if (imports_only &&
	    (ri.bind_self || (ri.bind != BindType::Global && ri.bind != BindType::Weak))) {
		return;
	}

	[[maybe_unused]] bool patched        = false;
	bool                  stubbed_import = false;
	bool                  stubbed_func   = false;

	// KYTY_PROFILER_BLOCK("patch");

	if (ri.resolved) {
		patched = PatchGuestMemory64(ri.vaddr, ri.value);
	} else {
		uint64_t value = 0;
		bool     weak  = (ri.bind == BindType::Weak || !program->fail_if_global_not_resolved);
		if (ri.type == SymbolType::Object && weak) {
			value = g_invalid_memory;
		} else if (ri.type == SymbolType::Func && jmprela_table && weak) {
			value          = RegisterStubbedImport(index, program, ri);
			stubbed_import = true;
			stubbed_func   = true;
		} else if (ri.type == SymbolType::Func && !jmprela_table && weak) {
			value        = RegisterStubbedImport(index, program, ri);
			stubbed_func = true;
		} else if (ri.type == SymbolType::NoType && weak) {
			value = RuntimeLinker::ReadFromElf(program, ri.vaddr) + ri.base_vaddr;
		}

		if (value != 0) {
			patched = PatchGuestMemory64(ri.vaddr, value);
		} else {
			auto dbg_str = fmt::format("[{:016x}] <- {:016x}, {}, {}, {}, {}", ri.vaddr, ri.value,
			                           ri.name.c_str(), Common::EnumName(ri.type).c_str(),
			                           Common::EnumName(ri.bind).c_str(), ri.dbg_name.c_str());

			if (unresolved != nullptr) {
				unresolved->push_back(dbg_str);
			} else {
				EXIT("Can't resolve: %s\n", dbg_str.c_str());
			}

			if (ri.type == SymbolType::Object) {
				value = g_invalid_memory;
			} else if (ri.type == SymbolType::Func || ri.type == SymbolType::NoType) {
				value        = RegisterStubbedImport(index, program, ri);
				stubbed_func = true;
				if (jmprela_table) {
					stubbed_import = true;
				}
			}

			if (value != 0) {
				patched = PatchGuestMemory64(ri.vaddr, value);
			}
		}
	}

	// KYTY_PROFILER_END_BLOCK;

	if (patched && stubbed_import) {
		const auto thunk = RegisterStubbedImport(index, program, ri);
		LOGF("Relocate: unresolved PLT import patched to stub [%u] [%016" PRIx64 "] <- %016" PRIx64
		     ", %s, %s, %s, %s\n",
		     index, ri.vaddr, thunk, ri.name.c_str(), Common::EnumName(ri.type).c_str(),
		     Common::EnumName(ri.bind).c_str(), Common::PathToString(program->file_name).c_str());
	} else if (patched && stubbed_func) {
		const auto thunk = RegisterStubbedImport(index, program, ri);
		LOGF("Relocate: unresolved non-PLT function patched to stub [%u] [%016" PRIx64
		     "] <- %016" PRIx64 ", %s, %s, %s, %s\n",
		     index, ri.vaddr, thunk, ri.name.c_str(), Common::EnumName(ri.type).c_str(),
		     Common::EnumName(ri.bind).c_str(), Common::PathToString(program->file_name).c_str());
	}

	if (program->dbg_print_reloc) {
		if (/* !dbg_str.ContainsStr("libc_") && */ patched && !ri.bind_self &&
		    (ri.bind == BindType::Global || ri.bind == BindType::Weak ||
		     ri.type == SymbolType::TlsModule)) {
			auto dbg_str = fmt::format("[{:016x}] <- {:016x}, {}, {}, {}, {}", ri.vaddr, ri.value,
			                           ri.name.c_str(), Common::EnumName(ri.type).c_str(),
			                           Common::EnumName(ri.bind).c_str(), ri.dbg_name.c_str());

			LOGF("Relocate: %s\n", dbg_str.c_str());
		}
	}
}

static void RelocateRecords(Elf64_Rela* records, uint64_t size, Program* program,
                            bool jmprela_table, bool imports_only,
                            std::vector<std::string>* unresolved) {
	KYTY_PROFILER_FUNCTION();

	uint32_t index = 0;
	for (auto* r = records;
	     reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++, index++) {
		RelocateRecord(index, r, program, jmprela_table, imports_only, unresolved);
	}
}

__attribute__((naked)) static KYTY_SYSV_ABI void RelocateHandlerReturnStub() {
	asm volatile("addq $8, %rsp\n\t"
	             "retq\n");
}

static KYTY_SYSV_ABI uint64_t RelocateHandler(RelocateHandlerStack s) {
	auto*       stack     = s.stack;
	auto*       program   = reinterpret_cast<Program*>(stack[-1]);
	auto        rel_index = stack[0];
	std::string name      = "<unknown function>";

	if (program != nullptr && program->dynamic_info != nullptr &&
	    program->dynamic_info->jmprela_table != nullptr) {
		auto ri = GetRelocationInfo(program->dynamic_info->jmprela_table + rel_index, program);

		name = ri.name.c_str();
	}

	// Restore return address (for stack trace)
	stack[-1] = reinterpret_cast<uint64_t>(RelocateHandlerReturnStub);

	LOGF("=== Stubbed function, returning OK ===\n[%d]\t%s\n", Common::Thread::GetThreadIdUnique(),
	     name.c_str());
	return 0;
}

static KYTY_MS_ABI uint8_t* TlsMainGetAddr() {
	EXIT_IF(g_tls_main_program == nullptr);

	if (g_tls_cached_main_program == g_tls_main_program && g_tls_cached_main_tcb != nullptr) {
		return g_tls_cached_main_tcb;
	}

	g_tls_cached_main_program = g_tls_main_program;
	g_tls_cached_main_tcb =
	    RuntimeLinker::TlsGetAddr(g_tls_main_program) + g_tls_main_program->tls.tcb_offset;
	return g_tls_cached_main_tcb;
}

static void PatchProgram(Program* program, uint64_t address, uint64_t size) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);

	if (size >= 12) {
		// Replace guest stack-canary/errno stores through fs:[0x28] with nops.
		// Windows x64 cannot host guest FS directly, and an unpatched shared-library access faults
		// at address 0x28.
		const uint8_t fs_store_pattern[8] = {0x64, 0xc7, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00};
		auto*         start_ptr           = reinterpret_cast<uint8_t*>(address);
		auto*         end_ptr             = start_ptr + size - 12;

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			if (memcmp(ptr, fs_store_pattern, sizeof(fs_store_pattern)) == 0) {
				LOGF("Patch fs:[0x28] store at addr: [%016" PRIx64 "]\n",
				     reinterpret_cast<uint64_t>(ptr));
				if (ptr + 16 < start_ptr + size && ptr[12] == 0xcd && ptr[13] == 0x45 &&
				    ptr[14] == 0x90 && ptr[15] == 0x0f && ptr[16] == 0x0b) {
					ptr[0] = 0x5d; // pop rbp
					ptr[1] = 0xc3; // ret
					std::memset(ptr + 2, 0x90, 15);
				} else {
					std::memset(ptr, 0x90, 12);
				}
			}
		}
	}

	if (!program->elf->IsShared() && program->tls.handler_vaddr != 0 &&
	    size >= Jit::Call9::GetSize()) {
		// Replace:
		//   66 66 66
		//   mov <reg>, qword ptr fs:[0x00]
		// with:
		//   call <handler>
		//   mov <reg>,rax
		//   nop ...
		const uint8_t tls_pattern[5]       = {0x64, 0x48, 0x8B, 0x00, 0x25};
		const uint8_t zero_displacement[4] = {};

		EXIT_IF(Jit::Call9::GetSize() != 9);

		auto* start_ptr = reinterpret_cast<uint8_t*>(address);
		auto* end_ptr   = start_ptr + size - Jit::Call9::GetSize();

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			auto*  inst_ptr     = ptr;
			size_t prefix_count = 0;
			while (prefix_count < 3 && inst_ptr < start_ptr + size && *inst_ptr == 0x66) {
				inst_ptr++;
				prefix_count++;
			}

			if (inst_ptr + Jit::Call9::GetSize() > start_ptr + size) {
				break;
			}

			const uint8_t modrm = inst_ptr[3];
			if (memcmp(inst_ptr, tls_pattern, 3) == 0 && (modrm & 0xc7u) == 0x04u &&
			    inst_ptr[4] == tls_pattern[4] &&
			    memcmp(inst_ptr + 5, zero_displacement, sizeof(zero_displacement)) == 0) {
				LOGF("Patch tls at addr: [%016" PRIx64 "]\n", reinterpret_cast<uint64_t>(inst_ptr));

				const auto reg = (modrm >> 3u) & 7u;
				EXIT_NOT_IMPLEMENTED(reg == 4u);

				// A raw scan can encounter a 0x66 in the preceding instruction, so do not
				// overwrite it. Call9 starts with REX.W to neutralize genuine 0x66
				// prefixes on AMD processors (before it could turn E8 into callw 16bit).
				auto* code = new (inst_ptr) Jit::Call9;
				code->SetFunc(reg == 0
				                  ? program->tls.handler_vaddr
				                  : program->tls.handler_vaddr + Jit::TlsRegStub::GetOffset(reg));
				ptr += prefix_count + Jit::Call9::GetSize() - 1;
			}
		}
	}
}

uint64_t RuntimeLinker::GetEntry() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->elf->GetEntry() + p->base_vaddr;
		}
	}
	return 0;
}

uint64_t RuntimeLinker::GetProcParam() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->proc_param_vaddr;
		}
	}
	return 0;
}

void RuntimeLinker::DbgDump(const std::string& folder) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		auto folder_str = Common::FixDirectorySlash(folder);
		folder_str += Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name));

		EXIT_IF(p->elf == nullptr);

		p->elf->DbgDump(folder_str);

		if (p->dynamic_info != nullptr) {
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table_entry_size != 0 &&
			                     p->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table_entry_size != 0 &&
			                     p->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->jmprela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table == nullptr);

			if (p->dynamic_info->symbol_table != nullptr) {
				DbgDumpSymbols(folder_str, p->dynamic_info->symbol_table,
				               p->dynamic_info->symbol_table_total_size,
				               p->dynamic_info->str_table);
			}
			if (p->dynamic_info->jmprela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->jmprela_table,
				            p->dynamic_info->jmprela_table_size, p->dynamic_info->str_table,
				            "jmprela_table.txt");
			}
			if (p->dynamic_info->rela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->rela_table,
				            p->dynamic_info->rela_table_total_size, p->dynamic_info->str_table,
				            "rela_table.txt");
			}
		}

		if (p->export_symbols != nullptr) {
			p->export_symbols->DbgDump(folder_str, "export_symbols.txt");
		}
		if (p->import_symbols != nullptr) {
			p->import_symbols->DbgDump(folder_str, "import_symbols.txt");
		}
	}
}

void RuntimeLinker::RelocateAll() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		Relocate(p);
	}

	m_relocated = true;
}

void RuntimeLinker::RelocateProgram(Program* program) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(program == nullptr);
	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	Relocate(program);
	if (!GamePatch::ApplyPending(program)) {
		EXIT("Failed to apply pending game cheat\n");
	}
}

void RuntimeLinker::UnloadProgram(Program* program) {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	if (auto it = std::find(m_programs.begin(), m_programs.end(), program);
	    it != m_programs.end()) {
		DeleteProgram(*it);
		m_programs.erase(it);
	} else {
		EXIT("program not found");
	}

	if (m_relocated) {
		RelocateAll();
	}
}

RuntimeLinker::RuntimeLinker(): m_symbols(std::make_unique<SymbolDatabase>()) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RuntimeLinker::~RuntimeLinker() {
	Clear();
}

Program* RuntimeLinker::LoadProgram(const std::filesystem::path& elf_name) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	static int32_t id_seq = 0;

	LOGF("Loading: %s\n", Common::PathToString(elf_name).c_str());

	auto  program_owner = std::make_unique<Program>();
	auto* program       = program_owner.get();

	program->rt        = this;
	program->file_name = elf_name;
	program->unique_id = ++id_seq;

	program->elf = std::make_unique<Elf64>();
	program->elf->Open(elf_name);

	if (program->elf->IsValid()) {
		LoadProgramToMemory(program);
		ParseProgramDynamicInfo(program);
		CreateSymbolDatabase(program);
	} else {
		EXIT("elf is not valid: %s\n", Common::PathToString(elf_name).c_str());
	}

	m_programs.push_back(program_owner.release());

	if (!program->elf->IsShared()) {
		program->fail_if_global_not_resolved = false;
		Libs::LibKernel::SetProgName(elf_name.filename().string());
	}

	if (Common::EndsWith(Common::ToLower(Common::DirectoryWithoutFilename(
	                         Common::PathToGenericString(elf_name))),
	                     "_module/")) {
		program->fail_if_global_not_resolved = false;
	}

	return program;
}

void RuntimeLinker::SaveMainProgram(const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		EXIT_IF(p->elf == nullptr);

		if (!p->elf->IsShared()) {
			p->elf->Save(elf_name);
			break;
		}
	}
}

void RuntimeLinker::SaveProgram(Program* program, const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	if (auto it = std::find(m_programs.begin(), m_programs.end(), program);
	    it != m_programs.end()) {
		EXIT_IF((*it)->elf == nullptr);

		(*it)->elf->Save(elf_name);
	} else {
		EXIT("program not found");
	}
}

void RuntimeLinker::Execute(const std::filesystem::path& game_patch) {
	KYTY_PROFILER_THREAD("Thread_Main");

	Libs::LibKernel::PthreadInitSelfForMainThread();
	auto* main_stack_top = Libs::LibKernel::PthreadCreateMainGuestStack();

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Guest code has no Windows stack probes and may jump over the guard page. Module
	// initializers execute on the host stack too, so grow it before calling any guest code.
	size_t expanded_size = 0;
	while (expanded_size < static_cast<size_t>(768) * 1024) {
		sys_dbg_stack_info_t stack {};
		SysStackUsage(stack);
		*reinterpret_cast<uint32_t*>(stack.guard_addr) = 0;
		expanded_size += stack.guard_size;
	}
#endif

	PreloadAdjacentPrograms();
	RelocateAll();

	if (!game_patch.empty()) {
		if (!GamePatch::Apply(game_patch, m_programs.empty() ? nullptr : m_programs.front(),
		                      m_programs)) {
			EXIT("Failed to apply game cheat\n");
		}
	}
	StartAllModules();

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Execute: %s\n---\n", "Main");

	if (auto entry = GetEntry(); entry != 0) {
		auto* params = reinterpret_cast<EntryParams*>(
		    (reinterpret_cast<uintptr_t>(main_stack_top) - 0x100u) & ~static_cast<uintptr_t>(0x0f));
		std::memset(params, 0, sizeof(EntryParams));
		params->argc    = 1;
		params->argv[0] = "KytyEmu";

		LOGF("stack_addr = %" PRIx64 "\n", reinterpret_cast<uint64_t>(params));

		RunEntry(entry, params, ProgramExitHandler,
		         reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(params) - 0x1000u));
	}
}

void RuntimeLinker::Clear() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);
	GamePatch::Clear();

	for (auto* p: m_programs) {
		DeleteProgram(p);
	}
	m_programs.clear();
	for (const auto page: g_unresolved_stub_thunk_pages) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(page, UNRESOLVED_STUB_PAGE_SIZE));
	}
	g_unresolved_stub_thunk_pages.clear();
	g_unresolved_stub_thunk_offset = 0;
	g_stubbed_imports.clear();
	g_unresolved_stub_call_log_count.store(0);
	if (g_invalid_memory != 0) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(g_invalid_memory, 4096));
		g_invalid_memory = 0;
	}
	g_tls_main_program        = nullptr;
	g_tls_cached_main_program = nullptr;
	g_tls_cached_main_tcb     = nullptr;
	g_desired_base_addr       = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	m_symbols.reset();
	m_relocated = false;
}

void RuntimeLinker::Resolve(const std::string& name, SymbolType type, Program* program,
                            SymbolRecord* out_info, bool* bind_self) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	EXIT_IF(out_info == nullptr);

	auto ids = Common::Split(name, '#');

	if (bind_self != nullptr) {
		*bind_self = false;
	}

	if (ids.size() == 3) {
		const LibraryId* l = FindLibrary(*program, ids.at(1));
		const ModuleId*  m = FindModule(*program, ids.at(2));

		auto resolve_by_nid = [this, type](const std::string& nid, SymbolRecord* out) -> bool {
			EXIT_IF(out == nullptr);

			if (m_symbols != nullptr) {
				if (const auto* rec = m_symbols->FindByNid(nid, type); rec != nullptr) {
					*out = *rec;
					return true;
				}
			}

			for (auto* p: m_programs) {
				if (p != nullptr && p->export_symbols != nullptr) {
					if (const auto* rec = p->export_symbols->FindByNid(nid, type); rec != nullptr) {
						*out = *rec;
						return true;
					}
				}
			}

			return false;
		};

		if (l != nullptr && m != nullptr) {
			SymbolResolve sr {};
			sr.name                 = ids.at(0);
			sr.library              = l->name;
			sr.library_version      = l->version;
			sr.module               = m->name;
			sr.module_version_major = m->version_major;
			sr.module_version_minor = m->version_minor;
			sr.type                 = type;

			const SymbolRecord* rec = nullptr;

			if (m_symbols != nullptr) {
				rec = m_symbols->Find(sr);
			}

			if (rec == nullptr) {
				if (auto* p = FindProgram(*m, *l); p != nullptr && p->export_symbols != nullptr) {
					rec = p->export_symbols->Find(sr);
					if (bind_self != nullptr) {
						*bind_self = (p == program);
					}
				}
			}

			if (rec == nullptr) {
				if (resolve_by_nid(sr.name, out_info)) {
					LOGF("PS5 NID fallback: %s -> %s\n", sr.name.c_str(), out_info->name.c_str());
					return;
				}
			}

			if (rec != nullptr) {
				//*out_vaddr = rec->vaddr;
				*out_info = *rec;
			} else {
				out_info->vaddr    = 0;
				out_info->name     = SymbolDatabase::GenerateName(sr);
				out_info->dbg_name = "";
			}
		} else {
			if (resolve_by_nid(ids.at(0), out_info)) {
				LOGF("PS5 NID fallback: %s -> %s (missing lib/module metadata)\n",
				     ids.at(0).c_str(), out_info->name.c_str());
				return;
			}

			EXIT("l == nullptr || m == nullptr");
		}
	} else {
		out_info->vaddr    = 0;
		out_info->name     = name;
		out_info->dbg_name = "";
	}
}

bool RuntimeLinker::ResolveLoadedSymbolByNid(const std::string& nid, SymbolType type,
                                             SymbolRecord* out_info) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	EXIT_IF(out_info == nullptr);

	for (auto* p: m_programs) {
		if (p != nullptr && p->export_symbols != nullptr) {
			if (const auto* rec = p->export_symbols->FindByNid(nid, type); rec != nullptr) {
				*out_info = *rec;
				return true;
			}
		}
	}

	if (m_symbols != nullptr) {
		if (const auto* rec = m_symbols->FindByNid(nid, type); rec != nullptr) {
			*out_info = *rec;
			return true;
		}
	}

	return false;
}

uint64_t RuntimeLinker::ReadFromElf(Program* program, uint64_t vaddr) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->base_vaddr == 0 || program->base_size == 0);
	EXIT_IF(program->elf == nullptr);

	uint64_t ret = 0;

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr      = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size = phdr[i].p_filesz;

			if (vaddr >= segment_addr && vaddr < segment_addr + segment_file_size) {
				program->elf->LoadSegment(reinterpret_cast<uint64_t>(&ret),
				                          phdr[i].p_offset + vaddr - segment_addr, sizeof(ret));
				break;
			}
		}
	}

	return ret;
}

Program* RuntimeLinker::FindProgramById(int32_t id) {
	Common::LockGuard lock(m_mutex);

	// Id 0 is reserved for main program
	if (id == 0 && !m_programs.empty()) {
		return m_programs.front();
	}

	for (auto* p: m_programs) {
		if (p->unique_id == id) {
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByFileName(const std::filesystem::path& elf_name) {
	Common::LockGuard lock(m_mutex);

	auto fixed_name = Common::FixFilenameSlash(Common::PathToGenericString(elf_name));
	for (auto* p: m_programs) {
		if (Common::EqualNoCase(Common::FixFilenameSlash(Common::PathToGenericString(p->file_name)),
		                        fixed_name)) {
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByAddr(uint64_t vaddr) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		const auto* ehdr = p->elf->GetEhdr();
		const auto* phdr = p->elf->GetPhdr();

		EXIT_IF(phdr == nullptr || ehdr == nullptr);

		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
			if (phdr[i].p_memsz != 0 &&
			    (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
				uint64_t segment_addr = phdr[i].p_vaddr + p->base_vaddr;
				uint64_t segment_size = GetAlignedSize(phdr + i);

				if (vaddr >= segment_addr && vaddr < segment_addr + segment_size) {
					return p;
				}
			}
		}
	}

	return nullptr;
}

void RuntimeLinker::StackTrace(uint64_t frame_ptr, uint64_t stack_ptr) {
	void* stack[20];
	int   depth = WalkGuestStack(frame_ptr, stack_ptr, stack, static_cast<int>(std::size(stack)));

	LOGF("Stack trace [thread = %d]:\n", Common::Thread::GetThreadIdUnique());

	for (int i = 0; i < depth; i++) {
		auto  vaddr = reinterpret_cast<uint64_t>(stack[i]);
		auto* p     = FindProgramByAddr(vaddr);
		LOGF("[%d] %016" PRIx64 ", off=%016" PRIx64 ", %s\n", i, vaddr,
		     (p == nullptr ? 0 : vaddr - p->base_vaddr),
		     (p == nullptr
		          ? "???"
		          : Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name))
		                .c_str()));
	}
}

static std::string GetProgramModuleName(const Program* program) {
	EXIT_IF(program == nullptr);

	if (program->dynamic_info != nullptr && program->dynamic_info->so_name != nullptr &&
	    program->dynamic_info->so_name[0] != '\0') {
		return std::string(program->dynamic_info->so_name);
	}

	return Common::FilenameWithoutDirectory(Common::PathToGenericString(program->file_name));
}

static bool ModuleStartDependenciesSatisfied(const Program*               program,
                                             const std::vector<Program*>& programs,
                                             const std::vector<Program*>& started) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);

	for (const auto* needed: program->dynamic_info->needed) {
		if (needed == nullptr || needed[0] == '\0') {
			continue;
		}

		const auto needed_name = std::string(needed);

		for (auto* dependency: programs) {
			if (dependency == nullptr || dependency == program || dependency->elf == nullptr ||
			    !dependency->elf->IsShared()) {
				continue;
			}

			const auto dependency_name = GetProgramModuleName(dependency);
			if (Common::EqualNoCase(dependency_name, needed_name) ||
			    Common::EqualNoCase(Common::FilenameWithoutDirectory(
			                            Common::PathToGenericString(dependency->file_name)),
			                        needed_name)) {
				if (std::find(started.begin(), started.end(), dependency) == started.end()) {
					return false;
				}
				break;
			}
		}
	}

	return true;
}

void RuntimeLinker::StartAllModules() {
	Common::LockGuard lock(m_mutex);

	std::vector<Program*> started;

	for (;;) {
		bool progressed = false;

		for (auto* p: m_programs) {
			if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
			    std::find(started.begin(), started.end(), p) == started.end() &&
			    ModuleStartDependenciesSatisfied(p, m_programs, started)) {
				StartModule(p, 0, nullptr, nullptr);
				started.push_back(p);
				progressed = true;
			}
		}

		if (!progressed) {
			break;
		}
	}

	for (auto* p: m_programs) {
		if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
		    std::find(started.begin(), started.end(), p) == started.end()) {
			StartModule(p, 0, nullptr, nullptr);
			started.push_back(p);
		}
	}
}

void RuntimeLinker::StopAllModules() {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		if (p->elf->IsShared() && p->dynamic_info->fini_vaddr != 0) {
			StopModule(p, 0, nullptr, nullptr);
		}
	}
}

static bool IsAdjacentModuleFile(const std::string& name) {
	auto lower = Common::ToLower(name);
	return Common::EndsWith(lower, ".prx") || Common::EndsWith(lower, ".sprx");
}

static bool SkipAdjacentModuleFile(const std::string& name) {
	auto lower = Common::ToLower(name);
	return lower == "eboot.bin" || lower == "libkernel.prx" || lower == "libkernel_sys.prx";
}

void RuntimeLinker::PreloadAdjacentPrograms() {
	if (m_programs.empty()) {
		return;
	}

	std::vector<std::filesystem::path> module_paths;

	auto is_loaded = [this](const std::filesystem::path& path) {
		auto fixed_path = Common::FixFilenameSlash(Common::PathToGenericString(path));
		for (auto* program: m_programs) {
			if (Common::EqualNoCase(
			        Common::FixFilenameSlash(Common::PathToGenericString(program->file_name)),
			        fixed_path)) {
				return true;
			}
		}
		return false;
	};

	auto add_path = [&module_paths, &is_loaded](const std::filesystem::path& path) {
		if (is_loaded(path)) {
			return;
		}
		for (const auto& p: module_paths) {
			if (Common::EqualNoCase(Common::PathToGenericString(p),
			                        Common::PathToGenericString(path))) {
				return;
			}
		}
		module_paths.push_back(path);
	};

	auto add_dir = [&add_path](const std::filesystem::path& dir) {
		if (!Common::File::IsDirectoryExisting(dir)) {
			return;
		}
		for (const auto& entry: Common::File::GetDirEntries(dir)) {
			if (entry.is_file && IsAdjacentModuleFile(entry.name) &&
			    !SkipAdjacentModuleFile(entry.name)) {
				add_path(dir / entry.name);
			}
		}
	};

	auto root = m_programs.at(0)->file_name.parent_path();
	if (root.empty()) {
		return;
	}

	add_dir(root);
	add_dir(root / "sce_module");
	add_dir(root / "sce_modules");

	for (const auto& path: module_paths) {
		auto* program                        = LoadProgram(path);
		program->fail_if_global_not_resolved = false;
	}
}

int RuntimeLinker::StartModule(Program* program, size_t args, const void* argp,
                               module_func_t func) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Start module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

	return reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->init_vaddr +
	                                                program->base_vaddr)(args, argp, func);
}

int RuntimeLinker::StopModule(Program* program, size_t args, const void* argp, module_func_t func) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Stop module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

	int result = reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->fini_vaddr +
	                                                      program->base_vaddr)(args, argp, func);

	Libs::LibKernel::PthreadDeleteStaticObjects(program);

	return result;
}

uint8_t* RuntimeLinker::TlsGetAddr(Program* program) {
	EXIT_IF(program == nullptr);

	Common::LockGuard lock(program->tls.mutex);

	auto& tls = program->tls.tlss[Common::Thread::GetThreadIdUnique()];

	if (tls.ptr == nullptr) {
		constexpr uint64_t TCB_SIZE  = 0x40;
		constexpr uint64_t TCB_ALIGN = 0x20;

		const auto tcb_offset =
		    program->tls.tcb_offset != 0 ? program->tls.tcb_offset : program->tls.image_size;
		const auto alloc_size = AlignUp(tcb_offset, TCB_ALIGN) + TCB_SIZE;
		tls.ptr        = reinterpret_cast<uint8_t*>(Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, alloc_size, Common::VirtualMemory::Mode::ReadWrite, "thread_local_storage"));
		tls.free_func  = nullptr;
		tls.vm_alloc   = true;
		tls.alloc_size = alloc_size;

		EXIT_IF(tls.ptr == nullptr);

		std::memset(tls.ptr, 0, alloc_size);

		if (!program->tls.init_image.empty()) {
			std::memcpy(tls.ptr, program->tls.init_image.data(), program->tls.init_image.size());
		} else {
			std::memcpy(tls.ptr, reinterpret_cast<void*>(program->tls.image_vaddr),
			            program->tls.init_size);
		}

		auto* tcb = reinterpret_cast<uint64_t*>(tls.ptr + tcb_offset);
		tcb[0]    = reinterpret_cast<uint64_t>(tcb);
	}

	return tls.ptr;
}

void RuntimeLinker::DeleteTls(Program* program, int thread_id) {
	EXIT_IF(program == nullptr);

	if (thread_id == Common::Thread::GetThreadIdUnique() && g_tls_cached_main_program == program) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}

	Common::LockGuard lock(program->tls.mutex);

	if (auto it = program->tls.tlss.find(thread_id); it != program->tls.tlss.end()) {
		FreeTlsBlock(&it->second);
		program->tls.tlss.erase(it);
	}
}

static uint64_t CalcBaseSize(const Elf64_Ehdr* ehdr, const Elf64_Phdr* phdr) {
	uint64_t base_size = 0;
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t last_addr = phdr[i].p_vaddr + GetAlignedSize(phdr + i);
			if (last_addr > base_size) {
				base_size = last_addr;
			}
		}
	}
	return base_size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RuntimeLinker::LoadProgramToMemory(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || program->base_vaddr != 0 || program->base_size != 0 ||
	        program->elf == nullptr);

	// static uint64_t desired_base_addr = DESIRED_BASE_ADDR;

	bool is_shared   = program->elf->IsShared();
	bool is_next_gen = program->elf->IsNextGen();

	EXIT_NOT_IMPLEMENTED(!is_shared && !is_next_gen);

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	program->base_size                 = CalcBaseSize(ehdr, phdr);
	constexpr uint64_t GUEST_PAGE_SIZE = 0x4000;
	EXIT_IF(program->base_size > UINT64_MAX - (GUEST_PAGE_SIZE - 1));
	program->base_size_aligned = AlignUp(program->base_size, GUEST_PAGE_SIZE);

	uint64_t tls_handler_size = is_shared ? 0 : Jit::SafeCall::GetSize();
	EXIT_IF(tls_handler_size > UINT64_MAX - program->base_size_aligned);
	program->mapped_size = program->base_size_aligned + tls_handler_size;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const bool         use_red_zone_protection  = Config::RedZoneProtectionEnabled();
	constexpr uint64_t RED_ZONE_TRAMPOLINE_SIZE = 8u * 1024u * 1024u;
	if (use_red_zone_protection) {
		EXIT_IF(RED_ZONE_TRAMPOLINE_SIZE > UINT64_MAX - program->mapped_size);
		program->mapped_size += RED_ZONE_TRAMPOLINE_SIZE;
	}
#endif

	program->base_vaddr = Libs::LibKernel::Memory::AllocateProgramMemory(
	    g_desired_base_addr, program->mapped_size, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    Common::PathToString(program->file_name.filename()).c_str());
	EXIT_IF(program->base_vaddr == 0);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (use_red_zone_protection) {
		program->red_zone_trampoline_vaddr = program->base_vaddr + program->base_size_aligned;
		program->red_zone_trampoline_size  = RED_ZONE_TRAMPOLINE_SIZE;
		RegisterRedZonePatchModule(reinterpret_cast<void*>(program->base_vaddr),
		                           program->base_size_aligned,
		                           reinterpret_cast<void*>(program->red_zone_trampoline_vaddr),
		                           program->red_zone_trampoline_size);
	}
#endif
	if (!is_shared) {
		program->tls.handler_vaddr = program->base_vaddr + program->base_size_aligned;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		program->tls.handler_vaddr += program->red_zone_trampoline_size;
#endif
	}

	g_desired_base_addr += CODE_BASE_INCR * (1 + program->mapped_size / CODE_BASE_INCR);

	EXIT_IF(program->base_size_aligned < program->base_size);
	LOGF("base_vaddr             = 0x%016" PRIx64 "\n"
	     "base_size              = 0x%016" PRIx64 "\n"
	     "base_size_aligned      = 0x%016" PRIx64 "\n"
	     "mapped_size            = 0x%016" PRIx64 "\n",
	     program->base_vaddr, program->base_size, program->base_size_aligned, program->mapped_size);
	if (!is_shared) {
		LOGF("tls_handler_size       = 0x%016" PRIx64 "\n", tls_handler_size);
	}

	if (!Common::HostException::InstallHandler(KytyExceptionHandler)) {
		EXIT("Failed to install the required vectored exception handler\n");
	}

	// program->elf->SetBaseVAddr(program->base_vaddr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::vector<std::pair<uint64_t, uint64_t>> executable_segments;
	uint64_t                                   eh_frame_header_addr = 0;
	uint64_t                                   eh_frame_header_size = 0;
#endif

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr        = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size   = phdr[i].p_filesz;
			uint64_t segment_memory_size = GetAlignedSize(phdr + i);
			auto     mode                = GetMode(phdr[i].p_flags);

			LOGF("[%d] addr        = 0x%016" PRIx64 "\n"
			     "[%d] file_size   = %" PRIu64 "\n"
			     "[%d] memory_size = %" PRIu64 "\n"
			     "[%d] mode        = %s\n",
			     i, segment_addr, i, segment_file_size, i, segment_memory_size, i,
			     Common::EnumName(mode).c_str());

			program->elf->LoadSegment(segment_addr, phdr[i].p_offset, segment_file_size);

			bool skip_protect = (phdr[i].p_type == PT_LOAD && is_next_gen &&
			                     mode == Common::VirtualMemory::Mode::NoAccess);

			if (Common::VirtualMemory::IsExecute(mode)) {
				PatchProgram(program, segment_addr, segment_memory_size);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
				if (use_red_zone_protection) {
					executable_segments.emplace_back(segment_addr, segment_file_size);
				}
#endif
			}

			if (!skip_protect) {
				Libs::LibKernel::Memory::SetProgramMemoryProtection(segment_addr,
				                                                    segment_memory_size, mode);

				if (Common::VirtualMemory::IsExecute(mode)) {
					Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_memory_size);
				}
			}
		}

		if (phdr[i].p_type == PT_TLS) {
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->tls.image_vaddr = phdr[i].p_vaddr + program->base_vaddr;
			program->tls.init_size   = std::min(phdr[i].p_filesz, GetAlignedSize(phdr + i));
			program->tls.image_size  = GetAlignedSize(phdr + i);
			program->tls.tcb_offset  = program->tls.image_size;

			LOGF("tls addr = 0x%016" PRIx64 "\n"
			     "tls init   = %" PRIu64 "\n"
			     "tls size   = %" PRIu64 "\n"
			     "tls offset = %" PRIu64 "\n",
			     program->tls.image_vaddr, program->tls.init_size, program->tls.image_size,
			     program->tls.tcb_offset);
		}

		if (phdr[i].p_type == PT_OS_PROCPARAM) {
			EXIT_IF(program->proc_param_vaddr != 0);
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->proc_param_vaddr = phdr[i].p_vaddr + program->base_vaddr;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (use_red_zone_protection && phdr[i].p_type == PT_GNU_EH_FRAME) {
			eh_frame_header_addr = phdr[i].p_vaddr + program->base_vaddr;
			eh_frame_header_size = phdr[i].p_memsz;
		}
#endif
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (use_red_zone_protection) {
		std::vector<uintptr_t> function_starts;
		if (!DecodeEhFrameFunctionStarts(eh_frame_header_addr, eh_frame_header_size,
		                                 &function_starts)) {
			LOGF("Windows guest red-zone patching could not decode function boundaries for %s\n",
			     Common::PathToString(program->file_name).c_str());
		}
		for (const auto& [segment_addr, segment_size]: executable_segments) {
			const auto result =
			    PatchRedZoneMemoryInstructions(segment_addr, segment_size, function_starts);
			LOGF("Windows guest red-zone patching: %s, functions=%" PRIu64 ", red_zone=%" PRIu64
			     ", memory=%" PRIu64 ", patched=%" PRIu64 ", short=%" PRIu64 ", stack=%" PRIu64
			     ", control=%" PRIu64 ", unrelocatable=%" PRIu64 "\n",
			     Common::PathToString(program->file_name.filename()).c_str(), result.function_count,
			     result.red_zone_function_count, result.memory_instruction_count,
			     result.patched_memory_instruction_count, result.short_memory_instruction_count,
			     result.stack_dependent_memory_instruction_count,
			     result.control_flow_memory_instruction_count,
			     result.unrelocatable_memory_instruction_count);
			Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_size);
		}
		Common::VirtualMemory::FlushInstructionCache(program->red_zone_trampoline_vaddr,
		                                             program->red_zone_trampoline_size);
	}
#endif

	if (!is_shared) {
		SetupTlsHandler(program);
	}

	LOGF("entry = 0x%016" PRIx64 "\n", program->elf->GetEntry() + program->base_vaddr);
}

void RuntimeLinker::DeleteProgram(Program* p) {
	auto program = std::unique_ptr<Program>(p);
	if (g_tls_main_program == program.get()) {
		g_tls_main_program = nullptr;
	}
	if (g_tls_cached_main_program == program.get()) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}
	for (auto& record: g_stubbed_imports) {
		if (record.patch_vaddr >= program->base_vaddr &&
		    record.patch_vaddr < program->base_vaddr + program->mapped_size) {
			record.patch_vaddr = 0;
		}
	}

	if (program->base_vaddr != 0 || program->mapped_size != 0) {
		EXIT_IF(program->base_vaddr == 0 || program->mapped_size == 0);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (program->red_zone_trampoline_size != 0) {
			UnregisterRedZonePatchModule(reinterpret_cast<void*>(program->base_vaddr));
		}
#endif
		EXIT_IF(
		    !Libs::LibKernel::Memory::FreeGuestMemory(program->base_vaddr, program->mapped_size));
	}

	if (program->custom_call_plt_vaddr != 0 || program->custom_call_plt_num != 0) {
		const auto size = Jit::CallPlt::GetSize(program->custom_call_plt_num);
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(program->custom_call_plt_vaddr, size));
	}
}

void RuntimeLinker::ParseProgramDynamicInfo(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->dynamic_info != nullptr);

	program->dynamic_info = std::make_unique<DynamicInfo>();

	auto* elf = program->elf.get();

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_HASH) && elf->HasDynValue(DT_HASH));
	GetDynDataOs(elf, &program->dynamic_info->hash_table, DT_OS_HASH);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->hash_table, DT_HASH);
	GetDynValue(elf, &program->dynamic_info->hash_table_size, DT_OS_HASHSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRTAB) && elf->HasDynValue(DT_STRTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRSZ) && elf->HasDynValue(DT_STRSZ));
	GetDynDataOs(elf, &program->dynamic_info->str_table, DT_OS_STRTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->str_table, DT_STRTAB);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_OS_STRSZ);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_STRSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMTAB) && elf->HasDynValue(DT_SYMTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMENT) && elf->HasDynValue(DT_SYMENT));
	GetDynDataOs(elf, &program->dynamic_info->symbol_table, DT_OS_SYMTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->symbol_table, DT_SYMTAB);
	GetDynValue(elf, &program->dynamic_info->symbol_table_total_size, DT_OS_SYMTABSZ);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_OS_SYMENT);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_SYMENT);

	GetDynPtr(elf, &program->dynamic_info->init_vaddr, DT_INIT);
	GetDynPtr(elf, &program->dynamic_info->fini_vaddr, DT_FINI);
	GetDynPtr(elf, &program->dynamic_info->init_array_vaddr, DT_INIT_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->fini_array_vaddr, DT_FINI_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->preinit_array_vaddr, DT_PREINIT_ARRAY);
	GetDynValue(elf, &program->dynamic_info->init_array_size, DT_INIT_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->fini_array_size, DT_FINI_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->preinit_array_size, DT_PREINIT_ARRAYSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTGOT) && elf->HasDynValue(DT_PLTGOT));
	GetDynPtr(elf, &program->dynamic_info->pltgot_vaddr, DT_OS_PLTGOT);
	GetDynPtr(elf, &program->dynamic_info->pltgot_vaddr, DT_PLTGOT);

	Elf64_Sxword jmprel_type = 0;
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTREL) && elf->HasDynValue(DT_PLTREL));
	GetDynValue(elf, &jmprel_type, DT_OS_PLTREL);
	GetDynValue(elf, &jmprel_type, DT_PLTREL);

	EXIT_NOT_IMPLEMENTED(jmprel_type != DT_RELA);
	if (jmprel_type == DT_RELA) {
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_JMPREL) && elf->HasDynValue(DT_JMPREL));
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTRELSZ) && elf->HasDynValue(DT_PLTRELSZ));
		GetDynDataOs(elf, &program->dynamic_info->jmprela_table, DT_OS_JMPREL);
		GetDynData(elf, program->base_vaddr, &program->dynamic_info->jmprela_table, DT_JMPREL);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_OS_PLTRELSZ);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_PLTRELSZ);
	}

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_RELA) && elf->HasDynValue(DT_RELA));
	GetDynDataOs(elf, &program->dynamic_info->rela_table, DT_OS_RELA);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->rela_table, DT_RELA);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_OS_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_OS_RELAENT);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_RELAENT);

	GetDynValue(elf, &program->dynamic_info->relative_count, DT_RELACOUNT);

	GetDynValue(elf, &program->dynamic_info->debug, DT_DEBUG);
	GetDynValue(elf, &program->dynamic_info->flags, DT_FLAGS);
	GetDynValue(elf, &program->dynamic_info->textrel, DT_TEXTREL);

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->debug != 0);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->textrel != 0);

	std::vector<uint64_t> needed;
	GetDynValues(elf, &needed, DT_NEEDED);
	for (auto need: needed) {
		program->dynamic_info->needed.push_back(program->dynamic_info->str_table + need);
	}

	uint64_t so_name = 0;
	GetDynValue(elf, &so_name, DT_SONAME);
	program->dynamic_info->so_name = program->dynamic_info->str_table + so_name;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_NEEDED_MODULE) &&
	                     elf->HasDynValue(DT_OS_NEEDED_MODULE_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_MODULE_INFO) &&
	                     elf->HasDynValue(DT_OS_MODULE_INFO_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_IMPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_IMPORT_LIB_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_EXPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_EXPORT_LIB_1));
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE);
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE_1);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO_1);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB_1);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB_1);
}

static void InstallRelocateHandler(Program* program) {
	KYTY_PROFILER_FUNCTION();

	uint64_t pltgot_vaddr = program->dynamic_info->pltgot_vaddr + program->base_vaddr;
	uint64_t pltgot_size  = static_cast<uint64_t>(3) * 8;
	void**   pltgot       = reinterpret_cast<void**>(pltgot_vaddr);

	Common::VirtualMemory::Mode old_mode {};
	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(
	    pltgot_vaddr, pltgot_size, Common::VirtualMemory::Mode::Write, &old_mode));

	pltgot[1] = program;
	pltgot[2] = reinterpret_cast<void*>(RelocateHandler);

	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(pltgot_vaddr, pltgot_size, old_mode));

	if (Common::VirtualMemory::IsExecute(old_mode)) {
		Common::VirtualMemory::FlushInstructionCache(pltgot_vaddr, pltgot_size);
	}

	// TODO(): check if this table already generated by compiler (sometimes it is missing)
	if (program->custom_call_plt_vaddr == 0) {
		program->custom_call_plt_num =
		    program->dynamic_info->jmprela_table_size / sizeof(Elf64_Rela);
		auto size                      = Jit::CallPlt::GetSize(program->custom_call_plt_num);
		program->custom_call_plt_vaddr = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    SYSTEM_RESERVED, size, Common::VirtualMemory::Mode::Write, "custom_call_plt");
		EXIT_NOT_IMPLEMENTED(program->custom_call_plt_vaddr == 0);
		auto* code = new (reinterpret_cast<void*>(program->custom_call_plt_vaddr))
		    Jit::CallPlt(program->custom_call_plt_num);
		code->SetPltGot(pltgot_vaddr);
		EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(program->custom_call_plt_vaddr, size,
		                                                     Common::VirtualMemory::Mode::Execute));
		Common::VirtualMemory::FlushInstructionCache(program->custom_call_plt_vaddr, size);
	}
}

void RuntimeLinker::Relocate(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);

	if (g_invalid_memory == 0) {
		g_invalid_memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    INVALID_MEMORY, 4096, Common::VirtualMemory::Mode::NoAccess, "invalid_memory", true);
		EXIT_NOT_IMPLEMENTED(g_invalid_memory == 0);
	}

	LOGF_COLOR(Log::Color::White, "--- Relocate program: %s ---\n",
	           Common::PathToString(program->file_name).c_str());

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->jmprela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->pltgot_vaddr == 0);

	InstallRelocateHandler(program);

	std::vector<std::string> unresolved;
	const bool               imports_only = program->relocated;

	RelocateRecords(program->dynamic_info->rela_table, program->dynamic_info->rela_table_total_size,
	                program, false, imports_only, &unresolved);
	RelocateRecords(program->dynamic_info->jmprela_table, program->dynamic_info->jmprela_table_size,
	                program, true, imports_only, &unresolved);
	program->relocated = true;

	if (program->tls.image_vaddr != 0 && program->tls.init_size != 0 &&
	    program->tls.init_image.empty()) {
		const auto* src = reinterpret_cast<const uint8_t*>(program->tls.image_vaddr);
		program->tls.init_image.assign(src, src + program->tls.init_size);
	}

	if (!unresolved.empty()) {
		LOGF("--- Stubbed unresolved imports: %zu ---\n", unresolved.size());
		for (const auto& symbol: unresolved) {
			LOGF("Stubbed: %s\n", symbol.c_str());
		}
	}
}

Program* RuntimeLinker::FindProgram(const ModuleId& m, const LibraryId& l) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		const auto& export_libs    = p->dynamic_info->export_libs;
		const auto& export_modules = p->dynamic_info->export_modules;

		if (std::find(export_libs.begin(), export_libs.end(), l) != export_libs.end() &&
		    std::find(export_modules.begin(), export_modules.end(), m) != export_modules.end()) {
			return p;
		}
	}
	return nullptr;
}

const ModuleId* RuntimeLinker::FindModule(const Program& program, const std::string& id) {
	const auto& import_modules = program.dynamic_info->import_modules;

	if (auto it = std::find_if(import_modules.begin(), import_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != import_modules.end()) {
		return &(*it);
	}

	const auto& export_modules = program.dynamic_info->export_modules;

	if (auto it = std::find_if(export_modules.begin(), export_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != export_modules.end()) {
		return &(*it);
	}

	return nullptr;
}

const LibraryId* RuntimeLinker::FindLibrary(const Program& program, const std::string& id) {
	const auto& import_libs = program.dynamic_info->import_libs;

	if (auto it = std::find_if(import_libs.begin(), import_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != import_libs.end()) {
		return &(*it);
	}

	const auto& export_libs = program.dynamic_info->export_libs;

	if (auto it = std::find_if(export_libs.begin(), export_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != export_libs.end()) {
		return &(*it);
	}

	return nullptr;
}

void RuntimeLinker::CreateSymbolDatabase(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->export_symbols != nullptr);
	EXIT_IF(program->import_symbols != nullptr);

	program->export_symbols = std::make_unique<SymbolDatabase>();
	program->import_symbols = std::make_unique<SymbolDatabase>();

	auto syms = [](Program* program, SymbolDatabase* symbols, bool is_export) {
		if (program->dynamic_info->symbol_table == nullptr ||
		    program->dynamic_info->str_table == nullptr) {
			return;
		}

		for (auto* sym = program->dynamic_info->symbol_table;
		     reinterpret_cast<uint8_t*>(sym) <
		     reinterpret_cast<uint8_t*>(program->dynamic_info->symbol_table) +
		         program->dynamic_info->symbol_table_total_size;
		     sym++) {
			std::string id   = std::string(program->dynamic_info->str_table + sym->st_name);
			auto        bind = sym->GetBind();
			auto        type = sym->GetType();
			auto        ids  = Common::Split(id, '#');

			if (ids.size() == 3) {
				const auto* l = FindLibrary(*program, ids.at(1));
				const auto* m = FindModule(*program, ids.at(2));

				if (l != nullptr && m != nullptr && (bind == STB_GLOBAL || bind == STB_WEAK) &&
				    (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
				    is_export == (sym->st_value != 0)) {
					SymbolResolve sr {};
					sr.name                 = ids.at(0);
					sr.library              = l->name;
					sr.library_version      = l->version;
					sr.module               = m->name;
					sr.module_version_major = m->version_major;
					sr.module_version_minor = m->version_minor;
					switch (type) {
						case STT_NOTYPE: sr.type = SymbolType::NoType; break;
						case STT_FUNC: sr.type = SymbolType::Func; break;
						case STT_OBJECT: sr.type = SymbolType::Object; break;
						default: sr.type = SymbolType::Unknown; break;
					}
					symbols->Add(sr, (is_export ? sym->st_value + program->base_vaddr : 0));
				}
			}
		}
	};

	syms(program, program->export_symbols.get(), true);
	syms(program, program->import_symbols.get(), false);
}

void RuntimeLinker::SetupTlsHandler(Program* program) {
	EXIT_IF(program == nullptr);
	EXIT_IF(g_tls_main_program != nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->elf->IsShared());
	EXIT_IF(program->tls.handler_vaddr == 0);

	g_tls_main_program = program;

	auto* code = new (reinterpret_cast<void*>(program->tls.handler_vaddr)) Jit::SafeCall;

	code->SetFunc(TlsMainGetAddr);

	for (uint8_t reg = 1; reg < 8; reg++) {
		if (reg == 4) {
			continue;
		}

		auto* stub = new (reinterpret_cast<void*>(program->tls.handler_vaddr +
		                                          Jit::TlsRegStub::GetOffset(reg))) Jit::TlsRegStub;
		stub->SetFunc(program->tls.handler_vaddr);
		stub->SetOutputReg(reg);
	}

	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(program->tls.handler_vaddr,
	                                                     Jit::SafeCall::GetSize(),
	                                                     Common::VirtualMemory::Mode::Execute));
	Common::VirtualMemory::FlushInstructionCache(program->tls.handler_vaddr,
	                                             Jit::SafeCall::GetSize());
}

void RuntimeLinker::DeleteTlss(int thread_id) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		DeleteTls(p, thread_id);
	}
}

void RuntimeLinker::SetApplicationHeapApi(void* const api[10]) {
	Common::LockGuard lock(m_mutex);

	if (api == nullptr || api[0] == nullptr || api[1] == nullptr) {
		m_application_heap_malloc         = nullptr;
		m_application_heap_free           = nullptr;
		m_application_heap_posix_memalign = nullptr;
		return;
	}

	m_application_heap_malloc = reinterpret_cast<application_heap_malloc_func_t>(api[0]);
	m_application_heap_free   = reinterpret_cast<application_heap_free_func_t>(api[1]);
	m_application_heap_posix_memalign =
	    reinterpret_cast<application_heap_posix_memalign_func_t>(api[6]);
}

void* RuntimeLinker::ApplicationHeapMemalign(uint64_t alignment, uint64_t size) {
	Common::LockGuard lock(m_mutex);

	if (m_application_heap_posix_memalign != nullptr) {
		void* ptr = nullptr;
		return m_application_heap_posix_memalign(&ptr, alignment, size) == 0 ? ptr : nullptr;
	}

	return nullptr;
}

void* RuntimeLinker::ApplicationHeapMalloc(uint64_t size) {
	Common::LockGuard lock(m_mutex);

	return m_application_heap_malloc != nullptr ? m_application_heap_malloc(size) : nullptr;
}

} // namespace Loader
