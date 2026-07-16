#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_VECTORALUOPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_VECTORALUOPS_H_

#include "graphics/shader/recompiler/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

bool DecodeVop2(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error);
bool DecodeVop1(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error);
bool DecodeVopc(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error);
bool DecodeVop3(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error);
bool DecodeVop3p(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction* inst, std::string* error);
bool DecodeVintrp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                  Instruction* inst, std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_VECTORALUOPS_H_ */
