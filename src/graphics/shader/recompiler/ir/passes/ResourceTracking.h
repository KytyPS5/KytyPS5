#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Collects immutable resource topology from typed SSA handles, interns their resolved dwords in
// descriptor_sources, then writes dense indices to handle flags and MemoryInfo.
void TrackResources(Program& program);

// Copies SRT slots, interned descriptor sources, and dense handle/memory indices from a program
// tracked on the original CFG onto a DispatcherFull emit program whose loop phis are not invariant.
void ImportTrackedResources(Program& dest, const Program& src);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
