#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEPTHRENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEPTHRENDERTARGET_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

// Without a stencil plane, Hi-Stencil fields are inactive. An active plane is compatible when
// Hi-Stencil is disabled or HTile backing is present.
inline constexpr bool depth_htile_stencil_acceleration_compatible(bool has_stencil, bool has_htile,
                                                                  bool htile_stencil_disabled) {
	return !has_stencil || htile_stencil_disabled || has_htile;
}

struct RenderDepthInfo {
	TextureCache::ImageDesc     desc;
	vk::Format                  format                   = vk::Format::eUndefined;
	uint32_t                    width                    = 0;
	uint32_t                    height                   = 0;
	uint32_t                    samples                  = 1;
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
	ImageId                     image_id;
	vk::ImageView               image_view = nullptr;
	uint64_t                    vaddr[3]   = {};
	uint64_t                    size[3]    = {};
	int                         vaddr_num  = 0;

	[[nodiscard]] vk::ImageAspectFlags AttachmentWriteAspects() const;
};

inline bool depth_attachment_read_only(const RenderDepthInfo& depth) {
	return !depth.AttachmentWriteAspects();
}

inline vk::ImageLayout depth_attachment_layout(const RenderDepthInfo& depth) {
	const auto available     = ImageViewOps::DepthAspectMask(depth.format);
	const auto writes        = depth.AttachmentWriteAspects();
	const bool has_depth     = static_cast<bool>(available & vk::ImageAspectFlagBits::eDepth);
	const bool has_stencil   = static_cast<bool>(available & vk::ImageAspectFlagBits::eStencil);
	const bool depth_write   = static_cast<bool>(writes & vk::ImageAspectFlagBits::eDepth);
	const bool stencil_write = static_cast<bool>(writes & vk::ImageAspectFlagBits::eStencil);
	if (!has_stencil) {
		return depth_write ? vk::ImageLayout::eDepthAttachmentOptimal
		                   : vk::ImageLayout::eDepthReadOnlyOptimal;
	}
	if (!has_depth) {
		return stencil_write ? vk::ImageLayout::eStencilAttachmentOptimal
		                     : vk::ImageLayout::eStencilReadOnlyOptimal;
	}
	if (depth_write && stencil_write) {
		return vk::ImageLayout::eDepthStencilAttachmentOptimal;
	}
	if (depth_write) {
		return vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal;
	}
	if (stencil_write) {
		return vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal;
	}
	return vk::ImageLayout::eDepthStencilReadOnlyOptimal;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEPTHRENDERTARGET_H_
