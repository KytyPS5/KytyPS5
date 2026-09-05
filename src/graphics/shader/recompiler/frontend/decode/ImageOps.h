#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IMAGEOPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IMAGEOPS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

void DecodeMimg(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);

const char* MimgSampleOpcodeName(uint32_t opcode);

struct ImageAddressComponent {
	uint32_t bit_offset;
	uint32_t bit_width;
};

ImageAddressComponent ImageAddressComponentLayout(uint32_t flags, uint32_t component);
uint32_t ImageAddressDwordCount(uint32_t flags, uint32_t components);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IMAGEOPS_H_ */
