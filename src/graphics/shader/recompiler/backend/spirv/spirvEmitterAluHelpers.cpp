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
	F32Class cls;
	cls.bits            = bits;
	const auto abs_bits = EmitAndConstant(state, cls.bits, 0x7fffffffu);
	cls.nan  = EmitCompareU32Constant(state, spv::OpUGreaterThan, abs_bits, 0x7f800000u);
	cls.zero = EmitCompareU32Constant(state, spv::OpIEqual, abs_bits, 0);
	return cls;
}

F32Class EmitClassifyF32(EmitterState& state, uint32_t value) {
	return EmitClassifyF32Bits(state, EmitBitcastF32ToU32(state, value));
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

uint32_t EmitMinMaxF32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto lhs_class = EmitClassifyF32(state, lhs);
	const auto rhs_class = EmitClassifyF32(state, rhs);

	const auto numeric_cond = state.builder.AllocateId();
	state.builder.AddFunction(max_value ? spv::OpFOrdGreaterThanEqual : spv::OpFOrdLessThan,
	                          TypeBool(state), numeric_cond, lhs, rhs);
	const auto ordered_bits =
	    EmitSelectValueU32(state, numeric_cond, lhs_class.bits, rhs_class.bits);

	const auto both_zero    = EmitLogicalAndBool(state, lhs_class.zero, rhs_class.zero);
	const auto zero_bits    = max_value ? EmitAndU32(state, lhs_class.bits, rhs_class.bits)
	                                    : EmitOrU32(state, lhs_class.bits, rhs_class.bits);
	const auto numeric_bits = EmitSelectValueU32(state, both_zero, zero_bits, ordered_bits);

	const auto rhs_nan_bits =
	    EmitSelectValueU32(state, rhs_class.nan, lhs_class.bits, numeric_bits);
	const auto result_bits = EmitSelectValueU32(state, lhs_class.nan, rhs_class.bits, rhs_nan_bits);
	return EmitBitcastU32ToF32(state, result_bits);
}

uint32_t EmitFlushF32DenormToSignedZero(EmitterState& state, uint32_t value) {
	const auto bits      = state.builder.AllocateId();
	const auto abs_bits  = state.builder.AllocateId();
	const auto sign_bits = state.builder.AllocateId();
	const auto subnormal = state.builder.AllocateId();
	const auto selected  = state.builder.AllocateId();
	const auto ret       = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, value);
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), abs_bits, bits,
	                          ConstantU32(state, 0x7fffffffu));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), sign_bits, bits,
	                          ConstantU32(state, 0x80000000u));
	state.builder.AddFunction(spv::OpULessThan, TypeBool(state), subnormal, abs_bits,
	                          ConstantU32(state, 0x00800000u));
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), selected, subnormal, sign_bits, bits);
	state.builder.AddFunction(spv::OpBitcast, TypeF32(state), ret, selected);
	return ret;
}

uint32_t EmitTrigCycleF32(EmitterState& state, uint32_t src, bool preserve_signed_zero) {
	const auto fract        = state.builder.AllocateId();
	const auto bits         = state.builder.AllocateId();
	const auto abs_bits     = state.builder.AllocateId();
	const auto large        = state.builder.AllocateId();
	const auto finite       = state.builder.AllocateId();
	const auto large_finite = state.builder.AllocateId();
	const auto reduced      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), fract, GlslStd450(state),
	                          GLSLstd450Fract, src);
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, src);
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), abs_bits, bits,
	                          ConstantU32(state, 0x7fffffffu));
	state.builder.AddFunction(spv::OpUGreaterThanEqual, TypeBool(state), large, abs_bits,
	                          ConstantU32(state, 0x4b000000u));
	state.builder.AddFunction(spv::OpULessThan, TypeBool(state), finite, abs_bits,
	                          ConstantU32(state, 0x7f800000u));
	state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), large_finite, large, finite);
	state.builder.AddFunction(spv::OpSelect, TypeF32(state), reduced, large_finite,
	                          ConstantF32(state, 0), fract);
	if (!preserve_signed_zero) {
		return reduced;
	}
	const auto zero = state.builder.AllocateId();
	const auto ret  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), zero, abs_bits,
	                          ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpSelect, TypeF32(state), ret, zero, src, reduced);
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
