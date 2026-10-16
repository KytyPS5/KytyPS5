#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Replaces a connected web of phis whose every non-phi input is the same value by that value.
// Returns the number of phis replaced.
uint32_t SimplifyPhiWebs(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
