#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANCOMMON_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANCOMMON_H_

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC    1
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_hash.hpp>

namespace Libs::Graphics {

using VulkanMemoryBarrier = vk::MemoryBarrier;

template <typename Handle>
[[nodiscard]] void* VulkanHandleToPointer(Handle handle) {
	return reinterpret_cast<void*>(static_cast<typename Handle::CType>(handle));
}

std::string VulkanToString(vk::Result value);
std::string VulkanToString(vk::Format value);
std::string VulkanToString(vk::ImageLayout value);
std::string VulkanToString(vk::QueueFlags value);
vk::Format  VulkanFormat(uint32_t guest_format);
void        RequireVulkanSuccess(vk::Result result, const char* operation);

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
