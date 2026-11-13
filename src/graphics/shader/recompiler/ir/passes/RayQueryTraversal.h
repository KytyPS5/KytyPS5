#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Drives each recognized compute BVH traversal loop (64-bit node pointers, header formats 1 and 3
// instances) with a ray query over the host mirror of its TLAS: every iteration takes one candidate
// triangle into the loop's own triangle test, any-hit and commit code, which keep the hit state.
// Without a valid mirror the loop traverses as before. Returns the loops it drives.
[[nodiscard]] uint32_t TraceTraversalsWithRayQueries(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
