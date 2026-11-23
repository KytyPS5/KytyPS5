#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64);
void SimplifyActiveLaneSelects(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
