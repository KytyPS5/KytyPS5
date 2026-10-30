#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t EmitMinMaxF64(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
	if (state.soft_float64) {
		return EmitSoftFloat64(state, max_value ? SoftFloat64Op::Max : SoftFloat64Op::Min,
		                       {lhs, rhs});
	}
	const auto bits_type = TypeU32Vector(state, 2);
	const auto lhs_bits = Unary(state, spv::OpBitcast, bits_type, lhs);
	const auto rhs_bits = Unary(state, spv::OpBitcast, bits_type, rhs);
	const auto ordered = Binary(state, max_value ? spv::OpFOrdGreaterThan : spv::OpFOrdLessThan,
	                            TypeBool(state), lhs, rhs);
	auto result = Select(state, TypeF64(state), ordered, lhs, rhs);

	// Equal values have identical bits except signed zero: min chooses -0, max chooses +0.
	const auto equal = Binary(state, spv::OpFOrdEqual, TypeBool(state), lhs, rhs);
	const auto equal_bits = Binary(state, max_value ? spv::OpBitwiseAnd : spv::OpBitwiseOr,
	                               bits_type, lhs_bits, rhs_bits);
	result = Select(state, TypeF64(state), equal,
	                Unary(state, spv::OpBitcast, TypeF64(state), equal_bits), result);

	// Non-IEEE mode selects the other operand for NaN, and lhs when both are NaN. The ordered
	// compares already pick rhs for a NaN lhs.
	return Select(state, TypeF64(state), Unary(state, spv::OpIsNan, TypeBool(state), rhs),
	              lhs, result);
}

uint32_t EmitMulHigh(EmitterState& state, uint32_t lhs, uint32_t rhs, bool signed_value) {
	const auto operand_type = signed_value ? TypeI32(state) : TypeU32(state);
	const auto pair_type    = signed_value ? TypeI32Pair(state) : TypeU32Pair(state);
	uint32_t   lhs_operand  = lhs;
	uint32_t   rhs_operand  = rhs;
	if (signed_value) {
		lhs_operand = Unary(state, spv::OpBitcast, TypeI32(state), lhs);
		rhs_operand = Unary(state, spv::OpBitcast, TypeI32(state), rhs);
	}
	const auto extended = state.builder.AllocateId();
	state.builder.AddFunction(signed_value ? spv::OpSMulExtended : spv::OpUMulExtended, pair_type,
	                          extended, lhs_operand, rhs_operand);
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, operand_type, high, extended, 1);
	return signed_value ? Unary(state, spv::OpBitcast, TypeU32(state), high) : high;
}

uint32_t EmitFMinMax3(EmitterState& state, uint32_t a, uint32_t b, uint32_t c, bool max_value) {
	return EmitMinMaxF32Value(state, EmitMinMaxF32Value(state, a, b, max_value), c, max_value);
}

uint32_t EmitExt(EmitterState& state, uint32_t type, uint32_t opcode,
                 std::initializer_list<uint32_t> args) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, type, result, GlslStd450(state), opcode, args);
	return result;
}

uint32_t EmitF32ToU32(EmitterState& state, uint32_t src, bool signed_value) {
	// Clamping first keeps the truncating conversion defined; NaN and values past the largest
	// float below the integer limit are selected afterwards.
	const auto clamped =
	    EmitExt(state, TypeF32(state), GLSLstd450NClamp,
	            {src, ConstantF32(state, signed_value ? 0xcf000000u : 0u),
	             ConstantF32(state, signed_value ? 0x4effffffu : 0x4f7fffffu)});
	const auto converted = signed_value
	                           ? Unary(state, spv::OpBitcast, TypeU32(state),
	                                   Unary(state, spv::OpConvertFToS, TypeI32(state), clamped))
	                           : Unary(state, spv::OpConvertFToU, TypeU32(state), clamped);
	const auto above     = Binary(state, spv::OpFOrdGreaterThanEqual, TypeBool(state), src,
	                              ConstantF32(state, signed_value ? 0x4f000000u : 0x4f800000u));
	const auto high =
	    Select(state, TypeU32(state), above,
	           ConstantU32(state, signed_value ? 0x7fffffffu : 0xffffffffu), converted);
	return Select(state, TypeU32(state), Unary(state, spv::OpIsNan, TypeBool(state), src),
	              ConstantU32(state, 0), high);
}

} // namespace

uint32_t EmitF32BitsToF16RtzBits(EmitterState& state, uint32_t bits) {
	// Native half conversion causes regressions; use compact integer RTZ conversion.
	const auto abs  = EmitAndConstant(state, bits, 0x7fffffffu);
	const auto sign = EmitAndConstant(
	    state, Binary(state, spv::OpShiftRightLogical, TypeU32(state), bits, ConstantU32(state, 16)),
	    0x8000u);
	const auto exp = Binary(state, spv::OpShiftRightLogical, TypeU32(state), abs,
	                        ConstantU32(state, 23));
	const auto mant = EmitAndConstant(state, abs, 0x007fffffu);

	// Align the hidden bit to the half subnormal mantissa. Even unused shifts must be < 32.
	const auto shift = EmitUMin32(
	    state, Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 126), exp),
	    ConstantU32(state, 31));
	const auto sub = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
	                        EmitOrU32(state, mant, ConstantU32(state, 0x00800000u)), shift);

	// Drop 13 mantissa bits and rebias the exponent; finite overflow saturates to max half.
	const auto truncated = Binary(state, spv::OpShiftRightLogical, TypeU32(state), abs,
	                              ConstantU32(state, 13));
	const auto normal = EmitUMin32(
	    state, Binary(state, spv::OpISub, TypeU32(state), truncated, ConstantU32(state, 0x1c000u)),
	    ConstantU32(state, 0x7bffu));
	const auto finite = Select(state, TypeU32(state),
	                           EmitCompareU32Constant(state, spv::OpULessThan, abs, 0x38800000u),
	                           sub, normal);
	const auto nan = EmitOrU32(state, EmitAndConstant(state, truncated, 0x03ffu),
	                           ConstantU32(state, 0x7e00u));
	const auto special = Select(state, TypeU32(state),
	                            EmitCompareU32Constant(state, spv::OpIEqual, mant, 0),
	                            ConstantU32(state, 0x7c00u), nan);
	const auto magnitude = Select(
	    state, TypeU32(state),
	    EmitCompareU32Constant(state, spv::OpUGreaterThanEqual, abs, 0x7f800000u), special, finite);
	return EmitOrU32(state, sign, magnitude);
}

namespace {

// Keeps a compiler from merging the operation with a neighbour. On a host where the mark is slow
// it is left out, and the caller says why the result is exact without it.
void MarkNoContraction(EmitterState& state, uint32_t result) {
	if (!state.host.faults.no_contraction_is_slow) {
		state.builder.AddAnnotation(spv::OpDecorate, result, spv::DecorationNoContraction);
	}
}

} // namespace

uint32_t EmitFPFma32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	const auto result = EmitGlsl<GLSLstd450Fma, IR::Type::F32>(state, a, b, c);
	// Without the mark: a fused multiply-add is one operation already, and has no part a compiler
	// could merge with a neighbour.
	MarkNoContraction(state, result);
	return result;
}

namespace {

uint32_t TypeF16x2(EmitterState& state) {
	if (state.f16x2_type == 0) {
		const auto half  = state.builder.Type(spv::OpTypeFloat, 16);
		state.f16x2_type = state.builder.Type(spv::OpTypeVector, half, 2);
	}
	return state.f16x2_type;
}

uint32_t BitcastTo(EmitterState& state, uint32_t type, uint32_t value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, type, result, value);
	return result;
}

// Lane `high` of a packed pair as F32, as the guest's lane reads give it (ConvertF32F16).
uint32_t HalfLaneF32(EmitterState& state, uint32_t packed, bool high) {
	return EmitF16BitsToF32(state, high ? EmitShiftRightLogical32(state, packed,
	                                                              ConstantU32(state, 16))
	                                    : packed);
}

uint32_t PackLanes(EmitterState& state, uint32_t low, uint32_t high) {
	const auto pair = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 2), pair, low, high);
	return EmitPackHalf2x16(state, pair);
}

// A packed FP16 operation: native on two-lane half vectors, or (without native FP16) each lane
// through F32 and back, as the translator did before these operations existed.
template <typename Native, typename Lane>
uint32_t PackedF16(EmitterState& state, std::initializer_list<uint32_t> args, Native native,
                   Lane lane) {
	if (state.native_float16) {
		std::array<uint32_t, 3> halves {};
		size_t                  index = 0;
		for (const auto arg: args) halves[index++] = BitcastTo(state, TypeF16x2(state), arg);
		return BitcastTo(state, TypeU32(state), native(halves));
	}
	std::array<uint32_t, 2> lanes {};
	for (const bool high: {false, true}) {
		std::array<uint32_t, 3> values {};
		size_t                  index = 0;
		for (const auto arg: args) values[index++] = HalfLaneF32(state, arg, high);
		lanes[high ? 1 : 0] = lane(values);
	}
	return PackLanes(state, lanes[0], lanes[1]);
}

uint32_t NativeF16x2(EmitterState& state, spv::Op op, uint32_t a, uint32_t b) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(op, TypeF16x2(state), result, a, b);
	MarkNoContraction(state, result);
	return result;
}

} // namespace

uint32_t EmitPackedFPAdd16(EmitterState& state, uint32_t a, uint32_t b) {
	return PackedF16(
	    state, {a, b}, [&](const auto& h) { return NativeF16x2(state, spv::OpFAdd, h[0], h[1]); },
	    [&](const auto& v) { return EmitFPAdd32(state, v[0], v[1]); });
}

uint32_t EmitPackedFPMul16(EmitterState& state, uint32_t a, uint32_t b) {
	return PackedF16(
	    state, {a, b}, [&](const auto& h) { return NativeF16x2(state, spv::OpFMul, h[0], h[1]); },
	    [&](const auto& v) { return EmitFPMul32(state, v[0], v[1]); });
}

uint32_t EmitPackedFPFma16(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	return PackedF16(
	    state, {a, b, c},
	    [&](const auto& h) {
		    const auto result = state.builder.AllocateId();
		    state.builder.AddFunction(spv::OpExtInst, TypeF16x2(state), result, GlslStd450(state),
		                              GLSLstd450Fma, h[0], h[1], h[2]);
		    return result;
	    },
	    [&](const auto& v) { return EmitFPFma32(state, v[0], v[1], v[2]); });
}

uint32_t EmitFPMad32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	if (state.host.faults.no_contraction_is_slow) {
		// The marks below are slow on this host. So the product goes to the sum through the
		// denormal flush of legacy MAD/MAC, which works on its bits: no multiply then feeds an add
		// directly, and there is nothing for a compiler to fuse. The operands and the sum are not
		// flushed, as on the other hosts below. A flush takes six instructions, and with five of
		// them a multiply-add took 33: a compute shader of Crash Bandicoot 4 grew to 1.65 million
		// SPIR-V words, and the Metal compiler took from 1.5 to over 10 minutes to build it.
		const auto product = EmitFPMul32(state, a, b);
		return EmitFPAdd32(state, EmitFlushF32DenormToSignedZero(state, product), c);
	}
	// Explicit denormal checks typically drop 3DMiniGolf menu FPS from 28-29 to 21-24
	// on NVIDIA RTX 5080 Laptop GPU, so leave them disabled for this experiment.
	// a = EmitFlushF32DenormToSignedZero(state, a);
	// b = EmitFlushF32DenormToSignedZero(state, b);
	// c = EmitFlushF32DenormToSignedZero(state, c);
	const auto product = EmitFPMul32(state, a, b);
	state.builder.AddAnnotation(spv::OpDecorate, product, spv::DecorationNoContraction);
	// const auto sum = EmitFPAdd32(state, EmitFlushF32DenormToSignedZero(state, product), c);
	const auto sum = EmitFPAdd32(state, product, c);
	state.builder.AddAnnotation(spv::OpDecorate, sum, spv::DecorationNoContraction);
	// return EmitFlushF32DenormToSignedZero(state, sum);
	return sum;
}

uint32_t EmitConvertU16U32(EmitterState& state, uint32_t arg0) {
	return EmitNative<spv::OpBitwiseAnd, IR::Type::U16>(state, arg0, ConstantU32(state, 0xffffu));
}

uint32_t EmitConvertU8U32(EmitterState& state, uint32_t arg0) {
	return EmitNative<spv::OpBitwiseAnd, IR::Type::U8>(state, arg0, ConstantU32(state, 0xffu));
}

uint32_t EmitConvertF16F32(EmitterState& state, uint32_t arg0) {
	const auto pair = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 2), pair, arg0,
	                          ConstantF32(state, 0));
	return EmitPackHalf2x16(state, pair);
}

uint32_t EmitConvertS32F32(EmitterState& state, uint32_t arg0) {
	return EmitF32ToU32(state, arg0, true);
}

uint32_t EmitConvertU32F32(EmitterState& state, uint32_t arg0) {
	return EmitF32ToU32(state, arg0, false);
}

uint32_t EmitConvertF32F64(EmitterState& state, uint32_t arg0) {
	if (state.soft_float64) {
		return Unary(state, spv::OpBitcast, TypeF32(state),
		             EmitSoftFloat64(state, SoftFloat64Op::ToF32, {arg0}));
	}
	const auto converted = Unary(state, spv::OpFConvert, TypeF32(state), arg0);
	const auto source    = EmitNative<spv::OpCompositeExtract, IR::Type::U32>(
	    state, Unary(state, spv::OpBitcast, TypeU32Vector(state, 2), arg0), 1u);
	const auto exponent = EmitAndConstant(state, source, 0x7ff00000u);
	const auto overflow =
	    Binary(state, spv::OpLogicalAnd, TypeBool(state),
	           EmitCompareU32Constant(state, spv::OpUGreaterThan, exponent, 0x47e00000u),
	           EmitCompareU32Constant(state, spv::OpULessThan, exponent, 0x7ff00000u));
	const auto clamped = EmitOrU32(state, EmitAndConstant(state, source, 0x80000000u),
	                               ConstantU32(state, 0x7f7fffffu));
	return EmitFlushF32DenormToSignedZero(
	    state, Select(state, TypeF32(state), overflow,
	                  Unary(state, spv::OpBitcast, TypeF32(state), clamped), converted));
}

uint32_t EmitConvertF64F32(EmitterState& state, uint32_t arg0) {
	if (state.soft_float64) {
		return EmitSoftFloat64(state, SoftFloat64Op::FromF32,
		                       {Unary(state, spv::OpBitcast, TypeU32(state), arg0)});
	}
	return EmitNative<spv::OpFConvert, IR::Type::F64>(state,
	                                                  EmitFlushF32DenormToSignedZero(state, arg0));
}

uint32_t EmitCompositeConstructU64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1) {
	arg1 = arg1.Resolve();
	return arg1.IsImmediate() && arg1.U32() == 0u
	           ? Unary(ctx.state, spv::OpUConvert, TypeU64(ctx.state), arg0)
	           : PackU64(ctx.state, arg0, ctx.Def(arg1));
}

uint32_t EmitCompositeExtractU64(EmitterState& state, uint32_t arg0, IR::Value arg1) {
	const auto value = arg1.U32() == 0u ? arg0
	                                    : Binary(state, spv::OpShiftRightLogical, TypeU64(state),
	                                             arg0, ConstantU32(state, 32));
	return Unary(state, spv::OpUConvert, TypeU32(state), value);
}

uint32_t EmitCompositeExtractU32x2(EmitterState& state, uint32_t arg0, IR::Value arg1) {
	return EmitNative<spv::OpCompositeExtract, IR::Type::U32>(state, arg0, arg1.U32());
}

uint32_t EmitPackFloat2x16Rtz(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitNative<spv::OpBitFieldInsert, IR::Type::U32>(
	    state, EmitF32BitsToF16RtzBits(state, EmitBitcastF32ToU32(state, arg0)),
	    EmitF32BitsToF16RtzBits(state, EmitBitcastF32ToU32(state, arg1)),
	    ConstantU32(state, 16), ConstantU32(state, 16));
}

uint32_t EmitFPSaturate32(EmitterState& state, uint32_t arg0) {
	// The clamp modifier runs in DX10 clamp mode: NaN becomes 0, as NClamp's NMax step does.
	return EmitExt(state, TypeF32(state), GLSLstd450NClamp,
	               {arg0, ConstantF32(state, 0), ConstantF32(state, 0x3f800000u)});
}

uint32_t EmitSMulHi(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMulHigh(state, arg0, arg1, true);
}

uint32_t EmitUMulHi(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMulHigh(state, arg0, arg1, false);
}

uint32_t EmitSMinTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitSMin32(state, arg0, EmitSMin32(state, arg1, arg2));
}

uint32_t EmitSMaxTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitSMax32(state, arg0, EmitSMax32(state, arg1, arg2));
}

uint32_t EmitUMinTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitUMin32(state, arg0, EmitUMin32(state, arg1, arg2));
}

uint32_t EmitUMaxTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitUMax32(state, arg0, EmitUMax32(state, arg1, arg2));
}

uint32_t EmitSMedTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	const auto high_min = EmitSMin32(state, EmitSMax32(state, arg0, arg1), arg2);
	return EmitSMax32(state, EmitSMin32(state, arg0, arg1), high_min);
}

uint32_t EmitUMedTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	const auto high_min = EmitUMin32(state, EmitUMax32(state, arg0, arg1), arg2);
	return EmitUMax32(state, EmitUMin32(state, arg0, arg1), high_min);
}

uint32_t EmitFPMin32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxF32Value(state, arg0, arg1, false);
}

uint32_t EmitFPMax32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxF32Value(state, arg0, arg1, true);
}

uint32_t EmitFPMin64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxF64(state, arg0, arg1, false);
}

uint32_t EmitFPMax64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxF64(state, arg0, arg1, true);
}

uint32_t EmitFPMinTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitFMinMax3(state, arg0, arg1, arg2, false);
}

uint32_t EmitFPMaxTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitFMinMax3(state, arg0, arg1, arg2, true);
}

uint32_t EmitFPRecip32(EmitterState& state, uint32_t arg0) {
	const auto source = EmitFlushF32DenormToSignedZero(state, arg0);
	return Binary(state, spv::OpFDiv, TypeF32(state), ConstantF32(state, 0x3f800000u), source);
}

uint32_t EmitFPRecip64(EmitterState& state, uint32_t arg0) {
	if (state.soft_float64) {
		return EmitSoftFloat64(state, SoftFloat64Op::Recip, {arg0});
	}
	// V_RCP_F64 takes denormal inputs and gives denormal results, but a driver's division may not
	// for a divisor outside [2^-1022, 2^1022] (RADV gave 0 for 1/2^1023 and an approximation for
	// 1/2^-1023). The divisor is scaled into that range by a power of two, and the result back.
	const auto f64      = TypeF64(state);
	const auto constant = [&](uint32_t high) {
		return state.builder.Constant(spv::OpConstant, f64, 0u, high);
	};
	const auto words = Unary(state, spv::OpBitcast, TypeU32Vector(state, 2), arg0);
	const auto high  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, words, 1u);
	const auto exponent = EmitNative<spv::OpBitFieldUExtract, IR::Type::U32>(
	    state, high, ConstantU32(state, 20), ConstantU32(state, 11));
	const auto large = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), exponent,
	                          ConstantU32(state, 0x7fdu));
	const auto tiny  = Binary(state, spv::OpULessThanEqual, TypeBool(state), exponent,
	                          ConstantU32(state, 2u));
	const auto scale = Select(state, f64, large, constant(0x3bf00000u), // 2^-64
	                          Select(state, f64, tiny, constant(0x43f00000u), // 2^64
	                                 constant(0x3ff00000u)));
	const auto quotient = EmitNative<spv::OpFDiv, IR::Type::F64>(
	    state, constant(0x3ff00000u), EmitNative<spv::OpFMul, IR::Type::F64>(state, arg0, scale));
	return EmitNative<spv::OpFMul, IR::Type::F64>(state, quotient, scale);
}

uint32_t EmitFPRecipIFlag32(EmitterState& state, uint32_t arg0) {
	// Integer-to-float inputs used by IFLAG cannot be denormal.
	return Binary(state, spv::OpFDiv, TypeF32(state), ConstantF32(state, 0x3f800000u), arg0);
}

uint32_t EmitFPRecipSqrt32(EmitterState& state, uint32_t arg0) {
	return EmitExt(state, TypeF32(state), GLSLstd450InverseSqrt,
	               {EmitFlushF32DenormToSignedZero(state, arg0)});
}

uint32_t EmitFPSqrt(EmitterState& state, uint32_t arg0) {
	return EmitExt(state, TypeF32(state), GLSLstd450Sqrt,
	               {EmitFlushF32DenormToSignedZero(state, arg0)});
}

uint32_t EmitFPExp2(EmitterState& state, uint32_t arg0) {
	// A denormal input already rounds to 1.0, the result for its flushed zero.
	return EmitExt(state, TypeF32(state), GLSLstd450Exp2, {arg0});
}

uint32_t EmitFPLog2(EmitterState& state, uint32_t arg0) {
	return EmitExt(state, TypeF32(state), GLSLstd450Log2,
	               {EmitFlushF32DenormToSignedZero(state, arg0)});
}

uint32_t EmitFPLdexp(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	const auto exponent = Unary(state, spv::OpBitcast, TypeI32(state), arg1);
	return EmitExt(state, TypeF32(state), GLSLstd450Ldexp, {arg0, exponent});
}

uint32_t EmitFPSin(EmitterState& state, uint32_t arg0) {
	auto source = EmitTrigCycleF32(state, arg0, true);
	source = Binary(state, spv::OpFMul, TypeF32(state), source, ConstantF32(state, 0x40c90fdbu));
	return EmitExt(state, TypeF32(state), GLSLstd450Sin, {source});
}

uint32_t EmitFPCos(EmitterState& state, uint32_t arg0) {
	auto source = EmitTrigCycleF32(state, arg0, false);
	source = Binary(state, spv::OpFMul, TypeF32(state), source, ConstantF32(state, 0x40c90fdbu));
	return EmitExt(state, TypeF32(state), GLSLstd450Cos, {source});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
