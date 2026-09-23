#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHDISPATCH_H_

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct MeshDispatchSlice {
	uint32_t group_offset    = 0;
	uint32_t group_count     = 0;
	uint32_t instance_offset = 0;
	uint32_t instance_count  = 0;
};

/**
 * @brief Splits a mesh workgroup grid into host-limit-compatible slices.
 *
 * Mesh draws are dispatched as `drawMeshTasksEXT(groups, instances, 1)`, and
 * drivers report per-dimension and total workgroup-count limits that guest
 * draws can exceed (e.g. an instance count above 65535). This returns the
 * sub-grids needed to replay such a draw without exceeding the limits. Each
 * slice keeps its own group and instance offsets so the mesh shader's inputs
 * (which derive the primitive range from the group and the instance from
 * `instance_offset` added to the draw's first instance) remain equivalent to
 * a single oversized dispatch.
 *
 * Hosts are required to report limits of at least one in every dimension.
 *
 * @param groups       Mesh workgroup count in the X dimension.
 * @param instances    Workgroup count in the Y dimension (guest instances).
 * @param max_groups   Host X-dimension workgroup limit.
 * @param max_instances Host Y-dimension workgroup limit.
 * @param max_total    Host total workgroup count limit.
 * @return The sub-grids covering `groups` x `instances`, or an empty vector when
 *         either count is zero.
 */
inline std::vector<MeshDispatchSlice> SplitMeshDispatch(uint32_t groups, uint32_t instances,
                                                        uint32_t max_groups, uint32_t max_instances,
                                                        uint32_t max_total) {
	std::vector<MeshDispatchSlice> slices;
	if (groups == 0u || instances == 0u) {
		return slices;
	}
	const uint32_t group_stride    = std::max(1u, max_groups);
	const uint32_t instance_stride = std::max(1u, max_instances);
	const uint32_t total_limit     = std::max(1u, max_total);
	for (uint32_t group = 0u; group < groups; group += group_stride) {
		const uint32_t group_count = std::min(group_stride, groups - group);
		// The total-limit cap is shared with the instance dimension. When the
		// remaining instance range already fits under that cap together with
		// this group chunk, keep it in one chunk.
		uint32_t instance_chunk = instance_stride;
		if (static_cast<uint64_t>(group_count) * instances > total_limit) {
			instance_chunk = std::max(1u, total_limit / group_count);
			instance_chunk = std::min(instance_chunk, instance_stride);
		}
		for (uint32_t instance = 0u; instance < instances; instance += instance_chunk) {
			slices.push_back(
			    {group, group_count, instance, std::min(instance_chunk, instances - instance)});
		}
	}
	return slices;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHDISPATCH_H_
