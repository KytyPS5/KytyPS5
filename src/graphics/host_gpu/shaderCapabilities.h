#ifndef KYTY_GRAPHICS_SHADER_CAPABILITIES_H_
#define KYTY_GRAPHICS_SHADER_CAPABILITIES_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderHostProfile.h"

namespace Libs::Graphics {

// Pass enabled logical-device features, not merely advertised availability.
inline ShaderRecompiler::ShaderHostProfile QueryShaderHostProfile(
    vk::PhysicalDevice physical_device, bool float64_enabled, bool fma_float64_enabled) {
	vk::PhysicalDeviceFloatControlsProperties controls {};
	vk::PhysicalDeviceProperties2 properties {};
	properties.pNext = &controls;
	physical_device.getProperties2(&properties);
	return {
	    .known = true,
	    .float64 = float64_enabled,
	    .fma_float64 = float64_enabled && fma_float64_enabled,
	    .rte_float64 = controls.shaderRoundingModeRTEFloat64 == VK_TRUE,
	    .rte_float32 = controls.shaderRoundingModeRTEFloat32 == VK_TRUE,
	    .signed_zero_inf_nan_preserve_float64 =
	        controls.shaderSignedZeroInfNanPreserveFloat64 == VK_TRUE,
	};
}

} // namespace Libs::Graphics

#endif
