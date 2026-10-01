#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_

#include "graphics/shader/shader.h"

#include <span>
#include <string>

namespace Libs::Graphics {

struct ShaderVertexMetadata {
	int                             vertex_buffer_reg = -1;
	int                             vertex_attrib_reg = -1;
	std::span<const ShaderSemantic> input_semantics;
};

// Validates the vertex metadata and returns a view into the shader header.
bool ShaderReadVertexMetadata(const ShaderMappedData& data, uint32_t max_user_sgprs,
                              ShaderVertexMetadata& metadata, std::string* error);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_ */
