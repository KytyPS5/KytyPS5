#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

bool Translator::TranslateInteger16Operation(const IR::Instruction& inst) {
	const auto mask16 = [&](IR::U32 value) {
		return ir.BitwiseAnd(value, IR::U32(IR::Value(0xffffu)));
	};
	IR::Value result;
	switch (inst.op) {
		case IR::Opcode::ShiftLeftLogicalU16:
		case IR::Opcode::ShiftRightLogicalU16:
		case IR::Opcode::ShiftRightArithmeticI16: {
			const bool arithmetic = inst.op == IR::Opcode::ShiftRightArithmeticI16;
			const auto value      = ReadU16AsU32(inst.src[0], arithmetic);
			const auto count =
			    ir.BitwiseAnd(ReadU16AsU32(inst.src[1], false), IR::U32(IR::Value(15u)));
			if (inst.op == IR::Opcode::ShiftLeftLogicalU16) {
				result = ir.ShiftLeftLogical(value, count);
			} else if (arithmetic) {
				result = ir.ShiftRightArithmetic(value, count);
			} else {
				result = ir.ShiftRightLogical(value, count);
			}
			break;
		}
		case IR::Opcode::IAddU16:
			result = ir.IAdd(ReadU16AsU32(inst.src[0], false), ReadU16AsU32(inst.src[1], false));
			break;
		case IR::Opcode::ISubI16:
			result = ir.ISub(ReadU16AsU32(inst.src[0], false), ReadU16AsU32(inst.src[1], false));
			break;
		case IR::Opcode::IMinI16:
		case IR::Opcode::IMaxI16:
		case IR::Opcode::UMinU16:
		case IR::Opcode::UMaxU16: {
			const bool      sign = inst.op == IR::Opcode::IMinI16 || inst.op == IR::Opcode::IMaxI16;
			IR::ValueOpcode opcode;
			switch (inst.op) {
				case IR::Opcode::IMinI16: opcode = IR::ValueOpcode::SMin32; break;
				case IR::Opcode::IMaxI16: opcode = IR::ValueOpcode::SMax32; break;
				case IR::Opcode::UMinU16: opcode = IR::ValueOpcode::UMin32; break;
				default: opcode = IR::ValueOpcode::UMax32; break;
			}
			result =
			    ir.Emit(opcode, {ReadU16AsU32(inst.src[0], sign), ReadU16AsU32(inst.src[1], sign)});
			break;
		}
		default: return false;
	}
	WriteU16(inst.dst, mask16(IR::U32(result)));
	return true;
}

bool Translator::TranslatePackedInteger16(const IR::Instruction& inst) {
	const auto lane = [&](const IR::Operand& operand, bool high, bool sign) {
		return ReadU16LaneAsU32(operand, high, sign);
	};
	const auto mask_count = [&](IR::U32 value) {
		return ir.BitwiseAnd(value, IR::U32(IR::Value(15u)));
	};
	const auto translate_lane = [&](bool high) -> IR::U32 {
		if (inst.op == IR::Opcode::PackedLshlrevB16 || inst.op == IR::Opcode::PackedLshrrevB16 ||
		    inst.op == IR::Opcode::PackedAshrrevI16) {
			const auto count      = mask_count(lane(inst.src[0], high, false));
			const bool arithmetic = inst.op == IR::Opcode::PackedAshrrevI16;
			const auto value      = lane(inst.src[1], high, arithmetic);
			if (inst.op == IR::Opcode::PackedLshlrevB16) {
				return ir.ShiftLeftLogical(value, count);
			}
			return arithmetic ? ir.ShiftRightArithmetic(value, count)
			                  : ir.ShiftRightLogical(value, count);
		}

		const bool signed_minmax =
		    inst.op == IR::Opcode::PackedMaxI16 || inst.op == IR::Opcode::PackedMinI16;
		const auto lhs = lane(inst.src[0], high, signed_minmax);
		const auto rhs = lane(inst.src[1], high, signed_minmax);
		switch (inst.op) {
			case IR::Opcode::PackedMadI16:
			case IR::Opcode::PackedMadU16:
				return ir.IAdd(ir.IMul(lhs, rhs),
				               lane(inst.src[2], high, inst.op == IR::Opcode::PackedMadI16));
			case IR::Opcode::PackedMulLoU16: return ir.IMul(lhs, rhs);
			case IR::Opcode::PackedAddI16:
			case IR::Opcode::PackedAddU16: return ir.IAdd(lhs, rhs);
			case IR::Opcode::PackedSubI16:
			case IR::Opcode::PackedSubU16: return ir.ISub(lhs, rhs);
			case IR::Opcode::PackedMaxI16:
				return IR::U32(ir.Emit(IR::ValueOpcode::SMax32, {lhs, rhs}));
			case IR::Opcode::PackedMinI16:
				return IR::U32(ir.Emit(IR::ValueOpcode::SMin32, {lhs, rhs}));
			case IR::Opcode::PackedMaxU16:
				return IR::U32(ir.Emit(IR::ValueOpcode::UMax32, {lhs, rhs}));
			case IR::Opcode::PackedMinU16:
				return IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {lhs, rhs}));
			default: EXIT("invalid packed integer opcode");
		}
	};

	switch (inst.op) {
		case IR::Opcode::PackedMadI16:
		case IR::Opcode::PackedMulLoU16:
		case IR::Opcode::PackedAddI16:
		case IR::Opcode::PackedSubI16:
		case IR::Opcode::PackedLshlrevB16:
		case IR::Opcode::PackedLshrrevB16:
		case IR::Opcode::PackedAshrrevI16:
		case IR::Opcode::PackedMaxI16:
		case IR::Opcode::PackedMinI16:
		case IR::Opcode::PackedMadU16:
		case IR::Opcode::PackedAddU16:
		case IR::Opcode::PackedSubU16:
		case IR::Opcode::PackedMaxU16:
		case IR::Opcode::PackedMinU16:
			WriteOperand(inst.dst, PackU16Lanes(translate_lane(false), translate_lane(true)));
			return true;
		default: return false;
	}
}

bool Translator::TranslatePerInvocationU64Mask(const IR::Instruction& inst) {
	if (!current_per_invocation_masks) {
		return false;
	}

	const auto lhs = [&] { return ReadMask(inst.src[0]); };
	const auto rhs = [&] { return ReadMask(inst.src[1]); };
	IR::U1     result;
	switch (inst.op) {
		case IR::Opcode::BitwiseAndU64: result = ir.LogicalAnd(lhs(), rhs()); break;
		case IR::Opcode::BitwiseOrU64: result = ir.LogicalOr(lhs(), rhs()); break;
		case IR::Opcode::BitwiseXorU64:
			result = IR::U1(ir.Emit(IR::ValueOpcode::LogicalXor, {lhs(), rhs()}));
			break;
		case IR::Opcode::BitwiseAndNotU64:
			result = ir.LogicalAnd(lhs(), ir.LogicalNot(rhs()));
			break;
		case IR::Opcode::BitwiseOrNotU64: result = ir.LogicalOr(lhs(), ir.LogicalNot(rhs())); break;
		case IR::Opcode::BitwiseNandU64: result = ir.LogicalNot(ir.LogicalAnd(lhs(), rhs())); break;
		case IR::Opcode::BitwiseNorU64: result = ir.LogicalNot(ir.LogicalOr(lhs(), rhs())); break;
		case IR::Opcode::BitwiseXnorU64:
			result = ir.LogicalNot(IR::U1(ir.Emit(IR::ValueOpcode::LogicalXor, {lhs(), rhs()})));
			break;
		case IR::Opcode::BitwiseNotU64: result = ir.LogicalNot(lhs()); break;
		default: return false;
	}
	WriteMask64(inst.dst, result);
	return true;
}

bool Translator::TranslateSimpleInteger(const IR::Instruction& inst) {
	IR::ValueOpcode opcode {};
	IR::Type        type = IR::Type::U32;
	switch (inst.op) {
		case IR::Opcode::AbsI32: opcode = IR::ValueOpcode::IAbs32; break;
		case IR::Opcode::IAddU32: opcode = IR::ValueOpcode::IAdd32; break;
		case IR::Opcode::ISubU32: opcode = IR::ValueOpcode::ISub32; break;
		case IR::Opcode::IMulU32: opcode = IR::ValueOpcode::IMul32; break;
		case IR::Opcode::UMulHighU32: opcode = IR::ValueOpcode::UMulHi; break;
		case IR::Opcode::SMulHighI32: opcode = IR::ValueOpcode::SMulHi; break;
		case IR::Opcode::IMinI32: opcode = IR::ValueOpcode::SMin32; break;
		case IR::Opcode::IMaxI32: opcode = IR::ValueOpcode::SMax32; break;
		case IR::Opcode::UMinU32: opcode = IR::ValueOpcode::UMin32; break;
		case IR::Opcode::UMaxU32: opcode = IR::ValueOpcode::UMax32; break;
		case IR::Opcode::IMin3I32: opcode = IR::ValueOpcode::SMinTri32; break;
		case IR::Opcode::IMax3I32: opcode = IR::ValueOpcode::SMaxTri32; break;
		case IR::Opcode::IMed3I32: opcode = IR::ValueOpcode::SMedTri32; break;
		case IR::Opcode::UMin3U32: opcode = IR::ValueOpcode::UMinTri32; break;
		case IR::Opcode::UMax3U32: opcode = IR::ValueOpcode::UMaxTri32; break;
		case IR::Opcode::UMed3U32: opcode = IR::ValueOpcode::UMedTri32; break;
		case IR::Opcode::BitwiseAndU32: opcode = IR::ValueOpcode::BitwiseAnd32; break;
		case IR::Opcode::BitwiseOrU32: opcode = IR::ValueOpcode::BitwiseOr32; break;
		case IR::Opcode::BitwiseXorU32: opcode = IR::ValueOpcode::BitwiseXor32; break;
		case IR::Opcode::BitwiseNotU32: opcode = IR::ValueOpcode::BitwiseNot32; break;
		case IR::Opcode::BitReverseU32: opcode = IR::ValueOpcode::BitReverse32; break;
		case IR::Opcode::BitCountU32: opcode = IR::ValueOpcode::BitCount32; break;
		case IR::Opcode::BitCountU64:
			opcode = IR::ValueOpcode::BitCount64;
			type   = IR::Type::U64;
			break;
		case IR::Opcode::FindLsbU32: opcode = IR::ValueOpcode::FindILsb32; break;
		case IR::Opcode::BitwiseAndU64:
			opcode = IR::ValueOpcode::BitwiseAnd64;
			type   = IR::Type::U64;
			break;
		case IR::Opcode::BitwiseOrU64:
			opcode = IR::ValueOpcode::BitwiseOr64;
			type   = IR::Type::U64;
			break;
		case IR::Opcode::ShiftLeftLogicalU32: opcode = IR::ValueOpcode::ShiftLeftLogical32; break;
		case IR::Opcode::ShiftRightLogicalU32: opcode = IR::ValueOpcode::ShiftRightLogical32; break;
		case IR::Opcode::ShiftRightArithmeticI32:
			opcode = IR::ValueOpcode::ShiftRightArithmetic32;
			break;
		case IR::Opcode::ShiftLeftLogicalU64:
			opcode = IR::ValueOpcode::ShiftLeftLogical64;
			type   = IR::Type::U64;
			break;
		case IR::Opcode::ShiftRightLogicalU64:
			opcode = IR::ValueOpcode::ShiftRightLogical64;
			type   = IR::Type::U64;
			break;
		default: return false;
	}

	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < inst.src_count; index++) {
		const auto arg_type = IR::ArgTypeOf(opcode, index);
		args[index] = ReadOperand(inst.src[index], arg_type == IR::Type::Void ? type : arg_type);
		if (index == 1u && (opcode == IR::ValueOpcode::ShiftLeftLogical32 ||
		                    opcode == IR::ValueOpcode::ShiftRightLogical32 ||
		                    opcode == IR::ValueOpcode::ShiftRightArithmetic32)) {
			args[index] = ir.BitwiseAnd(IR::U32(args[index]), IR::U32(IR::Value(31u)));
		} else if (index == 1u && (opcode == IR::ValueOpcode::ShiftLeftLogical64 ||
		                           opcode == IR::ValueOpcode::ShiftRightLogical64 ||
		                           opcode == IR::ValueOpcode::ShiftRightArithmetic64)) {
			args[index] = ir.BitwiseAnd(IR::U32(args[index]), IR::U32(IR::Value(63u)));
		}
	}
	IR::Value result;
	switch (inst.src_count) {
		case 1: result = ir.Emit(opcode, {args[0]}); break;
		case 2: result = ir.Emit(opcode, {args[0], args[1]}); break;
		case 3: result = ir.Emit(opcode, {args[0], args[1], args[2]}); break;
		default: EXIT("invalid simple integer source count: %u", inst.src_count);
	}
	WriteOperand(inst.dst, result);
	if (type == IR::Type::U64 && inst.dst.kind == IR::OperandKind::Register &&
	    inst.dst.reg.file == IR::RegisterFile::Scalar) {
		IR::U1 mask;
		if (inst.op == IR::Opcode::BitwiseAndU64) {
			mask = ir.LogicalAnd(ReadMask(inst.src[0]), ReadMask(inst.src[1]));
		} else if (inst.op == IR::Opcode::BitwiseOrU64) {
			mask = ir.LogicalOr(ReadMask(inst.src[0]), ReadMask(inst.src[1]));
		}
		if (!mask.IsEmpty()) {
			ir.SetThreadBitScalarReg(static_cast<IR::ScalarReg>(inst.dst.reg.index), mask);
		}
	}
	return true;
}

bool Translator::TranslateComposedInteger(const IR::Instruction& inst) {
	const auto binary_u32 = [&](auto operation) {
		const auto lhs = ReadU32(inst.src[0]);
		const auto rhs = ReadU32(inst.src[1]);
		return operation(lhs, rhs);
	};
	const auto binary_u64 = [&](auto operation) {
		const auto lhs = UnpackU64(ReadU64(inst.src[0]));
		const auto rhs = UnpackU64(ReadU64(inst.src[1]));
		return ir.PackUint2x32(operation(lhs[0], rhs[0]), operation(lhs[1], rhs[1]));
	};

	IR::Value result;
	switch (inst.op) {
		case IR::Opcode::BitwiseAndNotU32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseAnd(lhs, ir.BitwiseNot(rhs)); });
			break;
		case IR::Opcode::BitwiseOrNotU32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseOr(lhs, ir.BitwiseNot(rhs)); });
			break;
		case IR::Opcode::BitwiseNandU32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseAnd(lhs, rhs)); });
			break;
		case IR::Opcode::BitwiseNorU32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseOr(lhs, rhs)); });
			break;
		case IR::Opcode::BitwiseXnorU32:
			result = binary_u32(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseXor(lhs, rhs)); });
			break;
		case IR::Opcode::BitwiseXorU64:
			result = binary_u64([&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseXor(lhs, rhs); });
			break;
		case IR::Opcode::BitwiseAndNotU64:
			result = binary_u64(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseAnd(lhs, ir.BitwiseNot(rhs)); });
			break;
		case IR::Opcode::BitwiseOrNotU64:
			result = binary_u64(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseOr(lhs, ir.BitwiseNot(rhs)); });
			break;
		case IR::Opcode::BitwiseNandU64:
			result = binary_u64(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseAnd(lhs, rhs)); });
			break;
		case IR::Opcode::BitwiseNorU64:
			result = binary_u64(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseOr(lhs, rhs)); });
			break;
		case IR::Opcode::BitwiseXnorU64:
			result = binary_u64(
			    [&](IR::U32 lhs, IR::U32 rhs) { return ir.BitwiseNot(ir.BitwiseXor(lhs, rhs)); });
			break;
		case IR::Opcode::BitwiseNotU64: {
			const auto source = UnpackU64(ReadU64(inst.src[0]));
			result            = ir.PackUint2x32(ir.BitwiseNot(source[0]), ir.BitwiseNot(source[1]));
			break;
		}
		case IR::Opcode::BitwiseAndOrU32: {
			const auto lhs = ReadU32(inst.src[0]);
			const auto rhs = ReadU32(inst.src[1]);
			const auto add = ReadU32(inst.src[2]);
			result         = ir.BitwiseOr(ir.BitwiseAnd(lhs, rhs), add);
			break;
		}
		case IR::Opcode::BitwiseOr3U32:
			result = ir.BitwiseOr(ir.BitwiseOr(ReadU32(inst.src[0]), ReadU32(inst.src[1])),
			                      ReadU32(inst.src[2]));
			break;
		case IR::Opcode::BitwiseXor3U32:
			result = ir.BitwiseXor(ir.BitwiseXor(ReadU32(inst.src[0]), ReadU32(inst.src[1])),
			                       ReadU32(inst.src[2]));
			break;
		case IR::Opcode::FindLsbU64: {
			const auto source        = UnpackU64(ReadU64(inst.src[0]));
			const auto low_lsb       = IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {source[0]}));
			const auto high_lsb      = IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {source[1]}));
			const auto high_position = ir.IAdd(high_lsb, IR::U32(IR::Value(32u)));
			result = ir.Select(ir.INotEqual(source[0], IR::U32(IR::Value(0u))), low_lsb,
			                   ir.Select(ir.INotEqual(source[1], IR::U32(IR::Value(0u))),
			                             high_position, IR::U32(IR::Value(0xffffffffu))));
			break;
		}
		case IR::Opcode::FindMsbFromHighU32: {
			const auto source   = ReadU32(inst.src[0]);
			const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {source}));
			const auto position = ir.ISub(IR::U32(IR::Value(31u)), msb);
			result              = ir.Select(ir.INotEqual(source, IR::U32(IR::Value(0u))), position,
			                                IR::U32(IR::Value(0xffffffffu)));
			break;
		}
		case IR::Opcode::FindMsbFromHighU64: {
			const auto source   = ReadU64(inst.src[0]);
			const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb64, {source}));
			const auto position = ir.ISub(IR::U32(IR::Value(63u)), msb);
			const auto nonzero  = IR::U1(
			    ir.Emit(IR::ValueOpcode::INotEqual64,
			            {source, ir.PackUint2x32(IR::U32(IR::Value(0u)), IR::U32(IR::Value(0u)))}));
			result = ir.Select(nonzero, position, IR::U32(IR::Value(0xffffffffu)));
			break;
		}
		default: return false;
	}
	WriteOperand(inst.dst, result);
	if (result.GetType() == IR::Type::U64 && inst.dst.kind == IR::OperandKind::Register &&
	    inst.dst.reg.file == IR::RegisterFile::Scalar) {
		const auto lhs = ReadMask(inst.src[0]);
		IR::U1     mask;
		switch (inst.op) {
			case IR::Opcode::BitwiseXorU64:
				mask = IR::U1(ir.Emit(IR::ValueOpcode::LogicalXor, {lhs, ReadMask(inst.src[1])}));
				break;
			case IR::Opcode::BitwiseAndNotU64:
				mask = ir.LogicalAnd(lhs, ir.LogicalNot(ReadMask(inst.src[1])));
				break;
			case IR::Opcode::BitwiseOrNotU64:
				mask = ir.LogicalOr(lhs, ir.LogicalNot(ReadMask(inst.src[1])));
				break;
			case IR::Opcode::BitwiseNandU64:
				mask = ir.LogicalNot(ir.LogicalAnd(lhs, ReadMask(inst.src[1])));
				break;
			case IR::Opcode::BitwiseNorU64:
				mask = ir.LogicalNot(ir.LogicalOr(lhs, ReadMask(inst.src[1])));
				break;
			case IR::Opcode::BitwiseXnorU64:
				mask = ir.LogicalNot(
				    IR::U1(ir.Emit(IR::ValueOpcode::LogicalXor, {lhs, ReadMask(inst.src[1])})));
				break;
			case IR::Opcode::BitwiseNotU64: mask = ir.LogicalNot(lhs); break;
			default: break;
		}
		if (!mask.IsEmpty()) {
			ir.SetThreadBitScalarReg(static_cast<IR::ScalarReg>(inst.dst.reg.index), mask);
		}
	}
	return true;
}

bool Translator::TranslateExtendedInteger(const IR::Instruction& inst) {
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
	const auto select_u64 = [&](IR::U1 condition, IR::U64 true_value, IR::U64 false_value) {
		const auto lhs = UnpackU64(true_value);
		const auto rhs = UnpackU64(false_value);
		return ir.PackUint2x32(ir.Select(condition, lhs[0], rhs[0]),
		                       ir.Select(condition, lhs[1], rhs[1]));
	};
	const auto right_mask64 = [&](IR::U32 count) {
		const auto below32    = IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {count, imm(32)}));
		const auto above32    = IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThan32, {count, imm(32)}));
		const auto low_count  = ir.Select(below32, count, imm(32));
		const auto high_count = ir.Select(above32, ir.ISub(count, imm(32)), imm(0));
		return ir.PackUint2x32(right_mask32(low_count), right_mask32(high_count));
	};

	IR::Value result;
	switch (inst.op) {
		case IR::Opcode::IMadI24U32:
		case IR::Opcode::UMadU24U32:
		case IR::Opcode::IMulI24U32:
		case IR::Opcode::UMulU24U32: {
			const bool sign =
			    inst.op == IR::Opcode::IMadI24U32 || inst.op == IR::Opcode::IMulI24U32;
			const auto lhs   = extract(ReadU32(inst.src[0]), imm(0), imm(24), sign);
			const auto rhs   = extract(ReadU32(inst.src[1]), imm(0), imm(24), sign);
			auto       value = ir.IMul(lhs, rhs);
			if (inst.src_count == 3) {
				value = ir.IAdd(value, ReadU32(inst.src[2]));
			}
			result = value;
			break;
		}
		case IR::Opcode::UMadU64U32: {
			const auto lhs       = ReadU32(inst.src[0]);
			const auto rhs       = ReadU32(inst.src[1]);
			const auto add       = UnpackU64(ReadU64(inst.src[2]));
			const auto mul_low   = ir.IMul(lhs, rhs);
			const auto mul_high  = IR::U32(ir.Emit(IR::ValueOpcode::UMulHi, {lhs, rhs}));
			const auto low       = ir.IAdd(mul_low, add[0]);
			const auto carry_low = ir.ULessThan(low, mul_low);
			const auto high0     = ir.IAdd(mul_high, add[1]);
			const auto carry0    = ir.ULessThan(high0, mul_high);
			const auto high      = ir.IAdd(high0, ir.Select(carry_low, imm(1), imm(0)));
			const auto carry1    = ir.ULessThan(high, high0);
			WriteOperand(inst.dst, ir.PackUint2x32(low, high));
			if (inst.dst2.kind != IR::OperandKind::Null) {
				WriteMask(inst.dst2, ir.LogicalOr(carry0, carry1));
			}
			return true;
		}
		case IR::Opcode::SadU32: {
			const auto lhs = ReadU32(inst.src[0]);
			const auto rhs = ReadU32(inst.src[1]);
			const auto lo  = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {lhs, rhs}));
			const auto hi  = IR::U32(ir.Emit(IR::ValueOpcode::UMax32, {lhs, rhs}));
			result         = ir.IAdd(ir.ISub(hi, lo), ReadU32(inst.src[2]));
			break;
		}
		case IR::Opcode::IAdd3U32:
			result =
			    ir.IAdd(ir.IAdd(ReadU32(inst.src[0]), ReadU32(inst.src[1])), ReadU32(inst.src[2]));
			break;
		case IR::Opcode::BitClearU32:
		case IR::Opcode::BitSetU32: {
			const auto bit = ir.ShiftLeftLogical(imm(1), mask(ReadU32(inst.src[1]), 31u));
			result = inst.op == IR::Opcode::BitClearU32
			             ? IR::Value(ir.BitwiseAnd(ReadU32(inst.src[0]), ir.BitwiseNot(bit)))
			             : IR::Value(ir.BitwiseOr(ReadU32(inst.src[0]), bit));
			break;
		}
		case IR::Opcode::BitCountAddU32:
			result = ir.IAdd(IR::U32(ir.Emit(IR::ValueOpcode::BitCount32, {ReadU32(inst.src[0])})),
			                 ReadU32(inst.src[1]));
			break;
		case IR::Opcode::MaskedBitCountLowU32:
		case IR::Opcode::MaskedBitCountHighU32: {
			const auto lane  = IR::U32(ir.Emit(IR::ValueOpcode::LaneId));
			const auto local = mask(lane, 31u);
			const auto below = ir.ISub(ir.ShiftLeftLogical(imm(1), local), imm(1));
			const auto high_lane =
			    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThanEqual32, {lane, imm(32)}));
			const auto thread_mask = inst.op == IR::Opcode::MaskedBitCountLowU32
			                             ? ir.Select(high_lane, imm(0xffffffffu), below)
			                             : ir.Select(high_lane, below, imm(0));
			const auto active      = ir.BitwiseAnd(ReadU32(inst.src[0]), thread_mask);
			result = ir.IAdd(IR::U32(ir.Emit(IR::ValueOpcode::BitCount32, {active})),
			                 ReadU32(inst.src[1]));
			break;
		}
		case IR::Opcode::BitReplicateB64B32: {
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
			const auto source = ReadU32(inst.src[0]);
			result            = ir.PackUint2x32(replicate(mask(source, 0xffffu)),
			                                    replicate(ir.ShiftRightLogical(source, imm(16))));
			break;
		}
		case IR::Opcode::BitFieldMaskU32: {
			const auto count  = mask(ReadU32(inst.src[0]), 31u);
			const auto offset = mask(ReadU32(inst.src[1]), 31u);
			result =
			    ir.Emit(IR::ValueOpcode::BitFieldInsert, {imm(0), imm(0xffffffffu), offset, count});
			break;
		}
		case IR::Opcode::BitFieldMaskU64: {
			const auto count  = mask(ReadU32(inst.src[0]), 63u);
			const auto offset = mask(ReadU32(inst.src[1]), 63u);
			result = ir.Emit(IR::ValueOpcode::ShiftLeftLogical64, {right_mask64(count), offset});
			break;
		}
		case IR::Opcode::BitFieldExtractU32: {
			const auto source = ReadU32(inst.src[0]);
			const auto field  = ReadU32(inst.src[1]);
			const auto offset = extract(field, imm(0), imm(5), false);
			const auto count =
			    IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {extract(field, imm(16), imm(7), false),
			                                              ir.ISub(imm(32), offset)}));
			result = ir.Emit(IR::ValueOpcode::BitFieldUExtract, {source, offset, count});
			break;
		}
		case IR::Opcode::BitFieldExtractU64: {
			const auto source    = ReadU64(inst.src[0]);
			const auto field     = ReadU32(inst.src[1]);
			const auto offset    = extract(field, imm(0), imm(6), false);
			const auto raw_count = extract(field, imm(16), imm(7), false);
			const auto available = ir.ISub(imm(64), offset);
			const auto count = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {raw_count, available}));
			const auto shifted =
			    IR::U64(ir.Emit(IR::ValueOpcode::ShiftRightLogical64, {source, offset}));
			result = ir.Emit(IR::ValueOpcode::BitwiseAnd64, {shifted, right_mask64(count)});
			break;
		}
		case IR::Opcode::BitFieldExtract3U32:
		case IR::Opcode::BitFieldExtract3I32: {
			const auto source = ReadU32(inst.src[0]);
			const auto offset = mask(ReadU32(inst.src[1]), 31u);
			const auto count =
			    IR::U32(ir.Emit(IR::ValueOpcode::UMin32,
			                    {mask(ReadU32(inst.src[2]), 31u), ir.ISub(imm(32), offset)}));
			if (inst.op == IR::Opcode::BitFieldExtract3I32) {
				result =
				    ir.BitwiseAnd(ir.ShiftRightArithmetic(source, offset), right_mask32(count));
			} else {
				result = ir.Emit(IR::ValueOpcode::BitFieldUExtract, {source, offset, count});
			}
			break;
		}
		case IR::Opcode::BitFieldInsertSelectU32: {
			const auto bits   = ReadU32(inst.src[0]);
			const auto insert = ReadU32(inst.src[1]);
			const auto base   = ReadU32(inst.src[2]);
			result =
			    ir.BitwiseOr(ir.BitwiseAnd(bits, insert), ir.BitwiseAnd(ir.BitwiseNot(bits), base));
			break;
		}
		case IR::Opcode::BitCompare0B32:
		case IR::Opcode::BitCompare1B32: {
			const auto value    = ReadU32(inst.src[0]);
			const auto bit      = extract(value, mask(ReadU32(inst.src[1]), 31u), imm(1), false);
			const auto expected = imm(inst.op == IR::Opcode::BitCompare1B32 ? 1u : 0u);
			WriteOperand(inst.dst, ir.IEqual(bit, expected));
			return true;
		}
		case IR::Opcode::AlignBitU32: {
			const auto hi          = ReadU32(inst.src[0]);
			const auto lo          = ReadU32(inst.src[1]);
			const auto shift       = mask(ReadU32(inst.src[2]), 31u);
			const auto lo_part     = ir.ShiftRightLogical(lo, shift);
			const auto hi_part_raw = ir.ShiftLeftLogical(hi, mask(ir.ISub(imm(32), shift), 31u));
			const auto hi_part     = ir.Select(ir.INotEqual(shift, imm(0)), hi_part_raw, imm(0));
			result                 = ir.BitwiseOr(lo_part, hi_part);
			break;
		}
		case IR::Opcode::AlignByteU32: {
			const auto hi           = ReadU32(inst.src[0]);
			const auto lo           = ReadU32(inst.src[1]);
			const auto byte_offset  = mask(ReadU32(inst.src[2]), 31u);
			const auto bit_offset   = ir.ShiftLeftLogical(byte_offset, imm(3));
			const auto concatenated = ir.PackUint2x32(lo, hi);
			const auto shifted      = IR::U64(ir.Emit(IR::ValueOpcode::ShiftRightLogical64,
			                                          {concatenated, mask(bit_offset, 63u)}));
			const auto in_range =
			    IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {byte_offset, imm(8)}));
			result = ir.Select(in_range, UnpackU64(shifted)[0], imm(0));
			break;
		}
		case IR::Opcode::ShiftLeftAddU32:
			result =
			    ir.IAdd(ir.ShiftLeftLogical(ReadU32(inst.src[0]), mask(ReadU32(inst.src[1]), 31u)),
			            ReadU32(inst.src[2]));
			break;
		case IR::Opcode::AddShiftLeftU32:
			result = ir.ShiftLeftLogical(ir.IAdd(ReadU32(inst.src[0]), ReadU32(inst.src[1])),
			                             mask(ReadU32(inst.src[2]), 31u));
			break;
		case IR::Opcode::XorAddU32:
			result = ir.IAdd(ir.BitwiseXor(ReadU32(inst.src[0]), ReadU32(inst.src[1])),
			                 ReadU32(inst.src[2]));
			break;
		case IR::Opcode::ShiftLeftOrU32:
			result = ir.BitwiseOr(
			    ir.ShiftLeftLogical(ReadU32(inst.src[0]), mask(ReadU32(inst.src[1]), 31u)),
			    ReadU32(inst.src[2]));
			break;
		case IR::Opcode::SelectU32:
			result =
			    ir.Select(ReadCondition(inst.src[0]), ReadU32(inst.src[1]), ReadU32(inst.src[2]));
			break;
		case IR::Opcode::SelectMaskU32:
			result = ir.Select(ReadMask(inst.src[0]), ReadU32(inst.src[1]), ReadU32(inst.src[2]));
			break;
		case IR::Opcode::SelectF32Bits:
		case IR::Opcode::SelectMaskF32Bits: {
			const auto condition = inst.op == IR::Opcode::SelectMaskF32Bits
			                           ? ReadMask(inst.src[0])
			                           : ReadCondition(inst.src[0]);
			result               = ir.Emit(IR::ValueOpcode::SelectF32,
			                               {condition, ReadOperand(inst.src[1], IR::Type::F32),
			                                ReadOperand(inst.src[2], IR::Type::F32)});
			break;
		}
		case IR::Opcode::SelectU64:
			result =
			    select_u64(ReadCondition(inst.src[0]), ReadU64(inst.src[1]), ReadU64(inst.src[2]));
			break;
		case IR::Opcode::PackLowLowU16:
		case IR::Opcode::PackLowHighU16:
		case IR::Opcode::PackHighHighU16:
		case IR::Opcode::PackU16U32: {
			const bool high0 = inst.op == IR::Opcode::PackHighHighU16;
			const bool high1 = inst.op == IR::Opcode::PackLowHighU16 || high0;
			const auto lo =
			    high0 ? ir.ShiftRightLogical(ReadU32(inst.src[0]), imm(16)) : ReadU32(inst.src[0]);
			const auto hi =
			    high1 ? ir.ShiftRightLogical(ReadU32(inst.src[1]), imm(16)) : ReadU32(inst.src[1]);
			result =
			    ir.BitwiseOr(mask(lo, 0xffffu), ir.ShiftLeftLogical(mask(hi, 0xffffu), imm(16)));
			break;
		}
		default: return false;
	}
	WriteOperand(inst.dst, result);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
