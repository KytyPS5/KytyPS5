#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPUSYNCDIAGNOSTICS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPUSYNCDIAGNOSTICS_H_

#include <array>
#include <cstdint>
#include <limits>

namespace Libs::Graphics {

struct GpuSyncDiagnosticConfig {
	bool                    enabled        = false;
	bool                    valid          = true;
	uint64_t                min_workgroups = 0;
	bool                    exact_enabled  = false;
	std::array<uint32_t, 3> exact_groups   = {};
};

[[nodiscard]] inline uint64_t SaturatingWorkgroupCount(uint32_t x, uint32_t y,
                                                        uint32_t z) noexcept {
	uint64_t count = x;
	for (const auto dimension: {y, z}) {
		if (dimension != 0 && count > std::numeric_limits<uint64_t>::max() / dimension) {
			return std::numeric_limits<uint64_t>::max();
		}
		count *= dimension;
	}
	return count;
}

[[nodiscard]] inline bool GpuSyncDiagnosticMatchesDispatch(
    const GpuSyncDiagnosticConfig& config, uint32_t x, uint32_t y, uint32_t z) noexcept {
	const bool non_empty = x != 0 && y != 0 && z != 0;
	const bool matches_minimum =
	    SaturatingWorkgroupCount(x, y, z) >= config.min_workgroups;
	const bool matches_exact =
	    !config.exact_enabled || std::array<uint32_t, 3> {x, y, z} == config.exact_groups;
	return config.enabled && config.valid && non_empty && matches_minimum && matches_exact;
}

[[nodiscard]] inline bool GpuSyncDiagnosticMatchesDraw(
    const GpuSyncDiagnosticConfig& config, bool non_empty) noexcept {
	// Workgroup-count filters describe compute dispatches. Draws have no guest
	// workgroup tuple, so a non-empty draw remains traceable while diagnostics
	// are enabled, regardless of dispatch-only filters.
	return config.enabled && config.valid && non_empty;
}

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPUSYNCDIAGNOSTICS_H_ */
