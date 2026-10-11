#pragma once

#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64);
void LowerVertexLaunchMaskPass(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
