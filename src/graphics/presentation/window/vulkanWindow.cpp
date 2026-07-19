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
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
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

void VulkanGetSurfaceCapabilities(vk::PhysicalDevice physical_device, vk::SurfaceKHR surface,
                                  SurfaceCapabilities* r) {
	RequireVulkanSuccess(physical_device.getSurfaceCapabilitiesKHR(surface, &r->capabilities),
	                     "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

	r->formats = EnumerateVulkan<vk::SurfaceFormatKHR>( // @suppress("Ambiguous problem")
	    "vkGetPhysicalDeviceSurfaceFormatsKHR", [&](uint32_t* count, vk::SurfaceFormatKHR* values) {
		    return physical_device.getSurfaceFormatsKHR(surface, count, values);
	    });
	EXIT_NOT_IMPLEMENTED(r->formats.empty());

	r->present_modes = EnumerateVulkan<vk::PresentModeKHR>( // @suppress("Ambiguous problem")
	    "vkGetPhysicalDeviceSurfacePresentModesKHR",
	    [&](uint32_t* count, vk::PresentModeKHR* values) {
		    return physical_device.getSurfacePresentModesKHR(surface, count, values);
	    });
	EXIT_NOT_IMPLEMENTED(r->present_modes.empty());

	r->format_srgb_bgra32  = false;
	r->format_unorm_bgra32 = false;
	for (const auto& f: r->formats) {
		if (f.format == vk::Format::eB8G8R8A8Srgb &&
		    f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
			r->format_srgb_bgra32 = true;
		}
		if (f.format == vk::Format::eB8G8R8A8Unorm &&
		    f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
			r->format_unorm_bgra32 = true;
		}
		if (r->format_srgb_bgra32 && r->format_unorm_bgra32) {
			break;
		}
	}
}

static bool CheckFormat(vk::PhysicalDevice device, vk::Format format, bool tile,
                        vk::FormatFeatureFlags features) {
	vk::FormatProperties format_props {};
	device.getFormatProperties(format, &format_props);

	const auto supported_features =
	    (tile ? format_props.optimalTilingFeatures : format_props.linearTilingFeatures);
	return (supported_features & features) == features;
}

struct QueueInfo {
	uint32_t family   = 0;
	uint32_t index    = 0;
	bool     graphics = false;
	bool     compute  = false;
	bool     present  = false;
};

struct VulkanQueues {
	uint32_t               family_count = 0;
	std::vector<uint32_t>  family_used;
	std::vector<QueueInfo> available;
	std::vector<QueueInfo> graphics;
	std::vector<QueueInfo> compute;
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
	LOGF("\t present:\n");
	for (const auto& q: qs.present) {
		LOGF("\t\t family = %u, index = %u\n", q.family, q.index);
	}
}

static VulkanQueues VulkanFindQueues(vk::PhysicalDevice device, vk::SurfaceKHR surface,
                                     uint32_t graphics_num, uint32_t compute_num,
                                     uint32_t present_num) {
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);

	VulkanQueues qs;

	uint32_t queue_family_count = 0;
	device.getQueueFamilyProperties(&queue_family_count, nullptr);
	std::vector<vk::QueueFamilyProperties> queue_families(queue_family_count);
	device.getQueueFamilyProperties(&queue_family_count, queue_families.data());

	qs.family_count = queue_family_count;

	uint32_t family = 0;
	for (auto& f: queue_families) {
		vk::Bool32 presentation_supported = VK_FALSE;
		RequireVulkanSuccess(device.getSurfaceSupportKHR(family, surface, &presentation_supported),
		                     "vkGetPhysicalDeviceSurfaceSupportKHR");

		LOGF("\tqueue family: %s [count = %u], [present = %s]\n",
		     VulkanToString(f.queueFlags).c_str(), f.queueCount,
		     (presentation_supported == VK_TRUE ? "true" : "false"));

		for (uint32_t i = 0; i < f.queueCount; i++) {
			QueueInfo info;
			info.family   = family;
			info.index    = i;
			info.graphics = static_cast<bool>(f.queueFlags & vk::QueueFlagBits::eGraphics);
			info.compute  = static_cast<bool>(f.queueFlags & vk::QueueFlagBits::eCompute);
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

	const uint32_t graphics_family =
	    qs.graphics.empty() ? static_cast<uint32_t>(-1) : qs.graphics.front().family;
	select_queues(
	    compute_num,
	    [graphics_family](const auto& q) { return q.compute && q.family == graphics_family; },
	    qs.compute);
	if (compute_num != 0 && qs.compute.empty()) {
		auto graphics_compute = std::find_if(qs.graphics.begin(), qs.graphics.end(),
		                                     [](const auto& q) { return q.compute; });
		if (graphics_compute != qs.graphics.end()) {
			// Reuse the universal graphics queue when Intel GPUs expose no spare compute queue.
			qs.compute.push_back(*graphics_compute);
		}
	}

	select_queues(present_num, [](const auto& q) { return q.present; }, qs.present);

	return qs;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void VulkanFindPhysicalDevice(vk::Instance instance, vk::SurfaceKHR surface,
                                     const std::vector<const char*>& device_extensions,
                                     SurfaceCapabilities*            out_capabilities,
                                     vk::PhysicalDevice* out_device, VulkanQueues* out_queues) {
	EXIT_IF(instance == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(out_capabilities == nullptr);
	EXIT_IF(out_device == nullptr);
	EXIT_IF(out_queues == nullptr);

	auto devices = EnumerateVulkan<vk::PhysicalDevice>(
	    "vkEnumeratePhysicalDevices", [&](uint32_t* count, vk::PhysicalDevice* values) {
		    return instance.enumeratePhysicalDevices(count, values);
	    });
	EXIT_NOT_IMPLEMENTED(devices.empty());

	vk::PhysicalDevice  best_device = nullptr;
	VulkanQueues        best_queues;
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

		vk::PhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
		depth_clip_control.sType = vk::StructureType::ePhysicalDeviceDepthClipControlFeaturesEXT;
		depth_clip_control.pNext = &color_write_ext;

		vk::PhysicalDeviceVulkan12Features features12 {};
		features12.sType = vk::StructureType::ePhysicalDeviceVulkan12Features;
		features12.pNext = &depth_clip_control;
		features13.pNext = &features12;

		device_features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
		device_features2.pNext = &features13;

		device.getFeatures2(&device_features2);

		auto qs =
		    VulkanFindQueues(device, surface, GraphicContext::QUEUE_GFX_NUM,
		                     GraphicContext::QUEUE_COMPUTE_NUM, GraphicContext::QUEUE_PRESENT_NUM);

		VulkanDumpQueues(qs);

		if (qs.graphics.size() != GraphicContext::QUEUE_GFX_NUM ||
		    !(qs.compute.size() >= 1 && qs.compute.size() <= GraphicContext::QUEUE_COMPUTE_NUM) ||
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
			VulkanGetSurfaceCapabilities(device, surface, &candidate_capabilities);

			if (!(candidate_capabilities.capabilities.supportedUsageFlags &
			      vk::ImageUsageFlagBits::eTransferDst)) {
				LOGF("Surface cannot be destination of blit\n");
				skip_device = true;
			}
		}

		if (!skip_device && !CheckFormat(device, vk::Format::eR8G8B8A8Srgb, false,
		                                 vk::FormatFeatureFlagBits::eBlitSrc)) {
			LOGF("Format vk::Format::eR8G8B8A8Srgb cannot be used as transfer source\n");
			skip_device = true;
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
			best_queues       = qs;
			best_capabilities = std::move(candidate_capabilities);
		}
	}

	*out_device = best_device;
	*out_queues = best_queues;
	if (best_device != nullptr) {
		*out_capabilities = std::move(best_capabilities);
	}
}

static void VulkanInitSubgroupSizeControl(vk::PhysicalDevice physical_device, GraphicContext* ctx) {
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(ctx == nullptr);

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

	ctx->subgroup_size                 = properties11.subgroupSize;
	ctx->min_subgroup_size             = subgroup_size_control.minSubgroupSize;
	ctx->max_subgroup_size             = subgroup_size_control.maxSubgroupSize;
	ctx->required_subgroup_size_stages = subgroup_size_control.requiredSubgroupSizeStages;
	ctx->subgroup_size_control_enabled = features13.subgroupSizeControl == VK_TRUE &&
	                                     subgroup_size_control.minSubgroupSize <= 64 &&
	                                     subgroup_size_control.maxSubgroupSize >= 64;

	LOGF("Vulkan subgroup: default=%u min=%u max=%u stages=0x%08x size_control=%s\n",
	     ctx->subgroup_size, ctx->min_subgroup_size, ctx->max_subgroup_size,
	     static_cast<vk::ShaderStageFlags::MaskType>(ctx->required_subgroup_size_stages),
	     ctx->subgroup_size_control_enabled ? "true" : "false");
}

static vk::Device VulkanCreateDevice(vk::PhysicalDevice physical_device, vk::SurfaceKHR surface,
                                     const VulkanExtensions* r, const VulkanQueues& queues,
                                     const std::vector<const char*>& device_extensions,
                                     const GraphicContext*           ctx) {
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(r == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(ctx == nullptr);

	std::vector<vk::DeviceQueueCreateInfo> queue_create_info(queues.family_count);
	std::vector<std::vector<float>>        queue_priority(queues.family_count);
	uint32_t                               queue_create_info_num = 0;

	for (uint32_t i = 0; i < queues.family_count; i++) {
		if (queues.family_used[i] != 0) {
			for (uint32_t pi = 0; pi < queues.family_used[i]; pi++) {
				queue_priority[queue_create_info_num].push_back(1.0f);
			}

			queue_create_info[queue_create_info_num].sType =
			    vk::StructureType::eDeviceQueueCreateInfo;
			queue_create_info[queue_create_info_num].pNext            = nullptr;
			queue_create_info[queue_create_info_num].flags            = {};
			queue_create_info[queue_create_info_num].queueFamilyIndex = i;
			queue_create_info[queue_create_info_num].queueCount       = queues.family_used[i];
			queue_create_info[queue_create_info_num].pQueuePriorities =
			    queue_priority[queue_create_info_num].data();

			queue_create_info_num++;
		}
	}

	vk::PhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
	color_write_ext.sType = vk::StructureType::ePhysicalDeviceColorWriteEnableFeaturesEXT;
	color_write_ext.pNext = nullptr;
	color_write_ext.colorWriteEnable = VK_TRUE;

	vk::PhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
	depth_clip_control.sType = vk::StructureType::ePhysicalDeviceDepthClipControlFeaturesEXT;
	depth_clip_control.pNext = &color_write_ext;
	depth_clip_control.depthClipControl = VK_TRUE;

	vk::PhysicalDeviceVulkan12Features features12 {};
	features12.sType                    = vk::StructureType::ePhysicalDeviceVulkan12Features;
	features12.pNext                    = &depth_clip_control;
	features12.samplerMirrorClampToEdge = VK_TRUE;

	vk::PhysicalDeviceSubgroupSizeControlFeatures subgroup_size_control {};
	subgroup_size_control.sType = vk::StructureType::ePhysicalDeviceSubgroupSizeControlFeatures;
	subgroup_size_control.pNext = &features12;

	vk::PhysicalDeviceVulkan13Features supported_features13 {};
	supported_features13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;
	supported_features13.pNext = nullptr;

	const auto robustness2_ext_enabled =
	    HasExtension(device_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

	vk::PhysicalDeviceRobustness2FeaturesEXT supported_robustness2 {};
	supported_robustness2.sType = vk::StructureType::ePhysicalDeviceRobustness2FeaturesEXT;
	supported_robustness2.pNext = nullptr;
	if (robustness2_ext_enabled) {
		supported_features13.pNext = &supported_robustness2;
	}

	vk::PhysicalDeviceFeatures2 supported_features2 {};
	supported_features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
	supported_features2.pNext = &supported_features13;
	physical_device.getFeatures2(&supported_features2);

	vk::PhysicalDeviceFeatures device_features {};
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

	vk::PhysicalDeviceRobustness2FeaturesEXT robustness2 {};
	robustness2.sType = vk::StructureType::ePhysicalDeviceRobustness2FeaturesEXT;
	robustness2.pNext = const_cast<void*>(base_feature_chain);
	if (robustness2_ext_enabled) {
		robustness2.robustBufferAccess2 = supported_robustness2.robustBufferAccess2;
		robustness2.robustImageAccess2  = supported_robustness2.robustImageAccess2;
		robustness2.nullDescriptor      = supported_robustness2.nullDescriptor;
	}

	vk::PhysicalDeviceVulkan13Features features13 {};
	features13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;
	features13.pNext =
	    robustness2_ext_enabled ? &robustness2 : const_cast<void*>(base_feature_chain);
	features13.robustImageAccess = supported_features13.robustImageAccess;

	LOGF("Vulkan robustness: robustImageAccess=%s robustImageAccess2=%s\n",
	     features13.robustImageAccess == VK_TRUE ? "true" : "false",
	     robustness2_ext_enabled && robustness2.robustImageAccess2 == VK_TRUE ? "true" : "false");

	vk::DeviceCreateInfo create_info {};
	create_info.sType                = vk::StructureType::eDeviceCreateInfo;
	create_info.pNext                = &features13;
	create_info.flags                = {};
	create_info.pQueueCreateInfos    = queue_create_info.data();
	create_info.queueCreateInfoCount = queue_create_info_num;
	create_info.enabledLayerCount =
	    (r->enable_validation_layers ? static_cast<uint32_t>(r->required_layers.size()) : 0);
	create_info.ppEnabledLayerNames =
	    (r->enable_validation_layers ? r->required_layers.data() : nullptr);
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

static void VulkanGetExtensions(SDL_Window* window, VulkanExtensions* r) {
	EXIT_IF(window == nullptr);
	EXIT_IF(r == nullptr);

	uint32_t required_extensions_count = 0;

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

	r->available_extensions =
	    EnumerateVulkan<vk::ExtensionProperties>( // @suppress("Ambiguous problem")
	        "vkEnumerateInstanceExtensionProperties",
	        [](uint32_t* count, vk::ExtensionProperties* values) {
		        return vk::enumerateInstanceExtensionProperties(nullptr, count, values);
	        });

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
		LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName.data(),
		     ext.specVersion);
	}

	r->available_layers = EnumerateVulkan<vk::LayerProperties>( // @suppress("Ambiguous problem")
	    "vkEnumerateInstanceLayerProperties", [](uint32_t* count, vk::LayerProperties* values) {
		    return vk::enumerateInstanceLayerProperties(count, values);
	    });

	for (const auto& l: r->available_layers) {
		LOGF("Vulkan available layer: %s, specVersion = %u, implVersion = %u, %s\n",
		     l.layerName.data(), l.specVersion, l.implementationVersion, l.description.data());
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
			r->required_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		} else {
			r->enable_validation_layers = false;
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

static void VulkanCreateQueues(GraphicContext* ctx, const VulkanQueues& queues) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(queues.graphics.size() != 1);
	EXIT_IF(queues.present.size() != 1);
	EXIT_IF(!(queues.compute.size() >= 1 &&
	          queues.compute.size() <= GraphicContext::QUEUE_COMPUTE_NUM));

	auto get_queue = [ctx](int id, const QueueInfo& info) {
		ctx->queues[id].family = info.family;
		ctx->queues[id].index  = info.index;
		EXIT_IF(ctx->queues[id].vk_queue != nullptr);
		ctx->device.getQueue(ctx->queues[id].family, ctx->queues[id].index,
		                     &ctx->queues[id].vk_queue);
		EXIT_NOT_IMPLEMENTED(ctx->queues[id].vk_queue == nullptr);
	};

	get_queue(GraphicContext::QUEUE_GFX, queues.graphics[0]);
	ctx->queues[GraphicContext::QUEUE_UTIL].family = ctx->queues[GraphicContext::QUEUE_GFX].family;
	ctx->queues[GraphicContext::QUEUE_UTIL].index  = ctx->queues[GraphicContext::QUEUE_GFX].index;
	ctx->queues[GraphicContext::QUEUE_UTIL].vk_queue =
	    ctx->queues[GraphicContext::QUEUE_GFX].vk_queue;
	LOGF("Vulkan queue: using graphics queue for utility submissions to preserve resource "
	     "ordering\n");
	get_queue(GraphicContext::QUEUE_PRESENT, queues.present[0]);

	for (int id = 0; id < GraphicContext::QUEUE_COMPUTE_NUM; id++) {
		get_queue(GraphicContext::QUEUE_COMPUTE_START + id,
		          queues.compute[id % queues.compute.size()]);
	}

	for (int id = 0; id < GraphicContext::QUEUES_NUM; id++) {
		auto& queue = ctx->queues[id];
		if (queue.vk_queue == nullptr) {
			continue;
		}
		EXIT_IF(queue.mutex != nullptr);

		for (int other_id = 0; other_id < id; other_id++) {
			auto& other = ctx->queues[other_id];
			if (other.vk_queue != queue.vk_queue) {
				continue;
			}

			EXIT_IF(other.mutex == nullptr);
			queue.mutex = other.mutex;
			LOGF("Vulkan queue: sharing mutex for queue ids %d and %d\n", other_id, id);
			break;
		}

		if (queue.mutex == nullptr) {
			queue.mutex = &ctx->queue_mutexes[id];
		}
	}
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

void VulkanCreate(WindowContext* ctx) {
	EXIT_IF(ctx->window == nullptr);
	EXIT_IF(ctx->graphic_ctx.instance != nullptr);
	EXIT_IF(ctx->graphic_ctx.physical_device != nullptr);
	EXIT_IF(ctx->graphic_ctx.device != nullptr);
	EXIT_IF(ctx->surface_capabilities != nullptr);

	auto get_instance_proc_addr =
	    reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
	if (get_instance_proc_addr == nullptr) {
		EXIT("Could not load Vulkan: %s\n", SDL_GetError());
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

	VulkanExtensions r;
	VulkanGetExtensions(ctx->window, &r);
	VulkanCheckInstanceVersion();

	vk::ApplicationInfo app_info {};
	app_info.sType              = vk::StructureType::eApplicationInfo;
	app_info.pNext              = nullptr;
	app_info.pApplicationName   = "Kyty";
	app_info.applicationVersion = 1;
	app_info.pEngineName        = "Kyty";
	app_info.engineVersion      = 1;
	app_info.apiVersion         = VULKAN_TARGET_API_VERSION; // NOLINT

	vk::ValidationFeatureEnableEXT enabled_features[] = {
#ifdef KYTY_ENABLE_BEST_PRACTICES
	    vk::ValidationFeatureEnableEXT::eBestPractices,
#endif
#ifdef KYTY_ENABLE_DEBUG_PRINTF
	    vk::ValidationFeatureEnableEXT::eDebugPrintf,
#endif
	};

	uint32_t enabled_features_count =
	    sizeof(enabled_features) / sizeof(vk::ValidationFeatureEnableEXT);

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (!Config::SpirvDebugPrintfEnabled()) {
		enabled_features_count--;
	}
#endif

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

	const vk::Result result = vk::createInstance(&inst_info, nullptr, &ctx->graphic_ctx.instance);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eErrorIncompatibleDriver:
			EXIT("Unable to find a compatible Vulkan Driver");
		default: EXIT("Could not create a Vulkan instance (for unknown reasons)");
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(ctx->graphic_ctx.instance);

	if (r.enable_validation_layers) {
		dbg_create_info.pNext = nullptr;
		if (VulkanCreateDebugUtilsMessengerEXT(ctx->graphic_ctx.instance, &dbg_create_info, nullptr,
		                                       &ctx->graphic_ctx.debug_messenger) !=
		    vk::Result::eSuccess) {
			EXIT("Could not create debug messenger");
		}
	}

	vk::SurfaceKHR::CType native_surface = VK_NULL_HANDLE;
	if (SDL_Vulkan_CreateSurface(ctx->window,
	                             static_cast<vk::Instance::CType>(ctx->graphic_ctx.instance),
	                             &native_surface) == SDL_FALSE) {
		EXIT("Could not create a Vulkan surface");
	}
	ctx->surface = native_surface;

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

	ctx->graphic_ctx.physical_device.getProperties(&ctx->graphic_ctx.physical_device_properties);
	ctx->graphic_ctx.physical_device.getMemoryProperties(
	    &ctx->graphic_ctx.physical_device_memory_properties);
	const auto& device_properties = ctx->graphic_ctx.GetPhysicalDeviceProperties();

	LOGF("Select device: %s\n", device_properties.deviceName.data());

	{
		auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
		    "vkEnumerateDeviceExtensionProperties",
		    [&](uint32_t* count, vk::ExtensionProperties* values) {
			    return ctx->graphic_ctx.physical_device.enumerateDeviceExtensionProperties(
			        nullptr, count, values);
		    });

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
	VULKAN_HPP_DEFAULT_DISPATCHER.init(ctx->graphic_ctx.device);

	if (!VulkanCreateAllocator(&ctx->graphic_ctx)) {
		EXIT("Could not create Vulkan memory allocator");
	}

	VulkanCreateQueues(&ctx->graphic_ctx, queues);

	ctx->swapchain = VulkanCreateSwapchain(&ctx->graphic_ctx, 2);
	RenderDocSetActiveWindow(ctx->graphic_ctx.instance, ctx->window);
}

} // namespace Libs::Graphics
