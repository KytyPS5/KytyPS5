#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstddef>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
// Completes a faulting load into EAX as a zero read and resumes after it.
void SkipLoad(void* native_context, size_t length);

// Why the instruction at `code` cannot be emulated on this host, for the message of an illegal
// instruction that TryEmulate refused. Null when there is no known reason.
[[nodiscard]] const char* UnsupportedReason(const void* code);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
