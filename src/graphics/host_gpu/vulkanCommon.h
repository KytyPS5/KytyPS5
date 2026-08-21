#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANCOMMON_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANCOMMON_H_

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC    1
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/gpu_defs.h"

#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_hash.hpp>

namespace Libs::Graphics {

using VulkanMemoryBarrier = vk::MemoryBarrier;

std::string VulkanToString(vk::Result value);
std::string VulkanToString(vk::Format value);
std::string VulkanToString(vk::ImageLayout value);
std::string VulkanToString(vk::QueueFlags value);
vk::Format  VulkanFormat(Prospero::BufferFormat guest_format);
void        RequireVulkanSuccess(vk::Result result, const char* operation);

template <typename Handle, typename... Args>
void SetVulkanObjectNameF(vk::Device device, Handle handle, fmt::format_string<Args...> format,
                          Args&&... args) {
	if (!Config::GraphicsDebugDumpEnabled() || device == nullptr || handle == nullptr ||
	    VULKAN_HPP_DEFAULT_DISPATCHER.vkSetDebugUtilsObjectNameEXT == nullptr) {
		return;
	}

	const auto                      name = fmt::format(format, std::forward<Args>(args)...);
	vk::DebugUtilsObjectNameInfoEXT info {};
	info.sType        = vk::StructureType::eDebugUtilsObjectNameInfoEXT;
	info.objectType   = Handle::objectType;
	info.objectHandle = static_cast<uint64_t>(
	    reinterpret_cast<uintptr_t>(static_cast<typename Handle::CType>(handle)));
	info.pObjectName = name.c_str();
	(void)device.setDebugUtilsObjectNameEXT(&info);
}

template <typename T, typename Enumerator>
[[nodiscard]] std::vector<T> EnumerateVulkan(const char* operation, Enumerator&& enumerate) {
	for (;;) {
		uint32_t count = 0;
		RequireVulkanSuccess(enumerate(&count, nullptr), operation);
		if (count == 0) {
			return {};
		}

		std::vector<T> values(count);
		const auto     result = enumerate(&count, values.data());
		if (result == vk::Result::eSuccess) {
			values.resize(count);
			return values;
		}
		if (result != vk::Result::eIncomplete) {
			RequireVulkanSuccess(result, operation);
		}
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANCOMMON_H_
