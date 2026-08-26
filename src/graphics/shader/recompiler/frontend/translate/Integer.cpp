#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::TranslateInteger16Operation(const Decoder::Instruction& inst) {
	const auto mask16 = [&](IR::U32 value) {
		return ir.BitwiseAnd(value, IR::U32(IR::Value(0xffffu)));
	};
	IR::Value result;
	switch (inst.opcode) {
		case Decoder::Opcode::V_LSHLREV_B16:
		case Decoder::Opcode::V_LSHRREV_B16:
		case Decoder::Opcode::V_ASHRREV_I16: {
			const bool arithmetic = inst.opcode == Decoder::Opcode::V_ASHRREV_I16;
			const auto value      = ReadU16AsU32(inst.src1, arithmetic);
			const auto count =
			    ir.BitwiseAnd(ReadU16AsU32(inst.src0, false), IR::U32(IR::Value(15u)));
			if (inst.opcode == Decoder::Opcode::V_LSHLREV_B16) {
				result = ir.ShiftLeftLogical(value, count);
			} else if (arithmetic) {
				result = ir.ShiftRightArithmetic(value, count);
			} else {
				result = ir.ShiftRightLogical(value, count);
			}
			break;
		}
		case Decoder::Opcode::V_ADD_NC_U16:
		case Decoder::Opcode::V_ADD_NC_I16:
			result = ir.IAdd(ReadU16AsU32(inst.src0, false), ReadU16AsU32(inst.src1, false));
			break;
		case Decoder::Opcode::V_SUB_NC_U16:
		case Decoder::Opcode::V_SUB_NC_I16:
			result = ir.ISub(ReadU16AsU32(inst.src0, false), ReadU16AsU32(inst.src1, false));
			break;
		case Decoder::Opcode::V_MED3_I16:
			result = ir.Emit(IR::ValueOpcode::SMedTri32,
			                 {ReadU16AsU32(inst.src0, true), ReadU16AsU32(inst.src1, true),
			                  ReadU16AsU32(inst.src2, true)});
			break;
		case Decoder::Opcode::V_MIN_I16:
		case Decoder::Opcode::V_MAX_I16:
		case Decoder::Opcode::V_MIN_U16:
		case Decoder::Opcode::V_MAX_U16: {
			const bool      sign = inst.opcode == Decoder::Opcode::V_MIN_I16 ||
			                       inst.opcode == Decoder::Opcode::V_MAX_I16;
			IR::ValueOpcode opcode;
			switch (inst.opcode) {
				case Decoder::Opcode::V_MIN_I16: opcode = IR::ValueOpcode::SMin32; break;
				case Decoder::Opcode::V_MAX_I16: opcode = IR::ValueOpcode::SMax32; break;
				case Decoder::Opcode::V_MIN_U16: opcode = IR::ValueOpcode::UMin32; break;
				default: opcode = IR::ValueOpcode::UMax32; break;
			}
			result =
			    ir.Emit(opcode, {ReadU16AsU32(inst.src0, sign), ReadU16AsU32(inst.src1, sign)});
			break;
		}
		default: return false;
	}
	WriteU16(DestinationOperand(inst), mask16(IR::U32(result)));
	return true;
}

bool Translator::TranslatePackedInteger16(const Decoder::Instruction& inst) {
	const auto lane = [&](const Decoder::Operand& operand, bool high, bool sign) {
		return ReadU16LaneAsU32(operand, high, sign);
	};
	const auto mask_count = [&](IR::U32 value) {
		return ir.BitwiseAnd(value, IR::U32(IR::Value(15u)));
	};
	const auto translate_lane = [&](bool high) -> IR::U32 {
		if (inst.opcode == Decoder::Opcode::V_PK_LSHLREV_B16 ||
		    inst.opcode == Decoder::Opcode::V_PK_LSHRREV_B16 ||
		    inst.opcode == Decoder::Opcode::V_PK_ASHRREV_I16) {
			const auto count      = mask_count(lane(inst.src0, high, false));
			const bool arithmetic = inst.opcode == Decoder::Opcode::V_PK_ASHRREV_I16;
			const auto value      = lane(inst.src1, high, arithmetic);
			if (inst.opcode == Decoder::Opcode::V_PK_LSHLREV_B16) {
				return ir.ShiftLeftLogical(value, count);
			}
			return arithmetic ? ir.ShiftRightArithmetic(value, count)
			                  : ir.ShiftRightLogical(value, count);
		}

		const bool signed_minmax = inst.opcode == Decoder::Opcode::V_PK_MAX_I16 ||
		                           inst.opcode == Decoder::Opcode::V_PK_MIN_I16;
		const auto lhs           = lane(inst.src0, high, signed_minmax);
		const auto rhs           = lane(inst.src1, high, signed_minmax);
		switch (inst.opcode) {
			case Decoder::Opcode::V_PK_MAD_I16:
			case Decoder::Opcode::V_PK_MAD_U16:
				return ir.IAdd(ir.IMul(lhs, rhs),
				               lane(inst.src2, high, inst.opcode == Decoder::Opcode::V_PK_MAD_I16));
			case Decoder::Opcode::V_PK_MUL_LO_U16: return ir.IMul(lhs, rhs);
			case Decoder::Opcode::V_PK_ADD_I16:
			case Decoder::Opcode::V_PK_ADD_U16: return ir.IAdd(lhs, rhs);
			case Decoder::Opcode::V_PK_SUB_I16:
			case Decoder::Opcode::V_PK_SUB_U16: return ir.ISub(lhs, rhs);
			case Decoder::Opcode::V_PK_MAX_I16:
				return IR::U32(ir.Emit(IR::ValueOpcode::SMax32, {lhs, rhs}));
			case Decoder::Opcode::V_PK_MIN_I16:
				return IR::U32(ir.Emit(IR::ValueOpcode::SMin32, {lhs, rhs}));
			case Decoder::Opcode::V_PK_MAX_U16:
				return IR::U32(ir.Emit(IR::ValueOpcode::UMax32, {lhs, rhs}));
			case Decoder::Opcode::V_PK_MIN_U16:
				return IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {lhs, rhs}));
			default: EXIT("invalid packed integer opcode");
		}
	};

	switch (inst.opcode) {
		case Decoder::Opcode::V_PK_MAD_I16:
		case Decoder::Opcode::V_PK_MUL_LO_U16:
		case Decoder::Opcode::V_PK_ADD_I16:
		case Decoder::Opcode::V_PK_SUB_I16:
		case Decoder::Opcode::V_PK_LSHLREV_B16:
		case Decoder::Opcode::V_PK_LSHRREV_B16:
		case Decoder::Opcode::V_PK_ASHRREV_I16:
		case Decoder::Opcode::V_PK_MAX_I16:
		case Decoder::Opcode::V_PK_MIN_I16:
		case Decoder::Opcode::V_PK_MAD_U16:
		case Decoder::Opcode::V_PK_ADD_U16:
		case Decoder::Opcode::V_PK_SUB_U16:
		case Decoder::Opcode::V_PK_MAX_U16:
		case Decoder::Opcode::V_PK_MIN_U16:
			WriteOperand(DestinationOperand(inst),
			             PackU16Lanes(translate_lane(false), translate_lane(true)));
			return true;
		default: return false;
	}
}

IR::U1 Translator::EvaluateU64Mask(const Decoder::Instruction& inst) {
	const auto lhs = [&] { return ReadMask(inst.src0); };
	const auto rhs = [&] { return ReadMask(inst.src1); };
	switch (inst.opcode) {
		case Decoder::Opcode::S_AND_B64: return ir.LogicalAnd(lhs(), rhs());
		case Decoder::Opcode::S_OR_B64: return ir.LogicalOr(lhs(), rhs());
		case Decoder::Opcode::S_XOR_B64:
			return IR::U1(ir.Emit(IR::ValueOpcode::LogicalXor, {lhs(), rhs()}));
		case Decoder::Opcode::S_ANDN2_B64: return ir.LogicalAnd(lhs(), ir.LogicalNot(rhs()));
		case Decoder::Opcode::S_ORN2_B64: return ir.LogicalOr(lhs(), ir.LogicalNot(rhs()));
		case Decoder::Opcode::S_NAND_B64: return ir.LogicalNot(ir.LogicalAnd(lhs(), rhs()));
		case Decoder::Opcode::S_NOR_B64: return ir.LogicalNot(ir.LogicalOr(lhs(), rhs()));
		case Decoder::Opcode::S_XNOR_B64:
			return ir.LogicalNot(IR::U1(ir.Emit(IR::ValueOpcode::LogicalXor, {lhs(), rhs()})));
		case Decoder::Opcode::S_NOT_B64: return ir.LogicalNot(lhs());
		default: EXIT("invalid 64-bit mask opcode");
	}
}

bool Translator::TranslateSimpleInteger(const Decoder::Instruction& inst) {
	IR::ValueOpcode opcode {};
	IR::Type        type = IR::Type::U32;
	switch (inst.opcode) {
		case Decoder::Opcode::S_ABS_I32: opcode = IR::ValueOpcode::IAbs32; break;
		case Decoder::Opcode::S_MUL_I32:
		case Decoder::Opcode::S_MULK_I32:
		case Decoder::Opcode::V_MUL_LO_U32:
		case Decoder::Opcode::V_MUL_LO_I32: opcode = IR::ValueOpcode::IMul32; break;
		case Decoder::Opcode::S_MUL_HI_U32:
		case Decoder::Opcode::V_MUL_HI_U32: opcode = IR::ValueOpcode::UMulHi; break;
		case Decoder::Opcode::V_MUL_HI_I32: opcode = IR::ValueOpcode::SMulHi; break;
		case Decoder::Opcode::V_ADD_NC_U32: opcode = IR::ValueOpcode::IAdd32; break;
		case Decoder::Opcode::V_SUB_NC_U32:
		case Decoder::Opcode::V_SUBREV_NC_U32: opcode = IR::ValueOpcode::ISub32; break;
		case Decoder::Opcode::V_MIN_I32: opcode = IR::ValueOpcode::SMin32; break;
		case Decoder::Opcode::V_MAX_I32: opcode = IR::ValueOpcode::SMax32; break;
		case Decoder::Opcode::V_MIN_U32: opcode = IR::ValueOpcode::UMin32; break;
		case Decoder::Opcode::V_MAX_U32: opcode = IR::ValueOpcode::UMax32; break;
		case Decoder::Opcode::V_MIN3_I32: opcode = IR::ValueOpcode::SMinTri32; break;
		case Decoder::Opcode::V_MAX3_I32: opcode = IR::ValueOpcode::SMaxTri32; break;
		case Decoder::Opcode::V_MED3_I32: opcode = IR::ValueOpcode::SMedTri32; break;
		case Decoder::Opcode::V_MIN3_U32: opcode = IR::ValueOpcode::UMinTri32; break;
		case Decoder::Opcode::V_MAX3_U32: opcode = IR::ValueOpcode::UMaxTri32; break;
		case Decoder::Opcode::V_MED3_U32: opcode = IR::ValueOpcode::UMedTri32; break;
		case Decoder::Opcode::S_AND_B32:
		case Decoder::Opcode::V_AND_B32: opcode = IR::ValueOpcode::BitwiseAnd32; break;
		case Decoder::Opcode::S_OR_B32:
		case Decoder::Opcode::V_OR_B32: opcode = IR::ValueOpcode::BitwiseOr32; break;
		case Decoder::Opcode::S_XOR_B32:
		case Decoder::Opcode::V_XOR_B32: opcode = IR::ValueOpcode::BitwiseXor32; break;
		case Decoder::Opcode::S_NOT_B32:
		case Decoder::Opcode::V_NOT_B32: opcode = IR::ValueOpcode::BitwiseNot32; break;
		case Decoder::Opcode::S_BREV_B32:
		case Decoder::Opcode::V_BFREV_B32: opcode = IR::ValueOpcode::BitReverse32; break;
		case Decoder::Opcode::S_BCNT1_I32_B32: opcode = IR::ValueOpcode::BitCount32; break;
		case Decoder::Opcode::S_BCNT1_I32_B64:
			opcode = IR::ValueOpcode::BitCount64;
			type   = IR::Type::U64;
			break;
		case Decoder::Opcode::S_FF1_I32_B32:
		case Decoder::Opcode::V_FFBL_B32: opcode = IR::ValueOpcode::FindILsb32; break;
		case Decoder::Opcode::S_LSHL_B32:
		case Decoder::Opcode::V_LSHL_B32:
		case Decoder::Opcode::V_LSHLREV_B32: opcode = IR::ValueOpcode::ShiftLeftLogical32; break;
		case Decoder::Opcode::S_LSHR_B32:
		case Decoder::Opcode::V_LSHR_B32:
		case Decoder::Opcode::V_LSHRREV_B32: opcode = IR::ValueOpcode::ShiftRightLogical32; break;
		case Decoder::Opcode::S_ASHR_I32:
		case Decoder::Opcode::V_ASHR_I32:
		case Decoder::Opcode::V_ASHRREV_I32:
			opcode = IR::ValueOpcode::ShiftRightArithmetic32;
			break;
		case Decoder::Opcode::S_LSHL_B64:
		case Decoder::Opcode::V_LSHLREV_B64:
			opcode = IR::ValueOpcode::ShiftLeftLogical64;
			type   = IR::Type::U64;
			break;
		case Decoder::Opcode::S_LSHR_B64:
		case Decoder::Opcode::V_LSHRREV_B64:
			opcode = IR::ValueOpcode::ShiftRightLogical64;
			type   = IR::Type::U64;
			break;
		default: return false;
	}

	const bool               reverse = inst.opcode == Decoder::Opcode::V_SUBREV_NC_U32 ||
	                                   inst.opcode == Decoder::Opcode::V_LSHLREV_B32 ||
	                                   inst.opcode == Decoder::Opcode::V_LSHRREV_B32 ||
	                                   inst.opcode == Decoder::Opcode::V_ASHRREV_I32 ||
	                                   inst.opcode == Decoder::Opcode::V_LSHLREV_B64 ||
	                                   inst.opcode == Decoder::Opcode::V_LSHRREV_B64;
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < inst.src_count; index++) {
		const auto arg_type = IR::ArgTypeOf(opcode, index);
		const auto operand  = SourceAt(inst, reverse && index < 2u ? 1u - index : index);
		args[index]         = ReadOperand(operand, arg_type == IR::Type::Void ? type : arg_type);
		if (index == 1u && (opcode == IR::ValueOpcode::ShiftLeftLogical32 ||
		                    opcode == IR::ValueOpcode::ShiftRightLogical32 ||
		                    opcode == IR::ValueOpcode::ShiftRightArithmetic32)) {
			args[index] = ir.BitwiseAnd(IR::U32(args[index]), IR::U32(IR::Value(31u)));
		}
	}
	IR::Value result;
	switch (inst.src_count) {
		case 1: result = ir.Emit(opcode, {args[0]}); break;
		case 2: result = ir.Emit(opcode, {args[0], args[1]}); break;
		case 3: result = ir.Emit(opcode, {args[0], args[1], args[2]}); break;
		default: EXIT("invalid simple integer source count: %u", inst.src_count);
	}
	WriteOperand(DestinationOperand(inst), result);
	switch (inst.opcode) {
		case Decoder::Opcode::S_ABS_I32:
		case Decoder::Opcode::S_BCNT1_I32_B32:
		case Decoder::Opcode::S_BCNT1_I32_B64:
		case Decoder::Opcode::S_AND_B32:
		case Decoder::Opcode::S_OR_B32:
		case Decoder::Opcode::S_XOR_B32:
		case Decoder::Opcode::S_NOT_B32:
		case Decoder::Opcode::S_LSHL_B32:
		case Decoder::Opcode::S_LSHR_B32:
		case Decoder::Opcode::S_ASHR_I32:
			ir.SetScc(ir.INotEqual(IR::U32(result), IR::U32(IR::Value(0u))));
			break;
		case Decoder::Opcode::S_LSHL_B64:
		case Decoder::Opcode::S_LSHR_B64:
			ir.SetScc(
			    IR::U1(ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
			break;
		default: break;
	}
	return true;
}

bool Translator::TranslateComposedInteger(const Decoder::Instruction& inst) {
	const auto binary_u32 = [&](auto operation) {
		const auto lhs = ReadU32(inst.src0);
		const auto rhs = ReadU32(inst.src1);
		return operation(lhs, rhs);
	};

	IR::Value result;
	switch (inst.opcode) {
		case Decoder::Opcode::S_ANDN2_B32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseAnd(lhs, ir.BitwiseNot(rhs)); });
			break;
		case Decoder::Opcode::S_ORN2_B32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseOr(lhs, ir.BitwiseNot(rhs)); });
			break;
		case Decoder::Opcode::S_NAND_B32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseAnd(lhs, rhs)); });
			break;
		case Decoder::Opcode::S_NOR_B32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseOr(lhs, rhs)); });
			break;
		case Decoder::Opcode::S_XNOR_B32:
		case Decoder::Opcode::V_XNOR_B32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseXor(lhs, rhs)); });
			break;
		case Decoder::Opcode::V_AND_OR_B32: {
			const auto lhs = ReadU32(inst.src0);
			const auto rhs = ReadU32(inst.src1);
			const auto add = ReadU32(inst.src2);
			result         = ir.BitwiseOr(ir.BitwiseAnd(lhs, rhs), add);
			break;
		}
		case Decoder::Opcode::V_OR3_B32:
			result = ir.BitwiseOr(ir.BitwiseOr(ReadU32(inst.src0), ReadU32(inst.src1)),
			                      ReadU32(inst.src2));
			break;
		case Decoder::Opcode::V_XOR3_B32:
			result = ir.BitwiseXor(ir.BitwiseXor(ReadU32(inst.src0), ReadU32(inst.src1)),
			                       ReadU32(inst.src2));
			break;
		case Decoder::Opcode::S_FF1_I32_B64: {
			const auto source        = ExtractU64(ReadU64(inst.src0));
			const auto low_lsb       = IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {source[0]}));
			const auto high_lsb      = IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {source[1]}));
			const auto high_position = ir.IAdd(high_lsb, IR::U32(IR::Value(32u)));
			result = ir.Select(ir.INotEqual(source[0], IR::U32(IR::Value(0u))), low_lsb,
			                   ir.Select(ir.INotEqual(source[1], IR::U32(IR::Value(0u))),
			                             high_position, IR::U32(IR::Value(0xffffffffu))));
			break;
		}
		case Decoder::Opcode::V_FFBH_U32: {
			const auto source   = ReadU32(inst.src0);
			const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {source}));
			const auto position = ir.ISub(IR::U32(IR::Value(31u)), msb);
			result              = ir.Select(ir.INotEqual(source, IR::U32(IR::Value(0u))), position,
			                                IR::U32(IR::Value(0xffffffffu)));
			break;
		}
		case Decoder::Opcode::S_FLBIT_I32_B64: {
			const auto source   = ReadU64(inst.src0);
			const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb64, {source}));
			const auto position = ir.ISub(IR::U32(IR::Value(63u)), msb);
			const auto nonzero  = IR::U1(
			    ir.Emit(IR::ValueOpcode::INotEqual64,
			            {source, ir.ConstructU64(IR::U32(IR::Value(0u)), IR::U32(IR::Value(0u)))}));
			result = ir.Select(nonzero, position, IR::U32(IR::Value(0xffffffffu)));
			break;
		}
		default: return false;
	}
	WriteOperand(DestinationOperand(inst), result);
	switch (inst.opcode) {
		case Decoder::Opcode::S_ANDN2_B32:
		case Decoder::Opcode::S_ORN2_B32:
		case Decoder::Opcode::S_NAND_B32:
		case Decoder::Opcode::S_NOR_B32:
		case Decoder::Opcode::S_XNOR_B32:
			ir.SetScc(ir.INotEqual(IR::U32(result), IR::U32(IR::Value(0u))));
			break;
		default: break;
	}
	return true;
}

bool Translator::TranslateExtendedInteger(const Decoder::Instruction& inst) {
	const auto imm  = [](uint32_t value) { return IR::U32(IR::Value(value)); };
	const auto mask = [&](IR::U32 value, uint32_t bits) { return ir.BitwiseAnd(value, imm(bits)); };
	const auto extract = [&](IR::U32 value, IR::U32 offset, IR::U32 width, bool sign) {
		return IR::U32(
		    ir.Emit(sign ? IR::ValueOpcode::BitFieldSExtract : IR::ValueOpcode::BitFieldUExtract,
		            {value, offset, width}));
	};
	const auto right_mask32 = [&](IR::U32 count) {
		return IR::U32(
		    ir.Emit(IR::ValueOpcode::BitFieldInsert, {imm(0), imm(0xffffffffu), imm(0), count}));
	};
	const auto right_mask64 = [&](IR::U32 count) {
		const auto below32    = IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {count, imm(32)}));
		const auto above32    = IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThan32, {count, imm(32)}));
		const auto low_count  = ir.Select(below32, count, imm(32));
		const auto high_count = ir.Select(above32, ir.ISub(count, imm(32)), imm(0));
		return ir.ConstructU64(right_mask32(low_count), right_mask32(high_count));
	};

	IR::Value result;
	switch (inst.opcode) {
		case Decoder::Opcode::V_MAD_I32_I24:
		case Decoder::Opcode::V_MAD_U32_U24:
		case Decoder::Opcode::V_MUL_I32_I24:
		case Decoder::Opcode::V_MUL_U32_U24: {
			const bool sign  = inst.opcode == Decoder::Opcode::V_MAD_I32_I24 ||
			                   inst.opcode == Decoder::Opcode::V_MUL_I32_I24;
			const auto lhs   = extract(ReadU32(inst.src0), imm(0), imm(24), sign);
			const auto rhs   = extract(ReadU32(inst.src1), imm(0), imm(24), sign);
			auto       value = ir.IMul(lhs, rhs);
			if (inst.src_count == 3) {
				value = ir.IAdd(value, ReadU32(inst.src2));
			}
			result = value;
			break;
		}
		case Decoder::Opcode::V_MAD_U64_U32: {
			const auto lhs       = ReadU32(inst.src0);
			const auto rhs       = ReadU32(inst.src1);
			const auto add       = ExtractU64(ReadU64(inst.src2));
			const auto mul_low   = ir.IMul(lhs, rhs);
			const auto mul_high  = IR::U32(ir.Emit(IR::ValueOpcode::UMulHi, {lhs, rhs}));
			const auto low       = ir.IAdd(mul_low, add[0]);
			const auto carry_low = ir.ULessThan(low, mul_low);
			const auto high0     = ir.IAdd(mul_high, add[1]);
			const auto carry0    = ir.ULessThan(high0, mul_high);
			const auto high      = ir.IAdd(high0, ir.Select(carry_low, imm(1), imm(0)));
			const auto carry1    = ir.ULessThan(high, high0);
			WriteOperand(DestinationOperand(inst), ir.ConstructU64(low, high));
			if (inst.dst2.kind != Decoder::OperandKind::Null &&
			    inst.dst2.kind != Decoder::OperandKind::Unknown) {
				WriteMask(inst.dst2, ir.LogicalOr(carry0, carry1));
			}
			return true;
		}
		case Decoder::Opcode::V_SAD_U32: {
			const auto lhs = ReadU32(inst.src0);
			const auto rhs = ReadU32(inst.src1);
			const auto lo  = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {lhs, rhs}));
			const auto hi  = IR::U32(ir.Emit(IR::ValueOpcode::UMax32, {lhs, rhs}));
			result         = ir.IAdd(ir.ISub(hi, lo), ReadU32(inst.src2));
			break;
		}
		case Decoder::Opcode::V_ADD3_U32:
			result = ir.IAdd(ir.IAdd(ReadU32(inst.src0), ReadU32(inst.src1)), ReadU32(inst.src2));
			break;
		case Decoder::Opcode::S_BITSET0_B32:
		case Decoder::Opcode::S_BITSET1_B32: {
			const auto bit = ir.ShiftLeftLogical(imm(1), mask(ReadU32(inst.src0), 31u));
			result         = inst.opcode == Decoder::Opcode::S_BITSET0_B32
			                     ? IR::Value(ir.BitwiseAnd(ReadU32(inst.dst), ir.BitwiseNot(bit)))
			                     : IR::Value(ir.BitwiseOr(ReadU32(inst.dst), bit));
			break;
		}
		case Decoder::Opcode::V_BCNT_U32_B32:
			result = ir.IAdd(IR::U32(ir.Emit(IR::ValueOpcode::BitCount32, {ReadU32(inst.src0)})),
			                 ReadU32(inst.src1));
			break;
		case Decoder::Opcode::V_MBCNT_LO_U32_B32:
		case Decoder::Opcode::V_MBCNT_HI_U32_B32: {
			const auto lane  = IR::U32(ir.Emit(IR::ValueOpcode::LaneId));
			const auto local = mask(lane, 31u);
			const auto below = ir.ISub(ir.ShiftLeftLogical(imm(1), local), imm(1));
			const auto high_lane =
			    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThanEqual32, {lane, imm(32)}));
			const auto thread_mask = inst.opcode == Decoder::Opcode::V_MBCNT_LO_U32_B32
			                             ? ir.Select(high_lane, imm(0xffffffffu), below)
			                             : ir.Select(high_lane, below, imm(0));
			const auto active      = ir.BitwiseAnd(ReadU32(inst.src0), thread_mask);
			result = ir.IAdd(IR::U32(ir.Emit(IR::ValueOpcode::BitCount32, {active})),
			                 ReadU32(inst.src1));
			break;
		}
		case Decoder::Opcode::S_BITREPLICATE_B64_B32: {
			const auto replicate = [&](IR::U32 value) {
				auto bits = ir.BitwiseOr(value, ir.ShiftLeftLogical(value, imm(8)));
				bits      = ir.BitwiseAnd(bits, imm(0x00ff00ffu));
				bits      = ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, imm(4)));
				bits      = ir.BitwiseAnd(bits, imm(0x0f0f0f0fu));
				bits      = ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, imm(2)));
				bits      = ir.BitwiseAnd(bits, imm(0x33333333u));
				bits      = ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, imm(1)));
				bits      = ir.BitwiseAnd(bits, imm(0x55555555u));
				return ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, imm(1)));
			};
			const auto source = ReadU32(inst.src0);
			result            = ir.ConstructU64(replicate(mask(source, 0xffffu)),
			                                    replicate(ir.ShiftRightLogical(source, imm(16))));
			break;
		}
		case Decoder::Opcode::S_QUADMASK_B64: {
			const auto compact = [&](IR::U32 value) {
				auto bits = ir.BitwiseOr(value, ir.ShiftRightLogical(value, imm(1)));
				bits      = ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, imm(2)));
				bits      = mask(bits, 0x11111111u);
				bits = mask(ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, imm(3))), 0x03030303u);
				bits = mask(ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, imm(6))), 0x000f000fu);
				return mask(ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, imm(12))), 0xffu);
			};
			const auto source = ReadU32Pair(inst.src0);
			const auto quads =
			    ir.BitwiseOr(compact(source[0]), ir.ShiftLeftLogical(compact(source[1]), imm(8)));
			result = ir.ConstructU64(quads, imm(0));
			break;
		}
		case Decoder::Opcode::S_BFM_B32:
		case Decoder::Opcode::V_BFM_B32: {
			const auto count  = mask(ReadU32(inst.src0), 31u);
			const auto offset = mask(ReadU32(inst.src1), 31u);
			result =
			    ir.Emit(IR::ValueOpcode::BitFieldInsert, {imm(0), imm(0xffffffffu), offset, count});
			break;
		}
		case Decoder::Opcode::S_BFM_B64: {
			const auto count  = mask(ReadU32(inst.src0), 63u);
			const auto offset = mask(ReadU32(inst.src1), 63u);
			result = ir.Emit(IR::ValueOpcode::ShiftLeftLogical64, {right_mask64(count), offset});
			break;
		}
		case Decoder::Opcode::S_BFE_U32:
		case Decoder::Opcode::S_BFE_I32: {
			const auto source = ReadU32(inst.src0);
			const auto field  = ReadU32(inst.src1);
			const auto offset = extract(field, imm(0), imm(5), false);
			const auto count =
			    IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {extract(field, imm(16), imm(7), false),
			                                              ir.ISub(imm(32), offset)}));
			const auto opcode = inst.opcode == Decoder::Opcode::S_BFE_I32
			                        ? IR::ValueOpcode::BitFieldSExtract
			                        : IR::ValueOpcode::BitFieldUExtract;
			result            = ir.Emit(opcode, {source, offset, count});
			break;
		}
		case Decoder::Opcode::S_BFE_U64: {
			const auto source    = ReadU64(inst.src0);
			const auto field     = ReadU32(inst.src1);
			const auto offset    = extract(field, imm(0), imm(6), false);
			const auto raw_count = extract(field, imm(16), imm(7), false);
			const auto available = ir.ISub(imm(64), offset);
			const auto count = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {raw_count, available}));
			const auto shifted =
			    IR::U64(ir.Emit(IR::ValueOpcode::ShiftRightLogical64, {source, offset}));
			result = ir.Emit(IR::ValueOpcode::BitwiseAnd64, {shifted, right_mask64(count)});
			break;
		}
		case Decoder::Opcode::V_BFE_U32:
		case Decoder::Opcode::V_BFE_I32: {
			const auto source = ReadU32(inst.src0);
			const auto offset = mask(ReadU32(inst.src1), 31u);
			const auto count =
			    IR::U32(ir.Emit(IR::ValueOpcode::UMin32,
			                    {mask(ReadU32(inst.src2), 31u), ir.ISub(imm(32), offset)}));
			const auto opcode = inst.opcode == Decoder::Opcode::V_BFE_I32
			                        ? IR::ValueOpcode::BitFieldSExtract
			                        : IR::ValueOpcode::BitFieldUExtract;
			result            = ir.Emit(opcode, {source, offset, count});
			break;
		}
		case Decoder::Opcode::V_BFI_B32: {
			const auto bits   = ReadU32(inst.src0);
			const auto insert = ReadU32(inst.src1);
			const auto base   = ReadU32(inst.src2);
			result =
			    ir.BitwiseOr(ir.BitwiseAnd(bits, insert), ir.BitwiseAnd(ir.BitwiseNot(bits), base));
			break;
		}
		case Decoder::Opcode::S_BITCMP0_B32:
		case Decoder::Opcode::S_BITCMP1_B32: {
			const auto value    = ReadU32(inst.src0);
			const auto bit      = extract(value, mask(ReadU32(inst.src1), 31u), imm(1), false);
			const auto expected = imm(inst.opcode == Decoder::Opcode::S_BITCMP1_B32 ? 1u : 0u);
			WriteCompareResult(inst.dst, ir.IEqual(bit, expected));
			return true;
		}
		case Decoder::Opcode::V_ALIGNBIT_B32: {
			const auto hi          = ReadU32(inst.src0);
			const auto lo          = ReadU32(inst.src1);
			const auto shift       = mask(ReadU32(inst.src2), 31u);
			const auto lo_part     = ir.ShiftRightLogical(lo, shift);
			const auto hi_part_raw = ir.ShiftLeftLogical(hi, mask(ir.ISub(imm(32), shift), 31u));
			const auto hi_part     = ir.Select(ir.INotEqual(shift, imm(0)), hi_part_raw, imm(0));
			result                 = ir.BitwiseOr(lo_part, hi_part);
			break;
		}
		case Decoder::Opcode::V_ALIGNBYTE_B32: {
			const auto hi           = ReadU32(inst.src0);
			const auto lo           = ReadU32(inst.src1);
			const auto byte_offset  = mask(ReadU32(inst.src2), 31u);
			const auto bit_offset   = ir.ShiftLeftLogical(byte_offset, imm(3));
			const auto concatenated = ir.ConstructU64(lo, hi);
			const auto shifted      = IR::U64(ir.Emit(IR::ValueOpcode::ShiftRightLogical64,
			                                          {concatenated, mask(bit_offset, 63u)}));
			const auto in_range =
			    IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {byte_offset, imm(8)}));
			result = ir.Select(in_range, ExtractU64(shifted)[0], imm(0));
			break;
		}
		case Decoder::Opcode::V_LSHL_ADD_U32:
			result = ir.IAdd(ir.ShiftLeftLogical(ReadU32(inst.src0), mask(ReadU32(inst.src1), 31u)),
			                 ReadU32(inst.src2));
			break;
		case Decoder::Opcode::V_ADD_LSHL_U32:
			result = ir.ShiftLeftLogical(ir.IAdd(ReadU32(inst.src0), ReadU32(inst.src1)),
			                             mask(ReadU32(inst.src2), 31u));
			break;
		case Decoder::Opcode::V_XAD_U32:
			result =
			    ir.IAdd(ir.BitwiseXor(ReadU32(inst.src0), ReadU32(inst.src1)), ReadU32(inst.src2));
			break;
		case Decoder::Opcode::V_LSHL_OR_B32:
			result =
			    ir.BitwiseOr(ir.ShiftLeftLogical(ReadU32(inst.src0), mask(ReadU32(inst.src1), 31u)),
			                 ReadU32(inst.src2));
			break;
		case Decoder::Opcode::V_CNDMASK_B32: {
			Decoder::Operand mask_operand;
			mask_operand.kind = Decoder::OperandKind::VccLo;
			if (inst.src_count >= 3u) {
				mask_operand = inst.src2;
			}
			const auto condition = ReadMask(mask_operand);
			if (inst.src0.negate || inst.src0.absolute || inst.src1.negate || inst.src1.absolute) {
				result = ir.Emit(IR::ValueOpcode::SelectF32,
				                 {condition, ReadOperand(inst.src1, IR::Type::F32),
				                  ReadOperand(inst.src0, IR::Type::F32)});
			} else {
				result = ir.Select(condition, ReadU32(inst.src1), ReadU32(inst.src0));
			}
			break;
		}
		case Decoder::Opcode::S_PACK_LL_B32_B16:
		case Decoder::Opcode::S_PACK_LH_B32_B16:
		case Decoder::Opcode::S_PACK_HH_B32_B16:
		case Decoder::Opcode::V_CVT_PK_U16_U32:
		case Decoder::Opcode::V_CVT_PK_I16_I32: {
			const bool high0 = inst.opcode == Decoder::Opcode::S_PACK_HH_B32_B16;
			const bool high1 = inst.opcode == Decoder::Opcode::S_PACK_LH_B32_B16 || high0;
			const auto lo =
			    high0 ? ir.ShiftRightLogical(ReadU32(inst.src0), imm(16)) : ReadU32(inst.src0);
			const auto hi =
			    high1 ? ir.ShiftRightLogical(ReadU32(inst.src1), imm(16)) : ReadU32(inst.src1);
			result =
			    ir.BitwiseOr(mask(lo, 0xffffu), ir.ShiftLeftLogical(mask(hi, 0xffffu), imm(16)));
			break;
		}
		default: return false;
	}
	WriteOperand(DestinationOperand(inst), result);
	switch (inst.opcode) {
		case Decoder::Opcode::S_BFE_U32:
		case Decoder::Opcode::S_BFE_I32: ir.SetScc(ir.INotEqual(IR::U32(result), imm(0u))); break;
		case Decoder::Opcode::S_BFE_U64:
		case Decoder::Opcode::S_QUADMASK_B64:
			ir.SetScc(
			    IR::U1(ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
			break;
		default: break;
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
