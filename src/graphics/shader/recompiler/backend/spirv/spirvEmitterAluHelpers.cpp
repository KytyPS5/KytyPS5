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

static uint32_t EmitClassMaskBits(EmitterState& state, uint32_t bits, uint32_t mask, bool half) {
	const uint32_t top       = half ? 0x7fffu : 0x7fffffffu;
	const uint32_t inf       = half ? 0x7c00u : 0x7f800000u;
	const uint32_t quiet     = half ? 0x7e00u : 0x7fc00000u;
	const uint32_t normal    = half ? 0x0400u : 0x00800000u;
	const auto     magnitude = EmitAndConstant(state, bits, top);
	const auto     step      = [&](spv::Op opcode, uint32_t limit) {
		return Select(state, TypeU32(state),
		              EmitCompareU32Constant(state, opcode, magnitude, limit),
		              ConstantU32(state, 1), ConstantU32(state, 0));
	};
	const auto kind = EmitAddU32(
	    state, EmitAddU32(state, step(spv::OpINotEqual, 0), step(spv::OpUGreaterThanEqual, normal)),
	    step(spv::OpUGreaterThanEqual, inf));
	const auto negative =
	    EmitCompareU32Constant(state, spv::OpINotEqual, EmitAndConstant(state, bits, top + 1u), 0);
	const auto finite =
	    Select(state, TypeU32(state), negative,
	           Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 5), kind),
	           EmitAddU32(state, kind, ConstantU32(state, 6)));
	const auto nan = EmitCompareU32Constant(state, spv::OpUGreaterThan, magnitude, inf);
	const auto index =
	    Select(state, TypeU32(state), nan, step(spv::OpUGreaterThanEqual, quiet), finite);
	const auto selected = EmitAndConstant(
	    state, Binary(state, spv::OpShiftRightLogical, TypeU32(state), mask, index), 1);
	return EmitCompareU32Constant(state, spv::OpINotEqual, selected, 0);
}

static uint32_t EmitConstantClassMask(EmitterState& state, uint32_t bits, uint32_t mask,
                                      bool half) {
	const uint32_t inf    = half ? 0x7c00u : 0x7f800000u;
	const uint32_t quiet  = half ? 0x7e00u : 0x7fc00000u;
	const uint32_t top    = half ? 0x7fffu : 0x7fffffffu;
	const uint32_t normal = half ? 0x0400u : 0x00800000u;

	const std::array<std::array<uint32_t, 2>, 6> ranges {{{0, 0},
	                                                      {1, normal - 1},
	                                                      {normal, inf - 1},
	                                                      {inf, inf},
	                                                      {inf + 1, quiet - 1},
	                                                      {quiet, top}}};
	static constexpr std::array<uint32_t, 6> negative {5, 4, 3, 2, 0, 1};
	static constexpr std::array<uint32_t, 6> positive {6, 7, 8, 9, 0, 1};

	const auto magnitude = EmitAndConstant(state, bits, top);
	const auto selected  = [&](const std::array<uint32_t, 6>& classes) {
		uint32_t match = 0;
		for (size_t first = 0; first < ranges.size(); first++) {
			if ((mask >> classes[first] & 1u) == 0) continue;
			auto last = first;
			while (last + 1 < ranges.size() && (mask >> classes[last + 1] & 1u) != 0)
				last++;
			const auto lo   = ranges[first][0];
			const auto hi   = ranges[last][1];
			uint32_t   test = ConstantBool(state, true);
			if (lo == hi) {
				test = EmitCompareU32Constant(state, spv::OpIEqual, magnitude, lo);
			} else if (lo != 0 && hi != top) {
				test = EmitLogicalAndBool(
				    state, EmitCompareU32Constant(state, spv::OpUGreaterThanEqual, magnitude, lo),
				    EmitCompareU32Constant(state, spv::OpULessThanEqual, magnitude, hi));
			} else if (lo != 0) {
				test = EmitCompareU32Constant(state, spv::OpUGreaterThanEqual, magnitude, lo);
			} else if (hi != top) {
				test = EmitCompareU32Constant(state, spv::OpULessThanEqual, magnitude, hi);
			}
			match = match == 0 ? test : EmitLogicalOrBool(state, match, test);
			first = last;
		}
		return match == 0 ? ConstantBool(state, false) : match;
	};
	const auto same_for_both_signs = [&] {
		for (size_t i = 0; i < ranges.size(); i++) {
			if ((mask >> negative[i] & 1u) != (mask >> positive[i] & 1u)) return false;
		}
		return true;
	}();
	if (same_for_both_signs) {
		return selected(positive);
	}
	const auto sign =
	    EmitCompareU32Constant(state, spv::OpINotEqual, EmitAndConstant(state, bits, top + 1u), 0);
	return Select(state, TypeBool(state), sign, selected(negative), selected(positive));
}

static uint32_t EmitClassMask(ValueEmitContext& ctx, uint32_t bits, IR::Value mask, bool half) {
	mask = mask.Resolve();
	if (mask.IsImmediate()) {
		return EmitConstantClassMask(ctx.state, bits, mask.U32() & 0x3ffu, half);
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
	if (state.profile.native_float_min_max) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpExtInst, TypeF32(state), result, GlslStd450(state),
		                          max_value ? GLSLstd450NMax : GLSLstd450NMin, lhs, rhs);
		return result;
	}
	return EmitMinMaxF32(state, lhs, rhs, EmitBitcastF32ToU32(state, lhs),
	                     EmitBitcastF32ToU32(state, rhs), max_value);
}

uint32_t EmitFPMedTri32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
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
