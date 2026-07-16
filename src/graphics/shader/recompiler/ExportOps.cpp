#include "graphics/shader/recompiler/ExportOps.h"

#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

bool DecodeExp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
               std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated EXP instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0  = code[word_index];
	const uint32_t word1  = code[word_index + 1u];
	const uint32_t target = (word0 >> 4u) & 0x3fu;
	const uint32_t en     = word0 & 0xfu;

	inst->pc         = pc;
	inst->word       = word0;
	inst->word_count = 2;
	inst->family     = Family::EXP;
	inst->opcode_id  = target;
	inst->opcode     = Opcode::Exp;
	inst->exp.target = target;
	inst->exp.en     = en;
	inst->exp.compr  = ((word0 >> 10u) & 1u) != 0;
	inst->exp.done   = ((word0 >> 11u) & 1u) != 0;
	inst->exp.vm     = ((word0 >> 12u) & 1u) != 0;
	SetRawWords(inst, code, word_index, 2);

	if (!DecodeVectorGpr(word1 & 0xffu, &inst->src0, error) ||
	    !DecodeVectorGpr((word1 >> 8u) & 0xffu, &inst->src1, error) ||
	    !DecodeVectorGpr((word1 >> 16u) & 0xffu, &inst->src2, error) ||
	    !DecodeVectorGpr((word1 >> 24u) & 0xffu, &inst->src3, error)) {
		return false;
	}

	if (target == 0x14u && inst->exp.done && en == 0x1u) {
		inst->src_count = 1u;
	} else {
		inst->src_count = inst->exp.compr ? 2u : 4u;
	}
	if (en == 0u) {
		inst->src_count = 0;
	}

	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
