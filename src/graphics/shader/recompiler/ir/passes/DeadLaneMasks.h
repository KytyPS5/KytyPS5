#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Flag on the SelectU32 that keeps an inactive lane's old value in a VGPR write.
inline constexpr uint64_t VgprWriteMaskFlag = 1;

// Drops the VGPR write masks of a compute program whose inactive lanes no lane ever reads.
uint32_t EliminateUndemandedWriteMasks(Program& program);

// Tests: 0 turns the pass off, -1 restores the default.
void OverrideDeadLaneWriteMasks(int mode);

} // namespace Libs::Graphics::ShaderRecompiler::IR
