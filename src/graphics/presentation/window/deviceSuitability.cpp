#include "graphics/presentation/window/deviceSuitability.h"

#include <algorithm>

namespace Libs::Graphics {

namespace {

constexpr const char* kMinLodExtension          = "VK_EXT_image_view_min_lod";
constexpr const char* kBarycentricExtension     = "VK_KHR_fragment_shader_barycentric";
constexpr const char* kWorkgroupLayoutExtension = "VK_KHR_workgroup_memory_explicit_layout";
constexpr const char* kPushDescriptorExtension  = "VK_KHR_push_descriptor";
constexpr const char* kColorWriteExtension      = "VK_EXT_color_write_enable";
constexpr const char* kDepthClipEnableExtension = "VK_EXT_depth_clip_enable";
constexpr const char* kImageAtomic64Extension   = "VK_EXT_shader_image_atomic_int64";
constexpr const char* kMeshShaderExtension      = "VK_EXT_mesh_shader";
constexpr const char* kComputeDerivativesExtension = "VK_KHR_compute_shader_derivatives";
constexpr const char* kProvokingVertexExtension    = "VK_EXT_provoking_vertex";
constexpr const char* kFeedbackLayoutExtension     = "VK_EXT_attachment_feedback_loop_layout";
constexpr const char* kFeedbackDynamicExtension    = "VK_EXT_attachment_feedback_loop_dynamic_state";
constexpr const char* kMaintenance8Extension       = "VK_KHR_maintenance8";

bool HasExtension(const std::vector<std::string>& extensions, const std::string& name) {
	return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
}

} // namespace

DeviceDecision EvaluateDeviceSuitability(const DeviceFacts&              facts,
                                         const std::vector<std::string>& required_extensions) {
	DeviceDecision decision;

	const auto require = [&](bool supported, const char* name) {
		if (!supported) {
			decision.rejections.push_back(std::string(name) + " is not supported");
		}
	};

	// Every guest program may need these, and the emulator has no other way to do them.
	require(facts.depth_clip_control, "depthClipControl");
	require(facts.sampler_mirror_clamp_to_edge, "samplerMirrorClampToEdge");
	require(facts.storage_buffer_16bit_access, "storageBuffer16BitAccess");
	require(facts.storage_buffer_8bit_access, "storageBuffer8BitAccess");
	require(facts.timeline_semaphore, "timelineSemaphore");
	require(facts.shader_output_layer, "shaderOutputLayer");
	require(facts.shader_output_viewport_index, "shaderOutputViewportIndex");
	require(facts.buffer_device_address, "bufferDeviceAddress");
	require(facts.shader_sampled_image_array_non_uniform_indexing,
	        "shaderSampledImageArrayNonUniformIndexing");
	require(facts.robust_image_access, "robustImageAccess");
	require(facts.dynamic_rendering, "dynamicRendering");
	require(facts.synchronization2, "synchronization2");
	require(facts.sample_rate_shading, "sampleRateShading");
	require(facts.depth_bias_clamp, "depthBiasClamp");
	require(facts.shader_clip_distance, "shaderClipDistance");
	require(facts.large_points, "largePoints");
	require(facts.multi_viewport, "multiViewport");
	require(facts.fill_mode_non_solid, "fillModeNonSolid");
	require(facts.fragment_stores_and_atomics, "fragmentStoresAndAtomics");
	require(facts.sampler_anisotropy, "samplerAnisotropy");
	require(facts.robust_buffer_access, "robustBufferAccess");
	require(facts.shader_storage_image_write_without_format,
	        "shaderStorageImageWriteWithoutFormat");
	require(facts.shader_image_gather_extended, "shaderImageGatherExtended");
	require(facts.independent_blend, "independentBlend");
	require(facts.tessellation_shader, "tessellationShader");
	require(facts.shader_int64, "shaderInt64");
	require(facts.vertex_pipeline_stores_and_atomics, "vertexPipelineStoresAndAtomics");
	require(facts.dual_src_blend, "dualSrcBlend");

	for (const auto& extension: required_extensions) {
		if (!HasExtension(facts.extensions, extension)) {
			decision.rejections.push_back(extension + " is not supported");
		}
	}

	// Optional capabilities: on only when the device reports the feature (and the extension that
	// carries it).
	const auto with_extension = [&](bool feature, const char* extension) {
		return feature && HasExtension(facts.extensions, extension);
	};
	auto& caps                = decision.capabilities;
	caps.image_view_min_lod   = with_extension(facts.image_view_min_lod, kMinLodExtension);
	caps.cull_distance        = facts.shader_cull_distance;
	caps.buffer_int64_atomics = facts.shader_buffer_int64_atomics;
	// Native 64-bit atomics on shared memory need the explicit workgroup layout as well.
	caps.shared_int64_atomics =
	    facts.shader_shared_int64_atomics &&
	    with_extension(facts.workgroup_memory_explicit_layout, kWorkgroupLayoutExtension);
	caps.image_int64_atomics =
	    with_extension(facts.shader_image_int64_atomics, kImageAtomic64Extension);
	caps.float64 = facts.shader_float64 && facts.float64_preserves_special_values;
	caps.fragment_barycentric =
	    with_extension(facts.fragment_shader_barycentric, kBarycentricExtension);
	caps.depth_bounds       = facts.depth_bounds;
	caps.depth_clamp        = facts.depth_clamp;
	caps.depth_clip_enable  = with_extension(facts.depth_clip_enable, kDepthClipEnableExtension);
	caps.color_write_enable = with_extension(facts.color_write_enable, kColorWriteExtension);
	caps.mesh_shader        = with_extension(facts.mesh_shader, kMeshShaderExtension);
	caps.compute_derivatives =
	    with_extension(facts.compute_derivative_group_quads, kComputeDerivativesExtension);
	caps.push_descriptors   = HasExtension(facts.extensions, kPushDescriptorExtension);
	caps.packed_scaled_vertex_input = facts.packed_scaled_vertex_format;
	caps.provoking_vertex_last =
	    with_extension(facts.provoking_vertex_last, kProvokingVertexExtension);
	caps.attachment_feedback_loop =
	    with_extension(facts.attachment_feedback_loop_layout, kFeedbackLayoutExtension) &&
	    with_extension(facts.attachment_feedback_loop_dynamic_state, kFeedbackDynamicExtension);
	// Guest compute shaders run in waves of 64.
	constexpr uint32_t compute_stage = 0x20u; // VK_SHADER_STAGE_COMPUTE_BIT
	caps.compute_subgroup_size_control =
	    facts.subgroup_size_control && (facts.required_subgroup_size_stages & compute_stage) != 0u &&
	    facts.min_subgroup_size <= 64u && facts.max_subgroup_size >= 64u;
	caps.block_texel_view          = facts.block_texel_view;
	caps.depth_color_copy          = with_extension(facts.maintenance8, kMaintenance8Extension);
	caps.subgroup_supported_stages = facts.subgroup_supported_stages;
	caps.subgroup_supported_operations = facts.subgroup_supported_operations;

	decision.faults = FaultsOf(facts.driver);
	// Metal runs SIMD-group functions in a vertex function on an Apple GPU, and MoltenVK passes
	// them through, but reports no subgroup operations for the vertex stage (MoltenVK 1.4.2;
	// the vertex ordered-count GPU test checks it). A wave of the vertex stage is then one
	// SIMD group, as it is on the guest, instead of one vertex.
	constexpr uint32_t vertex_stage = 0x1u; // VK_SHADER_STAGE_VERTEX_BIT
	if (facts.driver == HostDriver::MoltenVk && facts.apple_gpu &&
	    (facts.subgroup_supported_stages & vertex_stage) == 0u) {
		decision.faults.vertex_subgroups_unreported = true;
		caps.subgroup_supported_stages |= vertex_stage;
	}

	const auto note = [&](bool available, const char* name) {
		if (!available) {
			decision.unavailable.push_back(std::string(name) + " is unavailable");
		}
	};
	note(caps.image_view_min_lod, "image view minLod");
	note(caps.cull_distance, "shaderCullDistance");
	note(caps.buffer_int64_atomics, "shaderBufferInt64Atomics");
	note(caps.shared_int64_atomics, "64-bit atomics on compute shared memory");
	note(caps.image_int64_atomics, "shaderImageInt64Atomics");
	note(caps.float64, "shaderFloat64 that keeps special values (64-bit floats run in software)");
	note(caps.fragment_barycentric, "fragmentShaderBarycentric");
	note(caps.depth_bounds, "depthBounds");
	note(caps.depth_clamp, "depthClamp");
	note(caps.depth_clip_enable, "depthClipEnable");
	note(caps.color_write_enable, "colorWriteEnable");
	note(caps.packed_scaled_vertex_input, "a packed 10-10-10-2 scaled vertex format");
	note(caps.mesh_shader, "meshShader");
	note(caps.compute_derivatives, "computeDerivativeGroupQuads");
	note(caps.push_descriptors, "push descriptors");
	note(caps.provoking_vertex_last, "provokingVertexLast");
	note(caps.attachment_feedback_loop, "attachment feedback loops");
	note(caps.compute_subgroup_size_control, "a compute subgroup size of 64");
	note(caps.block_texel_view, "block texel views");
	note(caps.depth_color_copy, "depth/color image copies");
	note(!decision.faults.pushed_buffers_have_no_size,
	     "a buffer size table for pushed descriptors (driver defect)");
	note(!decision.faults.no_per_vertex_inputs, "per-vertex pixel shader inputs (driver defect)");
	note(!decision.faults.no_centroid_barycentric, "centroid and sample barycentrics (driver defect)");

	return decision;
}

} // namespace Libs::Graphics
