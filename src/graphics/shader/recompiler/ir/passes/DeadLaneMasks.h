#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Flag on the SelectU32 that keeps an inactive lane's old value in a VGPR write.
inline constexpr uint64_t VgprWriteMaskFlag = 1;

uint32_t EliminateDeadLaneWriteMasks(Program& program, const std::vector<Inst*>& exec_reads);

uint32_t EliminateUndemandedWriteMasks(Program& program);

// KYTY_NO_DEAD_LANE_MASKS=1 turns the pass off.
[[nodiscard]] bool DeadLaneWriteMasksEnabled();
// Tests: 0 off, 1 on, -1 back to the environment.
void OverrideDeadLaneWriteMasks(int mode);

} // namespace Libs::Graphics::ShaderRecompiler::IR
