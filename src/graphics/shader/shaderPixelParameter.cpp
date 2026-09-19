#include "common/assert.h"
#include "graphics/shader/shader.h"

namespace Libs::Graphics {

namespace {

constexpr uint32_t PsInputOffsetMask = 0x0000001fu;
constexpr uint32_t PsInputFlatShade  = 0x00000400u;

} // namespace

uint32_t ShaderPixelParameterMappedLocation(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num ? info.interpolator_settings[input] & PsInputOffsetMask : input;
}

std::vector<uint32_t> ShaderPixelParameterInputs(const ShaderPixelInputInfo& info) {
	std::vector<uint32_t> inputs(info.input_num);
	for (uint32_t input = 0; input < info.input_num; input++) {
		inputs[input] = input;
	}
	return inputs;
}

ShaderPixelParameterPlan ShaderPixelParameterBuildPlan(const ShaderPixelInputInfo& info,
                                                       uint32_t vertex_export_mask) {
	ShaderPixelParameterPlan plan;
	plan.locations.fill(UINT32_MAX);
	if (info.input_num > plan.locations.size()) {
		return plan;
	}
	const auto inputs = ShaderPixelParameterInputs(info);
	for (uint32_t input = 0; input < info.input_num; input++) {
		const auto mapped = ShaderPixelParameterMappedLocation(info, input);
		if (mapped >= plan.locations.size() || (vertex_export_mask & (1u << mapped)) == 0) {
			return plan;
		}
	}
	for (uint32_t input = 0; input < info.input_num; input++) {
		const auto mapped   = ShaderPixelParameterMappedLocation(info, input);
		const auto location = ShaderPixelParameterLocation(info, inputs, input, vertex_export_mask);
		plan.locations[input] = location;
		if (location != mapped && std::find(plan.aliases.begin(), plan.aliases.end(),
		                                    std::pair {location, mapped}) == plan.aliases.end()) {
			plan.aliases.emplace_back(location, mapped);
		}
	}
	plan.valid = true;
	return plan;
}

uint32_t ShaderPixelParameterLocation(const ShaderPixelInputInfo& info,
                                      std::span<const uint32_t> active_inputs, uint32_t input,
                                      uint32_t reserved_mask) {
	std::array<bool, 32> used_locations {};
	for (const auto active_input: active_inputs) {
		used_locations[ShaderPixelParameterMappedLocation(info, active_input)] = true;
	}
	std::array<uint32_t, 64> group_locations;
	group_locations.fill(UINT32_MAX);
	for (const auto active_input: active_inputs) {
		const auto mapped   = ShaderPixelParameterMappedLocation(info, active_input);
		const auto group    = mapped * 2u + ShaderPixelParameterIsFlat(info, active_input);
		auto&      location = group_locations[group];
		if (location == UINT32_MAX) {
			location = mapped;
			// Smooth and custom interpolation read the same vertex output. Only
			// differing flat/smooth rectangle outputs need separate locations.
			if (group_locations[group ^ 1u] != UINT32_MAX) {
				location = 0;
				while (location < used_locations.size() &&
				       (used_locations[location] || (reserved_mask & (1u << location)) != 0)) {
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

} // namespace Libs::Graphics
