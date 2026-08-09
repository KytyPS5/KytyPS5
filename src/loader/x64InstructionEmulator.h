#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstddef>
#include <cstdint>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);

// Soft-continue guest AVs on poisoned / non-canonical addresses (e.g. Read[ffffffffffffffff]):
// skip the faulting memory op, zero GP/XMM destination on reads, advance RIP.
[[nodiscard]] bool TrySoftContinuePoisonAccess(void* native_context, uint64_t fault_vaddr,
                                               bool is_write, bool force = false,
                                               bool allow_system_module = false);

// Usermode INT/UD2 traps are commonly surfaced by Windows as AV Read[-1].
// Classify them without treating the trap as a poison memory operation.
[[nodiscard]] bool DescribeGuestAbortTrap(uint64_t rip, char* detail, size_t detail_size);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
