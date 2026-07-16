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
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

struct VulkanExtensions {
	bool enable_validation_layers = false;

	std::vector<const char*>           required_extensions;
	std::vector<VkExtensionProperties> available_extensions;
	std::vector<const char*>           required_layers;
	std::vector<VkLayerProperties>     available_layers;
};

static bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(),
	                   [name](const auto& ext) { return strcmp(ext.extensionName, name) == 0; });
}

static bool HasExtension(const std::vector<const char*>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(),
	                   [name](const char* ext) { return strcmp(ext, name) == 0; });
}

static bool HasLayer(const std::vector<VkLayerProperties>& layers, const char* name) {
	return std::any_of(layers.begin(), layers.end(),
	                   [name](const auto& layer) { return strcmp(layer.layerName, name) == 0; });
}

void VulkanGetSurfaceCapabilities(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                  SurfaceCapabilities* r) {
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &r->capabilities);

	uint32_t formats_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &formats_count, nullptr);

	EXIT_NOT_IMPLEMENTED(formats_count == 0);

	r->formats = std::vector<VkSurfaceFormatKHR>(formats_count); // @suppress("Ambiguous problem")
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &formats_count,
	                                     r->formats.data());

	uint32_t present_modes_count = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_modes_count,
	                                          nullptr);

	EXIT_NOT_IMPLEMENTED(present_modes_count == 0);

	r->present_modes =
	    std::vector<VkPresentModeKHR>(present_modes_count); // @suppress("Ambiguous problem")
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_modes_count,
	                                          r->present_modes.data());

	r->format_srgb_bgra32 = false;
	for (const auto& f: r->formats) {
		if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
		    f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			r->format_srgb_bgra32 = true;
			break;
		}
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
		    f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			r->format_unorm_bgra32 = true;
			break;
		}
	}
}

static bool CheckFormat(VkPhysicalDevice device, VkFormat format, bool tile,
                        VkFormatFeatureFlags features) {
	VkFormatProperties format_props {};
	vkGetPhysicalDeviceFormatProperties(device, format, &format_props);

	const auto supported_features =
	    (tile ? format_props.optimalTilingFeatures : format_props.linearTilingFeatures);
	return (supported_features & features) == features;
}

struct QueueInfo {
	uint32_t family   = 0;
	uint32_t index    = 0;
	bool     graphics = false;
	bool     compute  = false;
	bool     transfer = false;
	bool     present  = false;
};

struct VulkanQueues {
	uint32_t               family_count = 0;
	std::vector<uint32_t>  family_used;
	std::vector<QueueInfo> available;
	std::vector<QueueInfo> graphics;
	std::vector<QueueInfo> compute;
	std::vector<QueueInfo> transfer;
	std::vector<QueueInfo> present;
};

static void VulkanDumpQueues(const VulkanQueues& qs) {
	LOGF("Queues selected:\n"
	     "\t family_count = %u\n",
	     qs.family_count);
	std::vector<std::string> nums;
	for (auto u: qs.family_used) {
		nums.push_back(fmt::format("{}", u));
	}
	LOGF("\t family_used = [%s]\n"
	     "\t graphics:\n",
	     Common::Concat(nums, ", ").c_str());
	for (const auto& q: qs.graphics) {
		LOGF("\t\t family = %u, index = %u\n", q.family, q.index);
	}
	LOGF("\t compute:\n");
	for (const auto& q: qs.compute) {
		LOGF("\t\t family = %u, index = %u\n", q.family, q.index);
	}
	LOGF("\t transfer:\n");
	for (const auto& q: qs.transfer) {
		LOGF("\t\t family = %u, index = %u\n", q.family, q.index);
	}
	LOGF("\t present:\n");
	for (const auto& q: qs.present) {
		LOGF("\t\t family = %u, index = %u\n", q.family, q.index);
	}
}

static VulkanQueues VulkanFindQueues(VkPhysicalDevice device, VkSurfaceKHR surface,
                                     uint32_t graphics_num, uint32_t compute_num,
                                     uint32_t transfer_num, uint32_t present_num) {
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);

	VulkanQueues qs;

	uint32_t queue_family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
	std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

	qs.family_count = queue_family_count;

	uint32_t family = 0;
	for (auto& f: queue_families) {
		VkBool32 presentation_supported = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface, &presentation_supported);

		LOGF("\tqueue family: %s [count = %u], [present = %s]\n",
		     string_VkQueueFlags(f.queueFlags).c_str(), f.queueCount,
		     (presentation_supported == VK_TRUE ? "true" : "false"));

		for (uint32_t i = 0; i < f.queueCount; i++) {
			QueueInfo info;
			info.family   = family;
			info.index    = i;
			info.graphics = (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
			info.compute  = (f.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
			info.transfer = (f.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
			info.present  = (presentation_supported == VK_TRUE);

			qs.available.push_back(info);
		}

		qs.family_used.push_back(0);

		family++;
	}

	auto select_queues = [&qs](uint32_t count, auto matches, auto& selected) {
		for (uint32_t i = 0; i < count; i++) {
			auto it = std::find_if(qs.available.begin(), qs.available.end(), matches);
			if (it == qs.available.end()) {
				continue;
			}

			qs.family_used[it->family]++;
			selected.push_back(*it);
			qs.available.erase(it);
		}
	};

	select_queues(graphics_num, [](const auto& q) { return q.graphics; }, qs.graphics);
	select_queues(compute_num, [](const auto& q) { return q.compute; }, qs.compute);
	select_queues(transfer_num, [](const auto& q) { return q.transfer; }, qs.transfer);
	select_queues(present_num, [](const auto& q) { return q.present; }, qs.present);

	return qs;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void VulkanFindPhysicalDevice(VkInstance instance, VkSurfaceKHR surface,
                                     const std::vector<const char*>& device_extensions,
                                     SurfaceCapabilities*            out_capabilities,
                                     VkPhysicalDevice* out_device, VulkanQueues* out_queues) {
	EXIT_IF(instance == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(out_capabilities == nullptr);
	EXIT_IF(out_device == nullptr);
	EXIT_IF(out_queues == nullptr);

	uint32_t devices_count = 0;
	vkEnumeratePhysicalDevices(instance, &devices_count, nullptr);

	EXIT_NOT_IMPLEMENTED(devices_count == 0);

	std::vector<VkPhysicalDevice> devices(devices_count);
	vkEnumeratePhysicalDevices(instance, &devices_count, devices.data());

	VkPhysicalDevice best_device = nullptr;
	VulkanQueues     best_queues;

	for (const auto& device: devices) {
		bool skip_device = false;

		VkPhysicalDeviceProperties device_properties {};
		vkGetPhysicalDeviceProperties(device, &device_properties);

		LOGF("Vulkan device: %s\n", device_properties.deviceName);
		if (device_properties.apiVersion < VULKAN_TARGET_API_VERSION) {
			LOGF("Vulkan %u.%u is required, but device supports only %u.%u.%u\n",
			     VK_VERSION_MAJOR(VULKAN_TARGET_API_VERSION),
			     VK_VERSION_MINOR(VULKAN_TARGET_API_VERSION),
			     VK_VERSION_MAJOR(device_properties.apiVersion),
			     VK_VERSION_MINOR(device_properties.apiVersion),
			     VK_VERSION_PATCH(device_properties.apiVersion));
			continue;
		}

		VkPhysicalDeviceFeatures2 device_features2 {};

		VkPhysicalDeviceVulkan13Features features13 {};
		features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		features13.pNext = nullptr;

		VkPhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
		color_write_ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT;
		color_write_ext.pNext = nullptr;

		VkPhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
		depth_clip_control.sType =
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
		depth_clip_control.pNext = &color_write_ext;

		VkPhysicalDeviceVulkan12Features features12 {};
		features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		features12.pNext = &depth_clip_control;
		features13.pNext = &features12;

		device_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		device_features2.pNext = &features13;

		vkGetPhysicalDeviceFeatures2(device, &device_features2);

		auto qs = VulkanFindQueues(
		    device, surface, GraphicContext::QUEUE_GFX_NUM, GraphicContext::QUEUE_COMPUTE_NUM,
		    GraphicContext::QUEUE_UTIL_NUM, GraphicContext::QUEUE_PRESENT_NUM);

		VulkanDumpQueues(qs);

		if (qs.graphics.size() != GraphicContext::QUEUE_GFX_NUM ||
		    !(qs.compute.size() >= 1 && qs.compute.size() <= GraphicContext::QUEUE_COMPUTE_NUM) ||
		    qs.transfer.size() != GraphicContext::QUEUE_UTIL_NUM ||
		    qs.present.size() != GraphicContext::QUEUE_PRESENT_NUM) {
			LOGF("Not enough queues\n");
			skip_device = true;
		}

		if (color_write_ext.colorWriteEnable != VK_TRUE) {
			LOGF("colorWriteEnable is not supported\n");
			skip_device = true;
		}

		if (depth_clip_control.depthClipControl != VK_TRUE) {
			LOGF("depthClipControl is not supported\n");
			skip_device = true;
		}

		if (features12.samplerMirrorClampToEdge != VK_TRUE) {
			LOGF("samplerMirrorClampToEdge is not supported\n");
			skip_device = true;
		}
		if (features13.robustImageAccess != VK_TRUE) {
			LOGF("robustImageAccess is not supported\n");
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
			skip_device = true;
		}
		if (device_features2.features.shaderStorageImageWriteWithoutFormat != VK_TRUE) {
			LOGF("shaderStorageImageWriteWithoutFormat is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.shaderStorageImageReadWithoutFormat != VK_TRUE) {
			LOGF("shaderStorageImageReadWithoutFormat is not supported\n");
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
			uint32_t extensions_count = 0;
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensions_count, nullptr);

			EXIT_NOT_IMPLEMENTED(extensions_count == 0);

			std::vector<VkExtensionProperties> available_extensions(extensions_count);
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensions_count,
			                                     available_extensions.data());

			for (const char* ext: device_extensions) {
				if (!HasExtension(available_extensions, ext)) {
					skip_device = true;
					break;
				}
			}

			if (skip_device) {
				for (const auto& ext: available_extensions) {
					LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName,
					     ext.specVersion);
				}
			}
		}

		if (!skip_device) {
			VulkanGetSurfaceCapabilities(device, surface, out_capabilities);

			if ((out_capabilities->capabilities.supportedUsageFlags &
			     VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
				LOGF("Surface cannot be destination of blit\n");
				skip_device = true;
			}
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, false, VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
			LOGF("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as transfer source\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D32_SFLOAT, true,
		                                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
			LOGF("Format VK_FORMAT_D32_SFLOAT cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D32_SFLOAT_S8_UINT, true,
		                                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
			LOGF("Format VK_FORMAT_D32_SFLOAT_S8_UINT cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D16_UNORM, true,
		                                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
			LOGF("Format VK_FORMAT_D16_UNORM cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D24_UNORM_S8_UINT, true,
		                                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
			LOGF("Format VK_FORMAT_D24_UNORM_S8_UINT cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_BC3_SRGB_BLOCK, true,
		                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			LOGF("Format VK_FORMAT_BC3_SRGB_BLOCK cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true,
		                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			LOGF("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_R8_UNORM, true,
		                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			LOGF("Format VK_FORMAT_R8_UNORM cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8_UNORM, true,
		                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			LOGF("Format VK_FORMAT_R8G8_UNORM cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true,
		                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
		                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			LOGF("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8B8A8_UNORM, true,
			                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
			                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
				LOGF("Format VK_FORMAT_R8G8B8A8_UNORM cannot be used as texture\n");
				skip_device = true;
			}
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_B8G8R8A8_SRGB, true,
		                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
		                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			LOGF("Format VK_FORMAT_B8G8R8A8_SRGB cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, VK_FORMAT_B8G8R8A8_UNORM, true,
			                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
			                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
				LOGF("Format VK_FORMAT_B8G8R8A8_UNORM cannot be used as texture\n");
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
		    device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			best_device = device;
			best_queues = qs;
		}
	}

	*out_device = best_device;
	*out_queues = best_queues;
}

static void VulkanInitSubgroupSizeControl(VkPhysicalDevice physical_device, GraphicContext* ctx) {
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(ctx == nullptr);

	VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size_control {};
	subgroup_size_control.sType =
	    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
	subgroup_size_control.pNext = nullptr;

	VkPhysicalDeviceVulkan11Properties properties11 {};
	properties11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
	properties11.pNext = &subgroup_size_control;

	VkPhysicalDeviceProperties2 properties2 {};
	properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties2.pNext = &properties11;

	vkGetPhysicalDeviceProperties2(physical_device, &properties2);

	VkPhysicalDeviceVulkan13Features features13 {};
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13.pNext = nullptr;

	VkPhysicalDeviceFeatures2 features2 {};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &features13;

	vkGetPhysicalDeviceFeatures2(physical_device, &features2);

	ctx->subgroup_size                 = properties11.subgroupSize;
	ctx->min_subgroup_size             = subgroup_size_control.minSubgroupSize;
	ctx->max_subgroup_size             = subgroup_size_control.maxSubgroupSize;
	ctx->required_subgroup_size_stages = subgroup_size_control.requiredSubgroupSizeStages;
	ctx->subgroup_size_control_enabled = features13.subgroupSizeControl == VK_TRUE &&
	                                     subgroup_size_control.minSubgroupSize <= 64 &&
	                                     subgroup_size_control.maxSubgroupSize >= 64;

	LOGF("Vulkan subgroup: default=%u min=%u max=%u stages=0x%08x size_control=%s\n",
	     ctx->subgroup_size, ctx->min_subgroup_size, ctx->max_subgroup_size,
	     ctx->required_subgroup_size_stages, ctx->subgroup_size_control_enabled ? "true" : "false");
}

static VkDevice VulkanCreateDevice(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                   const VulkanExtensions* r, const VulkanQueues& queues,
                                   const std::vector<const char*>& device_extensions,
                                   const GraphicContext*           ctx) {
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(r == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(ctx == nullptr);

	std::vector<VkDeviceQueueCreateInfo> queue_create_info(queues.family_count);
	std::vector<std::vector<float>>      queue_priority(queues.family_count);
	uint32_t                             queue_create_info_num = 0;

	for (uint32_t i = 0; i < queues.family_count; i++) {
		if (queues.family_used[i] != 0) {
			for (uint32_t pi = 0; pi < queues.family_used[i]; pi++) {
				queue_priority[queue_create_info_num].push_back(1.0f);
			}

			queue_create_info[queue_create_info_num].sType =
			    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue_create_info[queue_create_info_num].pNext            = nullptr;
			queue_create_info[queue_create_info_num].flags            = 0;
			queue_create_info[queue_create_info_num].queueFamilyIndex = i;
			queue_create_info[queue_create_info_num].queueCount       = queues.family_used[i];
			queue_create_info[queue_create_info_num].pQueuePriorities =
			    queue_priority[queue_create_info_num].data();

			queue_create_info_num++;
		}
	}

	VkPhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
	color_write_ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT;
	color_write_ext.pNext = nullptr;
	color_write_ext.colorWriteEnable = VK_TRUE;

	VkPhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
	depth_clip_control.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
	depth_clip_control.pNext = &color_write_ext;
	depth_clip_control.depthClipControl = VK_TRUE;

	VkPhysicalDeviceVulkan12Features features12 {};
	features12.sType                    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.pNext                    = &depth_clip_control;
	features12.samplerMirrorClampToEdge = VK_TRUE;

	VkPhysicalDeviceSubgroupSizeControlFeatures subgroup_size_control {};
	subgroup_size_control.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
	subgroup_size_control.pNext = &features12;

	VkPhysicalDeviceVulkan13Features supported_features13 {};
	supported_features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	supported_features13.pNext = nullptr;

	const auto robustness2_ext_enabled =
	    HasExtension(device_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

	VkPhysicalDeviceRobustness2FeaturesEXT supported_robustness2 {};
	supported_robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
	supported_robustness2.pNext = nullptr;
	if (robustness2_ext_enabled) {
		supported_features13.pNext = &supported_robustness2;
	}

	VkPhysicalDeviceFeatures2 supported_features2 {};
	supported_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	supported_features2.pNext = &supported_features13;
	vkGetPhysicalDeviceFeatures2(physical_device, &supported_features2);

	VkPhysicalDeviceFeatures device_features {};
	device_features.fragmentStoresAndAtomics             = VK_TRUE;
	device_features.samplerAnisotropy                    = VK_TRUE;
	device_features.robustBufferAccess                   = VK_TRUE;
	device_features.depthBounds                          = VK_TRUE;
	device_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
	device_features.shaderStorageImageReadWithoutFormat  = VK_TRUE;
	device_features.shaderImageGatherExtended            = VK_TRUE;
	device_features.independentBlend                     = VK_TRUE;
	device_features.tessellationShader                   = VK_TRUE;
	device_features.vertexPipelineStoresAndAtomics =
	    supported_features2.features.vertexPipelineStoresAndAtomics;

	if (ctx->subgroup_size_control_enabled && supported_features13.subgroupSizeControl == VK_TRUE) {
		subgroup_size_control.subgroupSizeControl = VK_TRUE;
	}

	const auto* base_feature_chain = (subgroup_size_control.subgroupSizeControl == VK_TRUE
	                                      ? static_cast<const void*>(&subgroup_size_control)
	                                      : static_cast<const void*>(&features12));

	VkPhysicalDeviceRobustness2FeaturesEXT robustness2 {};
	robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
	robustness2.pNext = const_cast<void*>(base_feature_chain);
	if (robustness2_ext_enabled) {
		robustness2.robustBufferAccess2 = supported_robustness2.robustBufferAccess2;
		robustness2.robustImageAccess2  = supported_robustness2.robustImageAccess2;
		robustness2.nullDescriptor      = supported_robustness2.nullDescriptor;
	}

	VkPhysicalDeviceVulkan13Features features13 {};
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13.pNext =
	    robustness2_ext_enabled ? &robustness2 : const_cast<void*>(base_feature_chain);
	features13.robustImageAccess = supported_features13.robustImageAccess;

	LOGF("Vulkan robustness: robustImageAccess=%s robustImageAccess2=%s\n",
	     features13.robustImageAccess == VK_TRUE ? "true" : "false",
	     robustness2_ext_enabled && robustness2.robustImageAccess2 == VK_TRUE ? "true" : "false");

	VkDeviceCreateInfo create_info {};
	create_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create_info.pNext                = &features13;
	create_info.flags                = 0;
	create_info.pQueueCreateInfos    = queue_create_info.data();
	create_info.queueCreateInfoCount = queue_create_info_num;
	create_info.enabledLayerCount =
	    (r->enable_validation_layers ? static_cast<uint32_t>(r->required_layers.size()) : 0);
	create_info.ppEnabledLayerNames =
	    (r->enable_validation_layers ? r->required_layers.data() : nullptr);
	create_info.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
	create_info.ppEnabledExtensionNames = device_extensions.data();
	create_info.pEnabledFeatures        = &device_features;

	VkDevice device = nullptr;

	auto result = vkCreateDevice(physical_device, &create_info, nullptr, &device);
	if (result != VK_SUCCESS) {
		LOGF("vkCreateDevice failed: %s\n", string_VkResult(result));
		return nullptr;
	}

	return device;
}

static void VulkanGetExtensions(SDL_Window* window, VulkanExtensions* r) {
	EXIT_IF(window == nullptr);
	EXIT_IF(r == nullptr);

	uint32_t required_extensions_count  = 0;
	uint32_t available_extensions_count = 0;
	uint32_t available_layers_count     = 0;

	auto sdl_result = SDL_Vulkan_GetInstanceExtensions(window, &required_extensions_count, nullptr);

	EXIT_NOT_IMPLEMENTED(sdl_result == SDL_FALSE);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);

	r->required_extensions =
	    std::vector<const char*>(required_extensions_count); // @suppress("Ambiguous problem")
	std::memset(r->required_extensions.data(), 0,
	            sizeof(const char*) * r->required_extensions.size());

	sdl_result = SDL_Vulkan_GetInstanceExtensions(window, &required_extensions_count,
	                                              r->required_extensions.data());

	EXIT_NOT_IMPLEMENTED(sdl_result == SDL_FALSE);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);
	EXIT_NOT_IMPLEMENTED(required_extensions_count != r->required_extensions.size());

	vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions_count, nullptr);

	r->available_extensions = std::vector<VkExtensionProperties>(
	    available_extensions_count); // @suppress("Ambiguous problem")
	std::memset(r->available_extensions.data(), 0,
	            sizeof(VkExtensionProperties) * r->available_extensions.size());

	vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions_count,
	                                       r->available_extensions.data());

	EXIT_NOT_IMPLEMENTED(available_extensions_count != r->available_extensions.size());

	r->enable_validation_layers = Config::VulkanValidationEnabled();

	if (HasExtension(r->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
		r->required_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	} else {
		r->enable_validation_layers = false;
	}

	for (const char* ext: r->required_extensions) {
		LOGF("Vulkan required extension: %s\n", ext);
	}

	for (const auto& ext: r->available_extensions) {
		LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName, ext.specVersion);
	}

	vkEnumerateInstanceLayerProperties(&available_layers_count, nullptr);

	r->available_layers =
	    std::vector<VkLayerProperties>(available_layers_count); // @suppress("Ambiguous problem")
	std::memset(r->available_layers.data(), 0,
	            sizeof(VkLayerProperties) * r->available_layers.size());
	vkEnumerateInstanceLayerProperties(&available_layers_count, r->available_layers.data());

	EXIT_NOT_IMPLEMENTED(available_layers_count != r->available_layers.size());

	for (const auto& l: r->available_layers) {
		LOGF("Vulkan available layer: %s, specVersion = %u, implVersion = %u, %s\n", l.layerName,
		     l.specVersion, l.implementationVersion, l.description);
	}

	r->required_layers = {"VK_LAYER_KHRONOS_validation"};

	if (r->enable_validation_layers) {
		for (const char* l: r->required_layers) {
			if (!HasLayer(r->available_layers, l)) {
				LOGF("no validation layer: %s\n", l);
				r->enable_validation_layers = false;
				break;
			}
		}
	}

	if (r->enable_validation_layers) {
		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
		                                       &available_extensions_count, nullptr);

		std::vector<VkExtensionProperties> available_extensions(available_extensions_count);

		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
		                                       &available_extensions_count,
		                                       available_extensions.data());

		for (const auto& ext: available_extensions) {
			LOGF("VK_LAYER_KHRONOS_validation available extension: %s, version = %u\n",
			     ext.extensionName, ext.specVersion);
		}

		if (HasExtension(available_extensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
			r->required_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		} else {
			r->enable_validation_layers = false;
		}
	}
}

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    VkDebugUtilsMessageTypeFlagsEXT             message_types,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void* /*user_data*/) {
	EXIT_IF(callback_data == nullptr);
	EXIT_IF(callback_data->pMessage == nullptr);

	const char*     severity_str   = nullptr;
	fmt::text_style severity_style = Log::Color::Default;
	bool            skip           = false;
	bool            error          = false;
	bool            debug_printf   = false;
	switch (message_severity) {
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
			severity_str   = "V";
			severity_style = Log::Color::BrightWhite;
			skip           = true;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
			if ((message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0 &&
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
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
			severity_str   = "W";
			severity_style = Log::Color::Red;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
			severity_str   = "E";
			severity_style = Log::Color::BrightRed;
			error          = true;
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

static VKAPI_ATTR VkResult VKAPI_CALL VulkanCreateDebugUtilsMessengerEXT(
    VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* create_info,
    const VkAllocationCallbacks* allocator, VkDebugUtilsMessengerEXT* messenger) {
	EXIT_IF(instance == nullptr);

	if (auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
	        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
	    func != nullptr) {
		return func(instance, create_info, allocator, messenger);
	}
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void VulkanCreateQueues(GraphicContext* ctx, const VulkanQueues& queues) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(queues.graphics.size() != 1);
	EXIT_IF(queues.transfer.size() != 1);
	EXIT_IF(queues.present.size() != 1);
	EXIT_IF(!(queues.compute.size() >= 1 &&
	          queues.compute.size() <= GraphicContext::QUEUE_COMPUTE_NUM));

	auto get_queue = [ctx](int id, const QueueInfo& info, bool with_mutex = false) {
		ctx->queues[id].family = info.family;
		ctx->queues[id].index  = info.index;
		EXIT_IF(ctx->queues[id].vk_queue != nullptr);
		vkGetDeviceQueue(ctx->device, ctx->queues[id].family, ctx->queues[id].index,
		                 &ctx->queues[id].vk_queue);
		EXIT_NOT_IMPLEMENTED(ctx->queues[id].vk_queue == nullptr);
		if (with_mutex) {
			ctx->queues[id].mutex = new Common::Mutex;
		}
	};

	get_queue(GraphicContext::QUEUE_GFX, queues.graphics[0], true);
	ctx->queues[GraphicContext::QUEUE_UTIL].family = ctx->queues[GraphicContext::QUEUE_GFX].family;
	ctx->queues[GraphicContext::QUEUE_UTIL].index  = ctx->queues[GraphicContext::QUEUE_GFX].index;
	ctx->queues[GraphicContext::QUEUE_UTIL].vk_queue =
	    ctx->queues[GraphicContext::QUEUE_GFX].vk_queue;
	ctx->queues[GraphicContext::QUEUE_UTIL].mutex = ctx->queues[GraphicContext::QUEUE_GFX].mutex;
	LOGF("Vulkan queue: using graphics queue for utility submissions to preserve resource "
	     "ordering\n");
	get_queue(GraphicContext::QUEUE_PRESENT, queues.present[0], true);

	for (int id = 0; id < GraphicContext::QUEUE_COMPUTE_NUM; id++) {
		bool with_mutex = (GraphicContext::QUEUE_COMPUTE_NUM == queues.compute.size());
		get_queue(GraphicContext::QUEUE_COMPUTE_START + id,
		          queues.compute[id % queues.compute.size()], with_mutex);
	}

	for (int id = 0; id < GraphicContext::QUEUES_NUM; id++) {
		auto& queue = ctx->queues[id];
		if (queue.vk_queue == nullptr) {
			continue;
		}

		if (queue.mutex == nullptr) {
			queue.mutex = new Common::Mutex;
		}

		for (int other_id = 0; other_id < id; other_id++) {
			auto& other = ctx->queues[other_id];
			if (other.vk_queue != queue.vk_queue) {
				continue;
			}

			if (other.mutex == nullptr && queue.mutex != nullptr) {
				other.mutex = queue.mutex;
			} else {
				queue.mutex = other.mutex;
			}
			LOGF("Vulkan queue: sharing mutex for queue ids %d and %d\n", other_id, id);
			break;
		}
	}
}

static void VulkanCheckInstanceVersion() {
	uint32_t version = VK_API_VERSION_1_0;

	auto enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
	    vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
	if (enumerate_instance_version != nullptr) {
		auto result = enumerate_instance_version(&version);
		if (result != VK_SUCCESS) {
			EXIT("Could not query Vulkan loader version: %s\n", string_VkResult(result));
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

void VulkanCreate(WindowContext* ctx) {
	EXIT_IF(ctx->window == nullptr);
	EXIT_IF(ctx->graphic_ctx.instance != nullptr);
	EXIT_IF(ctx->graphic_ctx.physical_device != nullptr);
	EXIT_IF(ctx->graphic_ctx.device != nullptr);
	EXIT_IF(ctx->surface_capabilities != nullptr);

	VulkanExtensions r;
	VulkanGetExtensions(ctx->window, &r);
	VulkanCheckInstanceVersion();

	VkApplicationInfo app_info {};
	app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pNext              = nullptr;
	app_info.pApplicationName   = "Kyty";
	app_info.applicationVersion = 1;
	app_info.pEngineName        = "Kyty";
	app_info.engineVersion      = 1;
	app_info.apiVersion         = VULKAN_TARGET_API_VERSION; // NOLINT

	VkValidationFeatureEnableEXT enabled_features[] = {
#ifdef KYTY_ENABLE_BEST_PRACTICES
	    VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
#endif
#ifdef KYTY_ENABLE_DEBUG_PRINTF
	    VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
#endif
	};

	uint32_t enabled_features_count =
	    sizeof(enabled_features) / sizeof(VkValidationFeatureEnableEXT);

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (!Config::SpirvDebugPrintfEnabled()) {
		enabled_features_count--;
	}
#endif

	VkValidationFeaturesEXT validation_features {};
	validation_features.sType                          = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
	validation_features.pNext                          = nullptr;
	validation_features.enabledValidationFeatureCount  = enabled_features_count;
	validation_features.pEnabledValidationFeatures     = enabled_features;
	validation_features.disabledValidationFeatureCount = 0;
	validation_features.pDisabledValidationFeatures    = nullptr;

	VkDebugUtilsMessengerCreateInfoEXT dbg_create_info {};
	dbg_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dbg_create_info.pNext = &validation_features;
	dbg_create_info.flags = 0;
	dbg_create_info.messageSeverity =
	    static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) |
	    static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) |
	    static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) |
	    static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
	dbg_create_info.messageType =
	    static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) |
	    static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) |
	    static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
	dbg_create_info.pfnUserCallback = VulkanDebugMessengerCallback;
	dbg_create_info.pUserData       = nullptr;

	VkInstanceCreateInfo inst_info {};
	inst_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	inst_info.pNext                   = (r.enable_validation_layers ? &dbg_create_info : nullptr);
	inst_info.flags                   = 0;
	inst_info.pApplicationInfo        = &app_info;
	inst_info.enabledExtensionCount   = static_cast<uint32_t>(r.required_extensions.size());
	inst_info.ppEnabledExtensionNames = r.required_extensions.data();
	inst_info.enabledLayerCount =
	    (r.enable_validation_layers ? static_cast<uint32_t>(r.required_layers.size()) : 0);
	inst_info.ppEnabledLayerNames =
	    (r.enable_validation_layers ? r.required_layers.data() : nullptr);

	const VkResult result = vkCreateInstance(&inst_info, nullptr, &ctx->graphic_ctx.instance);
	switch (result) {
		case VK_SUCCESS: break;
		case VK_ERROR_INCOMPATIBLE_DRIVER: EXIT("Unable to find a compatible Vulkan Driver");
		default: EXIT("Could not create a Vulkan instance (for unknown reasons)");
	}

	if (r.enable_validation_layers) {
		dbg_create_info.pNext = nullptr;
		if (VulkanCreateDebugUtilsMessengerEXT(ctx->graphic_ctx.instance, &dbg_create_info, nullptr,
		                                       &ctx->graphic_ctx.debug_messenger) != VK_SUCCESS) {
			EXIT("Could not create debug messenger");
		}
	}

	if (SDL_Vulkan_CreateSurface(ctx->window, ctx->graphic_ctx.instance, &ctx->surface) ==
	    SDL_FALSE) {
		EXIT("Could not create a Vulkan surface");
	}

	std::vector<const char*> device_extensions = {
	    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
	    VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME,
	    "VK_KHR_maintenance1"};

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled()) {
		device_extensions.push_back(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	}
#endif

	ctx->surface_capabilities = new SurfaceCapabilities {};

	VulkanQueues queues;

	VulkanFindPhysicalDevice(ctx->graphic_ctx.instance, ctx->surface, device_extensions,
	                         ctx->surface_capabilities, &ctx->graphic_ctx.physical_device, &queues);

	if (ctx->graphic_ctx.physical_device == nullptr) {
		EXIT("Could not find suitable device");
	}

	vkGetPhysicalDeviceProperties(ctx->graphic_ctx.physical_device,
	                              &ctx->graphic_ctx.physical_device_properties);
	const auto& device_properties = ctx->graphic_ctx.GetPhysicalDeviceProperties();

	LOGF("Select device: %s\n", device_properties.deviceName);

	{
		uint32_t extensions_count = 0;
		vkEnumerateDeviceExtensionProperties(ctx->graphic_ctx.physical_device, nullptr,
		                                     &extensions_count, nullptr);

		std::vector<VkExtensionProperties> available_extensions(extensions_count);
		vkEnumerateDeviceExtensionProperties(ctx->graphic_ctx.physical_device, nullptr,
		                                     &extensions_count, available_extensions.data());

		if (HasExtension(available_extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
			ctx->graphic_ctx.memory_budget_ext_enabled = true;
		}
		if (HasExtension(available_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
		}
	}

	memcpy(ctx->device_name, device_properties.deviceName, sizeof(ctx->device_name));
	std::snprintf(ctx->processor_name, sizeof(ctx->processor_name), "%s",
	              Common::GetSystemInfo().ProcessorName.c_str());

	VulkanInitSubgroupSizeControl(ctx->graphic_ctx.physical_device, &ctx->graphic_ctx);

	ctx->graphic_ctx.device = VulkanCreateDevice(ctx->graphic_ctx.physical_device, ctx->surface, &r,
	                                             queues, device_extensions, &ctx->graphic_ctx);
	if (ctx->graphic_ctx.device == nullptr) {
		EXIT("Could not create device");
	}

	if (!VulkanCreateAllocator(&ctx->graphic_ctx)) {
		EXIT("Could not create Vulkan memory allocator");
	}

	VulkanCreateQueues(&ctx->graphic_ctx, queues);

	ctx->swapchain = VulkanCreateSwapchain(&ctx->graphic_ctx, 2);
	RenderDocSetActiveWindow(ctx->graphic_ctx.instance, ctx->window);
}

} // namespace Libs::Graphics
