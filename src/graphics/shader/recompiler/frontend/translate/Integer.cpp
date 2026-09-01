#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::Integer16Shift(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                bool arithmetic) {
	const auto value  = ReadU16AsU32(inst.src1, arithmetic);
	const auto count  = ir.BitwiseAnd(ReadU16AsU32(inst.src0, false), IR::U32(IR::Value(15u)));
	const auto result = IR::U32(ir.Emit(opcode, {value, count}));
	WriteU16(DestinationOperand(inst), ir.BitwiseAnd(result, IR::U32(IR::Value(0xffffu))));
	return true;
}

bool Translator::Integer16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                 bool sign) {
	const auto lhs    = ReadU16AsU32(inst.src0, sign);
	const auto rhs    = ReadU16AsU32(inst.src1, sign);
	const auto result = IR::U32(ir.Emit(opcode, {lhs, rhs}));
	WriteU16(DestinationOperand(inst), ir.BitwiseAnd(result, IR::U32(IR::Value(0xffffu))));
	return true;
}

bool Translator::V_MED3_I16(const Decoder::Instruction& inst) {
	const auto result = IR::U32(ir.Emit(
	    IR::ValueOpcode::SMedTri32, {ReadU16AsU32(inst.src0, true), ReadU16AsU32(inst.src1, true),
	                                 ReadU16AsU32(inst.src2, true)}));
	WriteU16(DestinationOperand(inst), ir.BitwiseAnd(result, IR::U32(IR::Value(0xffffu))));
	return true;
}

bool Translator::PackedInteger16Shift(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                      bool arithmetic) {
	const auto translate_lane = [&](bool high) {
		const auto count =
		    ir.BitwiseAnd(ReadU16LaneAsU32(inst.src0, high, false), IR::U32(IR::Value(15u)));
		const auto value = ReadU16LaneAsU32(inst.src1, high, arithmetic);
		return IR::U32(ir.Emit(opcode, {value, count}));
	};
	WriteOperand(DestinationOperand(inst),
	             PackU16Lanes(translate_lane(false), translate_lane(true)));
	return true;
}

bool Translator::PackedInteger16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
	const auto translate_lane = [&](bool high) {
		const auto lhs = ReadU16LaneAsU32(inst.src0, high, false);
		const auto rhs = ReadU16LaneAsU32(inst.src1, high, false);
		return IR::U32(ir.Emit(opcode, {lhs, rhs}));
	};
	WriteOperand(DestinationOperand(inst),
	             PackU16Lanes(translate_lane(false), translate_lane(true)));
	return true;
}

bool Translator::PackedInteger16Mad(const Decoder::Instruction& inst, bool sign) {
	const auto translate_lane = [&](bool high) {
		const auto lhs = ReadU16LaneAsU32(inst.src0, high, false);
		const auto rhs = ReadU16LaneAsU32(inst.src1, high, false);
		return ir.IAdd(ir.IMul(lhs, rhs), ReadU16LaneAsU32(inst.src2, high, sign));
	};
	WriteOperand(DestinationOperand(inst),
	             PackU16Lanes(translate_lane(false), translate_lane(true)));
	return true;
}

bool Translator::PackedInteger16MinMax(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                       bool sign) {
	const auto translate_lane = [&](bool high) {
		const auto lhs = ReadU16LaneAsU32(inst.src0, high, sign);
		const auto rhs = ReadU16LaneAsU32(inst.src1, high, sign);
		return IR::U32(ir.Emit(opcode, {lhs, rhs}));
	};
	WriteOperand(DestinationOperand(inst),
	             PackU16Lanes(translate_lane(false), translate_lane(true)));
	return true;
}

IR::U1 Translator::U64MaskBinary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                 bool negate_rhs, bool negate_result) {
	const auto lhs = ReadMask(inst.src0);
	auto       rhs = ReadMask(inst.src1);
	if (negate_rhs) {
		rhs = ir.LogicalNot(rhs);
	}
	auto result = IR::U1(ir.Emit(opcode, {lhs, rhs}));
	return negate_result ? ir.LogicalNot(result) : result;
}

bool Translator::S_U64_MASK(const Decoder::Instruction& inst, IR::ValueOpcode logical_opcode,
                            IR::ValueOpcode bit_opcode, bool negate_rhs, bool negate_result,
                            bool unary) {
	const auto invocation_result =
	    unary ? ir.LogicalNot(ReadMask(inst.src0))
	          : U64MaskBinary(inst, logical_opcode, negate_rhs, negate_result);
	const auto is_exec_or_vcc = [](const Decoder::Operand& operand) {
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
	if (unary) {
		result = {ir.BitwiseNot(lhs[0]), ir.BitwiseNot(lhs[1])};
	} else {
		const auto rhs = ReadU32Pair(inst.src1);
		for (uint32_t component = 0; component < 2u; component++) {
			auto value = ir.Emit(
			    bit_opcode, {lhs[component], negate_rhs ? IR::Value(ir.BitwiseNot(rhs[component]))
			                                            : IR::Value(rhs[component])});
			result[component] = negate_result ? ir.BitwiseNot(IR::U32(value)) : IR::U32(value);
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

bool Translator::SimpleInteger(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                               IR::Type type, bool reverse, bool mask_shift_count,
                               bool update_scc) {
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < inst.src_count; index++) {
		const auto arg_type = IR::ArgTypeOf(opcode, index);
		const auto operand  = SourceAt(inst, reverse && index < 2u ? 1u - index : index);
		args[index]         = ReadOperand(operand, arg_type == IR::Type::Void ? type : arg_type);
		if (mask_shift_count && index == 1u) {
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
	if (update_scc) {
		if (IR::TypeOf(opcode) == IR::Type::U64) {
			ir.SetScc(
			    IR::U1(ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
		} else {
			ir.SetScc(ir.INotEqual(IR::U32(result), IR::U32(IR::Value(0u))));
		}
	}
	return true;
}

bool Translator::ComposedIntegerBinary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                       bool negate_rhs, bool negate_result, bool update_scc) {
	const auto lhs = ReadU32(inst.src0);
	auto       rhs = ReadU32(inst.src1);
	if (negate_rhs) {
		rhs = ir.BitwiseNot(rhs);
	}
	auto result = IR::U32(ir.Emit(opcode, {lhs, rhs}));
	if (negate_result) {
		result = ir.BitwiseNot(result);
	}
	WriteOperand(DestinationOperand(inst), result);
	if (update_scc) {
		ir.SetScc(ir.INotEqual(result, IR::U32(IR::Value(0u))));
	}
	return true;
}

bool Translator::V_AND_OR_B32(const Decoder::Instruction& inst) {
	const auto result =
	    ir.BitwiseOr(ir.BitwiseAnd(ReadU32(inst.src0), ReadU32(inst.src1)), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_OR3_B32(const Decoder::Instruction& inst) {
	const auto result =
	    ir.BitwiseOr(ir.BitwiseOr(ReadU32(inst.src0), ReadU32(inst.src1)), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_XOR3_B32(const Decoder::Instruction& inst) {
	const auto result =
	    ir.BitwiseXor(ir.BitwiseXor(ReadU32(inst.src0), ReadU32(inst.src1)), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::S_FF1_I32_B64(const Decoder::Instruction& inst) {
	const auto source        = ExtractU64(ReadU64(inst.src0));
	const auto low_lsb       = IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {source[0]}));
	const auto high_lsb      = IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {source[1]}));
	const auto high_position = ir.IAdd(high_lsb, IR::U32(IR::Value(32u)));
	const auto result        = ir.Select(ir.INotEqual(source[0], IR::U32(IR::Value(0u))), low_lsb,
	                                     ir.Select(ir.INotEqual(source[1], IR::U32(IR::Value(0u))),
	                                               high_position, IR::U32(IR::Value(0xffffffffu))));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_FFBH_32(const Decoder::Instruction& inst, bool sign) {
	const auto source = ReadU32(inst.src0);
	const auto value  = sign ? ir.BitwiseXor(
	                              source, ir.ShiftRightArithmetic(source, IR::U32(IR::Value(31u))))
	                         : source;
	const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {value}));
	const auto position = ir.ISub(IR::U32(IR::Value(31u)), msb);
	const auto result   = ir.Select(ir.INotEqual(value, IR::U32(IR::Value(0u))), position,
	                                IR::U32(IR::Value(0xffffffffu)));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::S_FLBIT_I32_B64(const Decoder::Instruction& inst) {
	const auto source   = ReadU64(inst.src0);
	const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb64, {source}));
	const auto position = ir.ISub(IR::U32(IR::Value(63u)), msb);
	const auto nonzero =
	    IR::U1(ir.Emit(IR::ValueOpcode::INotEqual64,
	                   {source, ir.ConstructU64(IR::U32(IR::Value(0u)), IR::U32(IR::Value(0u)))}));
	const auto result = ir.Select(nonzero, position, IR::U32(IR::Value(0xffffffffu)));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::Integer24(const Decoder::Instruction& inst, bool sign, bool addend) {
	const auto extract24 = [&](IR::U32 value) {
		return IR::U32(
		    ir.Emit(sign ? IR::ValueOpcode::BitFieldSExtract : IR::ValueOpcode::BitFieldUExtract,
		            {value, IR::Value(0u), IR::Value(24u)}));
	};
	const auto lhs    = extract24(ReadU32(inst.src0));
	const auto rhs    = extract24(ReadU32(inst.src1));
	auto       result = ir.IMul(lhs, rhs);
	if (addend) {
		result = ir.IAdd(result, ReadU32(inst.src2));
	}
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_MAD_U64_U32(const Decoder::Instruction& inst) {
	const auto lhs       = ReadU32(inst.src0);
	const auto rhs       = ReadU32(inst.src1);
	const auto add       = ExtractU64(ReadU64(inst.src2));
	const auto mul_low   = ir.IMul(lhs, rhs);
	const auto mul_high  = IR::U32(ir.Emit(IR::ValueOpcode::UMulHi, {lhs, rhs}));
	const auto low       = ir.IAdd(mul_low, add[0]);
	const auto carry_low = ir.ULessThan(low, mul_low);
	const auto high0     = ir.IAdd(mul_high, add[1]);
	const auto carry0    = ir.ULessThan(high0, mul_high);
	const auto high =
	    ir.IAdd(high0, ir.Select(carry_low, IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u))));
	const auto carry1 = ir.ULessThan(high, high0);
	WriteOperand(DestinationOperand(inst), ir.ConstructU64(low, high));
	if (inst.dst2.kind != Decoder::OperandKind::Null &&
	    inst.dst2.kind != Decoder::OperandKind::Unknown) {
		WriteMask(inst.dst2, ir.LogicalOr(carry0, carry1));
	}
	return true;
}

bool Translator::V_SAD_U32(const Decoder::Instruction& inst) {
	const auto lhs    = ReadU32(inst.src0);
	const auto rhs    = ReadU32(inst.src1);
	const auto lo     = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {lhs, rhs}));
	const auto hi     = IR::U32(ir.Emit(IR::ValueOpcode::UMax32, {lhs, rhs}));
	const auto result = ir.IAdd(ir.ISub(hi, lo), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_ADD3_U32(const Decoder::Instruction& inst) {
	const auto result =
	    ir.IAdd(ir.IAdd(ReadU32(inst.src0), ReadU32(inst.src1)), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::S_BITSET_B32(const Decoder::Instruction& inst, bool set) {
	const auto offset = ir.BitwiseAnd(ReadU32(inst.src0), IR::U32(IR::Value(31u)));
	const auto bit    = ir.ShiftLeftLogical(IR::U32(IR::Value(1u)), offset);
	const auto old    = ReadU32(inst.dst);
	const auto result = set ? ir.BitwiseOr(old, bit) : ir.BitwiseAnd(old, ir.BitwiseNot(bit));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_BCNT_U32_B32(const Decoder::Instruction& inst) {
	const auto count = IR::U32(ir.Emit(IR::ValueOpcode::BitCount32, {ReadU32(inst.src0)}));
	WriteOperand(DestinationOperand(inst), ir.IAdd(count, ReadU32(inst.src1)));
	return true;
}

bool Translator::V_MBCNT_U32_B32(const Decoder::Instruction& inst, bool low) {
	const auto lane  = IR::U32(ir.Emit(IR::ValueOpcode::LaneId));
	const auto local = ir.BitwiseAnd(lane, IR::U32(IR::Value(31u)));
	const auto below =
	    ir.ISub(ir.ShiftLeftLogical(IR::U32(IR::Value(1u)), local), IR::U32(IR::Value(1u)));
	const auto high_lane =
	    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThanEqual32, {lane, IR::Value(32u)}));
	const auto thread_mask = low ? ir.Select(high_lane, IR::U32(IR::Value(0xffffffffu)), below)
	                             : ir.Select(high_lane, below, IR::U32(IR::Value(0u)));
	const auto active      = ir.BitwiseAnd(ReadU32(inst.src0), thread_mask);
	const auto count       = IR::U32(ir.Emit(IR::ValueOpcode::BitCount32, {active}));
	WriteOperand(DestinationOperand(inst), ir.IAdd(count, ReadU32(inst.src1)));
	return true;
}

bool Translator::S_BITREPLICATE_B64_B32(const Decoder::Instruction& inst) {
	const auto replicate = [&](IR::U32 value) {
		auto bits = ir.BitwiseOr(value, ir.ShiftLeftLogical(value, IR::U32(IR::Value(8u))));
		bits      = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x00ff00ffu)));
		bits      = ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, IR::U32(IR::Value(4u))));
		bits      = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x0f0f0f0fu)));
		bits      = ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, IR::U32(IR::Value(2u))));
		bits      = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x33333333u)));
		bits      = ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, IR::U32(IR::Value(1u))));
		bits      = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x55555555u)));
		return ir.BitwiseOr(bits, ir.ShiftLeftLogical(bits, IR::U32(IR::Value(1u))));
	};
	const auto source = ReadU32(inst.src0);
	const auto low    = ir.BitwiseAnd(source, IR::U32(IR::Value(0xffffu)));
	const auto high   = ir.ShiftRightLogical(source, IR::U32(IR::Value(16u)));
	WriteOperand(DestinationOperand(inst), ir.ConstructU64(replicate(low), replicate(high)));
	return true;
}

bool Translator::S_QUADMASK_B64(const Decoder::Instruction& inst) {
	const auto compact = [&](IR::U32 value) {
		auto bits = ir.BitwiseOr(value, ir.ShiftRightLogical(value, IR::U32(IR::Value(1u))));
		bits      = ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, IR::U32(IR::Value(2u))));
		bits      = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x11111111u)));
		bits = ir.BitwiseAnd(ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, IR::U32(IR::Value(3u)))),
		                     IR::U32(IR::Value(0x03030303u)));
		bits = ir.BitwiseAnd(ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, IR::U32(IR::Value(6u)))),
		                     IR::U32(IR::Value(0x000f000fu)));
		return ir.BitwiseAnd(
		    ir.BitwiseOr(bits, ir.ShiftRightLogical(bits, IR::U32(IR::Value(12u)))),
		    IR::U32(IR::Value(0xffu)));
	};
	const auto source = ReadU32Pair(inst.src0);
	const auto quads  = ir.BitwiseOr(
	    compact(source[0]), ir.ShiftLeftLogical(compact(source[1]), IR::U32(IR::Value(8u))));
	const auto result = ir.ConstructU64(quads, IR::U32(IR::Value(0u)));
	WriteOperand(DestinationOperand(inst), result);
	ir.SetScc(IR::U1(ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
	return true;
}

bool Translator::BFM_B32(const Decoder::Instruction& inst) {
	const auto count  = ir.BitwiseAnd(ReadU32(inst.src0), IR::U32(IR::Value(31u)));
	const auto offset = ir.BitwiseAnd(ReadU32(inst.src1), IR::U32(IR::Value(31u)));
	const auto result = ir.Emit(IR::ValueOpcode::BitFieldInsert,
	                            {IR::Value(0u), IR::Value(0xffffffffu), offset, count});
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

IR::U32 Translator::RightMask32(IR::U32 count) {
	return IR::U32(ir.Emit(IR::ValueOpcode::BitFieldInsert,
	                       {IR::Value(0u), IR::Value(0xffffffffu), IR::Value(0u), count}));
}

IR::U64 Translator::RightMask64(IR::U32 count) {
	const auto below32 = IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {count, IR::Value(32u)}));
	const auto above32 = IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThan32, {count, IR::Value(32u)}));
	const auto low_count = ir.Select(below32, count, IR::U32(IR::Value(32u)));
	const auto high_count =
	    ir.Select(above32, ir.ISub(count, IR::U32(IR::Value(32u))), IR::U32(IR::Value(0u)));
	return ir.ConstructU64(RightMask32(low_count), RightMask32(high_count));
}

bool Translator::S_BFM_B64(const Decoder::Instruction& inst) {
	const auto count  = ir.BitwiseAnd(ReadU32(inst.src0), IR::U32(IR::Value(63u)));
	const auto offset = ir.BitwiseAnd(ReadU32(inst.src1), IR::U32(IR::Value(63u)));
	const auto result = ir.Emit(IR::ValueOpcode::ShiftLeftLogical64, {RightMask64(count), offset});
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::S_BFE_U32(const Decoder::Instruction& inst, bool sign) {
	const auto source = ReadU32(inst.src0);
	const auto field  = ReadU32(inst.src1);
	const auto offset =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {field, IR::Value(0u), IR::Value(5u)}));
	const auto raw_count =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {field, IR::Value(16u), IR::Value(7u)}));
	const auto count = IR::U32(
	    ir.Emit(IR::ValueOpcode::UMin32, {raw_count, ir.ISub(IR::U32(IR::Value(32u)), offset)}));
	const auto opcode =
	    sign ? IR::ValueOpcode::BitFieldSExtract : IR::ValueOpcode::BitFieldUExtract;
	const auto result = IR::U32(ir.Emit(opcode, {source, offset, count}));
	WriteOperand(DestinationOperand(inst), result);
	ir.SetScc(ir.INotEqual(result, IR::U32(IR::Value(0u))));
	return true;
}

bool Translator::S_BFE_U64(const Decoder::Instruction& inst) {
	const auto source = ReadU64(inst.src0);
	const auto field  = ReadU32(inst.src1);
	const auto offset =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {field, IR::Value(0u), IR::Value(6u)}));
	const auto raw_count =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {field, IR::Value(16u), IR::Value(7u)}));
	const auto available = ir.ISub(IR::U32(IR::Value(64u)), offset);
	const auto count     = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {raw_count, available}));
	const auto shifted   = IR::U64(ir.Emit(IR::ValueOpcode::ShiftRightLogical64, {source, offset}));
	const auto result    = ir.Emit(IR::ValueOpcode::BitwiseAnd64, {shifted, RightMask64(count)});
	WriteOperand(DestinationOperand(inst), result);
	ir.SetScc(IR::U1(ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
	return true;
}

bool Translator::V_BFE_U32(const Decoder::Instruction& inst, bool sign) {
	const auto source    = ReadU32(inst.src0);
	const auto offset    = ir.BitwiseAnd(ReadU32(inst.src1), IR::U32(IR::Value(31u)));
	const auto raw_count = ir.BitwiseAnd(ReadU32(inst.src2), IR::U32(IR::Value(31u)));
	const auto count     = IR::U32(
	    ir.Emit(IR::ValueOpcode::UMin32, {raw_count, ir.ISub(IR::U32(IR::Value(32u)), offset)}));
	const auto opcode =
	    sign ? IR::ValueOpcode::BitFieldSExtract : IR::ValueOpcode::BitFieldUExtract;
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {source, offset, count}));
	return true;
}

bool Translator::V_BFI_B32(const Decoder::Instruction& inst) {
	const auto bits   = ReadU32(inst.src0);
	const auto insert = ReadU32(inst.src1);
	const auto base   = ReadU32(inst.src2);
	const auto result =
	    ir.BitwiseOr(ir.BitwiseAnd(bits, insert), ir.BitwiseAnd(ir.BitwiseNot(bits), base));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::S_BITCMP_B32(const Decoder::Instruction& inst, bool expected) {
	const auto value  = ReadU32(inst.src0);
	const auto offset = ir.BitwiseAnd(ReadU32(inst.src1), IR::U32(IR::Value(31u)));
	const auto bit =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {value, offset, IR::Value(1u)}));
	WriteCompareResult(inst.dst, ir.IEqual(bit, IR::U32(IR::Value(expected ? 1u : 0u))));
	return true;
}

bool Translator::V_ALIGNBIT_B32(const Decoder::Instruction& inst) {
	const auto hi      = ReadU32(inst.src0);
	const auto lo      = ReadU32(inst.src1);
	const auto shift   = ir.BitwiseAnd(ReadU32(inst.src2), IR::U32(IR::Value(31u)));
	const auto lo_part = ir.ShiftRightLogical(lo, shift);
	const auto inverse =
	    ir.BitwiseAnd(ir.ISub(IR::U32(IR::Value(32u)), shift), IR::U32(IR::Value(31u)));
	const auto hi_part_raw = ir.ShiftLeftLogical(hi, inverse);
	const auto hi_part =
	    ir.Select(ir.INotEqual(shift, IR::U32(IR::Value(0u))), hi_part_raw, IR::U32(IR::Value(0u)));
	WriteOperand(DestinationOperand(inst), ir.BitwiseOr(lo_part, hi_part));
	return true;
}

bool Translator::V_ALIGNBYTE_B32(const Decoder::Instruction& inst) {
	const auto hi           = ReadU32(inst.src0);
	const auto lo           = ReadU32(inst.src1);
	const auto byte_offset  = ir.BitwiseAnd(ReadU32(inst.src2), IR::U32(IR::Value(31u)));
	const auto bit_offset   = ir.ShiftLeftLogical(byte_offset, IR::U32(IR::Value(3u)));
	const auto concatenated = ir.ConstructU64(lo, hi);
	const auto shifted =
	    IR::U64(ir.Emit(IR::ValueOpcode::ShiftRightLogical64,
	                    {concatenated, ir.BitwiseAnd(bit_offset, IR::U32(IR::Value(63u)))}));
	const auto in_range =
	    IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {byte_offset, IR::Value(8u)}));
	WriteOperand(DestinationOperand(inst),
	             ir.Select(in_range, ExtractU64(shifted)[0], IR::U32(IR::Value(0u))));
	return true;
}

bool Translator::V_LSHL_ADD_U32(const Decoder::Instruction& inst) {
	const auto shift  = ir.BitwiseAnd(ReadU32(inst.src1), IR::U32(IR::Value(31u)));
	const auto result = ir.IAdd(ir.ShiftLeftLogical(ReadU32(inst.src0), shift), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_ADD_LSHL_U32(const Decoder::Instruction& inst) {
	const auto shift  = ir.BitwiseAnd(ReadU32(inst.src2), IR::U32(IR::Value(31u)));
	const auto result = ir.ShiftLeftLogical(ir.IAdd(ReadU32(inst.src0), ReadU32(inst.src1)), shift);
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_XAD_U32(const Decoder::Instruction& inst) {
	const auto result =
	    ir.IAdd(ir.BitwiseXor(ReadU32(inst.src0), ReadU32(inst.src1)), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_LSHL_OR_B32(const Decoder::Instruction& inst) {
	const auto shift = ir.BitwiseAnd(ReadU32(inst.src1), IR::U32(IR::Value(31u)));
	const auto result =
	    ir.BitwiseOr(ir.ShiftLeftLogical(ReadU32(inst.src0), shift), ReadU32(inst.src2));
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_CNDMASK_B32(const Decoder::Instruction& inst) {
	Decoder::Operand mask_operand;
	mask_operand.kind = Decoder::OperandKind::VccLo;
	if (inst.src_count >= 3u) {
		mask_operand = inst.src2;
	}
	const auto condition = ReadMask(mask_operand);
	IR::Value  result;
	if (inst.src0.negate || inst.src0.absolute || inst.src1.negate || inst.src1.absolute) {
		result =
		    ir.Emit(IR::ValueOpcode::SelectF32, {condition, ReadOperand(inst.src1, IR::Type::F32),
		                                         ReadOperand(inst.src0, IR::Type::F32)});
	} else {
		result = ir.Select(condition, ReadU32(inst.src1), ReadU32(inst.src0));
	}
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::PackB16(const Decoder::Instruction& inst, bool high0, bool high1) {
	const auto lo        = high0 ? ir.ShiftRightLogical(ReadU32(inst.src0), IR::U32(IR::Value(16u)))
	                             : ReadU32(inst.src0);
	const auto hi        = high1 ? ir.ShiftRightLogical(ReadU32(inst.src1), IR::U32(IR::Value(16u)))
	                             : ReadU32(inst.src1);
	const auto low_bits  = ir.BitwiseAnd(lo, IR::U32(IR::Value(0xffffu)));
	const auto high_bits = ir.ShiftLeftLogical(ir.BitwiseAnd(hi, IR::U32(IR::Value(0xffffu))),
	                                           IR::U32(IR::Value(16u)));
	WriteOperand(DestinationOperand(inst), ir.BitwiseOr(low_bits, high_bits));
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
