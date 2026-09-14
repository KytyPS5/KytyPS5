#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class ScalarReg : uint16_t {};
enum class VectorReg : uint16_t {};

constexpr uint32_t NumSgprRegs   = 106;
// TTMP0-15 are not architected SGPRs, but shaders read them like scalar registers. They live
// right above the SGPR file so every scalar-register pass covers them unchanged.
constexpr uint32_t NumTtmpRegs   = 16;
constexpr uint32_t NumScalarRegs = NumSgprRegs + NumTtmpRegs;
constexpr uint32_t NumVectorRegs = 256;

constexpr ScalarReg TtmpReg(uint32_t index) {
	return static_cast<ScalarReg>(NumSgprRegs + index);
}

constexpr uint32_t RegIndex(ScalarReg reg) {
	return static_cast<uint32_t>(reg);
}

constexpr uint32_t RegIndex(VectorReg reg) {
	return static_cast<uint32_t>(reg);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
