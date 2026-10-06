#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstdint>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
struct WideStoreSplitResult {
	uint64_t candidates = 0;  // plain 256-bit vector stores found
	uint64_t patched    = 0;  // rewritten into two 128-bit stores
	uint64_t too_short   = 0;  // shorter than a rel32 jump and no int3 padding within rel8 reach
	uint64_t via_cave    = 0;  // short stores patched with a rel8 jump through int3 padding
	uint64_t trapped          = 0;  // short stores patched to ud2, redirected by the SIGILL handler
	uint64_t relocated        = 0; // hot short stores patched by moving the next instructions along
	uint64_t unsupported = 0; // segment override, encoder failure or trampoline area full
	uint64_t trampoline_bytes = 0;
};

// Rosetta aborts the process when a 256-bit store that crosses into a write-protected page
// faults after writing its first half. 128-bit stores fault cleanly. Rewrite each plain
// 256-bit vector store in [address, address + size) into a jump to a trampoline that performs
// the store as two 128-bit halves and jumps back. Trampolines are written from *cursor up to
// trampoline_end, which must be within +/-2 GiB of the code.
WideStoreSplitResult SplitWideStores(uint64_t address, uint64_t size, uint64_t* cursor,
                                     uint64_t trampoline_end);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
