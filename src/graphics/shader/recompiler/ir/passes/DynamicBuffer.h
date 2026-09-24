#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Keep raw scalar buffer reads with GPU-dependent descriptors on the GPU.
uint32_t LowerDynamicBufferReads(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
