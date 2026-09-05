#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct Program;

void ConstantPropagationPass(const BlockList& blocks);
void EliminateMaskedValues(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
