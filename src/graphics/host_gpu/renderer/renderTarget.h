#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_

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

	bool operator==(const RenderAttachment&) const = default;
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
