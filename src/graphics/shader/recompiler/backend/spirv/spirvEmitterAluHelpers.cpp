#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitAndConstant(EmitterState& state, uint32_t value, uint32_t mask) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), ret, value,
	                          ConstantU32(state, mask));
	return ret;
}

uint32_t EmitCompareU32Constant(EmitterState& state, spv::Op opcode, uint32_t value,
                                uint32_t constant) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction(opcode, TypeBool(state), ret, value, ConstantU32(state, constant));
	return ret;
}

F32Class EmitClassifyF32Bits(EmitterState& state, uint32_t bits) {
	const auto abs_bits = EmitAndConstant(state, bits, 0x7fffffffu);
	return {EmitCompareU32Constant(state, spv::OpUGreaterThan, abs_bits, 0x7f800000u),
	        EmitCompareU32Constant(state, spv::OpIEqual, abs_bits, 0)};
}

static uint32_t EmitClassMaskBitMatch(EmitterState& state, uint32_t mask, uint32_t bit,
                                      uint32_t class_match) {
	const auto selected =
	    EmitCompareU32Constant(state, spv::OpINotEqual, EmitAndConstant(state, mask, 1u << bit), 0);
	return EmitLogicalAndBool(state, selected, class_match);
}

static uint32_t EmitClassMaskBits(EmitterState& state, uint32_t bits, uint32_t mask, bool half) {
	const uint32_t sign_mask     = half ? 0x8000u : 0x80000000u;
	const uint32_t exponent_mask = half ? 0x7c00u : 0x7f800000u;
	const uint32_t mantissa_mask = half ? 0x03ffu : 0x007fffffu;
	const uint32_t quiet_mask    = half ? 0x0200u : 0x00400000u;
	const auto     sign_bits     = EmitAndConstant(state, bits, sign_mask);
	const auto     abs_bits      = EmitAndConstant(state, bits, sign_mask - 1u);
	const auto     exponent_bits = EmitAndConstant(state, abs_bits, exponent_mask);
	const auto     mantissa_bits = EmitAndConstant(state, abs_bits, mantissa_mask);
	const auto     quiet_bits    = EmitAndConstant(state, mantissa_bits, quiet_mask);

	const auto sign             = EmitCompareU32Constant(state, spv::OpINotEqual, sign_bits, 0);
	const auto positive         = EmitLogicalNotBool(state, sign);
	const auto exponent_zero    = EmitCompareU32Constant(state, spv::OpIEqual, exponent_bits, 0);
	const auto exponent_nonzero = EmitLogicalNotBool(state, exponent_zero);
	const auto exponent_inf =
	    EmitCompareU32Constant(state, spv::OpIEqual, exponent_bits, exponent_mask);
	const auto finite_exponent  = EmitLogicalNotBool(state, exponent_inf);
	const auto mantissa_zero    = EmitCompareU32Constant(state, spv::OpIEqual, mantissa_bits, 0);
	const auto mantissa_nonzero = EmitLogicalNotBool(state, mantissa_zero);
	const auto quiet            = EmitCompareU32Constant(state, spv::OpINotEqual, quiet_bits, 0);

	const auto nan_common = EmitLogicalAndBool(state, exponent_inf, mantissa_nonzero);
	const auto snan       = EmitLogicalAndBool(state, nan_common, EmitLogicalNotBool(state, quiet));
	const auto qnan       = EmitLogicalAndBool(state, nan_common, quiet);
	const auto inf        = EmitLogicalAndBool(state, exponent_inf, mantissa_zero);
	const auto normal     = EmitLogicalAndBool(state, exponent_nonzero, finite_exponent);
	const auto denorm     = EmitLogicalAndBool(state, exponent_zero, mantissa_nonzero);
	const auto zero       = EmitCompareU32Constant(state, spv::OpIEqual, abs_bits, 0);

	uint32_t match = EmitClassMaskBitMatch(state, mask, 0, snan);
	match          = EmitLogicalOrBool(state, match, EmitClassMaskBitMatch(state, mask, 1, qnan));
	match          = EmitLogicalOrBool(
	    state, match, EmitClassMaskBitMatch(state, mask, 2, EmitLogicalAndBool(state, inf, sign)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 3, EmitLogicalAndBool(state, normal, sign)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 4, EmitLogicalAndBool(state, denorm, sign)));
	match = EmitLogicalOrBool(
	    state, match, EmitClassMaskBitMatch(state, mask, 5, EmitLogicalAndBool(state, zero, sign)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 6, EmitLogicalAndBool(state, zero, positive)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 7, EmitLogicalAndBool(state, denorm, positive)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 8, EmitLogicalAndBool(state, normal, positive)));
	return EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 9, EmitLogicalAndBool(state, inf, positive)));
}

uint32_t EmitClassMaskF32(EmitterState& state, uint32_t value, uint32_t mask) {
	return EmitClassMaskBits(state, EmitBitcastF32ToU32(state, value), mask, false);
}

uint32_t EmitClassMaskF16(EmitterState& state, uint32_t bits, uint32_t mask) {
	return EmitClassMaskBits(state, bits, mask, true);
}

static uint32_t EmitMinMaxF32(EmitterState& state, uint32_t lhs, uint32_t rhs, uint32_t lhs_bits,
                              uint32_t rhs_bits, bool max_value) {
	const auto both_negative =
	    EmitCompareU32Constant(state, spv::OpSLessThan, EmitAndU32(state, lhs_bits, rhs_bits), 0);
	const auto less = Binary(state, spv::OpSLessThan, TypeBool(state),
	                         max_value ? rhs_bits : lhs_bits, max_value ? lhs_bits : rhs_bits);
	const auto keep_lhs =
	    Binary(state, spv::OpLogicalNotEqual, TypeBool(state), less, both_negative);
	const auto ordered = Select(state, TypeF32(state), keep_lhs, lhs, rhs);
	const auto numeric = Select(state, TypeF32(state),
	                            Unary(state, spv::OpIsNan, TypeBool(state), lhs), rhs, ordered);
	return Select(state, TypeF32(state), Unary(state, spv::OpIsNan, TypeBool(state), rhs), lhs,
	              numeric);
}

uint32_t EmitMinMaxF32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
	if (state.profile.native_float_min_max) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpExtInst, TypeF32(state), result, GlslStd450(state),
		                          max_value ? GLSLstd450NMax : GLSLstd450NMin, lhs, rhs);
		return result;
	}
	return EmitMinMaxF32(state, lhs, rhs, EmitBitcastF32ToU32(state, lhs),
	                     EmitBitcastF32ToU32(state, rhs), max_value);
}

static uint32_t EmitOrderKeyF32(EmitterState& state, uint32_t value) {
	constexpr uint32_t mantissa_bits = 0x7fffffu;

	const auto bits = EmitBitcastF32ToU32(state, value);
	const auto sign =
	    Binary(state, spv::OpShiftRightArithmetic, TypeU32(state), bits, ConstantU32(state, 31));
	const auto magnitude_flip =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), sign, ConstantU32(state, 1));
	return Binary(state, spv::OpISub, TypeU32(state),
	              Binary(state, spv::OpBitwiseXor, TypeU32(state), bits, magnitude_flip),
	              ConstantU32(state, mantissa_bits));
}

static uint32_t EmitSignedMinMaxU32(EmitterState& state, uint32_t lhs, uint32_t rhs,
                                    bool max_value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeU32(state), result, GlslStd450(state),
	                          max_value ? GLSLstd450SMax : GLSLstd450SMin, lhs, rhs);
	return result;
}

uint32_t EmitFPMedTri32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	constexpr uint32_t infinity_key      = 0x7f000001u;
	constexpr uint32_t negative_zero_key = 0xff800000u;
	constexpr uint32_t positive_zero_key = 0xff800001u;

	const auto below = [&](uint32_t lhs, uint32_t rhs) {
		return Binary(state, spv::OpSLessThan, TypeBool(state), lhs, rhs);
	};
	const auto at_least = [&](uint32_t lhs, uint32_t rhs) {
		return Binary(state, spv::OpSGreaterThanEqual, TypeBool(state), lhs, rhs);
	};
	const auto pick = [&](uint32_t condition, uint32_t lhs, uint32_t rhs) {
		return Select(state, TypeF32(state), condition, lhs, rhs);
	};
	const auto a_key     = EmitOrderKeyF32(state, a);
	const auto b_key     = EmitOrderKeyF32(state, b);
	const auto c_key     = EmitSignedMinMaxU32(state, EmitOrderKeyF32(state, c),
	                                           ConstantU32(state, infinity_key + 1u), false);
	const auto a_below_b = below(a_key, b_key);
	const auto a_below_c = below(a_key, c_key);
	const auto b_below_c = below(b_key, c_key);
	const auto min3 =
	    pick(EmitLogicalAndBool(state, a_below_b, a_below_c), a, pick(b_below_c, b, c));
	const auto max_key =
	    EmitSignedMinMaxU32(state, EmitSignedMinMaxU32(state, a_key, b_key, true), c_key, true);
	const auto any_nan = Binary(state, spv::OpSGreaterThan, TypeBool(state), max_key,
	                            ConstantU32(state, infinity_key));
	const auto max_tie_key =
	    Select(state, TypeU32(state),
	           EmitCompareU32Constant(state, spv::OpIEqual, max_key, positive_zero_key),
	           ConstantU32(state, negative_zero_key), max_key);
	const auto a_is_max = at_least(a_key, max_tie_key);
	const auto b_is_max = at_least(b_key, max_tie_key);
	const auto median   = pick(a_is_max, pick(b_below_c, c, b),
	                           pick(b_is_max, pick(a_below_c, c, a), pick(a_below_b, b, a)));
	return pick(any_nan, min3, median);
}

uint32_t EmitFlushF32DenormToSignedZero(EmitterState& state, uint32_t value) {
	// x * 0 keeps the sign of a denormal; NaN and infinity fail the ordered compare.
	const auto magnitude = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), magnitude, GlslStd450(state),
	                          GLSLstd450FAbs, value);
	const auto denormal = Binary(state, spv::OpFOrdLessThan, TypeBool(state), magnitude,
	                             ConstantF32(state, 0x00800000u));
	return Select(state, TypeF32(state), denormal,
	              Binary(state, spv::OpFMul, TypeF32(state), value, ConstantF32(state, 0)), value);
}

uint32_t EmitTrigCycleF32(EmitterState& state, uint32_t src, bool preserve_signed_zero) {
	// Finite values of magnitude 2^23 and above are integers, so fract already gives them +0.
	const auto fract = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), fract, GlslStd450(state),
	                          GLSLstd450Fract, src);
	if (!preserve_signed_zero) {
		return fract;
	}
	const auto abs_bits = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                             Unary(state, spv::OpBitcast, TypeU32(state), src),
	                             ConstantU32(state, 0x7fffffffu));
	return Select(state, TypeF32(state),
	              Binary(state, spv::OpIEqual, TypeBool(state), abs_bits, ConstantU32(state, 0)),
	              src, fract);
}

uint32_t EmitF16BitsToF32(EmitterState& state, uint32_t bits) {
	const auto unpacked = state.builder.AllocateId();
	const auto ret      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32Vector(state, 2), unpacked, GlslStd450(state),
	                          GLSLstd450UnpackHalf2x16, bits);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), ret, unpacked, 0);
	return ret;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
