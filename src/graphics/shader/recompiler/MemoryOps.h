#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MEMORYOPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MEMORYOPS_H_

#include "graphics/shader/recompiler/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

bool DecodeSmem(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error);
bool DecodeMubuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction* inst, std::string* error);
bool DecodeMtbuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction* inst, std::string* error);
bool DecodeFlat(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error);
bool DecodeDs(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
              std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MEMORYOPS_H_ */
