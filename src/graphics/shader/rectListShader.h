#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct ShaderPixelInputInfo;
struct ShaderVertexInputInfo;

struct RectListShaders {
	std::vector<uint32_t> control;
	std::vector<uint32_t> evaluation;
};

RectListShaders BuildRectListShaders(const ShaderVertexInputInfo& vertex_info,
                                     const ShaderPixelInputInfo*  pixel_info);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
