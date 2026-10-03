#include "graphics/shader/shader.h"

#include "common/assert.h"

#include <algorithm>

namespace Libs::Graphics {

namespace {

constexpr uint32_t PsInputOffsetMask = 0x0000001fu;
constexpr uint32_t PsInputFlatShade  = 0x00000400u;
constexpr uint32_t PsInputOffsetHigh      = 0x00000020u;
constexpr uint32_t PsInputDefaultValShift = 8u;
constexpr uint32_t PsInputFp16InterpMode  = 0x00080000u;

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
	if (info.parameter_mode != ShaderPixelParameterMode::Rectangle) {
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
		const auto group =
		    mapped * 2u + ShaderPixelParameterGroupIsFlat(info, active_inputs, active_input);
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

bool ShaderPixelParameterIsFlat(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num && (info.interpolator_settings[input] & PsInputFlatShade) != 0 &&
	       !ShaderPixelParameterIsCustom(info, input);
}

bool ShaderPixelParameterIsCustom(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < 32u && (info.custom_interpolation_mask & (1u << input)) != 0;
}

bool ShaderPixelParameterIsDefault(const ShaderPixelInputInfo& info, uint32_t input) {
	if (input >= info.input_num || ShaderPixelParameterIsCustom(info, input)) {
		return false;
	}
	const auto settings = info.interpolator_settings[input];
	return (settings & PsInputOffsetHigh) != 0 && (settings & PsInputFlatShade) == 0 &&
	       (settings & PsInputFp16InterpMode) == 0;
}

uint32_t ShaderPixelParameterDefaultComponent(const ShaderPixelInputInfo& info, uint32_t input,
                                              uint32_t component) {
	// DEFAULT_VAL: 0 = (0,0,0,0), 1 = (0,0,0,1), 2 = (1,1,1,0), 3 = (1,1,1,1).
	const auto value = (info.interpolator_settings[input] >> PsInputDefaultValShift) & 0x3u;
	const bool one   = (component & 3u) == 3u ? (value & 1u) != 0 : (value & 2u) != 0;
	return one ? 0x3f800000u : 0u;
}

void ShaderPixelParameterDuplicates(const ShaderPixelInputInfo& info,
                                    std::span<const uint32_t> active_inputs, uint32_t& mask,
                                    uint8_t (&source)[32]) {
	mask = 0;
	std::fill(std::begin(source), std::end(source), uint8_t {0});
	for (const auto input: active_inputs) {
		const auto mapped   = ShaderPixelParameterMappedLocation(info, input);
		const auto location = ShaderPixelParameterLocation(info, active_inputs, input);
		if (location != mapped && location < 32u) {
			mask |= 1u << location;
			source[location] = static_cast<uint8_t>(mapped);
		}
	}
}

bool ShaderPixelParameterGroupIsFlat(const ShaderPixelInputInfo& info,
                                     std::span<const uint32_t> active_inputs, uint32_t input) {
	if (!ShaderPixelParameterIsFlat(info, input)) {
		return false;
	}
	const auto mapped = ShaderPixelParameterMappedLocation(info, input);
	return std::ranges::none_of(active_inputs, [&](uint32_t other) {
		return ShaderPixelParameterIsCustom(info, other) &&
		       ShaderPixelParameterMappedLocation(info, other) == mapped;
	});
}

} // namespace Libs::Graphics
