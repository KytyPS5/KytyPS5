#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_

#include "common/abi.h"
#include "common/common.h"

#include <utility>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct VulkanBuffer;
struct VulkanImage;
struct VulkanMemory;
struct DepthStencilVulkanImage;
struct VulkanSwapchain;

struct BufferImageCopy {
	uint32_t offset;
	uint32_t pitch;
	uint32_t dst_level;
	uint32_t width;
	uint32_t height;
	uint32_t copy_height = 0;
	uint32_t dst_layer   = 0;
	int      dst_x;
	int      dst_y;
	int      dst_z = 0;
};

struct ImageImageCopy {
	VulkanImage* src_image;
	uint32_t     src_level;
	uint32_t     dst_level;
	uint32_t     width;
	uint32_t     height;
	uint32_t     src_layer = 0;
	uint32_t     dst_layer = 0;
	int          src_x;
	int          src_y;
	int          src_z = 0;
	int          dst_x;
	int          dst_y;
	int          dst_z = 0;
};

enum class StagingBufferType { Texture, Vertex, Index, ReadBack };

class UtilScratchBuffer {
public:
	explicit UtilScratchBuffer(uint64_t size);
	~UtilScratchBuffer();
	KYTY_CLASS_NO_COPY(UtilScratchBuffer);

	[[nodiscard]] void* Data() const { return m_data; }

private:
	void* m_data = nullptr;
};

void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, uint32_t src_pitch,
                       VulkanImage* dst_image, uint64_t dst_layout);
void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, VulkanImage* dst_image,
                       const std::vector<BufferImageCopy>& regions, uint64_t dst_layout);
void UtilImageToBuffer(CommandBuffer* buffer, VulkanImage* src_image, VulkanBuffer* dst_buffer,
                       uint32_t dst_pitch, uint64_t src_layout,
                       VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
void UtilImageToImage(CommandBuffer* buffer, const std::vector<ImageImageCopy>& regions,
                      VulkanImage* dst_image, uint64_t dst_layout);
void UtilBlitImage(CommandBuffer* buffer, VulkanImage* src_image, VulkanSwapchain* dst_swapchain);
void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size,
                   uint32_t src_pitch, uint64_t dst_layout);
void UtilFillDepthImage(GraphicContext* ctx, DepthStencilVulkanImage* dst_image,
                        const void* src_data, uint64_t size, uint32_t src_pitch);
void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size,
                   const std::vector<BufferImageCopy>& regions, uint64_t dst_layout);
void UtilFillImage(GraphicContext* ctx, const std::vector<ImageImageCopy>& regions,
                   VulkanImage* dst_image, uint64_t dst_layout);
void UtilFillBuffer(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch,
                    VulkanImage* src_image, uint64_t src_layout,
                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
void UtilUploadBuffer(GraphicContext* ctx, StagingBufferType type, VulkanBuffer* dst_buffer,
                      const void* src_data, uint64_t size);
void UtilUploadBuffer(GraphicContext* ctx, StagingBufferType type, VulkanBuffer* dst_buffer,
                      uint64_t dst_offset, const void* src_data, uint64_t size);
void UtilCopyBuffer(VulkanBuffer* src_buffer, VulkanBuffer* dst_buffer, uint64_t size);
void UtilDownloadBuffer(GraphicContext* ctx, VulkanBuffer* src_buffer, uint64_t src_offset,
                        void* dst_data, uint64_t size);
bool UtilBufferIsTiled(uint64_t vaddr, uint64_t size);
void UtilSetDepthLayoutOptimal(DepthStencilVulkanImage* image);
void UtilSetImageLayoutOptimal(VulkanImage* image);
void UtilResetImageViews(VulkanImage* image);
void UtilCreateImageView(GraphicContext* ctx, VulkanImage* image, int view_index,
                         VkImageViewType view_type, VkImageAspectFlags aspect_mask,
                         VkComponentMapping components, uint32_t base_array_layer,
                         uint32_t base_mip_level, uint32_t layer_count, uint32_t level_count,
                         VkFormat          view_format = VK_FORMAT_UNDEFINED,
                         VkImageUsageFlags view_usage  = 0);
bool UtilIsBcFormat(VkFormat format);
uint32_t UtilGetBcBlockSize(VkFormat format);

void VulkanCreateBuffer(GraphicContext* gctx, uint64_t size, VulkanBuffer* buffer);
void VulkanDeleteBuffer(GraphicContext* gctx, VulkanBuffer* buffer);
bool VulkanCreateImage(GraphicContext* gctx, const VkImageCreateInfo* image_info,
                       VulkanImage* image, VulkanMemory* memory);
void VulkanDeleteImage(GraphicContext* gctx, VulkanImage* image, VulkanMemory* memory);
void VulkanDeviceWaitIdle(GraphicContext* ctx);

inline std::pair<int, int> UtilCalcMipmapOffset(uint32_t lod, uint32_t width, uint32_t height) {
	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	int      mip_x      = 0;
	int      mip_y      = 0;

	for (uint32_t i = 0; i < 16; i++) {
		if (i == lod) {
			return {mip_x, mip_y};
		}

		bool odd = ((i & 1u) != 0);
		mip_x += static_cast<int>(odd ? mip_width : 0);
		mip_y += static_cast<int>(odd ? 0 : mip_height);

		mip_width >>= (mip_width > 1 ? 1u : 0u);
		mip_height >>= (mip_height > 1 ? 1u : 0u);
	}

	return {mip_x, mip_y};
}

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_ */
