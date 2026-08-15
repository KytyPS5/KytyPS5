#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

bool Translator::TranslateIntegerCompare(const IR::Instruction& inst) {
	IR::ValueOpcode opcode {};
	IR::Type        type = IR::Type::U32;
	switch (inst.op) {
		case IR::Opcode::CompareFalse:
			WriteCompareResult(inst.dst, IR::U1(IR::Value(false)));
			return true;
		case IR::Opcode::CompareTrue:
			WriteCompareResult(inst.dst, IR::U1(IR::Value(true)));
			return true;
		case IR::Opcode::CompareEqU32:
		case IR::Opcode::CompareMaskEqU32:
		case IR::Opcode::CompareEqI32:
		case IR::Opcode::CompareMaskEqI32: opcode = IR::ValueOpcode::IEqual32; break;
		case IR::Opcode::CompareNeU32:
		case IR::Opcode::CompareMaskNeU32:
		case IR::Opcode::CompareNeI32:
		case IR::Opcode::CompareMaskNeI32: opcode = IR::ValueOpcode::INotEqual32; break;
		case IR::Opcode::CompareGtU32:
		case IR::Opcode::CompareMaskGtU32: opcode = IR::ValueOpcode::UGreaterThan32; break;
		case IR::Opcode::CompareGeU32:
		case IR::Opcode::CompareMaskGeU32: opcode = IR::ValueOpcode::UGreaterThanEqual32; break;
		case IR::Opcode::CompareLtU32:
		case IR::Opcode::CompareMaskLtU32: opcode = IR::ValueOpcode::ULessThan32; break;
		case IR::Opcode::CompareLeU32:
		case IR::Opcode::CompareMaskLeU32: opcode = IR::ValueOpcode::ULessThanEqual32; break;
		case IR::Opcode::CompareGtI32:
		case IR::Opcode::CompareMaskGtI32: opcode = IR::ValueOpcode::SGreaterThan32; break;
		case IR::Opcode::CompareGeI32:
		case IR::Opcode::CompareMaskGeI32: opcode = IR::ValueOpcode::SGreaterThanEqual32; break;
		case IR::Opcode::CompareLtI32:
		case IR::Opcode::CompareMaskLtI32: opcode = IR::ValueOpcode::SLessThan32; break;
		case IR::Opcode::CompareLeI32:
		case IR::Opcode::CompareMaskLeI32: opcode = IR::ValueOpcode::SLessThanEqual32; break;
		case IR::Opcode::CompareEqU64:
			opcode = IR::ValueOpcode::IEqual64;
			type   = IR::Type::U64;
			break;
		case IR::Opcode::CompareGtU64:
			opcode = IR::ValueOpcode::UGreaterThan64;
			type   = IR::Type::U64;
			break;
		case IR::Opcode::CompareNeU64:
			opcode = IR::ValueOpcode::INotEqual64;
			type   = IR::Type::U64;
			break;
		default: return false;
	}
	const auto lhs = ReadOperand(inst.src[0], type);
	const auto rhs = ReadOperand(inst.src[1], type);
	WriteCompareResult(inst.dst, IR::U1(ir.Emit(opcode, {lhs, rhs})));
	return true;
}

bool Translator::TranslateInteger16Compare(const IR::Instruction& inst) {
	IR::ValueOpcode opcode {};
	bool            signed_value = false;
	switch (inst.op) {
		case IR::Opcode::CompareEqU16:
		case IR::Opcode::CompareEqI16: opcode = IR::ValueOpcode::IEqual32; break;
		case IR::Opcode::CompareNeU16:
		case IR::Opcode::CompareNeI16: opcode = IR::ValueOpcode::INotEqual32; break;
		case IR::Opcode::CompareGtU16: opcode = IR::ValueOpcode::UGreaterThan32; break;
		case IR::Opcode::CompareGeU16: opcode = IR::ValueOpcode::UGreaterThanEqual32; break;
		case IR::Opcode::CompareLtU16: opcode = IR::ValueOpcode::ULessThan32; break;
		case IR::Opcode::CompareLeU16: opcode = IR::ValueOpcode::ULessThanEqual32; break;
		case IR::Opcode::CompareGtI16:
			opcode       = IR::ValueOpcode::SGreaterThan32;
			signed_value = true;
			break;
		case IR::Opcode::CompareGeI16:
			opcode       = IR::ValueOpcode::SGreaterThanEqual32;
			signed_value = true;
			break;
		case IR::Opcode::CompareLtI16:
			opcode       = IR::ValueOpcode::SLessThan32;
			signed_value = true;
			break;
		case IR::Opcode::CompareLeI16:
			opcode       = IR::ValueOpcode::SLessThanEqual32;
			signed_value = true;
			break;
		default: return false;
	}
	if (inst.op == IR::Opcode::CompareEqI16 || inst.op == IR::Opcode::CompareNeI16) {
		signed_value = true;
	}
	WriteCompareResult(inst.dst,
	                   IR::U1(ir.Emit(opcode, {ReadU16AsU32(inst.src[0], signed_value),
	                                           ReadU16AsU32(inst.src[1], signed_value)})));
	return true;
}

bool Translator::TranslateFloatCompare(const IR::Instruction& inst) {
	IR::ValueOpcode opcode {};
	bool            half = false;
	switch (inst.op) {
		case IR::Opcode::CompareEqF32:
		case IR::Opcode::CompareMaskEqF32: opcode = IR::ValueOpcode::FPOrdEqual32; break;
		case IR::Opcode::CompareNeF32:
		case IR::Opcode::CompareMaskNeF32: opcode = IR::ValueOpcode::FPOrdNotEqual32; break;
		case IR::Opcode::CompareGtF32:
		case IR::Opcode::CompareMaskGtF32: opcode = IR::ValueOpcode::FPOrdGreaterThan32; break;
		case IR::Opcode::CompareGeF32:
		case IR::Opcode::CompareMaskGeF32: opcode = IR::ValueOpcode::FPOrdGreaterThanEqual32; break;
		case IR::Opcode::CompareLtF32:
		case IR::Opcode::CompareMaskLtF32: opcode = IR::ValueOpcode::FPOrdLessThan32; break;
		case IR::Opcode::CompareLeF32:
		case IR::Opcode::CompareMaskLeF32: opcode = IR::ValueOpcode::FPOrdLessThanEqual32; break;
		case IR::Opcode::CompareUnordEqF32:
		case IR::Opcode::CompareMaskUnordEqF32: opcode = IR::ValueOpcode::FPUnordEqual32; break;
		case IR::Opcode::CompareUnordNeF32:
		case IR::Opcode::CompareMaskUnordNeF32: opcode = IR::ValueOpcode::FPUnordNotEqual32; break;
		case IR::Opcode::CompareUnordGtF32:
		case IR::Opcode::CompareMaskUnordGtF32:
			opcode = IR::ValueOpcode::FPUnordGreaterThan32;
			break;
		case IR::Opcode::CompareUnordGeF32:
		case IR::Opcode::CompareMaskUnordGeF32:
			opcode = IR::ValueOpcode::FPUnordGreaterThanEqual32;
			break;
		case IR::Opcode::CompareUnordLtF32:
		case IR::Opcode::CompareMaskUnordLtF32: opcode = IR::ValueOpcode::FPUnordLessThan32; break;
		case IR::Opcode::CompareUnordLeF32:
		case IR::Opcode::CompareMaskUnordLeF32:
			opcode = IR::ValueOpcode::FPUnordLessThanEqual32;
			break;
		case IR::Opcode::CompareEqF16:
		case IR::Opcode::CompareMaskEqF16:
			opcode = IR::ValueOpcode::FPOrdEqual32;
			half   = true;
			break;
		case IR::Opcode::CompareNeF16:
		case IR::Opcode::CompareMaskNeF16:
			opcode = IR::ValueOpcode::FPOrdNotEqual32;
			half   = true;
			break;
		case IR::Opcode::CompareGtF16:
		case IR::Opcode::CompareMaskGtF16:
			opcode = IR::ValueOpcode::FPOrdGreaterThan32;
			half   = true;
			break;
		case IR::Opcode::CompareGeF16:
		case IR::Opcode::CompareMaskGeF16:
			opcode = IR::ValueOpcode::FPOrdGreaterThanEqual32;
			half   = true;
			break;
		case IR::Opcode::CompareLtF16:
		case IR::Opcode::CompareMaskLtF16:
			opcode = IR::ValueOpcode::FPOrdLessThan32;
			half   = true;
			break;
		case IR::Opcode::CompareLeF16:
		case IR::Opcode::CompareMaskLeF16:
			opcode = IR::ValueOpcode::FPOrdLessThanEqual32;
			half   = true;
			break;
		case IR::Opcode::CompareUnordNeF16:
		case IR::Opcode::CompareMaskUnordNeF16:
			opcode = IR::ValueOpcode::FPUnordNotEqual32;
			half   = true;
			break;
		case IR::Opcode::CompareMaskUnordGeF16:
			opcode = IR::ValueOpcode::FPUnordGreaterThanEqual32;
			half   = true;
			break;
		case IR::Opcode::CompareOrderedF32:
		case IR::Opcode::CompareUnorderedF32: {
			const auto lhs       = IR::F32(ReadOperand(inst.src[0], IR::Type::F32));
			const auto rhs       = IR::F32(ReadOperand(inst.src[1], IR::Type::F32));
			const auto unordered = ir.LogicalOr(IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {lhs})),
			                                    IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {rhs})));
			const auto result =
			    inst.op == IR::Opcode::CompareUnorderedF32 ? unordered : ir.LogicalNot(unordered);
			WriteCompareResult(inst.dst, result);
			return true;
		}
		case IR::Opcode::CompareClassF32: {
			const auto value = ReadOperand(inst.src[0], IR::Type::F32);
			const auto mask  = ReadOperand(inst.src[1], IR::Type::U32);
			WriteCompareResult(inst.dst,
			                   IR::U1(ir.Emit(IR::ValueOpcode::FPCmpClass32, {value, mask})));
			return true;
		}
		default: return false;
	}
	const auto lhs =
	    half ? IR::Value(ReadF16AsF32(inst.src[0])) : ReadOperand(inst.src[0], IR::Type::F32);
	const auto rhs =
	    half ? IR::Value(ReadF16AsF32(inst.src[1])) : ReadOperand(inst.src[1], IR::Type::F32);
	WriteCompareResult(inst.dst, IR::U1(ir.Emit(opcode, {lhs, rhs})));
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
