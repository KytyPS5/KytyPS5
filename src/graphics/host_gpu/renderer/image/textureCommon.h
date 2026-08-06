#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <vector>

namespace Libs::Graphics {

struct GpuTileInfo;

struct RenderTargetFormatInfo {
	vk::Format                      format            = vk::Format::eUndefined;
	uint32_t                        bytes_per_element = 0;
	Prospero::ColorComponentMapping export_mapping;
};

struct TextureUploadMipLayout {
	uint64_t offset       = 0;
	uint64_t size         = 0;
	uint32_t row_length   = 0;
	uint32_t image_height = 0;
};

struct TextureUploadLayout {
	uint32_t               pitch               = 0;
	uint64_t               slice_stride        = 0;
	uint64_t               source_slice_stride = 0;
	TileSurfaceLayout      surface;
	TextureUploadMipLayout mips[16] = {};
};

vk::ComponentMapping   TextureGetComponentMapping(uint32_t swizzle);
vk::Format             TextureGetFormat(uint32_t fmt);
RenderTargetFormatInfo TextureGetRenderTargetFormat(uint32_t layout, uint32_t type, uint32_t order);
TextureUploadLayout    TextureCalcUploadLayout(uint32_t fmt, uint32_t width, uint32_t height,
                                               uint32_t levels, uint32_t depth, uint32_t tile,
                                               uint64_t upload_size, bool allow_depth_tile,
                                               bool volume_texture, const char* owner);
std::vector<vk::BufferImageCopy> TextureBuildImageCopies(const TextureUploadLayout& layout);
bool TextureBuildGpuTileInfos(uint64_t tiled_size, const std::vector<vk::BufferImageCopy>& regions,
                              const TextureUploadLayout& layout, uint32_t levels,
                              std::vector<GpuTileInfo>& infos);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_ */
