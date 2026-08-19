#include "graphics/host_gpu/renderer/pipeline/shaderSubgroup.h"

#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

namespace Libs::Graphics {

ShaderSubgroupCapabilities::ShaderSubgroupCapabilities(const GraphicContext& graphics)
    : subgroup_size(graphics.subgroup_size), min_subgroup_size(graphics.min_subgroup_size),
      max_subgroup_size(graphics.max_subgroup_size),
      required_subgroup_size_stages(graphics.required_subgroup_size_stages),
      subgroup_size_control_enabled(graphics.subgroup_size_control_enabled) {}

ShaderLaneMaskMode SelectGraphicsLaneMaskMode(uint32_t guest_wave_size) {
	// A 64-wide guest wave can't assume the host GPU's lane layout matches
	// GCN's. AMD RDNA also reports a 64-wide subgroup but uses a different
	// lane/pixel layout, which corrupted rendering under "native wave" mode.
	// Per-invocation emulation works regardless of the host's lane layout.
	if (guest_wave_size == 64u) {
		return ShaderLaneMaskMode::PerInvocation;
	}
	return ShaderLaneMaskMode::NativeWave;
}

ShaderSubgroupConfiguration ConfigureShaderSubgroup(const ShaderSubgroupCapabilities& capabilities,
                                                    vk::ShaderStageFlagBits           stage,
                                                    const ShaderRecompiler::IR::Program& program) {
	const auto guest_wave_size = program.wave_size;
	if ((guest_wave_size != 32u && guest_wave_size != 64u) ||
	    !program.spirv_requirements.has_value()) {
		return {};
	}
	if (stage != vk::ShaderStageFlagBits::eCompute) {
		const auto expected = SelectGraphicsLaneMaskMode(guest_wave_size);
		if (program.lane_mask_mode != expected || (capabilities.subgroup_size != guest_wave_size &&
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
	if (capabilities.subgroup_size == guest_wave_size) {
		return {ShaderSubgroupMode::Natural, 0};
	}
	if (capabilities.subgroup_size_control_enabled &&
	    (capabilities.required_subgroup_size_stages & stage) &&
	    guest_wave_size >= capabilities.min_subgroup_size &&
	    guest_wave_size <= capabilities.max_subgroup_size) {
		return {ShaderSubgroupMode::Controlled, guest_wave_size};
	}
	if (!program.spirv_requirements->requires_exact_subgroup) {
		return {ShaderSubgroupMode::FlattenedMasks, 0};
	}
	return {};
}

} // namespace Libs::Graphics
