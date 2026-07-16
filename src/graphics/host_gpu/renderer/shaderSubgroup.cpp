#include "graphics/host_gpu/renderer/shaderSubgroup.h"

#include "graphics/shader/recompiler/SpirvEmitter.h"

namespace Libs::Graphics {

ShaderLaneMaskMode SelectGraphicsLaneMaskMode(const GraphicContext& context,
                                              uint32_t              guest_wave_size) {
	return context.subgroup_size == 32u && guest_wave_size == 64u
	           ? ShaderLaneMaskMode::PerInvocation
	           : ShaderLaneMaskMode::NativeWave;
}

ShaderSubgroupConfiguration ConfigureShaderSubgroup(const GraphicContext&                context,
                                                    VkShaderStageFlagBits                stage,
                                                    const ShaderRecompiler::IR::Program& program) {
	const auto guest_wave_size = program.wave_size;
	if (guest_wave_size != 32u && guest_wave_size != 64u) {
		return {};
	}
	if (stage != VK_SHADER_STAGE_COMPUTE_BIT) {
		const auto expected = SelectGraphicsLaneMaskMode(context, guest_wave_size);
		if (program.lane_mask_mode != expected || (context.subgroup_size != guest_wave_size &&
		                                           expected != ShaderLaneMaskMode::PerInvocation)) {
			return {};
		}
		return {expected == ShaderLaneMaskMode::NativeWave
		            ? ShaderSubgroupMode::Natural
		            : ShaderSubgroupMode::PerInvocationGraphics,
		        0};
	}
	if (program.lane_mask_mode != ShaderLaneMaskMode::NativeWave) {
		return {};
	}
	if (context.subgroup_size == guest_wave_size) {
		return {ShaderSubgroupMode::Natural, 0};
	}
	if (context.subgroup_size_control_enabled &&
	    (context.required_subgroup_size_stages & stage) != 0 &&
	    guest_wave_size >= context.min_subgroup_size &&
	    guest_wave_size <= context.max_subgroup_size) {
		return {ShaderSubgroupMode::Controlled, guest_wave_size};
	}
	if (!ShaderRecompiler::Spirv::ProgramRequiresExactSubgroupSize(program)) {
		return {ShaderSubgroupMode::FlattenedMasks, 0};
	}
	return {};
}

} // namespace Libs::Graphics
