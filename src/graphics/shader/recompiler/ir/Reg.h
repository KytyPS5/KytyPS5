#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class ScalarReg : uint16_t {};
enum class VectorReg : uint16_t {};

// Architectural SGPRs occupy the low range. The current shader-only migration also accepts the
// scalar shadow registers produced by Kyty's pre-existing read-lane pass; that pass disappears
// once read-lane elimination operates on the SSA graph.
constexpr uint32_t NumScalarRegs = 512;
constexpr uint32_t NumVectorRegs = 256;

constexpr uint32_t RegIndex(ScalarReg reg) {
	return static_cast<uint32_t>(reg);
}

constexpr uint32_t RegIndex(VectorReg reg) {
	return static_cast<uint32_t>(reg);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
