#include "graphics/rt/hardware.h"

#include "common/assert.h"
#include "common/logging/log.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics::Rt {
namespace {

bool ExtensionAvailable(const std::vector<VkExtensionProperties>& available, const char* name) {
	return std::ranges::any_of(available, [name](const auto& extension) {
		return std::strcmp(extension.extensionName, name) == 0;
	});
}

void AppendExtensionIfMissing(std::vector<const char*>& extensions, const char* name) {
	if (std::ranges::none_of(extensions, [name](const char* extension) {
		    return std::strcmp(extension, name) == 0;
	    })) {
		extensions.push_back(name);
	}
}

} // namespace

void AppendHardwareRayTracingDeviceExtensions(
    const std::vector<VkExtensionProperties>& available_extensions,
    std::vector<const char*>* device_extensions, GraphicContext* ctx) {
	EXIT_IF(device_extensions == nullptr || ctx == nullptr);

	const char* required[] = {
	    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
	    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
	    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	    VK_KHR_RAY_QUERY_EXTENSION_NAME,
	};
	for (const auto* extension: required) {
		if (!ExtensionAvailable(available_extensions, extension)) {
			LOGF("Vulkan RT: extension %s is unavailable; hardware ray tracing disabled\n",
			     extension);
			ctx->rt_extensions_enabled = false;
			return;
		}
	}
	for (const auto* extension: required) {
		AppendExtensionIfMissing(*device_extensions, extension);
	}
	if (ExtensionAvailable(available_extensions, VK_KHR_SPIRV_1_4_EXTENSION_NAME)) {
		AppendExtensionIfMissing(*device_extensions, VK_KHR_SPIRV_1_4_EXTENSION_NAME);
	}
	if (ExtensionAvailable(available_extensions, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) {
		AppendExtensionIfMissing(*device_extensions, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
	}

	ctx->rt_extensions_enabled = true;
	LOGF("Vulkan RT: enabling hardware ray query extensions\n");
}

void LoadHardwareRayTracingFunctions(GraphicContext* ctx) {
	EXIT_IF(ctx == nullptr || ctx->device == nullptr);
	if (!ctx->rt_extensions_enabled) {
		return;
	}

	ctx->vkGetBufferDeviceAddressKHR = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
	    vkGetDeviceProcAddr(ctx->device, "vkGetBufferDeviceAddressKHR"));
	if (ctx->vkGetBufferDeviceAddressKHR == nullptr) {
		ctx->vkGetBufferDeviceAddressKHR = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
		    vkGetDeviceProcAddr(ctx->device, "vkGetBufferDeviceAddress"));
	}
	ctx->vkCreateAccelerationStructureKHR = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
	    vkGetDeviceProcAddr(ctx->device, "vkCreateAccelerationStructureKHR"));
	ctx->vkDestroyAccelerationStructureKHR =
	    reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
	        vkGetDeviceProcAddr(ctx->device, "vkDestroyAccelerationStructureKHR"));
	ctx->vkGetAccelerationStructureBuildSizesKHR =
	    reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
	        vkGetDeviceProcAddr(ctx->device, "vkGetAccelerationStructureBuildSizesKHR"));
	ctx->vkCmdBuildAccelerationStructuresKHR =
	    reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
	        vkGetDeviceProcAddr(ctx->device, "vkCmdBuildAccelerationStructuresKHR"));
	ctx->vkGetAccelerationStructureDeviceAddressKHR =
	    reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
	        vkGetDeviceProcAddr(ctx->device, "vkGetAccelerationStructureDeviceAddressKHR"));

	if (ctx->vkGetBufferDeviceAddressKHR == nullptr ||
	    ctx->vkCreateAccelerationStructureKHR == nullptr ||
	    ctx->vkDestroyAccelerationStructureKHR == nullptr ||
	    ctx->vkGetAccelerationStructureBuildSizesKHR == nullptr ||
	    ctx->vkCmdBuildAccelerationStructuresKHR == nullptr ||
	    ctx->vkGetAccelerationStructureDeviceAddressKHR == nullptr) {
		EXIT("Vulkan RT: failed to load required device functions\n");
	}
}

} // namespace Libs::Graphics::Rt
