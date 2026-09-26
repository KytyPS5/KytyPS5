#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// Host limits that bound a single vkCmdDrawMeshTasksEXT call.
struct MeshDispatchLimits {
	uint32_t max_groups    = 0; // maxMeshWorkGroupCount[0]
	uint32_t max_instances = 0; // maxMeshWorkGroupCount[1]
	uint32_t max_total     = 0; // maxMeshWorkGroupTotalCount
};

// One host draw of a guest mesh dispatch. base_group offsets gl_WorkGroupID.x in the mesh
// prolog and first_instance is relative to the guest draw's first instance.
struct MeshDispatchSlice {
	uint32_t base_group     = 0;
	uint32_t group_count    = 0;
	uint32_t first_instance = 0;
	uint32_t instance_count = 0;
};

// Upper bound on the host draws one guest dispatch may be replayed as; beyond it the
// dispatch is treated as invalid rather than expanded into an unbounded draw list.
constexpr uint64_t MaxMeshDispatchSlices = 4096;

// Splits a groups x instances mesh dispatch into host draws that each respect the
// per-dimension and total workgroup limits. Slices cover whole instance ranges first so
// primitive numbering stays contiguous within an instance. Returns no slices when the
// dispatch is empty, the limits cannot host a single workgroup, or the dispatch would
// need more than MaxMeshDispatchSlices draws.
inline std::vector<MeshDispatchSlice> SplitMeshDispatch(uint32_t groups, uint32_t instances,
                                                        const MeshDispatchLimits& limits) {
	std::vector<MeshDispatchSlice> slices;
	if (groups == 0 || instances == 0 || limits.max_groups == 0 || limits.max_instances == 0 ||
	    limits.max_total == 0) {
		return slices;
	}
	const uint32_t group_stride = std::min({groups, limits.max_groups, limits.max_total});
	const uint32_t instance_stride =
	    std::min({instances, limits.max_instances, limits.max_total / group_stride});
	const uint64_t group_slices    = (uint64_t {groups} - 1u) / group_stride + 1u;
	const uint64_t instance_slices = (uint64_t {instances} - 1u) / instance_stride + 1u;
	if (group_slices * instance_slices > MaxMeshDispatchSlices) {
		return slices;
	}
	slices.reserve(static_cast<size_t>(group_slices * instance_slices));
	for (uint32_t instance = 0; instance < instances;) {
		const uint32_t instance_count = std::min(instance_stride, instances - instance);
		for (uint32_t group = 0; group < groups;) {
			const uint32_t group_count = std::min(group_stride, groups - group);
			slices.push_back({group, group_count, instance, instance_count});
			group += group_count;
		}
		instance += instance_count;
	}
	return slices;
}

} // namespace Libs::Graphics
