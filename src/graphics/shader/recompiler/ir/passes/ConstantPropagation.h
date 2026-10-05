#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct Program;

void FoldVertexBranchSelects(Program& program);

void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64);

} // namespace Libs::Graphics::ShaderRecompiler::IR
