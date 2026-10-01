#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void ConstantPropagationPass(const BlockList& blocks);
struct Program;
uint32_t SimplifyBoundedLoopRegisters(Program& program);
uint32_t SimplifyLocalAddressStores(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
