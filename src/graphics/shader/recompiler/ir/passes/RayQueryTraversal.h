#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Drives each recognized compute BVH traversal loop over header format 1, 3 or 6 instances with a
// ray query over the host mirror of its TLAS: a flat walk over 64-bit node pointers, a BLAS walk
// nested under an instance leaf of a TLAS walk, or a walk over a stack the wave shares. Every
// iteration takes one candidate triangle into the loop's own triangle test, any-hit and commit
// code, or into copies of a shared-stack walk's instance and commit tests, which keep the hit
// state. Without a valid mirror the loop traverses as before. Returns the loops it drives.
[[nodiscard]] uint32_t TraceTraversalsWithRayQueries(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
