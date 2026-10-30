#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ReadLaneStats {
	uint32_t rewritten_reads      = 0;
	uint32_t rewritten_reductions = 0;
};

[[nodiscard]] ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size);

// A stage that runs one guest lane per host invocation, on a host that has no subgroup operations
// in that stage. The guest wave is modelled as lanes that all hold the same values, as the lane
// masks of such a stage already are (EXEC is all ones): READFIRSTLANE and READLANE return their
// operand, the lane id is 0, and a ballot is the predicate in every bit. The loop that indexes
// registers by a per-lane value (V_READFIRSTLANE, V_CMPX_EQ, V_MOVRELS) comes out right: its one
// pass finds the lane equal to itself and clears the mask. A scalar that V_WRITELANE put into one
// lane of a register is the exception to "all lanes the same": V_READLANE of that lane gets it
// back, also past masked writes and joining paths. Other lane operations are left alone.
// Returns the number of operations replaced.
[[nodiscard]] uint32_t LowerLaneOpsToSingleLane(Program& program);

// A wave-wide reduction that the guest compiler builds from lane operations (WaveActiveMin and
// the like): DPP row shifts, V_PERMLANEX16 and V_READLANE of the lanes that end up holding the
// result. On the hardware every lane of the wave exists, also those a fragment wave has no pixel
// for, and they hold partial results. The host has no invocation for such a lane, so the lane by
// lane form reads nothing there. A value that combines every lane of one operand, whose lanes
// outside EXEC hold the operation's identity, becomes one subgroup reduction of that operand.
// Returns the number of reductions found.
[[nodiscard]] uint32_t RecognizeWaveReductions(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
