#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct SharedMemoryBarrierStats {
	uint32_t inserted_barriers = 0;
};

[[nodiscard]] SharedMemoryBarrierStats
InsertSharedMemoryBarriers(Program& program, uint32_t wave_size,
	                       const ShaderComputeInputInfo& compute_info);

} // namespace Libs::Graphics::ShaderRecompiler::IR
