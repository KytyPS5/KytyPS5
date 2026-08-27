#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct SharedMemoryBarrierStats {
	uint32_t inserted_barriers = 0;
};

[[nodiscard]] SharedMemoryBarrierStats
InsertSharedMemoryBarriers(Program& program, uint32_t wave_size, uint32_t threadgroup_size,
                           uint32_t lds_size_dwords, bool needs_barriers);

} // namespace Libs::Graphics::ShaderRecompiler::IR
