#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstddef>
#include <cstdint>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
// Completes a faulting load into EAX with `value` and resumes after it.
void CompleteLoad(void* native_context, size_t length, uint32_t value);
// Emulates a GPR load instruction (MOV/MOVZX/MOVSX) from a 16-byte backing buffer.
[[nodiscard]] bool TryEmulateLoadFromBacking(void* native_context, const void* data, size_t size);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
