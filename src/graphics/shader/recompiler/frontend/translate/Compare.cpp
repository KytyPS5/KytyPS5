#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::TranslateIntegerCompare(const Decoder::Instruction& inst) {
	IR::ValueOpcode opcode {};
	IR::Type        type = IR::Type::U32;
	switch (inst.opcode) {
		case Decoder::Opcode::V_CMP_F_I32:
		case Decoder::Opcode::V_CMP_F_U32:
			WriteCompareResult(inst.dst, IR::U1(IR::Value(false)));
			return true;
		case Decoder::Opcode::V_CMP_T_I32:
		case Decoder::Opcode::V_CMP_T_U32:
			WriteCompareResult(inst.dst, IR::U1(IR::Value(true)));
			return true;
		case Decoder::Opcode::S_CMP_EQ_U32:
		case Decoder::Opcode::V_CMP_EQ_U32:
		case Decoder::Opcode::V_CMPX_EQ_U32:
		case Decoder::Opcode::S_CMP_EQ_I32:
		case Decoder::Opcode::V_CMP_EQ_I32:
		case Decoder::Opcode::V_CMPX_EQ_I32: opcode = IR::ValueOpcode::IEqual32; break;
		case Decoder::Opcode::S_CMP_LG_U32:
		case Decoder::Opcode::V_CMP_NE_U32:
		case Decoder::Opcode::V_CMPX_NE_U32:
		case Decoder::Opcode::S_CMP_LG_I32:
		case Decoder::Opcode::V_CMP_NE_I32:
		case Decoder::Opcode::V_CMPX_NE_I32: opcode = IR::ValueOpcode::INotEqual32; break;
		case Decoder::Opcode::S_CMP_GT_U32:
		case Decoder::Opcode::V_CMP_GT_U32:
		case Decoder::Opcode::V_CMPX_GT_U32: opcode = IR::ValueOpcode::UGreaterThan32; break;
		case Decoder::Opcode::S_CMP_GE_U32:
		case Decoder::Opcode::V_CMP_GE_U32:
		case Decoder::Opcode::V_CMPX_GE_U32: opcode = IR::ValueOpcode::UGreaterThanEqual32; break;
		case Decoder::Opcode::S_CMP_LT_U32:
		case Decoder::Opcode::V_CMP_LT_U32:
		case Decoder::Opcode::V_CMPX_LT_U32: opcode = IR::ValueOpcode::ULessThan32; break;
		case Decoder::Opcode::S_CMP_LE_U32:
		case Decoder::Opcode::V_CMP_LE_U32:
		case Decoder::Opcode::V_CMPX_LE_U32: opcode = IR::ValueOpcode::ULessThanEqual32; break;
		case Decoder::Opcode::S_CMP_GT_I32:
		case Decoder::Opcode::V_CMP_GT_I32:
		case Decoder::Opcode::V_CMPX_GT_I32: opcode = IR::ValueOpcode::SGreaterThan32; break;
		case Decoder::Opcode::S_CMP_GE_I32:
		case Decoder::Opcode::V_CMP_GE_I32:
		case Decoder::Opcode::V_CMPX_GE_I32: opcode = IR::ValueOpcode::SGreaterThanEqual32; break;
		case Decoder::Opcode::S_CMP_LT_I32:
		case Decoder::Opcode::V_CMP_LT_I32:
		case Decoder::Opcode::V_CMPX_LT_I32: opcode = IR::ValueOpcode::SLessThan32; break;
		case Decoder::Opcode::S_CMP_LE_I32:
		case Decoder::Opcode::V_CMP_LE_I32:
		case Decoder::Opcode::V_CMPX_LE_I32: opcode = IR::ValueOpcode::SLessThanEqual32; break;
		case Decoder::Opcode::S_CMP_EQ_U64:
		case Decoder::Opcode::V_CMP_EQ_I64:
			opcode = IR::ValueOpcode::IEqual64;
			type   = IR::Type::U64;
			break;
		case Decoder::Opcode::V_CMP_GT_U64:
			opcode = IR::ValueOpcode::UGreaterThan64;
			type   = IR::Type::U64;
			break;
		case Decoder::Opcode::S_CMP_LG_U64:
		case Decoder::Opcode::V_CMP_NE_U64:
		case Decoder::Opcode::V_CMPX_NE_I64:
		case Decoder::Opcode::V_CMPX_NE_U64:
			opcode = IR::ValueOpcode::INotEqual64;
			type   = IR::Type::U64;
			break;
		default: return false;
	}
	const auto lhs = ReadOperand(inst.src0, type);
	const auto rhs = ReadOperand(inst.src1, type);
	WriteCompareResult(inst.dst, IR::U1(ir.Emit(opcode, {lhs, rhs})));
	return true;
}

bool Translator::TranslateInteger16Compare(const Decoder::Instruction& inst) {
	IR::ValueOpcode opcode {};
	bool            signed_value = false;
	switch (inst.opcode) {
		case Decoder::Opcode::V_CMP_EQ_U16:
		case Decoder::Opcode::V_CMP_EQ_I16: opcode = IR::ValueOpcode::IEqual32; break;
		case Decoder::Opcode::V_CMP_NE_U16:
		case Decoder::Opcode::V_CMP_NE_I16: opcode = IR::ValueOpcode::INotEqual32; break;
		case Decoder::Opcode::V_CMP_GT_U16: opcode = IR::ValueOpcode::UGreaterThan32; break;
		case Decoder::Opcode::V_CMP_GE_U16: opcode = IR::ValueOpcode::UGreaterThanEqual32; break;
		case Decoder::Opcode::V_CMP_LT_U16: opcode = IR::ValueOpcode::ULessThan32; break;
		case Decoder::Opcode::V_CMP_LE_U16: opcode = IR::ValueOpcode::ULessThanEqual32; break;
		case Decoder::Opcode::V_CMP_GT_I16:
			opcode       = IR::ValueOpcode::SGreaterThan32;
			signed_value = true;
			break;
		case Decoder::Opcode::V_CMP_GE_I16:
			opcode       = IR::ValueOpcode::SGreaterThanEqual32;
			signed_value = true;
			break;
		case Decoder::Opcode::V_CMP_LT_I16:
			opcode       = IR::ValueOpcode::SLessThan32;
			signed_value = true;
			break;
		case Decoder::Opcode::V_CMP_LE_I16:
			opcode       = IR::ValueOpcode::SLessThanEqual32;
			signed_value = true;
			break;
		default: return false;
	}
	if (inst.opcode == Decoder::Opcode::V_CMP_EQ_I16 ||
	    inst.opcode == Decoder::Opcode::V_CMP_NE_I16) {
		signed_value = true;
	}
	WriteCompareResult(inst.dst, IR::U1(ir.Emit(opcode, {ReadU16AsU32(inst.src0, signed_value),
	                                                     ReadU16AsU32(inst.src1, signed_value)})));
	return true;
}

bool Translator::TranslateFloatCompare(const Decoder::Instruction& inst) {
	IR::ValueOpcode opcode {};
	bool            half = false;
	switch (inst.opcode) {
		case Decoder::Opcode::V_CMP_F_F32:
			WriteCompareResult(inst.dst, IR::U1(IR::Value(false)));
			return true;
		case Decoder::Opcode::V_CMP_TRU_F32:
			WriteCompareResult(inst.dst, IR::U1(IR::Value(true)));
			return true;
		case Decoder::Opcode::V_CMP_EQ_F32:
		case Decoder::Opcode::V_CMPX_EQ_F32: opcode = IR::ValueOpcode::FPOrdEqual32; break;
		case Decoder::Opcode::V_CMP_LG_F32:
		case Decoder::Opcode::V_CMPX_LG_F32: opcode = IR::ValueOpcode::FPOrdNotEqual32; break;
		case Decoder::Opcode::V_CMP_GT_F32:
		case Decoder::Opcode::V_CMPX_GT_F32: opcode = IR::ValueOpcode::FPOrdGreaterThan32; break;
		case Decoder::Opcode::V_CMP_GE_F32:
		case Decoder::Opcode::V_CMPX_GE_F32:
			opcode = IR::ValueOpcode::FPOrdGreaterThanEqual32;
			break;
		case Decoder::Opcode::V_CMP_LT_F32:
		case Decoder::Opcode::V_CMPX_LT_F32: opcode = IR::ValueOpcode::FPOrdLessThan32; break;
		case Decoder::Opcode::V_CMP_LE_F32:
		case Decoder::Opcode::V_CMPX_LE_F32: opcode = IR::ValueOpcode::FPOrdLessThanEqual32; break;
		case Decoder::Opcode::V_CMP_NLG_F32:
		case Decoder::Opcode::V_CMPX_NLG_F32: opcode = IR::ValueOpcode::FPUnordEqual32; break;
		case Decoder::Opcode::V_CMP_NEQ_F32:
		case Decoder::Opcode::V_CMPX_NEQ_F32: opcode = IR::ValueOpcode::FPUnordNotEqual32; break;
		case Decoder::Opcode::V_CMP_NLE_F32:
		case Decoder::Opcode::V_CMPX_NLE_F32: opcode = IR::ValueOpcode::FPUnordGreaterThan32; break;
		case Decoder::Opcode::V_CMP_NLT_F32:
		case Decoder::Opcode::V_CMPX_NLT_F32:
			opcode = IR::ValueOpcode::FPUnordGreaterThanEqual32;
			break;
		case Decoder::Opcode::V_CMP_NGE_F32:
		case Decoder::Opcode::V_CMPX_NGE_F32: opcode = IR::ValueOpcode::FPUnordLessThan32; break;
		case Decoder::Opcode::V_CMP_NGT_F32:
		case Decoder::Opcode::V_CMPX_NGT_F32:
			opcode = IR::ValueOpcode::FPUnordLessThanEqual32;
			break;
		case Decoder::Opcode::V_CMP_EQ_F16:
		case Decoder::Opcode::V_CMPX_EQ_F16:
			opcode = IR::ValueOpcode::FPOrdEqual32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMP_LG_F16:
			opcode = IR::ValueOpcode::FPOrdNotEqual32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMP_GT_F16:
		case Decoder::Opcode::V_CMPX_GT_F16:
			opcode = IR::ValueOpcode::FPOrdGreaterThan32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMP_GE_F16:
		case Decoder::Opcode::V_CMPX_GE_F16:
			opcode = IR::ValueOpcode::FPOrdGreaterThanEqual32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMP_LT_F16:
		case Decoder::Opcode::V_CMPX_LT_F16:
			opcode = IR::ValueOpcode::FPOrdLessThan32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMP_LE_F16:
		case Decoder::Opcode::V_CMPX_LE_F16:
			opcode = IR::ValueOpcode::FPOrdLessThanEqual32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMP_NEQ_F16:
		case Decoder::Opcode::V_CMPX_NEQ_F16:
			opcode = IR::ValueOpcode::FPUnordNotEqual32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMPX_NLT_F16:
			opcode = IR::ValueOpcode::FPUnordGreaterThanEqual32;
			half   = true;
			break;
		case Decoder::Opcode::V_CMP_O_F32:
		case Decoder::Opcode::V_CMP_U_F32: {
			const auto lhs       = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
			const auto rhs       = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
			const auto unordered = ir.LogicalOr(IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {lhs})),
			                                    IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {rhs})));
			const auto result =
			    inst.opcode == Decoder::Opcode::V_CMP_U_F32 ? unordered : ir.LogicalNot(unordered);
			WriteCompareResult(inst.dst, result);
			return true;
		}
		case Decoder::Opcode::V_CMP_CLASS_F32: {
			const auto value = ReadOperand(inst.src0, IR::Type::F32);
			const auto mask  = ReadOperand(inst.src1, IR::Type::U32);
			WriteCompareResult(inst.dst,
			                   IR::U1(ir.Emit(IR::ValueOpcode::FPCmpClass32, {value, mask})));
			return true;
		}
		default: return false;
	}
	const auto lhs =
	    half ? IR::Value(ReadF16AsF32(inst.src0)) : ReadOperand(inst.src0, IR::Type::F32);
	const auto rhs =
	    half ? IR::Value(ReadF16AsF32(inst.src1)) : ReadOperand(inst.src1, IR::Type::F32);
	WriteCompareResult(inst.dst, IR::U1(ir.Emit(opcode, {lhs, rhs})));
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
