#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitSelectValueU32(EmitterState& state, uint32_t cond, uint32_t true_value,
                            uint32_t false_value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, TypeU32(state), ret, cond, true_value, false_value});
	return ret;
}

uint32_t EmitAndConstant(EmitterState& state, uint32_t value, uint32_t mask);

void EmitShiftLeftLogicalU64Values(EmitterState& state, uint32_t low, uint32_t high, uint32_t shift,
                                   uint32_t& out_low, uint32_t& out_high) {
	const auto amount           = EmitAndConstant(state, shift, 63u);
	const auto dword_shift      = EmitAndConstant(state, amount, 31u);
	const auto less32           = state.builder.AllocateId();
	const auto non_zero         = state.builder.AllocateId();
	const auto carry_count_base = state.builder.AllocateId();
	const auto carry_count      = state.builder.AllocateId();
	const auto low_part         = state.builder.AllocateId();
	const auto high_part        = state.builder.AllocateId();
	const auto carry_part       = state.builder.AllocateId();
	const auto carry            = state.builder.AllocateId();
	const auto high_less        = state.builder.AllocateId();
	const auto high_ge          = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpULessThan, TypeBool(state), less32, amount, ConstantU32(state, 32)});
	state.builder.AddFunction(
	    {OpINotEqual, TypeBool(state), non_zero, amount, ConstantU32(state, 0)});
	state.builder.AddFunction(
	    {OpISub, TypeU32(state), carry_count_base, ConstantU32(state, 32), dword_shift});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), carry_count, carry_count_base, ConstantU32(state, 31)});
	state.builder.AddFunction({OpShiftLeftLogical, TypeU32(state), low_part, low, dword_shift});
	state.builder.AddFunction({OpShiftLeftLogical, TypeU32(state), high_part, high, dword_shift});
	state.builder.AddFunction({OpShiftRightLogical, TypeU32(state), carry_part, low, carry_count});
	state.builder.AddFunction(
	    {OpSelect, TypeU32(state), carry, non_zero, carry_part, ConstantU32(state, 0)});
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), high_less, high_part, carry});
	state.builder.AddFunction({OpShiftLeftLogical, TypeU32(state), high_ge, low, dword_shift});
	out_low  = EmitSelectValueU32(state, less32, low_part, ConstantU32(state, 0));
	out_high = EmitSelectValueU32(state, less32, high_less, high_ge);
}

void EmitShiftRightLogicalU64Values(EmitterState& state, uint32_t low, uint32_t high,
                                    uint32_t shift, uint32_t& out_low, uint32_t& out_high) {
	const auto amount           = EmitAndConstant(state, shift, 63u);
	const auto dword_shift      = EmitAndConstant(state, amount, 31u);
	const auto less32           = state.builder.AllocateId();
	const auto non_zero         = state.builder.AllocateId();
	const auto carry_count_base = state.builder.AllocateId();
	const auto carry_count      = state.builder.AllocateId();
	const auto low_part         = state.builder.AllocateId();
	const auto high_part        = state.builder.AllocateId();
	const auto carry_part       = state.builder.AllocateId();
	const auto carry            = state.builder.AllocateId();
	const auto low_less         = state.builder.AllocateId();
	const auto low_ge           = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpULessThan, TypeBool(state), less32, amount, ConstantU32(state, 32)});
	state.builder.AddFunction(
	    {OpINotEqual, TypeBool(state), non_zero, amount, ConstantU32(state, 0)});
	state.builder.AddFunction(
	    {OpISub, TypeU32(state), carry_count_base, ConstantU32(state, 32), dword_shift});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), carry_count, carry_count_base, ConstantU32(state, 31)});
	state.builder.AddFunction({OpShiftRightLogical, TypeU32(state), low_part, low, dword_shift});
	state.builder.AddFunction({OpShiftRightLogical, TypeU32(state), high_part, high, dword_shift});
	state.builder.AddFunction({OpShiftLeftLogical, TypeU32(state), carry_part, high, carry_count});
	state.builder.AddFunction(
	    {OpSelect, TypeU32(state), carry, non_zero, carry_part, ConstantU32(state, 0)});
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), low_less, low_part, carry});
	state.builder.AddFunction({OpShiftRightLogical, TypeU32(state), low_ge, high, dword_shift});
	out_low  = EmitSelectValueU32(state, less32, low_less, low_ge);
	out_high = EmitSelectValueU32(state, less32, high_part, ConstantU32(state, 0));
}

uint32_t EmitAndConstant(EmitterState& state, uint32_t value, uint32_t mask) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitwiseAnd, TypeU32(state), ret, value, ConstantU32(state, mask)});
	return ret;
}

uint32_t EmitShiftRightConstant(EmitterState& state, uint32_t value, uint32_t shift) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpShiftRightLogical, TypeU32(state), ret, value, ConstantU32(state, shift)});
	return ret;
}

uint32_t EmitOrU32(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), ret, lhs, rhs});
	return ret;
}

uint32_t EmitCompareU32Constant(EmitterState& state, uint32_t opcode, uint32_t value,
                                uint32_t constant) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({opcode, TypeBool(state), ret, value, ConstantU32(state, constant)});
	return ret;
}

uint32_t EmitSubConstantMinusU32(EmitterState& state, uint32_t constant, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpISub, TypeU32(state), ret, ConstantU32(state, constant), value});
	return ret;
}

uint32_t EmitF32ToF16RtzBits(EmitterState& state, uint32_t f32) {
	const auto bits = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, TypeU32(state), bits, f32});

	const auto sign = EmitAndConstant(state, EmitShiftRightConstant(state, bits, 16), 0x8000u);
	const auto exp  = EmitAndConstant(state, EmitShiftRightConstant(state, bits, 23), 0xffu);
	const auto mant = EmitAndConstant(state, bits, 0x007fffffu);

	const auto half_exp       = state.builder.AllocateId();
	const auto normal_exp     = state.builder.AllocateId();
	const auto normal_mant    = EmitShiftRightConstant(state, mant, 13);
	const auto normal_payload = state.builder.AllocateId();
	const auto normal         = state.builder.AllocateId();
	state.builder.AddFunction({OpISub, TypeU32(state), half_exp, exp, ConstantU32(state, 112)});
	state.builder.AddFunction(
	    {OpShiftLeftLogical, TypeU32(state), normal_exp, half_exp, ConstantU32(state, 10)});
	state.builder.AddFunction(
	    {OpBitwiseOr, TypeU32(state), normal_payload, normal_exp, normal_mant});
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), normal, sign, normal_payload});

	const auto mant_with_hidden = EmitOrU32(state, mant, ConstantU32(state, 0x00800000u));
	const auto raw_sub_shift    = EmitSubConstantMinusU32(state, 126, exp);
	const auto exp_lt_103       = EmitCompareU32Constant(state, OpULessThan, exp, 103);
	const auto exp_gt_112       = EmitCompareU32Constant(state, OpUGreaterThan, exp, 112);
	const auto sub_shift_low =
	    EmitSelectValueU32(state, exp_lt_103, ConstantU32(state, 31), raw_sub_shift);
	const auto sub_shift =
	    EmitSelectValueU32(state, exp_gt_112, ConstantU32(state, 14), sub_shift_low);
	const auto sub_mant  = state.builder.AllocateId();
	const auto subnormal = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpShiftRightLogical, TypeU32(state), sub_mant, mant_with_hidden, sub_shift});
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), subnormal, sign, sub_mant});

	const auto nan_payload =
	    EmitOrU32(state, EmitShiftRightConstant(state, mant, 13), ConstantU32(state, 0x0200u));
	const auto nan =
	    EmitOrU32(state, sign, EmitOrU32(state, ConstantU32(state, 0x7c00u), nan_payload));
	const auto inf        = EmitOrU32(state, sign, ConstantU32(state, 0x7c00u));
	const auto max_finite = EmitOrU32(state, sign, ConstantU32(state, 0x7bffu));
	const auto mant_zero  = EmitCompareU32Constant(state, OpIEqual, mant, 0);
	const auto special    = EmitSelectValueU32(state, mant_zero, inf, nan);

	const auto exp_le_112 = EmitCompareU32Constant(state, OpULessThanEqual, exp, 112);
	const auto exp_ge_143 = EmitCompareU32Constant(state, OpUGreaterThanEqual, exp, 143);
	const auto exp_eq_255 = EmitCompareU32Constant(state, OpIEqual, exp, 255);
	const auto finite0    = EmitSelectValueU32(state, exp_le_112, subnormal, normal);
	const auto finite1    = EmitSelectValueU32(state, exp_lt_103, sign, finite0);
	const auto finite2    = EmitSelectValueU32(state, exp_ge_143, max_finite, finite1);
	return EmitAndConstant(state, EmitSelectValueU32(state, exp_eq_255, special, finite2), 0xffffu);
}

uint32_t EmitMinMaxU32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto cond = state.builder.AllocateId();
	const auto ret  = state.builder.AllocateId();
	state.builder.AddFunction(
	    {max_value ? OpUGreaterThan : OpULessThan, TypeBool(state), cond, lhs, rhs});
	state.builder.AddFunction({OpSelect, TypeU32(state), ret, cond, lhs, rhs});
	return ret;
}

uint32_t EmitMinMaxI32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto cond = state.builder.AllocateId();
	const auto ret  = state.builder.AllocateId();
	state.builder.AddFunction(
	    {max_value ? OpSGreaterThan : OpSLessThan, TypeBool(state), cond, lhs, rhs});
	state.builder.AddFunction({OpSelect, TypeU32(state), ret, cond, lhs, rhs});
	return ret;
}

uint32_t EmitBitcastF32ToU32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, TypeU32(state), ret, value});
	return ret;
}

uint32_t EmitBitcastU32ToF32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, TypeF32(state), ret, value});
	return ret;
}

uint32_t EmitAndU32(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitwiseAnd, TypeU32(state), ret, lhs, rhs});
	return ret;
}

uint32_t EmitLogicalAndBool(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalAnd, TypeBool(state), ret, lhs, rhs});
	return ret;
}

uint32_t EmitLogicalOrBool(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalOr, TypeBool(state), ret, lhs, rhs});
	return ret;
}

uint32_t EmitLogicalNotBool(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalNot, TypeBool(state), ret, value});
	return ret;
}

F32Class EmitClassifyF32(EmitterState& state, uint32_t value) {
	F32Class cls;
	cls.bits                 = EmitBitcastF32ToU32(state, value);
	const auto abs_bits      = EmitAndConstant(state, cls.bits, 0x7fffffffu);
	const auto exponent_bits = EmitAndConstant(state, abs_bits, 0x7f800000u);
	const auto mantissa_bits = EmitAndConstant(state, abs_bits, 0x007fffffu);
	const auto exponent_max  = EmitCompareU32Constant(state, OpIEqual, exponent_bits, 0x7f800000u);
	const auto mantissa_nonzero = EmitCompareU32Constant(state, OpINotEqual, mantissa_bits, 0);
	cls.nan  = EmitLogicalAndBool(state, exponent_max, mantissa_nonzero);
	cls.zero = EmitCompareU32Constant(state, OpIEqual, abs_bits, 0);
	return cls;
}

uint32_t EmitClassMaskBitMatch(EmitterState& state, uint32_t mask, uint32_t bit,
                               uint32_t class_match) {
	const auto selected =
	    EmitCompareU32Constant(state, OpINotEqual, EmitAndConstant(state, mask, 1u << bit), 0);
	return EmitLogicalAndBool(state, selected, class_match);
}

uint32_t EmitClassMaskF32(EmitterState& state, uint32_t value, uint32_t mask) {
	const auto bits          = EmitBitcastF32ToU32(state, value);
	const auto sign_bits     = EmitAndConstant(state, bits, 0x80000000u);
	const auto abs_bits      = EmitAndConstant(state, bits, 0x7fffffffu);
	const auto exponent_bits = EmitAndConstant(state, abs_bits, 0x7f800000u);
	const auto mantissa_bits = EmitAndConstant(state, abs_bits, 0x007fffffu);
	const auto quiet_bits    = EmitAndConstant(state, mantissa_bits, 0x00400000u);

	const auto sign             = EmitCompareU32Constant(state, OpINotEqual, sign_bits, 0);
	const auto positive         = EmitLogicalNotBool(state, sign);
	const auto exponent_zero    = EmitCompareU32Constant(state, OpIEqual, exponent_bits, 0);
	const auto exponent_nonzero = EmitLogicalNotBool(state, exponent_zero);
	const auto exponent_inf = EmitCompareU32Constant(state, OpIEqual, exponent_bits, 0x7f800000u);
	const auto finite_exponent  = EmitLogicalNotBool(state, exponent_inf);
	const auto mantissa_zero    = EmitCompareU32Constant(state, OpIEqual, mantissa_bits, 0);
	const auto mantissa_nonzero = EmitLogicalNotBool(state, mantissa_zero);
	const auto quiet            = EmitCompareU32Constant(state, OpINotEqual, quiet_bits, 0);

	const auto nan_common = EmitLogicalAndBool(state, exponent_inf, mantissa_nonzero);
	const auto snan       = EmitLogicalAndBool(state, nan_common, EmitLogicalNotBool(state, quiet));
	const auto qnan       = EmitLogicalAndBool(state, nan_common, quiet);
	const auto inf        = EmitLogicalAndBool(state, exponent_inf, mantissa_zero);
	const auto normal     = EmitLogicalAndBool(state, exponent_nonzero, finite_exponent);
	const auto denorm     = EmitLogicalAndBool(state, exponent_zero, mantissa_nonzero);
	const auto zero       = EmitCompareU32Constant(state, OpIEqual, abs_bits, 0);

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

uint32_t EmitMinMaxF32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto lhs_class = EmitClassifyF32(state, lhs);
	const auto rhs_class = EmitClassifyF32(state, rhs);

	const auto numeric_cond = state.builder.AllocateId();
	state.builder.AddFunction({max_value ? OpFOrdGreaterThanEqual : OpFOrdLessThan, TypeBool(state),
	                           numeric_cond, lhs, rhs});
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

uint32_t EmitTruncF32Value(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpExtInst, TypeF32(state), ret, GlslStd450(state), GlslTrunc, value});
	return ret;
}

uint32_t EmitFlushF32DenormToSignedZero(EmitterState& state, uint32_t value) {
	const auto bits      = state.builder.AllocateId();
	const auto abs_bits  = state.builder.AllocateId();
	const auto sign_bits = state.builder.AllocateId();
	const auto non_zero  = state.builder.AllocateId();
	const auto subnormal = state.builder.AllocateId();
	const auto flush     = state.builder.AllocateId();
	const auto selected  = state.builder.AllocateId();
	const auto ret       = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, TypeU32(state), bits, value});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), abs_bits, bits, ConstantU32(state, 0x7fffffffu)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), sign_bits, bits, ConstantU32(state, 0x80000000u)});
	state.builder.AddFunction(
	    {OpINotEqual, TypeBool(state), non_zero, abs_bits, ConstantU32(state, 0)});
	state.builder.AddFunction(
	    {OpULessThan, TypeBool(state), subnormal, abs_bits, ConstantU32(state, 0x00800000u)});
	state.builder.AddFunction({OpLogicalAnd, TypeBool(state), flush, non_zero, subnormal});
	state.builder.AddFunction({OpSelect, TypeU32(state), selected, flush, sign_bits, bits});
	state.builder.AddFunction({OpBitcast, TypeF32(state), ret, selected});
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
	state.builder.AddFunction(
	    {OpExtInst, TypeF32(state), fract, GlslStd450(state), GlslFract, src});
	state.builder.AddFunction({OpBitcast, TypeU32(state), bits, src});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), abs_bits, bits, ConstantU32(state, 0x7fffffffu)});
	state.builder.AddFunction(
	    {OpUGreaterThanEqual, TypeBool(state), large, abs_bits, ConstantU32(state, 0x4b000000u)});
	state.builder.AddFunction(
	    {OpULessThan, TypeBool(state), finite, abs_bits, ConstantU32(state, 0x7f800000u)});
	state.builder.AddFunction({OpLogicalAnd, TypeBool(state), large_finite, large, finite});
	state.builder.AddFunction(
	    {OpSelect, TypeF32(state), reduced, large_finite, ConstantF32(state, 0), fract});
	if (!preserve_signed_zero) {
		return reduced;
	}
	const auto zero = state.builder.AllocateId();
	const auto ret  = state.builder.AllocateId();
	state.builder.AddFunction({OpIEqual, TypeBool(state), zero, abs_bits, ConstantU32(state, 0)});
	state.builder.AddFunction({OpSelect, TypeF32(state), ret, zero, src, reduced});
	return ret;
}

uint32_t EmitFNegateValue(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpFNegate, TypeF32(state), ret, value});
	return ret;
}

uint32_t EmitFAbsValue(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpExtInst, TypeF32(state), ret, GlslStd450(state), GlslFAbs, value});
	return ret;
}

uint32_t EmitF16BitsToF32(EmitterState& state, uint32_t bits) {
	const auto unpacked = state.builder.AllocateId();
	const auto ret      = state.builder.AllocateId();
	state.builder.AddFunction({OpExtInst, TypeF32Vector(state, 2), unpacked, GlslStd450(state),
	                           GlslUnpackHalf2x16, bits});
	state.builder.AddFunction({OpCompositeExtract, TypeF32(state), ret, unpacked, 0});
	return ret;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
