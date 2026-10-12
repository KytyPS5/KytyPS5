#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_WINDOW_DEVICESUITABILITY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_WINDOW_DEVICESUITABILITY_H_

#include "graphics/host_gpu/hostCapabilities.h"

#include <string>
#include <vector>

// Decides whether a Vulkan device can run the emulator and which optional capabilities it offers.
// The decision uses plain data only, so it can be tested without a GPU.

namespace Libs::Graphics {

// What the physical device reports. A feature of an extension is only meaningful when the
// extension is listed in `extensions`.
struct DeviceFacts {
	std::vector<std::string> extensions;
	HostDriver               driver = HostDriver::Other;
	// VkPhysicalDeviceProperties::vendorID is Apple's.
	bool apple_gpu = false;
	// VkPhysicalDeviceVulkan11Properties::subgroupSupportedStages.
	uint32_t subgroup_supported_stages = 0;
	// VkPhysicalDeviceVulkan11Properties::subgroupSupportedOperations.
	uint32_t subgroup_supported_operations = 0;

	// Core 1.0 features.
	bool depth_clamp                               = false;
	bool depth_bias_clamp                          = false;
	bool sample_rate_shading                       = false;
	bool shader_clip_distance                      = false;
	bool shader_cull_distance                      = false;
	bool large_points                              = false;
	bool multi_viewport                            = false;
	bool fill_mode_non_solid                       = false;
	bool fragment_stores_and_atomics               = false;
	bool vertex_pipeline_stores_and_atomics        = false;
	bool sampler_anisotropy                        = false;
	bool robust_buffer_access                      = false;
	bool depth_bounds                              = false;
	bool shader_storage_image_write_without_format = false;
	bool shader_image_gather_extended              = false;
	bool independent_blend                         = false;
	bool dual_src_blend                            = false;
	// VK_FORMAT_A2B10G10R10_USCALED_PACK32 can be a vertex attribute (not on Metal).
	bool packed_scaled_vertex_format               = false;
	bool tessellation_shader                       = false;
	bool shader_int64                              = false;
	bool shader_float64                            = false;

	// Vulkan 1.1 features.
	bool storage_buffer_16bit_access = false;

	// Vulkan 1.2 features.
	bool sampler_mirror_clamp_to_edge                    = false;
	bool timeline_semaphore                              = false;
	bool shader_output_layer                             = false;
	bool shader_output_viewport_index                    = false;
	bool buffer_device_address                           = false;
	bool storage_buffer_8bit_access                      = false;
	bool shader_sampled_image_array_non_uniform_indexing = false;
	bool shader_buffer_int64_atomics                     = false;
	bool shader_shared_int64_atomics                     = false;
	// The device keeps signed zero, infinity and NaN in 64-bit float operations
	// (VkPhysicalDeviceVulkan12Properties::shaderSignedZeroInfNanPreserveFloat64).
	bool float64_preserves_special_values = false;

	// Vulkan 1.3 features.
	bool robust_image_access = false;
	bool dynamic_rendering   = false;
	bool synchronization2    = false;
	bool subgroup_size_control = false;

	// VkPhysicalDeviceSubgroupSizeControlProperties.
	uint32_t min_subgroup_size             = 0;
	uint32_t max_subgroup_size             = 0;
	uint32_t required_subgroup_size_stages = 0;
	// A BC1 image can be created with VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT.
	bool block_texel_view = false;

	// Features of extensions.
	bool color_write_enable               = false; // VK_EXT_color_write_enable
	bool depth_clip_control               = false; // VK_EXT_depth_clip_control
	bool depth_clip_enable                = false; // VK_EXT_depth_clip_enable
	bool image_view_min_lod               = false; // VK_EXT_image_view_min_lod
	bool fragment_shader_barycentric      = false; // VK_KHR_fragment_shader_barycentric
	bool workgroup_memory_explicit_layout = false; // VK_KHR_workgroup_memory_explicit_layout
	bool shader_image_int64_atomics       = false; // VK_EXT_shader_image_atomic_int64
	bool mesh_shader                      = false; // VK_EXT_mesh_shader
	bool provoking_vertex_last            = false; // VK_EXT_provoking_vertex
	bool maintenance8                     = false; // VK_KHR_maintenance8
	// VK_EXT_attachment_feedback_loop_layout and VK_EXT_attachment_feedback_loop_dynamic_state.
	bool attachment_feedback_loop_layout        = false;
	bool attachment_feedback_loop_dynamic_state = false;
	// computeDerivativeGroupQuads of VK_KHR_compute_shader_derivatives.
	bool compute_derivative_group_quads = false;
};

struct DeviceDecision {
	// Why the device cannot run the emulator. Empty when it can.
	std::vector<std::string> rejections;
	// Optional capabilities the device lacks, one line each, for the log.
	std::vector<std::string> unavailable;
	HostCapabilities         capabilities;
	DriverFaults             faults;

	[[nodiscard]] bool Accepted() const { return rejections.empty(); }
};

// `required_extensions` are the extensions the caller cannot do without.
[[nodiscard]] DeviceDecision
EvaluateDeviceSuitability(const DeviceFacts&              facts,
                          const std::vector<std::string>& required_extensions);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_WINDOW_DEVICESUITABILITY_H_ */
