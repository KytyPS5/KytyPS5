#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Completes the immutable shader interface after resource tracking. On failure Program::info and
// all completion state remain unchanged. barycentric_supported is false on GPUs without
// VK_KHR_fragment_shader_barycentric (e.g. GTX 10-series); pixel params are then compiled for
// hardware interpolation instead of manual barycentric weighting.
void CollectShaderInfo(Program& program, ShaderStageInputInfo input_info,
                       bool barycentric_supported = true);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_ */
