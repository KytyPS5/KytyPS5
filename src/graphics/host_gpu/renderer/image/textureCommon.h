#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <span>
#include <vector>

namespace Libs::Graphics {

struct RenderTargetFormatInfo {
	vk::Format                      format            = vk::Format::eUndefined;
	uint32_t                        bytes_per_element = 0;
	Prospero::ColorComponentMapping export_mapping;
	Prospero::BufferFormat          guest_format = Prospero::BufferFormat::kInvalid;
};

struct SurfaceFormatInfo {
	vk::Format                      vk_format;
	Prospero::BufferFormat          conversion_format;
	Prospero::ColorComponentMapping host_to_storage;
};

struct TextureDownloadBlock {
	uint32_t width  = 1;
	uint32_t height = 1;
	uint32_t bytes  = 0;
};

struct TextureDownloadChunk {
	uint64_t                         offset = 0;
	uint64_t                         size   = 0;
	std::vector<vk::BufferImageCopy> regions;
};

vk::ComponentMapping   TextureGetComponentMapping(uint32_t                        swizzle,
                                                  Prospero::ColorComponentMapping host_to_storage);
SurfaceFormatInfo      TextureGetSurfaceFormatInfo(Prospero::BufferFormat format);
RenderTargetFormatInfo TextureGetRenderTargetFormat(Prospero::ChannelLayout layout,
                                                    Prospero::ChannelType   type,
                                                    Prospero::ChannelOrder  order);
// Splits 2D copies by block rows into chunks of at most capacity bytes, offsets chunk-relative.
bool TexturePlanDownloadChunks(std::span<const vk::BufferImageCopy> regions, uint64_t image_size,
                               const TextureDownloadBlock& block, uint64_t capacity,
                               uint64_t alignment, std::vector<TextureDownloadChunk>& out_chunks);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_ */
