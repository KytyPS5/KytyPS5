#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Why a guest shader could not be tracked. Structural rejections describe the guest program, not
// a broken emulator invariant, so callers drop the draw instead of terminating the process.
struct ResourceTrackingStatus {
	bool        ok = true;
	uint32_t    pc = 0;
	std::string reason;
};

// Collects immutable resource topology from typed SSA handles, interns their resolved dwords in
// descriptor_sources, then writes dense indices to handle flags and MemoryInfo. Leaves the program
// unmodified and reports the reason when the guest shader cannot be modelled.
ResourceTrackingStatus TrackResources(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
