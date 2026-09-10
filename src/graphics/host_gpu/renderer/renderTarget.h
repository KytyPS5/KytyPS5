#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_

#include "common/slotVector.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace Libs::Graphics {

static constexpr uint32_t RENDER_COLOR_ATTACHMENTS_MAX = 8;

struct RenderAttachment {
	vk::ImageView           image_view    = nullptr;
	vk::ImageLayout         image_layout  = vk::ImageLayout::eUndefined;
	std::array<uint32_t, 4> clear_value   = {};
	bool                    is_clear      = false;
	bool                    has_depth     = false;
	bool                    depth_clear   = false;
	bool                    has_stencil   = false;
	bool                    stencil_clear = false;
	// Debug-only: the color attachment's cache slot, so --draw-dump-folder can look the
	// image back up (for the actual vk::Image + format) once rendering to it ends. Left
	// default for the depth/stencil attachment, which draw-dump does not cover.
	Common::SlotId          image_id;
	// Debug-only, like image_id above: the last draw's shader identity and guest primitive type,
	// so --draw-dump can name a dumped PNG after the shader/primitive that produced it instead of
	// leaving a manual shader-hash hunt to a human. Unlike image_id, these change on every draw
	// even while the render target stays the same, so they
	// are deliberately excluded from operator== below -- including them would defeat
	// CommandBuffer::BeginRendering's same-state batching check and force a new render pass per
	// draw instead of per render-target switch.
	uint64_t                vs_shader_hash = 0;
	uint64_t                ps_shader_hash = 0;
	uint32_t                prim_type      = 0;
	// Debug-only, same rationale as above: the first few bound sampled textures' guest address,
	// guest format, and tile mode, so a dumped PNG can be traced to the exact texture bytes that
	// produced it without a second instrumentation pass. Was capped at 2 (missed the 3rd/4th
	// texture of any wider composite pass entirely, with no indication anything was cut); 4
	// keeps the sidecar CSV a fixed-width row while covering the common multi-texture blend
	// case this investigation actually needed.
	static constexpr uint32_t TEX_DEBUG_MAX      = 4;
	uint64_t                tex_address[TEX_DEBUG_MAX] = {};
	uint32_t                tex_format[TEX_DEBUG_MAX]  = {};
	uint32_t                tex_tile_mode[TEX_DEBUG_MAX] = {};

	bool operator==(const RenderAttachment& other) const {
		return image_view == other.image_view && image_layout == other.image_layout &&
		       clear_value == other.clear_value && is_clear == other.is_clear &&
		       has_depth == other.has_depth && depth_clear == other.depth_clear &&
		       has_stencil == other.has_stencil && stencil_clear == other.stencil_clear &&
		       image_id == other.image_id;
	}
};

struct RenderState {
	std::array<RenderAttachment, RENDER_COLOR_ATTACHMENTS_MAX> color_attachments;
	RenderAttachment                                           depth_stencil_attachment;
	uint32_t                                                   width                 = 0;
	uint32_t                                                   height                = 0;
	uint32_t                                                   num_layers            = 1;
	uint32_t                                                   num_color_attachments = 0;

	bool operator==(const RenderState&) const = default;
};

[[nodiscard]] inline constexpr uint32_t render_sample_count(uint32_t encoded_samples) {
	return encoded_samples <= 3 ? 1u << encoded_samples : 0;
}

[[nodiscard]] inline constexpr vk::SampleCountFlagBits vulkan_sample_count(uint32_t samples) {
	switch (samples) {
		case 1: return vk::SampleCountFlagBits::e1;
		case 2: return vk::SampleCountFlagBits::e2;
		case 4: return vk::SampleCountFlagBits::e4;
		case 8: return vk::SampleCountFlagBits::e8;
		default: return {};
	}
}

enum class TargetViewType : uint8_t { Image2D, Image2DArray, Unsupported };

struct TargetViewInfo {
	TargetViewType type         = TargetViewType::Unsupported;
	uint32_t       base_layer   = 0;
	uint32_t       layer_count  = 0;
	uint32_t       image_layers = 0;
};

inline constexpr TargetViewInfo ResolveTargetViewInfo(uint32_t base_layer, uint32_t last_layer,
                                                      uint32_t draw_layer_offset = 0) {
	if (base_layer > last_layer || draw_layer_offset != 0) {
		return {};
	}
	return {base_layer == last_layer ? TargetViewType::Image2D : TargetViewType::Image2DArray,
	        base_layer, last_layer - base_layer + 1u, last_layer + 1u};
}

#pragma pack(push, 1)

struct PipelineStencilStaticState {
	vk::StencilOp failOp      = vk::StencilOp::eKeep;
	vk::StencilOp passOp      = vk::StencilOp::eKeep;
	vk::StencilOp depthFailOp = vk::StencilOp::eKeep;
	vk::CompareOp compareOp   = vk::CompareOp::eNever;
};

struct PipelineStencilDynamicState {
	uint32_t compareMask = 0;
	uint32_t writeMask   = 0;
	uint32_t reference   = 0;
};

#pragma pack(pop)

inline constexpr bool stencil_face_accesses_attachment(const PipelineStencilStaticState&  state,
                                                       const PipelineStencilDynamicState& dynamic) {
	return state.compareOp != vk::CompareOp::eAlways ||
	       (dynamic.writeMask != 0 &&
	        (state.failOp != vk::StencilOp::eKeep || state.passOp != vk::StencilOp::eKeep ||
	         state.depthFailOp != vk::StencilOp::eKeep));
}

static_assert(std::is_trivially_copyable_v<PipelineStencilStaticState>);
static_assert(std::is_standard_layout_v<PipelineStencilStaticState>);
static_assert(alignof(PipelineStencilStaticState) == 1);
static_assert(sizeof(PipelineStencilStaticState) ==
              sizeof(vk::StencilOp) * 3 + sizeof(vk::CompareOp));
static_assert(std::is_trivially_copyable_v<PipelineStencilDynamicState>);
static_assert(std::is_standard_layout_v<PipelineStencilDynamicState>);
static_assert(alignof(PipelineStencilDynamicState) == 1);
static_assert(sizeof(PipelineStencilDynamicState) == sizeof(uint32_t) * 3);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
