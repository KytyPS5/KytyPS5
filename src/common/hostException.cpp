#include "common/hostException.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#elif defined(__APPLE__)
#include <csignal>
#include <sys/ucontext.h>
#else
#include <csignal>
#include <initializer_list>
#include <ucontext.h> // IWYU pragma: keep
#include <unistd.h>
#endif

// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::HostException {

#if !defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// A fault the guest handler declines is fatal, and Windows tears the process down without
// printing anything, which leaves a truncated log and no way to tell where it died. Report
// the faulting instruction before letting the search continue. The report is capped because
// this also sees first-chance faults that somebody else's __except may go on to handle.
static std::atomic_uint32_t g_unhandled_reports {0};

static void DumpExceptionRing() noexcept;

static void ReportUnhandledFault(PEXCEPTION_POINTERS exception, const ExceptionInfo& info) noexcept {
	if (g_unhandled_reports.fetch_add(1, std::memory_order_relaxed) >= 4) {
		return;
	}

	const char* access = "unknown";
	switch (info.access_violation_type) {
		case AccessViolationType::Read: access = "read"; break;
		case AccessViolationType::Write: access = "write"; break;
		case AccessViolationType::Execute: access = "execute"; break;
		default: break;
	}

	const auto module_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
	printf("\nUnhandled host fault: code=0x%08" PRIx32 " %s at 0x%016" PRIx64 "\n",
	       info.native_code, access, info.access_violation_vaddr);
	printf("\t rip = 0x%016" PRIx64 " (kyty_emulator.exe base 0x%016" PRIx64 ")\n",
	       info.exception_address, module_base);
	printf("\t rsp = 0x%016" PRIx64 ", rbp = 0x%016" PRIx64 "\n", info.rsp, info.rbp);
	printf("\t thread = %" PRIu32 "\n", static_cast<uint32_t>(GetCurrentThreadId()));

	printf("\t rax=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64 " rbx=%016" PRIx64 "\n",
	       info.rax, info.rcx, info.rdx, info.rbx);
	printf("\t rsi=%016" PRIx64 " rdi=%016" PRIx64 " r8 =%016" PRIx64 " r9 =%016" PRIx64 "\n",
	       info.rsi, info.rdi, info.r8, info.r9);
	printf("\t r10=%016" PRIx64 " r11=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64 "\n",
	       info.r10, info.r11, info.r12, info.r13);
	printf("\t r14=%016" PRIx64 " r15=%016" PRIx64 "\n", info.r14, info.r15);

	// Everything below rsp has already been overwritten by the exception frame this very fault
	// pushed, so it cannot say who clobbered the red zone. What is still original is the memory
	// *above* rsp: the callee-saved registers this frame pushed and the caller frames. If a guest
	// stack was recycled and zero-filled under the running thread, those read back as zeroes while
	// the live registers do not; if only the red zone was hit, they are intact.
	constexpr int64_t DumpBelow = 0xc0;
	constexpr int64_t DumpAbove = 0x200;
	MEMORY_BASIC_INFORMATION region {};
	const auto  low  = reinterpret_cast<const uint8_t*>(info.rsp - DumpBelow);
	const auto  high = reinterpret_cast<const uint8_t*>(info.rsp + DumpAbove);
	const bool  readable =
	    VirtualQuery(low, &region, sizeof(region)) == sizeof(region) &&
	    region.State == MEM_COMMIT &&
	    high <= static_cast<const uint8_t*>(region.BaseAddress) + region.RegionSize;
	printf("\t stack window [rsp-0x%" PRIx64 " .. rsp+0x%" PRIx64 ") readable=%d\n",
	       static_cast<uint64_t>(DumpBelow), static_cast<uint64_t>(DumpAbove),
	       static_cast<int>(readable));
	if (readable) {
		for (int64_t offset = -DumpBelow; offset < DumpAbove; offset += 32) {
			uint64_t words[4] {};
			memcpy(words, reinterpret_cast<const void*>(info.rsp + offset), sizeof(words));
			printf("\t rsp%c0x%03" PRIx64 ": %016" PRIx64 " %016" PRIx64 " %016" PRIx64
			       " %016" PRIx64 "\n",
			       offset < 0 ? '-' : '+', static_cast<uint64_t>(offset < 0 ? -offset : offset),
			       words[0], words[1], words[2], words[3]);
		}
		uint64_t saved[8] {};
		memcpy(saved, reinterpret_cast<const void*>(info.rsp), sizeof(saved));
		int zero_words = 0;
		for (const uint64_t word: saved) {
			zero_words += static_cast<int>(word == 0);
		}
		printf("\t saved-frame words above rsp zero=%d of 8 (a zero-filled guest stack shows 8)\n",
		       zero_words);
	}

	void* frames[32] {};
	const auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
	for (USHORT i = 0; i < count; i++) {
		printf("\t [%02d] 0x%016" PRIx64 "\n", i, reinterpret_cast<uint64_t>(frames[i]));
	}
	DumpExceptionRing();
	fflush(stdout);
}

// Direct measurement of the hazard this patcher exists to remove, rather than of the rare crash it
// eventually causes. Set KYTY_FAULT_WATCH=lo-hi (hex guest addresses) to have every fault taken
// inside that range reported with the stack pointer it was taken at. An instruction the red zone
// patcher covered faults with rsp already biased down by 128; an uncovered one faults at the
// guest's own rsp, which is precisely when the kernel's exception frame lands on live data.
static void ReportWatchedFault(uint64_t rip, uint64_t rsp) noexcept {
	static const auto range = [] {
		std::pair<uint64_t, uint64_t> value {1, 0};
		if (const char* env = std::getenv("KYTY_FAULT_WATCH"); env != nullptr) {
			char*      end = nullptr;
			const auto lo  = std::strtoull(env, &end, 16);
			if (end != env && *end == '-') {
				const char* second = end + 1;
				const auto  hi     = std::strtoull(second, &end, 16);
				if (end != second) {
					value = {lo, hi};
				}
			}
		}
		return value;
	}();
	if (rip < range.first || rip >= range.second) {
		return;
	}

	static std::atomic_uint64_t seen {0};
	const auto count = seen.fetch_add(1, std::memory_order_relaxed) + 1;
	if (count <= 64 || count % 256 == 0) {
		printf("FAULTWATCH n=%" PRIu64 " rip=0x%016" PRIx64 " rsp=0x%016" PRIx64 "\n", count, rip,
		       rsp);
		fflush(stdout);
	}
}

// Every exception the process takes, per thread, in a small ring. Windows dispatches a fault by
// building its exception frame on the faulting thread's own stack, below rsp, so a fault taken
// while a System V guest frame holds live data in its 128-byte red zone destroys that data. The
// ring makes the destroying fault visible after the fact: at the crash it names the instruction
// and the stack pointer of the last faults this thread took.
struct ExceptionRingEntry {
	uint32_t serial = 0;
	uint32_t code   = 0;
	uint64_t rip    = 0;
	uint64_t rsp    = 0;
	uint64_t vaddr  = 0;
};

constexpr uint32_t                  EXCEPTION_RING_SIZE = 24;
static thread_local ExceptionRingEntry g_exception_ring[EXCEPTION_RING_SIZE] {};
static thread_local uint32_t           g_exception_ring_next = 0;
static thread_local uint32_t           g_exception_serial    = 0;

// Where the kernel put the frame it built for this fault, relative to the stack pointer the guest
// was running on. Windows has no red zone, so the frame is free to start immediately below rsp;
// System V guest code keeps live data in the 128 bytes below rsp. Reported once so a run can be
// read without guessing.
static void ReportFrameExtentOnce(PEXCEPTION_POINTERS exception, uint64_t rsp) noexcept {
	static std::atomic_flag reported = ATOMIC_FLAG_INIT;
	if (reported.test_and_set(std::memory_order_relaxed)) {
		return;
	}
	const auto record  = reinterpret_cast<uint64_t>(exception->ExceptionRecord);
	const auto context = reinterpret_cast<uint64_t>(exception->ContextRecord);
	const auto record_top  = record + sizeof(EXCEPTION_RECORD);
	const auto context_top = context + sizeof(CONTEXT);
	const auto top         = record_top > context_top ? record_top : context_top;
	printf("FRAMEEXTENT rsp=0x%016" PRIx64 " record=0x%016" PRIx64 " context=0x%016" PRIx64
	       " frame_top=0x%016" PRIx64 " bytes_of_red_zone_destroyed=%" PRId64 "\n",
	       rsp, record, context, top, static_cast<int64_t>(rsp) - static_cast<int64_t>(top) >= 128
	                                      ? 0
	                                      : 128 - (static_cast<int64_t>(rsp) - static_cast<int64_t>(top)));
	fflush(stdout);
}

static void RecordException(uint32_t code, uint64_t rip, uint64_t rsp, uint64_t vaddr) noexcept {
	auto& entry  = g_exception_ring[g_exception_ring_next++ % EXCEPTION_RING_SIZE];
	entry.serial = ++g_exception_serial;
	entry.code   = code;
	entry.rip    = rip;
	entry.rsp    = rsp;
	entry.vaddr  = vaddr;
}

static void DumpExceptionRing() noexcept {
	printf("\t last faults on this thread (%" PRIu32 " total); a fault taken at the guest's own "
	       "rsp destroys its red zone\n",
	       g_exception_serial);
	for (uint32_t i = 0; i < EXCEPTION_RING_SIZE; i++) {
		const auto& entry = g_exception_ring[(g_exception_ring_next + i) % EXCEPTION_RING_SIZE];
		if (entry.serial == 0) {
			continue;
		}
		printf("\t FAULT n=%" PRIu32 " code=0x%08" PRIx32 " rip=0x%016" PRIx64 " rsp=0x%016" PRIx64
		       " at=0x%016" PRIx64 "\n",
		       entry.serial, entry.code, entry.rip, entry.rsp, entry.vaddr);
	}
}

static LONG WINAPI ExceptionFilter(PEXCEPTION_POINTERS exception) noexcept {
	auto* exception_record = exception->ExceptionRecord;

	if (exception_record->ExceptionCode == DBG_PRINTEXCEPTION_C ||
	    exception_record->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (exception_record->ExceptionCode == 0x406D1388) {
		// Set a thread name.
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	ExceptionInfo info {};
	info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);
	info.native_code       = exception_record->ExceptionCode;
	info.native_context    = exception->ContextRecord;

	if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
		info.type = ExceptionType::AccessViolation;
		switch (exception_record->ExceptionInformation[0]) {
			case 0: info.access_violation_type = AccessViolationType::Read; break;
			case 1: info.access_violation_type = AccessViolationType::Write; break;
			case 8: info.access_violation_type = AccessViolationType::Execute; break;
			default: info.access_violation_type = AccessViolationType::Unknown; break;
		}
		info.access_violation_vaddr = exception_record->ExceptionInformation[1];
	} else if (exception_record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	ReportWatchedFault(reinterpret_cast<uint64_t>(exception_record->ExceptionAddress),
	                   exception->ContextRecord->Rsp);
	ReportFrameExtentOnce(exception, exception->ContextRecord->Rsp);
	RecordException(exception_record->ExceptionCode,
	                reinterpret_cast<uint64_t>(exception_record->ExceptionAddress),
	                exception->ContextRecord->Rsp, info.access_violation_vaddr);

	info.rax = exception->ContextRecord->Rax;
	info.rbx = exception->ContextRecord->Rbx;
	info.rcx = exception->ContextRecord->Rcx;
	info.rdx = exception->ContextRecord->Rdx;
	info.rsi = exception->ContextRecord->Rsi;
	info.rdi = exception->ContextRecord->Rdi;
	info.rbp = exception->ContextRecord->Rbp;
	info.rsp = exception->ContextRecord->Rsp;
	info.r8  = exception->ContextRecord->R8;
	info.r9  = exception->ContextRecord->R9;
	info.r10 = exception->ContextRecord->R10;
	info.r11 = exception->ContextRecord->R11;
	info.r12 = exception->ContextRecord->R12;
	info.r13 = exception->ContextRecord->R13;
	info.r14 = exception->ContextRecord->R14;
	info.r15 = exception->ContextRecord->R15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	// Breakpoints and single-steps also land here whenever a handler declines the
	// event, and those are routine; only a genuine fault is worth reporting.
	if (info.type == ExceptionType::AccessViolation ||
	    info.type == ExceptionType::IllegalInstruction) {
		ReportUnhandledFault(exception, info);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);

// Translate the x86-64 page-fault error code (mcontext __es.__err) into an access type.
// bit 1 (0x2) = write, bit 4 (0x10) = instruction fetch, otherwise a read.
static AccessViolationType DecodeAccess(uint64_t err) {
	if ((err & 0x10u) != 0) {
		return AccessViolationType::Execute;
	}
	if ((err & 0x2u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}

// POSIX signal handler that mirrors the Windows vectored handler: build an ExceptionInfo
// from the mcontext and dispatch. A resolved fault (handler returns true) simply returns,
// re-executing the faulting instruction against the now-fixed protection. An unresolved
// fault restores the default disposition so the retry terminates the process.
static void SignalHandler(int sig, siginfo_t* si, void* uctx) {
	auto*       uc = static_cast<ucontext_t*>(uctx);
	const auto* mc = uc->uc_mcontext;
	const auto& ss = mc->__ss;

	ExceptionInfo info {};
	info.exception_address = ss.__rip;
	info.native_code       = static_cast<uint32_t>(si->si_code);
	info.native_context    = uctx;

	if (sig == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		info.type                   = ExceptionType::AccessViolation;
		info.access_violation_type  = DecodeAccess(mc->__es.__err);
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(si->si_addr);
	}

	info.rax = ss.__rax;
	info.rbx = ss.__rbx;
	info.rcx = ss.__rcx;
	info.rdx = ss.__rdx;
	info.rsi = ss.__rsi;
	info.rdi = ss.__rdi;
	info.rbp = ss.__rbp;
	info.rsp = ss.__rsp;
	info.r8  = ss.__r8;
	info.r9  = ss.__r9;
	info.r10 = ss.__r10;
	info.r11 = ss.__r11;
	info.r12 = ss.__r12;
	info.r13 = ss.__r13;
	info.r14 = ss.__r14;
	info.r15 = ss.__r15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return; // retry the faulting instruction against the fixed mapping
	}

	// Unresolved: restore the default action so the re-executed instruction terminates.
	struct sigaction dfl {};
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	sigaction(sig, &dfl, nullptr);
}

#else

// x86-64 page-fault error bits.
constexpr uint64_t PAGE_FAULT_ERROR_WRITE       = 0x02;
constexpr uint64_t PAGE_FAULT_ERROR_INSTRUCTION = 0x10;

// Let the kernel handle an unresolved fault on retry.
static void ChainToDefault(int signal_number) noexcept {
	struct sigaction restore {};
	restore.sa_handler = SIG_DFL;
	sigemptyset(&restore.sa_mask);
	restore.sa_flags = 0;
	::sigaction(signal_number, &restore, nullptr);
}

static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
	auto* context = static_cast<ucontext_t*>(native_context);
	auto* gregs   = context->uc_mcontext.gregs;

	ExceptionInfo info {};
	info.exception_address = static_cast<uint64_t>(gregs[REG_RIP]);
	info.native_code       = static_cast<uint32_t>(signal_number);
	info.native_context    = context;

	if (signal_number == SIGSEGV || signal_number == SIGBUS) {
		info.type             = ExceptionType::AccessViolation;
		const auto error_code = static_cast<uint64_t>(gregs[REG_ERR]);
		if ((error_code & PAGE_FAULT_ERROR_INSTRUCTION) != 0) {
			info.access_violation_type = AccessViolationType::Execute;
		} else if ((error_code & PAGE_FAULT_ERROR_WRITE) != 0) {
			info.access_violation_type = AccessViolationType::Write;
		} else {
			info.access_violation_type = AccessViolationType::Read;
		}
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	} else if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		ChainToDefault(signal_number);
		return;
	}

	info.rax = static_cast<uint64_t>(gregs[REG_RAX]);
	info.rbx = static_cast<uint64_t>(gregs[REG_RBX]);
	info.rcx = static_cast<uint64_t>(gregs[REG_RCX]);
	info.rdx = static_cast<uint64_t>(gregs[REG_RDX]);
	info.rsi = static_cast<uint64_t>(gregs[REG_RSI]);
	info.rdi = static_cast<uint64_t>(gregs[REG_RDI]);
	info.rbp = static_cast<uint64_t>(gregs[REG_RBP]);
	info.rsp = static_cast<uint64_t>(gregs[REG_RSP]);
	info.r8  = static_cast<uint64_t>(gregs[REG_R8]);
	info.r9  = static_cast<uint64_t>(gregs[REG_R9]);
	info.r10 = static_cast<uint64_t>(gregs[REG_R10]);
	info.r11 = static_cast<uint64_t>(gregs[REG_R11]);
	info.r12 = static_cast<uint64_t>(gregs[REG_R12]);
	info.r13 = static_cast<uint64_t>(gregs[REG_R13]);
	info.r14 = static_cast<uint64_t>(gregs[REG_R14]);
	info.r15 = static_cast<uint64_t>(gregs[REG_R15]);

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return;
	}

	ChainToDefault(signal_number);
}

#endif

bool InstallHandler(Handler handler) {
	if (handler == nullptr) {
		return false;
	}

	uint32_t expected_state = 0;
	if (!g_install_state.compare_exchange_strong(expected_state, 1, std::memory_order_acq_rel)) {
		return expected_state == 2 && g_handler.load(std::memory_order_acquire) == handler;
	}

	g_handler.store(handler, std::memory_order_release);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (AddVectoredExceptionHandler(0, ExceptionFilter) == nullptr) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("AddVectoredExceptionHandler() failed\n");
		return false;
	}
#elif defined(__APPLE__)
	struct sigaction sa {};
	sa.sa_sigaction = SignalHandler;
	sa.sa_flags     = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	// The guest signal-dispatch path (KernelRaiseException) interrupts threads with
	// SIGUSR1; block it while a fault is being resolved so a stop-the-world request
	// cannot preempt the handler between the protection fix and the retry.
	sigaddset(&sa.sa_mask, SIGUSR1);

	// macOS raises SIGBUS for protection faults on some paths and SIGSEGV on others;
	// SIGILL covers instructions the host cannot execute (routed to the x64 emulator).
	bool ok = sigaction(SIGSEGV, &sa, nullptr) == 0 && sigaction(SIGBUS, &sa, nullptr) == 0 &&
	          sigaction(SIGILL, &sa, nullptr) == 0;
	if (!ok) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("sigaction() failed to install the host fault handler\n");
		return false;
	}
#else
	struct sigaction action {};
	action.sa_sigaction = SignalHandler;
	sigemptyset(&action.sa_mask);
	// Fault resolution needs the normal thread stack.
	action.sa_flags = SA_SIGINFO | SA_RESTART;

	for (const int signal_number: {SIGSEGV, SIGBUS, SIGILL}) {
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			g_handler.store(nullptr, std::memory_order_release);
			g_install_state.store(0, std::memory_order_release);
			printf("sigaction(%d) failed\n", signal_number);
			return false;
		}
	}
#endif

	g_install_state.store(2, std::memory_order_release);
	return true;
}

} // namespace Common::HostException
