#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_

#include <cstdint>

namespace Libs::Graphics {

struct ShaderVertexInputInfo;

[[nodiscard]] int32_t  ResolveVertexOffset(uint32_t                     index_offset,
                                           const ShaderVertexInputInfo& vs_input_info);
[[nodiscard]] uint32_t ResolveInstanceOffset(const ShaderVertexInputInfo& vs_input_info);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
