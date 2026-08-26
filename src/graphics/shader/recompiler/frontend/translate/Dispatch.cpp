#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::TranslateInstruction(const Decoder::Instruction& inst, std::string* error) {
	current_opcode = inst.opcode;
	current_pc     = inst.pc;

	switch (inst.opcode) {
		case Decoder::Opcode::UNKNOWN:
		case Decoder::Opcode::COUNT:
			if (error != nullptr) {
				*error = "decoded opcode has no IR translation";
			}
			return false;
		case Decoder::Opcode::UNSUPPORTED:
			if (error != nullptr) {
				*error = "unsupported decoded instruction: " + Decoder::InstructionToString(inst);
			}
			return false;
		default: break;
	}

	bool translated = false;
	switch (inst.family) {
		case Decoder::Family::SOP1:
		case Decoder::Family::SOP2:
		case Decoder::Family::SOPK:
		case Decoder::Family::SOPC:
		case Decoder::Family::SOPP: translated = EmitScalar(inst, error); break;
		case Decoder::Family::VOP1:
		case Decoder::Family::VOP2:
		case Decoder::Family::VOP3:
		case Decoder::Family::VOP3P:
		case Decoder::Family::VOPC: translated = EmitVector(inst, error); break;
		case Decoder::Family::SMEM:
		case Decoder::Family::MUBUF:
		case Decoder::Family::MTBUF:
		case Decoder::Family::FLAT:
		case Decoder::Family::DS:
		case Decoder::Family::MIMG: translated = EmitMemory(inst, error); break;
		case Decoder::Family::VINTRP: translated = EmitInterpolation(inst, error); break;
		case Decoder::Family::EXP: translated = EXP(inst, error); break;
		default: break;
	}

	if (!translated && error != nullptr && error->empty()) {
		*error = "decoded opcode has no IR translation";
	}
	return translated;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
