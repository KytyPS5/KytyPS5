#include "graphics/host_gpu/renderer/shaderSubgroup.h"

#include "graphics/shader/recompiler/SpirvEmitter.h"

namespace Libs::Graphics {

ShaderLaneMaskMode SelectGraphicsLaneMaskMode(const GraphicContext& context,
                                              uint32_t              guest_wave_size) {
	// A 64-wide guest wave can't assume the host GPU's lane layout matches
	// GCN's. AMD RDNA also reports a 64-wide subgroup but uses a different
	// lane/pixel layout, which corrupted rendering under "native wave" mode.
	// Per-invocation emulation works regardless of the host's lane layout.
	if (guest_wave_size == 64u) {
		return ShaderLaneMaskMode::PerInvocation;
	}
	return ShaderLaneMaskMode::NativeWave;
}

ShaderSubgroupConfiguration ConfigureShaderSubgroup(const GraphicContext&                context,
                                                    vk::ShaderStageFlagBits              stage,
                                                    const ShaderRecompiler::IR::Program& program) {
	const auto guest_wave_size = program.wave_size;
	if (guest_wave_size != 32u && guest_wave_size != 64u) {
		return {};
	}
	if (stage != vk::ShaderStageFlagBits::eCompute) {
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
	if (context.subgroup_size_control_enabled && (context.required_subgroup_size_stages & stage) &&
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
