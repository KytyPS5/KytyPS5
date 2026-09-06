#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_COMPUTEWORKGROUP_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_COMPUTEWORKGROUP_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

struct ComputeWorkgroupLimits {
	// Offline compilation preserves the guest geometry unless a device is supplied.
	std::array<uint32_t, 3> max_size        = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
	uint32_t                max_invocations = UINT32_MAX;
	// Zero preserves offline compilation without assuming a native subgroup width.
	uint32_t                native_subgroup_size = 0;
	bool                    can_require_subgroup_size_64 = false;
};

struct ComputeWorkgroupLayout {
	std::array<uint32_t, 3> guest_size = {1, 1, 1};
	std::array<uint32_t, 3> host_size  = {1, 1, 1};

	[[nodiscard]] bool IsReshaped() const { return guest_size != host_size; }
};

// Dimensions are nonzero invocation counts, after applying the stage's defaults.
// Each guest workgroup remains one host workgroup with exactly the same invocations.
[[nodiscard]] inline std::optional<ComputeWorkgroupLayout>
PlanComputeWorkgroup(std::array<uint32_t, 3> guest_size, const ComputeWorkgroupLimits& limits) {
	uint32_t count = 1;
	bool     fits  = true;
	for (uint32_t axis = 0; axis < 3; axis++) {
		if (guest_size[axis] == 0 || limits.max_size[axis] == 0 ||
		    count > limits.max_invocations / guest_size[axis]) {
			return std::nullopt;
		}
		count *= guest_size[axis];
		fits &= guest_size[axis] <= limits.max_size[axis];
	}
	if (fits) {
		return ComputeWorkgroupLayout {guest_size, guest_size};
	}

	// Prefer X, then Y, keeping the choice deterministic for this device's cache.
	// Enumerating divisors also handles shapes that cannot be flattened into X alone.
	std::vector<uint32_t> divisors;
	for (uint32_t value = 1; value <= count / value; value++) {
		if (count % value == 0) {
			divisors.push_back(value);
			if (value != count / value) {
				divisors.push_back(count / value);
			}
		}
	}
	std::sort(divisors.begin(), divisors.end(), [](uint32_t a, uint32_t b) { return a > b; });
	for (const auto x: divisors) {
		if (x > limits.max_size[0]) {
			continue;
		}
		const auto yz = count / x;
		for (const auto y: divisors) {
			if (y > limits.max_size[1] || y > yz || yz % y != 0) {
				continue;
			}
			const auto z = yz / y;
			if (z <= limits.max_size[2]) {
				return ComputeWorkgroupLayout {guest_size, {x, y, z}};
			}
		}
	}
	return std::nullopt;
}

} // namespace Libs::Graphics::ShaderRecompiler

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_COMPUTEWORKGROUP_H_ */
