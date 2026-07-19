#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERSUBGROUP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERSUBGROUP_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ShaderIR.h"

namespace Libs::Graphics {

enum class ShaderSubgroupMode {
	Natural,
	Controlled,
	PerInvocationGraphics,
	FlattenedMasks,
	Unsupported
};

struct ShaderSubgroupConfiguration {
	ShaderSubgroupMode mode          = ShaderSubgroupMode::Unsupported;
	uint32_t           required_size = 0;
};

ShaderLaneMaskMode SelectGraphicsLaneMaskMode(const GraphicContext& context,
                                              uint32_t              guest_wave_size);

ShaderSubgroupConfiguration ConfigureShaderSubgroup(const GraphicContext&                context,
                                                    vk::ShaderStageFlagBits              stage,
                                                    const ShaderRecompiler::IR::Program& program);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERSUBGROUP_H_
