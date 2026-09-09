#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Rewrites a sampler S# upper dword (2-3) that only carries a stale wave mask to zero, so SRT
// planning does not choke on a value that is not real descriptor data. Must run before
// BuildSrtPlan.
void CanonicalizeSamplerScratchDwords(Program& program);

// Collects immutable resource topology from typed SSA handles, interns their resolved dwords in
// descriptor_sources, then writes dense indices to handle flags and MemoryInfo.
void TrackResources(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
