#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHGUARD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHGUARD_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace Libs::Graphics {

// A count past the device limits is undefined in Vulkan and a driver TDR on Windows.

enum class DispatchPlanAction : uint8_t {
	Record,
	SkipEmpty,
	SkipOverLimit,
};

struct DispatchPlan {
	DispatchPlanAction     action = DispatchPlanAction::Record;
	std::array<uint32_t, 3> groups {};
	uint32_t               failed_axis = 0;
};

[[nodiscard]] constexpr uint32_t GroupsFromThreads(uint32_t threads, uint32_t local_size) noexcept {
	const uint64_t size   = std::max(local_size, 1u);
	const uint64_t groups = (static_cast<uint64_t>(threads) + size - 1u) / size;
	return static_cast<uint32_t>(groups);
}

[[nodiscard]] constexpr DispatchPlan PlanComputeDispatch(std::array<uint32_t, 3> counts,
                                                         std::array<uint32_t, 3> local_size,
                                                         bool thread_dimensions,
                                                         std::array<uint32_t, 3> max_groups) noexcept {
	DispatchPlan plan {};
	for (uint32_t axis = 0; axis < 3; ++axis) {
		plan.groups[axis] =
		    thread_dimensions ? GroupsFromThreads(counts[axis], local_size[axis]) : counts[axis];
	}
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (plan.groups[axis] > max_groups[axis]) {
			plan.action      = DispatchPlanAction::SkipOverLimit;
			plan.failed_axis = axis;
			return plan;
		}
	}
	if (plan.groups[0] == 0 || plan.groups[1] == 0 || plan.groups[2] == 0) {
		plan.action = DispatchPlanAction::SkipEmpty;
	}
	return plan;
}

[[nodiscard]] constexpr bool MeshWorkGroupsWithinLimits(uint32_t groups_x, uint32_t groups_y,
                                                        std::array<uint32_t, 3> max_groups,
                                                        uint32_t max_total) noexcept {
	return groups_x <= max_groups[0] && groups_y <= max_groups[1] && max_groups[2] >= 1u &&
	       static_cast<uint64_t>(groups_x) * groups_y <= max_total;
}

// No Vulkan limit bounds a direct draw, but a count whose vertex or instance ids overflow 32 bits
// cannot be a real submission.
[[nodiscard]] constexpr bool DrawCountsRepresentable(uint32_t count, uint32_t instances,
                                                     uint32_t first_element,
                                                     uint32_t first_instance) noexcept {
	constexpr uint64_t max_ids = std::numeric_limits<uint32_t>::max();
	return static_cast<uint64_t>(count) + first_element <= max_ids + 1u &&
	       static_cast<uint64_t>(instances) + first_instance <= max_ids + 1u &&
	       static_cast<uint64_t>(count) * instances <= max_ids;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHGUARD_H_
