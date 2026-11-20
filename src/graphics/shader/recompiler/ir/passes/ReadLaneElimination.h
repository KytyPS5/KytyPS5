#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ReadLaneStats {
	uint32_t rewritten_reads      = 0;
	uint32_t rewritten_reductions = 0;
};

// Before SSA: a VGPR that only v_writelane writes holds spilled SGPRs, and a lane the shader has
// not written is undefined, not zero. Its initial value becomes an UndefU32 the read-lane
// elimination may take as any value, so a lane read on a path that skipped the spill (a
// structured exit, for one) still resolves to the spilled value. The returned values are
// released to zero once the reads are eliminated.
[[nodiscard]] std::vector<Inst*> MarkUndefinedLaneSpills(Program& program);
void ReleaseUndefinedLaneSpills(const std::vector<Inst*>& undefined);

[[nodiscard]] ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size);

} // namespace Libs::Graphics::ShaderRecompiler::IR
