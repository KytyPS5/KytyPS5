#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DYNAMICBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DYNAMICBUFFER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct DynamicReadStats {
	uint32_t const_buffer_reads = 0; // V# assembled from a loop-carried value
	uint32_t address_reads     = 0; // load address assembled from a lane-varying value
};

// Rewrites scalar buffer reads whose V# is built from a loop-carried value into raw
// address loads, so they stop needing a descriptor that cannot exist before the draw.
// Returns how many reads were lowered, per shape.
DynamicReadStats LowerDynamicBufferReads(Program& program);

// True when the value is built from a phi that ResolveInvariantPhi cannot collapse, i.e. the value
// differs between the paths that reach it and no descriptor can describe it before the dispatch.
// Unlike ValidateRuntimeValue this does not need a finished srt_reads list, so it is also usable
// from the passes that run before resource tracking.
bool AddressReachesDynamicPhi(const Program& program, Value value);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DYNAMICBUFFER_H_ */
