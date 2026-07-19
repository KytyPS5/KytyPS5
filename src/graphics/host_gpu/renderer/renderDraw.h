#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_

#include "graphics/host_gpu/renderer/renderTarget.h"

#include <cstdint>
#include <type_traits>

namespace Libs::Graphics {

struct ImageImageCopy;
struct RenderColorInfo;
struct ShaderVertexInputInfo;

#pragma pack(push, 1)

struct PipelineDynamicParameters {
	bool stencil_test_enable = false;

	float viewport_scale[3]  = {};
	float viewport_offset[3] = {};
	int   scissor_ltrb[4]    = {0};

	float    line_width                                       = 1.0f;
	uint32_t color_write_count                                = 1;
	bool     color_write_enable[RENDER_COLOR_ATTACHMENTS_MAX] = {true, true};

	PipelineStencilDynamicState stencil_front;
	PipelineStencilDynamicState stencil_back;
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PipelineDynamicParameters>);
static_assert(std::is_standard_layout_v<PipelineDynamicParameters>);
static_assert(alignof(PipelineDynamicParameters) == 1);
static_assert(sizeof(PipelineDynamicParameters) ==
              sizeof(bool) + sizeof(float[3]) + sizeof(float[3]) + sizeof(int[4]) + sizeof(float) +
                  sizeof(uint32_t) + sizeof(bool[RENDER_COLOR_ATTACHMENTS_MAX]) +
                  sizeof(PipelineStencilDynamicState) * 2);

[[nodiscard]] bool           IsSameColorResolveSubresource(const RenderColorInfo& src,
                                                           const RenderColorInfo& dst);
[[nodiscard]] ImageImageCopy MakeColorResolveCopy(const RenderColorInfo& src,
                                                  const RenderColorInfo& dst, uint32_t width,
                                                  uint32_t height);
[[nodiscard]] int32_t        ResolveVertexOffset(uint32_t                     index_offset,
                                                 const ShaderVertexInputInfo& vs_input_info);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
