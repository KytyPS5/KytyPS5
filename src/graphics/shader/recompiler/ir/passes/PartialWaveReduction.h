#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct PartialWaveReductionStats {
	uint32_t rewritten_reads = 0;
};

// KYTY_PARTIAL_WAVE_REDUCTION=1 (cached getenv).
[[nodiscard]] bool PartialWaveReductionEnabled();

// RDNA compilers emit wave-wide min/max/or/and reductions as: whole-wave EXEC, a neutral-element
// fill of the lanes that were inactive, four DPP row_shr steps, one V_PERMLANEX16, and finally
// V_READLANE of lane 31 (and 63). Real hardware owns all 32/64 lanes, so lanes that were never
// launched hold the neutral element. Host pixel waves only contain launched invocations, so a
// shuffle from the missing lane 31/63 (or a DPP/permlane source that does not exist) returns
// garbage or zero. This pass proves the pattern and replaces the lane read with a subgroup
// reduction over the invocations that exist, which is identical because missing lanes would have
// contributed the neutral element. Arbitrary V_READLANE is left alone.
[[nodiscard]] PartialWaveReductionStats LowerPartialWaveReductions(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
