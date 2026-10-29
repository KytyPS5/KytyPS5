#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <cstdint>

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
	// The part of the target the pass loads, draws and stores; an empty one is the whole target. A
	// pass that a draw ends again and again (an image has to change its layout) would otherwise
	// load and store a large target (a shadow atlas) in full each time.
	int32_t                                                    area_x      = 0;
	int32_t                                                    area_y      = 0;
	uint32_t                                                   area_width  = 0;
	uint32_t                                                   area_height = 0;

	bool operator==(const RenderState&) const = default;

	// The area as a rectangle: the whole target when it is empty.
	[[nodiscard]] int32_t Left() const { return area_width != 0u ? area_x : 0; }
	[[nodiscard]] int32_t Top() const { return area_width != 0u ? area_y : 0; }
	[[nodiscard]] int32_t Right() const {
		return area_width != 0u ? area_x + static_cast<int32_t>(area_width) : static_cast<int32_t>(width);
	}
	[[nodiscard]] int32_t Bottom() const {
		return area_width != 0u ? area_y + static_cast<int32_t>(area_height) : static_cast<int32_t>(height);
	}
	// Whether a pass begun with this state can take the draws of `other`: the same attachments, and
	// an area that holds the other's.
	[[nodiscard]] bool Covers(const RenderState& other) const {
		RenderState a = *this;
		RenderState b = other;
		a.area_x = b.area_x = 0;
		a.area_y = b.area_y = 0;
		a.area_width = b.area_width = 0;
		a.area_height = b.area_height = 0;
		return a == b && Left() <= other.Left() && Top() <= other.Top() && Right() >= other.Right() &&
		       Bottom() >= other.Bottom();
	}
	// The same state with an area that holds the other's as well.
	void Grow(const RenderState& other) {
		const int32_t left   = std::min(Left(), other.Left());
		const int32_t top    = std::min(Top(), other.Top());
		const int32_t right  = std::max(Right(), other.Right());
		const int32_t bottom = std::max(Bottom(), other.Bottom());
		area_x      = left;
		area_y      = top;
		area_width  = static_cast<uint32_t>(right - left);
		area_height = static_cast<uint32_t>(bottom - top);
	}
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

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
