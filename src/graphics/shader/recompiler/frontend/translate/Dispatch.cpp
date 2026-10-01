#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::FailMissingTranslation(const Decoder::Instruction& inst) {
	EXIT("opcode %s at pc 0x%08x has no IR translation",
	     Decoder::InstructionToString(inst).c_str(), inst.pc);
}

void Translator::TranslateInstruction(const Decoder::Instruction& inst) {
	current_opcode = inst.opcode;
	current_pc     = inst.pc;

	switch (inst.opcode) {
		case Decoder::Opcode::UNKNOWN:
		case Decoder::Opcode::COUNT:
			EXIT("decoded opcode has no IR translation at pc 0x%08x", inst.pc);
		case Decoder::Opcode::UNSUPPORTED:
			EXIT("unsupported decoded instruction: %s", Decoder::InstructionToString(inst).c_str());
		default: break;
	}

	switch (inst.family) {
		case Decoder::Family::SOP1:
		case Decoder::Family::SOP2:
		case Decoder::Family::SOPK:
		case Decoder::Family::SOPC:
		case Decoder::Family::SOPP: return EmitScalar(inst);
		case Decoder::Family::VOP1:
		case Decoder::Family::VOP2:
		case Decoder::Family::VOP3:
		case Decoder::Family::VOP3P:
		case Decoder::Family::VOPC: return EmitVector(inst);
		case Decoder::Family::SMEM:
		case Decoder::Family::MUBUF:
		case Decoder::Family::MTBUF:
		case Decoder::Family::FLAT:
		case Decoder::Family::DS:
		case Decoder::Family::MIMG: return EmitMemory(inst);
		case Decoder::Family::VINTRP: return EmitInterpolation(inst);
		case Decoder::Family::EXP: return EXP(inst);
		default: return FailMissingTranslation(inst);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
