#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_ADDRESSRANGES_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_ADDRESSRANGES_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Fills info.address_ranges with the guest ranges device-address reads provably stay in, and
// info.export_bounds with bounds on what a vertex stage exports to parameters.
void CollectAddressRanges(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_ADDRESSRANGES_H_ */
