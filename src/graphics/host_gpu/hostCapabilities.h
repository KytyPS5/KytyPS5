#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_HOSTCAPABILITIES_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_HOSTCAPABILITIES_H_

#include <cstdint>

// What the host GPU can do beyond the features every device must have. The device decision
// (deviceSuitability.h) fills these in once; the shader recompiler and the renderer read them and
// never ask the device or the platform again.

namespace Libs::Graphics {

// Optional capabilities. Each one is on only when the device reports it and the emulator
// enables it. Guest work that needs a capability that is off is either done another way or stops
// the emulator with a message that names the capability.
struct HostCapabilities {
	// A texture view can carry a minimum LOD (VK_EXT_image_view_min_lod).
	bool image_view_min_lod = false;
	bool cull_distance      = false;
	// 64-bit atomics on storage buffers, on compute shared memory, and on storage images.
	bool buffer_int64_atomics = false;
	bool shared_int64_atomics = false;
	bool image_int64_atomics  = false;
	bool float64              = false;
	bool fragment_barycentric = false;
	bool depth_bounds         = false;
	bool depth_clamp          = false;
	// Depth clipping can be set apart from depth clamping (VK_EXT_depth_clip_enable).
	bool depth_clip_enable = false;
	// Colour writes can be turned off per attachment at draw time (VK_EXT_color_write_enable).
	bool color_write_enable = false;
	bool mesh_shader        = false;
	// Derivatives in compute shaders, over 2x2 groups of invocations
	// (VK_KHR_compute_shader_derivatives).
	bool compute_derivatives = false;
	// Descriptors can be pushed with vkCmdPushDescriptorSet (VK_KHR_push_descriptor).
	bool push_descriptors = false;
	// A packed 10-10-10-2 scaled vertex attribute is fetched by the host. Without it the vertex
	// shader fetches the packed word and unpacks it.
	bool packed_scaled_vertex_input = false;
	// The last vertex of a primitive can be its provoking vertex (VK_EXT_provoking_vertex).
	bool provoking_vertex_last = false;
	// A depth target can be sampled while it is attached (VK_EXT_attachment_feedback_loop_layout
	// and its dynamic state).
	bool attachment_feedback_loop = false;
	// Compute shaders can run with a subgroup size of 64 (subgroupSizeControl).
	bool compute_subgroup_size_control = false;
	// A block-compressed image can have an uncompressed view of one block per texel
	// (VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT).
	bool block_texel_view = false;
	// Images can be copied between depth and color formats (maintenance8).
	bool depth_color_copy = false;
	// Stages that may use subgroup operations: VkPhysicalDeviceVulkan11Properties::
	// subgroupSupportedStages, as VkShaderStageFlagBits values.
	uint32_t subgroup_supported_stages = 0;
	// The subgroup operations those stages have: VkPhysicalDeviceVulkan11Properties::
	// subgroupSupportedOperations, as VkSubgroupFeatureFlagBits values.
	uint32_t subgroup_supported_operations = 0;

	// A host with every capability. For tests and offline compilation only: the renderer always
	// passes what its device enabled.
	[[nodiscard]] static constexpr HostCapabilities Full() {
		HostCapabilities all;
		all.image_view_min_lod        = true;
		all.cull_distance             = true;
		all.buffer_int64_atomics      = true;
		all.shared_int64_atomics      = true;
		all.image_int64_atomics       = true;
		all.float64                   = true;
		all.fragment_barycentric      = true;
		all.depth_bounds              = true;
		all.depth_clamp               = true;
		all.depth_clip_enable         = true;
		all.color_write_enable        = true;
		all.mesh_shader               = true;
		all.compute_derivatives       = true;
		all.push_descriptors          = true;
		all.packed_scaled_vertex_input = true;
		all.provoking_vertex_last         = true;
		all.attachment_feedback_loop      = true;
		all.compute_subgroup_size_control = true;
		all.block_texel_view              = true;
		all.depth_color_copy              = true;
		all.subgroup_supported_stages     = ~0u;
		all.subgroup_supported_operations = ~0u;
		return all;
	}

	bool operator==(const HostCapabilities&) const = default;
};

// The driver behind the device, where the emulator must know it.
enum class HostDriver : uint8_t { Other, MoltenVk };

// Defects of a driver: things the device reports as available that do not work. They are kept
// apart from capabilities, so that a capability never depends on a driver's name.
struct DriverFaults {
	// Pushed descriptors get no buffer size table, so a shader that asks for a buffer length
	// reads a null buffer.
	bool pushed_buffers_have_no_size = false;
	// A pixel shader input with the raw values of the three vertices of its triangle
	// (PerVertexKHR) cannot be translated for the device.
	bool no_per_vertex_inputs = false;
	// Barycentrics interpolated at the centroid or at a sample cannot be translated for the
	// device. A shader reads them at the pixel center instead.
	bool no_centroid_barycentric = false;
	// A plain load marked volatile may be done once and reused, so a loop that waits for a value
	// another invocation stores never sees it.
	bool volatile_loads_are_reused = false;
	// An operation marked NoContraction is translated to a call of a helper that is never
	// inlined or optimized. A shader with many of them runs several times slower.
	bool no_contraction_is_slow = false;
	// Creating a pipeline cache from saved data takes about a second per megabyte and blocks
	// the thread: a cache of a few hundred pipelines holds the start of a game for over a
	// minute. What it saves, the translation of shaders to the device's language, is done by
	// worker threads in the background anyway. So the cache is not kept on disk for the driver.
	bool pipeline_cache_load_is_slow = false;
	// A pipeline built with a pipeline cache translates its shaders under the cache's one lock:
	// worker threads that share a cache build their pipelines one at a time. Measured on a cold
	// start: ten builders, nine of them waiting on the lock nearly all the time. So each worker
	// has a cache of its own. (Without any cache, a run lost the device: Metal reported an
	// invalid resource.)
	bool pipeline_cache_serializes_builds = false;
	// The device runs subgroup operations in a vertex function, but the driver does not report
	// them for the vertex stage and has no subgroup built-in variables there. MoltenVK on an
	// Apple GPU: Metal has SIMD-group functions in vertex functions, but no lane index input.
	// The vertex stages then use the operations (see subgroup_supported_stages), and a lane
	// index comes from a scan.
	bool vertex_subgroups_unreported = false;

	bool operator==(const DriverFaults&) const = default;
};

// Measured on MoltenVK 1.4.2. No MoltenVK release without these defects is known, so they apply
// to every version.
[[nodiscard]] constexpr DriverFaults FaultsOf(HostDriver driver) {
	DriverFaults faults;
	if (driver == HostDriver::MoltenVk) {
		faults.pushed_buffers_have_no_size = true;
		faults.no_per_vertex_inputs        = true;
		faults.no_centroid_barycentric     = true;
		faults.volatile_loads_are_reused   = true;
		faults.no_contraction_is_slow      = true;
		faults.pipeline_cache_load_is_slow = true;
		faults.pipeline_cache_serializes_builds = true;
	}
	return faults;
}

// The host GPU as the shader recompiler and the renderer see it.
struct HostGpu {
	HostCapabilities capabilities;
	DriverFaults     faults;

	// A host with every capability and no defects. For tests and offline compilation only.
	[[nodiscard]] static constexpr HostGpu Full() { return {HostCapabilities::Full(), {}}; }

	bool operator==(const HostGpu&) const = default;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_HOSTCAPABILITIES_H_ */
