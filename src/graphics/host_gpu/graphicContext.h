#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/memoryHeadroom.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: export

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

struct VulkanImage;

inline constexpr uint32_t VULKAN_TARGET_API_VERSION = VK_API_VERSION_1_3;

struct GraphicContext {
	vk::Instance                       instance                              = nullptr;
	vk::DebugUtilsMessengerEXT         debug_messenger                       = nullptr;
	vk::PhysicalDevice                 physical_device                       = nullptr;
	vk::PhysicalDeviceProperties       physical_device_properties            = {};
	vk::PhysicalDeviceMemoryProperties physical_device_memory_properties     = {};
	vk::Device                         device                                = nullptr;
	VmaAllocator                       allocator                             = nullptr;
	bool                               memory_budget_ext_enabled             = false;
	bool                               compute_subgroup_size_control_enabled = false;
	bool                               sample_rate_shading_enabled           = false;
	bool                               shader_image_int64_atomics_enabled    = false;
	// bool fp64_denorm_preserve = false; // Temporarily disabled.
	bool                               attachment_feedback_loop_enabled      = false;
	bool                               provoking_vertex_last_enabled         = false;
	// VK_EXT_shader_stencil_export; without it the guest's stencil exports are dropped before the
	// program is compiled.
	bool                               shader_stencil_export_enabled         = false;
	bool                               pipeline_library_enabled              = false;
	bool                               pipeline_library_fast_linking         = false;
	bool                               supports_block_texel_view              = false;
	// Guest indirect draws stay on the GPU only when all three are enabled.
	bool                               draw_indirect_first_instance_enabled  = false;
	bool                               multi_draw_indirect_enabled           = false;
	bool                               draw_indirect_count_enabled           = false;
	bool                                      sparse_residency_buffer_enabled       = false;
	bool                                      mesh_shader_enabled                   = false;
	vk::PhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties                = {};
	uint32_t                           subgroup_size                         = 0;
	uint32_t                           min_subgroup_size                     = 0;
	uint32_t                           max_subgroup_size                     = 0;
	uint32_t                           max_push_descriptors                  = 0;
	vk::ShaderStageFlags               required_subgroup_size_stages         = {};
	Common::Mutex                      queue_mutex;
	uint32_t                           queue_family = static_cast<uint32_t>(-1);
	uint32_t                           queue_count  = 1;
	vk::Queue                          queue        = nullptr;
	// A second queue of `queue_family`, or null; CommandScheduler::RunDetached submits to it.
	vk::Queue                          readback_queue = nullptr;

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

	[[nodiscard]] bool SupportsComputeWave64() const noexcept {
		return subgroup_size == 64u || compute_subgroup_size_control_enabled;
	}

	[[nodiscard]] vk::DeviceSize StorageMinAlignment() const {
		const auto alignment = physical_device_properties.limits.minStorageBufferOffsetAlignment;
		return alignment != 0 ? alignment : 1;
	}

	struct ImageAllocationReport {
		vk::Result              result = vk::Result::eSuccess;
		Headroom::AllocationTier tier  = Headroom::AllocationTier::DeviceLocal;
		uint64_t                size                  = 0;
		bool                    host_fallback_allowed = true;
		Headroom::MemoryBudget  budget;

		[[nodiscard]] bool Failed() const noexcept { return result != vk::Result::eSuccess; }
	};

	[[nodiscard]] bool CreateAllocator();
	void               DestroyAllocator();
	void               LogMemoryBudget() const;
	[[nodiscard]] bool CanReportMemoryUsage() const noexcept { return memory_budget_ext_enabled; }
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;
	// Without VK_EXT_memory_budget `reported` is false and only the raw heap size is known.
	[[nodiscard]] Headroom::MemoryBudget GetMemoryBudget() const;
	// Bytes this image would need, measured by creating an unbacked VkImage; 0 if that fails.
	[[nodiscard]] uint64_t               ImageMemorySize(const vk::ImageCreateInfo& info) const;
	[[nodiscard]] bool CreateImage(const vk::ImageCreateInfo& info, VulkanImage& image,
	                               ImageAllocationReport* report = nullptr);
	void               DeleteImage(VulkanImage& image);
	// Destroys every idle pooled image and returns the bytes released.
	uint64_t           TrimImagePool();
	// Images that needed a new device allocation, as opposed to a pooled one.
	[[nodiscard]] uint64_t ImageAllocationCount() const noexcept {
		return m_image_allocations.load(std::memory_order_relaxed);
	}

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;

private:
	struct ImagePoolKey {
		vk::ImageCreateFlags    flags;
		vk::ImageType           type = vk::ImageType::e2D;
		vk::Format              format = vk::Format::eUndefined;
		vk::Extent3D            extent;
		uint32_t                mip_levels = 1;
		uint32_t                layers     = 1;
		vk::SampleCountFlagBits samples    = vk::SampleCountFlagBits::e1;
		vk::ImageUsageFlags     usage;

		bool operator==(const ImagePoolKey&) const = default;
	};
	struct PooledImage {
		ImagePoolKey                          key;
		vk::Image                             image      = nullptr;
		VmaAllocation                         allocation = nullptr;
		uint64_t                              size       = 0;
		std::chrono::steady_clock::time_point released;
	};

	[[nodiscard]] bool TakePooledImage(const ImagePoolKey& key, VulkanImage& image);
	[[nodiscard]] bool PoolImage(VulkanImage& image);
	void               ExpirePooledImages(std::vector<PooledImage>& victims, uint64_t incoming);

	std::mutex              m_image_pool_mutex;
	std::deque<PooledImage> m_image_pool;
	uint64_t                m_image_pool_bytes = 0;
	uint64_t                m_image_pool_limit = 0;
	std::atomic<uint64_t>   m_image_allocations {0};

	mutable std::mutex                                 m_format_properties_mutex;
	mutable std::map<vk::Format, vk::FormatProperties> m_format_properties;
	mutable std::mutex                                 m_image_format_properties_mutex;
	mutable std::map<std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
	                            vk::ImageCreateFlags>,
	                 std::pair<vk::Result, vk::ImageFormatProperties>>
	    m_image_format_properties;
};

struct VulkanImageState {
	vk::PipelineStageFlags2 pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	vk::AccessFlags2        access_mask = vk::AccessFlagBits2::eNone;
	vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
	bool                    flushed     = false;
};

struct VulkanImage {
	VulkanImage() = default;
	KYTY_CLASS_NO_COPY(VulkanImage);

	vk::Format                    format      = vk::Format::eUndefined;
	vk::ImageType                 image_type  = vk::ImageType::e2D;
	vk::Extent3D                  extent      = {1, 1, 1};
	uint32_t                      layers      = 1;
	uint32_t                      mip_levels  = 1;
	uint32_t                      samples     = 1;
	vk::ImageUsageFlags           usage       = {};
	vk::ImageCreateFlags          flags       = {};
	vk::Image                     image       = nullptr;
	VulkanImageState              state;
	std::vector<VulkanImageState> subresource_states;
	VmaAllocation                allocation = nullptr;
	// Created from a plain create info into device-local memory, so an identical request can reuse it.
	bool                          poolable   = false;
};



} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
