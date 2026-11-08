#include "common/assert.h"
#include "graphics/shader/shader.h"

#include <bit>

namespace Libs::Graphics {

namespace {

constexpr uint32_t PsInputOffsetMask = 0x0000001fu;
constexpr uint32_t PsInputFlatShade  = 0x00000400u;

} // namespace

uint32_t ShaderPixelExportTarget(uint32_t shader_mask, uint32_t export_index) {
	for (uint32_t target = 0; target < 8u; target++) {
		if (((shader_mask >> (target * 4u)) & 0xfu) == 0u) {
			continue;
		}
		if (export_index == 0u) {
			return target;
		}
		export_index--;
	}
	return UINT32_MAX;
}

uint32_t ShaderPixelParameterMappedLocation(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num ? info.interpolator_settings[input] & PsInputOffsetMask : input;
}

uint32_t ShaderPixelParameterLocation(const ShaderPixelInputInfo& info,
                                      std::span<const uint32_t> active_inputs, uint32_t input) {
	if (info.parameter_mode != ShaderPixelParameterMode::Rectangle && !info.flat_aliases_copied) {
		return ShaderPixelParameterMappedLocation(info, input);
	}
	std::array<bool, 32> used_locations {};
	for (const auto active_input: active_inputs) {
		used_locations[ShaderPixelParameterMappedLocation(info, active_input)] = true;
	}
	std::array<uint32_t, 64> group_locations;
	group_locations.fill(UINT32_MAX);
	for (const auto active_input: active_inputs) {
		const auto mapped = ShaderPixelParameterMappedLocation(info, active_input);
		const auto group  = mapped * 2u + ShaderPixelParameterIsFlat(info, active_input);
		auto&      location = group_locations[group];
		if (location == UINT32_MAX) {
			location = mapped;
			// Rectangle expansion broadcasts flat aliases through separate outputs.
			if (group_locations[group ^ 1u] != UINT32_MAX) {
				location = 0;
				while (location < used_locations.size() && used_locations[location]) {
					location++;
				}
				EXIT_NOT_IMPLEMENTED(location >= used_locations.size());
			}
			used_locations[location] = true;
		}

		if (active_input == input) {
			return location;
		}
	}
	return ShaderPixelParameterMappedLocation(info, input);
}

std::array<uint8_t, 32> ShaderVertexParameterCopies(const ShaderPixelInputInfo& info,
                                                    std::span<const uint32_t>   active_inputs) {
	std::array<uint8_t, 32> copies {};
	for (const auto input: active_inputs) {
		const auto parameter = ShaderPixelParameterMappedLocation(info, input);
		const auto location  = ShaderPixelParameterLocation(info, active_inputs, input);
		if (location != parameter) {
			copies[parameter] = static_cast<uint8_t>(location + 1u);
		}
	}
	return copies;
}

std::array<uint32_t, 3> ShaderPixelVertexValueLocations(uint32_t used_locations,
                                                        uint32_t per_vertex_locations,
                                                        uint32_t location) {
	EXIT_IF(location >= 32u || (per_vertex_locations & (1u << location)) == 0u);
	// The lowest free locations, three for each input in the order of the inputs' locations.
	uint32_t free = ~used_locations;
	for (uint32_t input = 0;; input++) {
		if ((per_vertex_locations & (1u << input)) == 0u) {
			continue;
		}
		std::array<uint32_t, 3> locations {};
		for (auto& value: locations) {
			EXIT_NOT_IMPLEMENTED(free == 0u);
			value = static_cast<uint32_t>(std::countr_zero(free));
			free &= free - 1u;
		}
		if (input == location) {
			return locations;
		}
	}
}

bool ShaderPixelParameterIsFlat(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num && (info.interpolator_settings[input] & PsInputFlatShade) != 0 &&
	       !ShaderPixelParameterIsCustom(info, input);
}

bool ShaderPixelParameterIsCustom(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < 32u && (info.custom_interpolation_mask & (1u << input)) != 0;
}

} // namespace Libs::Graphics
