#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ShaderInfoOptions {
	const ShaderVertexInputInfo*  vertex  = nullptr;
	const ShaderPixelInputInfo*   pixel   = nullptr;
	const ShaderComputeInputInfo* compute = nullptr;
	// False on GPUs without VK_KHR_fragment_shader_barycentric (e.g. GTX
	// 10-series). Pixel params are then compiled for hardware interpolation
	// instead of manual barycentric weighting.
	bool barycentric_supported = true;
};

// Completes the immutable shader interface after resource tracking. On failure Program::info and
// all completion state remain unchanged.
void CollectShaderInfo(Program& program, const ShaderInfoOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_ */
