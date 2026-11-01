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
// The clip distances a rectangle-list vertex program writes in gl_PerVertex (its zero-position
// plane included), which the generated stages declare and pass on. Cull distances stay out:
// MoltenVK supports none.
uint32_t RectListClipDistances(const ShaderRecompiler::IR::CompiledShaderInfo& vertex);
// Forwards the parameters the vertex stage exports, one per output location, and its clip
// distances.
RectListShaders BuildRectListShaders(std::span<const RectListParameter> parameters,
                                     uint32_t param_export_mask, uint32_t clip_distances = 0);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
