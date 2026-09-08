#pragma once

#include "graphics/shader/recompiler/ComputeWorkgroup.h"
#include "graphics/shader/shader.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler {
namespace IR { struct Program; }

struct ComputeExecutionPlan {
	ComputeWorkgroupLayout layout;
	// Independent-wave mode partitions a guest group; cooperative mode keeps
	// the complete guest group together and schedules its waves in software.
	uint32_t wave_partition_factor = 1;
	bool split_wave64 = false;
	bool cooperative_wave64 = false;
	// A cyclic external-memory writer in one complete guest wave is safe only
	// when both native32 halves rendezvous between guest memory instructions.
	bool synchronize_split_wave_memory = false;
	std::string error;

	// Both modes split a logical wave across native32 halves and use software collectives.
	[[nodiscard]] bool IsSplitWave64() const { return split_wave64; }
	[[nodiscard]] bool IsCooperativeWave64() const { return cooperative_wave64; }
	[[nodiscard]] bool SynchronizesSplitWaveMemory() const {
		return synchronize_split_wave_memory;
	}
};

// LLVM's GDS pointer lowering folds a byte displacement into DS_APPEND's
// 16-bit immediate. Keep the split-wave extension to whole DWORD counters;
// the runtime M0 and backing bounds remain separate from this shape check.
[[nodiscard]] constexpr bool IsSupportedWave64GdsAppendOffset(uint32_t offset) {
	return offset <= 0xfffcu && (offset & 3u) == 0u;
}

// Live shared accesses, not unused LDS reservations. Emission uses the same
// predicate so split-wave scratch and guest LDS occupy one Workgroup array.
[[nodiscard]] bool HasGuestLdsAccess(const IR::Program& program);

// Pure, deterministic and shared by emission and the runtime pipeline cache.
// A nonempty error forbids emission/dispatch; offline width zero remains native.
[[nodiscard]] ComputeExecutionPlan PlanComputeExecution(
    const IR::Program& program, ShaderStageInputInfo input_info,
    const ComputeWorkgroupLimits& limits);

// Normalize the dispatch dimensions before applying any host wave partition.
[[nodiscard]] inline std::array<uint32_t, 3> ComputeGuestWorkgroups(
    std::array<uint32_t, 3> raw_dimensions,
    const std::array<uint32_t, 3>& local_sizes, bool use_thread_dimensions) {
	if (!use_thread_dimensions) return raw_dimensions;
	for (uint32_t axis = 0; axis < 3; ++axis) {
		const uint32_t group_size = std::max(local_sizes[axis], 1u);
		// Quotient + remainder avoids overflow near UINT32_MAX.
		raw_dimensions[axis] = raw_dimensions[axis] / group_size +
		                       (raw_dimensions[axis] % group_size != 0u ? 1u : 0u);
	}
	return raw_dimensions;
}

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
