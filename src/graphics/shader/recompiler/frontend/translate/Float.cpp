#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>
#include <bit>
#include <cstdlib>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// KYTY_NATIVE_F16=0 evaluates F16 arithmetic per lane in F32 instead of natively.
bool NativeF16Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_NATIVE_F16");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

namespace {

IR::ValueOpcode NativeScalarF16Opcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::FPAdd32: return IR::ValueOpcode::FPAdd16;
		case IR::ValueOpcode::FPSub32: return IR::ValueOpcode::FPSub16;
		case IR::ValueOpcode::FPMul32: return IR::ValueOpcode::FPMul16;
		case IR::ValueOpcode::FPFma32: return IR::ValueOpcode::FPFma16;
		case IR::ValueOpcode::FPMin32: return IR::ValueOpcode::FPMin16;
		case IR::ValueOpcode::FPMax32: return IR::ValueOpcode::FPMax16;
		default: return IR::ValueOpcode::Identity;
	}
}

IR::ValueOpcode NativePackedF16Opcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::FPAdd32: return IR::ValueOpcode::PackedFPAdd16x2;
		case IR::ValueOpcode::FPMul32: return IR::ValueOpcode::PackedFPMul16x2;
		case IR::ValueOpcode::FPFma32: return IR::ValueOpcode::PackedFPFma16x2;
		case IR::ValueOpcode::FPMin32: return IR::ValueOpcode::PackedFPMin16x2;
		case IR::ValueOpcode::FPMax32: return IR::ValueOpcode::PackedFPMax16x2;
		default: return IR::ValueOpcode::Identity;
	}
}

} // namespace

IR::U32 Translator::ReadPackedF16(const Decoder::Operand& operand) {
	if (operand.sdwa_sel == 6u && operand.kind != Decoder::OperandKind::FloatInlineConstant) {
		// A register: OP_SEL picks each lane's half, a bit insert at most.
		const auto raw    = ApplyBitSourceModifiers(operand, ReadRawU32(operand));
		const auto insert = [&](IR::U32 base, IR::U32 half, uint32_t offset) {
			return IR::U32(ir.Emit(IR::ValueOpcode::BitFieldInsert,
			                       {base, half, IR::Value(offset), IR::Value(16u)}));
		};
		const auto high = [&] { return ir.ShiftRightLogical(raw, IR::U32(IR::Value(16u))); };
		auto       bits = raw;
		if (operand.op_sel && !operand.op_sel_hi) {
			bits = ir.BitwiseOr(high(), ir.ShiftLeftLogical(raw, IR::U32(IR::Value(16u))));
		} else if (!operand.op_sel && !operand.op_sel_hi) {
			bits = insert(raw, raw, 16u);
		} else if (operand.op_sel && operand.op_sel_hi) {
			bits = insert(raw, high(), 0u);
		}
		if (operand.absolute) {
			bits = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x7fff7fffu)));
		}
		const auto sign = (operand.negate ? 0x8000u : 0u) | (operand.negate_hi ? 0x80000000u : 0u);
		if (sign != 0u) {
			bits = ir.BitwiseXor(bits, IR::U32(IR::Value(sign)));
		}
		return bits;
	}
	const auto low  = Read16LaneBits(operand, false);
	const auto high = Read16LaneBits(operand, true);
	return ir.BitwiseOr(low, ir.ShiftLeftLogical(high, IR::U32(IR::Value(16u))));
}

void Translator::PackedFloat16(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                               bool accumulator, bool quiet_snan) {
	const auto native = NativePackedF16Opcode(opcode);
	if (NativeF16Enabled() && native != IR::ValueOpcode::Identity && inst.dst.omod == 0u &&
	    !inst.dst.clamp) {
		// Guest F16 add, mul, min and max round once to F16, which the native lanes match;
		// FMA rounds once as well, where the F32 evaluation could round twice.
		const auto lhs = ReadPackedF16(inst.src0);
		const auto rhs = ReadPackedF16(inst.src1);
		IR::U32    result;
		if (accumulator) {
			result = IR::U32(ir.Emit(native, {lhs, rhs, ReadPackedF16(inst.dst)}));
		} else if (inst.src_count == 3u) {
			result = IR::U32(ir.Emit(native, {lhs, rhs, ReadPackedF16(inst.src2)}));
		} else {
			result = IR::U32(ir.Emit(native, {lhs, rhs}));
		}
		auto raw = DestinationOperand(inst);
		if (quiet_snan) {
			result = QuietPackedSnan(inst, result);
		}
		WriteOperand(raw, result);
		return;
	}
	const auto translate_lane = [&](bool high) {
		const auto lhs = ReadF16LaneAsF32(inst.src0, high);
		const auto rhs = ReadF16LaneAsF32(inst.src1, high);
		IR::F32    result;
		if (accumulator) {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs, ReadF16LaneAsF32(inst.dst, high)}));
		} else if (inst.src_count == 3u) {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs, ReadF16LaneAsF32(inst.src2, high)}));
		} else {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs}));
		}
		return ApplyF32ResultModifiers(inst.dst, result);
	};
	auto raw    = DestinationOperand(inst);
	raw.omod    = 0u;
	raw.clamp   = false;
	auto result = PackHalf2x16(translate_lane(false), translate_lane(true));
	if (quiet_snan) {
		result = QuietPackedSnan(inst, result);
	}
	WriteOperand(raw, result);
}

IR::U32 Translator::QuietPackedSnan(const Decoder::Instruction& inst, IR::U32 result) {
	const auto quiet_snan_lane = [&](const Decoder::Operand& operand, bool high) {
		const auto bits     = ReadU16LaneAsU32(operand, high, false);
		const auto exponent = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x7e00u)));
		const auto payload  = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x01ffu)));
		const auto snan     = ir.LogicalAnd(ir.IEqual(exponent, IR::U32(IR::Value(0x7c00u))),
		                                    ir.INotEqual(payload, IR::U32(IR::Value(0u))));
		return std::pair {snan, ir.BitwiseOr(bits, IR::U32(IR::Value(0x0200u)))};
	};
	const auto override_lane = [&](bool high) {
		const auto [lhs_snan, lhs_quiet] = quiet_snan_lane(inst.src0, high);
		const auto [rhs_snan, rhs_quiet] = quiet_snan_lane(inst.src1, high);
		const auto normal = high ? ir.ShiftRightLogical(result, IR::U32(IR::Value(16u)))
		                         : ir.BitwiseAnd(result, IR::U32(IR::Value(0xffffu)));
		return ir.Select(lhs_snan, lhs_quiet, ir.Select(rhs_snan, rhs_quiet, normal));
	};
	return PackU16Lanes(override_lane(false), override_lane(true));
}

void Translator::Float16Unary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                              bool invalid_negative) {
	const auto argument = ReadF16AsF32(inst.src0);
	auto       result   = IR::F32(ir.Emit(opcode, {argument}));
	if (!invalid_negative) {
		WriteF16(DestinationOperand(inst), result);
		return;
	}
	const auto negative =
	    IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {argument, IR::Value::F32(0.0f)}));
	result             = ApplyF32ResultModifiers(inst.dst, result);
	const auto bits    = PackHalf2x16(result, IR::F32(IR::Value::F32(0.0f)));
	const auto invalid = IR::U32(IR::Value(inst.dst.clamp ? 0u : 0xfe00u));
	Write16Bits(DestinationOperand(inst), ir.Select(negative, invalid, bits));
}

void Translator::Float16Trig(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
	const auto argument = ReadF16AsF32(inst.src0);
	const auto magnitude =
	    ir.BitwiseAnd(ir.BitCastU32(argument), IR::U32(IR::Value(0x7fffffffu)));
	auto result = IR::F32(ir.Emit(opcode, {argument}));
	if (opcode == IR::ValueOpcode::FPSin) {
		const auto fraction = IR::F32(ir.Emit(IR::ValueOpcode::FPFract32, {argument}));
		const auto whole = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.0f)}));
		const auto half = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.5f)}));
		const auto nonzero = ir.INotEqual(magnitude, IR::U32(IR::Value(0u)));
		const auto cardinal = ir.LogicalOr(ir.LogicalAnd(whole, nonzero), half);
		result = IR::F32(ir.Emit(
		    IR::ValueOpcode::SelectF32, {cardinal, IR::Value::F32(0.0f), result}));
	} else {
		// The architectural quarter-cycle result is exactly zero. Evaluating cos(pi/2) with a
		// rounded F32 PI can otherwise produce a half-precision subnormal instead.
		const auto fraction = IR::F32(ir.Emit(IR::ValueOpcode::FPFract32, {argument}));
		const auto quarter = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.25f)}));
		const auto three_quarters = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.75f)}));
		result = IR::F32(
		    ir.Emit(IR::ValueOpcode::SelectF32,
		            {ir.LogicalOr(quarter, three_quarters), IR::Value::F32(0.0f), result}));
	}

	const auto infinite = ir.IEqual(magnitude, IR::U32(IR::Value(0x7f800000u)));
	result              = ApplyF32ResultModifiers(inst.dst, result);
	const auto bits    = PackHalf2x16(result, IR::F32(IR::Value::F32(0.0f)));
	const auto invalid = IR::U32(IR::Value(inst.dst.clamp ? 0u : 0xfe00u));
	Write16Bits(DestinationOperand(inst), ir.Select(infinite, invalid, bits));
}

IR::F16 Translator::ReadF16Lane(const Decoder::Operand& operand, bool high_lane) {
	if (operand.kind != Decoder::OperandKind::FloatInlineConstant && operand.sdwa_sel >= 4u) {
		auto raw_operand      = operand;
		raw_operand.sdwa_sel  = 6;
		raw_operand.sdwa_sext = false;
		const auto bits = ApplyBitSourceModifiers(raw_operand, ReadRawU32(operand));
		bool       high = high_lane ? operand.op_sel_hi : operand.op_sel;
		if (operand.sdwa_sel != 6u) {
			high = operand.sdwa_sel == 5u;
		}
		auto value =
		    IR::F16(ir.Emit(IR::ValueOpcode::ExtractF16Lane, {bits, IR::Value(high ? 1u : 0u)}));
		if (operand.absolute) {
			value = IR::F16(ir.Emit(IR::ValueOpcode::FPAbs16, {value}));
		}
		if (high_lane ? operand.negate_hi : operand.negate) {
			value = IR::F16(ir.Emit(IR::ValueOpcode::FPNeg16, {value}));
		}
		return value;
	}
	const auto bits = Read16LaneBits(operand, high_lane);
	return IR::F16(ir.Emit(IR::ValueOpcode::BitCastF16U16,
	                       {IR::U16(ir.Emit(IR::ValueOpcode::ConvertU16U32, {bits}))}));
}

bool Translator::WriteF16Lane(const Decoder::Operand& operand, IR::F16 value) {
	// The Write16Bits destination, merged as one half insert.
	auto destination = operand;
	if (!destination.explicit_sdwa_dst) {
		if (destination.sdwa_sel == 6u) {
			destination.sdwa_sel = 4u;
		}
		destination.sdwa_dst_unused = 2u;
	}
	if ((destination.sdwa_sel != 4u && destination.sdwa_sel != 5u) ||
	    destination.sdwa_dst_unused != 2u ||
	    (destination.kind != Decoder::OperandKind::Vgpr &&
	     destination.kind != Decoder::OperandKind::Sgpr)) {
		return false;
	}
	destination.omod   = 0u;
	destination.op_sel = false;
	destination.clamp  = false;
	const auto old     = ReadRawU32(PlainOperand(destination));
	const auto merged  = IR::U32(ir.Emit(IR::ValueOpcode::InsertF16Lane,
	                                     {old, value, IR::Value(destination.sdwa_sel == 5u ? 1u : 0u)}));
	destination.sdwa_sel = 6u;
	WriteRawU32(destination, merged);
	return true;
}

void Translator::Float16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                               bool reverse) {
	const auto native      = NativeScalarF16Opcode(opcode);
	const auto destination = DestinationOperand(inst);
	if (NativeF16Enabled() && native != IR::ValueOpcode::Identity && destination.omod == 0u &&
	    !destination.clamp) {
		const auto lhs    = ReadF16Lane(reverse ? inst.src1 : inst.src0, false);
		const auto rhs    = ReadF16Lane(reverse ? inst.src0 : inst.src1, false);
		const auto result = IR::F16(ir.Emit(native, {lhs, rhs}));
		if (!WriteF16Lane(destination, result)) {
			Write16Bits(destination, IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16,
			                                         {IR::U16(ir.Emit(IR::ValueOpcode::BitCastU16F16,
			                                                          {result}))})));
		}
		return;
	}
	const auto lhs = ReadF16AsF32(reverse ? inst.src1 : inst.src0);
	const auto rhs = ReadF16AsF32(reverse ? inst.src0 : inst.src1);
	WriteF16(DestinationOperand(inst), IR::F32(ir.Emit(opcode, {lhs, rhs})));
}

void Translator::Float16Ternary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                bool accumulator, bool mix) {
	const auto native      = NativeScalarF16Opcode(opcode);
	const auto destination = DestinationOperand(inst);
	if (NativeF16Enabled() && !mix && native == IR::ValueOpcode::FPFma16 &&
	    destination.omod == 0u && !destination.clamp) {
		std::array<IR::Value, 3> halves;
		for (uint32_t index = 0; index < halves.size(); index++) {
			halves[index] =
			    ReadF16Lane(accumulator && index == 2u ? inst.dst : SourceAt(inst, index), false);
		}
		const auto result = IR::F16(ir.Emit(native, {halves[0], halves[1], halves[2]}));
		if (!WriteF16Lane(destination, result)) {
			Write16Bits(destination, IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16,
			                                         {IR::U16(ir.Emit(IR::ValueOpcode::BitCastU16F16,
			                                                          {result}))})));
		}
		return;
	}
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = accumulator && index == 2u ? inst.dst : SourceAt(inst, index);
		args[index] = mix ? IR::Value(ReadMixF32(operand)) : IR::Value(ReadF16AsF32(operand));
	}
	WriteF16(DestinationOperand(inst), IR::F32(ir.Emit(opcode, {args[0], args[1], args[2]})));
}

void Translator::FloatUnary(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
	const auto type = IR::ArgTypeOf(opcode, 0);
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {ReadOperand(inst.src0, type)}));
}

void Translator::V_FRACT(const Decoder::Instruction& inst, IR::Type type) {
	// x - floor(x) rounds to 1.0 for a tiny negative x; v_fract returns the largest value below
	// one instead. The ordered compare lets NaN through.
	const auto destination = DestinationOperand(inst);
	if (type == IR::Type::F64) {
		const auto result    = ir.Emit(IR::ValueOpcode::FPFract64, {ReadOperand(inst.src0, type)});
		const auto below_one = ir.Emit(IR::ValueOpcode::BitCastF64U64,
		                               {IR::Value(uint64_t {0x3fefffffffffffff})});
		const auto above     = ir.Emit(IR::ValueOpcode::FPOrdGreaterThan64, {result, below_one});
		WriteOperand(destination, ir.Emit(IR::ValueOpcode::SelectF64, {above, below_one, result}));
		return;
	}
	const bool half   = type == IR::Type::F16;
	const auto source = half ? ReadF16AsF32(inst.src0) : IR::F32(ReadOperand(inst.src0, type));
	const auto result = IR::F32(ir.Emit(IR::ValueOpcode::FPFract32, {source}));
	// Clamp in the destination precision: 0x3f7fe000 is the half 0x3bff.
	const auto below_one =
	    IR::F32(IR::Value::F32(std::bit_cast<float>(half ? 0x3f7fe000u : 0x3f7fffffu)));
	const auto above   = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThan32, {result, below_one}));
	const auto clamped = SelectF32(above, below_one, result);
	if (half) {
		WriteF16(destination, clamped);
	} else {
		WriteOperand(destination, clamped);
	}
}

void Translator::FloatBinary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                             bool reverse) {
	std::array<IR::Value, 2> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = SourceAt(inst, reverse ? 1u - index : index);
		args[index]        = ReadOperand(operand, IR::ArgTypeOf(opcode, index));
	}
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {args[0], args[1]}));
}

void Translator::FloatTernary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                              bool accumulator, bool mix) {
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = accumulator && index == 2u ? inst.dst : SourceAt(inst, index);
		const auto type    = IR::ArgTypeOf(opcode, index);
		args[index]        = type == IR::Type::F32 && mix ? IR::Value(ReadMixF32(operand))
		                                                  : ReadOperand(operand, type);
	}
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {args[0], args[1], args[2]}));
}

void Translator::V_FREXP_MANT_F32(const Decoder::Instruction& inst) {
	const auto bits = ReadU32(inst.src0);
	const auto exponent =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {bits, IR::Value(23u), IR::Value(8u)}));
	const auto mantissa = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x007fffffu)));
	const auto sign     = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x80000000u)));
	const auto base     = ir.BitwiseOr(sign, IR::U32(IR::Value(0x3f000000u)));
	const auto normal   = ir.BitwiseOr(base, mantissa);
	const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {mantissa}));
	const auto shift    = ir.ISub(IR::U32(IR::Value(23u)), msb);
	const auto fraction =
	    ir.BitwiseAnd(ir.ShiftLeftLogical(mantissa, shift), IR::U32(IR::Value(0x007fffffu)));
	const auto subnormal = ir.BitwiseOr(base, fraction);
	const auto zero      = ir.IEqual(mantissa, IR::U32(IR::Value(0u)));
	const auto finite    = ir.Select(ir.INotEqual(exponent, IR::U32(IR::Value(0u))), normal,
	                                 ir.Select(zero, bits, subnormal));
	const auto result    = ir.Select(ir.IEqual(exponent, IR::U32(IR::Value(0xffu))), bits, finite);
	WriteOperand(DestinationOperand(inst), ir.BitCastF32(result));
}

void Translator::V_DOT2C_F32_F16(const Decoder::Instruction& inst) {
	auto a          = inst.src0;
	a.op_sel        = false;
	a.op_sel_hi     = true;
	auto b          = inst.src1;
	b.op_sel        = false;
	b.op_sel_hi     = true;
	const auto a_lo = ReadF16LaneAsF32(a, false);
	const auto a_hi = ReadF16LaneAsF32(a, true);
	const auto b_lo = ReadF16LaneAsF32(b, false);
	const auto b_hi = ReadF16LaneAsF32(b, true);
	const auto acc  = ReadMixF32(inst.dst);
	const auto lo   = IR::F32(ir.Emit(IR::ValueOpcode::FPFma32, {a_lo, b_lo, acc}));
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::FPFma32, {a_hi, b_hi, lo}));
}

void Translator::V_CUBEID_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 0u);
}

void Translator::V_CUBESC_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 1u);
}

void Translator::V_CUBETC_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 2u);
}

void Translator::V_CUBEMA_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 3u);
}

void Translator::FloatCube(const Decoder::Instruction& inst, uint32_t result_kind) {
	const auto x  = ReadMixF32(inst.src0);
	const auto y  = ReadMixF32(inst.src1);
	const auto z  = ReadMixF32(inst.src2);
	const auto nx = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {x}));
	const auto ny = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {y}));
	const auto nz = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {z}));
	const auto ax = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {x}));
	const auto ay = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {y}));
	const auto az = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {z}));
	const auto z_face =
	    ir.LogicalAnd(IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {az, ax})),
	                  IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {az, ay})));
	const auto y_face = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {ay, ax}));
	const auto x_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {x, IR::Value::F32(0.0f)}));
	const auto y_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {y, IR::Value::F32(0.0f)}));
	const auto z_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {z, IR::Value::F32(0.0f)}));
	const auto select_face = [&](IR::F32 x_value, IR::F32 y_value, IR::F32 z_value) {
		return SelectF32(z_face, z_value, SelectF32(y_face, y_value, x_value));
	};
	IR::F32 result;
	switch (result_kind) {
		case 0u:
			result = select_face(
			    SelectF32(x_neg, IR::F32(IR::Value::F32(1.0f)), IR::F32(IR::Value::F32(0.0f))),
			    SelectF32(y_neg, IR::F32(IR::Value::F32(3.0f)), IR::F32(IR::Value::F32(2.0f))),
			    SelectF32(z_neg, IR::F32(IR::Value::F32(5.0f)), IR::F32(IR::Value::F32(4.0f))));
			break;
		case 1u: result = select_face(SelectF32(x_neg, z, nz), x, SelectF32(z_neg, nx, x)); break;
		case 2u: result = select_face(ny, SelectF32(y_neg, nz, z), ny); break;
		case 3u:
			result = IR::F32(
			    ir.Emit(IR::ValueOpcode::FPMul32, {select_face(x, y, z), IR::Value::F32(2.0f)}));
			break;
		default: EXIT("invalid cube result kind");
	}
	WriteOperand(DestinationOperand(inst), result);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
