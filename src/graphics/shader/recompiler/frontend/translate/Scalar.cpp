#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::EmitScalar(const Decoder::Instruction& inst, std::string* error) {
	using O = Decoder::Opcode;
	switch (inst.opcode) {
		case O::S_MOV_B32:
		case O::S_MOVK_I32: MOV_B32(inst, false); return true;
		case O::S_MOV_B64: S_MOV_B64(inst); return true;
		case O::S_WQM_B64: S_WQM_B64(inst); return true;
		case O::S_GETPC_B64: return S_GETPC_B64(inst, error);
		case O::S_SETPC_B64: return true;
		case O::S_CSELECT_B32: S_CSELECT_B32(inst); return true;
		case O::S_CSELECT_B64: S_CSELECT_B64(inst); return true;
		case O::S_SETREG_B32: EmitControlNop(); return true;
		case O::S_WAITCNT: EmitWaitcnt(); return true;

		case O::S_AND_SAVEEXEC_B32:
			S_SAVEEXEC(inst, IR::ValueOpcode::LogicalAnd, false, false, false);
			return true;
		case O::S_ANDN1_SAVEEXEC_B32:
			S_SAVEEXEC(inst, IR::ValueOpcode::LogicalAnd, false, true, false);
			return true;
		case O::S_AND_SAVEEXEC_B64:
			S_SAVEEXEC(inst, IR::ValueOpcode::LogicalAnd, false, false, true);
			return true;
		case O::S_ANDN1_SAVEEXEC_B64:
			S_SAVEEXEC(inst, IR::ValueOpcode::LogicalAnd, false, true, true);
			return true;
		case O::S_ORN2_SAVEEXEC_B64:
			S_SAVEEXEC(inst, IR::ValueOpcode::LogicalOr, true, false, true);
			return true;
		case O::S_ADD_U32: ADD_U32(inst, false, false); return true;
		case O::S_ADDC_U32: ADD_U32(inst, false, true); return true;
		case O::S_SUB_U32: SUB_U32(inst, false, false); return true;
		case O::S_SUBB_U32: SUBB_U32(inst, false); return true;
		case O::S_ADD_I32: S_ADD_SUB_I32(inst, false); return true;
		case O::S_SUB_I32: S_ADD_SUB_I32(inst, true); return true;
		case O::S_LSHL1_ADD_U32: S_LSHL_ADD_U32(inst, 1u); return true;
		case O::S_LSHL2_ADD_U32: S_LSHL_ADD_U32(inst, 2u); return true;
		case O::S_LSHL3_ADD_U32: S_LSHL_ADD_U32(inst, 3u); return true;
		case O::S_LSHL4_ADD_U32: S_LSHL_ADD_U32(inst, 4u); return true;
		case O::S_MIN_I32:
			ScalarMinMax32(inst, IR::ValueOpcode::SMin32, IR::ValueOpcode::SLessThan32);
			return true;
		case O::S_MAX_I32:
			ScalarMinMax32(inst, IR::ValueOpcode::SMax32, IR::ValueOpcode::SGreaterThan32);
			return true;
		case O::S_MIN_U32:
			ScalarMinMax32(inst, IR::ValueOpcode::UMin32, IR::ValueOpcode::ULessThan32);
			return true;
		case O::S_MAX_U32:
			ScalarMinMax32(inst, IR::ValueOpcode::UMax32, IR::ValueOpcode::UGreaterThan32);
			return true;

		case O::S_CMP_EQ_U32:
		case O::S_CMP_EQ_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::IEqual32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_LG_U32:
		case O::S_CMP_LG_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::INotEqual32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_GT_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::UGreaterThan32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_GE_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::UGreaterThanEqual32, IR::Type::U32, true,
			                   false);
			return true;
		case O::S_CMP_LT_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::ULessThan32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_LE_U32:
			EmitIntegerCompare(inst, IR::ValueOpcode::ULessThanEqual32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_GT_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SGreaterThan32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_GE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SGreaterThanEqual32, IR::Type::U32, true,
			                   false);
			return true;
		case O::S_CMP_LT_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SLessThan32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_LE_I32:
			EmitIntegerCompare(inst, IR::ValueOpcode::SLessThanEqual32, IR::Type::U32, true, false);
			return true;
		case O::S_CMP_EQ_U64:
			EmitIntegerCompare(inst, IR::ValueOpcode::IEqual64, IR::Type::U64, true, false);
			return true;
		case O::S_CMP_LG_U64:
			EmitIntegerCompare(inst, IR::ValueOpcode::INotEqual64, IR::Type::U64, true, false);
			return true;

		case O::S_AND_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalAnd, IR::ValueOpcode::BitwiseAnd32,
			                  false, false, false);
		case O::S_ANDN2_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalAnd, IR::ValueOpcode::BitwiseAnd32,
			                  true, false, false);
		case O::S_OR_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalOr, IR::ValueOpcode::BitwiseOr32, false,
			                  false, false);
		case O::S_ORN2_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalOr, IR::ValueOpcode::BitwiseOr32, true,
			                  false, false);
		case O::S_XOR_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalXor, IR::ValueOpcode::BitwiseXor32,
			                  false, false, false);
		case O::S_NAND_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalAnd, IR::ValueOpcode::BitwiseAnd32,
			                  false, true, false);
		case O::S_NOR_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalOr, IR::ValueOpcode::BitwiseOr32, false,
			                  true, false);
		case O::S_XNOR_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalXor, IR::ValueOpcode::BitwiseXor32,
			                  false, true, false);
		case O::S_NOT_B64:
			return S_U64_MASK(inst, IR::ValueOpcode::LogicalAnd, IR::ValueOpcode::BitwiseAnd32,
			                  false, false, true);

		case O::S_ABS_I32:
			return SimpleInteger(inst, IR::ValueOpcode::IAbs32, IR::Type::U32, false, false, true);
		case O::S_MUL_I32:
		case O::S_MULK_I32:
			return SimpleInteger(inst, IR::ValueOpcode::IMul32, IR::Type::U32, false, false, false);
		case O::S_MUL_HI_U32:
			return SimpleInteger(inst, IR::ValueOpcode::UMulHi, IR::Type::U32, false, false, false);
		case O::S_AND_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseAnd32, IR::Type::U32, false, false,
			                     true);
		case O::S_OR_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseOr32, IR::Type::U32, false, false,
			                     true);
		case O::S_XOR_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseXor32, IR::Type::U32, false, false,
			                     true);
		case O::S_NOT_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitwiseNot32, IR::Type::U32, false, false,
			                     true);
		case O::S_BREV_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitReverse32, IR::Type::U32, false, false,
			                     false);
		case O::S_BCNT1_I32_B32:
			return SimpleInteger(inst, IR::ValueOpcode::BitCount32, IR::Type::U32, false, false,
			                     true);
		case O::S_BCNT1_I32_B64:
			return SimpleInteger(inst, IR::ValueOpcode::BitCount64, IR::Type::U64, false, false,
			                     true);
		case O::S_FF1_I32_B32:
			return SimpleInteger(inst, IR::ValueOpcode::FindILsb32, IR::Type::U32, false, false,
			                     false);
		case O::S_LSHL_B32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftLeftLogical32, IR::Type::U32, false,
			                     true, true);
		case O::S_LSHR_B32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightLogical32, IR::Type::U32, false,
			                     true, true);
		case O::S_ASHR_I32:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightArithmetic32, IR::Type::U32,
			                     false, true, true);
		case O::S_LSHL_B64:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftLeftLogical64, IR::Type::U64, false,
			                     false, true);
		case O::S_LSHR_B64:
			return SimpleInteger(inst, IR::ValueOpcode::ShiftRightLogical64, IR::Type::U64, false,
			                     false, true);

		case O::S_ANDN2_B32:
			return ComposedIntegerBinary(inst, IR::ValueOpcode::BitwiseAnd32, true, false, true);
		case O::S_ORN2_B32:
			return ComposedIntegerBinary(inst, IR::ValueOpcode::BitwiseOr32, true, false, true);
		case O::S_NAND_B32:
			return ComposedIntegerBinary(inst, IR::ValueOpcode::BitwiseAnd32, false, true, true);
		case O::S_NOR_B32:
			return ComposedIntegerBinary(inst, IR::ValueOpcode::BitwiseOr32, false, true, true);
		case O::S_XNOR_B32:
			return ComposedIntegerBinary(inst, IR::ValueOpcode::BitwiseXor32, false, true, true);
		case O::S_FF1_I32_B64: return S_FF1_I32_B64(inst);
		case O::S_FLBIT_I32_B64: return S_FLBIT_I32_B64(inst);

		case O::S_BITSET0_B32: return S_BITSET_B32(inst, false);
		case O::S_BITSET1_B32: return S_BITSET_B32(inst, true);
		case O::S_BITREPLICATE_B64_B32: return S_BITREPLICATE_B64_B32(inst);
		case O::S_QUADMASK_B64: return S_QUADMASK_B64(inst);
		case O::S_BFM_B32: return BFM_B32(inst);
		case O::S_BFM_B64: return S_BFM_B64(inst);
		case O::S_BFE_U32: return S_BFE_U32(inst, false);
		case O::S_BFE_I32: return S_BFE_U32(inst, true);
		case O::S_BFE_U64: return S_BFE_U64(inst);
		case O::S_BITCMP0_B32: return S_BITCMP_B32(inst, false);
		case O::S_BITCMP1_B32: return S_BITCMP_B32(inst, true);
		case O::S_PACK_LL_B32_B16: return PackB16(inst, false, false);
		case O::S_PACK_LH_B32_B16: return PackB16(inst, false, true);
		case O::S_PACK_HH_B32_B16: return PackB16(inst, true, true);

		case O::S_NOP:
		case O::S_SLEEP:
		case O::S_TRAP: EmitControlNop(); return true;
		case O::S_WAITCNT_DEPCTR: EmitWaitcnt(); return true;
		case O::S_BARRIER: S_BARRIER(); return true;
		case O::S_SENDMSG: S_SENDMSG(); return true;
		case O::S_TTRACEDATA: S_TTRACEDATA(); return true;
		case O::S_INST_PREFETCH: S_INST_PREFETCH(); return true;
		case O::S_BRANCH:
		case O::S_CBRANCH_SCC0:
		case O::S_CBRANCH_SCC1:
		case O::S_CBRANCH_VCCZ:
		case O::S_CBRANCH_VCCNZ:
		case O::S_CBRANCH_EXECZ:
		case O::S_CBRANCH_EXECNZ:
		case O::S_ENDPGM: return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
