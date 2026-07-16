#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/eventFlag.h"
#include "kernel/eventQueue.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kernel/semaphore.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs {

namespace LibC {
int          GetArgc();
const char** GetArgv();
} // namespace LibC

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

namespace LibKernelApr {
LIB_DEFINE(InitLibKernel_1_Apr);
} // namespace LibKernelApr

namespace LibKernel {

using KernelModule                   = int32_t;
using get_thread_atexit_count_func_t = KYTY_SYSV_ABI int (*)(KernelModule);
using thread_atexit_report_func_t    = KYTY_SYSV_ABI void (*)(KernelModule);

static uint32_t sha1_rol(uint32_t value, uint32_t bits) {
	return (value << bits) | (value >> (32u - bits));
}

static std::array<uint8_t, 20> sha1_digest(const uint8_t* data, size_t size) {
	uint32_t h0 = 0x67452301u;
	uint32_t h1 = 0xefcdab89u;
	uint32_t h2 = 0x98badcfeu;
	uint32_t h3 = 0x10325476u;
	uint32_t h4 = 0xc3d2e1f0u;

	std::vector<uint8_t> msg(data, data + size);
	const uint64_t       bit_len = static_cast<uint64_t>(size) * 8u;

	msg.push_back(0x80u);
	while ((msg.size() % 64u) != 56u) {
		msg.push_back(0u);
	}
	for (int i = 7; i >= 0; i--) {
		msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xffu));
	}

	for (size_t offset = 0; offset < msg.size(); offset += 64u) {
		uint32_t w[80] = {};
		for (int i = 0; i < 16; i++) {
			const auto j = offset + static_cast<size_t>(i) * 4u;
			w[i] = (static_cast<uint32_t>(msg[j + 0]) << 24u) |
			       (static_cast<uint32_t>(msg[j + 1]) << 16u) |
			       (static_cast<uint32_t>(msg[j + 2]) << 8u) | static_cast<uint32_t>(msg[j + 3]);
		}
		for (int i = 16; i < 80; i++) {
			w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}

		uint32_t a = h0;
		uint32_t b = h1;
		uint32_t c = h2;
		uint32_t d = h3;
		uint32_t e = h4;

		for (int i = 0; i < 80; i++) {
			uint32_t f = 0;
			uint32_t k = 0;
			if (i < 20) {
				f = (b & c) | ((~b) & d);
				k = 0x5a827999u;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1u;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdcu;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6u;
			}

			const auto temp = sha1_rol(a, 5) + f + e + k + w[i];
			e               = d;
			d               = c;
			c               = sha1_rol(b, 30);
			b               = a;
			a               = temp;
		}

		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
	}

	std::array<uint8_t, 20> digest = {};
	const uint32_t          h[5]   = {h0, h1, h2, h3, h4};
	for (int i = 0; i < 5; i++) {
		digest[static_cast<size_t>(i) * 4u + 0u] = static_cast<uint8_t>((h[i] >> 24u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 1u] = static_cast<uint8_t>((h[i] >> 16u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 2u] = static_cast<uint8_t>((h[i] >> 8u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 3u] = static_cast<uint8_t>(h[i] & 0xffu);
	}
	return digest;
}

static std::string kernel_symbol_to_nid(const char* symbol) {
	static constexpr std::array<uint8_t, 16> salt = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde,
	                                                 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1,
	                                                 0xc3, 0xe5, 0x52, 0x30};
	static constexpr char                    codes[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

	const auto           symbol_size = std::strlen(symbol);
	std::vector<uint8_t> input(symbol_size + salt.size());
	std::memcpy(input.data(), symbol, symbol_size);
	std::memcpy(input.data() + symbol_size, salt.data(), salt.size());

	const auto hash = sha1_digest(input.data(), input.size());

	uint64_t digest = 0;
	std::memcpy(&digest, hash.data(), sizeof(digest));

	char nid[12] = {};
	for (int i = 0; i < 10; i++) {
		nid[i] = codes[(digest >> (58 - i * 6)) & 0x3fu];
	}
	nid[10] = codes[(digest & 0xfu) * 4u];
	return std::string(nid);
}

static const Loader::SymbolRecord* kernel_find_export_symbol(const Loader::SymbolDatabase* symbols,
                                                             const char*                   symbol) {
	EXIT_IF(symbols == nullptr);
	EXIT_IF(symbol == nullptr);

	const auto nid = kernel_symbol_to_nid(symbol);

	if (const auto* record = symbols->FindByNid(nid, Loader::SymbolType::Func); record != nullptr) {
		return record;
	}
	if (const auto* record = symbols->FindByNid(nid, Loader::SymbolType::Object);
	    record != nullptr) {
		return record;
	}
	if (const auto* record = symbols->FindByNid(nid, Loader::SymbolType::NoType);
	    record != nullptr) {
		return record;
	}

	const auto symbol_name = std::string(symbol);
	if (const auto* record = symbols->FindByName(symbol_name, Loader::SymbolType::Func);
	    record != nullptr) {
		return record;
	}
	if (const auto* record = symbols->FindByName(symbol_name, Loader::SymbolType::Object);
	    record != nullptr) {
		return record;
	}
	return symbols->FindByName(symbol_name, Loader::SymbolType::NoType);
}

static void* KYTY_SYSV_ABI KernelApplicationHeapGetMem(uint64_t alignment, uint64_t size) {
	static std::atomic_uint32_t log_count = 0;

	if (alignment < 0x10u) {
		alignment = 0x10u;
	}
	if ((alignment & (alignment - 1u)) != 0u) {
		return nullptr;
	}

	auto* rt  = Common::Singleton<Loader::RuntimeLinker>::Instance();
	void* ptr = rt->ApplicationHeapMemalign(alignment, size);

	const auto index = log_count.fetch_add(1, std::memory_order_relaxed);
	if (index < 8 || (index % 600) == 0) {
		LOGF("\t KernelApplicationHeapGetMem alignment = 0x%016" PRIx64 ", size = 0x%016" PRIx64
		     ", ptr = 0x%016" PRIx64 "\n",
		     alignment, size, reinterpret_cast<uint64_t>(ptr));
	}

	return ptr;
}

int KYTY_SYSV_ABI KernelDlsym(KernelModule handle, const char* symbol, void** addr) {
	static std::atomic_uint32_t log_count = 0;
	const auto                  index     = log_count.fetch_add(1, std::memory_order_relaxed);
	if (index < 32 || (index % 600) == 0) {
		PRINT_NAME();
		LOGF("\t call_count = %" PRIu32 ", handle = %" PRId32 ", symbol = %s\n", index + 1, handle,
		     symbol != nullptr ? symbol : "(null)");
	}

	if (addr == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (symbol == nullptr) {
		*addr = nullptr;
		return KERNEL_ERROR_EFAULT;
	}

	auto* rt      = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = rt->FindProgramById(handle);
	if (program == nullptr || program->export_symbols == nullptr) {
		*addr = nullptr;
		LOGF("\t unresolved dlsym: handle = %" PRId32 ", symbol = %s, reason = module not found\n",
		     handle, symbol);
		return KERNEL_ERROR_ESRCH;
	}

	const auto* record = kernel_find_export_symbol(program->export_symbols.get(), symbol);
	if (record != nullptr) {
		*addr = reinterpret_cast<void*>(record->vaddr);
		if (index < 32 || (index % 600) == 0) {
			LOGF("\t resolved = 0x%016" PRIx64 " (%s)\n", record->vaddr, record->name.c_str());
		}
		return OK;
	}

	if (handle == 0 && std::strcmp(symbol, "scriptingGetMem") == 0) {
		*addr = reinterpret_cast<void*>(&KernelApplicationHeapGetMem);
		if (index < 32 || (index % 600) == 0) {
			LOGF("\t resolved = 0x%016" PRIx64 " (application heap scriptingGetMem)\n",
			     reinterpret_cast<uint64_t>(*addr));
		}
		return OK;
	}

	*addr = nullptr;
	LOGF("\t unresolved dlsym: handle = %" PRId32 ", module = %s, symbol = %s\n", handle,
	     Common::PathToString(program->file_name).c_str(), symbol);
	return KERNEL_ERROR_ESRCH;
}

#pragma pack(1)

struct KernelLoadModuleOpt {
	size_t size;
};

struct KernelUnloadModuleOpt {
	size_t size;
};

struct TlsInfo {
	Loader::Program* program;
	uint64_t         offset;
};

struct MallocReplace {
	uint64_t size               = sizeof(MallocReplace);
	void*    malloc_initialize  = nullptr;
	void*    malloc_finalize    = nullptr;
	void*    malloc             = nullptr;
	void*    free               = nullptr;
	void*    calloc             = nullptr;
	void*    realloc            = nullptr;
	void*    memalign           = nullptr;
	void*    reallocalign       = nullptr;
	void*    posix_memalign     = nullptr;
	void*    malloc_stats       = nullptr;
	void*    malloc_stats_fast  = nullptr;
	void*    malloc_usable_size = nullptr;
	void*    aligned_alloc      = nullptr;
};

struct NewReplace {
	uint64_t size                           = sizeof(NewReplace);
	void*    new_p                          = nullptr;
	void*    new_nothrow                    = nullptr;
	void*    new_array                      = nullptr;
	void*    new_array_nothrow              = nullptr;
	void*    delete_p                       = nullptr;
	void*    delete_nothrow                 = nullptr;
	void*    delete_array                   = nullptr;
	void*    delete_array_nothrow           = nullptr;
	void*    delete_with_size               = nullptr;
	void*    delete_with_size_nothrow       = nullptr;
	void*    delete_array_with_size         = nullptr;
	void*    delete_array_with_size_nothrow = nullptr;
};

struct ModuleInfo {
	uint64_t     size;
	uint64_t     info[32];
	KernelModule handle;
	uint8_t      pad[156];
};

struct ModuleInfoForUnwind {
	uint64_t st_size;
	char     name[256];
	uint64_t eh_frame_hdr_addr;
	uint64_t eh_frame_addr;
	uint64_t eh_frame_size;
	uint64_t seg0_addr;
	uint64_t seg0_size;
};

static_assert(sizeof(ModuleInfoForUnwind) == 304);

#pragma pack()

constexpr size_t PROGNAME_MAX_SIZE = 511;

static uint64_t             g_stack_chk_guard                     = 0xDeadBeef00000007;
static char                 g_progname_buf[PROGNAME_MAX_SIZE + 1] = {0};
static const char*          g_progname                            = g_progname_buf;
static std::atomic_uint64_t g_gpo_state_bits {0};

static get_thread_atexit_count_func_t g_get_thread_atexit_count_func = nullptr;
static thread_atexit_report_func_t    g_thread_atexit_report_func    = nullptr;

void SetProgName(const std::string& name) {
	strncpy(g_progname_buf, name.c_str(), PROGNAME_MAX_SIZE);
}

static KYTY_SYSV_ABI int* get_error_addr() {
	return Posix::GetErrorAddr();
}

static KYTY_SYSV_ABI int getargc() {
	PRINT_NAME();

	return LibC::GetArgc();
}

static KYTY_SYSV_ABI const char** getargv() {
	PRINT_NAME();

	return LibC::GetArgv();
}

static void dump_stack_chk_fail_context(uint64_t caller, uint64_t rsp) {
	LOGF("stack_chk_fail caller = 0x%016" PRIx64 "\n", caller);
	LOGF("stack_chk_fail guard  = 0x%016" PRIx64 " @ 0x%016" PRIx64 "\n", g_stack_chk_guard,
	     reinterpret_cast<uint64_t>(&g_stack_chk_guard));

	auto* linker  = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = linker->FindProgramByAddr(caller);

	LOGF("stack_chk_fail rsp    = 0x%016" PRIx64 "\n"
	     "stack_chk_fail caller/saved-guard scan:\n",
	     rsp);
	for (int i = -32; i < 512; i++) {
		auto addr  = rsp + static_cast<int64_t>(i) * 8;
		auto value = *reinterpret_cast<uint64_t*>(addr);

		if (value == caller) {
			auto saved_guard_addr = addr + 0x4a8;
			auto saved_guard      = *reinterpret_cast<uint64_t*>(saved_guard_addr);
			LOGF("  return addr at 0x%016" PRIx64
			     "; saved guard candidate [ret+0x4a8] = 0x%016" PRIx64 " -> 0x%016" PRIx64 "\n",
			     addr, saved_guard_addr, saved_guard);
		}

		if (value == g_stack_chk_guard) {
			LOGF("  guard value found at stack 0x%016" PRIx64 "\n", addr);
		}
	}

	LOGF("stack_chk_fail stack qwords:\n");
	for (int i = -4; i < 20; i++) {
		auto  addr  = rsp + static_cast<int64_t>(i) * 8;
		auto  value = *reinterpret_cast<uint64_t*>(addr);
		auto* p     = linker->FindProgramByAddr(value);
		auto  module_name =
		    (p == nullptr ? std::string() : Common::PathToString(p->file_name.filename()));
		LOGF("  [%+03d] 0x%016" PRIx64 " : 0x%016" PRIx64 " %s\n", i, addr, value,
		     module_name.c_str());
	}

	if (program == nullptr) {
		LOGF("stack_chk_fail caller module = <unknown>\n");
		return;
	}

	auto caller_module_name = Common::PathToString(program->file_name.filename());
	LOGF("stack_chk_fail caller module = %s, base = 0x%016" PRIx64 ", offset = 0x%016" PRIx64 "\n",
	     caller_module_name.c_str(), program->base_vaddr, caller - program->base_vaddr);

	auto read_byte = [program](uint64_t vaddr) {
		auto     aligned = vaddr & ~0x07ull;
		uint64_t value   = Loader::RuntimeLinker::ReadFromElf(program, aligned);
		return static_cast<uint8_t>((value >> ((vaddr - aligned) * 8u)) & 0xffu);
	};

	if (caller >= 5 && read_byte(caller - 5) == 0xe8) {
		uint32_t rel = 0;
		for (uint32_t i = 0; i < 4; i++) {
			rel |= static_cast<uint32_t>(read_byte(caller - 4 + i)) << (i * 8u);
		}
		auto target = caller + static_cast<int32_t>(rel);
		LOGF("stack_chk_fail preceding call = rel32 at 0x%016" PRIx64 ", target = 0x%016" PRIx64
		     "\n",
		     caller - 5, target);
	} else if (caller >= 6 && read_byte(caller - 6) == 0xff &&
	           (read_byte(caller - 5) & 0x38u) == 0x10) {
		LOGF("stack_chk_fail preceding call = indirect at 0x%016" PRIx64 "\n", caller - 6);
	}

	auto dump_start = (caller >= 64 ? caller - 64 : 0);
	auto dump_end   = caller + 32;

	LOGF("stack_chk_fail caller bytes from ELF:\n");
	for (auto addr = dump_start; addr < dump_end; addr += 16) {
		LOGF("  0x%016" PRIx64 ":", addr);
		for (uint32_t i = 0; i < 16; i++) {
			LOGF("%c%02" PRIx8, (addr + i == caller ? '>' : ' '), read_byte(addr + i));
		}
		LOGF("\n");
	}
}

static KYTY_SYSV_ABI void stack_chk_fail() {
	PRINT_NAME();

	uint64_t rsp = 0;
	asm volatile("movq %%rsp, %0" : "=r"(rsp));
	dump_stack_chk_fail_context(reinterpret_cast<uint64_t>(__builtin_return_address(0)), rsp);

	EXIT("stack fail!!!");
}

static KYTY_SYSV_ABI int sigprocmask(int /*how*/, const void* /*set*/, void* /*oset*/) {
	// PRINT_NAME();

	// LOGF("\t how = %d\n", how);
	// LOGF("\t set = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(set));
	// LOGF("\t oset = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(oset));

	return 0;
}

static bool IsAllowedExceptionSignal(int signum) {
	constexpr int POSIX_SIGHUP  = 1;
	constexpr int POSIX_SIGILL  = 4;
	constexpr int POSIX_SIGFPE  = 8;
	constexpr int POSIX_SIGBUS  = 10;
	constexpr int POSIX_SIGSEGV = 11;
	constexpr int POSIX_SIGUSR1 = 30;

	return signum == POSIX_SIGHUP || signum == POSIX_SIGILL || signum == POSIX_SIGFPE ||
	       signum == POSIX_SIGBUS || signum == POSIX_SIGSEGV || signum == POSIX_SIGUSR1;
}

static void* g_exception_handlers[130] = {};

using exception_handler_func_t = KYTY_SYSV_ABI void (*)(int, void*);

static thread_local bool g_dispatching_signal_handler = false;

struct SignalDispatchScope {
	SignalDispatchScope(): previous(g_dispatching_signal_handler) {
		g_dispatching_signal_handler = true;
	}
	~SignalDispatchScope() { g_dispatching_signal_handler = previous; }

	bool previous;
};

bool KernelIsDispatchingSignalOnCurrentThread() {
	return g_dispatching_signal_handler;
}

static void QueuePendingSignal(Pthread thread, int signum) {
	PthreadQueuePendingSignal(thread, signum);
}

static void WaitForSignalDispatch(Pthread thread, int signum) {
	constexpr auto DISPATCH_WAIT_STEP = std::chrono::microseconds(1000);
	constexpr auto DISPATCH_WAIT_MAX  = std::chrono::milliseconds(2);

	auto waited = std::chrono::microseconds(0);
	while (PthreadHasPendingSignal(thread, signum) && waited < DISPATCH_WAIT_MAX) {
		Common::Thread::SleepMicro(DISPATCH_WAIT_STEP.count());
		waited += DISPATCH_WAIT_STEP;
	}
}

static bool TakePendingSignal(Pthread thread, int signum) {
	return PthreadTakePendingSignal(thread, signum);
}

struct SignalMcontext {
	uint64_t mc_onstack;
	uint64_t mc_rdi;
	uint64_t mc_rsi;
	uint64_t mc_rdx;
	uint64_t mc_rcx;
	uint64_t mc_r8;
	uint64_t mc_r9;
	uint64_t mc_rax;
	uint64_t mc_rbx;
	uint64_t mc_rbp;
	uint64_t mc_r10;
	uint64_t mc_r11;
	uint64_t mc_r12;
	uint64_t mc_r13;
	uint64_t mc_r14;
	uint64_t mc_r15;
	int      mc_trapno;
	uint16_t mc_fs;
	uint16_t mc_gs;
	uint64_t mc_addr;
	int      mc_flags;
	uint16_t mc_es;
	uint16_t mc_ds;
	uint64_t mc_err;
	uint64_t mc_rip;
	uint64_t mc_cs;
	uint64_t mc_rflags;
	uint64_t mc_reserved_0xb8[8];
	uint64_t mc_rsp;
	uint64_t mc_ss;
	uint64_t mc_len;
	uint64_t mc_fpformat;
	uint64_t mc_ownedfp;
	uint64_t mc_lbrfrom;
	uint64_t mc_lbrto;
	uint64_t mc_aux1;
	uint64_t mc_aux2;
	uint64_t mc_fpstate[104];
	uint64_t mc_fsbase;
	uint64_t mc_gsbase;
	uint64_t mc_spare[6];
};

struct SignalStack {
	void*    ss_sp;
	uint64_t ss_size;
	int      ss_flags;
	int      _align;
};

struct SignalUcontext {
	SignalMcontext uc_mcontext;
};

static_assert(offsetof(SignalUcontext, uc_mcontext) == 0x00);
static_assert(offsetof(SignalMcontext, mc_rip) == 0xa0);
static_assert(offsetof(SignalUcontext, uc_mcontext) + offsetof(SignalMcontext, mc_rsp) == 0xf8);

static SignalUcontext CreateSignalUcontext(
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    const CONTEXT* source_ctx = nullptr
#endif
) {
	SignalUcontext ctx = {};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	CONTEXT host_ctx = {};
	if (source_ctx != nullptr) {
		host_ctx = *source_ctx;
	} else {
		host_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
		RtlCaptureContext(&host_ctx);
	}

	ctx.uc_mcontext.mc_r8     = host_ctx.R8;
	ctx.uc_mcontext.mc_r9     = host_ctx.R9;
	ctx.uc_mcontext.mc_r10    = host_ctx.R10;
	ctx.uc_mcontext.mc_r11    = host_ctx.R11;
	ctx.uc_mcontext.mc_r12    = host_ctx.R12;
	ctx.uc_mcontext.mc_r13    = host_ctx.R13;
	ctx.uc_mcontext.mc_r14    = host_ctx.R14;
	ctx.uc_mcontext.mc_r15    = host_ctx.R15;
	ctx.uc_mcontext.mc_rdi    = host_ctx.Rdi;
	ctx.uc_mcontext.mc_rsi    = host_ctx.Rsi;
	ctx.uc_mcontext.mc_rbp    = host_ctx.Rbp;
	ctx.uc_mcontext.mc_rbx    = host_ctx.Rbx;
	ctx.uc_mcontext.mc_rdx    = host_ctx.Rdx;
	ctx.uc_mcontext.mc_rax    = host_ctx.Rax;
	ctx.uc_mcontext.mc_rcx    = host_ctx.Rcx;
	ctx.uc_mcontext.mc_rsp    = host_ctx.Rsp;
	ctx.uc_mcontext.mc_rip    = host_ctx.Rip;
	ctx.uc_mcontext.mc_cs     = host_ctx.SegCs;
	ctx.uc_mcontext.mc_fs     = host_ctx.SegFs;
	ctx.uc_mcontext.mc_gs     = host_ctx.SegGs;
	ctx.uc_mcontext.mc_ss     = host_ctx.SegSs;
	ctx.uc_mcontext.mc_ds     = host_ctx.SegDs;
	ctx.uc_mcontext.mc_es     = host_ctx.SegEs;
	ctx.uc_mcontext.mc_rflags = host_ctx.EFlags;
	ctx.uc_mcontext.mc_len    = sizeof(SignalMcontext);
#else
	ctx.uc_mcontext.mc_len = sizeof(SignalMcontext);
	uint64_t stack_marker  = 0;
	ctx.uc_mcontext.mc_rsp = reinterpret_cast<uint64_t>(&stack_marker);
	ctx.uc_mcontext.mc_rbp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
	ctx.uc_mcontext.mc_rip = reinterpret_cast<uint64_t>(__builtin_return_address(0));
#endif

	return ctx;
}

static SignalUcontext CreateCurrentGuestCallSignalUcontext(uint64_t rip) {
	auto ctx = CreateSignalUcontext();

	uint64_t rsp = 0;
	uint64_t rbp = 0;
	uint64_t rbx = 0;
	uint64_t r12 = 0;
	uint64_t r13 = 0;
	uint64_t r14 = 0;
	uint64_t r15 = 0;
	asm volatile("movq %%rsp, %0\n\t"
	             "movq %%rbp, %1\n\t"
	             "movq %%rbx, %2\n\t"
	             "movq %%r12, %3\n\t"
	             "movq %%r13, %4\n\t"
	             "movq %%r14, %5\n\t"
	             "movq %%r15, %6\n\t"
	             : "=r"(rsp), "=r"(rbp), "=r"(rbx), "=r"(r12), "=r"(r13), "=r"(r14), "=r"(r15)
	             :
	             : "memory");

	ctx.uc_mcontext.mc_rax = 0;
	ctx.uc_mcontext.mc_rcx = 0;
	ctx.uc_mcontext.mc_rdx = 0;
	ctx.uc_mcontext.mc_rsi = 0;
	ctx.uc_mcontext.mc_rdi = 0;
	ctx.uc_mcontext.mc_r8  = 0;
	ctx.uc_mcontext.mc_r9  = 0;
	ctx.uc_mcontext.mc_r10 = 0;
	ctx.uc_mcontext.mc_r11 = 0;
	ctx.uc_mcontext.mc_rbx = rbx;
	ctx.uc_mcontext.mc_rbp = rbp;
	ctx.uc_mcontext.mc_rsp = rsp;
	ctx.uc_mcontext.mc_r12 = r12;
	ctx.uc_mcontext.mc_r13 = r13;
	ctx.uc_mcontext.mc_r14 = r14;
	ctx.uc_mcontext.mc_r15 = r15;
	ctx.uc_mcontext.mc_rip = rip;

	return ctx;
}

static bool IsGuestCodeAddress(uint64_t addr) {
	return addr >= 0x0000000900000000ull && addr < 0x0000000980000000ull;
}

static bool IsAddressInRange(uint64_t addr, uint64_t base, uint64_t size) {
	return size != 0 && addr >= base && addr < base + size;
}

static uint64_t GetFallbackStackPointer(Pthread thread) {
	uint64_t stack_addr = 0;
	uint64_t stack_size = 0;
	if (!PthreadGetGuestStack(thread, &stack_addr, &stack_size)) {
		return 0;
	}

	const auto stack_top = (stack_addr + stack_size) & ~0xfull;
	if (stack_top <= stack_addr + 0x100) {
		return stack_top;
	}
	return stack_top - 0x100;
}

static void SanitizeNonGuestSignalUcontext(SignalUcontext* ctx, Pthread thread) {
	if (ctx == nullptr || IsGuestCodeAddress(ctx->uc_mcontext.mc_rip)) {
		return;
	}

	uint64_t   stack_addr = 0;
	uint64_t   stack_size = 0;
	const bool has_stack  = PthreadGetGuestStack(thread, &stack_addr, &stack_size);
	const bool rsp_valid =
	    has_stack && IsAddressInRange(ctx->uc_mcontext.mc_rsp, stack_addr, stack_size);
	const bool rbp_valid =
	    has_stack && IsAddressInRange(ctx->uc_mcontext.mc_rbp, stack_addr, stack_size);
	const auto fallback_sp = GetFallbackStackPointer(thread);

	ctx->uc_mcontext.mc_rax = 0;
	ctx->uc_mcontext.mc_rbx = 0;
	ctx->uc_mcontext.mc_rcx = 0;
	ctx->uc_mcontext.mc_rdx = 0;
	ctx->uc_mcontext.mc_rsi = 0;
	ctx->uc_mcontext.mc_rdi = 0;
	ctx->uc_mcontext.mc_rbp = (rbp_valid ? ctx->uc_mcontext.mc_rbp : fallback_sp);
	ctx->uc_mcontext.mc_rsp = (rsp_valid ? ctx->uc_mcontext.mc_rsp : fallback_sp);
	ctx->uc_mcontext.mc_r8  = 0;
	ctx->uc_mcontext.mc_r9  = 0;
	ctx->uc_mcontext.mc_r10 = 0;
	ctx->uc_mcontext.mc_r11 = 0;
	ctx->uc_mcontext.mc_r12 = 0;
	ctx->uc_mcontext.mc_r13 = 0;
	ctx->uc_mcontext.mc_r14 = 0;
	ctx->uc_mcontext.mc_r15 = 0;
	ctx->uc_mcontext.mc_rip = 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static void ApplySignalUcontext(CONTEXT* dst_ctx, const SignalUcontext& src_ctx) {
	if (dst_ctx == nullptr) {
		return;
	}

	dst_ctx->R8     = src_ctx.uc_mcontext.mc_r8;
	dst_ctx->R9     = src_ctx.uc_mcontext.mc_r9;
	dst_ctx->R10    = src_ctx.uc_mcontext.mc_r10;
	dst_ctx->R11    = src_ctx.uc_mcontext.mc_r11;
	dst_ctx->R12    = src_ctx.uc_mcontext.mc_r12;
	dst_ctx->R13    = src_ctx.uc_mcontext.mc_r13;
	dst_ctx->R14    = src_ctx.uc_mcontext.mc_r14;
	dst_ctx->R15    = src_ctx.uc_mcontext.mc_r15;
	dst_ctx->Rdi    = src_ctx.uc_mcontext.mc_rdi;
	dst_ctx->Rsi    = src_ctx.uc_mcontext.mc_rsi;
	dst_ctx->Rbp    = src_ctx.uc_mcontext.mc_rbp;
	dst_ctx->Rbx    = src_ctx.uc_mcontext.mc_rbx;
	dst_ctx->Rdx    = src_ctx.uc_mcontext.mc_rdx;
	dst_ctx->Rax    = src_ctx.uc_mcontext.mc_rax;
	dst_ctx->Rcx    = src_ctx.uc_mcontext.mc_rcx;
	dst_ctx->Rsp    = src_ctx.uc_mcontext.mc_rsp;
	dst_ctx->Rip    = src_ctx.uc_mcontext.mc_rip;
	dst_ctx->SegCs  = static_cast<DWORD>(src_ctx.uc_mcontext.mc_cs);
	dst_ctx->SegFs  = src_ctx.uc_mcontext.mc_fs;
	dst_ctx->SegGs  = src_ctx.uc_mcontext.mc_gs;
	dst_ctx->SegSs  = static_cast<DWORD>(src_ctx.uc_mcontext.mc_ss);
	dst_ctx->SegDs  = src_ctx.uc_mcontext.mc_ds;
	dst_ctx->SegEs  = src_ctx.uc_mcontext.mc_es;
	dst_ctx->EFlags = static_cast<DWORD>(src_ctx.uc_mcontext.mc_rflags);
}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
enum KytyQueueUserApcFlags : ULONG_PTR {
	KytyQueueUserApcFlagsNone            = 0,
	KytyQueueUserApcFlagsSpecialUserApc  = 1,
	KytyQueueUserApcFlagsCallbackContext = 0x00010000,
	KytyQueueUserApcFlagsMaxValue        = 2,
};

union KytyUserApcOption {
	ULONG_PTR UserApcFlags;
	HANDLE    MemoryReserveHandle;
};

using KytyPsApcRoutine = void (*)(void* apc_arg1, void* apc_arg2, void* apc_arg3, PCONTEXT context);
using NtQueueApcThreadExFunc = uint64_t(WINAPI*)(HANDLE thread, KytyUserApcOption option,
                                                 KytyPsApcRoutine routine, void* arg1, void* arg2,
                                                 void* arg3);

static NtQueueApcThreadExFunc GetNtQueueApcThreadEx() {
	static auto* nt_queue_apc_thread_ex = reinterpret_cast<NtQueueApcThreadExFunc>(
	    GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueueApcThreadEx"));
	return nt_queue_apc_thread_ex;
}

static void SignalApcHandler(void* arg1, void* arg2, void* /*arg3*/, PCONTEXT context) {
	auto*      thread = static_cast<Pthread>(arg1);
	const auto signum = static_cast<int>(reinterpret_cast<intptr_t>(arg2));
	if (!TakePendingSignal(thread, signum)) {
		return;
	}

	auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
	if (handler != nullptr) {
		SignalDispatchScope scope;
		auto                ctx           = CreateSignalUcontext(context);
		auto                guest_context = IsGuestCodeAddress(ctx.uc_mcontext.mc_rip);
		if (!guest_context) {
			SanitizeNonGuestSignalUcontext(&ctx, thread);
			std::thread([thread, signum, handler, ctx]() mutable {
				SignalDispatchScope helper_scope;
				auto*               previous_self = PthreadSwapSelfForSignal(thread);
				handler(signum, &ctx);
				PthreadSwapSelfForSignal(previous_self);
			}).detach();
			return;
		}
		handler(signum, &ctx);
		if (guest_context) {
			ApplySignalUcontext(context, ctx);
		}
	}
}

static bool DispatchSignalWithSuspendedThreadContext(HANDLE target_thread, Pthread thread,
                                                     int signum, exception_handler_func_t handler) {
	if (target_thread == nullptr || thread == nullptr || handler == nullptr) {
		return false;
	}

	if (!PthreadHasPendingSignal(thread, signum)) {
		return true;
	}

	if (SuspendThread(target_thread) == static_cast<DWORD>(-1)) {
		return false;
	}

	bool    dispatched = false;
	CONTEXT host_ctx {};
	host_ctx.ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
	if (GetThreadContext(target_thread, &host_ctx) != FALSE) {
		if (TakePendingSignal(thread, signum)) {
			auto   ctx                  = CreateSignalUcontext(&host_ctx);
			auto   guest_context        = IsGuestCodeAddress(ctx.uc_mcontext.mc_rip);
			HANDLE helper_thread_handle = nullptr;
			if (guest_context) {
				if (DuplicateHandle(GetCurrentProcess(), target_thread, GetCurrentProcess(),
				                    &helper_thread_handle, THREAD_SUSPEND_RESUME, FALSE,
				                    0) == FALSE) {
					ResumeThread(target_thread);
					return false;
				}
			} else {
				SanitizeNonGuestSignalUcontext(&ctx, thread);
				ResumeThread(target_thread);
			}

			std::thread([helper_thread_handle, thread, signum, handler, ctx]() mutable {
				SignalDispatchScope scope;
				auto*               previous_self = PthreadSwapSelfForSignal(thread);
				handler(signum, &ctx);
				PthreadSwapSelfForSignal(previous_self);
				if (helper_thread_handle != nullptr) {
					ResumeThread(helper_thread_handle);
					CloseHandle(helper_thread_handle);
				}
			}).detach();
			dispatched = true;
			return true;
		}
		dispatched = true;
	}

	ResumeThread(target_thread);

	return dispatched;
}
#endif

void KernelDispatchPendingSignalForCurrentThread() {
	Pthread current = PthreadSelfOrNull();
	if (current == nullptr) {
		return;
	}

	for (int signum = 0; signum < static_cast<int>(std::size(g_exception_handlers)); signum++) {
		if (!TakePendingSignal(current, signum)) {
			continue;
		}

		auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
		if (handler != nullptr) {
			SignalDispatchScope scope;
			auto                ctx = CreateSignalUcontext();
			SanitizeNonGuestSignalUcontext(&ctx, current);
			handler(signum, &ctx);
		}
		return;
	}
}

static int KYTY_SYSV_ABI KernelInstallExceptionHandler(int signum, void* handler) {
	PRINT_NAME();

	LOGF("\t signum  = %d\n"
	     "\t handler = 0x%016" PRIx64 "\n",
	     signum, reinterpret_cast<uint64_t>(handler));

	if (!IsAllowedExceptionSignal(signum)) {
		return KERNEL_ERROR_EINVAL;
	}
	if (g_exception_handlers[signum] != nullptr) {
		return KERNEL_ERROR_EAGAIN;
	}

	g_exception_handlers[signum] = handler;
	return OK;
}

static int KYTY_SYSV_ABI KernelRemoveExceptionHandler(int signum) {
	PRINT_NAME();

	LOGF("\t signum = %d\n", signum);

	if (!IsAllowedExceptionSignal(signum)) {
		return KERNEL_ERROR_EINVAL;
	}

	g_exception_handlers[signum] = nullptr;
	return OK;
}

static int KYTY_SYSV_ABI KernelRaiseException(Pthread thread, int signum) {
	constexpr int POSIX_SIGUSR1 = 30;

	if (signum != POSIX_SIGUSR1 || thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
	if (handler != nullptr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto target_thread_id       = PthreadGetHostThreadId(thread);
		auto*      nt_queue_apc_thread_ex = GetNtQueueApcThreadEx();
		if (target_thread_id == 0) {
			return KERNEL_ERROR_EINVAL;
		}
		if (target_thread_id == static_cast<uint64_t>(GetCurrentThreadId())) {
			SignalDispatchScope scope;
			auto                ctx = CreateCurrentGuestCallSignalUcontext(
			    reinterpret_cast<uint64_t>(__builtin_return_address(0)));
			handler(signum, &ctx);
			return OK;
		}
		if (nt_queue_apc_thread_ex == nullptr) {
			return KERNEL_ERROR_EINVAL;
		}

		HANDLE target_thread =
		    OpenThread(THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
		               static_cast<DWORD>(target_thread_id));
		if (target_thread == nullptr) {
			return KERNEL_ERROR_EINVAL;
		}

		QueuePendingSignal(thread, signum);
		KytyUserApcOption option {};
		option.UserApcFlags = KytyQueueUserApcFlagsSpecialUserApc;

		const auto status =
		    nt_queue_apc_thread_ex(target_thread, option, SignalApcHandler, thread,
		                           reinterpret_cast<void*>(static_cast<intptr_t>(signum)), nullptr);

		if (status != 0) {
			TakePendingSignal(thread, signum);
			CloseHandle(target_thread);
			LOGF("\t NtQueueApcThreadEx failed: target_os_thread=%" PRIu64 ", status=0x%016" PRIx64
			     "\n",
			     target_thread_id, status);
			return KERNEL_ERROR_EINVAL;
		}

		Common::CondVar::SignalThread(PthreadGetUniqueId(thread));
		PthreadWakeForSignal(thread);
		WaitForSignalDispatch(thread, signum);
		if (PthreadHasPendingSignal(thread, signum)) {
			DispatchSignalWithSuspendedThreadContext(target_thread, thread, signum, handler);
		}
		CloseHandle(target_thread);
		return OK;
#else
		auto ctx = CreateSignalUcontext();
		handler(signum, &ctx);
#endif
	}

	return OK;
}

static KYTY_SYSV_ABI KernelModule KernelLoadStartModule(const char* module_file_name, size_t args,
                                                        const void* argp, uint32_t flags,
                                                        const KernelLoadModuleOpt* opt, int* res) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	LOGF("\tmodule_file_name = %s\n", module_file_name);

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(opt != nullptr);

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto module_path = FileSystem::GetRealFilename(std::string(module_file_name));
	if (!Common::File::IsFileExisting(module_path)) {
		LOGF("\tmodule real path missing = %s\n", Common::PathToString(module_path).c_str());
		if (res != nullptr) {
			*res = KERNEL_ERROR_ENOENT;
		}
		return KERNEL_ERROR_ENOENT;
	}

	auto* program = rt->FindProgramByFileName(module_path);
	if (program != nullptr) {
		if (res != nullptr) {
			*res = OK;
		}
		return static_cast<KernelModule>(program->unique_id);
	}

	program = rt->LoadProgram(module_path);

	auto handle = program->unique_id;

	program->dbg_print_reloc = true;

	rt->RelocateProgram(program);

	int result = rt->StartModule(program, args, argp, nullptr);

	LOGF("\tmodule_start() result = %d\n", result);

	EXIT_NOT_IMPLEMENTED(result < 0);

	if (res != nullptr) {
		*res = result;
	}

	return static_cast<KernelModule>(handle);
}

static int KYTY_SYSV_ABI KernelStopUnloadModule(KernelModule handle, size_t args, const void* argp,
                                                uint32_t flags, const KernelUnloadModuleOpt* opt,
                                                int* res) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(opt != nullptr);

	auto* program = rt->FindProgramById(handle);

	if (program == nullptr) {
		LOGF("\tinvalid module handle = %" PRId32 "\n", handle);
		return OK;
	}

	if (g_get_thread_atexit_count_func != nullptr &&
	    g_get_thread_atexit_count_func(program->unique_id) > 0) {
		LOGF("KernelStopUnloadModule: cannot unload %s\n",
		     Common::PathToString(program->file_name).c_str());
		if (g_thread_atexit_report_func != nullptr) {
			g_thread_atexit_report_func(program->unique_id);
		}
		return KERNEL_ERROR_EBUSY;
	}

	int result = rt->StopModule(program, args, argp, nullptr);

	LOGF("\tmodule_stop() result = %d\n", result);

	EXIT_NOT_IMPLEMENTED(result < 0);

	if (res != nullptr) {
		*res = result;
	}

	rt->UnloadProgram(program);

	return OK;
}

static void* KYTY_SYSV_ABI tls_get_addr(TlsInfo* info) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	return Loader::RuntimeLinker::TlsGetAddr(info->program) + info->offset;
}

static void* KYTY_SYSV_ABI KernelGetProcParam() {
	PRINT_NAME();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance(); // NOLINT

	return reinterpret_cast<void*>(rt->GetProcParam());
}

static int KYTY_SYSV_ABI KernelIsTrinityMode() {
	static std::atomic_uint32_t call_count {0};

	auto count = call_count.fetch_add(1, std::memory_order_relaxed);
	if (count < 8 || (count % 1024) == 0) {
		PRINT_NAME();
		LOGF("\tcall = %u, return = 0 (base PS5 mode)\n", count + 1);
	}

	return 0;
}

static int KYTY_SYSV_ABI KernelFsync(int fd) {
	PRINT_NAME();

	LOGF("\t fd = %d\n", fd);

	return OK;
}

static void KYTY_SYSV_ABI KernelSync() {
	PRINT_NAME();
}

static int KYTY_SYSV_ABI getpid() {
	PRINT_NAME();

	return 100;
}

static void KYTY_SYSV_ABI KernelRtldSetApplicationHeapAPI(void* api[]) {
	PRINT_NAME();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	rt->SetApplicationHeapApi(api);

	if (api == nullptr) {
		return;
	}

	for (int i = 0; i < 10; i++) {
		LOGF("\tapi[%d] = 0x%016" PRIx64 "\n", i, reinterpret_cast<uint64_t>(api[i]));
	}

	[[maybe_unused]] auto* heap_malloc         = api[0];
	[[maybe_unused]] auto* heap_free           = api[1];
	[[maybe_unused]] auto* heap_posix_memalign = api[6];
}

static int64_t KYTY_SYSV_ABI write(int d, const char* str, int64_t size) {
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(d < 0);

	if (Network::Net::IsSocket(d)) {
		return Network::Net::Send(d, str, static_cast<uint64_t>(size), 0);
	}

	if (d > 2) {
		return POSIX_N_CALL(FileSystem::KernelWrite(d, str, size));
	}

	int size_int = static_cast<int>(size);

	return size_int;
}

static int KYTY_SYSV_ABI open(const char* path, int flags, int mode) {
	return POSIX_N_CALL(FileSystem::KernelOpen(path, flags, mode));
}

static int KYTY_SYSV_ABI close(int d) {
	if (d < 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	if (d <= 2) {
		return 0;
	}

	if (Network::Net::IsSocket(d)) {
		return POSIX_NET_CALL(Network::Net::SocketClose(d));
	}

	return POSIX_CALL(FileSystem::KernelClose(d));
}

static int64_t KYTY_SYSV_ABI read(int d, void* buf, uint64_t nbytes) {
	// PRINT_NAME();

	if (buf == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	if (d < 0 || d == 1 || d == 2) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	if (Network::Net::IsSocket(d)) {
		return Network::Net::Recv(d, buf, nbytes, 0);
	}

	if (d > 2) {
		return POSIX_N_CALL(FileSystem::KernelRead(d, buf, nbytes));
	}

	if (nbytes == 0) {
		return 0;
	}

	auto* str = std::fgets(static_cast<char*>(buf), static_cast<int>(nbytes), stdin);

	return (str != nullptr ? static_cast<int64_t>(strlen(str)) : 0);
}

static bool DecodeEhFramePointer(const uint8_t* data, const uint8_t* end, uint8_t encoding,
                                 uint64_t field_addr, uint64_t* value) {
	EXIT_IF(value == nullptr);

	if (encoding == 0xff) {
		return false;
	}

	uint64_t out       = 0;
	auto     format    = encoding & 0x0f;
	auto     app       = encoding & 0x70;
	bool     is_signed = false;

	switch (format) {
		case 0x00:
			if (data + sizeof(uint64_t) > end) {
				return false;
			}
			std::memcpy(&out, data, sizeof(uint64_t));
			break;
		case 0x03:
			if (data + sizeof(uint32_t) > end) {
				return false;
			}
			{
				uint32_t tmp = 0;
				std::memcpy(&tmp, data, sizeof(tmp));
				out = tmp;
			}
			break;
		case 0x04:
			if (data + sizeof(uint64_t) > end) {
				return false;
			}
			std::memcpy(&out, data, sizeof(uint64_t));
			break;
		case 0x0b:
			if (data + sizeof(int32_t) > end) {
				return false;
			}
			{
				int32_t tmp = 0;
				std::memcpy(&tmp, data, sizeof(tmp));
				out       = static_cast<uint64_t>(static_cast<int64_t>(tmp));
				is_signed = true;
			}
			break;
		case 0x0c:
			if (data + sizeof(int64_t) > end) {
				return false;
			}
			std::memcpy(&out, data, sizeof(int64_t));
			is_signed = true;
			break;
		default: return false;
	}

	if (app == 0x10) {
		out = (is_signed ? static_cast<uint64_t>(static_cast<int64_t>(field_addr) +
		                                         static_cast<int64_t>(out))
		                 : field_addr + out);
	} else if (app != 0) {
		return false;
	}

	*value = out;
	return true;
}

int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags,
                                               ModuleInfoForUnwind* info) {
	if (flags >= 3) {
		if (info != nullptr) {
			std::memset(info, 0, sizeof(ModuleInfoForUnwind));
		}
		return KERNEL_ERROR_EINVAL;
	}

	if (info == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (info->st_size < sizeof(ModuleInfoForUnwind)) {
		return KERNEL_ERROR_EINVAL;
	}

	auto* rt      = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = rt->FindProgramByAddr(addr);
	if (program == nullptr || program->elf == nullptr) {
		if (addr < 0x800000000ull) {
			// TODO(unwind): guest unwinding can reach a Kyty host return address below the guest VA
			// range. Report a synthetic boundary with no unwind tables so libc stops cleanly
			// instead of raising.
			std::memset(info, 0, sizeof(ModuleInfoForUnwind));
			info->st_size = sizeof(ModuleInfoForUnwind);
			std::snprintf(info->name, sizeof(info->name), "%s", "KytyHostBoundary");
			info->seg0_addr = addr & ~0xfffffull;
			info->seg0_size = 0x100000;
			return OK;
		}

		return KERNEL_ERROR_ESRCH;
	}

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	if (ehdr == nullptr || phdr == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	constexpr Loader::Elf64_Word PT_GNU_EH_FRAME = 0x6474e550;

	uint64_t eh_frame_hdr_size = 0;

	std::memset(info, 0, sizeof(ModuleInfoForUnwind));
	info->st_size = sizeof(ModuleInfoForUnwind);

	if (program->dynamic_info != nullptr && program->dynamic_info->so_name != nullptr &&
	    program->dynamic_info->so_name[0] != '\0') {
		std::snprintf(info->name, sizeof(info->name), "%s", program->dynamic_info->so_name);
	} else {
		auto fallback_name = Common::PathToString(program->file_name.filename());
		std::snprintf(info->name, sizeof(info->name), "%s", fallback_name.c_str());
	}

	uint64_t image_start = UINT64_MAX;
	uint64_t image_end   = 0;

	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz == 0) {
			continue;
		}

		if (phdr[i].p_type == Loader::PT_LOAD || phdr[i].p_type == Loader::PT_OS_RELRO) {
			const auto start = program->base_vaddr + phdr[i].p_vaddr;
			const auto end   = start + phdr[i].p_memsz;
			image_start      = std::min(image_start, start);
			image_end        = std::max(image_end, end);
		}

		if (phdr[i].p_type == PT_GNU_EH_FRAME) {
			info->eh_frame_hdr_addr = program->base_vaddr + phdr[i].p_vaddr;
			eh_frame_hdr_size       = phdr[i].p_memsz;
		}
	}

	const auto* shdr = program->elf->GetShdr();
	if (shdr != nullptr) {
		for (int i = 0; i < ehdr->e_shnum; i++) {
			const char* section_name = program->elf->GetSectionName(i);
			if (section_name != nullptr && std::strcmp(section_name, ".eh_frame") == 0) {
				info->eh_frame_addr = program->base_vaddr + shdr[i].sh_addr;
				info->eh_frame_size = shdr[i].sh_size;
				break;
			}
		}
	}

	if (info->eh_frame_hdr_addr != 0 && eh_frame_hdr_size >= 4) {
		const auto* hdr = reinterpret_cast<const uint8_t*>(info->eh_frame_hdr_addr);
		const auto* end = hdr + eh_frame_hdr_size;
		if (hdr[0] == 1) {
			uint64_t eh_frame_addr = 0;
			if (DecodeEhFramePointer(hdr + 4, end, hdr[1], info->eh_frame_hdr_addr + 4,
			                         &eh_frame_addr)) {
				info->eh_frame_addr = eh_frame_addr;
				if (info->eh_frame_size == 0 && info->eh_frame_hdr_addr > eh_frame_addr) {
					info->eh_frame_size = info->eh_frame_hdr_addr - eh_frame_addr;
				} else if (info->eh_frame_size == 0 &&
				           program->base_size_aligned >
				               info->eh_frame_hdr_addr - program->base_vaddr) {
					info->eh_frame_size = program->base_size_aligned -
					                      (info->eh_frame_hdr_addr - program->base_vaddr);
				}
			}
		}
	}

	if (image_start != UINT64_MAX && image_end > image_start) {
		info->seg0_addr = image_start;
		info->seg0_size = image_end - image_start;
	}

	if (info->eh_frame_addr != 0 && info->eh_frame_size == 0) {
		for (int i = 0; i < ehdr->e_phnum; i++) {
			if (phdr[i].p_memsz == 0 ||
			    (phdr[i].p_type != Loader::PT_LOAD && phdr[i].p_type != Loader::PT_OS_RELRO)) {
				continue;
			}

			const auto start = program->base_vaddr + phdr[i].p_vaddr;
			const auto end   = start + phdr[i].p_memsz;
			if (info->eh_frame_addr >= start && info->eh_frame_addr < end) {
				info->eh_frame_size = end - info->eh_frame_addr;
				break;
			}
		}
	}

	return OK;
}

static int KYTY_SYSV_ABI KernelGetModuleInfoFromAddr(uint64_t addr, int n, ModuleInfo* r) {
	PRINT_NAME();

	LOGF("\taddr = %016" PRIx64 "\n"
	     "\tn = %d\n",
	     addr, n);

	EXIT_NOT_IMPLEMENTED(n != 2);
	EXIT_NOT_IMPLEMENTED(r == nullptr);

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* p = rt->FindProgramByAddr(addr);

	if (p == nullptr) {
		LOGF("\thandle: not found\n");
		r->handle = 0;
		return -1;
	}

	r->handle = p->unique_id;

	LOGF("\thandle: %d\n", r->handle);

	return 0;
}

static void KYTY_SYSV_ABI KernelDebugRaiseExceptionOnReleaseMode(int /*c1*/, int /*c2*/) {
	PRINT_NAME();
}

static void KYTY_SYSV_ABI KernelDebugRaiseException(int /*c1*/, int /*c2*/) {
	PRINT_NAME();
}

static void KYTY_SYSV_ABI exit(int code) {
	PRINT_NAME();

	::exit(code);
}

static KYTY_SYSV_ABI MallocReplace* KernelGetSanitizerMallocReplaceExternal() {
	PRINT_NAME();

	static MallocReplace ret;

	return &ret;
}

static KYTY_SYSV_ABI NewReplace* KernelGetSanitizerNewReplaceExternal() {
	PRINT_NAME();

	static NewReplace ret;

	return &ret;
}

static KYTY_SYSV_ABI int elf_phdr_match_addr(ModuleInfo* m, uint64_t dtor_vaddr) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(m == nullptr);

	auto* rt     = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* p      = rt->FindProgramByAddr(dtor_vaddr);
	int   result = (p != nullptr && p->unique_id == m->handle) ? 1 : 0;

	LOGF("\thandle     = %" PRId32 "\n"
	     "\tdtor_vaddr = %016" PRIx64 "\n"
	     "\tmatch      = %s\n",
	     m->handle, dtor_vaddr, result == 1 ? "true" : "false");

	return result;
}

int KYTY_SYSV_ABI KernelUuidCreate(uint32_t* uuid) {
	PRINT_NAME();

	if (uuid == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	static thread_local std::mt19937 rng(std::random_device {}());
	uuid[0] = rng();
	uuid[1] = rng();
	uuid[2] = rng();
	uuid[3] = rng();

	return OK;
}

static KYTY_SYSV_ABI int KernelGetOpenPsId(void* open_ps_id) {
	PRINT_NAME();

	if (open_ps_id == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	static constexpr uint8_t id[16] = {
	    0x4b, 0x79, 0x74, 0x79, 0x4f, 0x70, 0x65, 0x6e,
	    0x50, 0x73, 0x49, 0x64, 0x00, 0x00, 0x00, 0x01,
	};

	std::memcpy(open_ps_id, id, sizeof(id));

	return OK;
}

static KYTY_SYSV_ABI void pthread_cxa_finalize(void* /*p*/) {
	PRINT_NAME();
}

void KYTY_SYSV_ABI KernelSetThreadAtexitCount(get_thread_atexit_count_func_t func) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_get_thread_atexit_count_func != nullptr);

	g_get_thread_atexit_count_func = func;
}

void KYTY_SYSV_ABI KernelSetThreadAtexitReport(thread_atexit_report_func_t func) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_thread_atexit_report_func != nullptr);

	g_thread_atexit_report_func = func;
}

int KYTY_SYSV_ABI KernelRtldThreadAtexitIncrement(uint64_t* /*c*/) {
	PRINT_NAME();

	//__sync_fetch_and_add(c, 1);

	return 0;
}

int KYTY_SYSV_ABI KernelRtldThreadAtexitDecrement(uint64_t* /*c*/) {
	PRINT_NAME();

	//__sync_fetch_and_sub(c, 1);

	return 0;
}

static KYTY_SYSV_ABI int KernelGetCurrentCpu() {
	PRINT_NAME();

	return 0;
}

int KYTY_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

int KYTY_SYSV_ABI clock_getres(int clock_id, LibKernel::KernelTimespec* res) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGetres(clock_id, res));
}

void KYTY_SYSV_ABI KernelSetGPO(uint32_t bits) {
	g_gpo_state_bits.store(bits, std::memory_order_relaxed);

	static std::atomic_uint32_t call_count {0};
	auto                        count = call_count.fetch_add(1, std::memory_order_relaxed);
	if (count < 16 || (count % 1024) == 0) {
		PRINT_NAME();
		LOGF("\tcall = %u, bits = 0x%08" PRIx32 "\n", count + 1, bits);
	}
}

uint64_t KYTY_SYSV_ABI KernelGetGPI() {
	auto bits = g_gpo_state_bits.load(std::memory_order_relaxed);

	static std::atomic_uint32_t call_count {0};
	auto                        count = call_count.fetch_add(1, std::memory_order_relaxed);
	if (count < 16 || (count % 1024) == 0) {
		PRINT_NAME();
		LOGF("\tcall = %u, return = 0x%016" PRIx64 "\n", count + 1, bits);
	}

	return bits;
}

} // namespace LibKernel

namespace Posix {

LIB_VERSION("Posix", 1, "libkernel", 1, 1);

int KYTY_SYSV_ABI getpagesize() {
	PRINT_NAME();

	return 0x4000;
}

int KYTY_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

int KYTY_SYSV_ABI clock_getres(int clock_id, LibKernel::KernelTimespec* res) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGetres(clock_id, res));
}

struct KernelTimezone {
	int32_t tz_minuteswest;
	int32_t tz_dsttime;
};

int KYTY_SYSV_ABI gettimeofday(LibKernel::KernelTimeval* time, KernelTimezone* timezone) {
	PRINT_NAME();

	if (time != nullptr) {
		int result = LibKernel::KernelGettimeofday(time);
		if (result != OK) {
			*GetErrorAddr() = LibKernel::KernelToPosix(result);
			return -1;
		}
	}

	if (timezone != nullptr) {
		timezone->tz_minuteswest = 0;
		timezone->tz_dsttime     = 0;
	}

	return 0;
}

int KYTY_SYSV_ABI nanosleep(const LibKernel::KernelTimespec* rqtp,
                            LibKernel::KernelTimespec*       rmtp) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelNanosleep(rqtp, rmtp));
}

int KYTY_SYSV_ABI stat(const char* path, LibKernel::FileSystem::FileStat* sb) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelStat(path, sb));
}

int KYTY_SYSV_ABI mkdir(const char* path, uint16_t mode) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelMkdir(path, mode));
}

int64_t KYTY_SYSV_ABI lseek(int d, int64_t offset, int whence) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelLseek(d, offset, whence));
}

int64_t KYTY_SYSV_ABI pread(int d, void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelPread(d, buf, nbytes, offset));
}

int64_t KYTY_SYSV_ABI pwrite(int d, const void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelPwrite(d, buf, nbytes, offset));
}

int KYTY_SYSV_ABI flock(int d, int operation) {
	PRINT_NAME();

	constexpr int lock_sh = 0x01;
	constexpr int lock_ex = 0x02;
	constexpr int lock_nb = 0x04;
	constexpr int lock_un = 0x08;

	auto lock_mode = operation & ~(lock_nb);
	if (lock_mode != lock_sh && lock_mode != lock_ex && lock_mode != lock_un) {
		*GetErrorAddr() = POSIX_EINVAL;
		return -1;
	}

	LibKernel::FileSystem::FileStat sb {};
	auto                            result = LibKernel::FileSystem::KernelFstat(d, &sb);
	if (result != OK) {
		*GetErrorAddr() = LibKernel::KernelToPosix(result);
		return -1;
	}

	return 0;
}

int64_t KYTY_SYSV_ABI fstat(int d, LibKernel::FileSystem::FileStat* sb) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelFstat(d, sb));
}

int KYTY_SYSV_ABI socket(int family, int type, int protocol) {
	PRINT_NAME();
	return Network::Net::Socket(family, type, protocol);
}

int KYTY_SYSV_ABI bind(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();
	return Network::Net::Bind(s, addr, addrlen);
}

int KYTY_SYSV_ABI connect(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();
	return Network::Net::Connect(s, addr, addrlen);
}

int KYTY_SYSV_ABI listen(int s, int backlog) {
	PRINT_NAME();
	return Network::Net::Listen(s, backlog);
}

int KYTY_SYSV_ABI accept(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();
	return Network::Net::Accept(s, addr, addrlen);
}

int KYTY_SYSV_ABI getsockname(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();
	return Network::Net::Getsockname(s, addr, addrlen);
}

int KYTY_SYSV_ABI getsockopt(int s, int level, int optname, void* optval, uint32_t* optlen) {
	PRINT_NAME();
	return Network::Net::Getsockopt(s, level, optname, optval, optlen);
}

int KYTY_SYSV_ABI setsockopt(int s, int level, int optname, const void* optval, uint32_t optlen) {
	PRINT_NAME();
	return Network::Net::Setsockopt(s, level, optname, optval, optlen);
}

int KYTY_SYSV_ABI select(int nfds, void* readfds, void* writefds, void* exceptfds,
                         const void* timeout) {
	PRINT_NAME();
	return Network::Net::Select(nfds, readfds, writefds, exceptfds, timeout);
}

int64_t KYTY_SYSV_ABI send(int s, const void* buf, uint64_t len, int flags) {
	PRINT_NAME();
	return Network::Net::Send(s, buf, len, flags);
}

int64_t KYTY_SYSV_ABI recv(int s, void* buf, uint64_t len, int flags) {
	PRINT_NAME();
	return Network::Net::Recv(s, buf, len, flags);
}

const char* KYTY_SYSV_ABI inet_ntop(int af, const void* src, char* dst, uint32_t size) {
	PRINT_NAME();

	const char* result = Network::Net::NetInetNtop(af, src, dst, size);
	if (result == nullptr) {
		*GetErrorAddr() = (af == 2 ? POSIX_ENOSPC : POSIX_EAFNOSUPPORT);
	}

	return result;
}

uint64_t KYTY_SYSV_ABI cfwBSQyr5Ys(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                                   uint64_t a6) {
	PRINT_NAME();

	LOGF("\t return = 0x%016" PRIx64 "\n"
	     "\t a1     = 0x%016" PRIx64 "\n"
	     "\t a2     = 0x%016" PRIx64 "\n"
	     "\t a3     = 0x%016" PRIx64 "\n"
	     "\t a4     = 0x%016" PRIx64 "\n"
	     "\t a5     = 0x%016" PRIx64 "\n"
	     "\t a6     = 0x%016" PRIx64 "\n"
	     "\t result = 0\n",
	     reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2, a3, a4, a5, a6);

	return 0;
}

uint64_t KYTY_SYSV_ABI KernelSyncOnAddressV1(uint64_t op, uint64_t address, uint64_t value,
                                             uint64_t size, uint64_t timeout, uint64_t flags) {
	static std::atomic_uint32_t log_count = 0;
	const auto                  index     = log_count.fetch_add(1, std::memory_order_relaxed);

	if (index < 16) {
		LOGF("\t libkernel_sync_on_address_v1: op=0x%016" PRIx64 ", address=0x%016" PRIx64
		     ", value=0x%016" PRIx64 ", size=0x%016" PRIx64 ", timeout=0x%016" PRIx64
		     ", flags=0x%016" PRIx64 "\n",
		     op, address, value, size, timeout, flags);
	}

	if (op != 0 && address == 0 && value == 0 && size == 0) {
		// This unsupported form is used as a yield/wait by some Unity jobs.
		// SleepMicro() uses a sub-millisecond busy wait on Windows, which can
		// pin every worker thread when the guest polls this path.
		Common::Thread::Sleep(timeout == 0 ? 1 : 2);
	}

	return 0;
}

LIB_DEFINE(InitLibKernel_1_Posix) {
	LIB_FUNC("k+AXqu2-eBc", getpagesize);
	LIB_FUNC("lLMT9vJAck0", clock_gettime);
	LIB_FUNC("smIj7eqzZE8", clock_getres);
	LIB_FUNC("n88vx3C5nW8", gettimeofday);
	LIB_FUNC("NhpspxdjEKU", nanosleep);
	LIB_FUNC("yS8U2TGCe1A", nanosleep);
	LIB_FUNC("E6ao34wPw+U", stat);
	LIB_FUNC("JGMio+21L4c", mkdir);
	LIB_FUNC("pDuPEf3m4fI", Posix::sem_init);
	LIB_FUNC("cDW233RAwWo", Posix::sem_destroy);
	LIB_FUNC("YCV5dGGBcCo", Posix::sem_wait);
	LIB_FUNC("WBWzsRifCEA", Posix::sem_trywait);
	LIB_FUNC("4SbrhCozqQU", Posix::sem_reltimedwait_np);
	LIB_FUNC("w5IHyvahg-o", Posix::sem_timedwait);
	LIB_FUNC("IKP8typ0QUk", Posix::sem_post);
	LIB_FUNC("Bq+LRV-N6Hk", Posix::sem_getvalue);

	LIB_FUNC("OxhIB8LB-PQ", Posix::pthread_create);
	LIB_FUNC("Jmi+9w9u0E4", Posix::pthread_create_name_np);
	LIB_FUNC("+U1R4WtXvoc", Posix::pthread_detach);
	LIB_FUNC("FJrT5LuUBAU", Posix::pthread_exit);
	LIB_FUNC("EotR8a3ASf4", Posix::pthread_self);
	LIB_FUNC("9vyP6Z7bqzc", Posix::pthread_rename_np);
	LIB_FUNC("lZzFeSxPl08", Posix::pthread_setcancelstate);
	LIB_FUNC("a2P9wYGeZvc", Posix::pthread_setprio);
	LIB_FUNC("FIs3-UQT9sg", Posix::pthread_getschedparam);
	LIB_FUNC("P41kTWUS3EI", Posix::pthread_getschedparam);
	LIB_FUNC("Xs9hdiD7sAA", Posix::pthread_setschedparam);
	LIB_FUNC("oIRFTjoILbg", Posix::pthread_setschedparam);
	LIB_FUNC("B5GmVDKwpn0", Posix::pthread_yield);
	LIB_FUNC("h9CcP3J0oVM", Posix::pthread_join);
	LIB_FUNC("wtkt-teR1so", Posix::pthread_attr_init);
	LIB_FUNC("zHchY8ft5pk", Posix::pthread_attr_destroy);
	LIB_FUNC("vQm4fDEsWi8", Posix::pthread_attr_getstack);
	LIB_FUNC("2Q0z6rnBrTE", Posix::pthread_attr_setstacksize);
	LIB_FUNC("Ucsu-OK+els", Posix::pthread_attr_get_np);
	LIB_FUNC("RtLRV-pBTTY", Posix::pthread_attr_getschedpolicy);
	LIB_FUNC("JarMIy8kKEY", Posix::pthread_attr_setschedpolicy);
	LIB_FUNC("E+tyo3lp5Lw", Posix::pthread_attr_setdetachstate);
	LIB_FUNC("euKRgm0Vn2M", Posix::pthread_attr_setschedparam);
	LIB_FUNC("7ZlAakEf0Qg", Posix::pthread_attr_setinheritsched);
	LIB_FUNC("0qOtCR-ZHck", Posix::pthread_attr_getstacksize);
	LIB_FUNC("VUT1ZSrHT0I", Posix::pthread_attr_getdetachstate);
	LIB_FUNC("JKyG3SWyA10", Posix::pthread_attr_setguardsize);
	LIB_FUNC("JNkVVsVDmOk", Posix::pthread_attr_getguardsize);
	LIB_FUNC("qlk9pSLsUmM", Posix::pthread_attr_getschedparam);
	LIB_FUNC("FXPWHNk8Of0", Posix::pthread_attr_getschedparam);
	LIB_FUNC("7Xl257M4VNI", LibKernel::PthreadEqual);
	LIB_FUNC("7H0iTOciTLo", Posix::pthread_mutex_lock);
	LIB_FUNC("K-jXhbt2gn4", Posix::pthread_mutex_trylock);
	LIB_FUNC("2Z+PpY6CaJg", Posix::pthread_mutex_unlock);
	LIB_FUNC("ttHNfU+qDBU", Posix::pthread_mutex_init);
	LIB_FUNC("dQHWEsJtoE4", Posix::pthread_mutexattr_init);
	LIB_FUNC("mDmgMOGVUqg", Posix::pthread_mutexattr_settype);
	LIB_FUNC("5txKfcMUAok", Posix::pthread_mutexattr_setprotocol);
	LIB_FUNC("HF7lK46xzjY", Posix::pthread_mutexattr_destroy);
	LIB_FUNC("ltCfaGr2JGE", Posix::pthread_mutex_destroy);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC("2MOy+rUfuhQ", Posix::pthread_cond_signal);
	LIB_FUNC("Op8TBGY5KHg", Posix::pthread_cond_wait);
	LIB_FUNC("Z4QosVuAsA0", Posix::pthread_once);
	LIB_FUNC("1471ajPzxh0", Posix::pthread_rwlock_destroy);
	LIB_FUNC("sIlRvQqsN2Y", Posix::pthread_rwlock_wrlock);
	LIB_FUNC("SFxTMOfuCkE", LibKernel::PthreadRwlockTryrdlock);
	LIB_FUNC("XhWHn6P5R7U", LibKernel::PthreadRwlockTrywrlock);
	LIB_FUNC("mqULNdimTn0", Posix::pthread_key_create);
	LIB_FUNC("6BpEZuDT7YI", Posix::pthread_key_delete);
	LIB_FUNC("WrOLvHU0yQM", Posix::pthread_setspecific);
	LIB_FUNC("0-KXaS70xy4", Posix::pthread_getspecific);
	LIB_FUNC("-quPa4SEJUw", Posix::pthread_getstack);
	LIB_FUNC("YQOfxL4QfeU", LibKernel::Memory::KernelMprotect);
	LIB_FUNC("UqDGjXA5yUM", LibKernel::Memory::KernelMunmap);
	LIB_FUNC("AqBioC2vF3I", LibKernel::read);
	LIB_FUNC("TU-d9PfIHPM", Posix::socket);
	LIB_FUNC("KuOmgKoqCdY", Posix::bind);
	LIB_FUNC("XVL8So3QJUk", Posix::connect);
	LIB_FUNC("pxnCmagrtao", Posix::listen);
	LIB_FUNC("3e+4Iv7IJ8U", Posix::accept);
	LIB_FUNC("RenI1lL1WFk", Posix::getsockname);
	LIB_FUNC("6O8EwYOgH9Y", Posix::getsockopt);
	LIB_FUNC("fFxGkxF2bVo", Posix::setsockopt);
	LIB_FUNC("T8fER+tIGgk", Posix::select);
	LIB_FUNC("fZOeZIOEmLw", Posix::send);
	LIB_FUNC("Ez8xjo9UF4E", Posix::recv);
	LIB_FUNC("5jRCs2axtr4", Posix::inet_ntop);
	LIB_FUNC("cfwBSQyr5Ys", cfwBSQyr5Ys);
}

} // namespace Posix

namespace Coredump {

LIB_VERSION("Coredump", 1, "Coredump", 1, 1);

static uint64_t g_coredump_handler         = 0;
static uint64_t g_coredump_handler_context = 0;

int KYTY_SYSV_ABI sceCoredumpRegisterCoredumpHandler(uint64_t handler, size_t stack_size,
                                                     uint64_t context) {
	PRINT_NAME();

	g_coredump_handler         = handler;
	g_coredump_handler_context = context;

	LOGF("\t handler = 0x%016" PRIx64 "\n"
	     "\t stack   = 0x%016" PRIx64 "\n"
	     "\t context = 0x%016" PRIx64 "\n",
	     handler, static_cast<uint64_t>(stack_size), context);

	return OK;
}

int KYTY_SYSV_ABI sceCoredumpUnregisterCoredumpHandler() {
	PRINT_NAME();

	g_coredump_handler         = 0;
	g_coredump_handler_context = 0;

	return OK;
}

} // namespace Coredump

namespace Fiber {

LIB_VERSION("Fiber", 1, "Fiber", 1, 1);

constexpr int32_t FIBER_ERROR_NULL       = -2141650943; /* 0x80590001 */
constexpr int32_t FIBER_ERROR_ALIGNMENT  = -2141650942; /* 0x80590002 */
constexpr int32_t FIBER_ERROR_RANGE      = -2141650941; /* 0x80590003 */
constexpr int32_t FIBER_ERROR_INVALID    = -2141650940; /* 0x80590004 */
constexpr int32_t FIBER_ERROR_PERMISSION = -2141650939; /* 0x80590005 */
constexpr int32_t FIBER_ERROR_STATE      = -2141650938; /* 0x80590006 */

constexpr uint32_t FIBER_MAGIC_START       = 0xdef1649c;
constexpr uint32_t FIBER_MAGIC_END         = 0xb37592a0;
constexpr uint32_t FIBER_OPT_MAGIC         = 0xbb40e64d;
constexpr uint64_t FIBER_STACK_MAGIC       = 0x7149f2ca7149f2ca;
constexpr uint64_t FIBER_CONTEXT_MIN_SIZE  = 512;
constexpr size_t   FIBER_MAX_NAME_LENGTH   = 31;
constexpr uint32_t FIBER_STATE_RUNNING     = 1;
constexpr uint32_t FIBER_STATE_IDLE        = 2;
constexpr uint32_t FIBER_STATE_TERMINATED  = 3;
constexpr uint32_t FIBER_FLAG_SET_FPU_REGS = 0x100;

using FiberEntry = KYTY_SYSV_ABI void (*)(uint64_t arg_on_initialize, uint64_t arg_on_run);

struct FiberOptParam {
	uint32_t magic;
};

struct FiberCpuContext {
	uint64_t rbx;
	uint64_t rbp;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rsp;
	uint64_t rip;
};

static_assert(sizeof(FiberCpuContext) == 80);

struct FiberObject {
	uint32_t        magic_start;
	uint32_t        state;
	FiberEntry      entry;
	uint64_t        arg_on_initialize;
	void*           addr_context;
	uint64_t        size_context;
	char            name[FIBER_MAX_NAME_LENGTH + 1];
	void*           context;
	uint32_t        flags;
	uint32_t        padding;
	void*           context_start;
	void*           context_end;
	FiberCpuContext saved_context;
	uint64_t        arg_on_run;
	uint64_t        arg_on_return;
	bool            context_valid;
	uint32_t        magic_end;
};

static_assert(sizeof(FiberObject) <= 256);

struct FiberInfo {
	uint64_t   size;
	FiberEntry entry;
	uint64_t   arg_on_initialize;
	void*      addr_context;
	uint64_t   size_context;
	char       name[FIBER_MAX_NAME_LENGTH + 1];
	uint64_t   size_context_margin;
	uint8_t    padding[48];
};

static_assert(sizeof(FiberInfo) == 128);

static bool FiberIsValid(const FiberObject* fiber) {
	return fiber != nullptr && fiber->magic_start == FIBER_MAGIC_START &&
	       fiber->magic_end == FIBER_MAGIC_END;
}

static thread_local FiberObject*                  g_current_fiber       = nullptr;
static thread_local FiberObject*                  g_thread_return_fiber = nullptr;
static thread_local FiberObject*                  g_starting_fiber      = nullptr;
static thread_local FiberObject*                  g_pending_idle_fiber  = nullptr;
static thread_local FiberCpuContext               g_thread_fiber_context {};
static std::mutex                                 g_fiber_owner_mutex;
static std::unordered_map<FiberObject*, uint64_t> g_fiber_owner_thread;
static std::unordered_map<uint64_t, FiberObject*> g_fiber_current_by_thread;

static uint32_t FiberLoadState(const FiberObject* fiber) {
	auto& state = const_cast<uint32_t&>(fiber->state);
	return std::atomic_ref<uint32_t>(state).load(std::memory_order_acquire);
}

static uint64_t FiberCurrentHostThreadId() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint64_t>(::GetCurrentThreadId());
#else
	return std::hash<std::thread::id> {}(std::this_thread::get_id());
#endif
}

static void FiberSetOwner(FiberObject* fiber) {
	std::lock_guard lock(g_fiber_owner_mutex);
	g_fiber_owner_thread[fiber] = FiberCurrentHostThreadId();
}

static void FiberClearOwner(FiberObject* fiber) {
	std::lock_guard lock(g_fiber_owner_mutex);
	g_fiber_owner_thread.erase(fiber);
}

static uint64_t FiberGetOwner(FiberObject* fiber) {
	std::lock_guard lock(g_fiber_owner_mutex);
	auto            it = g_fiber_owner_thread.find(fiber);
	return it != g_fiber_owner_thread.end() ? it->second : 0;
}

static void FiberSetCurrentFiber(FiberObject* fiber) {
	g_current_fiber = fiber;

	std::lock_guard lock(g_fiber_owner_mutex);
	const auto      thread_id = FiberCurrentHostThreadId();
	if (fiber != nullptr) {
		g_fiber_current_by_thread[thread_id] = fiber;
	} else {
		g_fiber_current_by_thread.erase(thread_id);
	}
}

static void FiberStoreState(FiberObject* fiber, uint32_t state) {
	std::atomic_ref<uint32_t>(fiber->state).store(state, std::memory_order_release);
	if (state == FIBER_STATE_RUNNING) {
		FiberSetOwner(fiber);
	} else {
		FiberClearOwner(fiber);
	}
}

static void FiberDeferIdle(FiberObject* fiber) {
	g_pending_idle_fiber = fiber;
}

static void FiberCommitDeferredIdle() {
	auto* fiber          = g_pending_idle_fiber;
	g_pending_idle_fiber = nullptr;
	if (fiber != nullptr) {
		FiberStoreState(fiber, FIBER_STATE_IDLE);
	}
}

static bool FiberCompareExchangeState(FiberObject* fiber, uint32_t expected, uint32_t desired) {
	const bool ok = std::atomic_ref<uint32_t>(fiber->state)
	                    .compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
	                                             std::memory_order_acquire);
	if (ok) {
		if (desired == FIBER_STATE_RUNNING) {
			FiberSetOwner(fiber);
		} else {
			FiberClearOwner(fiber);
		}
	}
	return ok;
}

static bool FiberWaitAndEnterRunning(FiberObject* fiber, uint32_t* observed_state) {
	const auto start = std::chrono::steady_clock::now();
	uint32_t   spin  = 0;
	auto&      state = fiber->state;
	auto       ref   = std::atomic_ref<uint32_t>(state);

	for (;;) {
		uint32_t expected = FIBER_STATE_IDLE;
		if (ref.compare_exchange_strong(expected, FIBER_STATE_RUNNING, std::memory_order_acq_rel,
		                                std::memory_order_acquire)) {
			FiberSetOwner(fiber);
			return true;
		}
		if (observed_state != nullptr) {
			*observed_state = expected;
		}
		if (expected != FIBER_STATE_RUNNING) {
			return false;
		}
		if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(5)) {
			return false;
		}
		if (spin++ < 64) {
			std::this_thread::yield();
		} else {
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	}
}

static bool FiberRepairStaleRunningOnThisThread(FiberObject* fiber, uint32_t observed_state) {
	if (observed_state != FIBER_STATE_RUNNING || fiber == g_current_fiber) {
		return false;
	}

	const auto owner = FiberGetOwner(fiber);
	if (owner != 0) {
		return false;
	}

	return FiberCompareExchangeState(fiber, FIBER_STATE_RUNNING, FIBER_STATE_IDLE);
}

static void FiberSetContextValid(FiberObject* fiber, bool valid) {
	fiber->context_valid = valid;
	fiber->context       = valid ? &fiber->saved_context : nullptr;
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((noinline, returns_twice)) static int FiberSaveContext(FiberCpuContext* ctx) {
	int ret = 0;
	asm volatile("movq %[ctx], %%r10\n\t"
	             "movq %%rbx, 0(%%r10)\n\t"
	             "movq %%rbp, 8(%%r10)\n\t"
	             "movq %%rdi, 16(%%r10)\n\t"
	             "movq %%rsi, 24(%%r10)\n\t"
	             "movq %%r12, 32(%%r10)\n\t"
	             "movq %%r13, 40(%%r10)\n\t"
	             "movq %%r14, 48(%%r10)\n\t"
	             "movq %%r15, 56(%%r10)\n\t"
	             "leaq 8(%%rsp), %%r11\n\t"
	             "movq %%r11, 64(%%r10)\n\t"
	             "movq (%%rsp), %%r11\n\t"
	             "movq %%r11, 72(%%r10)\n\t"
	             "xorl %%eax, %%eax\n\t"
	             : "=a"(ret)
	             : [ctx] "r"(ctx)
	             : "memory", "r10", "r11");
	return ret;
}

__attribute__((noreturn, noinline)) static void FiberRestoreContext(FiberCpuContext* ctx,
                                                                    uint64_t         ret) {
	asm volatile("movq %[ctx], %%r10\n\t"
	             "movq 72(%%r10), %%r11\n\t"
	             "movq 0(%%r10), %%rbx\n\t"
	             "movq 8(%%r10), %%rbp\n\t"
	             "movq 16(%%r10), %%rdi\n\t"
	             "movq 24(%%r10), %%rsi\n\t"
	             "movq 32(%%r10), %%r12\n\t"
	             "movq 40(%%r10), %%r13\n\t"
	             "movq 48(%%r10), %%r14\n\t"
	             "movq 56(%%r10), %%r15\n\t"
	             "movq 64(%%r10), %%rsp\n\t"
	             "movq %[ret], %%rax\n\t"
	             "jmp *%%r11\n\t"
	             :
	             : [ctx] "r"(ctx), [ret] "r"(ret)
	             : "memory", "rax", "r10", "r11");
	__builtin_unreachable();
}
#else
static int FiberSaveContext(FiberCpuContext* ctx) {
	(void)ctx;
	return 0;
}

static void FiberRestoreContext(FiberCpuContext* ctx, uint64_t ret) {
	(void)ctx;
	(void)ret;
	EXIT("Fiber context switching is only implemented on x86_64\n");
}
#endif

[[noreturn]] static void FiberStartTrampoline();

[[noreturn]] static void FiberStartOnGuestStack(FiberObject* fiber) {
	FiberCpuContext ctx {};
	const auto      stack_top = reinterpret_cast<uintptr_t>(fiber->addr_context) +
	                            static_cast<uintptr_t>(fiber->size_context);
	auto            rsp       = (stack_top & ~static_cast<uintptr_t>(0x0f));
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	rsp -= 4u * sizeof(uint64_t);
#endif
	rsp -= sizeof(uint64_t);
	*reinterpret_cast<uint64_t*>(rsp) = 0;

	ctx.rsp = rsp;
	ctx.rip = reinterpret_cast<uint64_t>(&FiberStartTrampoline);

	g_starting_fiber = fiber;
	FiberRestoreContext(&ctx, 1);
}

[[noreturn]] static void FiberStartTrampoline() {
	auto* fiber      = g_starting_fiber;
	g_starting_fiber = nullptr;
	if (fiber == nullptr) {
		EXIT("Fiber start without current fiber\n");
	}

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(fiber);

	fiber->entry(fiber->arg_on_initialize, fiber->arg_on_run);

	FiberStoreState(fiber, FIBER_STATE_TERMINATED);
	FiberSetContextValid(fiber, false);
	fiber->arg_on_return  = 0;
	g_thread_return_fiber = fiber;
	FiberSetCurrentFiber(nullptr);

	FiberRestoreContext(&g_thread_fiber_context, 1);
}

int32_t KYTY_SYSV_ABI FiberInitialize(FiberObject* fiber, const char* name, FiberEntry entry,
                                      uint64_t arg_on_initialize, void* addr_context,
                                      uint64_t size_context, const FiberOptParam* opt_param,
                                      uint32_t build_version) {
	PRINT_NAME();

	if (fiber == nullptr || name == nullptr || entry == nullptr) {
		return FIBER_ERROR_NULL;
	}

	if ((reinterpret_cast<uint64_t>(fiber) & 7u) != 0 ||
	    (reinterpret_cast<uint64_t>(addr_context) & 15u) != 0 ||
	    (opt_param != nullptr && (reinterpret_cast<uint64_t>(opt_param) & 7u) != 0)) {
		return FIBER_ERROR_ALIGNMENT;
	}

	if (size_context != 0 && size_context < FIBER_CONTEXT_MIN_SIZE) {
		return FIBER_ERROR_RANGE;
	}

	if ((size_context & 15u) != 0 || (addr_context == nullptr && size_context != 0) ||
	    (addr_context != nullptr && size_context == 0) ||
	    (opt_param != nullptr && opt_param->magic != FIBER_OPT_MAGIC)) {
		return FIBER_ERROR_INVALID;
	}

	std::memset(fiber, 0, sizeof(*fiber));
	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';

	fiber->magic_start       = FIBER_MAGIC_START;
	fiber->state             = FIBER_STATE_IDLE;
	fiber->entry             = entry;
	fiber->arg_on_initialize = arg_on_initialize;
	fiber->addr_context      = addr_context;
	fiber->size_context      = size_context;
	fiber->flags             = (build_version >= 0x03500000 ? FIBER_FLAG_SET_FPU_REGS : 0);
	fiber->context_start     = addr_context;
	fiber->context_end =
	    (addr_context != nullptr ? static_cast<uint8_t*>(addr_context) + size_context : nullptr);
	std::memset(&fiber->saved_context, 0, sizeof(fiber->saved_context));
	FiberSetContextValid(fiber, false);
	fiber->magic_end = FIBER_MAGIC_END;

	if (addr_context != nullptr) {
		*static_cast<uint64_t*>(addr_context) = FIBER_STACK_MAGIC;
	}

	LOGF("\t fiber init: %s, entry = 0x%016" PRIx64 ", context = 0x%016" PRIx64 ", size = %" PRIu64
	     "\n",
	     fiber->name, reinterpret_cast<uint64_t>(entry), reinterpret_cast<uint64_t>(addr_context),
	     size_context);

	return OK;
}

int32_t KYTY_SYSV_ABI FiberInitializeInternal(FiberObject* fiber, const char* name,
                                              FiberEntry entry, uint64_t arg_on_initialize,
                                              void* addr_context, uint64_t size_context,
                                              const FiberOptParam* opt_param, uint32_t flags,
                                              uint32_t build_version) {
	auto ret = FiberInitialize(fiber, name, entry, arg_on_initialize, addr_context, size_context,
	                           opt_param, build_version);
	if (ret == OK && fiber != nullptr) {
		fiber->flags |= flags;
	}
	return ret;
}

int32_t KYTY_SYSV_ABI FiberOptParamInitialize(FiberOptParam* opt_param) {
	PRINT_NAME();

	if (opt_param == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if ((reinterpret_cast<uint64_t>(opt_param) & 7u) != 0) {
		return FIBER_ERROR_ALIGNMENT;
	}

	std::memset(opt_param, 0, 128);
	opt_param->magic = FIBER_OPT_MAGIC;

	return OK;
}

int32_t KYTY_SYSV_ABI FiberFinalize(FiberObject* fiber) {
	PRINT_NAME();

	if (fiber == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_TERMINATED)) {
		return FIBER_ERROR_STATE;
	}

	return OK;
}

int32_t KYTY_SYSV_ABI FiberRun(FiberObject* fiber, uint64_t arg_on_run, uint64_t* arg_on_return) {
	PRINT_NAME();

	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	if (g_current_fiber != nullptr) {
		return FIBER_ERROR_PERMISSION;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING)) {
		return FIBER_ERROR_STATE;
	}

	fiber->arg_on_run     = arg_on_run;
	fiber->arg_on_return  = 0;
	g_thread_return_fiber = nullptr;

	if (FiberSaveContext(&g_thread_fiber_context) == 0) {
		if (fiber->context_valid) {
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(nullptr);
	auto* returned_fiber  = (g_thread_return_fiber != nullptr ? g_thread_return_fiber : fiber);
	g_thread_return_fiber = nullptr;

	if (arg_on_return != nullptr) {
		*arg_on_return = returned_fiber->arg_on_return;
	}

	return (FiberLoadState(returned_fiber) == FIBER_STATE_TERMINATED ? FIBER_ERROR_STATE : OK);
}

int32_t KYTY_SYSV_ABI FiberSwitch(FiberObject* fiber, uint64_t arg_on_run,
                                  uint64_t* arg_on_return) {
	PRINT_NAME();

	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	if (g_current_fiber == nullptr) {
		return FIBER_ERROR_PERMISSION;
	}

	for (;;) {
		uint32_t observed_state = 0;
		if (FiberWaitAndEnterRunning(fiber, &observed_state)) {
			break;
		}
		if (FiberRepairStaleRunningOnThisThread(fiber, observed_state)) {
			continue;
		}
		return FIBER_ERROR_STATE;
	}

	auto* caller = g_current_fiber;

	fiber->arg_on_run    = arg_on_run;
	fiber->arg_on_return = 0;

	if (FiberSaveContext(&caller->saved_context) == 0) {
		FiberSetContextValid(caller, true);
		FiberDeferIdle(caller);
		if (fiber->context_valid) {
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(caller);
	FiberStoreState(caller, FIBER_STATE_RUNNING);

	if (arg_on_return != nullptr) {
		*arg_on_return = caller->arg_on_run;
	}

	return OK;
}

int32_t KYTY_SYSV_ABI FiberGetSelf(FiberObject** fiber) {
	PRINT_NAME();

	if (fiber == nullptr) {
		return FIBER_ERROR_NULL;
	}

	*fiber = g_current_fiber;

	return OK;
}

int32_t KYTY_SYSV_ABI FiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run) {
	PRINT_NAME();

	if (g_current_fiber == nullptr) {
		return FIBER_ERROR_PERMISSION;
	}

	auto* fiber          = g_current_fiber;
	fiber->arg_on_return = arg_on_return;

	if (FiberSaveContext(&fiber->saved_context) == 0) {
		FiberSetContextValid(fiber, true);
		g_thread_return_fiber = fiber;
		FiberDeferIdle(fiber);
		FiberRestoreContext(&g_thread_fiber_context, 1);
	}

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(fiber);
	FiberStoreState(fiber, FIBER_STATE_RUNNING);
	if (arg_on_run != nullptr) {
		*arg_on_run = fiber->arg_on_run;
	}

	return OK;
}

int32_t KYTY_SYSV_ABI FiberGetInfo(FiberObject* fiber, FiberInfo* fiber_info) {
	PRINT_NAME();

	if (fiber == nullptr || fiber_info == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}

	std::memset(fiber_info, 0, sizeof(*fiber_info));
	fiber_info->size              = sizeof(*fiber_info);
	fiber_info->entry             = fiber->entry;
	fiber_info->arg_on_initialize = fiber->arg_on_initialize;
	fiber_info->addr_context      = fiber->addr_context;
	fiber_info->size_context      = fiber->size_context;
	std::strncpy(fiber_info->name, fiber->name, FIBER_MAX_NAME_LENGTH);

	return OK;
}

int32_t KYTY_SYSV_ABI FiberStartContextSizeCheck(uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int32_t KYTY_SYSV_ABI FiberStopContextSizeCheck() {
	PRINT_NAME();
	return OK;
}

int32_t KYTY_SYSV_ABI FiberRename(FiberObject* fiber, const char* name) {
	PRINT_NAME();

	if (fiber == nullptr || name == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}

	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';

	return OK;
}

int32_t KYTY_SYSV_ABI FiberGetThreadFramePointerAddress(uint64_t* addr_frame_pointer) {
	PRINT_NAME();

	if (addr_frame_pointer == nullptr) {
		return FIBER_ERROR_NULL;
	}

	*addr_frame_pointer = 0;

	return OK;
}

} // namespace Fiber

int chmod(const char* path, int mode) {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI KernelAioInitializeImpl(void* param, int32_t size) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n"
	     "\t size  = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(param), static_cast<uint32_t>(size));

	return OK;
}

void KYTY_SYSV_ABI KernelAioInitializeParam(void* param) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
}

constexpr int32_t KERNEL_AIO_STATE_SUBMITTED  = 1;
constexpr int32_t KERNEL_AIO_STATE_PROCESSING = 2;
constexpr int32_t KERNEL_AIO_STATE_COMPLETED  = 3;
constexpr int32_t KERNEL_AIO_STATE_ABORTED    = 4;
constexpr int32_t KERNEL_AIO_MAX_QUEUE        = 512;
constexpr int32_t KERNEL_AIO_MAX_REQUESTS     = 128;

struct KernelAioResult {
	int64_t  return_value;
	uint32_t state;
};

struct KernelAioRwRequest {
	int64_t          offset;
	size_t           nbyte;
	void*            buf;
	KernelAioResult* result;
	int32_t          fd;
};

static std::array<std::atomic_int32_t, KERNEL_AIO_MAX_QUEUE> g_kernel_aio_state = {};
static std::atomic_int32_t                                   g_kernel_aio_next_id {1};

static int32_t kernel_aio_next_id() {
	const auto sequence = g_kernel_aio_next_id.fetch_add(1, std::memory_order_relaxed);
	return ((sequence - 1) % (KERNEL_AIO_MAX_QUEUE - 1)) + 1;
}

static bool kernel_aio_is_valid_id(int32_t id) {
	return id > 0 && id < KERNEL_AIO_MAX_QUEUE;
}

int KYTY_SYSV_ABI KernelAioSubmitReadCommands(KernelAioRwRequest* req, int32_t size, int32_t prio,
                                              int32_t* id) {
	PRINT_NAME();

	if (req == nullptr || id == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	if (size <= 0 || size > KERNEL_AIO_MAX_REQUESTS) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto submit_id = kernel_aio_next_id();
	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_PROCESSING, std::memory_order_release);

	for (int32_t i = 0; i < size; i++) {
		if (req[i].result == nullptr) {
			g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_ABORTED,
			                                    std::memory_order_release);
			return LibKernel::KERNEL_ERROR_EFAULT;
		}

		req[i].result->state = KERNEL_AIO_STATE_SUBMITTED;

		const auto result =
		    LibKernel::FileSystem::KernelPread(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);

		req[i].result->return_value = result;
		req[i].result->state = (result < 0 ? KERNEL_AIO_STATE_ABORTED : KERNEL_AIO_STATE_COMPLETED);
	}

	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_COMPLETED, std::memory_order_release);
	*id = submit_id;

	(void)prio;
	return OK;
}

int KYTY_SYSV_ABI KernelAioSubmitWriteCommands(KernelAioRwRequest* req, int32_t size, int32_t prio,
                                               int32_t* id) {
	PRINT_NAME();

	if (req == nullptr || id == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	if (size <= 0 || size > KERNEL_AIO_MAX_REQUESTS) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto submit_id = kernel_aio_next_id();
	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_PROCESSING, std::memory_order_release);

	for (int32_t i = 0; i < size; i++) {
		if (req[i].result == nullptr) {
			g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_ABORTED,
			                                    std::memory_order_release);
			return LibKernel::KERNEL_ERROR_EFAULT;
		}

		req[i].result->state = KERNEL_AIO_STATE_SUBMITTED;

		const auto result =
		    LibKernel::FileSystem::KernelPwrite(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);

		req[i].result->return_value = result;
		req[i].result->state = (result < 0 ? KERNEL_AIO_STATE_ABORTED : KERNEL_AIO_STATE_COMPLETED);
	}

	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_COMPLETED, std::memory_order_release);
	*id = submit_id;

	(void)prio;
	return OK;
}

int KYTY_SYSV_ABI KernelAioWaitRequest(int32_t id, int32_t* state, uint32_t* usec) {
	PRINT_NAME();

	if (state == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	if (!kernel_aio_is_valid_id(id)) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	uint32_t waited  = 0;
	auto     current = g_kernel_aio_state[id].load(std::memory_order_acquire);
	while (current == KERNEL_AIO_STATE_PROCESSING) {
		if (usec != nullptr && *usec != 0 && waited >= *usec) {
			*state = current;
			return LibKernel::KERNEL_ERROR_ETIMEDOUT;
		}

		Common::Thread::SleepMicro(10);
		waited += 10;
		current = g_kernel_aio_state[id].load(std::memory_order_acquire);
	}

	*state = current;
	return OK;
}

int KYTY_SYSV_ABI KernelAioDeleteRequest(int32_t id, int32_t* ret) {
	PRINT_NAME();

	if (ret == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	if (!kernel_aio_is_valid_id(id)) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	g_kernel_aio_state[id].store(KERNEL_AIO_STATE_ABORTED, std::memory_order_release);
	*ret = OK;

	return OK;
}

namespace FileSystem = LibKernel::FileSystem;
namespace Memory     = LibKernel::Memory;
namespace EventQueue = LibKernel::EventQueue;
namespace EventFlag  = LibKernel::EventFlag;
namespace Semaphore  = LibKernel::Semaphore;

LIB_DEFINE(InitFiber_1) {
	LIB_USING(Fiber);

	LIB_FUNC("hVYD7Ou2pCQ", Fiber::FiberInitialize);
	LIB_FUNC("7+OJIpko9RY", Fiber::FiberInitializeInternal);
	LIB_FUNC("asjUJJ+aa8s", Fiber::FiberOptParamInitialize);
	LIB_FUNC("JeNX5F-NzQU", Fiber::FiberFinalize);
	LIB_FUNC("a0LLrZWac0M", Fiber::FiberRun);
	LIB_FUNC("PFT2S-tJ7Uk", Fiber::FiberSwitch);
	LIB_FUNC("p+zLIOg27zU", Fiber::FiberGetSelf);
	LIB_FUNC("B0ZX2hx9DMw", Fiber::FiberReturnToThread);
	LIB_FUNC("avfGJ94g36Q", Fiber::FiberRun);
	LIB_FUNC("ZqhZFuzKT6U", Fiber::FiberSwitch);
	LIB_FUNC("uq2Y5BFz0PE", Fiber::FiberGetInfo);
	LIB_FUNC("Lcqty+QNWFc", Fiber::FiberStartContextSizeCheck);
	LIB_FUNC("Kj4nXMpnM8Y", Fiber::FiberStopContextSizeCheck);
	LIB_FUNC("JzyT91ucGDc", Fiber::FiberRename);
	LIB_FUNC("0dy4JtMUcMQ", Fiber::FiberGetThreadFramePointerAddress);
}

LIB_DEFINE(InitCoredump_1) {
	LIB_USING(Coredump);

	LIB_FUNC("8zLSfEfW5AU", Coredump::sceCoredumpRegisterCoredumpHandler);
	LIB_FUNC("fFkhOgztiCA", Coredump::sceCoredumpUnregisterCoredumpHandler);
}

LIB_DEFINE(InitLibKernel_1_FS) {
	LIB_FUNC("1G3lF1Gg1k8", FileSystem::KernelOpen);
	LIB_FUNC("UK2Tl2DWUns", FileSystem::KernelClose);
	LIB_FUNC("Cg4srZ6TKbU", FileSystem::KernelRead);
	LIB_FUNC("4wSze92BhLI", FileSystem::KernelWrite);
	LIB_FUNC("+r3rMFwItV4", FileSystem::KernelPread);
	LIB_FUNC("nKWi-N2HBV4", FileSystem::KernelPwrite);
	LIB_FUNC("eV9wAD2riIA", FileSystem::KernelStat);
	LIB_FUNC("kBwCPsYX-m4", FileSystem::KernelFstat);
	LIB_FUNC("AUXVxWeJU-A", FileSystem::KernelUnlink);
	LIB_FUNC("52NcYU9+lEo", FileSystem::KernelRename);
	LIB_FUNC("taRWhTJFTgE", FileSystem::KernelGetdirentries);
	LIB_FUNC("oib76F-12fk", FileSystem::KernelLseek);
	LIB_FUNC("j2AIqSqJP0w", FileSystem::KernelGetdents);
	LIB_FUNC("1-LFLmRFxxM", FileSystem::KernelMkdir);
	LIB_FUNC("naInUjYt3so", FileSystem::KernelRmdir);
	LIB_FUNC("uWyW3v98sU4", FileSystem::KernelCheckReachability);
}

LIB_DEFINE(InitLibKernel_1_Mem) {
	LIB_FUNC("mL8NDH86iQI", Memory::KernelMapNamedFlexibleMemory);
	LIB_FUNC("IWIBBdTHit4", Memory::KernelMapFlexibleMemory);
	LIB_FUNC("DGMG3JshrZU", Memory::KernelSetVirtualRangeName);
	// 6xx
	LIB_FUNC("4h6F1LLbTiw", Memory::KernelMapFlexibleMemory);
	LIB_FUNC("cQke9UuBQOk", Memory::KernelMunmap);
	LIB_FUNC("pO96TwzOm5E", Memory::KernelGetDirectMemorySize);
	LIB_FUNC("C0f7TJcbfac", Memory::KernelAvailableDirectMemorySize);
	LIB_FUNC("tZ2yplY8MBY", Memory::KernelGetPageTableStats);
	LIB_FUNC("rTXw65xmLIA", Memory::KernelAllocateDirectMemory);
	LIB_FUNC("B+vc2AO2Zrc", Memory::KernelAllocateMainDirectMemory);
	LIB_FUNC("rVjRvHJ0X6c", Memory::KernelVirtualQuery);
	LIB_FUNC("yDBwVAolDgg", Memory::KernelIsStack);
	LIB_FUNC("7oxv3PPCumo", Memory::KernelReserveVirtualRange);
	LIB_FUNC("L-Q3LEjIbgA", Memory::KernelMapDirectMemory);
	LIB_FUNC("BQQniolj9tQ", Memory::KernelMapDirectMemory2);
	LIB_FUNC("NcaWUxfMNIQ", Memory::KernelMapNamedDirectMemory);
	LIB_FUNC("BohYr-F7-is", Memory::KernelSetPrtAperture);
	LIB_FUNC("L0v2Go5jOuM", Memory::KernelGetPrtAperture);
	LIB_FUNC("hwVSPCmp5tM", Memory::KernelCheckedReleaseDirectMemory);
	LIB_FUNC("MBuItvba6z8", Memory::KernelReleaseDirectMemory);
	LIB_FUNC("jh+8XiK4LeE", Memory::KernelIsAddressSanitizerEnabled);
	LIB_FUNC("WFcfL2lzido", Memory::KernelQueryMemoryProtection);
	LIB_FUNC("BHouLQzh0X0", Memory::KernelDirectMemoryQuery);
	LIB_FUNC("aNz11fnnzi4", Memory::KernelAvailableFlexibleMemorySize);
	LIB_FUNC("n1-v6FgU7MQ", Memory::KernelConfiguredFlexibleMemorySize);
	LIB_FUNC("vSMAm3cxYTY", Memory::KernelMprotect);
	LIB_FUNC("9bfdLIyuwCY", Memory::KernelMtypeprotect);
	LIB_FUNC("2SKEx6bSq-4", Memory::KernelBatchMap);
	LIB_FUNC("kBJzF8x4SyE", Memory::KernelBatchMap2);
	LIB_FUNC("pU-QydtGcGY", Memory::KernelMemoryPoolReserve);
	LIB_FUNC("qCSfqDILlns", Memory::KernelMemoryPoolExpand);
	LIB_FUNC("Vzl66WmfLvk", Memory::KernelMemoryPoolCommit);
	LIB_FUNC("LXo1tpFqJGs", Memory::KernelMemoryPoolDecommit);
	LIB_FUNC("YN878uKRBbE", Memory::KernelMemoryPoolBatch);
	LIB_FUNC("bvD+95Q6asU", Memory::KernelMemoryPoolGetBlockStats);
}

LIB_DEFINE(InitLibKernel_1_Equeue) {
	LIB_FUNC("D0OdFMjp46I", EventQueue::KernelCreateEqueue);
	LIB_FUNC("jpFjmgAC5AE", EventQueue::KernelDeleteEqueue);
	LIB_FUNC("fzyMKs9kim0", EventQueue::KernelWaitEqueue);
	LIB_FUNC("vz+pg2zdopI", EventQueue::KernelGetEventUserData);
	LIB_FUNC("mJ7aghmgvfc", EventQueue::KernelGetEventId);
	LIB_FUNC("23CPPI1tyBY", EventQueue::KernelGetEventFilter);
	LIB_FUNC("kwGyyjohI50", EventQueue::KernelGetEventData);
	LIB_FUNC("Q0qr9AyqJSk", EventQueue::KernelGetEventFflags);
	LIB_FUNC("Uu-iDFC9aUc", EventQueue::KernelGetEventError);
	LIB_FUNC("4R6-OvI2cEA", EventQueue::KernelAddUserEvent);
	LIB_FUNC("WDszmSbWuDk", EventQueue::KernelAddUserEventEdge);
	LIB_FUNC("F6e0kwo4cnk", EventQueue::KernelTriggerUserEvent);
	LIB_FUNC("LJDwdSNTnDg", EventQueue::KernelDeleteUserEvent);
	LIB_FUNC("R74tt43xP6k", EventQueue::KernelAddHRTimerEvent);
	LIB_FUNC("J+LF6LwObXU", EventQueue::KernelDeleteHRTimerEvent);
	LIB_FUNC("bBfz7kMF2Ho", EventQueue::KernelAddAmprEvent);
	LIB_FUNC("vuae5JPNt9A", EventQueue::KernelAddAmprSystemEvent);
	LIB_FUNC("bMmid3pfyjo", EventQueue::KernelDeleteAmprEvent);
	LIB_FUNC("Ij+ryuEClXQ", EventQueue::KernelDeleteAmprSystemEvent);
}

LIB_DEFINE(InitLibKernel_1_EventFlag) {
	LIB_FUNC("PZku4ZrXJqg", EventFlag::KernelCancelEventFlag);
	LIB_FUNC("7uhBFWRAS60", EventFlag::KernelClearEventFlag);
	LIB_FUNC("BpFoboUJoZU", EventFlag::KernelCreateEventFlag);
	LIB_FUNC("8mql9OcQnd4", EventFlag::KernelDeleteEventFlag);
	LIB_FUNC("9lvj5DjHZiA", EventFlag::KernelPollEventFlag);
	LIB_FUNC("JTvBflhYazQ", EventFlag::KernelWaitEventFlag);
	LIB_FUNC("IOnSvHzqu6A", EventFlag::KernelSetEventFlag);
}

LIB_DEFINE(InitLibKernel_1_Semaphore) {
	LIB_FUNC("188x57JYp0g", Semaphore::KernelCreateSema);
	LIB_FUNC("R1Jvn8bSCW8", Semaphore::KernelDeleteSema);
	LIB_FUNC("Zxa0VhQVTsk", Semaphore::KernelWaitSema);
	LIB_FUNC("12wOHk8ywb0", Semaphore::KernelPollSema);
	LIB_FUNC("4czppHBiriw", Semaphore::KernelSignalSema);
	LIB_FUNC("4DM06U2BNEY", Semaphore::KernelCancelSema);
	LIB_FUNC("GEnUkDZoUwY", Semaphore::PthreadSemInit);
	LIB_FUNC("Vwc+L05e6oE", Semaphore::PthreadSemDestroy);
	LIB_FUNC("C36iRE0F5sE", Semaphore::PthreadSemWait);
	LIB_FUNC("H2a+IN9TP0E", Semaphore::PthreadSemTrywait);
	LIB_FUNC("fjN6NQHhK8k", Semaphore::PthreadSemTimedwait);
	LIB_FUNC("aishVAiFaYM", Semaphore::PthreadSemPost);
	LIB_FUNC("DjpBvGlaWbQ", Semaphore::PthreadSemGetvalue);
}

LIB_DEFINE(InitLibKernel_1_Pthread) {
	LIB_FUNC("9UK1vLZQft4", LibKernel::PthreadMutexLock);
	LIB_FUNC("tn3VlD0hG60", LibKernel::PthreadMutexUnlock);
	LIB_FUNC("2Of0f+3mhhE", LibKernel::PthreadMutexDestroy);
	LIB_FUNC("cmo1RIYva9o", LibKernel::PthreadMutexInit);
	LIB_FUNC("upoVrzMHFeE", LibKernel::PthreadMutexTrylock);
	LIB_FUNC("smWEktiyyG0", LibKernel::PthreadMutexattrDestroy);
	LIB_FUNC("F8bUHwAG284", LibKernel::PthreadMutexattrInit);
	LIB_FUNC("iMp8QpE+XO4", LibKernel::PthreadMutexattrSettype);
	LIB_FUNC("1FGvU0i9saQ", LibKernel::PthreadMutexattrSetprotocol);

	LIB_FUNC("aI+OeCz8xrQ", LibKernel::PthreadSelf);
	LIB_FUNC("EotR8a3ASf4", LibKernel::PthreadSelf);
	LIB_FUNC("6UgtwV+0zb4", LibKernel::PthreadCreate);
	LIB_FUNC("3PtV6p3QNX4", LibKernel::PthreadEqual);
	LIB_FUNC("onNY9Byn-W8", LibKernel::PthreadJoin);
	LIB_FUNC("4qGrR6eoP9Y", LibKernel::PthreadDetach);
	LIB_FUNC("3kg7rT0NQIs", LibKernel::PthreadExit);
	LIB_FUNC("OAmWq+OHSjw", LibKernel::PthreadSetcancelstate);
	LIB_FUNC("sCJd99Phct0", LibKernel::PthreadSetcanceltype);
	LIB_FUNC("How7B8Oet6k", LibKernel::PthreadGetname);
	LIB_FUNC("GBUY7ywdULE", LibKernel::PthreadRename);
	LIB_FUNC("bt3CTBKmGyI", LibKernel::PthreadSetaffinity);
	LIB_FUNC("rcrVFJsQWRY", LibKernel::PthreadGetaffinity);
	LIB_FUNC("1tKyG7RlMJo", LibKernel::PthreadGetprio);
	LIB_FUNC("W0Hpm2X0uPE", LibKernel::PthreadSetprio);
	LIB_FUNC("T72hz6ffq08", LibKernel::PthreadYield);
	LIB_FUNC("6XG4B33N09g", LibKernel::PthreadYield);
	LIB_FUNC("CBNtXOoef-E", Posix::sched_get_priority_max);
	LIB_FUNC("m0iS6jNsXds", Posix::sched_get_priority_min);
	LIB_FUNC("EI-5-jlq2dE", LibKernel::PthreadGetthreadid);
	LIB_FUNC("geDaqgH9lTg", LibKernel::PthreadKeyCreate);
	LIB_FUNC("PrdHuuDekhY", LibKernel::PthreadKeyDelete);
	LIB_FUNC("+BzXYkqYeLE", LibKernel::PthreadSetspecific);
	LIB_FUNC("eoht7mQOCmo", LibKernel::PthreadGetspecific);

	LIB_FUNC("62KCwEMmzcM", LibKernel::PthreadAttrDestroy);
	LIB_FUNC("x1X76arYMxU", LibKernel::PthreadAttrGet);
	LIB_FUNC("8+s5BzZjxSg", LibKernel::PthreadAttrGetaffinity);
	LIB_FUNC("nsYoNRywwNg", LibKernel::PthreadAttrInit);
	LIB_FUNC("JaRMy+QcpeU", LibKernel::PthreadAttrGetdetachstate);
	LIB_FUNC("-quPa4SEJUw", LibKernel::PthreadAttrGetstack);
	LIB_FUNC("Ru36fiTtJzA", LibKernel::PthreadAttrGetstackaddr);
	LIB_FUNC("-fA+7ZlGDQs", LibKernel::PthreadAttrGetstacksize);
	LIB_FUNC("txHtngJ+eyc", LibKernel::PthreadAttrGetguardsize);
	LIB_FUNC("9RnL-m0+diQ", LibKernel::PthreadAttrGetsolosched);
	LIB_FUNC("Bvn74vj6oLo", LibKernel::PthreadAttrSetstack);
	LIB_FUNC("F+yfmduIBB8", LibKernel::PthreadAttrSetstackaddr);
	LIB_FUNC("UTXzJbWhhTE", LibKernel::PthreadAttrSetstacksize);
	LIB_FUNC("2Q0z6rnBrTE", LibKernel::PthreadAttrSetstacksize);
	LIB_FUNC("-Wreprtu0Qs", LibKernel::PthreadAttrSetdetachstate);
	LIB_FUNC("El+cQ20DynU", LibKernel::PthreadAttrSetguardsize);
	LIB_FUNC("eXbUSpEaTsA", LibKernel::PthreadAttrSetinheritsched);
	LIB_FUNC("DzES9hQF4f4", LibKernel::PthreadAttrSetschedparam);
	LIB_FUNC("4+h9EzwKF4I", LibKernel::PthreadAttrSetschedpolicy);
	LIB_FUNC("Dk6FC-TI+7Q", LibKernel::PthreadAttrSetsolosched);
	LIB_FUNC("3qxgM4ezETA", LibKernel::PthreadAttrSetaffinity);
	LIB_FUNC("FXPWHNk8Of0", LibKernel::PthreadAttrGetschedparam);

	LIB_FUNC("6ULAa0fq4jA", LibKernel::PthreadRwlockInit);
	LIB_FUNC("ytQULN-nhL4", LibKernel::PthreadRwlockInit);
	LIB_FUNC("BB+kb08Tl9A", LibKernel::PthreadRwlockDestroy);
	LIB_FUNC("Ox9i0c7L5w0", LibKernel::PthreadRwlockRdlock);
	LIB_FUNC("iGjsr1WAtI0", LibKernel::PthreadRwlockRdlock);
	LIB_FUNC("+L98PIbGttk", LibKernel::PthreadRwlockUnlock);
	LIB_FUNC("EgmLo6EWgso", LibKernel::PthreadRwlockUnlock);
	LIB_FUNC("mqdNorrB+gI", LibKernel::PthreadRwlockWrlock);
	LIB_FUNC("sIlRvQqsN2Y", LibKernel::PthreadRwlockWrlock);
	LIB_FUNC("bIHoZCTomsI", LibKernel::PthreadRwlockTrywrlock);
	LIB_FUNC("i2ifZ3fS2fo", LibKernel::PthreadRwlockattrDestroy);
	LIB_FUNC("yOfGg-I1ZII", LibKernel::PthreadRwlockattrInit);
	LIB_FUNC("qsdmgXjqSgk", LibKernel::PthreadRwlockattrDestroy);
	LIB_FUNC("xFebsA4YsFI", LibKernel::PthreadRwlockattrInit);
	LIB_FUNC("h-OifiouBd8", LibKernel::PthreadRwlockattrSettype);

	LIB_FUNC("2Tb92quprl0", LibKernel::PthreadCondInit);
	LIB_FUNC("0TyVk4MSLt0", LibKernel::PthreadCondInit);
	LIB_FUNC("g+PZd2hiacg", LibKernel::PthreadCondDestroy);
	LIB_FUNC("RXXqi4CtF8w", LibKernel::PthreadCondDestroy);
	LIB_FUNC("WKAXJ4XBPQ4", LibKernel::PthreadCondWait);
	LIB_FUNC("JGgj7Uvrl+A", LibKernel::PthreadCondBroadcast);
	LIB_FUNC("kDh-NfxgMtE", LibKernel::PthreadCondSignal);
	LIB_FUNC("2MOy+rUfuhQ", LibKernel::PthreadCondSignal);
	LIB_FUNC("o69RpYO-Mu0", LibKernel::PthreadCondSignalto);
	LIB_FUNC("BmMjYxmew1w", LibKernel::PthreadCondTimedwait);
	LIB_FUNC("27bAgiJmOh0", Posix::pthread_cond_timedwait);
	LIB_FUNC("m5-2bsNfv7s", LibKernel::PthreadCondattrInit);
	LIB_FUNC("mKoTx03HRWA", Posix::pthread_condattr_init);
	LIB_FUNC("waPcxYiR3WA", LibKernel::PthreadCondattrDestroy);
	LIB_FUNC("dJcuQVn6-Iw", Posix::pthread_condattr_destroy);
	LIB_FUNC("EjllaAqAPZo", Posix::pthread_condattr_setclock);

	LIB_FUNC("wRYVA5Zolso", LibKernel::KernelClockGetres);
	LIB_FUNC("QBi7HCK03hw", LibKernel::KernelClockGettime);
	LIB_FUNC("ejekcaNQNq0", LibKernel::KernelGettimeofday);
	LIB_FUNC("kOcnerypnQA", LibKernel::KernelGettimezone);
	LIB_FUNC("0NTHN1NKONI", LibKernel::KernelConvertLocaltimeToUtc);
	LIB_FUNC("-o5uEDpN+oY", LibKernel::KernelConvertUtcToLocaltime);
	LIB_FUNC("n88vx3C5nW8", Posix::gettimeofday);
	LIB_FUNC("-2IRUCO--PM", LibKernel::KernelReadTsc);
	LIB_FUNC("1j3S3n-tTW4", LibKernel::KernelGetTscFrequency);
	LIB_FUNC("4J2sUJmuHZQ", LibKernel::KernelGetProcessTime);
	LIB_FUNC("BNowx2l588E", LibKernel::KernelGetProcessTimeCounterFrequency);
	LIB_FUNC("fgxnMeTNUtY", LibKernel::KernelGetProcessTimeCounter);

	LIB_FUNC("7H0iTOciTLo", Posix::pthread_mutex_lock);
	LIB_FUNC("2Z+PpY6CaJg", Posix::pthread_mutex_unlock);
	LIB_FUNC("IafI2PxcPnQ", LibKernel::PthreadMutexTimedlock);
	LIB_FUNC("Io9+nTKXZtA", Posix::pthread_mutex_timedlock);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC("Op8TBGY5KHg", Posix::pthread_cond_wait);
	LIB_FUNC("14bOACANTBo", Posix::pthread_once);
	LIB_FUNC("Z4QosVuAsA0", Posix::pthread_once);
	LIB_FUNC("Oy6IpwgtYOk", Posix::lseek);
	LIB_FUNC("ezv-RSBNKqI", Posix::pread);
	LIB_FUNC("C2kJ-byS5rM", Posix::pwrite);
	LIB_FUNC("9eMlfusH4sU", Posix::flock);
	LIB_FUNC("mqQMh1zPPT8", Posix::fstat);

	LIB_FUNC("z0dtnPxYgtg", chmod);
	LIB_FUNC("VAzswvTOCzI", FileSystem::KernelUnlink);
	LIB_FUNC("JGMio+21L4c", Posix::mkdir);
	LIB_FUNC("wuCroIGjt2g", FileSystem::KernelOpen);
	LIB_FUNC("bY-PO6JhzhQ", FileSystem::KernelClose);
	LIB_FUNC("FN4gaPmuFV8", FileSystem::KernelWrite);
}

static void AddLibkernelUnityFunc(Loader::SymbolDatabase* s, const char* nid, uint64_t func,
                                  const std::string& dbg_name) {
	Loader::SymbolResolve sr {};
	sr.name                 = nid;
	sr.library              = "libkernel_unity";
	sr.library_version      = 1;
	sr.module               = "libkernel";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = Loader::SymbolType::Func;
	s->Add(sr, func, dbg_name);
}

LIB_DEFINE(InitLibKernel_1) {
	InitLibKernel_1_FS(s);
	InitLibKernel_1_Mem(s);
	InitLibKernel_1_Equeue(s);
	InitLibKernel_1_EventFlag(s);
	InitLibKernel_1_Semaphore(s);
	InitLibKernel_1_Pthread(s);
	LibKernelApr::InitLibKernel_1_Apr(s);
	Posix::InitLibKernel_1_Posix(s);

	LIB_OBJECT("f7uOxY9mM1U", &LibKernel::g_stack_chk_guard);
	LIB_OBJECT("djxxOmW6-aw", &LibKernel::g_progname);

	LIB_FUNC("1jfXLRVzisc", LibKernel::KernelUsleep);
	LIB_FUNC("QcteRwbsnV0", LibKernel::KernelUsleep);
	LIB_FUNC("QvsZxomvUHs", LibKernel::KernelNanosleep);
	LIB_FUNC("LwG8g3niqwA", LibKernel::KernelDlsym);
	LIB_FUNC("-ZR+hG7aDHw", LibKernel::KernelSleep);
	LIB_FUNC("6c3rCVE-fTU", LibKernel::open);
	LIB_FUNC("6xVpy0Fdq+I", LibKernel::sigprocmask);
	LIB_FUNC("6Z83sYWFlA8", LibKernel::exit);
	LIB_FUNC("8OnWXlgQlvo", LibKernel::KernelRtldThreadAtexitDecrement);
	LIB_FUNC("959qrazPIrg", LibKernel::KernelGetProcParam);
	LIB_FUNC("tU5e3f9gSiU", LibKernel::KernelIsTrinityMode);
	LIB_FUNC("fTx66l5iWIA", LibKernel::KernelFsync);
	LIB_FUNC("uvT2iYBBnkY", LibKernel::KernelSync);
	LIB_FUNC("HoLVWNanBBc", LibKernel::getpid);
	LIB_FUNC("9BcDykPmo1I", LibKernel::get_error_addr);
	LIB_FUNC("k+AXqu2-eBc", Posix::getpagesize);
	LIB_FUNC("bnZxYgAFeA0", LibKernel::KernelGetSanitizerNewReplaceExternal);
	LIB_FUNC("ca7v6Cxulzs", LibKernel::KernelSetGPO);
	LIB_FUNC("4oXYe9Xmk0Q", LibKernel::KernelGetGPI);
	LIB_FUNC("DRuBt2pvICk", LibKernel::read);
	LIB_FUNC("AqBioC2vF3I", LibKernel::read);
	LIB_FUNC("f7KBOafysXo", LibKernel::KernelGetModuleInfoFromAddr);
	LIB_FUNC("RpQJJVKTiFM", LibKernel::KernelGetModuleInfoForUnwind);
	LIB_FUNC("Fjc4-n1+y2g", LibKernel::elf_phdr_match_addr);
	LIB_FUNC("FxVZqBAA7ks", LibKernel::write);
	LIB_FUNC("FJmglmTMdr4", LibKernel::getargv);
	LIB_FUNC("kbw4UHHSYy0", LibKernel::pthread_cxa_finalize);
	LIB_FUNC("lLMT9vJAck0", LibKernel::clock_gettime);
	LIB_FUNC("5TgME6AYty4", KernelAioDeleteRequest);
	LIB_FUNC("HgX7+AORI58", KernelAioSubmitReadCommands);
	LIB_FUNC("KOF-oJbQVvc", KernelAioWaitRequest);
	LIB_FUNC("XQ8C8y+de+E", KernelAioSubmitWriteCommands);
	LIB_FUNC("nu4a0-arQis", KernelAioInitializeParam);
	LIB_FUNC("il03nluKfMk", LibKernel::KernelRaiseException);
	LIB_FUNC("iKJMWrAumPE", LibKernel::getargc);
	LIB_FUNC("NNtFaKJbPt0", LibKernel::close);
	LIB_FUNC("OMDRKKAZ8I4", LibKernel::KernelDebugRaiseException);
	LIB_FUNC("Ou3iL1abvng", LibKernel::stack_chk_fail);
	LIB_FUNC("p5EcQeEeJAE", LibKernel::KernelRtldSetApplicationHeapAPI);
	LIB_FUNC("pB-yGZ2nQ9o", LibKernel::KernelSetThreadAtexitCount);
	LIB_FUNC("py6L8jiVAN8", LibKernel::KernelGetSanitizerMallocReplaceExternal);
	LIB_FUNC("Qhv5ARAoOEc", LibKernel::KernelRemoveExceptionHandler);
	LIB_FUNC("QKd0qM58Qes", LibKernel::KernelStopUnloadModule);
	LIB_FUNC("rNhWz+lvOMU", LibKernel::KernelSetThreadDtors);
	LIB_FUNC("Tz4RNUCBbGI", LibKernel::KernelRtldThreadAtexitIncrement);
	LIB_FUNC("vNe1w4diLCs", LibKernel::tls_get_addr);
	LIB_FUNC("vYU8P9Td2Zo", KernelAioInitializeImpl);
	LIB_FUNC("WhCc1w3EhSI", LibKernel::KernelSetThreadAtexitReport);
	LIB_FUNC("WkwEd3N7w0Y", LibKernel::KernelInstallExceptionHandler);
	LIB_FUNC("g0VTBxfJyu0", LibKernel::KernelGetCurrentCpu);
	LIB_FUNC("wzvqT4UqKX8", LibKernel::KernelLoadStartModule);
	LIB_FUNC("Xjoosiw+XPI", LibKernel::KernelUuidCreate);
	LIB_FUNC("DLORcroUqbc", LibKernel::KernelGetOpenPsId);
	LIB_FUNC("zE-wXIZjLoM", LibKernel::KernelDebugRaiseExceptionOnReleaseMode);
	LIB_FUNC("Hc4CaR6JBL0", Posix::KernelSyncOnAddressV1);
	LIB_FUNC("q2y-wDIVWZA", Posix::KernelSyncOnAddressV1);

	AddLibkernelUnityFunc(s, "Qhv5ARAoOEc",
	                      reinterpret_cast<uint64_t>(LibKernel::KernelRemoveExceptionHandler),
	                      "LibKernel::KernelRemoveExceptionHandler");
	AddLibkernelUnityFunc(s, "il03nluKfMk",
	                      reinterpret_cast<uint64_t>(LibKernel::KernelRaiseException),
	                      "LibKernel::KernelRaiseException");
	AddLibkernelUnityFunc(s, "WkwEd3N7w0Y",
	                      reinterpret_cast<uint64_t>(LibKernel::KernelInstallExceptionHandler),
	                      "LibKernel::KernelInstallExceptionHandler");
}

} // namespace Libs
