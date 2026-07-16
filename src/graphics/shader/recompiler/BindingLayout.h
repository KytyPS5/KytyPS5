#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_

#include "graphics/shader/recompiler/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct BindingLayoutOptions {
	uint32_t descriptor_set       = 0;
	uint32_t push_constant_offset = 0;
	uint32_t max_push_dwords      = 32;
};

bool AllocateBindings(Program* program, const BindingLayoutOptions& options, std::string* error);

const DescriptorBinding* FindBinding(const BindingLayout& layout, DescriptorBindingKind kind);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_ */
