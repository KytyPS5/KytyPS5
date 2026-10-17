#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ConstantPropagationStats {
	// Instructions queued again because a fold changed something they depend on.
	uint64_t revisited_users = 0;
};

ConstantPropagationStats ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64);

} // namespace Libs::Graphics::ShaderRecompiler::IR
