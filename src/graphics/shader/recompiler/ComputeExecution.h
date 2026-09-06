#pragma once

#include "graphics/shader/recompiler/ComputeWorkgroup.h"
#include "graphics/shader/shader.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler {
namespace IR { struct Program; }

struct ComputeExecutionPlan {
	ComputeWorkgroupLayout layout;
	// Split mode dispatches one host workgroup for each complete guest wave64.
	uint32_t wave_partition_factor = 1;
	bool split_wave64 = false;
	std::string error;

	[[nodiscard]] bool IsSplitWave64() const { return split_wave64; }
};

// Pure, deterministic and shared by emission and the runtime pipeline cache.
// A nonempty error forbids emission/dispatch; offline width zero remains native.
[[nodiscard]] ComputeExecutionPlan PlanComputeExecution(
    const IR::Program& program, ShaderStageInputInfo input_info,
    const ComputeWorkgroupLimits& limits);

// All axes are checked even though splitting expands X only. Zero work is valid.
[[nodiscard]] inline std::optional<std::array<uint32_t, 3>> PlanComputeDispatchGroups(
    std::array<uint32_t, 3> guest_groups, uint32_t wave_partition_factor,
    const std::array<uint32_t, 3>& max_group_counts) {
	if (wave_partition_factor == 0) return std::nullopt;
	const uint64_t expanded_x = uint64_t(guest_groups[0]) * wave_partition_factor;
	if (expanded_x > UINT32_MAX) return std::nullopt;
	guest_groups[0] = static_cast<uint32_t>(expanded_x);
	for (uint32_t axis = 0; axis < 3; ++axis)
		if (guest_groups[axis] > max_group_counts[axis]) return std::nullopt;
	return guest_groups;
}

} // namespace Libs::Graphics::ShaderRecompiler
