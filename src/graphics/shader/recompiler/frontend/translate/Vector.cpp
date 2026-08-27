#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::EmitVector(const Decoder::Instruction& inst, std::string* error) {
	using O = Decoder::Opcode;
	switch (inst.opcode) {
		case O::V_NOP: return true;
		case O::V_ADD_I32: ADD_U32(inst, true, false); return true;
		case O::V_ADDC_U32: ADD_U32(inst, true, true); return true;
		case O::V_SUB_I32: SUB_U32(inst, true, false); return true;
		case O::V_SUBREV_I32: SUB_U32(inst, true, true); return true;
		case O::V_SUBREV_CO_CI_U32: SUBB_U32(inst, true); return true;

		case O::V_MOV_B32: MOV_B32(inst, true); return true;
		case O::V_MOVRELS_B32: return V_MOVRELS_B32(inst, error);
		case O::V_MOVRELD_B32: return V_MOVRELD_B32(inst, error);
		case O::V_READFIRSTLANE_B32: V_READFIRSTLANE_B32(inst); return true;
		case O::V_READLANE_B32: V_READLANE_B32(inst); return true;
		case O::V_WRITELANE_B32: V_WRITELANE_B32(inst); return true;
		case O::V_PERMLANE16_B32: V_PERMLANE16_B32(inst, false); return true;
		case O::V_PERMLANEX16_B32: V_PERMLANE16_B32(inst, true); return true;

		case O::V_CMP_F_I32:
		case O::V_CMP_F_U32: EmitCompareConstant(inst, false, false, false); return true;
		case O::V_CMP_T_I32:
		case O::V_CMP_T_U32: EmitCompareConstant(inst, true, false, false); return true;
		case O::V_CMP_EQ_U32:
		case O::V_CMP_EQ_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::IEqual32, IR::Type::U32, false, false);
			return true;
		case O::V_CMPX_EQ_U32:
		case O::V_CMPX_EQ_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::IEqual32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_NE_U32:
		case O::V_CMP_NE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::INotEqual32, IR::Type::U32, false, false);
			return true;
		case O::V_CMPX_NE_U32:
		case O::V_CMPX_NE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::INotEqual32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_GT_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::UGreaterThan32, IR::Type::U32, false, false);
			return true;
		case O::V_CMPX_GT_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::UGreaterThan32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_GE_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::UGreaterThanEqual32, IR::Type::U32, false,
			                   false);
			return true;
		case O::V_CMPX_GE_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::UGreaterThanEqual32, IR::Type::U32, false,
			                   true);
			return true;
		case O::V_CMP_LT_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::ULessThan32, IR::Type::U32, false, false);
			return true;
		case O::V_CMPX_LT_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::ULessThan32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_LE_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::ULessThanEqual32, IR::Type::U32, false,
			                   false);
			return true;
		case O::V_CMPX_LE_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::ULessThanEqual32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_GT_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SGreaterThan32, IR::Type::U32, false, false);
			return true;
		case O::V_CMPX_GT_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SGreaterThan32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_GE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SGreaterThanEqual32, IR::Type::U32, false,
			                   false);
			return true;
		case O::V_CMPX_GE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SGreaterThanEqual32, IR::Type::U32, false,
			                   true);
			return true;
		case O::V_CMP_LT_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SLessThan32, IR::Type::U32, false, false);
			return true;
		case O::V_CMPX_LT_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SLessThan32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_LE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SLessThanEqual32, IR::Type::U32, false,
			                   false);
			return true;
		case O::V_CMPX_LE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SLessThanEqual32, IR::Type::U32, false, true);
			return true;
		case O::V_CMP_EQ_I64:
			EmitIntegerCompare(inst, IR::ValueOpcode::IEqual64, IR::Type::U64, false, false);
			return true;
		case O::V_CMP_GT_U64:
			EmitIntegerCompare(inst, IR::ValueOpcode::UGreaterThan64, IR::Type::U64, false, false);
			return true;
		case O::V_CMP_NE_U64:
			EmitIntegerCompare(inst, IR::ValueOpcode::INotEqual64, IR::Type::U64, false, false);
			return true;
		case O::V_CMPX_NE_I64:
		case O::V_CMPX_NE_U64:
			EmitIntegerCompare(inst, IR::ValueOpcode::INotEqual64, IR::Type::U64, false, true);
			return true;

		case O::V_CMP_EQ_U16:
			EmitInteger16Compare(inst, IR::ValueOpcode::IEqual32, false, false);
			return true;
		case O::V_CMP_EQ_I16:
			EmitInteger16Compare(inst, IR::ValueOpcode::IEqual32, true, false);
			return true;
		case O::V_CMP_NE_U16:
			EmitInteger16Compare(inst, IR::ValueOpcode::INotEqual32, false, false);
			return true;
		case O::V_CMP_NE_I16:
			EmitInteger16Compare(inst, IR::ValueOpcode::INotEqual32, true, false);
			return true;
		case O::V_CMP_GT_U16:
			EmitInteger16Compare(inst, IR::ValueOpcode::UGreaterThan32, false, false);
			return true;
		case O::V_CMPX_GT_U16:
			EmitInteger16Compare(inst, IR::ValueOpcode::UGreaterThan32, false, true);
			return true;
		case O::V_CMP_GE_U16:
			EmitInteger16Compare(inst, IR::ValueOpcode::UGreaterThanEqual32, false, false);
			return true;
		case O::V_CMP_LT_U16:
			EmitInteger16Compare(inst, IR::ValueOpcode::ULessThan32, false, false);
			return true;
		case O::V_CMP_LE_U16:
			EmitInteger16Compare(inst, IR::ValueOpcode::ULessThanEqual32, false, false);
			return true;
		case O::V_CMP_GT_I16:
			EmitInteger16Compare(inst, IR::ValueOpcode::SGreaterThan32, true, false);
			return true;
		case O::V_CMP_GE_I16:
			EmitInteger16Compare(inst, IR::ValueOpcode::SGreaterThanEqual32, true, false);
			return true;
		case O::V_CMP_LT_I16:
			EmitInteger16Compare(inst, IR::ValueOpcode::SLessThan32, true, false);
			return true;
		case O::V_CMP_LE_I16:
			EmitInteger16Compare(inst, IR::ValueOpcode::SLessThanEqual32, true, false);
			return true;

		case O::V_CMP_F_F32: EmitCompareConstant(inst, false, false, false); return true;
		case O::V_CMP_TRU_F32: EmitCompareConstant(inst, true, false, false); return true;
		case O::V_CMP_EQ_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdEqual32, false, false);
			return true;
		case O::V_CMPX_EQ_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdEqual32, false, true);
			return true;
		case O::V_CMP_LG_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdNotEqual32, false, false);
			return true;
		case O::V_CMPX_LG_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdNotEqual32, false, true);
			return true;
		case O::V_CMP_GT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThan32, false, false);
			return true;
		case O::V_CMPX_GT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThan32, false, true);
			return true;
		case O::V_CMP_GE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThanEqual32, false, false);
			return true;
		case O::V_CMPX_GE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThanEqual32, false, true);
			return true;
		case O::V_CMP_LT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThan32, false, false);
			return true;
		case O::V_CMPX_LT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThan32, false, true);
			return true;
		case O::V_CMP_LE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThanEqual32, false, false);
			return true;
		case O::V_CMPX_LE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThanEqual32, false, true);
			return true;
		case O::V_CMP_NLG_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordEqual32, false, false);
			return true;
		case O::V_CMPX_NLG_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordEqual32, false, true);
			return true;
		case O::V_CMP_NEQ_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordNotEqual32, false, false);
			return true;
		case O::V_CMPX_NEQ_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordNotEqual32, false, true);
			return true;
		case O::V_CMP_NLE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordGreaterThan32, false, false);
			return true;
		case O::V_CMPX_NLE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordGreaterThan32, false, true);
			return true;
		case O::V_CMP_NLT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordGreaterThanEqual32, false, false);
			return true;
		case O::V_CMPX_NLT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordGreaterThanEqual32, false, true);
			return true;
		case O::V_CMP_NGE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordLessThan32, false, false);
			return true;
		case O::V_CMPX_NGE_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordLessThan32, false, true);
			return true;
		case O::V_CMP_NGT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordLessThanEqual32, false, false);
			return true;
		case O::V_CMPX_NGT_F32:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordLessThanEqual32, false, true);
			return true;
		case O::V_CMP_EQ_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdEqual32, true, false);
			return true;
		case O::V_CMPX_EQ_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdEqual32, true, true);
			return true;
		case O::V_CMP_LG_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdNotEqual32, true, false);
			return true;
		case O::V_CMP_GT_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThan32, true, false);
			return true;
		case O::V_CMPX_GT_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThan32, true, true);
			return true;
		case O::V_CMP_GE_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThanEqual32, true, false);
			return true;
		case O::V_CMPX_GE_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdGreaterThanEqual32, true, true);
			return true;
		case O::V_CMP_LT_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThan32, true, false);
			return true;
		case O::V_CMPX_LT_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThan32, true, true);
			return true;
		case O::V_CMP_LE_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThanEqual32, true, false);
			return true;
		case O::V_CMPX_LE_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPOrdLessThanEqual32, true, true);
			return true;
		case O::V_CMPX_NGT_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordLessThanEqual32, true, true);
			return true;
		case O::V_CMP_NEQ_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordNotEqual32, true, false);
			return true;
		case O::V_CMPX_NEQ_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordNotEqual32, true, true);
			return true;
		case O::V_CMPX_NLT_F16:
			EmitFloatCompare(inst, IR::ValueOpcode::FPUnordGreaterThanEqual32, true, true);
			return true;
		case O::V_CMP_O_F32: EmitFloatOrderedCompare(inst, true); return true;
		case O::V_CMP_U_F32: EmitFloatOrderedCompare(inst, false); return true;
		case O::V_CMP_CLASS_F32: V_CMP_CLASS_F32(inst); return true;

		case O::V_CVT_F32_UBYTE0: V_CVT_F32_UBYTE(inst, 0); return true;
		case O::V_CVT_F32_UBYTE1: V_CVT_F32_UBYTE(inst, 1); return true;
		case O::V_CVT_F32_UBYTE2: V_CVT_F32_UBYTE(inst, 2); return true;
		case O::V_CVT_F32_UBYTE3: V_CVT_F32_UBYTE(inst, 3); return true;
		case O::V_CVT_F32_U32: V_CVT_F32_U32(inst); return true;
		case O::V_CVT_F32_I32: V_CVT_F32_I32(inst); return true;
		case O::V_CVT_U32_F32: V_CVT_U32_F32(inst); return true;
		case O::V_CVT_I32_F32: V_CVT_I32_F32(inst); return true;
		case O::V_CVT_F16_F32: V_CVT_F16_F32(inst); return true;
		case O::V_CVT_F32_F16: V_CVT_F32_F16(inst); return true;
		case O::V_CVT_F16_U16: V_CVT_F16_16(inst, false); return true;
		case O::V_CVT_F16_I16: V_CVT_F16_16(inst, true); return true;
		case O::V_CVT_U16_F16: V_CVT_16_F16(inst, false); return true;
		case O::V_CVT_I16_F16: V_CVT_16_F16(inst, true); return true;
		case O::V_CVT_RPI_I32_F32: V_CVT_RPI_I32_F32(inst); return true;
		case O::V_CVT_FLR_I32_F32: V_CVT_FLR_I32_F32(inst); return true;
		case O::V_FREXP_EXP_I32_F32: V_FREXP_EXP_I32_F32(inst); return true;
		case O::V_CVT_OFF_F32_I4: V_CVT_OFF_F32_I4(inst); return true;
		case O::V_CVT_PKRTZ_F16_F32: V_CVT_PKRTZ_F16_F32(inst); return true;
		case O::V_CVT_PKNORM_I16_F32:
			V_CVT_PKNORM_F32(inst, IR::ValueOpcode::PackSnorm2x16);
			return true;
		case O::V_CVT_PKNORM_U16_F32:
			V_CVT_PKNORM_F32(inst, IR::ValueOpcode::PackUnorm2x16);
			return true;
		case O::V_CVT_PK_U8_F32: V_CVT_PK_U8_F32(inst); return true;
		case O::V_PACK_B32_F16: V_PACK_B32_F16(inst); return true;
		case O::V_CVT_PK_U16_U32:
		case O::V_CVT_PK_I16_I32: return PackB16(inst, false, false);

		case O::V_LSHLREV_B16:
			return Integer16Shift(inst, IR::ValueOpcode::ShiftLeftLogical32, false);
		case O::V_LSHRREV_B16:
			return Integer16Shift(inst, IR::ValueOpcode::ShiftRightLogical32, false);
		case O::V_ASHRREV_I16:
			return Integer16Shift(inst, IR::ValueOpcode::ShiftRightArithmetic32, true);
		case O::V_ADD_NC_U16:
		case O::V_ADD_NC_I16: return Integer16Binary(inst, IR::ValueOpcode::IAdd32, false);
		case O::V_SUB_NC_U16:
		case O::V_SUB_NC_I16: return Integer16Binary(inst, IR::ValueOpcode::ISub32, false);
		case O::V_MED3_I16: return V_MED3_I16(inst);
		case O::V_MIN_I16: return Integer16Binary(inst, IR::ValueOpcode::SMin32, true);
		case O::V_MAX_I16: return Integer16Binary(inst, IR::ValueOpcode::SMax32, true);
		case O::V_MIN_U16: return Integer16Binary(inst, IR::ValueOpcode::UMin32, false);
		case O::V_MAX_U16: return Integer16Binary(inst, IR::ValueOpcode::UMax32, false);

		case O::V_PK_LSHLREV_B16:
			return PackedInteger16Shift(inst, IR::ValueOpcode::ShiftLeftLogical32, false);
		case O::V_PK_LSHRREV_B16:
			return PackedInteger16Shift(inst, IR::ValueOpcode::ShiftRightLogical32, false);
		case O::V_PK_ASHRREV_I16:
			return PackedInteger16Shift(inst, IR::ValueOpcode::ShiftRightArithmetic32, true);
		case O::V_PK_MAD_I16: return PackedInteger16Mad(inst, true);
		case O::V_PK_MAD_U16: return PackedInteger16Mad(inst, false);
		case O::V_PK_MUL_LO_U16: return PackedInteger16Binary(inst, IR::ValueOpcode::IMul32);
		case O::V_PK_ADD_I16:
		case O::V_PK_ADD_U16: return PackedInteger16Binary(inst, IR::ValueOpcode::IAdd32);
		case O::V_PK_SUB_I16:
		case O::V_PK_SUB_U16: return PackedInteger16Binary(inst, IR::ValueOpcode::ISub32);
		case O::V_PK_MAX_I16: return PackedInteger16MinMax(inst, IR::ValueOpcode::SMax32, true);
		case O::V_PK_MIN_I16: return PackedInteger16MinMax(inst, IR::ValueOpcode::SMin32, true);
		case O::V_PK_MAX_U16: return PackedInteger16MinMax(inst, IR::ValueOpcode::UMax32, false);
		case O::V_PK_MIN_U16: return PackedInteger16MinMax(inst, IR::ValueOpcode::UMin32, false);

		case O::V_PK_ADD_F16: return PackedFloat16(inst, IR::ValueOpcode::FPAdd32, false, false);
		case O::V_PK_MUL_F16: return PackedFloat16(inst, IR::ValueOpcode::FPMul32, false, false);
		case O::V_PK_MIN_F16: return PackedFloat16(inst, IR::ValueOpcode::FPMin32, false, true);
		case O::V_PK_MAX_F16: return PackedFloat16(inst, IR::ValueOpcode::FPMax32, false, true);
		case O::V_PK_FMA_F16: return PackedFloat16(inst, IR::ValueOpcode::FPFma32, false, false);
		case O::V_PK_FMAC_F16: return PackedFloat16(inst, IR::ValueOpcode::FPFma32, true, false);

		case O::V_ADD_F16: return Float16Binary(inst, IR::ValueOpcode::FPAdd32, false);
		case O::V_SUB_F16: return Float16Binary(inst, IR::ValueOpcode::FPSub32, false);
		case O::V_SUBREV_F16: return Float16Binary(inst, IR::ValueOpcode::FPSub32, true);
		case O::V_MUL_F16: return Float16Binary(inst, IR::ValueOpcode::FPMul32, false);
		case O::V_MIN_F16: return Float16Binary(inst, IR::ValueOpcode::FPMin32, false);
		case O::V_MAX_F16: return Float16Binary(inst, IR::ValueOpcode::FPMax32, false);
		case O::V_FMAC_F16: return Float16Ternary(inst, IR::ValueOpcode::FPFma32, true, false);
		case O::V_FMAMK_F16:
		case O::V_FMAAK_F16:
		case O::V_FMA_F16: return Float16Ternary(inst, IR::ValueOpcode::FPFma32, false, false);
		case O::V_MAD_MIXLO_F16:
		case O::V_MAD_MIXHI_F16: return Float16Ternary(inst, IR::ValueOpcode::FPFma32, false, true);
		case O::V_RCP_F16: return Float16Unary(inst, IR::ValueOpcode::FPRecip32, false);
		case O::V_SQRT_F16: return Float16Unary(inst, IR::ValueOpcode::FPSqrt, true);
		case O::V_RSQ_F16: return Float16Unary(inst, IR::ValueOpcode::FPRecipSqrt32, true);
		case O::V_LOG_F16: return Float16Unary(inst, IR::ValueOpcode::FPLog2, true);
		case O::V_EXP_F16: return Float16Unary(inst, IR::ValueOpcode::FPExp2, false);
		case O::V_FLOOR_F16: return Float16Unary(inst, IR::ValueOpcode::FPFloor32, false);
		case O::V_CEIL_F16: return Float16Unary(inst, IR::ValueOpcode::FPCeil32, false);
		case O::V_TRUNC_F16: return Float16Unary(inst, IR::ValueOpcode::FPTrunc32, false);
		case O::V_RNDNE_F16: return Float16Unary(inst, IR::ValueOpcode::FPRoundEven32, false);
		case O::V_MIN3_F16: return Float16Ternary(inst, IR::ValueOpcode::FPMinTri32, false, false);
		case O::V_MAX3_F16: return Float16Ternary(inst, IR::ValueOpcode::FPMaxTri32, false, false);
		case O::V_MED3_F16: return Float16Ternary(inst, IR::ValueOpcode::FPMedTri32, false, false);

		case O::V_FREXP_MANT_F32: return V_FREXP_MANT_F32(inst);
		case O::V_RCP_F32: return FloatUnary(inst, IR::ValueOpcode::FPRecip32);
		case O::V_RCP_IFLAG_F32: return FloatUnary(inst, IR::ValueOpcode::FPRecipIFlag32);
		case O::V_FRACT_F32: return FloatUnary(inst, IR::ValueOpcode::FPFract32);
		case O::V_TRUNC_F32: return FloatUnary(inst, IR::ValueOpcode::FPTrunc32);
		case O::V_CEIL_F32: return FloatUnary(inst, IR::ValueOpcode::FPCeil32);
		case O::V_RNDNE_F32: return FloatUnary(inst, IR::ValueOpcode::FPRoundEven32);
		case O::V_FLOOR_F32: return FloatUnary(inst, IR::ValueOpcode::FPFloor32);
		case O::V_EXP_F32: return FloatUnary(inst, IR::ValueOpcode::FPExp2);
		case O::V_LOG_F32: return FloatUnary(inst, IR::ValueOpcode::FPLog2);
		case O::V_RSQ_F32: return FloatUnary(inst, IR::ValueOpcode::FPRecipSqrt32);
		case O::V_SQRT_F32: return FloatUnary(inst, IR::ValueOpcode::FPSqrt);
		case O::V_SIN_F32: return FloatUnary(inst, IR::ValueOpcode::FPSin);
		case O::V_COS_F32: return FloatUnary(inst, IR::ValueOpcode::FPCos);
		case O::V_ADD_F32: return FloatBinary(inst, IR::ValueOpcode::FPAdd32, false);
		case O::V_SUB_F32: return FloatBinary(inst, IR::ValueOpcode::FPSub32, false);
		case O::V_SUBREV_F32: return FloatBinary(inst, IR::ValueOpcode::FPSub32, true);
		case O::V_MUL_F32: return FloatBinary(inst, IR::ValueOpcode::FPMul32, false);
		case O::V_MIN_F32: return FloatBinary(inst, IR::ValueOpcode::FPMin32, false);
		case O::V_MAX_F32: return FloatBinary(inst, IR::ValueOpcode::FPMax32, false);
		case O::V_LDEXP_F32: return FloatBinary(inst, IR::ValueOpcode::FPLdexp, false);
		case O::V_MAC_F32: return FloatTernary(inst, IR::ValueOpcode::FPFma32, true, true);
		case O::V_MADMK_F32:
		case O::V_MADAK_F32:
		case O::V_MAD_F32:
		case O::V_FMA_F32: return FloatTernary(inst, IR::ValueOpcode::FPFma32, false, true);
		case O::V_MIN3_F32: return FloatTernary(inst, IR::ValueOpcode::FPMinTri32, false, false);
		case O::V_MAX3_F32: return FloatTernary(inst, IR::ValueOpcode::FPMaxTri32, false, false);
		case O::V_MED3_F32: return FloatTernary(inst, IR::ValueOpcode::FPMedTri32, false, false);
		case O::V_DOT2C_F32_F16: return V_DOT2C_F32_F16(inst);
		case O::V_CUBEID_F32: return V_CUBEID_F32(inst);
		case O::V_CUBESC_F32: return V_CUBESC_F32(inst);
		case O::V_CUBETC_F32: return V_CUBETC_F32(inst);
		case O::V_CUBEMA_F32: return V_CUBEMA_F32(inst);

		case O::V_MUL_LO_U32:
		case O::V_MUL_LO_I32:
			return SimpleInteger(inst, IR::ValueOpcode::IMul32, IR::Type::U32, false, false, false);
		case O::V_MUL_HI_U32:
			return SimpleInteger(inst, IR::ValueOpcode::UMulHi, IR::Type::U32, false, false, false);
		case O::V_MUL_HI_I32:
			return SimpleInteger(inst, IR::ValueOpcode::SMulHi, IR::Type::U32, false, false, false);
		case O::V_ADD_NC_U32:
			return SimpleInteger(inst, IR::ValueOpcode::IAdd32, IR::Type::U32, false, false, false);
		case O::V_SUB_NC_U32:
			return SimpleInteger(inst, IR::ValueOpcode::ISub32, IR::Type::U32, false, false, false);
		case O::V_SUBREV_NC_U32:
			return SimpleInteger(inst, IR::ValueOpcode::ISub32, IR::Type::U32, true, false, false);
		case O::V_MIN_I32:
			return SimpleInteger(inst, IR::ValueOpcode::SMin32, IR::Type::U32, false, false, false);
		case O::V_MAX_I32:
			return SimpleInteger(inst, IR::ValueOpcode::SMax32, IR::Type::U32, false, false, false);
		case O::V_MIN_U32:
			return SimpleInteger(inst, IR::ValueOpcode::UMin32, IR::Type::U32, false, false, false);
		case O::V_MAX_U32:
			return SimpleInteger(inst, IR::ValueOpcode::UMax32, IR::Type::U32, false, false, false);
		case O::V_MIN3_I32:
			return SimpleInteger(inst, IR::ValueOpcode::SMinTri32, IR::Type::U32, false, false,
			                     false);
		case O::V_MAX3_I32:
			return SimpleInteger(inst, IR::ValueOpcode::SMaxTri32, IR::Type::U32, false, false,
			                     false);
		case O::V_MED3_I32:
			return SimpleInteger(inst, IR::ValueOpcode::SMedTri32, IR::Type::U32, false, false,
			                     false);
		case O::V_MIN3_U32:
			return SimpleInteger(inst, IR::ValueOpcode::UMinTri32, IR::Type::U32, false, false,
			                     false);
		case O::V_MAX3_U32:
			return SimpleInteger(inst, IR::ValueOpcode::UMaxTri32, IR::Type::U32, false, false,
			                     false);
		case O::V_MED3_U32:
			return SimpleInteger(inst, IR::ValueOpcode::UMedTri32, IR::Type::U32, false, false,
			                     false);
		case O::V_AND_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseAnd32, IR::Type::U32, false, false,
			                     false);
		case O::V_OR_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseOr32, IR::Type::U32, false, false,
			                     false);
		case O::V_XOR_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseXor32, IR::Type::U32, false, false,
			                     false);
		case O::V_NOT_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseNot32, IR::Type::U32, false, false,
			                     false);
		case O::V_BFREV_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitReverse32, IR::Type::U32, false, false,
			                     false);
		case O::V_FFBL_B32:
			return SimpleInteger(inst, IR::ValueOpcode::FindILsb32, IR::Type::U32, false, false,
			                     false);
		case O::V_LSHL_B32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftLeftLogical32, IR::Type::U32, false,
			                     true, false);
		case O::V_LSHLREV_B32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftLeftLogical32, IR::Type::U32, true,
			                     true, false);
		case O::V_LSHR_B32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightLogical32, IR::Type::U32, false,
			                     true, false);
		case O::V_LSHRREV_B32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightLogical32, IR::Type::U32, true,
			                     true, false);
		case O::V_ASHR_I32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightArithmetic32, IR::Type::U32,
			                     false, true, false);
		case O::V_ASHRREV_I32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightArithmetic32, IR::Type::U32, true,
			                     true, false);
		case O::V_LSHLREV_B64:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftLeftLogical64, IR::Type::U64, true,
			                     false, false);
		case O::V_LSHRREV_B64:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightLogical64, IR::Type::U64, true,
			                     false, false);

		case O::V_XNOR_B32:
			return ComposedIntegerBinary(inst, IR::ValueOpcode::BitwiseXor32, false, true, false);
		case O::V_AND_OR_B32: return V_AND_OR_B32(inst);
		case O::V_OR3_B32: return V_OR3_B32(inst);
		case O::V_XOR3_B32: return V_XOR3_B32(inst);
		case O::V_FFBH_U32: return V_FFBH_U32(inst);

		case O::V_MAD_I32_I24: return Integer24(inst, true, true);
		case O::V_MAD_U32_U24: return Integer24(inst, false, true);
		case O::V_MUL_I32_I24: return Integer24(inst, true, false);
		case O::V_MUL_U32_U24: return Integer24(inst, false, false);
		case O::V_MAD_U64_U32: return V_MAD_U64_U32(inst);
		case O::V_SAD_U32: return V_SAD_U32(inst);
		case O::V_ADD3_U32: return V_ADD3_U32(inst);
		case O::V_BCNT_U32_B32: return V_BCNT_U32_B32(inst);
		case O::V_MBCNT_LO_U32_B32: return V_MBCNT_U32_B32(inst, true);
		case O::V_MBCNT_HI_U32_B32: return V_MBCNT_U32_B32(inst, false);
		case O::V_BFM_B32: return BFM_B32(inst);
		case O::V_BFE_U32: return V_BFE_U32(inst, false);
		case O::V_BFE_I32: return V_BFE_U32(inst, true);
		case O::V_BFI_B32: return V_BFI_B32(inst);
		case O::V_ALIGNBIT_B32: return V_ALIGNBIT_B32(inst);
		case O::V_ALIGNBYTE_B32: return V_ALIGNBYTE_B32(inst);
		case O::V_LSHL_ADD_U32: return V_LSHL_ADD_U32(inst);
		case O::V_ADD_LSHL_U32: return V_ADD_LSHL_U32(inst);
		case O::V_XAD_U32: return V_XAD_U32(inst);
		case O::V_LSHL_OR_B32: return V_LSHL_OR_B32(inst);
		case O::V_CNDMASK_B32: return V_CNDMASK_B32(inst);
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
