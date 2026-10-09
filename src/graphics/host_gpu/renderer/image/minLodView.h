#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_IMAGE_MINLODVIEW_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_IMAGE_MINLODVIEW_H_

// T# MinLod on devices without VK_EXT_image_view_min_lod (e.g. MoltenVK). Pure helper, no Vulkan.

#include <cstdint>

#include "graphics/shader/minLodShift.h"

namespace Libs::Graphics {

struct MinLodViewRange {
	uint32_t base_level  = 0;
	uint32_t level_count = 0;
	uint32_t min_lod     = 0; // U4.8 clamp still to be applied relative to base_level (0 = fully mapped)
};

// `min_lod` is the T# clamp relative to `base_level` in 1/256 level units. Without a native view clamp the
// view starts at MinLodShift() levels higher (shared with the recompiler's RESINFO correction). The sampler
// (and the shader, for Dynamic mip mode) keeps working relative to the new base.
constexpr MinLodViewRange MapMinLodToBaseLevel(uint32_t base_level, uint32_t level_count, uint32_t min_lod) {
	if (min_lod == 0 || level_count == 0) {
		return {base_level, level_count, min_lod};
	}
	const uint32_t shift = MinLodShift(level_count, min_lod);
	return {base_level + shift, level_count - shift, 0};
}

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_HOST_GPU_RENDERER_IMAGE_MINLODVIEW_H_ */
