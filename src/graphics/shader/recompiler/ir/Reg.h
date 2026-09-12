#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class ScalarReg : uint16_t {};
enum class VectorReg : uint16_t {};

// 0-105 are the SGPRs, 108-123 are TTMP0-15 (trap temporaries - SpongeBob
// PPSA26893 shaders read them). 106-107 (VCC) and 124-127 (M0/EXEC) live in
// separate state, so the index space is sparse on purpose.
constexpr uint32_t NumScalarRegs = 124;
constexpr uint32_t NumVectorRegs = 256;

constexpr uint32_t RegIndex(ScalarReg reg) {
	return static_cast<uint32_t>(reg);
}

constexpr uint32_t RegIndex(VectorReg reg) {
	return static_cast<uint32_t>(reg);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
