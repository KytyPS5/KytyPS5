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

// Splits a groups x instances mesh dispatch into host draws that each respect the
// per-dimension and total workgroup limits. Slices cover whole instance ranges first so
// primitive numbering stays contiguous within an instance. Returns no slices when the
// dispatch is empty or the limits cannot host a single workgroup.
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
