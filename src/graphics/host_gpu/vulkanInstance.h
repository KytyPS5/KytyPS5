#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANINSTANCE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANINSTANCE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <map>
#include <mutex>
#include <tuple>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

inline constexpr uint32_t VULKAN_TARGET_API_VERSION = VK_API_VERSION_1_3;

struct VulkanQueueInfo {
	Common::Mutex* mutex    = nullptr;
	uint32_t       family   = static_cast<uint32_t>(-1);
	uint32_t       index    = static_cast<uint32_t>(-1);
	vk::Queue      vk_queue = nullptr;
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

	vk::Instance                       instance                          = nullptr;
	vk::DebugUtilsMessengerEXT         debug_messenger                   = nullptr;
	vk::PhysicalDevice                 physical_device                   = nullptr;
	vk::PhysicalDeviceProperties       physical_device_properties        = {};
	vk::PhysicalDeviceMemoryProperties physical_device_memory_properties = {};
	vk::Device                         device                            = nullptr;
	VmaAllocator                       allocator                         = nullptr;
	bool                               memory_budget_ext_enabled         = false;
	bool                               rt_extensions_enabled             = false;
	bool                               subgroup_size_control_enabled     = false;
	uint32_t                           subgroup_size                     = 0;
	uint32_t                           min_subgroup_size                 = 0;
	uint32_t                           max_subgroup_size                 = 0;
	vk::ShaderStageFlags               required_subgroup_size_stages     = {};
	VulkanQueueInfo                    queues[QUEUES_NUM];

	[[nodiscard]] const vk::PhysicalDeviceProperties& GetPhysicalDeviceProperties() const {
		return physical_device_properties;
	}

	[[nodiscard]] const vk::PhysicalDeviceMemoryProperties&
	GetPhysicalDeviceMemoryProperties() const {
		return physical_device_memory_properties;
	}

	[[nodiscard]] vk::FormatProperties GetFormatProperties(vk::Format format) const {
		std::scoped_lock lock(m_format_properties_mutex);
		auto [it, inserted] = m_format_properties.try_emplace(format);
		if (inserted) {
			physical_device.getFormatProperties(format, &it->second);
		}
		return it->second;
	}

	[[nodiscard]] vk::Result GetImageFormatProperties(vk::Format format, vk::ImageType type,
	                                                  vk::ImageTiling            tiling,
	                                                  vk::ImageUsageFlags        usage,
	                                                  vk::ImageCreateFlags       flags,
	                                                  vk::ImageFormatProperties* properties) const {
		using Key = std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
		                       vk::ImageCreateFlags>;
		std::scoped_lock lock(m_image_format_properties_mutex);
		auto [it, inserted] =
		    m_image_format_properties.try_emplace(Key {format, type, tiling, usage, flags});
		if (inserted) {
			it->second.first = physical_device.getImageFormatProperties(format, type, tiling, usage,
			                                                            flags, &it->second.second);
		}
		if (properties != nullptr) {
			*properties = it->second.second;
		}
		return it->second.first;
	}

	[[nodiscard]] vk::DeviceSize StorageMinAlignment() const {
		const auto alignment = physical_device_properties.limits.minStorageBufferOffsetAlignment;
		return alignment != 0 ? alignment : 1;
	}

private:
	mutable std::mutex                                 m_format_properties_mutex;
	mutable std::map<vk::Format, vk::FormatProperties> m_format_properties;
	mutable std::mutex                                 m_image_format_properties_mutex;
	mutable std::map<std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
	                            vk::ImageCreateFlags>,
	                 std::pair<vk::Result, vk::ImageFormatProperties>>
	    m_image_format_properties;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANINSTANCE_H_
