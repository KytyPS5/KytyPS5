#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IMAGEOPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IMAGEOPS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

bool DecodeMimg(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst,
                std::string* error);

const char* MimgSampleOpcodeName(uint32_t opcode);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IMAGEOPS_H_ */
