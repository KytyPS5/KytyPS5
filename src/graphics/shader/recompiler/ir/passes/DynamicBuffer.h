#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DYNAMICBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DYNAMICBUFFER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Rewrites scalar buffer reads whose V# is built from a loop-carried value into raw
// address loads, so they stop needing a descriptor that cannot exist before the draw.
// Returns how many reads were lowered.
uint32_t LowerDynamicBufferReads(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DYNAMICBUFFER_H_ */
