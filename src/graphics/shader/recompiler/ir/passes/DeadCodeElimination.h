#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void RemoveIdentities(const BlockList& blocks);
void EliminateDeadCode(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
