#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

uint32_t FoldUnreachableIndexCompares(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
