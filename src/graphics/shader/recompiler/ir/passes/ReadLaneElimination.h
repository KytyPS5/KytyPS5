#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ReadLaneStats {
	uint32_t rewritten_reads = 0;
};

[[nodiscard]] ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size);

} // namespace Libs::Graphics::ShaderRecompiler::IR
