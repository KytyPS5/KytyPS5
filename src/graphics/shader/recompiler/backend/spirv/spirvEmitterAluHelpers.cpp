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

// A class test against a constant mask, the usual operand, builds only the classes the mask
// selects: DS's v_cmp_class_f32 with 0x38 (negative and finite) is 6 instructions instead of 86.
static uint32_t EmitClassMaskConstantBits(EmitterState& state, uint32_t bits, uint32_t mask,
                                          bool half) {
	mask &= 0x3ffu;
	if (mask == 0u) return ConstantBool(state, false);
	if (mask == 0x3ffu) return ConstantBool(state, true);
	const uint32_t sign_mask     = half ? 0x8000u : 0x80000000u;
	const uint32_t exponent_mask = half ? 0x7c00u : 0x7f800000u;
	const uint32_t mantissa_mask = half ? 0x03ffu : 0x007fffffu;
	const uint32_t quiet_mask    = half ? 0x0200u : 0x00400000u;
	// Each value is emitted the first time a selected class needs it.
	uint32_t   abs_bits = 0, exponent_bits = 0, mantissa_bits = 0;
	const auto get_abs = [&]() {
		if (abs_bits == 0) abs_bits = EmitAndConstant(state, bits, sign_mask - 1u);
		return abs_bits;
	};
	const auto get_exponent = [&]() {
		if (exponent_bits == 0) exponent_bits = EmitAndConstant(state, get_abs(), exponent_mask);
		return exponent_bits;
	};
	const auto get_mantissa = [&]() {
		if (mantissa_bits == 0) mantissa_bits = EmitAndConstant(state, get_abs(), mantissa_mask);
		return mantissa_bits;
	};
	const auto exponent_is = [&](spv::Op op, uint32_t value) {
		return EmitCompareU32Constant(state, op, get_exponent(), value);
	};
	const auto mantissa_is = [&](spv::Op op) {
		return EmitCompareU32Constant(state, op, get_mantissa(), 0);
	};
	// Classes by mask bit: NaNs (0, 1) have no sign; the others are negative (2-5) or positive
	// (9-6) infinity, normal, denormal and zero.
	enum Class : uint32_t { Inf, Normal, Denorm, Zero, Finite, ClassCount };
	const auto selected = [&](uint32_t bit) { return (mask >> bit) & 1u; };
	std::array<uint32_t, ClassCount> signs {}; // bit 0: negative, bit 1: positive
	for (uint32_t c = Inf; c <= Zero; c++) {
		signs[c] = selected(2u + c) | (selected(9u - c) << 1u);
	}
	// Normal, denormal and zero of the same signs are the finite values.
	if (signs[Normal] != 0u && signs[Normal] == signs[Denorm] && signs[Normal] == signs[Zero]) {
		signs[Finite] = signs[Normal];
		signs[Normal] = signs[Denorm] = signs[Zero] = 0u;
	}
	uint32_t   sign  = 0;
	uint32_t   match = 0;
	const auto add   = [&](uint32_t term) {
		match = match == 0 ? term : EmitLogicalOrBool(state, match, term);
	};
	const auto add_signed = [&](uint32_t term, uint32_t which) {
		if (which == 3u) {
			add(term);
			return;
		}
		if (sign == 0) {
			sign = EmitCompareU32Constant(state, spv::OpINotEqual,
			                              EmitAndConstant(state, bits, sign_mask), 0);
		}
		add(EmitLogicalAndBool(state, term,
		                       which == 1u ? sign : EmitLogicalNotBool(state, sign)));
	};
	if (selected(0u) || selected(1u)) {
		const auto nan = EmitLogicalAndBool(state, exponent_is(spv::OpIEqual, exponent_mask),
		                                    mantissa_is(spv::OpINotEqual));
		if (selected(0u) && selected(1u)) {
			add(nan);
		} else {
			const auto quiet = EmitCompareU32Constant(
			    state, spv::OpINotEqual, EmitAndConstant(state, get_mantissa(), quiet_mask), 0);
			add(EmitLogicalAndBool(state, nan,
			                       selected(1u) ? quiet : EmitLogicalNotBool(state, quiet)));
		}
	}
	for (uint32_t c = Inf; c < ClassCount; c++) {
		if (signs[c] == 0u) continue;
		uint32_t term = 0;
		switch (c) {
			case Inf:
				term = EmitLogicalAndBool(state, exponent_is(spv::OpIEqual, exponent_mask),
				                          mantissa_is(spv::OpIEqual));
				break;
			case Normal:
				term = EmitLogicalAndBool(state, exponent_is(spv::OpINotEqual, 0),
				                          exponent_is(spv::OpINotEqual, exponent_mask));
				break;
			case Denorm:
				term = EmitLogicalAndBool(state, exponent_is(spv::OpIEqual, 0),
				                          mantissa_is(spv::OpINotEqual));
				break;
			case Zero: term = EmitCompareU32Constant(state, spv::OpIEqual, get_abs(), 0); break;
			default: term = exponent_is(spv::OpINotEqual, exponent_mask); break;
		}
		add_signed(term, signs[c]);
	}
	return match;
}

static uint32_t EmitClassMask(ValueEmitContext& ctx, uint32_t bits, IR::Value mask, bool half) {
	mask = mask.Resolve();
	if (mask.IsImmediate()) {
		return EmitClassMaskConstantBits(ctx.state, bits, mask.U32(), half);
	}
	return EmitClassMaskBits(ctx.state, bits, ctx.Def(mask), half);
}

uint32_t EmitClassMaskF32(ValueEmitContext& ctx, uint32_t value, IR::Value mask) {
	return EmitClassMask(ctx, EmitBitcastF32ToU32(ctx.state, value), mask, false);
}

uint32_t EmitClassMaskF16(ValueEmitContext& ctx, uint32_t bits, IR::Value mask) {
	return EmitClassMask(ctx, bits, mask, true);
}

static uint32_t EmitMinMaxF32(EmitterState& state, uint32_t lhs, uint32_t rhs, uint32_t lhs_bits,
                              uint32_t rhs_bits, bool max_value) {
	// PS5 V_MIN/V_MAX_F32 return the other source for a NaN source (S0 when both are NaN)
	// and order -0 below +0. An ordered compare takes S1 for a NaN S0 and, unlike NMin/NMax,
	// always returns the exact bits of one source.
	const auto either    = EmitOrU32(state, lhs_bits, rhs_bits);
	const auto both_zero = EmitCompareU32Constant(state, spv::OpIEqual,
	                                              EmitAndConstant(state, either, 0x7fffffffu), 0);
	const auto zero =
	    EmitBitcastU32ToF32(state, max_value ? EmitAndU32(state, lhs_bits, rhs_bits) : either);
	const auto keep_lhs =
	    Binary(state, max_value ? spv::OpFOrdGreaterThanEqual : spv::OpFOrdLessThan,
		       TypeBool(state), lhs, rhs);
	const auto ordered = Select(state, TypeF32(state), keep_lhs, lhs, rhs);
	const auto numeric = Select(state, TypeF32(state), both_zero, zero, ordered);
	return Select(state, TypeF32(state), Unary(state, spv::OpIsNan, TypeBool(state), rhs), lhs,
	              numeric);
}

uint32_t EmitMinMaxF32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
	if (state.profile.native_float_min_max || state.profile.relaxed_float) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpExtInst, TypeF32(state), result, GlslStd450(state),
		                          max_value ? GLSLstd450NMax : GLSLstd450NMin, lhs, rhs);
		return result;
	}
	return EmitMinMaxF32(state, lhs, rhs, EmitBitcastF32ToU32(state, lhs),
	                     EmitBitcastF32ToU32(state, rhs), max_value);
}

uint32_t EmitFPMedTri32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	if (state.profile.relaxed_float) {
		// The smallest pairwise maximum; a NaN source gives the minimum of the other two.
		const auto ab = EmitMinMaxF32Value(state, a, b, true);
		const auto bc = EmitMinMaxF32Value(state, b, c, true);
		const auto ac = EmitMinMaxF32Value(state, a, c, true);
		return EmitMinMaxF32Value(state, EmitMinMaxF32Value(state, ab, bc, false), ac, false);
	}
	const auto a_bits = EmitBitcastF32ToU32(state, a);
	const auto b_bits = EmitBitcastF32ToU32(state, b);
	const auto c_bits = EmitBitcastF32ToU32(state, c);
	// The NaN path is RDNA2 MIN3, which keeps the later source when two are NaN.
	const auto min_ba = EmitMinMaxF32(state, b, a, b_bits, a_bits, false);
	const auto min3 =
	    EmitMinMaxF32(state, c, min_ba, c_bits, EmitBitcastF32ToU32(state, min_ba), false);
	const auto max_ab = EmitMinMaxF32(state, a, b, a_bits, b_bits, true);
	const auto max3 =
	    EmitMinMaxF32(state, max_ab, c, EmitBitcastF32ToU32(state, max_ab), c_bits, true);
	// RDNA2 compares numerically against MAX3, including ties between signed zeros, then takes
	// V_MAX_F32 of the other two sources in their original order.
	const auto a_is_max = Binary(state, spv::OpFOrdEqual, TypeBool(state), max3, a);
	const auto b_is_max = Binary(state, spv::OpFOrdEqual, TypeBool(state), max3, b);
	const auto x        = Select(state, TypeF32(state), a_is_max, b, a);
	const auto y =
	    Select(state, TypeF32(state), EmitLogicalOrBool(state, a_is_max, b_is_max), c, b);
	const auto median = EmitMinMaxF32Value(state, x, y, true);
	const auto is_nan = [&](uint32_t value) {
		return Unary(state, spv::OpIsNan, TypeBool(state), value);
	};
	const auto any_nan =
	    EmitLogicalOrBool(state, EmitLogicalOrBool(state, is_nan(a), is_nan(b)), is_nan(c));
	return Select(state, TypeF32(state), any_nan, min3, median);
}

uint32_t EmitFlushF32DenormToSignedZero(EmitterState& state, uint32_t value) {
	if (state.profile.relaxed_float) {
		return value;
	}
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

uint32_t EmitConvertF32F16Lane(EmitterState& state, uint32_t bits, IR::Value lane) {
	// UnpackHalf2x16 reads either half directly; hosts select the half in the conversion.
	const auto unpacked = state.builder.AllocateId();
	const auto ret      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32Vector(state, 2), unpacked, GlslStd450(state),
	                          GLSLstd450UnpackHalf2x16, bits);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), ret, unpacked,
	                          lane.Resolve().U32() != 0u ? 1u : 0u);
	return ret;
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
