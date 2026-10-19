#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class ScalarReg : uint16_t {};
enum class VectorReg : uint16_t {};

// Trap temporaries (TTMP0-11, operand codes 112-123) behave as extra scalar registers.
// They are decoded into the slots right after the SGPRs.
constexpr uint32_t NumSgprs      = 106;
constexpr uint32_t NumTrapRegs   = 12;
constexpr uint32_t NumScalarRegs = NumSgprs + NumTrapRegs;
constexpr uint32_t NumVectorRegs = 256;

constexpr uint32_t RegIndex(ScalarReg reg) {
	return static_cast<uint32_t>(reg);
}

constexpr uint32_t RegIndex(VectorReg reg) {
	return static_cast<uint32_t>(reg);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
