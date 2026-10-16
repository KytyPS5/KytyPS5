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
	// Discovery keeps guest image information but can remap the view into a larger cache image.
	TextureCache::ImageDesc     desc;
	bool                        depth_clear_enable       = false;
	bool                        depth_load_clear_enable  = false;
	float                       depth_clear_value        = 0.0f;
	bool                        depth_test_enable        = false;
	// Effective draw writes; discovery applies test, target-write and clear controls.
	bool                        depth_write_enable       = false;
	vk::CompareOp               depth_compare_op         = vk::CompareOp::eNever;
	bool                        depth_bounds_test_enable = false;
	bool                        stencil_clear_enable     = false;
	uint8_t                     stencil_clear_value      = 0;
	bool                        stencil_test_enable      = false;
	vk::StencilOpState          stencil_front;
	vk::StencilOpState          stencil_back;
	ImageId                     image_id;

	[[nodiscard]] vk::ImageAspectFlags AttachmentWriteAspects() const;
};

inline vk::ImageAspectFlags DepthFeedbackAspects(vk::ImageAspectFlags draw_writes,
                                                 const ImageViewInfo& target,
                                                 const ImageViewInfo& sampled) {
	if (!ImageRangeOverlaps(target.base_level, target.level_count, sampled.base_level,
	                        sampled.level_count) ||
	    !ImageRangeOverlaps(target.base_layer, target.layer_count, sampled.base_layer,
	                        sampled.layer_count)) {
		return {};
	}
	return draw_writes & sampled.aspect;
}

inline vk::ImageAspectFlags DepthReadableAspects(vk::ImageLayout layout) {
	switch (layout) {
		case vk::ImageLayout::eDepthReadOnlyOptimal:
		case vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal:
			return vk::ImageAspectFlagBits::eDepth;
		case vk::ImageLayout::eStencilReadOnlyOptimal:
		case vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal:
			return vk::ImageAspectFlagBits::eStencil;
		case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		case vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT:
		case vk::ImageLayout::eGeneral:
			return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
		default:
			return {};
	}
}

// Aspects a draw may write through an attachment in this layout.
inline vk::ImageAspectFlags DepthWritableAspects(vk::ImageLayout layout) {
	switch (layout) {
		case vk::ImageLayout::eDepthStencilAttachmentOptimal:
			return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
		case vk::ImageLayout::eDepthAttachmentOptimal:
		case vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal:
			return vk::ImageAspectFlagBits::eDepth;
		case vk::ImageLayout::eStencilAttachmentOptimal:
		case vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal:
			return vk::ImageAspectFlagBits::eStencil;
		default: return {};
	}
}

// Whether a depth attachment in `layout` permits a draw that writes `writes` (draw writes and load
// clears alike) and samples `sampled` aspects. Feedback-loop layouts are left to the caller.
inline bool DepthAttachmentLayoutPermits(vk::ImageLayout layout, vk::ImageAspectFlags writes,
                                         vk::ImageAspectFlags sampled) {
	switch (layout) {
		case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		case vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal:
		case vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal:
		case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		case vk::ImageLayout::eDepthAttachmentOptimal:
		case vk::ImageLayout::eDepthReadOnlyOptimal:
		case vk::ImageLayout::eStencilAttachmentOptimal:
		case vk::ImageLayout::eStencilReadOnlyOptimal:
			return !(writes & ~DepthWritableAspects(layout)) &&
			       !(sampled & ~DepthReadableAspects(layout));
		default: return false;
	}
}

// The layout a depth attachment enters for a draw that writes `writes` and samples `sampled`
// aspects, the sampled ones read-only. Draws keep it while it permits their accesses: changing it
// ends the render pass and can make the GPU decompress the surface. With nothing sampled every
// aspect is writable, as games alternate which aspects consecutive draws write; otherwise
// unwritten aspects are read-only as well, so draws that only test and sample need no barriers.
inline vk::ImageLayout depth_attachment_layout(vk::ImageAspectFlags available,
                                               vk::ImageAspectFlags writes,
                                               vk::ImageAspectFlags sampled) {
	const auto writable         = sampled ? writes & ~sampled : available;
	const bool has_depth        = static_cast<bool>(available & vk::ImageAspectFlagBits::eDepth);
	const bool has_stencil      = static_cast<bool>(available & vk::ImageAspectFlagBits::eStencil);
	const bool depth_writable   = static_cast<bool>(writable & vk::ImageAspectFlagBits::eDepth);
	const bool stencil_writable = static_cast<bool>(writable & vk::ImageAspectFlagBits::eStencil);
	if (!has_stencil) {
		return depth_writable ? vk::ImageLayout::eDepthAttachmentOptimal
		                      : vk::ImageLayout::eDepthReadOnlyOptimal;
	}
	if (!has_depth) {
		return stencil_writable ? vk::ImageLayout::eStencilAttachmentOptimal
		                        : vk::ImageLayout::eStencilReadOnlyOptimal;
	}
	if (depth_writable && stencil_writable) {
		return vk::ImageLayout::eDepthStencilAttachmentOptimal;
	}
	if (depth_writable) {
		return vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal;
	}
	if (stencil_writable) {
		return vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal;
	}
	return vk::ImageLayout::eDepthStencilReadOnlyOptimal;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEPTHRENDERTARGET_H_
