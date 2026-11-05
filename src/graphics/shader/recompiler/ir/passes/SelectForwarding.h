#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Within lane-local arithmetic whose result is only kept by Select(c, result, old), an
// operand Select(c, a, b) can read a directly: lanes where c is false discard the result.
void ForwardGuardedSelects(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
