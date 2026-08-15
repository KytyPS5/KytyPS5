#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_EXPORTOPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_EXPORTOPS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

void DecodeExp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_EXPORTOPS_H_ */
