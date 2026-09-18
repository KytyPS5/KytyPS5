#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>
#include <limits>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::PackedFloat16(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                               bool accumulator, bool quiet_snan) {
	const auto translate_lane = [&](bool high) {
		const auto lhs = ReadF16LaneAsF32(inst.src0, high, true);
		const auto rhs = ReadF16LaneAsF32(inst.src1, high, true);
		IR::F32    result;
		if (accumulator) {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs, ReadF16LaneAsF32(inst.dst, high, true)}));
		} else if (inst.src_count == 3u) {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs, ReadF16LaneAsF32(inst.src2, high, true)}));
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
		const auto quiet_snan_lane = [&](const Decoder::Operand& operand, bool high) {
			const auto bits     = ReadU16LaneAsU32(operand, high, false);
			const auto exponent = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x7c00u)));
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
		result = PackU16Lanes(override_lane(false), override_lane(true));
	}
	WriteOperand(raw, result);
	return true;
}

bool Translator::Float16Unary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                              bool invalid_negative) {
	const auto argument = ReadF16AsF32(inst.src0);
	auto       result   = IR::F32(ir.Emit(opcode, {argument}));
	if (!invalid_negative) {
		WriteF16(DestinationOperand(inst), result);
		return true;
	}
	const auto negative =
	    IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {argument, IR::Value::F32(0.0f)}));
	result             = ApplyF32ResultModifiers(inst.dst, result);
	const auto bits    = PackHalf2x16(result, IR::F32(IR::Value::F32(0.0f)));
	const auto invalid = IR::U32(IR::Value(inst.dst.clamp ? 0u : 0xfe00u));
	Write16Bits(DestinationOperand(inst), ir.Select(negative, invalid, bits));
	return true;
}

bool Translator::Float16Trig(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
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
	return true;
}

bool Translator::Float16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                               bool reverse) {
	const auto lhs = ReadF16AsF32(reverse ? inst.src1 : inst.src0);
	const auto rhs = ReadF16AsF32(reverse ? inst.src0 : inst.src1);
	WriteF16(DestinationOperand(inst), IR::F32(ir.Emit(opcode, {lhs, rhs})));
	return true;
}

bool Translator::Float16Ternary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                bool accumulator, bool mix) {
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = accumulator && index == 2u ? inst.dst : SourceAt(inst, index);
		args[index] = mix ? IR::Value(ReadMixF32(operand)) : IR::Value(ReadF16AsF32(operand));
	}
	WriteF16(DestinationOperand(inst), IR::F32(ir.Emit(opcode, {args[0], args[1], args[2]})));
	return true;
}

bool Translator::FloatUnary(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
	const auto type = IR::ArgTypeOf(opcode, 0);
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {ReadOperand(inst.src0, type)}));
	return true;
}

bool Translator::FloatBinary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                             bool reverse) {
	std::array<IR::Value, 2> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = SourceAt(inst, reverse ? 1u - index : index);
		args[index]        = ReadOperand(operand, IR::ArgTypeOf(opcode, index));
	}
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {args[0], args[1]}));
	return true;
}

bool Translator::FloatTernary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                              bool accumulator, bool mix) {
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = accumulator && index == 2u ? inst.dst : SourceAt(inst, index);
		const auto type    = IR::ArgTypeOf(opcode, index);
		args[index]        = type == IR::Type::F32 && mix ? IR::Value(ReadMixF32(operand))
		                                                  : ReadOperand(operand, type);
	}
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {args[0], args[1], args[2]}));
	return true;
}

bool Translator::V_FREXP_MANT_F32(const Decoder::Instruction& inst) {
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
	return true;
}

bool Translator::V_DOT2C_F32_F16(const Decoder::Instruction& inst) {
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
	return true;
}

bool Translator::V_CUBEID_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 0u);
}

bool Translator::V_CUBESC_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 1u);
}

bool Translator::V_CUBETC_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 2u);
}

bool Translator::V_CUBEMA_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 3u);
}

bool Translator::FloatCube(const Decoder::Instruction& inst, uint32_t result_kind) {
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
		case 3u: {
			const auto two = IR::F32(IR::Value::F32(2.0f));
			result         = select_face(IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {x, two})),
			                             IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {y, two})),
			                             IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {z, two})));
			break;
		}
		default: EXIT("invalid cube result kind");
	}
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

// Half-precision frexp works on the raw 16-bit encoding: the mantissa keeps its bits and the
// exponent is replaced by the one that puts the value in [0.5, 1).
bool Translator::V_FREXP_MANT_F16(const Decoder::Instruction& inst) {
	const auto bits = ReadF16LaneBits(inst.src0, false);
	const auto exponent =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {bits, IR::Value(10u), IR::Value(5u)}));
	const auto mantissa = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x03ffu)));
	const auto sign     = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x8000u)));
	const auto base     = ir.BitwiseOr(sign, IR::U32(IR::Value(0x3800u)));
	const auto normal   = ir.BitwiseOr(base, mantissa);
	const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {mantissa}));
	const auto shift    = ir.ISub(IR::U32(IR::Value(10u)), msb);
	const auto fraction =
	    ir.BitwiseAnd(ir.ShiftLeftLogical(mantissa, shift), IR::U32(IR::Value(0x03ffu)));
	const auto subnormal = ir.BitwiseOr(base, fraction);
	const auto zero      = ir.IEqual(mantissa, IR::U32(IR::Value(0u)));
	const auto finite    = ir.Select(ir.INotEqual(exponent, IR::U32(IR::Value(0u))), normal,
	                                 ir.Select(zero, bits, subnormal));
	const auto result    = ir.Select(ir.IEqual(exponent, IR::U32(IR::Value(0x1fu))), bits, finite);
	Write16Bits(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_FREXP_EXP_I16_F16(const Decoder::Instruction& inst) {
	const auto bits = ReadF16LaneBits(inst.src0, false);
	const auto exponent =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {bits, IR::Value(10u), IR::Value(5u)}));
	const auto mantissa  = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x03ffu)));
	const auto normal    = ir.ISub(exponent, IR::U32(IR::Value(14u)));
	const auto msb       = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {mantissa}));
	const auto subnormal = ir.ISub(msb, IR::U32(IR::Value(23u)));
	const auto denormal  = ir.Select(ir.INotEqual(mantissa, IR::U32(IR::Value(0u))), subnormal,
	                                 IR::U32(IR::Value(0u)));
	const auto finite    = ir.Select(ir.INotEqual(exponent, IR::U32(IR::Value(0u))), normal,
	                                 denormal);
	const auto result    = ir.Select(ir.IEqual(exponent, IR::U32(IR::Value(0x1fu))),
	                                 IR::U32(IR::Value(0u)), finite);
	Write16Bits(DestinationOperand(inst), ir.BitwiseAnd(result, IR::U32(IR::Value(0xffffu))));
	return true;
}

// The legacy multiply returns +0.0 whenever either operand is zero, even against NaN or infinity.
IR::F32 Translator::FloatMulLegacy(IR::F32 lhs, IR::F32 rhs) {
	const auto zero    = IR::F32(IR::Value::F32(0.0f));
	const auto product = IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {lhs, rhs}));
	const auto either_zero =
	    ir.LogicalOr(IR::U1(ir.Emit(IR::ValueOpcode::FPOrdEqual32, {lhs, zero})),
	                 IR::U1(ir.Emit(IR::ValueOpcode::FPOrdEqual32, {rhs, zero})));
	return SelectF32(either_zero, zero, product);
}

bool Translator::FloatLegacy(const Decoder::Instruction& inst, bool add, bool accumulator) {
	const auto lhs = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
	const auto rhs = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
	auto       result = FloatMulLegacy(lhs, rhs);
	if (add) {
		const auto& addend_operand = accumulator ? inst.dst : inst.src2;
		const auto  addend         = IR::F32(ReadOperand(addend_operand, IR::Type::F32));
		result = IR::F32(ir.Emit(IR::ValueOpcode::FPAdd32, {result, addend}));
	}
	WriteOperand(DestinationOperand(inst), result);
	return true;
}

bool Translator::V_LDEXP_F16(const Decoder::Instruction& inst) {
	const auto value = ReadF16AsF32(inst.src0);
	WriteF16(DestinationOperand(inst),
	         IR::F32(ir.Emit(IR::ValueOpcode::FPLdexp, {value, ReadU32(inst.src1)})));
	return true;
}

// Division fix-up applies the special-case numerics of a divide to the quotient the reciprocal
// sequence produced. S0 is that quotient, S1 the denominator and S2 the numerator.
bool Translator::V_DIV_FIXUP(const Decoder::Instruction& inst, bool half) {
	const auto read = [&](const Decoder::Operand& operand) {
		return half ? ReadF16AsF32(operand) : IR::F32(ReadOperand(operand, IR::Type::F32));
	};
	const auto quotient    = read(inst.src0);
	const auto denominator = read(inst.src1);
	const auto numerator   = read(inst.src2);
	const auto zero        = IR::F32(IR::Value::F32(0.0f));
	const auto infinity    = IR::F32(IR::Value::F32(std::numeric_limits<float>::infinity()));
	const auto quiet_nan =
	    ir.BitCastF32(IR::U32(IR::Value(0xffc00000u)));
	const auto is_nan = [&](IR::F32 value) {
		return IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {value}));
	};
	const auto is_zero = [&](IR::F32 value) {
		return IR::U1(ir.Emit(IR::ValueOpcode::FPOrdEqual32, {value, zero}));
	};
	const auto is_infinite = [&](IR::F32 value) {
		return IR::U1(ir.Emit(IR::ValueOpcode::FPOrdEqual32,
		                      {IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {value})), infinity}));
	};
	const auto sign_bit = [&](IR::F32 value) {
		return ir.ShiftRightLogical(ir.BitCastU32(value), IR::U32(IR::Value(31u)));
	};
	const auto negative_result =
	    ir.INotEqual(ir.BitwiseXor(sign_bit(denominator), sign_bit(numerator)),
	                 IR::U32(IR::Value(0u)));
	const auto magnitude    = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {quotient}));
	const auto signed_value = SelectF32(negative_result,
	                                    IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {magnitude})),
	                                    magnitude);
	const auto signed_infinity =
	    SelectF32(negative_result, IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {infinity})),
	              infinity);
	const auto signed_zero =
	    SelectF32(negative_result, IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {zero})), zero);
	auto result = signed_value;
	result      = SelectF32(ir.LogicalOr(is_infinite(denominator), is_zero(numerator)), signed_zero,
	                        result);
	result = SelectF32(ir.LogicalOr(is_zero(denominator), is_infinite(numerator)), signed_infinity,
	                   result);
	result = SelectF32(ir.LogicalAnd(is_infinite(denominator), is_infinite(numerator)), quiet_nan,
	                   result);
	result = SelectF32(ir.LogicalAnd(is_zero(denominator), is_zero(numerator)), quiet_nan, result);
	result = SelectF32(is_nan(denominator), quiet_nan, result);
	result = SelectF32(is_nan(numerator), quiet_nan, result);
	if (half) {
		WriteF16(DestinationOperand(inst), result);
	} else {
		WriteOperand(DestinationOperand(inst), result);
	}
	return true;
}

// The architectural scaling only protects the Newton-Raphson iteration from exponents that would
// make the intermediate reciprocal overflow or flush to zero. Passing the operand through unscaled
// keeps every other quotient exact and leaves the post-scale flag clear for V_DIV_FMAS_F32.
bool Translator::V_DIV_SCALE_F32(const Decoder::Instruction& inst) {
	WriteOperand(DestinationOperand(inst), ReadOperand(inst.src0, IR::Type::F32));
	if (inst.dst2.kind != Decoder::OperandKind::Null &&
	    inst.dst2.kind != Decoder::OperandKind::Unknown) {
		WriteMask(inst.dst2, IR::U1(IR::Value(false)));
	}
	return true;
}

bool Translator::V_DIV_FMAS_F32(const Decoder::Instruction& inst) {
	const auto value = IR::F32(ir.Emit(IR::ValueOpcode::FPFma32,
	                                   {ReadOperand(inst.src0, IR::Type::F32),
	                                    ReadOperand(inst.src1, IR::Type::F32),
	                                    ReadOperand(inst.src2, IR::Type::F32)}));
	// VCC carries the post-scale request produced by V_DIV_SCALE_F32.
	const auto scaled = IR::F32(
	    ir.Emit(IR::ValueOpcode::FPMul32, {value, IR::Value::F32(4294967296.0f)}));
	WriteOperand(DestinationOperand(inst), SelectF32(ir.GetVcc(), scaled, value));
	return true;
}

bool Translator::V_DOT2_F32_F16(const Decoder::Instruction& inst) {
	auto a      = inst.src0;
	a.op_sel    = false;
	a.op_sel_hi = true;
	auto b      = inst.src1;
	b.op_sel    = false;
	b.op_sel_hi = true;
	const auto low = IR::F32(ir.Emit(IR::ValueOpcode::FPFma32,
	                                 {ReadF16LaneAsF32(a, false), ReadF16LaneAsF32(b, false),
	                                  ReadOperand(inst.src2, IR::Type::F32)}));
	WriteOperand(DestinationOperand(inst),
	             ir.Emit(IR::ValueOpcode::FPFma32,
	                     {ReadF16LaneAsF32(a, true), ReadF16LaneAsF32(b, true), low}));
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
