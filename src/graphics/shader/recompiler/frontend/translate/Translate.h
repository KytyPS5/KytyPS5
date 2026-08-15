#pragma once

#include "graphics/shader/recompiler/ir/ValueProgram.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool TranslateProgram(const IR::Program& source, IR::ValueProgram& result,
                      const ShaderVertexInputInfo*  vertex_input_info,
                      const ShaderPixelInputInfo*   pixel_input_info,
                      const ShaderComputeInputInfo* compute_input_info, std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
