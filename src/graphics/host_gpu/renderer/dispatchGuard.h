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
	SkipUnrepresentable,
};

struct DispatchPlan {
	DispatchPlanAction      action = DispatchPlanAction::Record;
	std::array<uint32_t, 3> groups {};
};

[[nodiscard]] constexpr uint32_t GroupsFromThreads(uint32_t threads, uint32_t local_size) noexcept {
	const uint64_t size   = std::max(local_size, 1u);
	const uint64_t groups = (static_cast<uint64_t>(threads) + size - 1u) / size;
	return static_cast<uint32_t>(groups);
}

[[nodiscard]] constexpr DispatchPlan
PlanComputeDispatch(std::array<uint32_t, 3> counts, std::array<uint32_t, 3> local_size,
                    bool thread_dimensions, std::array<uint32_t, 3> max_groups) noexcept {
	DispatchPlan plan {};
	for (uint32_t axis = 0; axis < 3; ++axis) {
		plan.groups[axis] =
		    thread_dimensions ? GroupsFromThreads(counts[axis], local_size[axis]) : counts[axis];
	}
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (plan.groups[axis] > max_groups[axis]) {
			plan.action = DispatchPlanAction::SkipOverLimit;
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

// A count whose vertex or instance ids overflow 32 bits cannot be a real submission.
[[nodiscard]] constexpr bool DrawCountsRepresentable(uint32_t count, uint32_t instances,
                                                     uint32_t first_element,
                                                     uint32_t first_instance) noexcept {
	constexpr uint64_t max_ids = std::numeric_limits<uint32_t>::max();
	return static_cast<uint64_t>(count) + first_element <= max_ids + 1u &&
	       static_cast<uint64_t>(instances) + first_instance <= max_ids + 1u &&
	       static_cast<uint64_t>(count) * instances <= max_ids;
}

[[nodiscard]] constexpr uint32_t MeshInputPrimitiveCount(uint32_t vertices, uint32_t size,
                                                         uint32_t step) noexcept {
	return vertices < size ? 0u : (vertices - size) / step + 1u;
}

struct MeshDrawShape {
	uint32_t                primitive_size       = 1;
	uint32_t                primitive_step       = 1;
	uint32_t                primitives_per_group = 1;
	std::array<uint32_t, 3> max_groups {};
	uint32_t                max_total = 0;
};

struct MeshDrawPlan {
	DispatchPlanAction action     = DispatchPlanAction::Record;
	uint32_t           primitives = 0;
	uint32_t           groups_x   = 0;
	uint32_t           groups_y   = 0;
};

[[nodiscard]] constexpr MeshDrawPlan PlanMeshDraw(const MeshDrawShape& shape, uint32_t count,
                                                  uint32_t instances) noexcept {
	MeshDrawPlan plan {};
	plan.primitives = MeshInputPrimitiveCount(count, shape.primitive_size, shape.primitive_step);
	if (plan.primitives == 0 || instances == 0) {
		plan.action = DispatchPlanAction::SkipEmpty;
		return plan;
	}
	plan.groups_x = (plan.primitives - 1u) / shape.primitives_per_group + 1u;
	plan.groups_y = instances;
	if (!MeshWorkGroupsWithinLimits(plan.groups_x, plan.groups_y, shape.max_groups,
	                                shape.max_total)) {
		plan.action = DispatchPlanAction::SkipOverLimit;
	}
	return plan;
}

struct MeshIndirectRecord {
	uint32_t count          = 0;
	uint32_t instances      = 0;
	uint32_t first_vertex   = 0;
	uint32_t first_instance = 0;
};

// What the CPU replay does with one non-indexed record; the task shader derives it the same way.
[[nodiscard]] constexpr MeshDrawPlan
DeriveMeshIndirectRecord(const MeshDrawShape& shape, const MeshIndirectRecord& record) noexcept {
	if (record.count == 0 || record.instances == 0) {
		return {.action = DispatchPlanAction::SkipEmpty};
	}
	if (!DrawCountsRepresentable(record.count, record.instances, record.first_vertex,
	                             record.first_instance)) {
		return {.action = DispatchPlanAction::SkipUnrepresentable};
	}
	return PlanMeshDraw(shape, record.count, record.instances);
}

struct MeshIndexedRecord {
	uint32_t count          = 0;
	uint32_t instances      = 0;
	uint32_t first_index    = 0;
	uint32_t base_vertex    = 0;
	uint32_t first_instance = 0;
};

// The base vertex reaches the shader only through its SGPR.
[[nodiscard]] constexpr MeshDrawPlan
DeriveMeshIndexedRecord(const MeshDrawShape& shape, const MeshIndexedRecord& record) noexcept {
	return DeriveMeshIndirectRecord(shape,
	                                {record.count, record.instances, 0, record.first_instance});
}

struct IndexWindow {
	uint64_t address = 0;
	// Indices from `address` that lie inside INDEX_BUFFER_SIZE; later ones read as zero.
	uint32_t readable = 0;
};

// An indirect record's indices; an INDEX_BUFFER_SIZE of zero was never set and bounds nothing.
[[nodiscard]] constexpr IndexWindow IndirectIndexWindow(uint64_t base, uint32_t element_size,
                                                        uint32_t buffer_size,
                                                        uint32_t first_index) noexcept {
	const uint64_t address = base + static_cast<uint64_t>(first_index) * element_size;
	if (buffer_size == 0) {
		return {address, std::numeric_limits<uint32_t>::max()};
	}
	return {address, buffer_size > first_index ? buffer_size - first_index : 0u};
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHGUARD_H_
