#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void RewriteToSsa(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
