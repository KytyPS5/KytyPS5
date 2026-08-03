#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

class RenderCommandBuffer;

enum class RenderColorType {
	NoColorOutput,
	RenderTexture,
};

struct RenderColorInfo {
	RenderColorType                 type = RenderColorType::NoColorOutput;
	TextureCache::ImageDesc         desc;
	ImageId                         image_id;
	vk::ImageView                   image_view       = nullptr;
	vk::Format                      format           = vk::Format::eUndefined;
	vk::Extent2D                    extent           = {};
	uint32_t                        base_mip_level   = 0;
	uint32_t                        base_array_layer = 0;
	uint64_t                        base_addr        = 0;
	uint64_t                        buffer_size      = 0;
	uint32_t                        target_slot      = 0;
	uint32_t                        samples          = 1;
	Prospero::ColorComponentMapping export_mapping;
	bool                            color_clear_enable = false;
	vk::ClearColorValue             color_clear_value {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
