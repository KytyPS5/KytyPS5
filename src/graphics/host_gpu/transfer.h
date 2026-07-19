#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_TRANSFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_TRANSFER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct VulkanBuffer;
struct VulkanImage;
struct DepthStencilVulkanImage;
struct VulkanSwapchain;

struct BufferImageCopy {
	uint32_t             offset;
	uint32_t             pitch;
	uint32_t             dst_level;
	uint32_t             width;
	uint32_t             height;
	uint32_t             copy_height = 0;
	uint32_t             dst_layer   = 0;
	int                  dst_x;
	int                  dst_y;
	int                  dst_z  = 0;
	vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
};

struct ImageBufferCopy {
	uint32_t             offset;
	uint32_t             pitch;
	uint32_t             src_level;
	uint32_t             width;
	uint32_t             height;
	uint32_t             copy_height = 0;
	uint32_t             src_layer   = 0;
	int                  src_x;
	int                  src_y;
	int                  src_z  = 0;
	vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
};

struct ImageImageCopy {
	VulkanImage*         src_image;
	uint32_t             src_level;
	uint32_t             dst_level;
	uint32_t             width;
	uint32_t             height;
	uint32_t             src_layer  = 0;
	uint32_t             dst_layer  = 0;
	vk::ImageAspectFlags src_aspect = vk::ImageAspectFlagBits::eColor;
	vk::ImageAspectFlags dst_aspect = vk::ImageAspectFlagBits::eColor;
	int                  src_x;
	int                  src_y;
	int                  src_z = 0;
	int                  dst_x;
	int                  dst_y;
	int                  dst_z = 0;
};

namespace Transfer {

[[nodiscard]] std::vector<ImageBufferCopy>
MakeLayeredImageBufferCopies(uint32_t layers, uint64_t slice_size, uint32_t pitch, uint32_t width,
                             uint32_t             height,
                             vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

enum class StagingBufferType { Texture, Vertex, ReadBack };

class ScratchBuffer {
public:
	explicit ScratchBuffer(uint64_t size);
	~ScratchBuffer();
	KYTY_CLASS_NO_COPY(ScratchBuffer);

	[[nodiscard]] void* Data() const { return m_data; }

private:
	void* m_data = nullptr;
};

void CopyImage(CommandBuffer* buffer, std::span<const ImageImageCopy> regions,
               VulkanImage* dst_image, vk::ImageLayout dst_layout);
void CopyImageViaBuffer(CommandBuffer* buffer, GraphicContext* ctx, VulkanImage* src_image,
                        vk::ImageAspectFlags src_aspect, VulkanImage* dst_image,
                        vk::ImageAspectFlags dst_aspect, uint32_t bytes_per_element,
                        vk::ImageLayout dst_layout);
void BlitToSwapchain(CommandBuffer* buffer, VulkanImage* src_image, VulkanSwapchain* dst_swapchain);
void ClearColorImage(CommandBuffer* buffer, VulkanImage* image, const vk::ClearColorValue& color);
void UploadImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size,
                 uint32_t src_pitch, vk::ImageLayout dst_layout);
void UploadImage(GraphicContext* ctx, DepthStencilVulkanImage* dst_image, const void* src_data,
                 uint64_t size, uint32_t src_pitch, vk::ImageAspectFlags aspect);
void UploadImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size,
                 std::span<const BufferImageCopy> regions, vk::ImageLayout dst_layout);
void CopyImageImmediate(GraphicContext* ctx, std::span<const ImageImageCopy> regions,
                        VulkanImage* dst_image, vk::ImageLayout dst_layout);
void DownloadImage(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch,
                   VulkanImage* src_image, vk::ImageLayout src_layout,
                   vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);
void DownloadImage(GraphicContext* ctx, void* dst_data, uint64_t size,
                   std::span<const ImageBufferCopy> regions, VulkanImage* src_image,
                   vk::ImageLayout src_layout);
void UploadBuffer(GraphicContext* ctx, StagingBufferType type, VulkanBuffer* dst_buffer,
                  uint64_t dst_offset, const void* src_data, uint64_t size);
void CopyBuffer(VulkanBuffer* src_buffer, VulkanBuffer* dst_buffer, uint64_t size);
void DownloadBuffer(GraphicContext* ctx, VulkanBuffer* src_buffer, uint64_t src_offset,
                    void* dst_data, uint64_t size);
void ReleaseCachedResources(GraphicContext* ctx);
bool GuestBufferIsTiled(uint64_t vaddr, uint64_t size);
bool IsBlockCompressedFormat(vk::Format format);
uint32_t BlockCompressedBytesPerBlock(vk::Format format);

void WaitForGraphicsIdle(GraphicContext* ctx);

inline std::pair<int, int> MipmapAtlasOffset(uint32_t lod, uint32_t width, uint32_t height) {
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

} // namespace Transfer

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_TRANSFER_H_
