#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Flag on the SelectU32 that keeps an inactive lane's old value in a VGPR write.
inline constexpr uint64_t VgprWriteMaskFlag = 1;

uint32_t EliminateDeadLaneWriteMasks(Program& program, const std::vector<Inst*>& exec_reads);

uint32_t EliminateUndemandedWriteMasks(Program& program);

// Tests: 0 turns both passes off, -1 restores the default.
void OverrideDeadLaneWriteMasks(int mode);

} // namespace Libs::Graphics::ShaderRecompiler::IR
