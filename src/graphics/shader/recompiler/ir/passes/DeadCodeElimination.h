#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct Program;

void RemoveIdentities(Program& program);
void EliminateDeadCode(const BlockList& blocks);
// Replaces selects of a lane's new or old value by the new value where the old one is never
// needed: every consumer of the result is masked by the select's condition again.
// Returns how many it replaced.
uint32_t EliminateMaskedSelects(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
