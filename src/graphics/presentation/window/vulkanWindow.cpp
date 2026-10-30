#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "common/trace.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vertexSubgroupProbe.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/openxrHeadset.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/deviceSuitability.h"
#include "graphics/presentation/window/windowInternal.h"
#include "kernel/memory.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <cstdlib>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF

namespace Libs::Graphics {

struct VulkanExtensions {
	bool enable_validation_layers = false;

	std::vector<const char*>             required_extensions;
	std::vector<vk::ExtensionProperties> available_extensions;
	std::vector<const char*>             required_layers;
	std::vector<vk::LayerProperties>     available_layers;
};

vk::PhysicalDeviceVulkan11Features WindowContext::RequiredVulkan11Features() noexcept {
	vk::PhysicalDeviceVulkan11Features features {};
	features.storageBuffer16BitAccess = VK_TRUE;
	features.shaderDrawParameters     = VK_TRUE;
	return features;
}

vk::PhysicalDeviceVulkan12Features WindowContext::RequiredVulkan12Features() noexcept {
	vk::PhysicalDeviceVulkan12Features features {};
	features.samplerMirrorClampToEdge  = VK_TRUE;
	features.timelineSemaphore         = VK_TRUE;
	features.shaderOutputLayer         = VK_TRUE;
	features.shaderOutputViewportIndex = VK_TRUE;
	features.bufferDeviceAddress       = VK_TRUE;
	features.storageBuffer8BitAccess   = VK_TRUE;
	features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	features.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
	features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
	features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	features.descriptorBindingUpdateUnusedWhilePending    = VK_TRUE;
	features.descriptorBindingPartiallyBound              = VK_TRUE;
	return features;
}

vk::PhysicalDeviceVulkan13Features WindowContext::RequiredVulkan13Features() noexcept {
	vk::PhysicalDeviceVulkan13Features features {};
	features.dynamicRendering = VK_TRUE;
	features.synchronization2 = VK_TRUE;
	return features;
}

static bool HasExtension(const std::vector<vk::ExtensionProperties>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(),
	                   [name](const auto& ext) { return strcmp(ext.extensionName, name) == 0; });
}

static bool HasExtension(const std::vector<const char*>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(),
	                   [name](const char* ext) { return strcmp(ext, name) == 0; });
}

static bool HasLayer(const std::vector<vk::LayerProperties>& layers, const char* name) {
	return std::any_of(layers.begin(), layers.end(),
	                   [name](const auto& layer) { return strcmp(layer.layerName, name) == 0; });
}

static void GetSurfaceCapabilities(vk::PhysicalDevice physical_device, vk::SurfaceKHR surface,
                                   SurfaceCapabilities& r) {
	RequireVulkanSuccess(physical_device.getSurfaceCapabilitiesKHR(surface, &r.capabilities),
	                     "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

	r.formats = EnumerateVulkan<vk::SurfaceFormatKHR>( // @suppress("Ambiguous problem")
	    "vkGetPhysicalDeviceSurfaceFormatsKHR", [&](uint32_t* count, vk::SurfaceFormatKHR* values) {
		    return physical_device.getSurfaceFormatsKHR(surface, count, values);
	    });
	EXIT_NOT_IMPLEMENTED(r.formats.empty());

	r.present_modes = EnumerateVulkan<vk::PresentModeKHR>( // @suppress("Ambiguous problem")
	    "vkGetPhysicalDeviceSurfacePresentModesKHR",
	    [&](uint32_t* count, vk::PresentModeKHR* values) {
		    return physical_device.getSurfacePresentModesKHR(surface, count, values);
	    });
	EXIT_NOT_IMPLEMENTED(r.present_modes.empty());
}

static bool CheckFormat(vk::PhysicalDevice device, vk::Format format, bool tile,
                        vk::FormatFeatureFlags features) {
	vk::FormatProperties format_props {};
	device.getFormatProperties(format, &format_props);

	const auto supported_features =
	    (tile ? format_props.optimalTilingFeatures : format_props.linearTilingFeatures);
	return (supported_features & features) == features;
}

static uint32_t VulkanFindQueueFamily(vk::PhysicalDevice device, vk::SurfaceKHR surface) {
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);

	uint32_t queue_family_count = 0;
	device.getQueueFamilyProperties(&queue_family_count, nullptr);
	std::vector<vk::QueueFamilyProperties> queue_families(queue_family_count);
	device.getQueueFamilyProperties(&queue_family_count, queue_families.data());

	const auto required = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
	for (uint32_t family = 0; family < queue_family_count; family++) {
		const auto& properties             = queue_families[family];
		vk::Bool32  presentation_supported = VK_FALSE;
		RequireVulkanSuccess(device.getSurfaceSupportKHR(family, surface, &presentation_supported),
		                     "vkGetPhysicalDeviceSurfaceSupportKHR");

		LOGF("\tqueue family: %s [count = %u], [present = %s]\n",
		     vk::to_string(properties.queueFlags).c_str(), properties.queueCount,
		     (presentation_supported == VK_TRUE ? "true" : "false"));
		if (properties.queueCount != 0 && (properties.queueFlags & required) == required &&
		    presentation_supported == VK_TRUE) {
			LOGF("\tselected universal queue family %u\n", family);
			return family;
		}
	}
	return static_cast<uint32_t>(-1);
}

// On failure out_device is null and out_rejections says why each device was skipped.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
DeviceFacts WindowContext::ReadDeviceFacts(vk::PhysicalDevice                          device,
                                           const std::vector<vk::ExtensionProperties>& extensions) {
	// A feature struct of an extension the device lacks is left untouched, so every field of it
	// stays false.
	vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR      derivatives {};
	vk::PhysicalDeviceMeshShaderFeaturesEXT                    mesh {};
	mesh.pNext = &derivatives;
	vk::PhysicalDeviceShaderImageAtomicInt64FeaturesEXT        image_atomic_int64 {};
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR workgroup_layout {};
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR     barycentric {};
	vk::PhysicalDeviceColorWriteEnableFeaturesEXT              color_write {};
	vk::PhysicalDeviceImageViewMinLodFeaturesEXT               min_lod {};
	vk::PhysicalDeviceDepthClipEnableFeaturesEXT               depth_clip_enable {};
	vk::PhysicalDeviceDepthClipControlFeaturesEXT              depth_clip_control {};
	vk::PhysicalDeviceProvokingVertexFeaturesEXT               provoking_vertex {};
	vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT  feedback_layout {};
	vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT feedback_dynamic {};
	vk::PhysicalDeviceMaintenance8FeaturesKHR                  maintenance8 {};
	vk::PhysicalDeviceVulkan11Features                         features11 {};
	vk::PhysicalDeviceVulkan12Features                         features12 {};
	vk::PhysicalDeviceVulkan13Features                         features13 {};
	vk::PhysicalDeviceFeatures2                                features2 {};
	image_atomic_int64.pNext = &mesh;
	workgroup_layout.pNext   = &image_atomic_int64;
	barycentric.pNext        = &workgroup_layout;
	color_write.pNext        = &barycentric;
	min_lod.pNext            = &color_write;
	depth_clip_enable.pNext  = &min_lod;
	depth_clip_control.pNext = &depth_clip_enable;
	provoking_vertex.pNext   = &depth_clip_control;
	feedback_layout.pNext    = &provoking_vertex;
	feedback_dynamic.pNext   = &feedback_layout;
	maintenance8.pNext       = &feedback_dynamic;
	features11.pNext         = &maintenance8;
	features12.pNext         = &features11;
	features13.pNext         = &features12;
	features2.pNext          = &features13;
	device.getFeatures2(&features2);

	vk::PhysicalDeviceSubgroupSizeControlProperties subgroup_size_control {};
	vk::PhysicalDeviceVulkan11Properties            properties11 {};
	vk::PhysicalDeviceVulkan12Properties            properties12 {};
	vk::PhysicalDeviceProperties2                   properties2 {};
	properties11.pNext = &subgroup_size_control;
	properties12.pNext = &properties11;
	properties2.pNext  = &properties12;
	device.getProperties2(&properties2);

	DeviceFacts facts;
	facts.extensions.reserve(extensions.size());
	for (const auto& extension: extensions) {
		facts.extensions.emplace_back(extension.extensionName.data());
	}
	facts.driver =
	    properties12.driverID == vk::DriverId::eMoltenvk ? HostDriver::MoltenVk : HostDriver::Other;
	facts.apple_gpu = properties2.properties.vendorID == 0x106bu;
	facts.amd_shader_compiler =
	    properties2.properties.vendorID == 0x1002u &&
	    (properties12.driverID == vk::DriverId::eMesaRadv ||
	     properties12.driverID == vk::DriverId::eAmdOpenSource ||
	     properties12.driverID == vk::DriverId::eAmdProprietary);
	facts.float32_preserves_special_values =
	    properties12.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE;
	facts.subgroup_supported_stages =
	    static_cast<vk::ShaderStageFlags::MaskType>(properties11.subgroupSupportedStages);
	facts.subgroup_supported_operations =
	    static_cast<vk::SubgroupFeatureFlags::MaskType>(properties11.subgroupSupportedOperations);
	facts.max_per_stage_samplers = properties2.properties.limits.maxPerStageDescriptorSamplers;
	facts.float64_preserves_special_values =
	    properties12.shaderSignedZeroInfNanPreserveFloat64 == VK_TRUE;
	facts.float16_preserves_special_values =
	    properties12.shaderSignedZeroInfNanPreserveFloat16 == VK_TRUE;

	const auto& core                         = features2.features;
	facts.depth_clamp                        = core.depthClamp == VK_TRUE;
	facts.draw_indirect_first_instance       = core.drawIndirectFirstInstance == VK_TRUE;
	facts.depth_bias_clamp                   = core.depthBiasClamp == VK_TRUE;
	facts.sample_rate_shading                = core.sampleRateShading == VK_TRUE;
	facts.shader_clip_distance               = core.shaderClipDistance == VK_TRUE;
	facts.shader_cull_distance               = core.shaderCullDistance == VK_TRUE;
	facts.large_points                       = core.largePoints == VK_TRUE;
	facts.multi_viewport                     = core.multiViewport == VK_TRUE;
	facts.fill_mode_non_solid                = core.fillModeNonSolid == VK_TRUE;
	facts.fragment_stores_and_atomics        = core.fragmentStoresAndAtomics == VK_TRUE;
	facts.vertex_pipeline_stores_and_atomics = core.vertexPipelineStoresAndAtomics == VK_TRUE;
	facts.sampler_anisotropy                 = core.samplerAnisotropy == VK_TRUE;
	facts.robust_buffer_access               = core.robustBufferAccess == VK_TRUE;
	facts.depth_bounds                       = core.depthBounds == VK_TRUE;
	facts.shader_storage_image_write_without_format =
	    core.shaderStorageImageWriteWithoutFormat == VK_TRUE;
	facts.shader_image_gather_extended = core.shaderImageGatherExtended == VK_TRUE;
	facts.independent_blend            = core.independentBlend == VK_TRUE;
	facts.dual_src_blend               = core.dualSrcBlend == VK_TRUE;
	facts.tessellation_shader          = core.tessellationShader == VK_TRUE;
	facts.shader_int64                 = core.shaderInt64 == VK_TRUE;
	facts.shader_float64               = core.shaderFloat64 == VK_TRUE;

	facts.storage_buffer_16bit_access  = features11.storageBuffer16BitAccess == VK_TRUE;
	facts.sampler_mirror_clamp_to_edge = features12.samplerMirrorClampToEdge == VK_TRUE;
	facts.shader_float16               = features12.shaderFloat16 == VK_TRUE;
	facts.timeline_semaphore           = features12.timelineSemaphore == VK_TRUE;
	facts.shader_output_layer          = features12.shaderOutputLayer == VK_TRUE;
	facts.shader_output_viewport_index = features12.shaderOutputViewportIndex == VK_TRUE;
	facts.buffer_device_address        = features12.bufferDeviceAddress == VK_TRUE;
	facts.storage_buffer_8bit_access   = features12.storageBuffer8BitAccess == VK_TRUE;
	facts.shader_sampled_image_array_non_uniform_indexing =
	    features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
	facts.shader_storage_buffer_array_non_uniform_indexing =
	    features12.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE;
	facts.shader_storage_image_array_non_uniform_indexing =
	    features12.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
	facts.shader_buffer_int64_atomics = features12.shaderBufferInt64Atomics == VK_TRUE;
	facts.shader_shared_int64_atomics = features12.shaderSharedInt64Atomics == VK_TRUE;
	facts.robust_image_access         = features13.robustImageAccess == VK_TRUE;
	facts.dynamic_rendering           = features13.dynamicRendering == VK_TRUE;
	facts.synchronization2            = features13.synchronization2 == VK_TRUE;
	facts.subgroup_size_control       = features13.subgroupSizeControl == VK_TRUE;
	facts.min_subgroup_size           = subgroup_size_control.minSubgroupSize;
	facts.max_subgroup_size           = subgroup_size_control.maxSubgroupSize;
	facts.required_subgroup_size_stages =
	    static_cast<vk::ShaderStageFlags::MaskType>(subgroup_size_control.requiredSubgroupSizeStages);
	const vk::PhysicalDeviceImageFormatInfo2 block_texel_view_info {
	    .format = vk::Format::eBc1RgbaUnormBlock,
	    .type   = vk::ImageType::e2D,
	    .tiling = vk::ImageTiling::eOptimal,
	    .usage  = vk::ImageUsageFlagBits::eSampled,
	    .flags  = vk::ImageCreateFlagBits::eBlockTexelViewCompatible,
	};
	facts.block_texel_view =
	    device.getImageFormatProperties2(block_texel_view_info).result == vk::Result::eSuccess;

	facts.color_write_enable          = color_write.colorWriteEnable == VK_TRUE;
	facts.packed_scaled_vertex_format =
	    static_cast<bool>(device.getFormatProperties(vk::Format::eA2B10G10R10UscaledPack32)
	                          .bufferFeatures &
	                      vk::FormatFeatureFlagBits::eVertexBuffer);
	facts.depth_clip_control          = depth_clip_control.depthClipControl == VK_TRUE;
	facts.depth_clip_enable           = depth_clip_enable.depthClipEnable == VK_TRUE;
	facts.image_view_min_lod          = min_lod.minLod == VK_TRUE;
	facts.fragment_shader_barycentric = barycentric.fragmentShaderBarycentric == VK_TRUE;
	facts.workgroup_memory_explicit_layout =
	    workgroup_layout.workgroupMemoryExplicitLayout == VK_TRUE;
	facts.shader_image_int64_atomics = image_atomic_int64.shaderImageInt64Atomics == VK_TRUE;
	facts.mesh_shader                = mesh.meshShader == VK_TRUE;
	facts.compute_derivative_group_quads = derivatives.computeDerivativeGroupQuads == VK_TRUE;
	facts.provoking_vertex_last          = provoking_vertex.provokingVertexLast == VK_TRUE;
	facts.maintenance8                   = maintenance8.maintenance8 == VK_TRUE;
	facts.attachment_feedback_loop_layout =
	    feedback_layout.attachmentFeedbackLoopLayout == VK_TRUE;
	facts.attachment_feedback_loop_dynamic_state =
	    feedback_dynamic.attachmentFeedbackLoopDynamicState == VK_TRUE;
	return facts;
}

static void VulkanFindPhysicalDevice(vk::Instance instance, vk::SurfaceKHR surface,
                                     const std::vector<const char*>& device_extensions,
                                     SurfaceCapabilities&            out_capabilities,
                                     vk::PhysicalDevice& out_device, uint32_t& out_queue_family,
                                     DeviceDecision& out_decision, std::string& out_rejections) {
	EXIT_IF(instance == nullptr);
	EXIT_IF(surface == nullptr);

	auto devices = EnumerateVulkan<vk::PhysicalDevice>(
	    "vkEnumeratePhysicalDevices", [&](uint32_t* count, vk::PhysicalDevice* values) {
		    return instance.enumeratePhysicalDevices(count, values);
	    });
	EXIT_NOT_IMPLEMENTED(devices.empty());

	if (Config::GetGpuIndex() >= 0) {
		if (static_cast<size_t>(Config::GetGpuIndex()) < devices.size()) {
			devices = {devices[Config::GetGpuIndex()]};
		} else {
			LOGF("Vulkan GPU index %d is unavailable; selecting automatically\n",
			     Config::GetGpuIndex());
		}
	} else if (vk::PhysicalDevice headset; Vr::OpenXrPhysicalDevice(instance, headset)) {
		devices = {headset}; // The headset runtime renders on this GPU.
	}

	vk::PhysicalDevice  best_device       = nullptr;
	uint32_t            best_queue_family = static_cast<uint32_t>(-1);
	SurfaceCapabilities best_capabilities;
	DeviceDecision      best_decision;

	const std::vector<std::string> required_extensions(device_extensions.begin(),
	                                                   device_extensions.end());

	for (const auto& device: devices) {
		bool skip_device = false;

		vk::PhysicalDeviceProperties device_properties {};
		device.getProperties(&device_properties);

		const auto reject = [&](const std::string& reason) {
			LOGF("%s\n", reason.c_str());
			out_rejections +=
			    skip_device ? "; " : fmt::format("\n  {}: ", device_properties.deviceName.data());
			out_rejections += reason;
			skip_device = true;
		};

		LOGF("Vulkan device: %s\n", device_properties.deviceName.data());
		if (device_properties.apiVersion < VULKAN_TARGET_API_VERSION) {
			reject(fmt::format("Vulkan {}.{} is required, but device supports only {}.{}.{}",
			                   VK_VERSION_MAJOR(VULKAN_TARGET_API_VERSION),
			                   VK_VERSION_MINOR(VULKAN_TARGET_API_VERSION),
			                   VK_VERSION_MAJOR(device_properties.apiVersion),
			                   VK_VERSION_MINOR(device_properties.apiVersion),
			                   VK_VERSION_PATCH(device_properties.apiVersion)));
			continue;
		}

		const auto queue_family = VulkanFindQueueFamily(device, surface);
		if (queue_family == static_cast<uint32_t>(-1)) {
			reject("No universal graphics, compute, and presentation queue");
		}

		auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
		    "vkEnumerateDeviceExtensionProperties",
		    [&](uint32_t* count, vk::ExtensionProperties* values) {
			    return device.enumerateDeviceExtensionProperties(nullptr, count, values);
		    });
		EXIT_NOT_IMPLEMENTED(available_extensions.empty());

		auto decision = EvaluateDeviceSuitability(
		    WindowContext::ReadDeviceFacts(device, available_extensions), required_extensions);
		for (const auto& reason: decision.rejections) {
			reject(reason);
		}
		if (skip_device) {
			for (const auto& ext: available_extensions) {
				LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName.data(),
				     ext.specVersion);
			}
		}

		SurfaceCapabilities candidate_capabilities;
		if (!skip_device) {
			GetSurfaceCapabilities(device, surface, candidate_capabilities);

			if (!(candidate_capabilities.capabilities.supportedUsageFlags &
			      vk::ImageUsageFlagBits::eTransferDst)) {
				reject("Surface cannot be destination of blit");
			}
		}

		const auto check_format = [&](vk::Format format, vk::FormatFeatureFlags features,
		                              const char* usage) {
			if (!skip_device && !CheckFormat(device, format, true, features)) {
				reject(fmt::format("Format vk::Format::e{} cannot be used as {}",
				                   vk::to_string(format), usage));
			}
		};

		const auto depth_features = vk::FormatFeatureFlagBits::eDepthStencilAttachment;
		const auto texture_features =
		    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst;
		const auto storage_features =
		    vk::FormatFeatureFlagBits::eStorageImage | vk::FormatFeatureFlagBits::eTransferDst;

		check_format(vk::Format::eD32Sfloat, depth_features, "depth buffer");
		check_format(vk::Format::eD32SfloatS8Uint, depth_features, "depth buffer");
		check_format(vk::Format::eD16Unorm, depth_features, "depth buffer");
		check_format(vk::Format::eBc3SrgbBlock, texture_features, "texture");
		check_format(vk::Format::eR8G8B8A8Srgb, texture_features, "texture");
		check_format(vk::Format::eR8Unorm, texture_features, "texture");
		check_format(vk::Format::eR8G8Unorm, texture_features, "texture");

		if (!skip_device &&
		    !CheckFormat(device, vk::Format::eR8G8B8A8Srgb, true, storage_features)) {
			LOGF("Format vk::Format::eR8G8B8A8Srgb cannot be used as storage image\n");
			check_format(vk::Format::eR8G8B8A8Unorm, storage_features, "storage image");
		}

		if (!skip_device &&
		    !CheckFormat(device, vk::Format::eB8G8R8A8Srgb, true, storage_features)) {
			LOGF("Format vk::Format::eB8G8R8A8Srgb cannot be used as storage image\n");
			check_format(vk::Format::eB8G8R8A8Unorm, storage_features, "storage image");
		}

		if (!skip_device && device_properties.limits.maxSamplerAnisotropy < 16.0f) {
			reject("maxSamplerAnisotropy < 16.0f");
		}

		if (skip_device) {
			continue;
		}

		if (best_device == nullptr ||
		    device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
			best_device       = device;
			best_queue_family = queue_family;
			best_capabilities = std::move(candidate_capabilities);
			best_decision     = std::move(decision);
		}
	}

	out_device       = best_device;
	out_queue_family = best_queue_family;
	if (best_device != nullptr) {
		out_capabilities = std::move(best_capabilities);
		out_decision     = std::move(best_decision);
	}
}

static vk::Device VulkanCreateDevice(GraphicContext& graphics,
	                                 const std::vector<const char*>& device_extensions) {
	const auto physical_device = graphics.physical_device;
	const auto queue_family    = graphics.queue_family;
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(queue_family == static_cast<uint32_t>(-1));

	const float               queue_priority = 1.0f;
	vk::DeviceQueueCreateInfo queue_create_info {};
	queue_create_info.queueFamilyIndex = queue_family;
	queue_create_info.queueCount       = 1;
	queue_create_info.pQueuePriorities = &queue_priority;
	// The emulator's work is heavy and long (a frame of a big game can keep the GPU busy for
	// seconds): at a low global priority the desktop's own GPU work goes first, and the screen stays
	// usable. KYTY_GPU_PRIORITY=normal leaves the queue at the default.
	vk::DeviceQueueGlobalPriorityCreateInfoKHR queue_global_priority {};
	if (graphics.low_queue_priority) {
		queue_global_priority.globalPriority = vk::QueueGlobalPriorityKHR::eLow;
		queue_create_info.pNext              = &queue_global_priority;
	}

	// An optional capability is on only when its extension is among the enabled ones.
	auto& caps = graphics.host.capabilities;
	caps.mesh_shader =
	    caps.mesh_shader && HasExtension(device_extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
	caps.image_int64_atomics =
	    caps.image_int64_atomics &&
	    HasExtension(device_extensions, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);

	// The feature struct of an optional extension is chained only when the capability is on.
	void*                                         optional_features = nullptr;
	vk::PhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
	if (caps.color_write_enable) {
		color_write_ext.colorWriteEnable = VK_TRUE;
		color_write_ext.pNext            = optional_features;
		optional_features                = &color_write_ext;
	}
	vk::PhysicalDeviceDepthClipEnableFeaturesEXT depth_clip_enable {};
	if (caps.depth_clip_enable) {
		depth_clip_enable.depthClipEnable = VK_TRUE;
		depth_clip_enable.pNext           = optional_features;
		optional_features                 = &depth_clip_enable;
	}
	vk::PhysicalDeviceImageViewMinLodFeaturesEXT image_view_min_lod {};
	if (caps.image_view_min_lod) {
		image_view_min_lod.minLod = VK_TRUE;
		image_view_min_lod.pNext  = optional_features;
		optional_features         = &image_view_min_lod;
	}
	vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR compute_derivatives {};
	if (caps.compute_derivatives) {
		compute_derivatives.computeDerivativeGroupQuads = VK_TRUE;
		compute_derivatives.pNext                       = optional_features;
		optional_features                               = &compute_derivatives;
	}
	vk::PhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
	depth_clip_control.pNext            = optional_features;
	depth_clip_control.depthClipControl = VK_TRUE;

	const bool workgroup_layout_extension =
	    HasExtension(device_extensions, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR supported_workgroup_layout {};
	vk::PhysicalDeviceVulkan12Features supported_features12 {};
	supported_features12.pNext = workgroup_layout_extension ? &supported_workgroup_layout : nullptr;
	vk::PhysicalDeviceVulkan13Features supported_features13 {};
	supported_features13.pNext = &supported_features12;

	const auto robustness2_ext_enabled =
	    HasExtension(device_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

	vk::PhysicalDeviceRobustness2FeaturesEXT supported_robustness2 {};
	if (robustness2_ext_enabled) {
		supported_robustness2.pNext = supported_features13.pNext;
		supported_features13.pNext = &supported_robustness2;
	}

	const bool mesh_extension = HasExtension(device_extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
	vk::PhysicalDeviceMeshShaderFeaturesEXT supported_mesh {};
	supported_mesh.pNext = &supported_features13;
	vk::PhysicalDeviceFeatures2 supported_features2 {};
	supported_features2.pNext = mesh_extension ? static_cast<void*>(&supported_mesh)
	                                           : static_cast<void*>(&supported_features13);
	const bool feedback_extensions =
	    HasExtension(device_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
	    HasExtension(device_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
	vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback_layout {};
	vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT feedback_dynamic {};
	if (feedback_extensions) {
		feedback_dynamic.pNext = supported_features2.pNext;
		feedback_layout.pNext  = &feedback_dynamic;
		supported_features2.pNext = &feedback_layout;
	}
	const bool provoking_extension =
	    HasExtension(device_extensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
	vk::PhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex {};
	if (provoking_extension) {
		provoking_vertex.pNext = supported_features2.pNext;
		supported_features2.pNext = &provoking_vertex;
	}
	const bool maintenance8_extension =
	    HasExtension(device_extensions, VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
	vk::PhysicalDeviceMaintenance8FeaturesKHR maintenance8 {};
	if (maintenance8_extension) {
		maintenance8.pNext = supported_features2.pNext;
		supported_features2.pNext = &maintenance8;
	}
	const bool shader_clock_extension =
	    HasExtension(device_extensions, VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
	vk::PhysicalDeviceShaderClockFeaturesKHR shader_clock {};
	if (shader_clock_extension) {
		shader_clock.pNext = supported_features2.pNext;
		supported_features2.pNext = &shader_clock;
	}
	const bool image_atomic_int64_extension =
	    HasExtension(device_extensions, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
	vk::PhysicalDeviceShaderImageAtomicInt64FeaturesEXT image_atomic_int64 {};
	if (image_atomic_int64_extension) {
		image_atomic_int64.pNext = supported_features2.pNext;
		supported_features2.pNext = &image_atomic_int64;
	}
	const bool conditional_rendering_extension =
	    HasExtension(device_extensions, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
	vk::PhysicalDeviceConditionalRenderingFeaturesEXT conditional_rendering {};
	if (conditional_rendering_extension) {
		conditional_rendering.pNext = supported_features2.pNext;
		supported_features2.pNext   = &conditional_rendering;
	}
	physical_device.getFeatures2(&supported_features2);

	auto features12 = WindowContext::RequiredVulkan12Features();
	features12.shaderBufferInt64Atomics = caps.buffer_int64_atomics ? VK_TRUE : VK_FALSE;
	features12.shaderSharedInt64Atomics = caps.shared_int64_atomics ? VK_TRUE : VK_FALSE;
	features12.shaderFloat16            = caps.float16 ? VK_TRUE : VK_FALSE;
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR workgroup_layout {};
	workgroup_layout.workgroupMemoryExplicitLayout =
	    supported_workgroup_layout.workgroupMemoryExplicitLayout;
	workgroup_layout.pNext = &depth_clip_control;
	features12.pNext = workgroup_layout_extension ? static_cast<void*>(&workgroup_layout)
	                                             : static_cast<void*>(&depth_clip_control);
	auto features11 = WindowContext::RequiredVulkan11Features();
	features11.pNext = features12.pNext;
	features12.pNext = &features11;
	vk::PhysicalDeviceSubgroupSizeControlProperties subgroup_size_control {};

	vk::PhysicalDeviceVulkan11Properties properties11 {};
	properties11.pNext = &subgroup_size_control;

	vk::PhysicalDeviceFloatControlsProperties float_controls {};
	float_controls.pNext = &properties11;
	vk::PhysicalDeviceProperties2 properties2 {};
	properties2.pNext = &float_controls;

	if (caps.mesh_shader) {
		subgroup_size_control.pNext = &graphics.mesh_shader_properties;
	}
	physical_device.getProperties2(&properties2);
	EXIT_NOT_IMPLEMENTED(!(properties11.subgroupSupportedOperations & vk::SubgroupFeatureFlagBits::eVote));

	graphics.subgroup_size                 = properties11.subgroupSize;
	graphics.min_subgroup_size             = subgroup_size_control.minSubgroupSize;
	graphics.max_subgroup_size             = subgroup_size_control.maxSubgroupSize;
	graphics.required_subgroup_size_stages = subgroup_size_control.requiredSubgroupSizeStages;

	LOGF("Vulkan subgroup: default=%u min=%u max=%u stages=0x%08x size_control=%s wave64=%s\n",
	     graphics.subgroup_size, graphics.min_subgroup_size, graphics.max_subgroup_size,
	     static_cast<vk::ShaderStageFlags::MaskType>(graphics.required_subgroup_size_stages),
	     graphics.host.capabilities.compute_subgroup_size_control ? "true" : "false",
	     graphics.SupportsComputeWave64() ? "true" : "false");
	// Decided by the device decision; on only when the extensions are among the enabled ones.
	caps.provoking_vertex_last    = caps.provoking_vertex_last && provoking_extension;
	caps.attachment_feedback_loop = caps.attachment_feedback_loop && feedback_extensions;
	caps.maintenance8             = caps.maintenance8 && maintenance8_extension;
	// The device clock ticks at timestampPeriod nanoseconds.
	if (shader_clock_extension && shader_clock.shaderDeviceClock) {
		caps.device_clock_period_ps = static_cast<uint32_t>(
		    std::lround(graphics.physical_device_properties.limits.timestampPeriod * 1000.0f));
	}
	graphics.device_clock_period_ps = caps.device_clock_period_ps;
	graphics.conditional_rendering_enabled =
	    conditional_rendering_extension && conditional_rendering.conditionalRendering;
	LOGF("Vulkan native FP16: %s\n", graphics.host.capabilities.float16 ? "true" : "false");
	LOGF("Vulkan depth feedback support: %s\n",
	     graphics.host.capabilities.attachment_feedback_loop ? "true" : "false");
	if (caps.mesh_shader) {
		LOGF("Vulkan MeshEXT: invocations=%u vertices=%u primitives=%u shared=%u\n",
		     graphics.mesh_shader_properties.maxMeshWorkGroupInvocations,
		     graphics.mesh_shader_properties.maxMeshOutputVertices,
		     graphics.mesh_shader_properties.maxMeshOutputPrimitives,
		     graphics.mesh_shader_properties.maxMeshSharedMemorySize);
	}
	vk::PhysicalDeviceFeatures device_features {};
	device_features.fragmentStoresAndAtomics = VK_TRUE;
	device_features.samplerAnisotropy        = VK_TRUE;
	device_features.robustBufferAccess       = VK_TRUE;
	device_features.pipelineStatisticsQuery  = supported_features2.features.pipelineStatisticsQuery;
	device_features.depthBounds                           = caps.depth_bounds ? VK_TRUE : VK_FALSE;
	device_features.depthClamp                            = caps.depth_clamp ? VK_TRUE : VK_FALSE;
	device_features.drawIndirectFirstInstance = caps.draw_indirect_first_instance ? VK_TRUE : VK_FALSE;
	device_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
	device_features.shaderStorageImageMultisample        =
	    supported_features2.features.shaderStorageImageMultisample;
	device_features.shaderImageGatherExtended            = VK_TRUE;
	device_features.shaderResourceMinLod                 = VK_TRUE;
	device_features.independentBlend                     = VK_TRUE;
	device_features.dualSrcBlend                         = VK_TRUE;
	device_features.tessellationShader                   = VK_TRUE;
	device_features.drawIndirectFirstInstance            = VK_TRUE;
	device_features.multiDrawIndirect                    = VK_TRUE;
	device_features.sampleRateShading                    = VK_TRUE;
	device_features.depthBiasClamp                       = VK_TRUE;
	device_features.shaderClipDistance                   = VK_TRUE;
	device_features.shaderCullDistance                    = caps.cull_distance ? VK_TRUE : VK_FALSE;
	device_features.largePoints                          = VK_TRUE;
	device_features.multiViewport                        = VK_TRUE;
	device_features.fillModeNonSolid                      = VK_TRUE;
	device_features.vertexPipelineStoresAndAtomics       = VK_TRUE;
	// Storage image sample counts beyond 1x require the feature.
	if (!device_features.shaderStorageImageMultisample) {
		graphics.physical_device_properties.limits.storageImageSampleCounts =
		    vk::SampleCountFlagBits::e1;
	}
	graphics.sample_rate_shading_enabled                 = true;
	// Occlusion queries reset their queries from the host and count samples exactly where they can.
	graphics.occlusion_queries_enabled =
	    Config::OcclusionQueriesEnabled() && supported_features12.hostQueryReset;
	graphics.occlusion_query_precise_enabled =
	    graphics.occlusion_queries_enabled && supported_features2.features.occlusionQueryPrecise;
	features12.hostQueryReset             = graphics.occlusion_queries_enabled;
	device_features.occlusionQueryPrecise = graphics.occlusion_query_precise_enabled;
	LOGF("Vulkan occlusion queries: %s, precise: %s\n",
	     graphics.occlusion_queries_enabled ? "true" : "false",
	     graphics.occlusion_query_precise_enabled ? "true" : "false");
	device_features.shaderInt64 = VK_TRUE;
	device_features.shaderFloat64                         = caps.float64 ? VK_TRUE : VK_FALSE;

	vk::PhysicalDeviceRobustness2FeaturesEXT robustness2 {};
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR fragment_barycentric {};
	void*                                                  below_robustness = &features12;
	if (caps.fragment_barycentric) {
		fragment_barycentric.pNext                     = &features12;
		fragment_barycentric.fragmentShaderBarycentric = VK_TRUE;
		below_robustness                               = &fragment_barycentric;
	}
	robustness2.pNext = below_robustness;
	if (robustness2_ext_enabled) {
		robustness2.robustBufferAccess2 = supported_robustness2.robustBufferAccess2;
		robustness2.robustImageAccess2  = supported_robustness2.robustImageAccess2;
		robustness2.nullDescriptor      = supported_robustness2.nullDescriptor;
	}
	graphics.null_descriptor_enabled = robustness2_ext_enabled && robustness2.nullDescriptor == VK_TRUE;

	auto features13 = WindowContext::RequiredVulkan13Features();
	features13.pNext =
	    robustness2_ext_enabled ? static_cast<void*>(&robustness2) : below_robustness;
	features13.robustImageAccess   = supported_features13.robustImageAccess;
	features13.subgroupSizeControl =
	    graphics.host.capabilities.compute_subgroup_size_control ? VK_TRUE : VK_FALSE;

	LOGF("Vulkan robustness: robustImageAccess=%s robustImageAccess2=%s\n",
	     features13.robustImageAccess == VK_TRUE ? "true" : "false",
	     robustness2_ext_enabled && robustness2.robustImageAccess2 == VK_TRUE ? "true" : "false");

	vk::DeviceCreateInfo create_info {};
	vk::PhysicalDeviceMeshShaderFeaturesEXT mesh_features {};
	mesh_features.pNext                 = &features13;
	mesh_features.meshShader            = caps.mesh_shader ? VK_TRUE : VK_FALSE;
	feedback_dynamic.pNext =
	    mesh_extension ? static_cast<void*>(&mesh_features) : static_cast<void*>(&features13);
	create_info.pNext = graphics.host.capabilities.attachment_feedback_loop
	                        ? static_cast<void*>(&feedback_layout)
	                        : feedback_dynamic.pNext;
	if (graphics.host.capabilities.provoking_vertex_last) {
		provoking_vertex.pNext = const_cast<void*>(create_info.pNext);
		provoking_vertex.transformFeedbackPreservesProvokingVertex = VK_FALSE;
		create_info.pNext = &provoking_vertex;
	}
	if (caps.image_int64_atomics) {
		image_atomic_int64.pNext = const_cast<void*>(create_info.pNext);
		image_atomic_int64.sparseImageInt64Atomics = VK_FALSE;
		create_info.pNext = &image_atomic_int64;
	}
	if (caps.maintenance8) {
		maintenance8.pNext = const_cast<void*>(create_info.pNext);
		create_info.pNext = &maintenance8;
	}
	vk::PhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipeline_executable {};
	if (graphics.pipeline_executable_info_enabled) {
		pipeline_executable.pNext                  = const_cast<void*>(create_info.pNext);
		pipeline_executable.pipelineExecutableInfo = VK_TRUE;
		create_info.pNext                          = &pipeline_executable;
	}
	if (graphics.device_clock_period_ps != 0u) {
		shader_clock.pNext = const_cast<void*>(create_info.pNext);
		create_info.pNext = &shader_clock;
	}
	if (graphics.conditional_rendering_enabled) {
		conditional_rendering.pNext = const_cast<void*>(create_info.pNext);
		create_info.pNext = &conditional_rendering;
	}
	// The renderer only uses the Vulkan 1.3 core dynamic states, but RADV (Mesa 26.3) creates its
	// PS epilog cache only when this EXT feature is enabled and crashes in vkCmdDraw when a
	// depth- or stencil-writing fragment shader needs an epilog.
	vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT extended_dynamic_state {};
	extended_dynamic_state.extendedDynamicState = VK_TRUE;
	if (HasExtension(device_extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)) {
		extended_dynamic_state.pNext = const_cast<void*>(create_info.pNext);
		create_info.pNext = &extended_dynamic_state;
	}
	create_info.pQueueCreateInfos       = &queue_create_info;
	create_info.queueCreateInfoCount    = 1;
	create_info.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
	create_info.ppEnabledExtensionNames = device_extensions.data();
	create_info.pEnabledFeatures        = &device_features;

	vk::Device device = nullptr;

	auto result = physical_device.createDevice(&create_info, nullptr, &device);
	if (result != vk::Result::eSuccess && graphics.low_queue_priority) {
		// The host refuses the low priority (or the extension): the default one.
		LOGF("vkCreateDevice with a low queue priority failed: %s; trying the default\n",
		     vk::to_string(result).c_str());
		graphics.low_queue_priority = false;
		queue_create_info.pNext     = nullptr;
		std::vector<const char*> without;
		for (const auto* extension: device_extensions) {
			if (std::string_view(extension) != VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) without.push_back(extension);
		}
		create_info.enabledExtensionCount   = static_cast<uint32_t>(without.size());
		create_info.ppEnabledExtensionNames = without.data();
		result = physical_device.createDevice(&create_info, nullptr, &device);
	}
	if (result != vk::Result::eSuccess) {
		LOGF("vkCreateDevice failed: %s\n", vk::to_string(result).c_str());
		return nullptr;
	}

	return device;
}

static void VulkanGetExtensions(VulkanExtensions& r) {
	uint32_t required_extensions_count = 0;

	const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&required_extensions_count);
	EXIT_NOT_IMPLEMENTED(extensions == nullptr);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);
	r.required_extensions.assign(extensions, extensions + required_extensions_count);

	r.available_extensions =
	    EnumerateVulkan<vk::ExtensionProperties>( // @suppress("Ambiguous problem")
	        "vkEnumerateInstanceExtensionProperties",
	        [](uint32_t* count, vk::ExtensionProperties* values) {
		        return vk::enumerateInstanceExtensionProperties(nullptr, count, values);
	        });

	r.enable_validation_layers = Config::VulkanValidationEnabled();
	if (Config::HdrEnabled() &&
	    HasExtension(r.available_extensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) &&
	    !HasExtension(r.required_extensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
		r.required_extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
	}

	if (HasExtension(r.available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
		r.required_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	} else {
		r.enable_validation_layers = false;
	}

	for (const char* ext: r.required_extensions) {
		LOGF("Vulkan required extension: %s\n", ext);
	}

	for (const auto& ext: r.available_extensions) {
		LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName.data(),
		     ext.specVersion);
	}

	r.available_layers = EnumerateVulkan<vk::LayerProperties>( // @suppress("Ambiguous problem")
	    "vkEnumerateInstanceLayerProperties", [](uint32_t* count, vk::LayerProperties* values) {
		    return vk::enumerateInstanceLayerProperties(count, values);
	    });

	for (const auto& l: r.available_layers) {
		LOGF("Vulkan available layer: %s, specVersion = %u, implVersion = %u, %s\n",
		     l.layerName.data(), l.specVersion, l.implementationVersion, l.description.data());
	}

	r.required_layers = {"VK_LAYER_KHRONOS_validation"};

	if (r.enable_validation_layers) {
		for (const char* l: r.required_layers) {
			if (!HasLayer(r.available_layers, l)) {
				LOGF("no validation layer: %s\n", l);
				r.enable_validation_layers = false;
				break;
			}
		}
	}

	if (r.enable_validation_layers) {
		auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
		    "vkEnumerateInstanceExtensionProperties",
		    [](uint32_t* count, vk::ExtensionProperties* values) {
			    return vk::enumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
			                                                    count, values);
		    });

		for (const auto& ext: available_extensions) {
			LOGF("VK_LAYER_KHRONOS_validation available extension: %s, version = %u\n",
			     ext.extensionName.data(), ext.specVersion);
		}

		if (HasExtension(available_extensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
			r.required_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		} else {
			r.enable_validation_layers = false;
		}
	}
}

static VKAPI_ATTR vk::Bool32 VKAPI_CALL VulkanDebugMessengerCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    vk::DebugUtilsMessageTypeFlagsEXT             message_types,
    const vk::DebugUtilsMessengerCallbackDataEXT* callback_data, void* /*user_data*/) {
	EXIT_IF(callback_data == nullptr);
	EXIT_IF(callback_data->pMessage == nullptr);

	const char*     severity_str   = nullptr;
	fmt::text_style severity_style = Log::Color::Default;
	bool            skip           = false;
	bool            error          = false;
	bool            debug_printf   = false;
	switch (message_severity) {
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
			severity_str   = "V";
			severity_style = Log::Color::BrightWhite;
			skip           = true;
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
			if ((message_types & vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation) &&
			    Config::SpirvDebugPrintfEnabled() && callback_data->pMessageIdName != nullptr &&
			    strcmp(callback_data->pMessageIdName, "UNASSIGNED-DEBUG-PRINTF") == 0) {
				debug_printf   = true;
				severity_style = Log::Color::BrightYellow;
				skip           = true;
			} else {
				severity_str   = "I";
				severity_style = Log::Color::Default;
				skip           = true;
			}
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
			severity_str   = "W";
			severity_style = Log::Color::Red;
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
			severity_str   = "E";
			severity_style = Log::Color::BrightRed;
			// Only validation errors are fatal; GENERAL-type errors can come
			// from unrelated loader/layer issues (e.g. a broken overlay).
			error = static_cast<bool>(message_types &
			                          vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
			break;
		default: severity_str = "?";
	}

	if ((error || !skip) && Trace::Enabled()) {
		// A traced run collects every message instead of stopping at the first error.
		Trace::Note(error ? "vk_validation_error" : "vk_validation_warning",
		            callback_data->pMessageIdName != nullptr ? callback_data->pMessageIdName : "?",
		            callback_data->pMessage);
		error = false;
	}
	if (error) {
		EXIT_COLOR(severity_style, "[Vulkan][%s][%u]: %s\n", severity_str,
		           static_cast<uint32_t>(message_types), callback_data->pMessage);
	}

	if (!skip) {
		LOGF_COLOR(severity_style, "[Vulkan][%s][%u]: %s\n", severity_str,
		           static_cast<uint32_t>(message_types), callback_data->pMessage);
	}

	if (debug_printf) {
		auto strs = Common::Split(std::string(callback_data->pMessage), '|');
		if (!strs.empty()) {
			LOGF_COLOR(severity_style, "%s\n", strs[strs.size() - 1].c_str());
		}
	}

	return VK_FALSE;
}

static VKAPI_ATTR vk::Result VKAPI_CALL VulkanCreateDebugUtilsMessengerEXT(
    vk::Instance instance, const vk::DebugUtilsMessengerCreateInfoEXT* create_info,
    const vk::AllocationCallbacks* allocator, vk::DebugUtilsMessengerEXT* messenger) {
	EXIT_IF(instance == nullptr);

	if (auto func = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDebugUtilsMessengerEXT; func != nullptr) {
		return instance.createDebugUtilsMessengerEXT(create_info, allocator, messenger);
	}
	return vk::Result::eErrorExtensionNotPresent;
}

static void VulkanCheckInstanceVersion() {
	uint32_t version = VK_API_VERSION_1_0;

	if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion != nullptr) {
		auto result = vk::enumerateInstanceVersion(&version);
		if (result != vk::Result::eSuccess) {
			EXIT("Could not query Vulkan loader version: %s\n", vk::to_string(result).c_str());
		}
	}

	LOGF("Vulkan loader version: %u.%u.%u\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
	     VK_VERSION_PATCH(version));
	if (version < VULKAN_TARGET_API_VERSION) {
		EXIT("Vulkan %u.%u is required, but loader supports only %u.%u.%u\n",
		     VK_VERSION_MAJOR(VULKAN_TARGET_API_VERSION),
		     VK_VERSION_MINOR(VULKAN_TARGET_API_VERSION), VK_VERSION_MAJOR(version),
		     VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
	}
}

void WindowContext::CreateVulkan() {
	EXIT_IF(window == nullptr);
	EXIT_IF(graphic_ctx.instance != nullptr);
	EXIT_IF(graphic_ctx.physical_device != nullptr);
	EXIT_IF(graphic_ctx.device != nullptr);
	EXIT_IF(surface != nullptr);

	auto get_instance_proc_addr =
	    reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
	if (get_instance_proc_addr == nullptr) {
		EXIT("Could not load Vulkan: %s\n", SDL_GetError());
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

	Vr::OpenXrInit();

	VulkanExtensions r;
	VulkanGetExtensions(r);
	Vr::OpenXrAppendExtensions(r.required_extensions, r.available_extensions, false);
	VulkanCheckInstanceVersion();

	vk::ApplicationInfo app_info {};
	app_info.pApplicationName   = "Kyty";
	app_info.applicationVersion = 1;
	app_info.pEngineName        = "Kyty";
	app_info.engineVersion      = 1;
	app_info.apiVersion         = VULKAN_TARGET_API_VERSION; // NOLINT

	if (Config::SpirvDebugPrintfEnabled() && Config::GpuAssistedValidationEnabled()) {
		EXIT("--spirv-debug-printf and --gpu-assisted-validation are mutually exclusive\n");
	}

	vk::ValidationFeatureEnableEXT enabled_features[3]    = {};
	uint32_t                       enabled_features_count = 0;
#ifdef KYTY_ENABLE_BEST_PRACTICES
	enabled_features[enabled_features_count++] = vk::ValidationFeatureEnableEXT::eBestPractices;
#endif
#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled()) {
		enabled_features[enabled_features_count++] = vk::ValidationFeatureEnableEXT::eDebugPrintf;
	}
#endif
	if (Config::GpuAssistedValidationEnabled()) {
		enabled_features[enabled_features_count++] = vk::ValidationFeatureEnableEXT::eGpuAssisted;
		LOGF("Vulkan GPU-assisted validation is enabled; expect a large slowdown\n");
		std::printf("Vulkan GPU-assisted validation is enabled; expect a large slowdown\n");
		std::fflush(stdout);
	}

	vk::ValidationFeaturesEXT validation_features {};
	validation_features.enabledValidationFeatureCount  = enabled_features_count;
	validation_features.pEnabledValidationFeatures     = enabled_features;

	vk::DebugUtilsMessengerCreateInfoEXT dbg_create_info {};
	dbg_create_info.pNext           = &validation_features;
	dbg_create_info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
	dbg_create_info.messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
	                                  vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
	                                  vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
	dbg_create_info.pfnUserCallback = VulkanDebugMessengerCallback;

	vk::InstanceCreateInfo inst_info {};
	inst_info.pNext                   = (r.enable_validation_layers ? &dbg_create_info : nullptr);
	// A portability driver such as MoltenVK shows its devices only when this extension and its
	// flag are on.
	if (HasExtension(r.available_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		if (!HasExtension(r.required_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
			r.required_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		}
		inst_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
		LOGF("Vulkan instance: enabled %s\n", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	}
	// MoltenVK turns the Vulkan commands of a command buffer into Metal commands when the buffer
	// is submitted, by default on the thread that submits. That thread is the GPU thread, which
	// has the next frame to record. Let MoltenVK do it on its own queue thread instead. The
	// setting is addressed to MoltenVK by name; any other driver ignores it.
	// Pipelines are built on several worker threads. By default Metal compiles one shader at a
	// time for the whole process; this lets it compile as many at once as it has cores for.
	static const vk::Bool32   synchronous_queue_submits   = VK_FALSE;
	static const vk::Bool32   maximize_concurrent_compile = VK_TRUE;
	const vk::LayerSettingEXT layer_settings[]            = {
	    {"MoltenVK", "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", vk::LayerSettingTypeEXT::eBool32, 1,
	     &synchronous_queue_submits},
	    {"MoltenVK", "MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION",
	     vk::LayerSettingTypeEXT::eBool32, 1, &maximize_concurrent_compile},
	};
	vk::LayerSettingsCreateInfoEXT layer_settings_info {};
	if (HasExtension(r.available_extensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
		if (!HasExtension(r.required_extensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
			r.required_extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
		}
		layer_settings_info.settingCount = static_cast<uint32_t>(std::size(layer_settings));
		layer_settings_info.pSettings    = layer_settings;
		layer_settings_info.pNext        = inst_info.pNext;
		inst_info.pNext                  = &layer_settings_info;
		LOGF("Vulkan instance: enabled %s\n", VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
	}
	inst_info.pApplicationInfo        = &app_info;
	inst_info.enabledExtensionCount   = static_cast<uint32_t>(r.required_extensions.size());
	inst_info.ppEnabledExtensionNames = r.required_extensions.data();
	inst_info.enabledLayerCount =
	    (r.enable_validation_layers ? static_cast<uint32_t>(r.required_layers.size()) : 0);
	inst_info.ppEnabledLayerNames =
	    (r.enable_validation_layers ? r.required_layers.data() : nullptr);

	const vk::Result result = vk::createInstance(&inst_info, nullptr, &graphic_ctx.instance);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eErrorIncompatibleDriver:
			EXIT("Unable to find a compatible Vulkan Driver");
		default: EXIT("Could not create a Vulkan instance (for unknown reasons)");
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphic_ctx.instance);

	if (r.enable_validation_layers) {
		dbg_create_info.pNext = nullptr;
		if (VulkanCreateDebugUtilsMessengerEXT(graphic_ctx.instance, &dbg_create_info, nullptr,
		                                       &graphic_ctx.debug_messenger) !=
		    vk::Result::eSuccess) {
			EXIT("Could not create debug messenger");
		}
	}

	vk::SurfaceKHR::CType native_surface = VK_NULL_HANDLE;
	if (!SDL_Vulkan_CreateSurface(window, static_cast<vk::Instance::CType>(graphic_ctx.instance),
	                              nullptr, &native_surface)) {
		EXIT("Could not create a Vulkan surface");
	}
	surface = native_surface;

	// What no device can do without. Extensions of optional capabilities are added after the
	// device is chosen.
	std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	                                              VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
	                                              "VK_KHR_maintenance1"};

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled()) {
		device_extensions.push_back(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	}
#endif

	std::string rejected_devices;
	DeviceDecision decision;
	VulkanFindPhysicalDevice(graphic_ctx.instance, surface, device_extensions, surface_capabilities,
	                         graphic_ctx.physical_device, graphic_ctx.queue_family, decision,
	                         rejected_devices);

	if (graphic_ctx.physical_device == nullptr) {
		EXIT("Could not find suitable device:%s", rejected_devices.c_str());
	}

	vk::PhysicalDevicePushDescriptorProperties push_descriptor_properties {};
	vk::PhysicalDeviceProperties2              physical_device_properties {};
	physical_device_properties.pNext = &push_descriptor_properties;
	graphic_ctx.physical_device.getProperties2(&physical_device_properties);
	graphic_ctx.physical_device_properties = physical_device_properties.properties;
	graphic_ctx.max_push_descriptors       = push_descriptor_properties.maxPushDescriptors;
	graphic_ctx.physical_device.getMemoryProperties(&graphic_ctx.physical_device_memory_properties);
	const auto& device_properties = graphic_ctx.GetPhysicalDeviceProperties();

	LOGF("Select device: %s\n", device_properties.deviceName.data());

	graphic_ctx.host = {decision.capabilities, decision.faults};
	for (const auto& line: decision.unavailable) {
		Log::WriteToConsoleAndLog(fmt::format("Host GPU: {}\n", line));
	}


	{
		auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
		    "vkEnumerateDeviceExtensionProperties",
		    [&](uint32_t* count, vk::ExtensionProperties* values) {
			    return graphic_ctx.physical_device.enumerateDeviceExtensionProperties(
			        nullptr, count, values);
		    });

		const auto&                        caps                    = graphic_ctx.host.capabilities;
		const std::pair<bool, const char*> capability_extensions[] = {
		    {caps.push_descriptors, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME},
		    {caps.image_view_min_lod, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME},
		    {caps.depth_clip_enable, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME},
		    {caps.color_write_enable, VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME},
		    {caps.fragment_barycentric, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME},
		    {caps.compute_derivatives, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME},
		    {caps.maintenance8, VK_KHR_MAINTENANCE_8_EXTENSION_NAME},
		};
		for (const auto& [enabled, extension]: capability_extensions) {
			if (enabled) {
				device_extensions.push_back(extension);
			}
		}
		// The Vulkan specification requires this extension to be enabled when a device has it.
		if (HasExtension(available_extensions, "VK_KHR_portability_subset")) {
			device_extensions.push_back("VK_KHR_portability_subset");
		}

		if (const char* priority = std::getenv("KYTY_GPU_PRIORITY");
		    (priority == nullptr || std::string_view(priority) != "normal") &&
		    HasExtension(available_extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME)) {
			device_extensions.push_back(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);
			graphic_ctx.low_queue_priority = true;
		}
		if (std::getenv("KYTY_GPU_BREADCRUMBS") != nullptr &&
		    HasExtension(available_extensions, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) {
			device_extensions.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
			graphic_ctx.buffer_marker_enabled = true;
		}
		if (std::getenv("KYTY_PIPELINE_IR") != nullptr &&
		    HasExtension(available_extensions, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)) {
			device_extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
			graphic_ctx.pipeline_executable_info_enabled = true;
		}
		if (HasExtension(available_extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
			graphic_ctx.memory_budget_ext_enabled = true;
		}
		if (HasExtension(available_extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
		    HasExtension(available_extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
			device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
			device_extensions.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
			graphic_ctx.dma_buf_import_enabled = true;
		}
		for (const auto* extension: {VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
		                             VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME,
		                             VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
		                             VK_EXT_MESH_SHADER_EXTENSION_NAME,
		                             VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
		                             VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
		                             VK_KHR_SHADER_CLOCK_EXTENSION_NAME,
		                             VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME,
		                             VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME}) {
			if (HasExtension(available_extensions, extension)) {
				device_extensions.push_back(extension);
			}
		}
		if (HasExtension(available_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
		    HasExtension(available_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
			device_extensions.push_back(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
		}
		Vr::OpenXrAppendExtensions(device_extensions, available_extensions, true);
	}

	graphic_ctx.device = VulkanCreateDevice(graphic_ctx, device_extensions);
	if (graphic_ctx.device == nullptr) {
		EXIT("Could not create device");
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphic_ctx.device);
	graphic_ctx.device.getQueue(graphic_ctx.queue_family, 0, &graphic_ctx.queue);
	EXIT_IF(graphic_ctx.queue == nullptr);

	// The vertex subgroup operations the driver does not report work only after this device has
	// run them (see vertexSubgroupProbe.h).
	if (graphic_ctx.host.faults.vertex_subgroups_unreported) {
		const bool confirmed = ProbeVertexSubgroups(graphic_ctx);
		ApplyVertexSubgroupProbe(graphic_ctx.host, confirmed);
		Log::WriteToConsoleAndLog(fmt::format(
		    "Host GPU: vertex subgroup operations {}\n",
		    confirmed ? "passed their probe: a vertex wave is one SIMD group"
		              : "failed their probe: a vertex wave is one vertex"));
	}

	if (!graphic_ctx.CreateAllocator()) {
		EXIT("Could not create Vulkan memory allocator");
	}

	render_context = std::make_unique<RenderContext>(graphic_ctx);
	LibKernel::Memory::InstallGpuResources(render_context.get());
	presenter = std::make_unique<Presenter>(*this);
	Vr::OpenXrStartSession(graphic_ctx);
}

void WindowContext::RefreshSurfaceCapabilities() {
	EXIT_IF(graphic_ctx.physical_device == nullptr || surface == nullptr);
	GetSurfaceCapabilities(graphic_ctx.physical_device, surface, surface_capabilities);
}

void WindowContext::RecreateSurface() {
	EXIT_IF(window == nullptr || graphic_ctx.instance == nullptr);
	if (surface != nullptr) {
		graphic_ctx.instance.destroySurfaceKHR(surface, nullptr);
		surface = nullptr;
	}
	vk::SurfaceKHR::CType native_surface = VK_NULL_HANDLE;
	if (!SDL_Vulkan_CreateSurface(window, static_cast<vk::Instance::CType>(graphic_ctx.instance),
	                              nullptr, &native_surface)) {
		EXIT("Could not recreate the Vulkan surface: %s\n", SDL_GetError());
	}
	surface = native_surface;
}

WindowContext::~WindowContext() {
	Vr::OpenXrShutdown();
	ShutdownSystemOverlayInput();
	presenter.reset();
	LibKernel::Memory::InstallGpuResources(nullptr);
	render_context.reset();

	if (graphic_ctx.device != nullptr) {
		RequireVulkanSuccess(graphic_ctx.device.waitIdle(), "wait for Vulkan device shutdown");
		graphic_ctx.DestroyAllocator();
		graphic_ctx.device.destroy(nullptr);
		graphic_ctx.device = nullptr;
		graphic_ctx.queue  = nullptr;
	}
	if (surface != nullptr) {
		graphic_ctx.instance.destroySurfaceKHR(surface, nullptr);
		surface = nullptr;
	}
	if (graphic_ctx.debug_messenger != nullptr) {
		graphic_ctx.instance.destroyDebugUtilsMessengerEXT(graphic_ctx.debug_messenger, nullptr);
		graphic_ctx.debug_messenger = nullptr;
	}
	if (graphic_ctx.instance != nullptr) {
		graphic_ctx.instance.destroy(nullptr);
		graphic_ctx.instance        = nullptr;
		graphic_ctx.physical_device = nullptr;
	}
	if (window != nullptr) {
		SDL_DestroyWindow(window);
		window = nullptr;
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

} // namespace Libs::Graphics
