#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

bool Translator::TranslateU64MaskOperation(const IR::Instruction& inst) {
	const auto invocation_result = EvaluateU64Mask(inst);
	const auto is_exec_or_vcc    = [](const IR::Operand& operand) {
		return operand.kind == IR::OperandKind::Register &&
		       (operand.reg.file == IR::RegisterFile::Exec ||
		        operand.reg.file == IR::RegisterFile::Vcc);
	};
	if (is_exec_or_vcc(inst.dst) || is_exec_or_vcc(inst.src[0]) ||
	    (inst.src_count > 1u && is_exec_or_vcc(inst.src[1]))) {
		WriteMask64(inst.dst, invocation_result);
		ir.SetScc(invocation_result);
		return true;
	}

	const auto lhs        = ReadU32Pair(inst.src[0]);
	auto       mask_valid = ReadMaskValid(inst.src[0]);
	if (inst.src_count > 1u) {
		mask_valid = ir.LogicalAnd(mask_valid, ReadMaskValid(inst.src[1]));
	}
	std::array<IR::U32, 2> result;
	if (inst.op == IR::Opcode::BitwiseNotU64) {
		result = {ir.BitwiseNot(lhs[0]), ir.BitwiseNot(lhs[1])};
	} else {
		const auto rhs = ReadU32Pair(inst.src[1]);
		for (uint32_t component = 0; component < 2u; component++) {
			switch (inst.op) {
				case IR::Opcode::BitwiseAndU64:
					result[component] = ir.BitwiseAnd(lhs[component], rhs[component]);
					break;
				case IR::Opcode::BitwiseAndNotU64:
					result[component] =
					    ir.BitwiseAnd(lhs[component], ir.BitwiseNot(rhs[component]));
					break;
				case IR::Opcode::BitwiseOrU64:
					result[component] = ir.BitwiseOr(lhs[component], rhs[component]);
					break;
				case IR::Opcode::BitwiseOrNotU64:
					result[component] = ir.BitwiseOr(lhs[component], ir.BitwiseNot(rhs[component]));
					break;
				case IR::Opcode::BitwiseXorU64:
					result[component] = ir.BitwiseXor(lhs[component], rhs[component]);
					break;
				case IR::Opcode::BitwiseNandU64:
					result[component] =
					    ir.BitwiseNot(ir.BitwiseAnd(lhs[component], rhs[component]));
					break;
				case IR::Opcode::BitwiseNorU64:
					result[component] = ir.BitwiseNot(ir.BitwiseOr(lhs[component], rhs[component]));
					break;
				case IR::Opcode::BitwiseXnorU64:
					result[component] =
					    ir.BitwiseNot(ir.BitwiseXor(lhs[component], rhs[component]));
					break;
				default: return false;
			}
		}
	}
	WriteU32Pair(inst.dst, result);
	if (inst.dst.kind == IR::OperandKind::Register &&
	    inst.dst.reg.file == IR::RegisterFile::Scalar) {
		const auto dst = static_cast<IR::ScalarReg>(inst.dst.reg.index);
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

bool Translator::TranslateInstruction(const IR::Instruction& inst) {
	switch (IR::GetOpcodeInfo(inst.op).lowering_class) {
		case IR::LoweringClass::Control: return TranslateControlOperation(inst);
		case IR::LoweringClass::Move: return TranslateMove(inst);
		case IR::LoweringClass::Lane: return TranslateLaneOperation(inst);
		case IR::LoweringClass::State: return TranslateStateOperation(inst);
		case IR::LoweringClass::Memory: return TranslateMemoryOperation(inst);
		case IR::LoweringClass::Attribute: return TranslateAttributeOperation(inst);
		case IR::LoweringClass::IntegerCompare: return TranslateIntegerCompare(inst);
		case IR::LoweringClass::Integer16Compare: return TranslateInteger16Compare(inst);
		case IR::LoweringClass::FloatCompare: return TranslateFloatCompare(inst);
		case IR::LoweringClass::Conversion: return TranslateConversion(inst);
		case IR::LoweringClass::Integer16: return TranslateInteger16Operation(inst);
		case IR::LoweringClass::PackedInteger16: return TranslatePackedInteger16(inst);
		case IR::LoweringClass::PackedFloat16: return TranslatePackedFloat16(inst);
		case IR::LoweringClass::Float16: return TranslateFloat16Operation(inst);
		case IR::LoweringClass::Float: return TranslateFloatOperation(inst);
		case IR::LoweringClass::U64Mask: return TranslateU64MaskOperation(inst);
		case IR::LoweringClass::SimpleInteger: return TranslateSimpleInteger(inst);
		case IR::LoweringClass::ComposedInteger: return TranslateComposedInteger(inst);
		case IR::LoweringClass::ExtendedInteger: return TranslateExtendedInteger(inst);
	}
	return false;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
