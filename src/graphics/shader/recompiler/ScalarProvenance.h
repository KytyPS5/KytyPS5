#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SCALARPROVENANCE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SCALARPROVENANCE_H_

#include "graphics/shader/recompiler/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Builds scalar reaching definitions and attaches descriptor sources to individual memory uses.
bool BuildScalarProvenance(Program* program, std::string* error);

uint32_t ScalarValueArgCount(ScalarValueOp op);

const DescriptorValue* GetDescriptorSource(const Program& program, uint32_t source);
bool                   DescriptorSourceResolved(const Program& program, uint32_t source);

std::string ScalarValueToString(const ScalarProvenance& provenance, uint32_t value);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SCALARPROVENANCE_H_ */
