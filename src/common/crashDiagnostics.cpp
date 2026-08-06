#include "common/crashDiagnostics.h"

#include "common/common.h"
#include "common/fatalLog.h"
#include "common/threads.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#include <intrin.h>
#include <psapi.h>
#endif

namespace Common {

namespace {

constexpr size_t kHleRingSize = 64;
constexpr size_t kHleNameMax  = 96;

struct HleRingEntry {
	uint32_t tid = 0;
	char     library[32] {};
	char     module[32] {};
	char     func[kHleNameMax] {};
};

std::atomic<uint32_t> g_hle_seq {0};
HleRingEntry          g_hle_ring[kHleRingSize] {};
std::atomic<bool>               g_log_guest_av {false};
std::atomic<uint32_t>           g_guest_av_logs {0};
Common::SoftIdleKeepAliveFn     g_soft_idle_keep_alive = nullptr;
char                            g_halt_reason[256] {"(none)"};
std::atomic<uint32_t>           g_halt_reason_seq {0};
std::atomic<bool>               g_cfg_softcontinue_seen {false};
std::atomic<uint32_t>           g_cfg_fault_host_tid {0};
std::atomic<int>                g_cfg_fault_unique_id {-1};

void CopyCapped(char* dst, size_t dst_size, const char* src) {
	if (dst == nullptr || dst_size == 0) {
		return;
	}
	if (src == nullptr) {
		dst[0] = '\0';
		return;
	}
	std::snprintf(dst, dst_size, "%s", src);
}

} // namespace

bool RunExitWatcherIfRequested(int argc, char** argv) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (argc < 4 || argv == nullptr || argv[1] == nullptr) {
		return false;
	}
	if (std::strcmp(argv[1], "--kyty-exit-watch") != 0) {
		return false;
	}
	const DWORD parent_pid = static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10));
	const char* out_path   = argv[3];
	if (parent_pid == 0 || out_path == nullptr || out_path[0] == '\0') {
		return true;
	}
	HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
	if (parent == nullptr) {
		HANDLE file = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
		                          FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			char line[128];
			std::snprintf(line, sizeof(line),
			              "watch_open_failed parent=%lu gle=%lu\r\n",
			              static_cast<unsigned long>(parent_pid),
			              static_cast<unsigned long>(GetLastError()));
			DWORD written = 0;
			WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
			CloseHandle(file);
		}
		return true;
	}
	WaitForSingleObject(parent, INFINITE);
	DWORD code = 0;
	if (GetExitCodeProcess(parent, &code) == 0) {
		code = 0xFFFFFFFFu;
	}
	CloseHandle(parent);
	HANDLE file = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
	                          FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		char line[256];
		std::snprintf(line, sizeof(line),
		              "parent_pid=%lu\r\nexit_code=0x%08lX\r\nexit_code_dec=%lu\r\n",
		              static_cast<unsigned long>(parent_pid), static_cast<unsigned long>(code),
		              static_cast<unsigned long>(code));
		DWORD written = 0;
		WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
		FlushFileBuffers(file);
		CloseHandle(file);
	}
	return true;
#else
	(void)argc;
	(void)argv;
	return false;
#endif
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static void SpawnExitWatcher() {
	char module_path[MAX_PATH] {};
	if (GetModuleFileNameA(nullptr, module_path, MAX_PATH) == 0) {
		return;
	}
	char out_path[MAX_PATH] {};
	std::snprintf(out_path, sizeof(out_path), "%s", module_path);
	char* slash = std::strrchr(out_path, '\\');
	if (slash != nullptr) {
		std::snprintf(slash + 1, sizeof(out_path) - static_cast<size_t>(slash + 1 - out_path),
		              "_kyty_exit_code.txt");
	} else {
		std::snprintf(out_path, sizeof(out_path), "_kyty_exit_code.txt");
	}
	DeleteFileA(out_path);

	char cmd[1024];
	std::snprintf(cmd, sizeof(cmd), "\"%s\" --kyty-exit-watch %lu \"%s\"", module_path,
	              static_cast<unsigned long>(GetCurrentProcessId()), out_path);

	STARTUPINFOA si {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi {};
	if (CreateProcessA(module_path, cmd, nullptr, nullptr, FALSE,
	                   CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi) != 0) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		char message[320];
		std::snprintf(message, sizeof(message), "CrashTrace: exit watcher spawned out=%s",
		              out_path);
		EmergencyLogRaw(message);
	} else {
		char message[160];
		std::snprintf(message, sizeof(message), "CrashTrace: exit watcher spawn FAILED gle=%lu",
		              static_cast<unsigned long>(GetLastError()));
		EmergencyLogRaw(message);
	}
}
#endif

void EnableGuestAccessViolationLogging(bool enable) {
	g_log_guest_av.store(enable, std::memory_order_release);
}

void NoteHleCall(const char* library, const char* module, const char* func) {
	const uint32_t seq = g_hle_seq.fetch_add(1, std::memory_order_relaxed);
	auto&          e   = g_hle_ring[seq % kHleRingSize];
	e.tid              = static_cast<uint32_t>(Thread::GetThreadIdUnique());
	CopyCapped(e.library, sizeof(e.library), library);
	CopyCapped(e.module, sizeof(e.module), module);
	CopyCapped(e.func, sizeof(e.func), func);
}

void FlushHleRingToFatal(const char* reason) {
	char header[192];
	std::snprintf(header, sizeof(header), "HLE ring flush reason=%s seq=%" PRIu32,
	              reason != nullptr ? reason : "(null)", g_hle_seq.load(std::memory_order_relaxed));
	LogFatalToFile(header);

	const uint32_t seq   = g_hle_seq.load(std::memory_order_relaxed);
	const size_t   count = seq < kHleRingSize ? seq : kHleRingSize;
	for (size_t i = 0; i < count; i++) {
		const size_t idx =
		    count < kHleRingSize ? i : static_cast<size_t>((seq - count + i) % kHleRingSize);
		const auto& e = g_hle_ring[idx];
		char        line[256];
		std::snprintf(line, sizeof(line), "  hle[%zu] tid=%u %s::%s::%s", i, e.tid, e.library,
		              e.module, e.func);
		LogFatalToFile(line);
	}
	std::fflush(stderr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
#endif
}

void NoteHaltReason(const char* kind, const char* detail) {
	const uint32_t seq = g_halt_reason_seq.fetch_add(1, std::memory_order_relaxed) + 1;
	char           line[256];
	if (detail != nullptr && detail[0] != '\0') {
		std::snprintf(line, sizeof(line), "HaltReason: kind=%s detail=%s seq=%" PRIu32,
		              kind != nullptr ? kind : "?", detail, seq);
	} else {
		std::snprintf(line, sizeof(line), "HaltReason: kind=%s seq=%" PRIu32,
		              kind != nullptr ? kind : "?", seq);
	}
	std::snprintf(g_halt_reason, sizeof(g_halt_reason), "%s", line);
	EmergencyLogRaw(line);
	LogFatalToFile(line);
	std::fprintf(stderr, "%s\n", line);
	std::fflush(stderr);
}

const char* GetLastHaltReason() {
	return g_halt_reason;
}

void NoteCfgSoftContinue(uint32_t host_tid, int unique_id) {
	g_cfg_softcontinue_seen.store(true, std::memory_order_release);
	if (host_tid != 0) {
		g_cfg_fault_host_tid.store(host_tid, std::memory_order_release);
	}
	if (unique_id >= 0) {
		g_cfg_fault_unique_id.store(unique_id, std::memory_order_release);
	}
}

bool CfgSoftContinueSeen() {
	return g_cfg_softcontinue_seen.load(std::memory_order_acquire);
}

uint32_t CfgSoftContinueFaultHostTid() {
	return g_cfg_fault_host_tid.load(std::memory_order_acquire);
}

int CfgSoftContinueFaultUniqueId() {
	return g_cfg_fault_unique_id.load(std::memory_order_acquire);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

using SecerrHandlerFunc    = void(__cdecl*)(int, void*);
using SetSecerrHandlerFunc = SecerrHandlerFunc(__cdecl*)(SecerrHandlerFunc);

[[maybe_unused]] static void SecurityFailureHandler(int code, void* return_address) {
	// /GS and __fastfail(STACK_COOKIE) do NOT invoke VEH — this handler is the only breadcrumb.
	char module_name[MAX_PATH] = {};
	uint64_t rva = 0;
	if (return_address != nullptr) {
		HMODULE owner_module = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       static_cast<LPCSTR>(return_address), &owner_module) != 0 &&
		    owner_module != nullptr) {
			(void)GetModuleFileNameA(owner_module, module_name, MAX_PATH);
			rva = reinterpret_cast<uint64_t>(return_address) -
			      reinterpret_cast<uint64_t>(owner_module);
		}
	}

	char message[640];
	std::snprintf(message, sizeof(message),
	              "SecurityFailure(/GS|fastfail): code=%d ret=%p rva=0x%016" PRIx64
	              " tid=%lu module=%s",
	              code, return_address, rva, static_cast<unsigned long>(GetCurrentThreadId()),
	              module_name[0] != '\0' ? module_name : "(unknown)");
	EmergencyLogRaw(message);
	FlushHleRingToFatal("security_failure");
	FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
}

static LONG CALLBACK FatalFailfastVectoredHandler(PEXCEPTION_POINTERS exception) {
	if (exception == nullptr || exception->ExceptionRecord == nullptr) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Prevent recursive CrashVEH logging from eating the stack (seen as rsp decreasing
	// ~0xFC0 per frame until STATUS_STACK_OVERFLOW while soft-idle teardown faults).
	static thread_local int tls_veh_depth = 0;
	if (tls_veh_depth > 0) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	struct DepthGuard {
		DepthGuard() { ++tls_veh_depth; }
		~DepthGuard() { --tls_veh_depth; }
	} depth_guard;

	const auto code = static_cast<uint32_t>(exception->ExceptionRecord->ExceptionCode);
	auto*      ctx  = exception->ContextRecord;
	const auto rip  = ctx != nullptr ? ctx->Rip : 0ull;
	const auto rsp  = ctx != nullptr ? ctx->Rsp : 0ull;
	const auto addr =
	    exception->ExceptionRecord->NumberParameters > 1
	        ? exception->ExceptionRecord->ExceptionInformation[1]
	        : 0ull;

	// STACK_OVERFLOW: log once, no heavy work (FlushHleRing / GetModuleHandle can re-fault).
	if (code == 0xC00000FDu) {
		static std::atomic<uint32_t> so_logs {0};
		if (so_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
			char message[256];
			std::snprintf(message, sizeof(message),
			              "FatalFailfast VEH: STACK_OVERFLOW rip=0x%016" PRIx64 " rsp=0x%016" PRIx64,
			              rip, rsp);
			EmergencyLogRaw(message);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Always breadcrumb hard faults (even when guest-AV logging is off) — silent deaths
	// were leaving only UserShadowStack in _kyty_fatal.txt.
	const bool interesting =
	    code == 0xC0000005u || // ACCESS_VIOLATION
	    code == 0xC000001Du || // ILLEGAL_INSTRUCTION
	    code == 0xC0000096u || // PRIVILEGED_INSTRUCTION
	    code == 0xC0000409u || // STACK_BUFFER_OVERRUN
	    code == 0xC0000374u || // HEAP_CORRUPTION
	    code == 0xC0000094u || // INTEGER_DIVIDE_BY_ZERO
	    code == 0x80000003u || // BREAKPOINT (often from bad runtime patches)
	    code == 0xC00002B4u || // FLOAT_MULTIPLE_FAULTS / related
	    code == 0xC00002B5u;
	if (interesting) {
		static std::atomic<uint32_t> hard_logs {0};
		if (hard_logs.fetch_add(1, std::memory_order_relaxed) < 64) {
			char message[384];
			std::snprintf(message, sizeof(message),
			              "CrashVEH: code=0x%08" PRIx32 " rip=0x%016" PRIx64 " rsp=0x%016" PRIx64
			              " addr=0x%016" PRIx64 " tid=%lu",
			              code, rip, rsp, static_cast<uint64_t>(addr),
			              static_cast<unsigned long>(GetCurrentThreadId()));
			EmergencyLogRaw(message);
		}
	}

	if (g_log_guest_av.load(std::memory_order_acquire) &&
	    g_guest_av_logs.fetch_add(1, std::memory_order_relaxed) < 32) {
		char message[384];
		std::snprintf(message, sizeof(message),
		              "GuestExc VEH: code=0x%08" PRIx32 " rip=0x%016" PRIx64
		              " addr=0x%016" PRIx64 " write=%u",
		              code, rip, static_cast<uint64_t>(addr),
		              exception->ExceptionRecord->NumberParameters > 0
		                  ? static_cast<unsigned>(
		                        exception->ExceptionRecord->ExceptionInformation[0])
		                  : 0u);
		LogFatalToFile(message);
		FlushHleRingToFatal("guest_exc");
		FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
	}
	if (code == 0xC0000005u) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	// STATUS_STACK_BUFFER_OVERRUN / HEAP_CORRUPTION — often from __fastfail.
	if (code != 0xC0000409u && code != 0xC0000374u) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	char  module_name[MAX_PATH] = {};
	HMODULE    owner_module = nullptr;
	if (rip != 0 &&
	    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCSTR>(rip), &owner_module) != 0 &&
	    owner_module != nullptr) {
		(void)GetModuleFileNameA(owner_module, module_name, MAX_PATH);
	}

	char message[768];
	const auto info0 =
	    exception->ExceptionRecord->NumberParameters > 0
	        ? exception->ExceptionRecord->ExceptionInformation[0]
	        : 0ull;
	const auto info1 =
	    exception->ExceptionRecord->NumberParameters > 1
	        ? exception->ExceptionRecord->ExceptionInformation[1]
	        : 0ull;
	std::snprintf(message, sizeof(message),
	              "FatalFailfast VEH: code=0x%08" PRIx32 ", addr=0x%016" PRIx64
	              ", rip=0x%016" PRIx64 ", rsp=0x%016" PRIx64
	              ", info0=0x%016" PRIx64 ", info1=0x%016" PRIx64 ", module=%s",
	              code, reinterpret_cast<uint64_t>(exception->ExceptionRecord->ExceptionAddress),
	              rip, ctx != nullptr ? ctx->Rsp : 0ull, static_cast<uint64_t>(info0),
	              static_cast<uint64_t>(info1),
	              module_name[0] != '\0' ? module_name : "(unknown)");
	LogFatalToFile(message);
	if (ctx != nullptr) {
		char regs[384];
		std::snprintf(regs, sizeof(regs),
		              "  failfast regs rax=0x%016" PRIx64 " rcx=0x%016" PRIx64
		              " rdx=0x%016" PRIx64 " rbx=0x%016" PRIx64 " rsi=0x%016" PRIx64
		              " rdi=0x%016" PRIx64,
		              ctx->Rax, ctx->Rcx, ctx->Rdx, ctx->Rbx, ctx->Rsi, ctx->Rdi);
		LogFatalToFile(regs);
	}
	FlushHleRingToFatal("veh_failfast");
	FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
	return EXCEPTION_CONTINUE_SEARCH;
}

#endif

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
	if (info != nullptr && info->ExceptionRecord != nullptr) {
		char message[256];
		const auto* ctx = info->ContextRecord;
		std::snprintf(message, sizeof(message),
		              "UnhandledExceptionFilter: code=0x%08" PRIx32 " rip=0x%016" PRIx64
		              " tid=%u",
		              static_cast<uint32_t>(info->ExceptionRecord->ExceptionCode),
		              ctx != nullptr ? ctx->Rip : 0ull, GetCurrentThreadId());
		EmergencyLogRaw(message);
		FlushHleRingToFatal("unhandled_filter");
	} else {
		EmergencyLogRaw("UnhandledExceptionFilter: (null info)");
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL WINAPI ConsoleCtrlFilter(DWORD ctrl) {
	char message[96];
	std::snprintf(message, sizeof(message), "ConsoleCtrl: type=%" PRIu32, ctrl);
	EmergencyLogRaw(message);
	LogFatalToFile(message);
	// CTRL_CLOSE_EVENT (2): keep process alive so a hung RegisterBuffers2 / soft-idle is not
	// misreported as a clean exit (watcher would otherwise see 0xC000013A).
	if (ctrl == 2u /* CTRL_CLOSE_EVENT */) {
		return TRUE;
	}
	return FALSE;
}

static void AtexitBreadcrumb() {
	EmergencyLogRaw("atexit: process exiting");
}

static void TerminateBreadcrumb() {
	EmergencyLogRaw("std::terminate called");
	std::abort();
}

using ExitProcessFn      = void(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);

static ExitProcessFn      g_real_exit_process      = nullptr;
static TerminateProcessFn g_real_terminate_process = nullptr;
static std::atomic<uint32_t> g_exit_hook_logs {0};

static bool WriteAbsJmp(void* from, const void* to) {
	auto* p = static_cast<uint8_t*>(from);
	// mov rax, imm64 ; jmp rax
	p[0] = 0x48;
	p[1] = 0xB8;
	const auto imm = reinterpret_cast<uint64_t>(to);
	std::memcpy(p + 2, &imm, sizeof(imm));
	p[10] = 0xFF;
	p[11] = 0xE0;
	return true;
}

// tramp_steal = complete instructions copied into the trampoline (must not include
// rip-relative call/jcc that would break when relocated).
// patch_len   = bytes overwritten at target (>= 12 for abs jmp); may be > tramp_steal.
static bool InstallInlineHook(void* target, void* hook, void* trampoline, size_t tramp_size,
                              void** original_out, size_t tramp_steal = 16,
                              size_t patch_len = 0) {
	if (patch_len == 0) {
		patch_len = tramp_steal;
	}
	if (target == nullptr || hook == nullptr || trampoline == nullptr || tramp_size < 48 ||
	    tramp_steal < 1 || tramp_steal > 32 || patch_len < 12 || patch_len > 32 ||
	    tramp_steal + 12 > tramp_size) {
		return false;
	}
	DWORD old_prot = 0;
	if (VirtualProtect(target, patch_len, PAGE_EXECUTE_READWRITE, &old_prot) == 0) {
		return false;
	}
	std::memcpy(trampoline, target, tramp_steal);
	// Resume after the overwritten region — never mid-patch (tramp_steal < patch_len).
	WriteAbsJmp(static_cast<uint8_t*>(trampoline) + tramp_steal,
	            static_cast<uint8_t*>(target) + patch_len);
	DWORD tramp_prot = 0;
	VirtualProtect(trampoline, tramp_size, PAGE_EXECUTE_READWRITE, &tramp_prot);
	FlushInstructionCache(GetCurrentProcess(), trampoline, tramp_size);

	WriteAbsJmp(target, hook);
	auto* t = static_cast<uint8_t*>(target);
	for (size_t i = 12; i < patch_len; ++i) {
		t[i] = 0x90;
	}
	FlushInstructionCache(GetCurrentProcess(), target, patch_len);
	DWORD ignored = 0;
	VirtualProtect(target, patch_len, old_prot, &ignored);
	if (original_out != nullptr) {
		*original_out = trampoline;
	}
	return true;
}

// ucrt/vcruntime __report_gsfailure prologue:
//   mov [rsp+8],rcx ; sub rsp,38h ; mov ecx,17h ; call [rip+...]
// Stealing 16 bytes splits the call — use 14 (through mov ecx).
static size_t ReportGsFailureStealSize(const void* target) {
	const auto* p = static_cast<const uint8_t*>(target);
	if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x4c && p[3] == 0x24 && p[4] == 0x08 &&
	    p[5] == 0x48 && p[6] == 0x83 && p[7] == 0xec && p[9] == 0xb9) {
		return 14;
	}
	return 16;
}

static void LogExitHookFrames(const char* tag, UINT code) {
	if (g_exit_hook_logs.fetch_add(1, std::memory_order_relaxed) >= 32) {
		return;
	}
	void* frames[10] = {};
	const USHORT n =
	    CaptureStackBackTrace(2, static_cast<DWORD>(sizeof(frames) / sizeof(frames[0])), frames,
	                          nullptr);
	char message[384];
	std::snprintf(message, sizeof(message),
	              "HOOK %s code=%u tid=%lu frames=%u ret0=%p ret1=%p ret2=%p ret3=%p", tag, code,
	              static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned>(n),
	              n > 0 ? frames[0] : nullptr, n > 1 ? frames[1] : nullptr,
	              n > 2 ? frames[2] : nullptr, n > 3 ? frames[3] : nullptr);
	EmergencyLogRaw(message);
	FlushHleRingToFatal(tag);
	FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
}

static void SoftIdleForever(const char* why, UINT code) {
	if (code == 321u || code == 0xC0000025u || code == 0xC0000417u) {
		char detail[96];
		std::snprintf(detail, sizeof(detail), "%s code=%u", why != nullptr ? why : "exit", code);
		const char* kind = "ExitProcess321";
		if (code == 0xC0000025u) {
			kind = "TerminateNonContinuable";
		} else if (code == 0xC0000417u) {
			kind = "InvalidCruntimeParameter";
		}
		NoteHaltReason(kind, detail);
	}
	char message[320];
	std::snprintf(message, sizeof(message),
	              "GuestExit: intercepted %s code=%u — soft-idle (KYTY_ALLOW_GUEST_EXIT=1 to honor) "
	              "last=%s",
	              why != nullptr ? why : "exit", code, g_halt_reason);
	EmergencyLogRaw(message);
	LogFatalToFile(message);
	std::fprintf(stderr, "%s\n", message);
	FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
	if (g_soft_idle_keep_alive != nullptr) {
		g_soft_idle_keep_alive(180);
	}
	for (uint32_t tick = 0;; ++tick) {
		Sleep(1000);
		if (g_soft_idle_keep_alive != nullptr && (tick % 30) == 0) {
			g_soft_idle_keep_alive(60);
		}
	}
}

static bool AllowGuestProcessExit() {
	const char* allow = std::getenv("KYTY_ALLOW_GUEST_EXIT");
	return allow != nullptr && allow[0] == '1';
}

// Soft-idle only Kyty EXIT_HALT (321) and FailFast NONCONTINUABLE. Never block:
// ExitProcess(0), exit(1) CRT failures, Ctrl+C (STATUS_CONTROL_C_EXIT).
static bool ShouldSoftIdleExitCode(UINT code) {
	if (AllowGuestProcessExit()) {
		return false;
	}
	if (code == 321u) {
		return true;
	}
	if (code == 0xC0000025u) { // EXCEPTION_NONCONTINUABLE_EXCEPTION (HostException::FailFast)
		return true;
	}
	// CRT invalid-parameter / abort teardown — soft-idle so we can inspect RegisterBuffers2.
	if (code == 0xC0000417u) { // STATUS_INVALID_CRUNTIME_PARAMETER
		return true;
	}
	return false;
}

static void WINAPI HookedExitProcess(UINT code) {
	LogExitHookFrames("ExitProcess", code);
	if (ShouldSoftIdleExitCode(code)) {
		SoftIdleForever("ExitProcess", code);
	}
	if (g_real_exit_process != nullptr) {
		g_real_exit_process(code);
	}
	for (;;) {
		Sleep(1000);
	}
}

static BOOL WINAPI HookedTerminateProcess(HANDLE process, UINT code) {
	HANDLE self = GetCurrentProcess();
	const bool self_target =
	    process == self || process == reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)) ||
	    process == nullptr;
	if (self_target) {
		LogExitHookFrames("TerminateProcess", code);
		if (ShouldSoftIdleExitCode(code)) {
			SoftIdleForever("TerminateProcess", code);
		}
	}
	if (g_real_terminate_process != nullptr) {
		return g_real_terminate_process(process, code);
	}
	return FALSE;
}

alignas(16) static uint8_t g_gs_fail_tramps[8][64] {};
static void* g_gs_hooked_targets[8] {};
static HANDLE g_gs_log_file = INVALID_HANDLE_VALUE;
static char   g_gs_msg[256] {};
static volatile long g_gs_hit_count = 0;

static void GsAppendHex(char*& p, char* end, uint64_t value, int width) {
	static constexpr char kHex[] = "0123456789abcdef";
	for (int i = width - 1; i >= 0 && p < end; --i) {
		*p++ = kHex[(value >> (static_cast<unsigned>(i) * 4u)) & 0xfu];
	}
}

static void GsWriteMsg(const char* msg, size_t len) {
	if (msg == nullptr || len == 0) {
		return;
	}
	DWORD written = 0;
	if (g_gs_log_file != nullptr && g_gs_log_file != INVALID_HANDLE_VALUE) {
		(void)WriteFile(g_gs_log_file, msg, static_cast<DWORD>(len), &written, nullptr);
		(void)WriteFile(g_gs_log_file, "\r\n", 2, &written, nullptr);
		FlushFileBuffers(g_gs_log_file);
	}
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	if (err != nullptr && err != INVALID_HANDLE_VALUE) {
		(void)WriteFile(err, msg, static_cast<DWORD>(len), &written, nullptr);
		(void)WriteFile(err, "\r\n", 2, &written, nullptr);
	}
}

// Soft-continue: log with static buffers only (no fmt / large locals), then return so
// __security_check_cookie's caller resumes instead of __fastfail.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline, no_stack_protector))
#endif
static void __cdecl HookedReportGsFailure(uintptr_t cookie) {
	void* const retaddr = _ReturnAddress();
	const long hit = InterlockedIncrement(&g_gs_hit_count);
	char* p = g_gs_msg;
	char* const end = g_gs_msg + sizeof(g_gs_msg) - 1;
	const char prefix[] = "HOOK __report_gsfailure soft hit=";
	for (const char* s = prefix; *s != '\0' && p < end; ++s) {
		*p++ = *s;
	}
	GsAppendHex(p, end, static_cast<uint64_t>(hit), 8);
	const char mid[] = " cookie=0x";
	for (const char* s = mid; *s != '\0' && p < end; ++s) {
		*p++ = *s;
	}
	GsAppendHex(p, end, cookie, 16);
	const char mid2[] = " ret=";
	for (const char* s = mid2; *s != '\0' && p < end; ++s) {
		*p++ = *s;
	}
	GsAppendHex(p, end, reinterpret_cast<uint64_t>(retaddr), 16);
	*p = '\0';
	GsWriteMsg(g_gs_msg, static_cast<size_t>(p - g_gs_msg));

	HMODULE owner = nullptr;
	if (retaddr != nullptr &&
	    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       static_cast<LPCSTR>(retaddr), &owner) != 0 &&
	    owner != nullptr) {
		char* q = g_gs_msg;
		const char pfx[] = "HOOK __report_gsfailure rva=0x";
		for (const char* s = pfx; *s != '\0' && q < end; ++s) {
			*q++ = *s;
		}
		GsAppendHex(q, end,
		            reinterpret_cast<uint64_t>(retaddr) - reinterpret_cast<uint64_t>(owner), 16);
		*q = '\0';
		GsWriteMsg(g_gs_msg, static_cast<size_t>(q - g_gs_msg));
	}
}

static void OpenGsLogFile() {
	char module_path[MAX_PATH] {};
	const DWORD n = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return;
	}
	char* slash = std::strrchr(module_path, '\\');
	if (slash == nullptr) {
		return;
	}
	*(slash + 1) = '\0';
	char path[MAX_PATH + 32] {};
	std::snprintf(path, sizeof(path), "%s_kyty_gsfail.txt", module_path);
	g_gs_log_file =
	    CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static int NopSecurityCheckCookies(HMODULE mod) {
	// Replace __security_check_cookie entry with a bare `ret`.
	// Nopping only the first jne is NOT enough: the complement check
	// (`rol` / `test cx` / `jne` / `jmp __report_gsfailure`) still failfasts.
	if (mod == nullptr) {
		return 0;
	}
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return 0;
	}
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return 0;
	}
	auto* section = IMAGE_FIRST_SECTION(nt);
	int patched = 0;
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
			continue;
		}
		auto* start = reinterpret_cast<uint8_t*>(mod) + section->VirtualAddress;
		const size_t size = section->Misc.VirtualSize;
		for (size_t off = 0; off + 28 <= size; ++off) {
			uint8_t* p = start + off;
			if (p[0] != 0x48 || p[1] != 0x3b || p[2] != 0x0d || p[7] != 0x75) {
				continue;
			}
			bool cookie_like = false;
			for (size_t k = 8; k + 4 < 28 && off + k + 4 < size; ++k) {
				if (p[k] == 0x48 && p[k + 1] == 0xc1 && p[k + 2] == 0xc1 && p[k + 3] == 0x10) {
					cookie_like = true;
					break;
				}
			}
			if (!cookie_like) {
				continue;
			}
			DWORD old_prot = 0;
			if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old_prot) == 0) {
				continue;
			}
			p[0] = 0xc3; // ret — ignore cookie entirely
			FlushInstructionCache(GetCurrentProcess(), p, 1);
			DWORD ignored = 0;
			VirtualProtect(p, 1, old_prot, &ignored);
			++patched;
		}
	}
	return patched;
}

// Neutralize inlined `mov ecx, 2; int 29h` (FAST_FAIL_STACK_COOKIE) — used by
// __report_gsfailure and some CRT epilogues that skip __security_check_cookie.
static int PatchFastFailStackCookie(HMODULE mod) {
	if (mod == nullptr) {
		return 0;
	}
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return 0;
	}
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return 0;
	}
	auto* section = IMAGE_FIRST_SECTION(nt);
	int patched = 0;
	static constexpr uint8_t kPat[] = {0xb9, 0x02, 0x00, 0x00, 0x00, 0xcd, 0x29};
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
			continue;
		}
		auto* start = reinterpret_cast<uint8_t*>(mod) + section->VirtualAddress;
		const size_t size = section->Misc.VirtualSize;
		for (size_t off = 0; off + sizeof(kPat) <= size; ++off) {
			uint8_t* p = start + off;
			if (std::memcmp(p, kPat, sizeof(kPat)) != 0) {
				continue;
			}
			DWORD old_prot = 0;
			if (VirtualProtect(p, sizeof(kPat), PAGE_EXECUTE_READWRITE, &old_prot) == 0) {
				continue;
			}
			std::memset(p, 0x90, sizeof(kPat));
			FlushInstructionCache(GetCurrentProcess(), p, sizeof(kPat));
			DWORD ignored = 0;
			VirtualProtect(p, sizeof(kPat), old_prot, &ignored);
			++patched;
		}
	}
	return patched;
}

// Patch `int 29h` only when clearly a FAST_FAIL sequence: `mov ecx, imm32; int 29h`.
// Blanket CD 29 scans corrupt unrelated instructions (especially in ntdll).
static int PatchFastFailInt29(HMODULE mod) {
	if (mod == nullptr) {
		return 0;
	}
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return 0;
	}
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return 0;
	}
	auto* section = IMAGE_FIRST_SECTION(nt);
	int patched = 0;
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
			continue;
		}
		auto* start = reinterpret_cast<uint8_t*>(mod) + section->VirtualAddress;
		const size_t size = section->Misc.VirtualSize;
		for (size_t off = 0; off + 7 <= size; ++off) {
			uint8_t* p = start + off;
			// mov ecx, imm32 ; int 29h
			if (p[0] != 0xb9 || p[5] != 0xcd || p[6] != 0x29) {
				continue;
			}
			DWORD old_prot = 0;
			if (VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old_prot) == 0) {
				continue;
			}
			std::memset(p, 0x90, 7);
			FlushInstructionCache(GetCurrentProcess(), p, 7);
			DWORD ignored = 0;
			VirtualProtect(p, 7, old_prot, &ignored);
			++patched;
		}
	}
	return patched;
}

static void BypassGsInAllModules() {
	HMODULE mods[256] {};
	DWORD needed = 0;
	int cookie_rets = 0;
	int fastfail_nops = 0;
	int int29_nops = 0;
	int modules = 0;
	// Safe pattern only (`mov ecx,imm32; int 29h`) — OK on all modules including ntdll.
	// Never blanket-nop bare CD 29 (false positives → 0x80000003).
	if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) != 0) {
		const unsigned count =
		    static_cast<unsigned>(needed / sizeof(HMODULE) < 256 ? needed / sizeof(HMODULE) : 256);
		for (unsigned i = 0; i < count; ++i) {
			cookie_rets += NopSecurityCheckCookies(mods[i]);
			fastfail_nops += PatchFastFailStackCookie(mods[i]);
			int29_nops += PatchFastFailInt29(mods[i]);
			++modules;
		}
	} else {
		HMODULE exe = GetModuleHandleW(nullptr);
		cookie_rets += NopSecurityCheckCookies(exe);
		fastfail_nops += PatchFastFailStackCookie(exe);
		int29_nops += PatchFastFailInt29(exe);
		modules = 1;
	}
	char message[192];
	std::snprintf(message, sizeof(message),
	              "CrashTrace: GS bypass modules=%d cookie_ret=%d fastfail2_nops=%d "
	              "int29_safe_nops=%d",
	              modules, cookie_rets, fastfail_nops, int29_nops);
	EmergencyLogRaw(message);
}

static bool HookReportGsFailureTarget(void* target, const char* tag, int* hooked) {
	if (target == nullptr || hooked == nullptr) {
		return false;
	}
	const int max_hooks =
	    static_cast<int>(sizeof(g_gs_fail_tramps) / sizeof(g_gs_fail_tramps[0]));
	if (*hooked >= max_hooks) {
		return false;
	}
	for (int i = 0; i < *hooked; ++i) {
		if (g_gs_hooked_targets[i] == target) {
			return false;
		}
	}
	const size_t stolen = ReportGsFailureStealSize(target);
	void* orig = nullptr;
	if (!InstallInlineHook(target, reinterpret_cast<void*>(&HookedReportGsFailure),
	                       g_gs_fail_tramps[*hooked], sizeof(g_gs_fail_tramps[0]), &orig, stolen)) {
		char message[192];
		std::snprintf(message, sizeof(message),
		              "CrashTrace: __report_gsfailure hook FAILED at %s", tag);
		EmergencyLogRaw(message);
		return false;
	}
	g_gs_hooked_targets[*hooked] = target;
	++(*hooked);
	char message[224];
	std::snprintf(message, sizeof(message),
	              "CrashTrace: __report_gsfailure INLINE hooked in %s stolen=%zu ptr=%p", tag,
	              stolen, target);
	EmergencyLogRaw(message);
	return true;
}

static void ScanModuleForLocalReportGsFailure(HMODULE mod, const char* tag, int* hooked) {
	// kyty_emulator.exe does NOT import __report_gsfailure — the CRT startup object links a
	// private copy that __security_check_cookie jmps to, then int 29h. Scan for that prologue.
	if (mod == nullptr || hooked == nullptr) {
		return;
	}
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return;
	}
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return;
	}
	auto* section = IMAGE_FIRST_SECTION(nt);
	static constexpr uint8_t kPrologue[] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x48, 0x83, 0xec,
	                                        0x38, 0xb9, 0x17, 0x00, 0x00, 0x00};
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
			continue;
		}
		auto* start = reinterpret_cast<uint8_t*>(mod) + section->VirtualAddress;
		const size_t size = section->Misc.VirtualSize;
		if (size < sizeof(kPrologue)) {
			continue;
		}
		for (size_t off = 0; off + sizeof(kPrologue) <= size; ++off) {
			if (std::memcmp(start + off, kPrologue, sizeof(kPrologue)) != 0) {
				continue;
			}
			char local_tag[96];
			std::snprintf(local_tag, sizeof(local_tag), "%s+.text+0x%zx", tag, off);
			(void)HookReportGsFailureTarget(start + off, local_tag, hooked);
		}
	}
}

static void InstallGsFailureHook() {
	OpenGsLogFile();
	int hooked = 0;
	// Only hook the LOCAL exe copy — CRT exports are irrelevant here and a 16-byte
	// steal on vcruntime can split a call instruction.
	HMODULE exe = GetModuleHandleW(nullptr);
	ScanModuleForLocalReportGsFailure(exe, "kyty_emulator.exe", &hooked);
	char message[96];
	std::snprintf(message, sizeof(message), "CrashTrace: __report_gsfailure hooks=%d", hooked);
	EmergencyLogRaw(message);

	BypassGsInAllModules();
}

static bool PatchIatInModule(HMODULE module, const char* dll_name, const char* func_name,
                             void* hook, void** original_out) {
	if (module == nullptr || dll_name == nullptr || func_name == nullptr || hook == nullptr) {
		return false;
	}
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return false;
	}
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return false;
	}
	const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0 || dir.Size == 0) {
		return false;
	}
	auto* import_desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
	    reinterpret_cast<uint8_t*>(module) + dir.VirtualAddress);
	for (; import_desc->Name != 0; ++import_desc) {
		const char* name =
		    reinterpret_cast<const char*>(reinterpret_cast<uint8_t*>(module) + import_desc->Name);
		if (_stricmp(name, dll_name) != 0) {
			continue;
		}
		auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<uint8_t*>(module) +
		                                                  import_desc->FirstThunk);
		auto* orig_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
		    reinterpret_cast<uint8_t*>(module) +
		    (import_desc->OriginalFirstThunk != 0 ? import_desc->OriginalFirstThunk
		                                          : import_desc->FirstThunk));
		for (; orig_thunk->u1.AddressOfData != 0; ++thunk, ++orig_thunk) {
			if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) {
				continue;
			}
			auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
			    reinterpret_cast<uint8_t*>(module) + orig_thunk->u1.AddressOfData);
			if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), func_name) != 0) {
				continue;
			}
			DWORD old_protect = 0;
			if (VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE,
			                   &old_protect) == 0) {
				return false;
			}
			if (original_out != nullptr && *original_out == nullptr) {
				*original_out = reinterpret_cast<void*>(static_cast<uintptr_t>(thunk->u1.Function));
			}
			thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
			DWORD ignored = 0;
			VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), old_protect, &ignored);
			return true;
		}
	}
	return false;
}

static void InstallExitHooks() {
	HMODULE modules[512] {};
	DWORD   bytes = 0;
	if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes) == 0) {
		modules[0] = GetModuleHandleA(nullptr);
		bytes      = sizeof(HMODULE);
	}
	const size_t count = bytes / sizeof(HMODULE);
	void*        exit_orig = nullptr;
	void*        term_orig = nullptr;
	const char* dlls[] = {"KERNEL32.dll",
	                      "kernel32.dll",
	                      "KERNELBASE.dll",
	                      "ntdll.dll",
	                      "NTDLL.dll",
	                      "api-ms-win-core-processthreads-l1-1-0.dll",
	                      "api-ms-win-core-processthreads-l1-1-1.dll",
	                      "api-ms-win-core-processthreads-l1-1-2.dll"};
	int exit_hits = 0;
	int term_hits = 0;
	for (size_t mi = 0; mi < count; ++mi) {
		if (modules[mi] == nullptr) {
			continue;
		}
		for (const char* dll: dlls) {
			void* scratch_exit = exit_orig;
			void* scratch_term = term_orig;
			if (PatchIatInModule(modules[mi], dll, "ExitProcess",
			                     reinterpret_cast<void*>(&HookedExitProcess), &scratch_exit)) {
				exit_orig = scratch_exit;
				++exit_hits;
			}
			if (PatchIatInModule(modules[mi], dll, "TerminateProcess",
			                     reinterpret_cast<void*>(&HookedTerminateProcess), &scratch_term)) {
				term_orig = scratch_term;
				++term_hits;
			}
		}
	}
	g_real_exit_process =
	    exit_orig != nullptr ? reinterpret_cast<ExitProcessFn>(exit_orig) : ExitProcess;
	g_real_terminate_process = term_orig != nullptr
	                               ? reinterpret_cast<TerminateProcessFn>(term_orig)
	                               : TerminateProcess;

	// Do not hook ntdll RtlExitUserProcess / NtTerminateProcess — fragile trampolines and
	// re-entrancy. Soft-idle 321 is covered by ExitProcess + TerminateProcess IAT hooks.
	EmergencyLogRaw("CrashTrace: NtTerminateProcess/RtlExitUserProcess unhooked (passthrough)");
	char message[256];
	std::snprintf(message, sizeof(message),
	              "CrashTrace: Exit=%d Term=%d modules=%zu", exit_hits, term_hits, count);
	EmergencyLogRaw(message);
}

void InstallCrashDiagnostics() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef ProcessUserShadowStackPolicy
#define ProcessUserShadowStackPolicy static_cast<PROCESS_MITIGATION_POLICY>(9)
#endif
	PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY shadow_stack_policy {};
	if (SetProcessMitigationPolicy(ProcessUserShadowStackPolicy, &shadow_stack_policy,
	                               sizeof(shadow_stack_policy)) == 0) {
		::printf("Warning: SetProcessMitigationPolicy(UserShadowStack) failed: 0x%08" PRIx32 "\n",
		         static_cast<uint32_t>(GetLastError()));
	}

	PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY shadow_stack_state {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy,
	                               &shadow_stack_state, sizeof(shadow_stack_state)) != 0) {
		char message[256];
		std::snprintf(
		    message, sizeof(message),
		    "UserShadowStack policy: Flags=0x%08" PRIx32
		    " Enable=%u Audit=%u SetContextIpValidation=%u Strict=%u",
		    static_cast<uint32_t>(shadow_stack_state.Flags),
		    static_cast<unsigned>(shadow_stack_state.EnableUserShadowStack),
		    static_cast<unsigned>(shadow_stack_state.AuditUserShadowStack),
		    static_cast<unsigned>(shadow_stack_state.SetContextIpValidation),
		    static_cast<unsigned>(shadow_stack_state.EnableUserShadowStackStrictMode));
		::printf("%s\n", message);
		LogFatalToFile(message);
	} else {
		::printf("Warning: GetProcessMitigationPolicy(UserShadowStack) failed: 0x%08" PRIx32 "\n",
		         static_cast<uint32_t>(GetLastError()));
	}

	if (AddVectoredExceptionHandler(1, FatalFailfastVectoredHandler) == nullptr) {
		::printf("Warning: AddVectoredExceptionHandler(FatalFailfast) failed\n");
	} else {
		::printf("CrashTrace: FatalFailfast VEH installed\n");
	}

	SetUnhandledExceptionFilter(UnhandledFilter);
	SetConsoleCtrlHandler(ConsoleCtrlFilter, TRUE);
	std::atexit(AtexitBreadcrumb);
	std::set_terminate(TerminateBreadcrumb);
	InstallExitHooks();
	SpawnExitWatcher();
	InstallGsFailureHook();

	EmergencyLogRaw("CrashTrace: emergency log + heartbeat + unhandled filter armed");

	// Background pulse — if this stops updating, death was not a clean Exit/Terminate path.
	std::thread([] {
		for (uint32_t i = 0;; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			char beat[80];
			std::snprintf(beat, sizeof(beat), "heartbeat watchdog tick=%u tid=%lu", i,
			              static_cast<unsigned long>(GetCurrentThreadId()));
			HeartbeatLog(beat);
		}
	}).detach();

	// `_set_security_error_handler` is gone on modern UCRT; rely on InstallGsFailureHook above.
	EmergencyLogRaw("CrashTrace: security_error_handler skipped (use __report_gsfailure hook)");
#endif
}

void SetSoftIdleKeepAlive(SoftIdleKeepAliveFn fn) {
	g_soft_idle_keep_alive = fn;
}

} // namespace Common
