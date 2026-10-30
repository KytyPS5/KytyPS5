#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/hostCapabilities.h"
#include "graphics/host_gpu/queueCommits.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: export

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
	// VK_EXT_external_memory_dma_buf and VK_KHR_external_memory_fd: guest memory can be imported
	// (guestMemoryImport.h).
	bool                               dma_buf_import_enabled                = false;
	// VK_AMD_buffer_marker, for the breadcrumbs of KYTY_GPU_BREADCRUMBS (gpuBreadcrumbs.h).
	bool                               buffer_marker_enabled                 = false;
	// The device was made with a low global queue priority (see VulkanCreateDevice).
	bool                               low_queue_priority                    = false;
	// VK_KHR_pipeline_executable_properties, for KYTY_PIPELINE_IR: the driver's statistics and
	// internal representations of the compute pipelines of one guest shader.
	bool                               pipeline_executable_info_enabled      = false;
	// VK_EXT_robustness2's nullDescriptor: a null guest texture is bound as a null descriptor.
	// Off where the device lacks it (MoltenVK), and the renderer binds a placeholder instead.
	bool                               null_descriptor_enabled               = false;
	bool                               sample_rate_shading_enabled           = false;
	// bool fp64_denorm_preserve = false; // Temporarily disabled.
	// What the device enabled, not only what it supports, and the known defects of its driver.
	HostGpu                                   host;
	bool                                      conditional_rendering_enabled   = false;
	bool                                      occlusion_queries_enabled       = false;
	bool                                      occlusion_query_precise_enabled = false;
	vk::PhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties                = {};
	uint32_t                           subgroup_size                         = 0;
	uint32_t                           min_subgroup_size                     = 0;
	uint32_t                           max_subgroup_size                     = 0;
	uint32_t                           max_push_descriptors                  = 0;
	uint32_t                           device_clock_period_ps                = 0;
	vk::DescriptorSetLayout            image_heap_layout                     = nullptr;
	vk::ShaderStageFlags               required_subgroup_size_stages         = {};
	Common::Mutex                      queue_mutex;
	// What every submitter committed to the queue. Under queue_mutex.
	QueueCommits queue_commits;

	// Waits until every command buffer committed to the queue has completed, then runs `then`
	// before anything new can be committed: for destroying resources the queue may hold (see
	// queueCommits.h). Waits only when some command buffer may still run.
	template <typename Then>
	void WithQueueIdle(Then&& then) {
		Common::LockGuard lock(queue_mutex);
		if (!queue_commits.AllDone()) {
			RequireVulkanSuccess(queue.waitIdle(), "vkQueueWaitIdle");
			queue_commits.NoteIdle();
		}
		then();
	}
	uint32_t                           queue_family = static_cast<uint32_t>(-1);
	vk::Queue                          queue        = nullptr;

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
		return subgroup_size == 64u || host.capabilities.compute_subgroup_size_control;
	}

	[[nodiscard]] vk::DeviceSize StorageMinAlignment() const {
		const auto alignment = physical_device_properties.limits.minStorageBufferOffsetAlignment;
		return alignment != 0 ? alignment : 1;
	}

	[[nodiscard]] bool CreateAllocator();
	void               DestroyAllocator();
	void               LogMemoryBudget() const;
	[[nodiscard]] bool CanReportMemoryUsage() const noexcept { return memory_budget_ext_enabled; }
	// The GPU uses the system memory, not memory of its own (see cacheCollection.h).
	[[nodiscard]] bool UsesSystemMemory() const noexcept {
		return physical_device_properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu;
	}
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;

	struct HeapUsage {
		uint64_t usage        = 0; // Whole process, as reported by VK_EXT_memory_budget.
		uint64_t budget       = 0;
		uint32_t allocations  = 0;
		bool     device_local = false;
	};
	[[nodiscard]] uint32_t GetHeapUsage(std::span<HeapUsage, VK_MAX_MEMORY_HEAPS> heaps) const;
	[[nodiscard]] bool     CreateImage(const vk::ImageCreateInfo& info, VulkanImage& image);
	void                   DeleteImage(VulkanImage& image);

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;

private:
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
};



} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
