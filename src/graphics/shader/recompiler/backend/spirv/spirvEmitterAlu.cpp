#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

struct Pair {
	uint32_t low  = 0;
	uint32_t high = 0;
};

Pair ExtractPair(EmitterState& state, uint32_t value) {
	Pair result {state.builder.AllocateId(), state.builder.AllocateId()};
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), result.low, value, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), result.high, value, 1);
	return result;
}

uint32_t MakePair(EmitterState& state, uint32_t low, uint32_t high) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeU64(state), result, low, high);
	return result;
}

uint32_t CompareEqual64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value,
                        bool not_equal) {
	const auto compare = Binary(state, not_equal ? spv::OpINotEqual : spv::OpIEqual,
	                            TypeBoolVector(state, 2), lhs_value, rhs_value);
	return Unary(state, not_equal ? spv::OpAny : spv::OpAll, TypeBool(state), compare);
}

uint32_t CompareOrdered64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value,
                          spv::Op high_compare, spv::Op low_compare) {
	const auto lhs         = ExtractPair(state, lhs_value);
	const auto rhs         = ExtractPair(state, rhs_value);
	const auto high_equal  = Binary(state, spv::OpIEqual, TypeBool(state), lhs.high, rhs.high);
	const auto high_result = Binary(state, high_compare, TypeBool(state), lhs.high, rhs.high);
	const auto low_result  = Binary(state, low_compare, TypeBool(state), lhs.low, rhs.low);
	const auto low_path = Binary(state, spv::OpLogicalAnd, TypeBool(state), high_equal, low_result);
	return Binary(state, spv::OpLogicalOr, TypeBool(state), high_result, low_path);
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

uint32_t EmitShift64(EmitterState& state, spv::Op opcode, uint32_t value, uint32_t shift) {
	const auto pair       = ExtractPair(state, value);
	const auto amount     = EmitAndConstant(state, shift, 63u);
	const auto word_shift = EmitAndConstant(state, amount, 31u);
	const auto at_least_32 =
	    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), amount, ConstantU32(state, 32));
	const auto nonzero =
	    Binary(state, spv::OpINotEqual, TypeBool(state), amount, ConstantU32(state, 0));
	const auto carry_count = EmitAndConstant(
	    state, Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 32), word_shift), 31u);
	if (opcode == spv::OpShiftLeftLogical) {
		const auto low = Binary(state, opcode, TypeU32(state), pair.low, word_shift);
		const auto carry =
		    Select(state, TypeU32(state), nonzero,
		           Binary(state, spv::OpShiftRightLogical, TypeU32(state), pair.low, carry_count),
		           ConstantU32(state, 0));
		const auto high =
		    Binary(state, spv::OpBitwiseOr, TypeU32(state),
		           Binary(state, opcode, TypeU32(state), pair.high, word_shift), carry);
		return MakePair(state,
		                Select(state, TypeU32(state), at_least_32, ConstantU32(state, 0), low),
		                Select(state, TypeU32(state), at_least_32, low, high));
	}
	const auto high = Binary(state, opcode, TypeU32(state), pair.high, word_shift);
	const auto carry =
	    Select(state, TypeU32(state), nonzero,
	           Binary(state, spv::OpShiftLeftLogical, TypeU32(state), pair.high, carry_count),
	           ConstantU32(state, 0));
	const auto low = Binary(
	    state, spv::OpBitwiseOr, TypeU32(state),
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), pair.low, word_shift), carry);
	const auto fill = opcode == spv::OpShiftRightArithmetic
	                      ? Binary(state, opcode, TypeU32(state), pair.high, ConstantU32(state, 31))
	                      : ConstantU32(state, 0);
	return MakePair(state, Select(state, TypeU32(state), at_least_32, high, low),
	                Select(state, TypeU32(state), at_least_32, fill, high));
}

uint32_t EmitConstantShift64(EmitterState& state, spv::Op opcode, uint32_t value, uint32_t shift) {
	shift &= 63u;
	if (shift == 0u) {
		return value;
	}
	const auto pair = ExtractPair(state, value);
	if (opcode == spv::OpShiftLeftLogical) {
		if (shift < 32u) {
			const auto low  = Binary(state, spv::OpShiftLeftLogical, TypeU32(state), pair.low,
			                         ConstantU32(state, shift));
			const auto high = Binary(state, spv::OpBitwiseOr, TypeU32(state),
			                         Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
			                                pair.high, ConstantU32(state, shift)),
			                         Binary(state, spv::OpShiftRightLogical, TypeU32(state),
			                                pair.low, ConstantU32(state, 32u - shift)));
			return MakePair(state, low, high);
		}
		return MakePair(state, ConstantU32(state, 0),
		                shift == 32u ? pair.low
		                             : Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		                                      pair.low, ConstantU32(state, shift - 32u)));
	}
	if (shift < 32u) {
		const auto low = Binary(state, spv::OpBitwiseOr, TypeU32(state),
		                        Binary(state, spv::OpShiftRightLogical, TypeU32(state), pair.low,
		                               ConstantU32(state, shift)),
		                        Binary(state, spv::OpShiftLeftLogical, TypeU32(state), pair.high,
		                               ConstantU32(state, 32u - shift)));
		const auto high =
		    Binary(state, opcode, TypeU32(state), pair.high, ConstantU32(state, shift));
		return MakePair(state, low, high);
	}
	const auto high = opcode == spv::OpShiftRightArithmetic
	                      ? Binary(state, spv::OpShiftRightArithmetic, TypeU32(state), pair.high,
	                               ConstantU32(state, 31u))
	                      : ConstantU32(state, 0);
	const auto low  = shift == 32u ? pair.high
	                               : Binary(state, opcode, TypeU32(state), pair.high,
	                                        ConstantU32(state, shift - 32u));
	return MakePair(state, low, high);
}

uint32_t EmitMinMax3(EmitterState& state, uint32_t a, uint32_t b, uint32_t c, bool signed_value,
                     bool max_value) {
	const auto ab = signed_value ? EmitMinMaxI32Value(state, a, b, max_value)
	                             : EmitMinMaxU32Value(state, a, b, max_value);
	return signed_value ? EmitMinMaxI32Value(state, ab, c, max_value)
	                    : EmitMinMaxU32Value(state, ab, c, max_value);
}

uint32_t EmitMed3(EmitterState& state, uint32_t a, uint32_t b, uint32_t c, bool signed_value) {
	const auto minimum = EmitMinMax3(state, a, b, c, signed_value, false);
	const auto maximum = EmitMinMax3(state, a, b, c, signed_value, true);
	const auto ab      = Binary(state, spv::OpIAdd, TypeU32(state), a, b);
	const auto abc     = Binary(state, spv::OpIAdd, TypeU32(state), ab, c);
	return Binary(state, spv::OpISub, TypeU32(state),
	              Binary(state, spv::OpISub, TypeU32(state), abc, minimum), maximum);
}

uint32_t EmitFMinMax3(EmitterState& state, uint32_t a, uint32_t b, uint32_t c, bool max_value) {
	return EmitMinMaxF32Value(state, EmitMinMaxF32Value(state, a, b, max_value), c, max_value);
}

uint32_t EmitExt(EmitterState& state, uint32_t type, uint32_t opcode,
                 std::initializer_list<uint32_t> args) {
	const auto            result = state.builder.AllocateId();
	std::vector<uint32_t> words {spv::OpExtInst, type, result, GlslStd450(state), opcode};
	words.insert(words.end(), args.begin(), args.end());
	state.builder.AddFunction(words);
	return result;
}

uint32_t EmitF32ToU32(EmitterState& state, uint32_t src, bool signed_value) {
	const auto trunc         = EmitTruncF32Value(state, src);
	const auto converted_raw = state.builder.AllocateId();
	if (signed_value) {
		const auto converted_i = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpConvertFToS, TypeI32(state), converted_i, trunc);
		state.builder.AddFunction(spv::OpBitcast, TypeU32(state), converted_raw, converted_i);
	} else {
		state.builder.AddFunction(spv::OpConvertFToU, TypeU32(state), converted_raw, trunc);
	}
	const auto nan = EmitClassifyF32(state, src).nan;
	if (signed_value) {
		const auto below = Binary(state, spv::OpFOrdLessThanEqual, TypeBool(state), src,
		                          ConstantF32(state, 0xcf000000u));
		const auto above = Binary(state, spv::OpFOrdGreaterThanEqual, TypeBool(state), src,
		                          ConstantF32(state, 0x4f000000u));
		const auto high =
		    Select(state, TypeU32(state), above, ConstantU32(state, 0x7fffffffu), converted_raw);
		const auto low =
		    Select(state, TypeU32(state), below, ConstantU32(state, 0x80000000u), high);
		return Select(state, TypeU32(state), nan, ConstantU32(state, 0), low);
	}
	const auto below =
	    Binary(state, spv::OpFOrdLessThanEqual, TypeBool(state), src, ConstantF32(state, 0));
	const auto above = Binary(state, spv::OpFOrdGreaterThanEqual, TypeBool(state), src,
	                          ConstantF32(state, 0x4f800000u));
	const auto zero  = Binary(state, spv::OpLogicalOr, TypeBool(state), nan, below);
	const auto high =
	    Select(state, TypeU32(state), above, ConstantU32(state, 0xffffffffu), converted_raw);
	return Select(state, TypeU32(state), zero, ConstantU32(state, 0), high);
}

} // namespace
uint32_t EmitFPMedTri32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	const auto min_ab   = EmitMinMaxF32Value(state, a, b, false);
	const auto min3     = EmitMinMaxF32Value(state, min_ab, c, false);
	const auto max_ab   = EmitMinMaxF32Value(state, a, b, true);
	const auto high_min = EmitMinMaxF32Value(state, max_ab, c, false);
	const auto median   = EmitMinMaxF32Value(state, min_ab, high_min, true);
	const auto nan_ab   = Binary(state, spv::OpLogicalOr, TypeBool(state),
	                             EmitClassifyF32(state, a).nan, EmitClassifyF32(state, b).nan);
	const auto any_nan =
	    Binary(state, spv::OpLogicalOr, TypeBool(state), nan_ab, EmitClassifyF32(state, c).nan);
	return Select(state, TypeF32(state), any_nan, min3, median);
}

uint32_t EmitFindUMsb64(EmitterState& state, uint32_t value) {
	const auto pair   = ExtractPair(state, value);
	const auto high_i = state.builder.AllocateId();
	const auto low_i  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeI32(state), high_i, GlslStd450(state),
	                          GLSLstd450FindUMsb, pair.high);
	state.builder.AddFunction(spv::OpExtInst, TypeI32(state), low_i, GlslStd450(state),
	                          GLSLstd450FindUMsb, pair.low);
	const auto high = Unary(state, spv::OpBitcast, TypeU32(state), high_i);
	const auto low  = Unary(state, spv::OpBitcast, TypeU32(state), low_i);
	const auto high_nonzero =
	    Binary(state, spv::OpINotEqual, TypeBool(state), pair.high, ConstantU32(state, 0));
	return Select(state, TypeU32(state), high_nonzero,
	              Binary(state, spv::OpIAdd, TypeU32(state), high, ConstantU32(state, 32)), low);
}

uint32_t EmitIMul64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value) {
	const auto lhs   = ExtractPair(state, lhs_value);
	const auto rhs   = ExtractPair(state, rhs_value);
	const auto low   = Binary(state, spv::OpIMul, TypeU32(state), lhs.low, rhs.low);
	const auto high0 = EmitMulHigh(state, lhs.low, rhs.low, false);
	const auto high1 = Binary(state, spv::OpIMul, TypeU32(state), lhs.low, rhs.high);
	const auto high2 = Binary(state, spv::OpIMul, TypeU32(state), lhs.high, rhs.low);
	return MakePair(state, low,
	                Binary(state, spv::OpIAdd, TypeU32(state),
	                       Binary(state, spv::OpIAdd, TypeU32(state), high0, high1), high2));
}

uint32_t EmitISub64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value) {
	const auto lhs    = ExtractPair(state, lhs_value);
	const auto rhs    = ExtractPair(state, rhs_value);
	const auto low    = Binary(state, spv::OpISub, TypeU32(state), lhs.low, rhs.low);
	const auto borrow = Binary(state, spv::OpULessThan, TypeBool(state), lhs.low, rhs.low);
	const auto borrow_u32 =
	    Select(state, TypeU32(state), borrow, ConstantU32(state, 1), ConstantU32(state, 0));
	const auto high0 = Binary(state, spv::OpISub, TypeU32(state), lhs.high, rhs.high);
	const auto high  = Binary(state, spv::OpISub, TypeU32(state), high0, borrow_u32);
	return MakePair(state, low, high);
}

uint32_t EmitIAdd64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value) {
	const auto lhs      = ExtractPair(state, lhs_value);
	const auto rhs      = ExtractPair(state, rhs_value);
	const auto low_pair = state.builder.AllocateId();
	const auto low      = state.builder.AllocateId();
	const auto carry    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIAddCarry, TypeU32Pair(state), low_pair, lhs.low, rhs.low);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, low_pair, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), carry, low_pair, 1);
	const auto high0 = Binary(state, spv::OpIAdd, TypeU32(state), lhs.high, rhs.high);
	const auto high  = Binary(state, spv::OpIAdd, TypeU32(state), high0, carry);
	return MakePair(state, low, high);
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

uint32_t EmitConvertF32S32(EmitterState& state, uint32_t arg0) {
	const auto signed_value = Unary(state, spv::OpBitcast, TypeI32(state), arg0);
	return EmitNative<spv::OpConvertSToF, IR::Type::F32>(state, signed_value);
}

uint32_t EmitCompositeExtractU64(EmitterState& state, uint32_t arg0, IR::Value arg1) {
	return EmitNative<spv::OpCompositeExtract, IR::Type::U32>(state, arg0, arg1.U32());
}

uint32_t EmitPackFloat2x16Rtz(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	const auto low  = EmitF32ToF16RtzBits(state, arg0);
	const auto high = Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
	                         EmitF32ToF16RtzBits(state, arg1), ConstantU32(state, 16));
	return Binary(state, spv::OpBitwiseOr, TypeU32(state), low, high);
}

uint32_t EmitFPSaturate32(EmitterState& state, uint32_t arg0) {
	return EmitExt(state, TypeF32(state), GLSLstd450FClamp,
	               {arg0, ConstantF32(state, 0), ConstantF32(state, 0x3f800000u)});
}

uint32_t EmitSMulHi(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMulHigh(state, arg0, arg1, true);
}

uint32_t EmitUMulHi(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMulHigh(state, arg0, arg1, false);
}

uint32_t EmitIAbs32(EmitterState& state, uint32_t arg0) {
	const auto value = arg0;
	const auto neg   = Unary(state, spv::OpSNegate, TypeU32(state), value);
	const auto negative =
	    Binary(state, spv::OpSLessThan, TypeBool(state), value, ConstantU32(state, 0));
	return Select(state, TypeU32(state), negative, neg, value);
}

uint32_t EmitShiftLeftLogical64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1) {
	auto& state = ctx.state;
	return arg1.Resolve().IsImmediate()
	           ? EmitConstantShift64(state, spv::OpShiftLeftLogical, arg0, arg1.Resolve().U32())
	           : EmitShift64(state, spv::OpShiftLeftLogical, arg0, ctx.Def(arg1));
}

uint32_t EmitShiftRightLogical64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1) {
	auto& state = ctx.state;
	return arg1.Resolve().IsImmediate()
	           ? EmitConstantShift64(state, spv::OpShiftRightLogical, arg0, arg1.Resolve().U32())
	           : EmitShift64(state, spv::OpShiftRightLogical, arg0, ctx.Def(arg1));
}

uint32_t EmitShiftRightArithmetic64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1) {
	auto& state = ctx.state;
	return arg1.Resolve().IsImmediate()
	           ? EmitConstantShift64(state, spv::OpShiftRightArithmetic, arg0, arg1.Resolve().U32())
	           : EmitShift64(state, spv::OpShiftRightArithmetic, arg0, ctx.Def(arg1));
}

uint32_t EmitBitwiseAnd64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return Binary(state, spv::OpBitwiseAnd, TypeU64(state), arg0, arg1);
}

uint32_t EmitBitCount64(EmitterState& state, uint32_t arg0) {
	const auto pair = ExtractPair(state, Unary(state, spv::OpBitCount, TypeU64(state), arg0));
	return Binary(state, spv::OpIAdd, TypeU32(state), pair.low, pair.high);
}

uint32_t EmitFindILsb32(EmitterState& state, uint32_t arg0) {
	const auto value = EmitExt(state, TypeI32(state), GLSLstd450FindILsb, {arg0});
	return Unary(state, spv::OpBitcast, TypeU32(state), value);
}

uint32_t EmitFindUMsb32(EmitterState& state, uint32_t arg0) {
	const auto value = EmitExt(state, TypeI32(state), GLSLstd450FindUMsb, {arg0});
	return Unary(state, spv::OpBitcast, TypeU32(state), value);
}

uint32_t EmitSMin32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxI32Value(state, arg0, arg1, false);
}

uint32_t EmitSMax32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxI32Value(state, arg0, arg1, true);
}

uint32_t EmitUMin32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxU32Value(state, arg0, arg1, false);
}

uint32_t EmitUMax32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxU32Value(state, arg0, arg1, true);
}

uint32_t EmitSMinTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitMinMax3(state, arg0, arg1, arg2, true, false);
}

uint32_t EmitSMaxTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitMinMax3(state, arg0, arg1, arg2, true, true);
}

uint32_t EmitUMinTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitMinMax3(state, arg0, arg1, arg2, false, false);
}

uint32_t EmitUMaxTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitMinMax3(state, arg0, arg1, arg2, false, true);
}

uint32_t EmitSMedTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitMed3(state, arg0, arg1, arg2, true);
}

uint32_t EmitUMedTri32(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	return EmitMed3(state, arg0, arg1, arg2, false);
}

uint32_t EmitIEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return CompareEqual64(state, arg0, arg1, false);
}

uint32_t EmitINotEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return CompareEqual64(state, arg0, arg1, true);
}

uint32_t EmitULessThan64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return CompareOrdered64(state, arg0, arg1, spv::OpULessThan, spv::OpULessThan);
}

uint32_t EmitSLessThan64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return CompareOrdered64(state, arg0, arg1, spv::OpSLessThan, spv::OpULessThan);
}

uint32_t EmitUGreaterThan64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return CompareOrdered64(state, arg0, arg1, spv::OpUGreaterThan, spv::OpUGreaterThan);
}

uint32_t EmitFPIsNan32(EmitterState& state, uint32_t arg0) {
	return EmitNative<spv::OpFUnordNotEqual, IR::Type::U1>(state, arg0, arg0);
}

uint32_t EmitFPMin32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxF32Value(state, arg0, arg1, false);
}

uint32_t EmitFPMax32(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return EmitMinMaxF32Value(state, arg0, arg1, true);
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
	return EmitExt(state, TypeF32(state), GLSLstd450Exp2,
	               {EmitFlushF32DenormToSignedZero(state, arg0)});
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
