#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SCALARALUOPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SCALARALUOPS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

void DecodeSop1(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);
void DecodeSop2(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);
void DecodeSopk(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);
void DecodeSopc(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);
void DecodeSopp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SCALARALUOPS_H_ */
