#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct ShaderPixelInputInfo;

namespace ShaderRecompiler::IR {
struct CompiledShaderInfo;
} // namespace ShaderRecompiler::IR

// A pixel parameter the generated stages forward from vertex export input_location.
struct RectListParameter {
	uint32_t input_location  = 0;
	uint32_t output_location = 0;
	bool     flat            = false;
};

struct RectListShaders {
	std::vector<uint32_t> control;
	std::vector<uint32_t> evaluation;
};

// Every parameter a rectangle-list pixel program reads.
std::vector<RectListParameter> RectListParameters(
    const ShaderPixelInputInfo& pixel_info, const ShaderRecompiler::IR::CompiledShaderInfo& program);
// Forwards the parameters the vertex stage exports, one per output location.
// `clip_count`: the clip distances the vertex shader writes (Spirv::VertexClipDistanceCount). They
// go through the rectangle's stages like the position, so a clip plane cuts the rectangle as it
// would the guest's.
RectListShaders BuildRectListShaders(std::span<const RectListParameter> parameters,
                                     uint32_t param_export_mask, uint32_t clip_count);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
