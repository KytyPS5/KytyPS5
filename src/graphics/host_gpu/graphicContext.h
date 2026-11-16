#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/sparseBinding.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: export

#include <array>
#include <map>
#include <mutex>
#include <span>
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
	bool                               shader_image_int64_atomics_enabled    = false;
	bool                               attachment_feedback_loop_enabled      = false;
	bool                               provoking_vertex_last_enabled         = false;
	bool                               maintenance8_enabled                  = false;
	// Sparse 2D images: textures the T# MIN_LOD clamps leave their unsampled levels unbound.
	bool sparse_residency_image2d_enabled = false;
	// Sparse buffers take memory page by page; aliased ones can share a page.
	bool sparse_residency_buffer_enabled  = false;
	bool sparse_residency_aliased_enabled = false;
	// The largest buffer the device creates.
	uint64_t max_buffer_size = 0;
	// Null without sparse images and buffers; created with the allocator.
	SparseBinding*                            sparse_binding                         = nullptr;
	bool                               robust_buffer_access_enabled          = false;
	bool                               ray_query_enabled                     = false;
	bool                               r8_srgb_views                         = false;
	bool                               r8g8_srgb_views                       = false;
	bool                               r4g4b4a4_render_targets               = false;
	bool                               conditional_rendering_enabled         = false;
	bool                               sampler_filter_minmax_enabled         = false;
	bool                               occlusion_queries_enabled             = false;
	bool                               occlusion_query_precise_enabled       = false;
	bool                               supports_block_texel_view              = false;
	bool                                      mesh_shader_enabled                   = false;
	vk::PhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties                = {};
	// VK_EXT_sample_locations: the sample counts that take custom locations on a 1x1 grid.
	bool                               sample_locations_enabled              = false;
	vk::SampleCountFlags               sample_location_counts                = {};
	std::array<float, 2>               sample_location_range                 = {};
	uint32_t                           sample_location_sub_pixel_bits        = 0;
	uint32_t                           subgroup_size                         = 0;
	uint32_t                           min_subgroup_size                     = 0;
	uint32_t                           max_subgroup_size                     = 0;
	uint32_t                           max_push_descriptors                  = 0;
	uint32_t                           device_clock_period_ps                = 0;
	vk::DescriptorSetLayout            image_heap_layout                     = nullptr;
	// The sets of a draw's candidate buffers and images, and the most of each one draw binds.
	vk::DescriptorSetLayout            candidate_buffer_layout = nullptr;
	uint32_t                           max_candidate_buffers   = 0;
	vk::DescriptorSetLayout            candidate_image_layout  = nullptr;
	uint32_t                           max_candidate_images    = 0;
	Common::Mutex                      queue_mutex;
	uint32_t                           queue_family = static_cast<uint32_t>(-1);
	vk::Queue                          queue        = nullptr;

	[[nodiscard]] const vk::PhysicalDeviceProperties& GetPhysicalDeviceProperties() const {
		return physical_device_properties;
	}

	[[nodiscard]] vk::FormatProperties GetFormatProperties(vk::Format format) const {
		std::scoped_lock lock(m_format_properties_mutex);
		auto [it, inserted] = m_format_properties.try_emplace(format);
		if (inserted) {
			physical_device.getFormatProperties(format, &it->second);
		}
		return it->second;
	}

	// Sampled views of format filter linearly, as guest samplers may.
	[[nodiscard]] bool SamplesFiltered(vk::Format format) const {
		constexpr vk::FormatFeatureFlags features =
		    vk::FormatFeatureFlagBits::eSampledImage |
		    vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
		return (GetFormatProperties(format).optimalTilingFeatures & features) == features;
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

	// Fills the sample_location_* fields from the physical device.
	void QuerySampleLocations(bool extension_enabled);

	[[nodiscard]] bool CreateAllocator();
	// Called by CreateAllocator; a caller creating the VMA allocator itself calls it next.
	void                   CreateSparseBinding();
	void               DestroyAllocator();
	void               LogMemoryBudget() const;
	[[nodiscard]] bool CanReportMemoryUsage() const noexcept { return memory_budget_ext_enabled; }
	// The device memory the driver holds for this process, what it allocates itself included.
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;
	// Whether `size` more bytes of device-local memory stay within the device-local budget.
	[[nodiscard]] bool DeviceBudgetAllows(uint64_t size) const;
	// Device-local memory through `allocate`: from the blocks already held, then from new memory
	// while the budget allows. False when neither served it.
	template <typename Allocate>
	[[nodiscard]] bool AllocateDeviceLocal(uint64_t size, VmaAllocationCreateInfo info,
	                                       Allocate&& allocate) const {
		info.requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		info.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
		if (allocate(info) == VK_SUCCESS) {
			return true;
		}
		if (!DeviceBudgetAllows(size)) {
			return false;
		}
		info.flags &=
		    ~static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT);
		return allocate(info) == VK_SUCCESS;
	}
	// Replaces the device-local budget; zero keeps the reported one. For tests.
	uint64_t device_budget_override = 0;

	struct HeapUsage {
		uint64_t usage        = 0; // Whole process, as reported by VK_EXT_memory_budget.
		uint64_t budget       = 0;
		uint32_t allocations  = 0;
		bool     device_local = false;
	};
	[[nodiscard]] uint32_t GetHeapUsage(std::span<HeapUsage, VK_MAX_MEMORY_HEAPS> heaps) const;
	// Levels below `first_bound_level` are never accessed and need no memory, where the device
	// can leave them unbound (a sparse image).
	[[nodiscard]] bool     CreateImage(const vk::ImageCreateInfo& info, VulkanImage& image,
	                                   uint32_t first_bound_level = 0);
	void                   DeleteImage(VulkanImage& image);

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;

private:
	// A sparse image whose levels from `first_level` on (and its mip tail) have device-local
	// memory; false, with nothing created, where the device or format cannot do it.
	[[nodiscard]] bool CreatePartialImage(const vk::ImageCreateInfo& info, uint32_t first_level,
	                                      VulkanImage& image);

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
	// Created by CreatePartialImage: `allocation` backs only its bound levels.
	bool sparse = false;
};



} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
