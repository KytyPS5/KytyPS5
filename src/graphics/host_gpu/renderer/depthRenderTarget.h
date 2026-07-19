#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEPTHRENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEPTHRENDERTARGET_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;
struct DepthStencilVulkanImage;

namespace HW {
class Context;
} // namespace HW

inline constexpr bool depth_htile_stencil_acceleration_compatible(bool has_stencil, bool has_htile,
                                                                  bool acceleration_disabled) {
	return acceleration_disabled || (has_stencil && has_htile);
}

struct RenderDepthInfo {
	vk::Format                  format                   = vk::Format::eUndefined;
	uint32_t                    width                    = 0;
	uint32_t                    height                   = 0;
	bool                        htile                    = false;
	uint64_t                    depth_buffer_size        = 0;
	uint64_t                    depth_buffer_vaddr       = 0;
	uint64_t                    depth_tile_swizzle       = 0;
	uint64_t                    stencil_buffer_size      = 0;
	uint64_t                    stencil_buffer_vaddr     = 0;
	uint64_t                    stencil_tile_swizzle     = 0;
	uint64_t                    htile_buffer_size        = 0;
	uint64_t                    htile_buffer_vaddr       = 0;
	uint64_t                    htile_tile_swizzle       = 0;
	bool                        depth_clear_enable       = false;
	bool                        depth_load_clear_enable  = false;
	bool                        depth_meta_clear_enable  = false;
	float                       depth_clear_value        = 0.0f;
	bool                        depth_test_enable        = false;
	bool                        depth_write_enable       = false;
	vk::CompareOp               depth_compare_op         = vk::CompareOp::eNever;
	bool                        depth_bounds_test_enable = false;
	float                       depth_min_bounds         = 0.0f;
	float                       depth_max_bounds         = 0.0f;
	bool                        stencil_clear_enable     = false;
	uint8_t                     stencil_clear_value      = 0;
	bool                        stencil_test_enable      = false;
	PipelineStencilStaticState  stencil_static_front;
	PipelineStencilStaticState  stencil_static_back;
	PipelineStencilDynamicState stencil_dynamic_front;
	PipelineStencilDynamicState stencil_dynamic_back;
	DepthStencilVulkanImage*    vulkan_buffer = nullptr;
	vk::ImageView               vulkan_view   = nullptr;
	uint64_t                    vaddr[3]      = {};
	uint64_t                    size[3]       = {};
	int                         vaddr_num     = 0;
};

inline bool depth_attachment_read_only(const RenderDepthInfo* depth) {
	EXIT_IF(depth == nullptr);
	const bool stencil_write =
	    depth->stencil_test_enable &&
	    (depth->stencil_dynamic_front.writeMask != 0 || depth->stencil_dynamic_back.writeMask != 0);
	return !depth->depth_load_clear_enable && !depth->stencil_clear_enable &&
	       !depth->depth_write_enable && !stencil_write;
}

inline vk::ImageLayout depth_attachment_layout(const RenderDepthInfo* depth) {
	return depth_attachment_read_only(depth) ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
	                                         : vk::ImageLayout::eDepthStencilAttachmentOptimal;
}

void ResolveRenderDepthTarget(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                              RenderDepthInfo* r);
void MarkRenderTargetGpuWritten(const RenderDepthInfo& target);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEPTHRENDERTARGET_H_
