#include "graphics/shader/shader.h"

#include "common/assert.h"

namespace Libs::Graphics {

namespace {

constexpr uint32_t PsInputOffsetMask = 0x0000001fu;
constexpr uint32_t PsInputFlatShade  = 0x00000400u;

} // namespace

uint32_t ShaderPixelParameterMappedLocation(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num ? info.interpolator_settings[input] & PsInputOffsetMask : input;
}

uint32_t ShaderPixelParameterLocation(const ShaderPixelInputInfo& info,
                                      std::span<const uint32_t> active_inputs, uint32_t input) {
	std::array<bool, 32> used_locations {};
	for (const auto active_input: active_inputs) {
		auto location = ShaderPixelParameterMappedLocation(info, active_input);
		if (location < used_locations.size() && used_locations[location]) {
			location = active_input;
			while (location < used_locations.size() && used_locations[location]) {
				location++;
			}
			EXIT_NOT_IMPLEMENTED(location >= used_locations.size());
		}

		if (active_input == input) {
			return location;
		}
		used_locations[location] = true;
	}
	return ShaderPixelParameterMappedLocation(info, input);
}

bool ShaderPixelParameterIsFlat(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num && (info.interpolator_settings[input] & PsInputFlatShade) != 0 &&
	       !ShaderPixelParameterIsCustom(info, input);
}

bool ShaderPixelParameterIsCustom(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < 32u && (info.custom_interpolation_mask & (1u << input)) != 0;
}

} // namespace Libs::Graphics
