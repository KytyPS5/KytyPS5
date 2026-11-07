#include "common/hostException.h"

#include <atomic>
#include <cstdio>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#else
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h> // IWYU pragma: keep
#endif
#endif

// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::HostException {

static std::atomic<Handler> g_handler {nullptr};
static_assert(decltype(g_handler)::is_always_lock_free);

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX

// macOS uses the same POSIX platform setting and needs a signal stack too.
class ThreadSignalStack {
public:
	ThreadSignalStack() {
		const auto page_size = static_cast<size_t>(::getpagesize());
		const auto stack_size =
		    (std::max<size_t>(64 * 1024, MINSIGSTKSZ) + page_size - 1) & ~(page_size - 1);
		if (::posix_memalign(&m_memory, page_size, stack_size) != 0) {
			return;
		}

		stack_t stack {};
		stack.ss_sp   = m_memory;
		stack.ss_size = stack_size;
		if (::sigaltstack(&stack, &m_previous) != 0) {
			std::free(m_memory);
			m_memory = nullptr;
		}
	}

	~ThreadSignalStack() {
		if (m_memory != nullptr && ::sigaltstack(&m_previous, nullptr) == 0) {
			std::free(m_memory);
		}
	}

	[[nodiscard]] bool IsInitialized() const { return m_memory != nullptr; }

	KYTY_CLASS_NO_COPY(ThreadSignalStack)

private:
	void*   m_memory = nullptr;
	stack_t m_previous {};
};

bool InitializeThreadSignalStack() {
	// Keep fault handling off guest stacks, which GPU tracking can make read-only.
	thread_local ThreadSignalStack signal_stack;
	return signal_stack.IsInitialized();
}

int GuestSignalDispatchSignal() {
#if defined(__APPLE__)
	// macOS has no realtime signals, and the host uses SIGUSR1 for nothing else.
	return SIGUSR1;
#else
	return SIGRTMIN + 3;
#endif
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static LONG WINAPI ExceptionFilter(PEXCEPTION_POINTERS exception) noexcept {
	auto* exception_record = exception->ExceptionRecord;

	if (exception_record->ExceptionCode == 0x406D1388) {
		// 0x406D1388 only names the thread for an attached debugger.
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	ExceptionInfo info {};
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
	info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);
	info.native_code       = exception_record->ExceptionCode;
	info.native_context    = exception->ContextRecord;

	const auto* registers = exception->ContextRecord;
	info.rax              = registers->Rax;
	info.rbx              = registers->Rbx;
	info.rcx              = registers->Rcx;
	info.rdx              = registers->Rdx;
	info.rsi              = registers->Rsi;
	info.rdi              = registers->Rdi;
	info.rbp              = registers->Rbp;
	info.rsp              = registers->Rsp;
	info.r8               = registers->R8;
	info.r9               = registers->R9;
	info.r10              = registers->R10;
	info.r11              = registers->R11;
	info.r12              = registers->R12;
	info.r13              = registers->R13;
	info.r14              = registers->R14;
	info.r15              = registers->R15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

#else

// x86-64 page-fault error code: bit 4 = instruction fetch, bit 1 = write, otherwise a read.
static AccessViolationType DecodeAccess(const ucontext_t* context) {
#if defined(__APPLE__)
	const auto error = static_cast<uint64_t>(context->uc_mcontext->__es.__err);
#else
	const auto error = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_ERR]);
#endif
	if ((error & 0x10u) != 0) {
		return AccessViolationType::Execute;
	}
	if ((error & 0x2u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}

// A resolved fault returns and retries the instruction; an unresolved one restores the default
// action so the retry terminates the process.
static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
	auto* context = static_cast<ucontext_t*>(native_context);

	ExceptionInfo info {};
#if defined(__APPLE__)
	const auto& registers  = context->uc_mcontext->__ss;
	info.exception_address = registers.__rip;
	info.rax               = registers.__rax;
	info.rbx               = registers.__rbx;
	info.rcx               = registers.__rcx;
	info.rdx               = registers.__rdx;
	info.rsi               = registers.__rsi;
	info.rdi               = registers.__rdi;
	info.rbp               = registers.__rbp;
	info.rsp               = registers.__rsp;
	info.r8                = registers.__r8;
	info.r9                = registers.__r9;
	info.r10               = registers.__r10;
	info.r11               = registers.__r11;
	info.r12               = registers.__r12;
	info.r13               = registers.__r13;
	info.r14               = registers.__r14;
	info.r15               = registers.__r15;
#else
	const auto* registers  = context->uc_mcontext.gregs;
	info.exception_address = static_cast<uint64_t>(registers[REG_RIP]);
	info.rax               = static_cast<uint64_t>(registers[REG_RAX]);
	info.rbx               = static_cast<uint64_t>(registers[REG_RBX]);
	info.rcx               = static_cast<uint64_t>(registers[REG_RCX]);
	info.rdx               = static_cast<uint64_t>(registers[REG_RDX]);
	info.rsi               = static_cast<uint64_t>(registers[REG_RSI]);
	info.rdi               = static_cast<uint64_t>(registers[REG_RDI]);
	info.rbp               = static_cast<uint64_t>(registers[REG_RBP]);
	info.rsp               = static_cast<uint64_t>(registers[REG_RSP]);
	info.r8                = static_cast<uint64_t>(registers[REG_R8]);
	info.r9                = static_cast<uint64_t>(registers[REG_R9]);
	info.r10               = static_cast<uint64_t>(registers[REG_R10]);
	info.r11               = static_cast<uint64_t>(registers[REG_R11]);
	info.r12               = static_cast<uint64_t>(registers[REG_R12]);
	info.r13               = static_cast<uint64_t>(registers[REG_R13]);
	info.r14               = static_cast<uint64_t>(registers[REG_R14]);
	info.r15               = static_cast<uint64_t>(registers[REG_R15]);
#endif
	info.native_code    = static_cast<uint32_t>(signal_number);
	info.native_context = context;
	if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		info.type                   = ExceptionType::AccessViolation;
		info.access_violation_type  = DecodeAccess(context);
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	}

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return;
	}

	struct sigaction restore {};
	restore.sa_handler = SIG_DFL;
	sigemptyset(&restore.sa_mask);
	::sigaction(signal_number, &restore, nullptr);
}

#endif

static bool InstallPlatformHandler() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (AddVectoredExceptionHandler(0, ExceptionFilter) == nullptr) {
		printf("AddVectoredExceptionHandler() failed\n");
		return false;
	}
#else
	struct sigaction action {};
	action.sa_sigaction = SignalHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
	sigaddset(&action.sa_mask, GuestSignalDispatchSignal());

	// macOS raises SIGBUS for some protection faults; SIGILL routes instructions the host
	// cannot execute to the x64 emulator.
	for (const int signal_number: {SIGSEGV, SIGBUS, SIGILL}) {
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			printf("sigaction(%d) failed\n", signal_number);
			return false;
		}
	}
#endif
	return true;
}

// The first handler stays installed; later calls only report whether it is theirs.
bool InstallHandler(Handler handler) {
	static const bool installed = [handler] {
		g_handler.store(handler, std::memory_order_release);
		return InstallPlatformHandler();
	}();
	return installed && g_handler.load(std::memory_order_acquire) == handler;
}

} // namespace Common::HostException
