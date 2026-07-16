#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANINSTANCE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANINSTANCE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

inline constexpr uint32_t VULKAN_TARGET_API_VERSION = VK_API_VERSION_1_3;

struct VulkanQueueInfo {
	Common::Mutex* mutex    = nullptr;
	uint32_t       family   = static_cast<uint32_t>(-1);
	uint32_t       index    = static_cast<uint32_t>(-1);
	VkQueue        vk_queue = nullptr;
};

struct VulkanInstance {
	static constexpr int QUEUES_NUM          = 11;
	static constexpr int QUEUE_GFX           = 8;
	static constexpr int QUEUE_GFX_NUM       = 1;
	static constexpr int QUEUE_UTIL          = 9;
	static constexpr int QUEUE_UTIL_NUM      = 1;
	static constexpr int QUEUE_PRESENT       = 10;
	static constexpr int QUEUE_PRESENT_NUM   = 1;
	static constexpr int QUEUE_COMPUTE_START = 0;
	static constexpr int QUEUE_COMPUTE_NUM   = 8;

	VkInstance                 instance                      = nullptr;
	VkDebugUtilsMessengerEXT   debug_messenger               = nullptr;
	VkPhysicalDevice           physical_device               = nullptr;
	VkPhysicalDeviceProperties physical_device_properties    = {};
	VkDevice                   device                        = nullptr;
	VmaAllocator               allocator                     = nullptr;
	bool                       memory_budget_ext_enabled     = false;
	bool                       subgroup_size_control_enabled = false;
	uint32_t                   subgroup_size                 = 0;
	uint32_t                   min_subgroup_size             = 0;
	uint32_t                   max_subgroup_size             = 0;
	VkShaderStageFlags         required_subgroup_size_stages = 0;
	VulkanQueueInfo            queues[QUEUES_NUM];

	[[nodiscard]] const VkPhysicalDeviceProperties& GetPhysicalDeviceProperties() const {
		return physical_device_properties;
	}

	[[nodiscard]] VkDeviceSize StorageMinAlignment() const {
		const auto alignment = physical_device_properties.limits.minStorageBufferOffsetAlignment;
		return alignment != 0 ? alignment : 1;
	}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANINSTANCE_H_
