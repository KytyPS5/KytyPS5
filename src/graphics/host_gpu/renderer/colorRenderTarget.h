#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;
struct VulkanImage;

namespace HW {
class Context;
} // namespace HW

enum class RenderColorType {
	NoColorOutput,
	DisplayBuffer,
	RenderTexture,
};

struct RenderColorInfo {
	RenderColorType                 type             = RenderColorType::NoColorOutput;
	VulkanImage*                    vulkan_buffer    = nullptr;
	vk::ImageView                   vulkan_view      = nullptr;
	vk::Format                      format           = vk::Format::eUndefined;
	vk::Extent2D                    extent           = {};
	uint32_t                        base_mip_level   = 0;
	uint32_t                        base_array_layer = 0;
	uint64_t                        base_addr        = 0;
	uint64_t                        buffer_size      = 0;
	uint32_t                        target_slot      = 0;
	Prospero::ColorComponentMapping export_mapping;
	bool                            color_clear_enable = false;
	vk::ClearColorValue             color_clear_value {};
};

void ResolveRenderColorTarget(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                              RenderColorInfo* r, uint32_t render_target_slice_offset = 0,
                              uint32_t render_target_slot            = UINT32_MAX,
                              bool     ignore_target_mask            = false,
                              bool     reuse_existing_render_texture = false);
void MarkRenderTargetGpuWritten(const RenderColorInfo& target);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
