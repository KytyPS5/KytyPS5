#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <type_traits>

namespace Libs::Graphics {

static constexpr uint32_t RENDER_COLOR_ATTACHMENTS_MAX = 8;

inline constexpr bool depth_msaa_single_sample_compatible(uint32_t encoded_fragments) {
	return encoded_fragments == 1 || encoded_fragments == 2;
}

inline constexpr bool color_msaa_single_sample_compatible(uint32_t encoded_samples,
                                                          uint32_t encoded_fragments) {
	return encoded_samples == encoded_fragments &&
	       depth_msaa_single_sample_compatible(encoded_fragments);
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
