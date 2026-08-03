#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERSUBGROUP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERSUBGROUP_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

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

struct ShaderSubgroupCapabilities {
	explicit ShaderSubgroupCapabilities(const GraphicContext& graphics);

	uint32_t             subgroup_size     = 0;
	uint32_t             min_subgroup_size = 0;
	uint32_t             max_subgroup_size = 0;
	vk::ShaderStageFlags required_subgroup_size_stages;
	bool                 subgroup_size_control_enabled = false;
};

ShaderLaneMaskMode SelectGraphicsLaneMaskMode(uint32_t guest_wave_size);

ShaderSubgroupConfiguration ConfigureShaderSubgroup(const ShaderSubgroupCapabilities& capabilities,
                                                    vk::ShaderStageFlagBits           stage,
                                                    const ShaderRecompiler::IR::Program& program);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERSUBGROUP_H_
