#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VERTEXSUBGROUPPROBE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VERTEXSUBGROUPPROBE_H_

#include "graphics/host_gpu/hostCapabilities.h"

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <vector>

// DriverFaults::vertex_subgroups_unreported lets vertex shaders use subgroup operations that the
// driver does not report for the vertex stage. A probe checks on the device itself that they work
// before the shaders use them: a vertex shader of a draw of points records, for each vertex, what
// the operations the emitter uses (an exclusive scan, a reduction, a ballot and a shuffle) gave.

namespace Libs::Graphics {

// What the probe's shader records for one vertex.
struct VertexSubgroupSample {
	uint32_t lane         = 0; // the exclusive scan of 1: its rank in its group
	uint32_t count        = 0; // the reduction of 1: the size of its group
	uint32_t first_vertex = 0; // the vertex index of its group's first lane, by a shuffle
	uint32_t ballot_low   = 0; // the low word of a ballot of true
};

// The samples of a draw whose vertex indices are the sample indices describe groups of at most
// 32 vertices, each named by its first vertex, whose ranks are 0 to its size minus one. The
// lanes of a group are its lowest lanes: the emitter uses a rank as a lane index.
[[nodiscard]] inline bool VertexSubgroupSamplesAreConsistent(
    std::span<const VertexSubgroupSample> samples) {
	struct Group {
		uint32_t           count      = 0;
		uint32_t           ballot_low = 0;
		std::set<uint32_t> lanes;
	};
	std::map<uint32_t, Group> groups;
	for (const auto& sample: samples) {
		if (sample.count == 0 || sample.count > 32 || sample.lane >= sample.count ||
		    sample.first_vertex >= samples.size()) {
			return false;
		}
		const uint32_t lowest_lanes =
		    sample.count == 32 ? ~uint32_t {0} : (uint32_t {1} << sample.count) - 1u;
		if (sample.ballot_low != lowest_lanes) {
			return false;
		}
		const auto& first = samples[sample.first_vertex];
		if (first.lane != 0 || first.first_vertex != sample.first_vertex) {
			return false;
		}
		auto& group = groups[sample.first_vertex];
		if (group.lanes.empty()) {
			group.count      = sample.count;
			group.ballot_low = sample.ballot_low;
		} else if (group.count != sample.count || group.ballot_low != sample.ballot_low) {
			return false;
		}
		if (!group.lanes.insert(sample.lane).second) {
			return false;
		}
	}
	for (const auto& [first_vertex, group]: groups) {
		if (group.lanes.size() != group.count) {
			return false;
		}
	}
	return !samples.empty();
}

// Without a passed probe, the vertex stage has no subgroup operations.
inline void ApplyVertexSubgroupProbe(HostGpu& host, bool passed) {
	constexpr uint32_t vertex_stage = 0x1u; // VK_SHADER_STAGE_VERTEX_BIT
	if (host.faults.vertex_subgroups_unreported && !passed) {
		host.faults.vertex_subgroups_unreported = false;
		host.capabilities.subgroup_supported_stages &= ~vertex_stage;
	}
}

// The probe's vertex shader: binding 0 of set 0 is a storage buffer of four words per vertex.
[[nodiscard]] std::vector<uint32_t> VertexSubgroupProbeSpirv();

struct GraphicContext;

// Runs the probe on the device of `graphics` and checks its samples. False also when a Vulkan
// call fails.
[[nodiscard]] bool ProbeVertexSubgroups(GraphicContext& graphics);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_VERTEXSUBGROUPPROBE_H_
