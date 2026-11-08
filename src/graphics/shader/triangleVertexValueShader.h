#ifndef EMULATOR_SRC_GRAPHICS_SHADER_TRIANGLEVERTEXVALUESHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_TRIANGLEVERTEXVALUESHADER_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct ShaderPixelInputInfo;
struct ShaderVertexInputInfo;

// A pixel shader may read the raw value of an input at each vertex of its triangle. A host with
// no per-vertex pixel shader inputs cannot give it those values. The draw then runs as patches of
// three control points through these two shaders. They change nothing about the triangle: the
// control shader passes the vertices on, and the evaluation shader emits the same three vertices
// at tessellation level 1. On the way they copy the value at each vertex into three flat outputs
// (see ShaderPixelVertexValueLocations), where the pixel shader reads them.
struct TriangleVertexValueShaders {
	std::vector<uint32_t> control;
	std::vector<uint32_t> evaluation;
};

// True when the pixel shader reads raw vertex values of any input.
bool ShaderPixelReadsVertexValues(const ShaderPixelInputInfo& pixel_info);

TriangleVertexValueShaders BuildTriangleVertexValueShaders(const ShaderVertexInputInfo& vertex_info,
                                                           const ShaderPixelInputInfo&  pixel_info,
                                                           bool provoking_vertex_last);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_TRIANGLEVERTEXVALUESHADER_H_
