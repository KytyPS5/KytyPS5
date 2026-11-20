#ifndef KYTY_LOADER_DEMONS_SOULS_IDLE_H_
#define KYTY_LOADER_DEMONS_SOULS_IDLE_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(__x86_64__) || defined(_M_X64)
#include <xbyak/xbyak.h>
#endif

// [HACK] Demon's Souls' job workers wait for work by polling the job queues in a loop that
// pauses, reads the TSC and goes round again for two seconds of idleness before blocking. Work
// comes every frame, so a dozen workers spin at ~95% of a CPU all through play. Install() routes
// the idle loop's poll and the wait's TSC read through thunks that sleep a little first.
namespace Loader {
struct Program;
namespace DemonsSoulsIdle {

// For the game's main program, before its module initializers or threads run.
void Install(Program* program);

// The job workers' idle loop as every build seen has it (01.005.000 and 01.007.000): the call of
// the poll for work, the test of its result and the loop back, the call's displacement left out
// (IdleCallAt: the call's offset in it); and the poll's prologue, its frame size left out
// (01.005.000 0x98 bytes, 01.007.000 0x88).
inline constexpr std::string_view IdleCall =
    "48 89 df 4c 89 f6 48 c7 45 c8 00 00 00 00 e8 ?? ?? ?? ?? 84 c0 74 d7 "
    "4c 8b 7d c8 49 8b b7 90 00 00 00 48 85 f6 74";
inline constexpr size_t           IdleCallAt = 14;
inline constexpr std::string_view PollPrologue =
    "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec ?? ?? ?? ?? 4c 8b 35";

// The wait for work the idle loop calls before its poll: it scans the job queues, then `pause`,
// reads the TSC (a helper call) and goes round again until the worker has been idle for two
// seconds before it blocks. The pause, the load of the worker's last activity time, the helper
// call (SpinCallAt: the call's offset), the elapsed-time test. The helper (rdtsc; shl rdx, 32;
// or rax, rdx; ret) may already go through the guest instruction patches that give the guest's
// TSC rate, so the thunk calls it as it is.
inline constexpr std::string_view SpinCall =
    "f3 90 4e 8b bc f3 10 0e 00 00 e8 ?? ?? ?? ?? 4c 29 f8 48 85 c0 0f 8e";
inline constexpr size_t SpinCallAt = 10;

#if defined(__x86_64__) || defined(_M_X64)
// Saves every register the guest may expect kept (all but RSP's callee-saved ones, which the host
// call keeps) and the x87/SSE state around a SysV call of `function`.
inline void EmitSavedCall(Xbyak::CodeGenerator& c, const void* function) {
	using namespace Xbyak::util;
	c.pushfq();
	for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11}) {
		c.push(reg);
	}
	// Entry RSP is 8 mod 16; the 80-byte save keeps that, the 520 bytes make it 0 for FXSAVE and
	// the call.
	c.sub(rsp, 520);
	constexpr uint8_t SaveFp[] {0x48, 0x0f, 0xae, 0x04, 0x24}; // FXSAVE64 [rsp]
	c.db(SaveFp, sizeof(SaveFp));
	c.mov(rax, reinterpret_cast<uint64_t>(function));
	c.call(rax);
	c.fxrstor64(ptr[rsp]);
	c.add(rsp, 520);
	for (const auto& reg: {r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax}) {
		c.pop(reg);
	}
	c.popfq();
}

// The idle loop's poll: the poll itself, then, when it found no work, the sleep. The poll's
// observable register state is kept, also on the idle path: its caller may rely on registers the
// poll was known not to change.
inline void EmitPollThunk(Xbyak::CodeGenerator& c, const void* poll, const void* sleep) {
	using namespace Xbyak::util;
	Xbyak::Label done;
	c.sub(rsp, 8);
	c.call(poll);
	c.add(rsp, 8);
	c.test(al, al);
	c.jnz(done, Xbyak::CodeGenerator::T_NEAR);
	EmitSavedCall(c, sleep);
	c.L(done);
	c.ret();
}

// The TSC helper's stand-in in the wait for work: the sleep, then the helper itself (a tail jump:
// it returns to the wait).
inline void EmitSpinThunk(Xbyak::CodeGenerator& c, const void* sleep, const void* helper) {
	using namespace Xbyak::util;
	EmitSavedCall(c, sleep);
	c.mov(rax, reinterpret_cast<uint64_t>(helper));
	c.jmp(rax);
}
#endif

} // namespace DemonsSoulsIdle
} // namespace Loader

#endif /* KYTY_LOADER_DEMONS_SOULS_IDLE_H_ */
