#include "common/guardedCopy.h"

#if defined(_WIN32)

#include <windows.h>

extern "C" {
uint64_t   kyty_guarded_copy(void* destination, const void* source, uint64_t size);
extern const char kyty_guarded_copy_fault[];
extern const char kyty_guarded_copy_resume[];
}

// rep movsb: a fault leaves rip on the instruction, rcx the bytes not copied and rsi, rdi past the copied ones.
// Win64: destination rcx, source rdx, size r8 (the direction flag clear); rdi and rsi are the caller's, kept in r9
// and r10: a leaf function without a frame needs no unwind data.
// (A function-level asm statement, not a module-level one: clang-cl assembles module-level asm in Intel syntax without
// -flto and in AT&T with it; a statement's syntax is its own.)
extern "C" [[gnu::naked, gnu::noinline]] uint64_t kyty_guarded_copy(void*, const void*, uint64_t) {
	__asm__ volatile(".globl kyty_guarded_copy_fault\n\t"
	                 ".globl kyty_guarded_copy_resume\n\t"
	                 "movq %rdi, %r9\n\t"
	                 "movq %rsi, %r10\n\t"
	                 "movq %rcx, %rdi\n\t"
	                 "movq %rdx, %rsi\n\t"
	                 "movq %r8, %rcx\n\t"
	                 "kyty_guarded_copy_fault:\n\t"
	                 "rep movsb\n\t"
	                 "kyty_guarded_copy_resume:\n\t"
	                 "movq %rcx, %rax\n\t"
	                 "movq %r9, %rdi\n\t"
	                 "movq %r10, %rsi\n\t"
	                 "retq\n\t");
}

namespace Common {

uint64_t GuardedCopy(void* destination, const void* source, uint64_t size) noexcept {
	return size == 0 ? 0 : kyty_guarded_copy(destination, source, size);
}

bool ResumeGuardedCopy(void* context) noexcept {
	auto* registers = static_cast<CONTEXT*>(context);
	if (registers->Rip != reinterpret_cast<DWORD64>(kyty_guarded_copy_fault)) {
		return false;
	}
	registers->Rip = reinterpret_cast<DWORD64>(kyty_guarded_copy_resume);
	return true;
}

} // namespace Common

#else

#include <fcntl.h>
#include <unistd.h>

namespace Common {

uint64_t GuardedCopy(void* destination, const void* source, uint64_t size) noexcept {
	if (size == 0) {
		return 0;
	}
	// /proc/self/mem serves this process's whole address space: a read of an unmapped page fails with EIO and a read
	// that spans into one returns a short count, so what was copied is exactly what was readable where it started.
	static const int mem = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
	if (mem < 0) {
		return size;
	}
	auto*       dst    = static_cast<uint8_t*>(destination);
	const auto* src    = static_cast<const uint8_t*>(source);
	uint64_t    copied = 0;
	while (copied < size) {
		const auto done = ::pread(mem, dst + copied, static_cast<size_t>(size - copied),
		                          static_cast<off_t>(reinterpret_cast<uintptr_t>(src + copied)));
		if (done <= 0) {
			break;
		}
		copied += static_cast<uint64_t>(done);
	}
	return size - copied;
}

} // namespace Common

#endif
