#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class ScalarReg : uint16_t {};
enum class VectorReg : uint16_t {};

// Trap temporaries behave as extra scalar registers past the SGPR file.
// RDNA2 (PS5) maps TTMP0-15 to operand codes 108-123; pre-RDNA parts exposed
// only the TTMP0-11 subset at 112-123, which this range also covers.
constexpr uint32_t NumSgprs      = 106;
constexpr uint32_t NumTrapRegs   = 16;
constexpr uint32_t NumScalarRegs = NumSgprs + NumTrapRegs;
constexpr uint32_t NumVectorRegs = 256;

constexpr uint32_t RegIndex(ScalarReg reg) {
	return static_cast<uint32_t>(reg);
}

constexpr uint32_t RegIndex(VectorReg reg) {
	return static_cast<uint32_t>(reg);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
