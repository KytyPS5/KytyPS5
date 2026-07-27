#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: export
#include "graphics/host_gpu/vulkanInstance.h"

#include <vector>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

class CommandBuffer;
struct VulkanBuffer;
struct VulkanImage;
struct VulkanMemory;

struct GraphicContext: public VulkanInstance {
	[[nodiscard]] bool CreateAllocator();
	void               DestroyAllocator();
	void               LogMemoryBudget() const;
	[[nodiscard]] bool CanReportMemoryUsage() const noexcept { return memory_budget_ext_enabled; }
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;
	void                   CreateBuffer(uint64_t size, VulkanBuffer& buffer);
	void                   DeleteBuffer(VulkanBuffer& buffer);
	[[nodiscard]] bool     CreateImage(const vk::ImageCreateInfo& info, VulkanImage& image);
	void                   DeleteImage(VulkanImage& image);
	void                   MapMemory(VulkanMemory& memory, void*& data);
	void                   UnmapMemory(VulkanMemory& memory);
	void                   AppendHardwareRayTracingDeviceExtensions(
	    const std::vector<vk::ExtensionProperties>& available_extensions,
	    std::vector<const char*>&                   device_extensions);
	void LoadHardwareRayTracingFunctions() const;

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;
};

struct VulkanMemory {
	vk::MemoryRequirements  requirements       = {};
	vk::MemoryPropertyFlags property           = {};
	vk::MemoryPropertyFlags preferred_property = {};
	vk::DeviceMemory        memory             = nullptr;
	VmaAllocation           allocation         = nullptr;
	VmaAllocationInfo       allocation_info    = {};
	vk::DeviceSize          offset             = 0;
	uint32_t                type               = 0;
	uint64_t                unique_id          = 0;
};

struct VulkanImageState {
	vk::PipelineStageFlags2 pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	vk::AccessFlags2        access_mask = vk::AccessFlagBits2::eNone;
	vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
};

struct VulkanImage {
	VulkanImage() = default;
	KYTY_CLASS_NO_COPY(VulkanImage);

	vk::Format             format      = vk::Format::eUndefined;
	vk::ImageType          image_type  = vk::ImageType::e2D;
	vk::Extent3D           extent      = {1, 1, 1};
	uint32_t               guest_pitch = 0;
	uint32_t               layers      = 1;
	uint32_t               mip_levels  = 1;
	uint32_t               samples     = 1;
	vk::ImageUsageFlags    usage       = {};
	vk::ImageCreateFlags   flags       = {};
	vk::Image              image       = nullptr;
	VulkanImageState       state;
	std::vector<VulkanImageState> subresource_states;
	Graphics::VulkanMemory memory;
};

struct VulkanBuffer {
	vk::Buffer           buffer = nullptr;
	VulkanMemory         memory;
	vk::BufferUsageFlags usage       = {};
	uint64_t             buffer_size = 0;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
