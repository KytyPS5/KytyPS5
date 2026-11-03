#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_

#include "graphics/shader/shader.h"

#include <span>

namespace Libs::Graphics {

struct ShaderVertexMetadata {
	int                             vertex_buffer_reg = -1;
	int                             vertex_attrib_reg = -1;
	std::span<const ShaderSemantic> input_semantics;
};

// Reads the vertex metadata as a view into the shader header. Returns nullptr if it is
// valid, otherwise why it was rejected.
const char* ShaderReadVertexMetadata(const ShaderMappedData& data, uint32_t max_user_sgprs,
                                     ShaderVertexMetadata& metadata);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_ */
