#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hints.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_pixels.h"
#include "SDL_rwops.h"
#include "SDL_stdinc.h"
#include "SDL_surface.h"
#include "SDL_thread.h"
#include "SDL_touch.h"
#include "SDL_video.h"
#include "SDL_vulkan.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/imeOverlay.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"
#include "kernel/memory.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

struct VulkanExtensions {
	bool enable_validation_layers = false;

	std::vector<const char*>             required_extensions;
	std::vector<vk::ExtensionProperties> available_extensions;
	std::vector<const char*>             required_layers;
	std::vector<vk::LayerProperties>     available_layers;
};

vk::PhysicalDeviceVulkan13Features WindowContext::RequiredVulkan13Features() noexcept {
	vk::PhysicalDeviceVulkan13Features features {};
	features.sType            = vk::StructureType::ePhysicalDeviceVulkan13Features;
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
		     VulkanToString(properties.queueFlags).c_str(), properties.queueCount,
		     (presentation_supported == VK_TRUE ? "true" : "false"));
		if (properties.queueCount != 0 && (properties.queueFlags & required) == required &&
		    presentation_supported == VK_TRUE) {
			LOGF("\tselected universal queue family %u\n", family);
			return family;
		}
	}
	return static_cast<uint32_t>(-1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void VulkanFindPhysicalDevice(vk::Instance instance, vk::SurfaceKHR surface,
                                     const std::vector<const char*>& device_extensions,
                                     SurfaceCapabilities&            out_capabilities,
                                     vk::PhysicalDevice& out_device, uint32_t& out_queue_family) {
	EXIT_IF(instance == nullptr);
	EXIT_IF(surface == nullptr);

	auto devices = EnumerateVulkan<vk::PhysicalDevice>(
	    "vkEnumeratePhysicalDevices", [&](uint32_t* count, vk::PhysicalDevice* values) {
		    return instance.enumeratePhysicalDevices(count, values);
	    });
	EXIT_NOT_IMPLEMENTED(devices.empty());

	vk::PhysicalDevice  best_device       = nullptr;
	uint32_t            best_queue_family = static_cast<uint32_t>(-1);
	SurfaceCapabilities best_capabilities;

	for (const auto& device: devices) {
		bool skip_device = false;

		vk::PhysicalDeviceProperties device_properties {};
		device.getProperties(&device_properties);

		LOGF("Vulkan device: %s\n", device_properties.deviceName.data());
		if (device_properties.apiVersion < VULKAN_TARGET_API_VERSION) {
			LOGF("Vulkan %u.%u is required, but device supports only %u.%u.%u\n",
			     VK_VERSION_MAJOR(VULKAN_TARGET_API_VERSION),
			     VK_VERSION_MINOR(VULKAN_TARGET_API_VERSION),
			     VK_VERSION_MAJOR(device_properties.apiVersion),
			     VK_VERSION_MINOR(device_properties.apiVersion),
			     VK_VERSION_PATCH(device_properties.apiVersion));
			continue;
		}

		vk::PhysicalDeviceFeatures2 device_features2 {};

		vk::PhysicalDeviceVulkan13Features features13 {};
		features13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;
		features13.pNext = nullptr;

		vk::PhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
		color_write_ext.sType = vk::StructureType::ePhysicalDeviceColorWriteEnableFeaturesEXT;
		color_write_ext.pNext = nullptr;

		vk::PhysicalDeviceDepthClipEnableFeaturesEXT depth_clip_enable {};
		depth_clip_enable.sType = vk::StructureType::ePhysicalDeviceDepthClipEnableFeaturesEXT;
		depth_clip_enable.pNext = &color_write_ext;

		vk::PhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
		depth_clip_control.sType = vk::StructureType::ePhysicalDeviceDepthClipControlFeaturesEXT;
		depth_clip_control.pNext = &depth_clip_enable;

		vk::PhysicalDeviceVulkan12Features features12 {};
		features12.sType = vk::StructureType::ePhysicalDeviceVulkan12Features;
#if defined(__APPLE__)
		features12.pNext = &depth_clip_control;
#else
		vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR fragment_barycentric {};
		fragment_barycentric.sType =
		    vk::StructureType::ePhysicalDeviceFragmentShaderBarycentricFeaturesKHR;
		fragment_barycentric.pNext = &depth_clip_control;
		features12.pNext           = &fragment_barycentric;
#endif
		features13.pNext = &features12;

		device_features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
		device_features2.pNext = &features13;

		device.getFeatures2(&device_features2);
		const auto required_features13 = WindowContext::RequiredVulkan13Features();

		const auto queue_family = VulkanFindQueueFamily(device, surface);
		if (queue_family == static_cast<uint32_t>(-1)) {
			LOGF("No universal graphics, compute, and presentation queue\n");
			skip_device = true;
		}

		if (color_write_ext.colorWriteEnable != VK_TRUE) {
			LOGF("colorWriteEnable is not supported\n");
#if !defined(__APPLE__)
			skip_device = true;
#endif
		}

		if (depth_clip_control.depthClipControl != VK_TRUE) {
			LOGF("depthClipControl is not supported\n");
			skip_device = true;
		}
		if (depth_clip_enable.depthClipEnable != VK_TRUE) {
			LOGF("depthClipEnable is not supported\n");
#if !defined(__APPLE__)
			skip_device = true;
#endif
		}
#if !defined(__APPLE__)
		if (fragment_barycentric.fragmentShaderBarycentric != VK_TRUE) {
			LOGF("fragmentShaderBarycentric is not supported\n");
			skip_device = true;
		}
#endif

		if (features12.samplerMirrorClampToEdge != VK_TRUE) {
			LOGF("samplerMirrorClampToEdge is not supported\n");
			skip_device = true;
		}
		if (features12.timelineSemaphore != VK_TRUE) {
			LOGF("timelineSemaphore is not supported\n");
			skip_device = true;
		}
		if (features13.robustImageAccess != VK_TRUE) {
			LOGF("robustImageAccess is not supported\n");
			skip_device = true;
		}
		if (required_features13.dynamicRendering == VK_TRUE &&
		    features13.dynamicRendering != VK_TRUE) {
			LOGF("dynamicRendering is not supported\n");
			skip_device = true;
		}
		if (required_features13.synchronization2 == VK_TRUE &&
		    features13.synchronization2 != VK_TRUE) {
			LOGF("synchronization2 is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.sampleRateShading != VK_TRUE) {
			LOGF("sampleRateShading is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.depthBiasClamp != VK_TRUE) {
			LOGF("depthBiasClamp is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.shaderClipDistance != VK_TRUE) {
			LOGF("shaderClipDistance is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.shaderCullDistance != VK_TRUE) {
			LOGF("shaderCullDistance is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.largePoints != VK_TRUE) {
			LOGF("largePoints is not supported\n");
			skip_device = true;
		}
		if (features12.shaderOutputLayer != VK_TRUE) {
			LOGF("shaderOutputLayer is not supported\n");
			skip_device = true;
		}

		if (device_features2.features.fragmentStoresAndAtomics != VK_TRUE) {
			LOGF("fragmentStoresAndAtomics is not supported\n");
			skip_device = true;
		}

		if (device_features2.features.samplerAnisotropy != VK_TRUE) {
			LOGF("samplerAnisotropy is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.robustBufferAccess != VK_TRUE) {
			LOGF("robustBufferAccess is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.depthBounds != VK_TRUE) {
			LOGF("depthBounds is not supported\n");
#if !defined(__APPLE__)
			skip_device = true;
#endif
		}
		if (device_features2.features.shaderStorageImageWriteWithoutFormat != VK_TRUE) {
			LOGF("shaderStorageImageWriteWithoutFormat is not supported\n");
			skip_device = true;
		}

		if (device_features2.features.shaderImageGatherExtended != VK_TRUE) {
			LOGF("shaderImageGatherExtended is not supported\n");
			skip_device = true;
		}

		if (device_features2.features.independentBlend != VK_TRUE) {
			LOGF("independentBlend is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.tessellationShader != VK_TRUE) {
			LOGF("tessellationShader is not supported\n");
			skip_device = true;
		}

		if (!skip_device) {
			auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
			    "vkEnumerateDeviceExtensionProperties",
			    [&](uint32_t* count, vk::ExtensionProperties* values) {
				    return device.enumerateDeviceExtensionProperties(nullptr, count, values);
			    });
			EXIT_NOT_IMPLEMENTED(available_extensions.empty());

			for (const char* ext: device_extensions) {
				if (!HasExtension(available_extensions, ext)) {
					skip_device = true;
					break;
				}
			}

			if (skip_device) {
				for (const auto& ext: available_extensions) {
					LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName.data(),
					     ext.specVersion);
				}
			}
		}

		SurfaceCapabilities candidate_capabilities;
		if (!skip_device) {
			GetSurfaceCapabilities(device, surface, candidate_capabilities);

			if (!(candidate_capabilities.capabilities.supportedUsageFlags &
			      vk::ImageUsageFlagBits::eTransferDst)) {
				LOGF("Surface cannot be destination of blit\n");
				skip_device = true;
			}
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eD32Sfloat, true,
		                                 vk::FormatFeatureFlagBits::eDepthStencilAttachment)) {
			LOGF("Format vk::Format::eD32Sfloat cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eD32SfloatS8Uint, true,
		                                 vk::FormatFeatureFlagBits::eDepthStencilAttachment)) {
			LOGF("Format vk::Format::eD32SfloatS8Uint cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eD16Unorm, true,
		                                 vk::FormatFeatureFlagBits::eDepthStencilAttachment)) {
			LOGF("Format vk::Format::eD16Unorm cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eBc3SrgbBlock, true,
		                                 vk::FormatFeatureFlagBits::eSampledImage |
		                                     vk::FormatFeatureFlagBits::eTransferDst)) {
			LOGF("Format vk::Format::eBc3SrgbBlock cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eR8G8B8A8Srgb, true,
		                                 vk::FormatFeatureFlagBits::eSampledImage |
		                                     vk::FormatFeatureFlagBits::eTransferDst)) {
			LOGF("Format vk::Format::eR8G8B8A8Srgb cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eR8Unorm, true,
		                                 vk::FormatFeatureFlagBits::eSampledImage |
		                                     vk::FormatFeatureFlagBits::eTransferDst)) {
			LOGF("Format vk::Format::eR8Unorm cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eR8G8Unorm, true,
		                                 vk::FormatFeatureFlagBits::eSampledImage |
		                                     vk::FormatFeatureFlagBits::eTransferDst)) {
			LOGF("Format vk::Format::eR8G8Unorm cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eR8G8B8A8Srgb, true,
		                                 vk::FormatFeatureFlagBits::eStorageImage |
		                                     vk::FormatFeatureFlagBits::eTransferDst)) {
			LOGF("Format vk::Format::eR8G8B8A8Srgb cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, vk::Format::eR8G8B8A8Unorm, true,
			                                 vk::FormatFeatureFlagBits::eStorageImage |
			                                     vk::FormatFeatureFlagBits::eTransferDst)) {
				LOGF("Format vk::Format::eR8G8B8A8Unorm cannot be used as texture\n");
				skip_device = true;
			}
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eB8G8R8A8Srgb, true,
		                                 vk::FormatFeatureFlagBits::eStorageImage |
		                                     vk::FormatFeatureFlagBits::eTransferDst)) {
			LOGF("Format vk::Format::eB8G8R8A8Srgb cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, vk::Format::eB8G8R8A8Unorm, true,
			                                 vk::FormatFeatureFlagBits::eStorageImage |
			                                     vk::FormatFeatureFlagBits::eTransferDst)) {
				LOGF("Format vk::Format::eB8G8R8A8Unorm cannot be used as texture\n");
				skip_device = true;
			}
		}

		if (!skip_device && device_properties.limits.maxSamplerAnisotropy < 16.0f) {
			LOGF("maxSamplerAnisotropy < 16.0f");
			skip_device = true;
		}

		if (skip_device) {
			continue;
		}

		if (best_device == nullptr ||
		    device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
			best_device       = device;
			best_queue_family = queue_family;
			best_capabilities = std::move(candidate_capabilities);
		}
	}

	out_device       = best_device;
	out_queue_family = best_queue_family;
	if (best_device != nullptr) {
		out_capabilities = std::move(best_capabilities);
	}
}

static void VulkanInitSubgroupSizeControl(vk::PhysicalDevice physical_device,
                                          GraphicContext&    graphics) {
	EXIT_IF(physical_device == nullptr);

	vk::PhysicalDeviceSubgroupSizeControlProperties subgroup_size_control {};
	subgroup_size_control.sType = vk::StructureType::ePhysicalDeviceSubgroupSizeControlProperties;
	subgroup_size_control.pNext = nullptr;

	vk::PhysicalDeviceVulkan11Properties properties11 {};
	properties11.sType = vk::StructureType::ePhysicalDeviceVulkan11Properties;
	properties11.pNext = &subgroup_size_control;

	vk::PhysicalDeviceProperties2 properties2 {};
	properties2.sType = vk::StructureType::ePhysicalDeviceProperties2;
	properties2.pNext = &properties11;

	physical_device.getProperties2(&properties2);

	vk::PhysicalDeviceVulkan13Features features13 {};
	features13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;
	features13.pNext = nullptr;

	vk::PhysicalDeviceFeatures2 features2 {};
	features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
	features2.pNext = &features13;

	physical_device.getFeatures2(&features2);

	graphics.subgroup_size                 = properties11.subgroupSize;
	graphics.min_subgroup_size             = subgroup_size_control.minSubgroupSize;
	graphics.max_subgroup_size             = subgroup_size_control.maxSubgroupSize;
	graphics.required_subgroup_size_stages = subgroup_size_control.requiredSubgroupSizeStages;
	graphics.compute_subgroup_size_control_enabled =
	    features13.subgroupSizeControl == VK_TRUE &&
	    (graphics.required_subgroup_size_stages & vk::ShaderStageFlagBits::eCompute) &&
	    subgroup_size_control.minSubgroupSize <= 64 &&
	    subgroup_size_control.maxSubgroupSize >= 64;
	graphics.compute_wave64_supported =
	    graphics.subgroup_size == 64u || graphics.compute_subgroup_size_control_enabled;

	LOGF("Vulkan subgroup: default=%u min=%u max=%u stages=0x%08x size_control=%s wave64=%s\n",
	     graphics.subgroup_size, graphics.min_subgroup_size, graphics.max_subgroup_size,
	     static_cast<vk::ShaderStageFlags::MaskType>(graphics.required_subgroup_size_stages),
	     graphics.compute_subgroup_size_control_enabled ? "true" : "false",
	     graphics.compute_wave64_supported ? "true" : "false");
}

static vk::Device VulkanCreateDevice(vk::PhysicalDevice physical_device, const VulkanExtensions& r,
                                     uint32_t                        queue_family,
                                     const std::vector<const char*>& device_extensions,
                                     GraphicContext&                 graphics) {
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(queue_family == static_cast<uint32_t>(-1));

	const float               queue_priority = 1.0f;
	vk::DeviceQueueCreateInfo queue_create_info {};
	queue_create_info.sType            = vk::StructureType::eDeviceQueueCreateInfo;
	queue_create_info.queueFamilyIndex = queue_family;
	queue_create_info.queueCount       = 1;
	queue_create_info.pQueuePriorities = &queue_priority;

	vk::PhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
	color_write_ext.sType = vk::StructureType::ePhysicalDeviceColorWriteEnableFeaturesEXT;
	color_write_ext.pNext = nullptr;
	color_write_ext.colorWriteEnable = VK_TRUE;

	vk::PhysicalDeviceDepthClipEnableFeaturesEXT depth_clip_enable {};
	depth_clip_enable.sType = vk::StructureType::ePhysicalDeviceDepthClipEnableFeaturesEXT;
	depth_clip_enable.pNext = &color_write_ext;
	depth_clip_enable.depthClipEnable = VK_TRUE;

	vk::PhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
	depth_clip_control.sType = vk::StructureType::ePhysicalDeviceDepthClipControlFeaturesEXT;
	// MoltenVK lacks VK_EXT_depth_clip_enable and VK_EXT_color_write_enable, so drop those
	// feature structs from the chain on macOS (the renderer falls back to default depth
	// clipping and static color-write masks).
#if defined(__APPLE__)
	depth_clip_control.pNext = nullptr;
#else
	depth_clip_control.pNext = &depth_clip_enable;
#endif
	depth_clip_control.depthClipControl = VK_TRUE;

	vk::PhysicalDeviceVulkan12Features features12 {};
	features12.sType                    = vk::StructureType::ePhysicalDeviceVulkan12Features;
	features12.pNext                    = &depth_clip_control;
	features12.samplerMirrorClampToEdge = VK_TRUE;

	vk::PhysicalDeviceVulkan13Features supported_features13 {};
	supported_features13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;

	vk::PhysicalDeviceVulkan12Features supported_features12 {};
	supported_features12.sType = vk::StructureType::ePhysicalDeviceVulkan12Features;
	supported_features12.pNext = nullptr;
	supported_features13.pNext = &supported_features12;

#if !defined(__APPLE__)
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR supported_fragment_barycentric {};
	supported_fragment_barycentric.sType =
	    vk::StructureType::ePhysicalDeviceFragmentShaderBarycentricFeaturesKHR;
	supported_fragment_barycentric.pNext = nullptr;
	supported_features12.pNext           = &supported_fragment_barycentric;
#endif

	const auto robustness2_ext_enabled =
	    HasExtension(device_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

	vk::PhysicalDeviceRobustness2FeaturesEXT supported_robustness2 {};
	supported_robustness2.sType = vk::StructureType::ePhysicalDeviceRobustness2FeaturesEXT;
	supported_robustness2.pNext = nullptr;
	if (robustness2_ext_enabled) {
#if defined(__APPLE__)
		supported_features12.pNext = &supported_robustness2;
#else
		supported_fragment_barycentric.pNext = &supported_robustness2;
#endif
	}

	vk::PhysicalDeviceFeatures2 supported_features2 {};
	supported_features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
	supported_features2.pNext = &supported_features13;
	physical_device.getFeatures2(&supported_features2);
	const auto required_features13 = WindowContext::RequiredVulkan13Features();
	EXIT_NOT_IMPLEMENTED(supported_features12.timelineSemaphore != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(required_features13.dynamicRendering == VK_TRUE &&
	                     supported_features13.dynamicRendering != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(required_features13.synchronization2 == VK_TRUE &&
	                     supported_features13.synchronization2 != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features2.features.sampleRateShading != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features2.features.depthBiasClamp != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features2.features.shaderClipDistance != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features2.features.shaderCullDistance != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features2.features.largePoints != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features12.shaderOutputLayer != VK_TRUE);
#if !defined(__APPLE__)
	EXIT_NOT_IMPLEMENTED(supported_fragment_barycentric.fragmentShaderBarycentric != VK_TRUE);
#endif
	features12.timelineSemaphore = VK_TRUE;
	features12.shaderOutputLayer = VK_TRUE;

	if (Config::GpuAssistedValidationEnabled()) {
		if (supported_features12.bufferDeviceAddress != VK_TRUE) {
			EXIT("--gpu-assisted-validation requires bufferDeviceAddress, unsupported by this "
			     "device\n");
		}
		features12.bufferDeviceAddress = VK_TRUE;
	}

	vk::PhysicalDeviceFeatures device_features {};
	device_features.fragmentStoresAndAtomics = VK_TRUE;
	device_features.samplerAnisotropy        = VK_TRUE;
	device_features.robustBufferAccess       = VK_TRUE;
#if !defined(__APPLE__)
	device_features.depthBounds = VK_TRUE; // unsupported by MoltenVK
#endif
	device_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
	device_features.shaderImageGatherExtended            = VK_TRUE;
	device_features.independentBlend                     = VK_TRUE;
	device_features.tessellationShader                   = VK_TRUE;
	device_features.sampleRateShading                    = VK_TRUE;
	device_features.depthBiasClamp                       = VK_TRUE;
	device_features.shaderClipDistance                   = VK_TRUE;
	device_features.shaderCullDistance                   = VK_TRUE;
	device_features.largePoints                          = VK_TRUE;
	graphics.sample_rate_shading_enabled                 = true;
	device_features.vertexPipelineStoresAndAtomics =
	    supported_features2.features.vertexPipelineStoresAndAtomics;
	if (Config::GpuAssistedValidationEnabled()) {
		device_features.shaderInt64 = supported_features2.features.shaderInt64;
	}

	vk::PhysicalDeviceRobustness2FeaturesEXT robustness2 {};
	robustness2.sType = vk::StructureType::ePhysicalDeviceRobustness2FeaturesEXT;
#if defined(__APPLE__)
	robustness2.pNext = &features12;
#else
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR fragment_barycentric {};
	fragment_barycentric.sType =
	    vk::StructureType::ePhysicalDeviceFragmentShaderBarycentricFeaturesKHR;
	fragment_barycentric.pNext                     = &features12;
	fragment_barycentric.fragmentShaderBarycentric = VK_TRUE;
	robustness2.pNext                              = &fragment_barycentric;
#endif
	if (robustness2_ext_enabled) {
		robustness2.robustBufferAccess2 = supported_robustness2.robustBufferAccess2;
		robustness2.robustImageAccess2  = supported_robustness2.robustImageAccess2;
		robustness2.nullDescriptor      = supported_robustness2.nullDescriptor;
	}

	const bool subgroup_size_control_enabled =
	    graphics.compute_subgroup_size_control_enabled &&
	    supported_features13.subgroupSizeControl == VK_TRUE;

	auto features13 = required_features13;
#if defined(__APPLE__)
	features13.pNext = robustness2_ext_enabled ? static_cast<void*>(&robustness2)
	                                           : static_cast<void*>(&features12);
#else
	features13.pNext = robustness2_ext_enabled ? static_cast<void*>(&robustness2)
	                                           : static_cast<void*>(&fragment_barycentric);
#endif
	features13.robustImageAccess   = supported_features13.robustImageAccess;
	features13.subgroupSizeControl = subgroup_size_control_enabled ? VK_TRUE : VK_FALSE;

	LOGF("Vulkan robustness: robustImageAccess=%s robustImageAccess2=%s\n",
	     features13.robustImageAccess == VK_TRUE ? "true" : "false",
	     robustness2_ext_enabled && robustness2.robustImageAccess2 == VK_TRUE ? "true" : "false");

	vk::DeviceCreateInfo create_info {};
	create_info.sType                   = vk::StructureType::eDeviceCreateInfo;
	create_info.pNext                   = &features13;
	create_info.flags                   = {};
	create_info.pQueueCreateInfos       = &queue_create_info;
	create_info.queueCreateInfoCount    = 1;
	create_info.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
	create_info.ppEnabledExtensionNames = device_extensions.data();
	create_info.pEnabledFeatures        = &device_features;

	vk::Device device = nullptr;

	auto result = physical_device.createDevice(&create_info, nullptr, &device);
	if (result != vk::Result::eSuccess) {
		LOGF("vkCreateDevice failed: %s\n", VulkanToString(result).c_str());
		return nullptr;
	}

	return device;
}

static void VulkanGetExtensions(SDL_Window* window, VulkanExtensions& r) {
	EXIT_IF(window == nullptr);

	uint32_t required_extensions_count = 0;

	auto sdl_result = SDL_Vulkan_GetInstanceExtensions(window, &required_extensions_count, nullptr);

	EXIT_NOT_IMPLEMENTED(sdl_result == SDL_FALSE);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);

	r.required_extensions =
	    std::vector<const char*>(required_extensions_count); // @suppress("Ambiguous problem")
	std::memset(r.required_extensions.data(), 0,
	            sizeof(const char*) * r.required_extensions.size());

	sdl_result = SDL_Vulkan_GetInstanceExtensions(window, &required_extensions_count,
	                                              r.required_extensions.data());

	EXIT_NOT_IMPLEMENTED(sdl_result == SDL_FALSE);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);
	EXIT_NOT_IMPLEMENTED(required_extensions_count != r.required_extensions.size());

	r.available_extensions =
	    EnumerateVulkan<vk::ExtensionProperties>( // @suppress("Ambiguous problem")
	        "vkEnumerateInstanceExtensionProperties",
	        [](uint32_t* count, vk::ExtensionProperties* values) {
		        return vk::enumerateInstanceExtensionProperties(nullptr, count, values);
	        });

	r.enable_validation_layers = Config::VulkanValidationEnabled();

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
			EXIT("Could not query Vulkan loader version: %s\n", VulkanToString(result).c_str());
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

	VulkanExtensions r;
	VulkanGetExtensions(window, r);
	VulkanCheckInstanceVersion();

	vk::ApplicationInfo app_info {};
	app_info.sType              = vk::StructureType::eApplicationInfo;
	app_info.pNext              = nullptr;
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
	validation_features.sType                          = vk::StructureType::eValidationFeaturesEXT;
	validation_features.pNext                          = nullptr;
	validation_features.enabledValidationFeatureCount  = enabled_features_count;
	validation_features.pEnabledValidationFeatures     = enabled_features;
	validation_features.disabledValidationFeatureCount = 0;
	validation_features.pDisabledValidationFeatures    = nullptr;

	vk::DebugUtilsMessengerCreateInfoEXT dbg_create_info {};
	dbg_create_info.sType           = vk::StructureType::eDebugUtilsMessengerCreateInfoEXT;
	dbg_create_info.pNext           = &validation_features;
	dbg_create_info.flags           = {};
	dbg_create_info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
	dbg_create_info.messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
	                                  vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
	                                  vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
	dbg_create_info.pfnUserCallback = VulkanDebugMessengerCallback;
	dbg_create_info.pUserData       = nullptr;

	vk::InstanceCreateInfo inst_info {};
	inst_info.sType                   = vk::StructureType::eInstanceCreateInfo;
	inst_info.pNext                   = (r.enable_validation_layers ? &dbg_create_info : nullptr);
	inst_info.flags                   = {};
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
	if (SDL_Vulkan_CreateSurface(window, static_cast<vk::Instance::CType>(graphic_ctx.instance),
	                             &native_surface) == SDL_FALSE) {
		EXIT("Could not create a Vulkan surface");
	}
	surface = native_surface;

	std::vector<const char*> device_extensions = {
	    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
	    VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	    "VK_KHR_maintenance1"};

#if defined(__APPLE__)
	// MoltenVK lacks VK_EXT_depth_clip_enable and VK_EXT_color_write_enable; the renderer
	// falls back to default depth clipping and static color-write masks on macOS. It also
	// requires VK_KHR_portability_subset per the Vulkan portability spec.
	device_extensions.push_back("VK_KHR_portability_subset");
#else
	device_extensions.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
	device_extensions.push_back(VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME);
	device_extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
#endif

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled()) {
		device_extensions.push_back(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	}
#endif

	uint32_t queue_family = static_cast<uint32_t>(-1);

	VulkanFindPhysicalDevice(graphic_ctx.instance, surface, device_extensions, surface_capabilities,
	                         graphic_ctx.physical_device, queue_family);

	if (graphic_ctx.physical_device == nullptr) {
		EXIT("Could not find suitable device");
	}

	vk::PhysicalDevicePushDescriptorProperties push_descriptor_properties {};
	vk::PhysicalDeviceProperties2              physical_device_properties {};
	push_descriptor_properties.sType = vk::StructureType::ePhysicalDevicePushDescriptorProperties;
	push_descriptor_properties.pNext = nullptr;
	physical_device_properties.sType = vk::StructureType::ePhysicalDeviceProperties2;
	physical_device_properties.pNext = &push_descriptor_properties;
	graphic_ctx.physical_device.getProperties2(&physical_device_properties);
	graphic_ctx.physical_device_properties = physical_device_properties.properties;
	graphic_ctx.max_push_descriptors       = push_descriptor_properties.maxPushDescriptors;
	graphic_ctx.physical_device.getMemoryProperties(&graphic_ctx.physical_device_memory_properties);
	const auto& device_properties = graphic_ctx.GetPhysicalDeviceProperties();

	LOGF("Select device: %s\n", device_properties.deviceName.data());

	{
		auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
		    "vkEnumerateDeviceExtensionProperties",
		    [&](uint32_t* count, vk::ExtensionProperties* values) {
			    return graphic_ctx.physical_device.enumerateDeviceExtensionProperties(
			        nullptr, count, values);
		    });

		if (HasExtension(available_extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
			graphic_ctx.memory_budget_ext_enabled = true;
		}
		if (HasExtension(available_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
		}
	}

	memcpy(device_name, device_properties.deviceName, sizeof(device_name));
	std::snprintf(processor_name, sizeof(processor_name), "%s",
	              Common::GetSystemInfo().ProcessorName.c_str());

	VulkanInitSubgroupSizeControl(graphic_ctx.physical_device, graphic_ctx);

	graphic_ctx.device = VulkanCreateDevice(graphic_ctx.physical_device, r, queue_family,
	                                        device_extensions, graphic_ctx);
	if (graphic_ctx.device == nullptr) {
		EXIT("Could not create device");
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphic_ctx.device);
	graphic_ctx.queue_family = queue_family;
	graphic_ctx.device.getQueue(queue_family, 0, &graphic_ctx.queue);
	EXIT_IF(graphic_ctx.queue == nullptr);

	if (!graphic_ctx.CreateAllocator()) {
		EXIT("Could not create Vulkan memory allocator");
	}

	render_context = std::make_unique<RenderContext>(graphic_ctx);
	LibKernel::Memory::InstallGpuResources(&render_context->GetGpuResources());
	presenter = std::make_unique<Presenter>(*this);
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
	if (SDL_Vulkan_CreateSurface(window, static_cast<vk::Instance::CType>(graphic_ctx.instance),
	                             &native_surface) == SDL_FALSE) {
		EXIT("Could not recreate the Vulkan surface: %s\n", SDL_GetError());
	}
	surface = native_surface;
	RefreshSurfaceCapabilities();
}

WindowContext::~WindowContext() {
	ShutdownImeInput();
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
	SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
}

} // namespace Libs::Graphics
