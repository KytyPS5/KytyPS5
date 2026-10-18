#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/vulkanCommon.h"

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

vk::ComponentMapping   TextureGetComponentMapping(uint32_t                        swizzle,
                                                  Prospero::ColorComponentMapping host_to_storage);
SurfaceFormatInfo      TextureGetSurfaceFormatInfo(Prospero::BufferFormat format);
RenderTargetFormatInfo TextureGetRenderTargetFormat(Prospero::ChannelLayout layout,
                                                    Prospero::ChannelType   type,
                                                    Prospero::ChannelOrder  order);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TEXTURECOMMON_H_ */
