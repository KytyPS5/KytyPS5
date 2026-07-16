#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanInstance.h"

#include <memory>
#include <mutex>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h> // IWYU pragma: export

namespace Libs::Graphics {

class CommandBuffer;

struct VulkanSwapchain {
	~VulkanSwapchain();

	VkSwapchainKHR                              swapchain        = nullptr;
	VkFormat                                    swapchain_format = VK_FORMAT_UNDEFINED;
	VkExtent2D                                  swapchain_extent = {};
	std::unique_ptr<VkImage[]>                  swapchain_images;
	std::unique_ptr<VkImageView[]>              swapchain_image_views;
	uint32_t                                    swapchain_images_count = 0;
	std::unique_ptr<VkSemaphore[]>              image_acquired_semaphores;
	std::unique_ptr<VkSemaphore[]>              render_complete_semaphores;
	uint32_t                                    current_index = 0;
	uint32_t                                    present_frame = 0;
	std::vector<std::unique_ptr<CommandBuffer>> present_command_buffers;
};

struct VulkanCommandPool {
	Common::Mutex                      mutex;
	VkCommandPool                      pool = nullptr;
	std::unique_ptr<VkCommandBuffer[]> buffers;
	std::unique_ptr<VkFence[]>         fences;
	std::unique_ptr<VkSemaphore[]>     semaphores;
	std::unique_ptr<bool[]>            busy;
	uint32_t                           buffers_count = 0;
};

struct GraphicContext: public VulkanInstance {
	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;
};

struct VulkanMemory {
	VkMemoryRequirements  requirements    = {};
	VkMemoryPropertyFlags property        = 0;
	VkDeviceMemory        memory          = nullptr;
	VmaAllocation         allocation      = nullptr;
	VmaAllocationInfo     allocation_info = {};
	VkDeviceSize          offset          = 0;
	uint32_t              type            = 0;
	uint64_t              unique_id       = 0;
};

enum class VulkanImageType {
	Unknown,
	VideoOut,
	DepthStencil,
	Texture,
	StorageTexture,
	RenderTexture
};

struct VulkanImage {
	static constexpr int VIEW_MAX                   = 21;
	static constexpr int VIEW_DEFAULT               = 0;
	static constexpr int VIEW_BGRA                  = 1;
	static constexpr int VIEW_DEPTH_TEXTURE         = 2;
	static constexpr int VIEW_R001                  = 3;
	static constexpr int VIEW_RGB1                  = 4;
	static constexpr int VIEW_R000                  = 5;
	static constexpr int VIEW_RG01                  = 6;
	static constexpr int VIEW_000R                  = 7;
	static constexpr int VIEW_DEFAULT_ARRAY         = 8;
	static constexpr int VIEW_BGRA_ARRAY            = 9;
	static constexpr int VIEW_DEPTH_TEXTURE_ARRAY   = 10;
	static constexpr int VIEW_R001_ARRAY            = 11;
	static constexpr int VIEW_RGB1_ARRAY            = 12;
	static constexpr int VIEW_R000_ARRAY            = 13;
	static constexpr int VIEW_RG01_ARRAY            = 14;
	static constexpr int VIEW_000R_ARRAY            = 15;
	static constexpr int VIEW_STENCIL_TEXTURE       = 16;
	static constexpr int VIEW_STENCIL_TEXTURE_ARRAY = 17;
	static constexpr int VIEW_STORAGE               = 18;
	static constexpr int VIEW_STORAGE_ARRAY         = 19;
	static constexpr int VIEW_RGBA8_BGRA            = 20;

	explicit VulkanImage(VulkanImageType type): type(type) {}

	VulkanImageType        type                 = VulkanImageType::Unknown;
	VkFormat               format               = VK_FORMAT_UNDEFINED;
	VkExtent2D             extent               = {};
	uint32_t               guest_pitch          = 0;
	uint32_t               layers               = 1;
	uint32_t               mip_levels           = 1;
	VkImage                image                = nullptr;
	VkImageView            image_view[VIEW_MAX] = {};
	VkImageLayout          layout               = VK_IMAGE_LAYOUT_UNDEFINED;
	Graphics::VulkanMemory memory;
};

struct VideoOutVulkanImage: public VulkanImage {
	VideoOutVulkanImage(): VulkanImage(VulkanImageType::VideoOut) {}
};

struct DepthStencilVulkanImage: public VulkanImage {
	DepthStencilVulkanImage(): VulkanImage(VulkanImageType::DepthStencil) {}
	bool compressed = false;
};

struct GpuTextureVulkanImage: public VulkanImage {
	explicit GpuTextureVulkanImage(VulkanImageType type): VulkanImage(type) {}
};

struct TextureVulkanImage: public GpuTextureVulkanImage {
	TextureVulkanImage(): GpuTextureVulkanImage(VulkanImageType::Texture) {}
};

struct StorageTextureVulkanImage: public GpuTextureVulkanImage {
	struct SampledView {
		VkFormat    format      = VK_FORMAT_UNDEFINED;
		uint32_t    swizzle     = 0;
		uint32_t    type        = 0;
		uint32_t    base_level  = 0;
		uint32_t    level_count = 0;
		VkImageView view        = nullptr;
	};
	StorageTextureVulkanImage(): GpuTextureVulkanImage(VulkanImageType::StorageTexture) {}
	std::mutex               sampled_view_mutex;
	std::vector<SampledView> sampled_views;
};

struct RenderTextureVulkanImage: public VulkanImage {
	struct AttachmentView {
		VkFormat    format = VK_FORMAT_UNDEFINED;
		uint32_t    level  = 0;
		VkImageView view   = nullptr;
	};
	struct SampledView {
		VkFormat    format      = VK_FORMAT_UNDEFINED;
		uint32_t    base_level  = 0;
		uint32_t    level_count = 0;
		int         variant     = VIEW_DEFAULT;
		VkImageView view        = nullptr;
	};
	struct StorageView {
		VkFormat    format      = VK_FORMAT_UNDEFINED;
		uint32_t    base_level  = 0;
		uint32_t    level_count = 0;
		VkImageView view        = nullptr;
	};
	RenderTextureVulkanImage(): VulkanImage(VulkanImageType::RenderTexture) {}
	VkImageView                 render_view[16] = {};
	std::mutex                  attachment_view_mutex;
	std::vector<AttachmentView> attachment_views;
	std::mutex                  sampled_view_mutex;
	std::vector<SampledView>    sampled_views;
	std::mutex                  storage_view_mutex;
	std::vector<StorageView>    storage_views;
};

struct VulkanBuffer {
	VkBuffer           buffer = nullptr;
	VulkanMemory       memory;
	VkBufferUsageFlags usage       = 0;
	uint64_t           buffer_size = 0;
};

struct StorageVulkanBuffer: public VulkanBuffer {};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
