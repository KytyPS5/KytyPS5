#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::TranslateU64MaskOperation(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::S_AND_B64:
		case Decoder::Opcode::S_ANDN2_B64:
		case Decoder::Opcode::S_OR_B64:
		case Decoder::Opcode::S_ORN2_B64:
		case Decoder::Opcode::S_XOR_B64:
		case Decoder::Opcode::S_NAND_B64:
		case Decoder::Opcode::S_NOR_B64:
		case Decoder::Opcode::S_XNOR_B64:
		case Decoder::Opcode::S_NOT_B64: break;
		default: return false;
	}
	const auto invocation_result = EvaluateU64Mask(inst);
	const auto is_exec_or_vcc    = [](const Decoder::Operand& operand) {
		switch (operand.kind) {
			case Decoder::OperandKind::ExecLo:
			case Decoder::OperandKind::ExecHi:
			case Decoder::OperandKind::VccLo:
			case Decoder::OperandKind::VccHi: return true;
			default: return false;
		}
	};
	if (is_exec_or_vcc(inst.dst) || is_exec_or_vcc(inst.src0) ||
	    (inst.src_count > 1u && is_exec_or_vcc(inst.src1))) {
		WriteMask64(inst.dst, invocation_result);
		ir.SetScc(invocation_result);
		return true;
	}

	const auto lhs        = ReadU32Pair(inst.src0);
	auto       mask_valid = ReadMaskValid(inst.src0);
	if (inst.src_count > 1u) {
		mask_valid = ir.LogicalAnd(mask_valid, ReadMaskValid(inst.src1));
	}
	std::array<IR::U32, 2> result;
	if (inst.opcode == Decoder::Opcode::S_NOT_B64) {
		result = {ir.BitwiseNot(lhs[0]), ir.BitwiseNot(lhs[1])};
	} else {
		const auto rhs = ReadU32Pair(inst.src1);
		for (uint32_t component = 0; component < 2u; component++) {
			switch (inst.opcode) {
				case Decoder::Opcode::S_AND_B64:
					result[component] = ir.BitwiseAnd(lhs[component], rhs[component]);
					break;
				case Decoder::Opcode::S_ANDN2_B64:
					result[component] =
					    ir.BitwiseAnd(lhs[component], ir.BitwiseNot(rhs[component]));
					break;
				case Decoder::Opcode::S_OR_B64:
					result[component] = ir.BitwiseOr(lhs[component], rhs[component]);
					break;
				case Decoder::Opcode::S_ORN2_B64:
					result[component] = ir.BitwiseOr(lhs[component], ir.BitwiseNot(rhs[component]));
					break;
				case Decoder::Opcode::S_XOR_B64:
					result[component] = ir.BitwiseXor(lhs[component], rhs[component]);
					break;
				case Decoder::Opcode::S_NAND_B64:
					result[component] =
					    ir.BitwiseNot(ir.BitwiseAnd(lhs[component], rhs[component]));
					break;
				case Decoder::Opcode::S_NOR_B64:
					result[component] = ir.BitwiseNot(ir.BitwiseOr(lhs[component], rhs[component]));
					break;
				case Decoder::Opcode::S_XNOR_B64:
					result[component] =
					    ir.BitwiseNot(ir.BitwiseXor(lhs[component], rhs[component]));
					break;
				default: return false;
			}
		}
	}
	WriteU32Pair(inst.dst, result);
	if (inst.dst.kind == Decoder::OperandKind::Sgpr) {
		const auto dst = static_cast<IR::ScalarReg>(inst.dst.reg);
		ir.SetThreadBitScalarReg(dst, invocation_result);
		ir.SetScalarMaskTag(dst, mask_valid);
		const auto raw_nonzero =
		    ir.INotEqual(ir.BitwiseOr(result[0], result[1]), IR::U32(IR::Value(0u)));
		ir.SetScc(IR::U1(
		    ir.Emit(IR::ValueOpcode::SelectU1, {mask_valid, invocation_result, raw_nonzero})));
	} else {
		ir.SetScc(ir.INotEqual(ir.BitwiseOr(result[0], result[1]), IR::U32(IR::Value(0u))));
	}
	return true;
}

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
		case Decoder::Opcode::S_GETPC_B64: return TranslateMove(inst, error);
		case Decoder::Opcode::V_MOVRELS_B32:
		case Decoder::Opcode::V_MOVRELD_B32: return TranslateLaneOperation(inst, error);
		default: break;
	}

	switch (inst.family) {
		case Decoder::Family::SMEM:
		case Decoder::Family::MUBUF:
		case Decoder::Family::MTBUF:
		case Decoder::Family::FLAT:
		case Decoder::Family::DS:
		case Decoder::Family::MIMG: return TranslateMemoryOperation(inst, error);
		case Decoder::Family::EXP: return TranslateAttributeOperation(inst, error);
		default: break;
	}

	switch (inst.opcode) {
		case Decoder::Opcode::V_INTERP_P1_F32:
		case Decoder::Opcode::V_INTERP_P2_F32:
		case Decoder::Opcode::V_INTERP_MOV_F32: return TranslateAttributeOperation(inst, error);
		case Decoder::Opcode::S_BRANCH:
		case Decoder::Opcode::S_CBRANCH_SCC0:
		case Decoder::Opcode::S_CBRANCH_SCC1:
		case Decoder::Opcode::S_CBRANCH_VCCZ:
		case Decoder::Opcode::S_CBRANCH_VCCNZ:
		case Decoder::Opcode::S_CBRANCH_EXECZ:
		case Decoder::Opcode::S_CBRANCH_EXECNZ:
		case Decoder::Opcode::S_SETPC_B64:
		case Decoder::Opcode::S_ENDPGM: return true;
		default: break;
	}

	const bool translated = TranslateControlOperation(inst) || TranslateStateOperation(inst) ||
	                        TranslateMove(inst, error) || TranslateLaneOperation(inst, error) ||
	                        TranslateIntegerCompare(inst) || TranslateInteger16Compare(inst) ||
	                        TranslateFloatCompare(inst) || TranslateConversion(inst) ||
	                        TranslateInteger16Operation(inst) || TranslatePackedInteger16(inst) ||
	                        TranslatePackedFloat16(inst) || TranslateFloat16Operation(inst) ||
	                        TranslateFloatOperation(inst) || TranslateU64MaskOperation(inst) ||
	                        TranslateSimpleInteger(inst) || TranslateComposedInteger(inst) ||
	                        TranslateExtendedInteger(inst);
	if (!translated && error != nullptr) {
		*error = "decoded opcode has no IR translation";
	}
	return translated;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
