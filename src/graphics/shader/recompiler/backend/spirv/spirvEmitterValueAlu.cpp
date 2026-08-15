#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

struct Pair {
	uint32_t low  = 0;
	uint32_t high = 0;
};

uint32_t NewUnary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, result, value});
	return result;
}

uint32_t NewBinary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t lhs,
                   uint32_t rhs) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, result, lhs, rhs});
	return result;
}

uint32_t NewSelect(EmitterState& state, uint32_t type, uint32_t condition, uint32_t true_value,
                   uint32_t false_value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, type, result, condition, true_value, false_value});
	return result;
}

Pair ExtractPair(EmitterState& state, uint32_t value) {
	Pair result {state.builder.AllocateId(), state.builder.AllocateId()};
	state.builder.AddFunction({OpCompositeExtract, state.uint_type, result.low, value, 0});
	state.builder.AddFunction({OpCompositeExtract, state.uint_type, result.high, value, 1});
	return result;
}

uint32_t MakePair(EmitterState& state, uint32_t low, uint32_t high) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, state.uint_pair_type, result, low, high});
	return result;
}

uint32_t Compare64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value,
                   uint32_t high_compare, uint32_t low_compare, bool equal_compare = false,
                   bool not_equal = false) {
	const auto lhs        = ExtractPair(state, lhs_value);
	const auto rhs        = ExtractPair(state, rhs_value);
	const auto high_equal = NewBinary(state, OpIEqual, state.bool_type, lhs.high, rhs.high);
	const auto low_equal  = NewBinary(state, OpIEqual, state.bool_type, lhs.low, rhs.low);
	if (equal_compare) {
		return NewBinary(
		    state, not_equal ? OpLogicalOr : OpLogicalAnd, state.bool_type,
		    not_equal ? NewUnary(state, OpLogicalNot, state.bool_type, high_equal) : high_equal,
		    not_equal ? NewUnary(state, OpLogicalNot, state.bool_type, low_equal) : low_equal);
	}
	const auto high_result = NewBinary(state, high_compare, state.bool_type, lhs.high, rhs.high);
	const auto low_result  = NewBinary(state, low_compare, state.bool_type, lhs.low, rhs.low);
	const auto low_path = NewBinary(state, OpLogicalAnd, state.bool_type, high_equal, low_result);
	return NewBinary(state, OpLogicalOr, state.bool_type, high_result, low_path);
}

uint32_t EmitAdd64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value) {
	const auto lhs   = ExtractPair(state, lhs_value);
	const auto rhs   = ExtractPair(state, rhs_value);
	const auto carry = EmitAddCarryValues(state, lhs.low, rhs.low, ConstantU32(state, 0));
	const auto high0 = NewBinary(state, OpIAdd, state.uint_type, lhs.high, rhs.high);
	const auto high  = NewBinary(state, OpIAdd, state.uint_type, high0, carry.carry);
	return MakePair(state, carry.sum, high);
}

uint32_t EmitSub64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value) {
	const auto lhs    = ExtractPair(state, lhs_value);
	const auto rhs    = ExtractPair(state, rhs_value);
	const auto low    = NewBinary(state, OpISub, state.uint_type, lhs.low, rhs.low);
	const auto borrow = NewBinary(state, OpULessThan, state.bool_type, lhs.low, rhs.low);
	const auto borrow_u32 =
	    NewSelect(state, state.uint_type, borrow, ConstantU32(state, 1), ConstantU32(state, 0));
	const auto high0 = NewBinary(state, OpISub, state.uint_type, lhs.high, rhs.high);
	const auto high  = NewBinary(state, OpISub, state.uint_type, high0, borrow_u32);
	return MakePair(state, low, high);
}

uint32_t EmitMulHigh(EmitterState& state, uint32_t lhs, uint32_t rhs, bool signed_value) {
	const auto operand_type = signed_value ? state.int_type : state.uint_type;
	const auto pair_type    = signed_value ? state.int_pair_type : state.uint_pair_type;
	uint32_t   lhs_operand  = lhs;
	uint32_t   rhs_operand  = rhs;
	if (signed_value) {
		lhs_operand = NewUnary(state, OpBitcast, state.int_type, lhs);
		rhs_operand = NewUnary(state, OpBitcast, state.int_type, rhs);
	}
	const auto extended = state.builder.AllocateId();
	state.builder.AddFunction({signed_value ? OpSMulExtended : OpUMulExtended, pair_type, extended,
	                           lhs_operand, rhs_operand});
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, operand_type, high, extended, 1});
	return signed_value ? NewUnary(state, OpBitcast, state.uint_type, high) : high;
}

uint32_t EmitMul64(EmitterState& state, uint32_t lhs_value, uint32_t rhs_value) {
	const auto lhs   = ExtractPair(state, lhs_value);
	const auto rhs   = ExtractPair(state, rhs_value);
	const auto low   = NewBinary(state, OpIMul, state.uint_type, lhs.low, rhs.low);
	const auto high0 = EmitMulHigh(state, lhs.low, rhs.low, false);
	const auto high1 = NewBinary(state, OpIMul, state.uint_type, lhs.low, rhs.high);
	const auto high2 = NewBinary(state, OpIMul, state.uint_type, lhs.high, rhs.low);
	return MakePair(state, low,
	                NewBinary(state, OpIAdd, state.uint_type,
	                          NewBinary(state, OpIAdd, state.uint_type, high0, high1), high2));
}

uint32_t EmitBitwise64(EmitterState& state, uint32_t opcode, uint32_t lhs_value,
                       uint32_t rhs_value) {
	const auto lhs = ExtractPair(state, lhs_value);
	const auto rhs = ExtractPair(state, rhs_value);
	return MakePair(state, NewBinary(state, opcode, state.uint_type, lhs.low, rhs.low),
	                NewBinary(state, opcode, state.uint_type, lhs.high, rhs.high));
}

uint32_t EmitShift64(EmitterState& state, uint32_t opcode, uint32_t value, uint32_t shift) {
	const auto pair = ExtractPair(state, value);
	uint32_t   low  = 0;
	uint32_t   high = 0;
	if (opcode == OpShiftLeftLogical) {
		EmitShiftLeftLogicalU64Values(state, pair.low, pair.high, shift, low, high);
	} else if (opcode == OpShiftRightLogical) {
		EmitShiftRightLogicalU64Values(state, pair.low, pair.high, shift, low, high);
	} else {
		const auto logical      = EmitShift64(state, OpShiftRightLogical, value, shift);
		const auto logical_pair = ExtractPair(state, logical);
		const auto sign =
		    NewBinary(state, OpSLessThan, state.bool_type, pair.high, ConstantU32(state, 0));
		const auto inverse =
		    NewBinary(state, OpISub, state.uint_type, ConstantU32(state, 64), shift);
		const auto fill = NewBinary(state, OpShiftLeftLogical, state.uint_type,
		                            ConstantU32(state, 0xffffffffu), inverse);
		const auto high_shift =
		    NewBinary(state, OpISub, state.uint_type, shift, ConstantU32(state, 32));
		const auto high_fill = NewSelect(
		    state, state.uint_type,
		    NewBinary(state, OpUGreaterThanEqual, state.bool_type, shift, ConstantU32(state, 32)),
		    ConstantU32(state, 0xffffffffu), fill);
		const auto fill_low =
		    NewSelect(state, state.uint_type, sign,
		              NewSelect(state, state.uint_type,
		                        NewBinary(state, OpUGreaterThanEqual, state.bool_type, shift,
		                                  ConstantU32(state, 32)),
		                        NewBinary(state, OpShiftLeftLogical, state.uint_type,
		                                  ConstantU32(state, 0xffffffffu), high_shift),
		                        ConstantU32(state, 0)),
		              ConstantU32(state, 0));
		const auto fill_high =
		    NewSelect(state, state.uint_type, sign, high_fill, ConstantU32(state, 0));
		return MakePair(
		    state, NewBinary(state, OpBitwiseOr, state.uint_type, logical_pair.low, fill_low),
		    NewBinary(state, OpBitwiseOr, state.uint_type, logical_pair.high, fill_high));
	}
	return MakePair(state, low, high);
}

uint32_t EmitFindMsb64(EmitterState& state, uint32_t value) {
	const auto pair   = ExtractPair(state, value);
	const auto high_i = state.builder.AllocateId();
	const auto low_i  = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpExtInst, state.int_type, high_i, state.glsl_std450, GlslFindUMsb, pair.high});
	state.builder.AddFunction(
	    {OpExtInst, state.int_type, low_i, state.glsl_std450, GlslFindUMsb, pair.low});
	const auto high = NewUnary(state, OpBitcast, state.uint_type, high_i);
	const auto low  = NewUnary(state, OpBitcast, state.uint_type, low_i);
	const auto high_nonzero =
	    NewBinary(state, OpINotEqual, state.bool_type, pair.high, ConstantU32(state, 0));
	return NewSelect(state, state.uint_type, high_nonzero,
	                 NewBinary(state, OpIAdd, state.uint_type, high, ConstantU32(state, 32)), low);
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
	const auto ab      = NewBinary(state, OpIAdd, state.uint_type, a, b);
	const auto abc     = NewBinary(state, OpIAdd, state.uint_type, ab, c);
	return NewBinary(state, OpISub, state.uint_type,
	                 NewBinary(state, OpISub, state.uint_type, abc, minimum), maximum);
}

uint32_t EmitFMinMax3(EmitterState& state, uint32_t a, uint32_t b, uint32_t c, bool max_value) {
	return EmitMinMaxF32Value(state, EmitMinMaxF32Value(state, a, b, max_value), c, max_value);
}

uint32_t EmitFMed3(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	const auto min_ab   = EmitMinMaxF32Value(state, a, b, false);
	const auto min3     = EmitMinMaxF32Value(state, min_ab, c, false);
	const auto max_ab   = EmitMinMaxF32Value(state, a, b, true);
	const auto high_min = EmitMinMaxF32Value(state, max_ab, c, false);
	const auto median   = EmitMinMaxF32Value(state, min_ab, high_min, true);
	const auto nan_ab   = NewBinary(state, OpLogicalOr, state.bool_type,
	                                EmitClassifyF32(state, a).nan, EmitClassifyF32(state, b).nan);
	const auto any_nan =
	    NewBinary(state, OpLogicalOr, state.bool_type, nan_ab, EmitClassifyF32(state, c).nan);
	return NewSelect(state, state.float_type, any_nan, min3, median);
}

uint32_t EmitExt(EmitterState& state, uint32_t type, uint32_t opcode,
                 std::initializer_list<uint32_t> args) {
	const auto            result = state.builder.AllocateId();
	std::vector<uint32_t> words {OpExtInst, type, result, state.glsl_std450, opcode};
	words.insert(words.end(), args.begin(), args.end());
	state.builder.AddFunction(words);
	return result;
}

uint32_t EmitF32ToU32(EmitterState& state, uint32_t src, bool signed_value) {
	const auto trunc         = EmitTruncF32Value(state, src);
	const auto converted_raw = state.builder.AllocateId();
	if (signed_value) {
		const auto converted_i = state.builder.AllocateId();
		state.builder.AddFunction({OpConvertFToS, state.int_type, converted_i, trunc});
		state.builder.AddFunction({OpBitcast, state.uint_type, converted_raw, converted_i});
	} else {
		state.builder.AddFunction({OpConvertFToU, state.uint_type, converted_raw, trunc});
	}
	const auto nan = EmitClassifyF32(state, src).nan;
	if (signed_value) {
		const auto below = NewBinary(state, OpFOrdLessThanEqual, state.bool_type, src,
		                             ConstantF32(state, 0xcf000000u));
		const auto above = NewBinary(state, OpFOrdGreaterThanEqual, state.bool_type, src,
		                             ConstantF32(state, 0x4f000000u));
		const auto high  = NewSelect(state, state.uint_type, above, ConstantU32(state, 0x7fffffffu),
		                             converted_raw);
		const auto low =
		    NewSelect(state, state.uint_type, below, ConstantU32(state, 0x80000000u), high);
		return NewSelect(state, state.uint_type, nan, ConstantU32(state, 0), low);
	}
	const auto below =
	    NewBinary(state, OpFOrdLessThanEqual, state.bool_type, src, ConstantF32(state, 0));
	const auto above = NewBinary(state, OpFOrdGreaterThanEqual, state.bool_type, src,
	                             ConstantF32(state, 0x4f800000u));
	const auto zero  = NewBinary(state, OpLogicalOr, state.bool_type, nan, below);
	const auto high =
	    NewSelect(state, state.uint_type, above, ConstantU32(state, 0xffffffffu), converted_raw);
	return NewSelect(state, state.uint_type, zero, ConstantU32(state, 0), high);
}

uint32_t EmitPackHalf(EmitterState& state, uint32_t src) {
	return EmitExt(state, state.uint_type, GlslPackHalf2x16, {src});
}

uint32_t EmitUnpackHalf(EmitterState& state, uint32_t bits) {
	const auto pair   = EmitExt(state, state.vec2_float_type, GlslUnpackHalf2x16, {bits});
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, state.float_type, result, pair, 0});
	return result;
}

} // namespace

bool EmitValueAlu(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto op    = inst.GetOpcode();
	const auto unary = [&](uint32_t spirv, IR::Type type) {
		ctx.Emit(inst, spirv, type, {ctx.Arg(inst, 0)});
		return true;
	};
	const auto binary = [&](uint32_t spirv, IR::Type type) {
		ctx.Emit(inst, spirv, type, {ctx.Arg(inst, 0), ctx.Arg(inst, 1)});
		return true;
	};
	const auto ext_unary = [&](uint32_t ext) {
		ctx.Define(inst, EmitExt(state, state.float_type, ext, {ctx.Arg(inst, 0)}));
		return true;
	};
	switch (op) {
		case IR::ValueOpcode::BitCastU16F16:
		case IR::ValueOpcode::BitCastF16U16:
		case IR::ValueOpcode::ConvertU32U16:
		case IR::ValueOpcode::ConvertU32U8:
		case IR::ValueOpcode::PackUint2x32:
		case IR::ValueOpcode::UnpackUint2x32: ctx.Define(inst, ctx.Arg(inst, 0)); return true;
		case IR::ValueOpcode::BitCastU32F32: return unary(OpBitcast, IR::Type::U32);
		case IR::ValueOpcode::BitCastF32U32: return unary(OpBitcast, IR::Type::F32);
		case IR::ValueOpcode::ConvertU16U32:
			ctx.Emit(inst, OpBitwiseAnd, IR::Type::U16,
			         {ctx.Arg(inst, 0), ConstantU32(state, 0xffffu)});
			return true;
		case IR::ValueOpcode::ConvertU8U32:
			ctx.Emit(inst, OpBitwiseAnd, IR::Type::U8,
			         {ctx.Arg(inst, 0), ConstantU32(state, 0xffu)});
			return true;
		case IR::ValueOpcode::ConvertF16F32: {
			const auto pair = state.builder.AllocateId();
			state.builder.AddFunction({OpCompositeConstruct, state.vec2_float_type, pair,
			                           ctx.Arg(inst, 0), ConstantF32(state, 0)});
			ctx.Define(inst, EmitPackHalf(state, pair));
			return true;
		}
		case IR::ValueOpcode::ConvertF32F16:
			ctx.Define(inst, EmitUnpackHalf(state, ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::ConvertS32F32:
			ctx.Define(inst, EmitF32ToU32(state, ctx.Arg(inst, 0), true));
			return true;
		case IR::ValueOpcode::ConvertU32F32:
			ctx.Define(inst, EmitF32ToU32(state, ctx.Arg(inst, 0), false));
			return true;
		case IR::ValueOpcode::ConvertF32S32: {
			const auto signed_value = NewUnary(state, OpBitcast, state.int_type, ctx.Arg(inst, 0));
			ctx.Emit(inst, OpConvertSToF, IR::Type::F32, {signed_value});
			return true;
		}
		case IR::ValueOpcode::ConvertF32U32: return unary(OpConvertUToF, IR::Type::F32);
		case IR::ValueOpcode::CompositeConstructU32x2:
			ctx.Emit(inst, OpCompositeConstruct, IR::Type::U32x2,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1)});
			return true;
		case IR::ValueOpcode::CompositeConstructF32x2:
			ctx.Emit(inst, OpCompositeConstruct, IR::Type::F32x2,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1)});
			return true;
		case IR::ValueOpcode::CompositeConstructU32x4:
			ctx.Emit(inst, OpCompositeConstruct, IR::Type::U32x4,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2), ctx.Arg(inst, 3)});
			return true;
		case IR::ValueOpcode::CompositeExtractU32x2:
		case IR::ValueOpcode::CompositeExtractU32x4:
			ctx.Emit(inst, OpCompositeExtract, IR::Type::U32,
			         {ctx.Arg(inst, 0), inst.Arg(1).U32()});
			return true;
		case IR::ValueOpcode::PackHalf2x16:
			ctx.Define(inst, EmitExt(state, state.uint_type, GlslPackHalf2x16, {ctx.Arg(inst, 0)}));
			return true;
		case IR::ValueOpcode::PackSnorm2x16:
			ctx.Define(inst,
			           EmitExt(state, state.uint_type, GlslPackSnorm2x16, {ctx.Arg(inst, 0)}));
			return true;
		case IR::ValueOpcode::PackUnorm2x16:
			ctx.Define(inst,
			           EmitExt(state, state.uint_type, GlslPackUnorm2x16, {ctx.Arg(inst, 0)}));
			return true;
		case IR::ValueOpcode::PackFloat2x16Rtz: {
			const auto low = EmitF32ToF16RtzBits(state, ctx.Arg(inst, 0));
			const auto high =
			    NewBinary(state, OpShiftLeftLogical, state.uint_type,
			              EmitF32ToF16RtzBits(state, ctx.Arg(inst, 1)), ConstantU32(state, 16));
			ctx.Define(inst, NewBinary(state, OpBitwiseOr, state.uint_type, low, high));
			return true;
		}
		case IR::ValueOpcode::FPAbs32:
			ctx.Define(inst, EmitFAbsValue(state, ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::FPNeg32:
			ctx.Define(inst, EmitFNegateValue(state, ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::FPSaturate32:
			ctx.Define(inst, EmitExt(state, state.float_type, GlslFClamp,
			                         {ctx.Arg(inst, 0), ConstantF32(state, 0),
			                          ConstantF32(state, 0x3f800000u)}));
			return true;
		case IR::ValueOpcode::BitFieldInsert:
			ctx.Emit(inst, OpBitFieldInsert, IR::Type::U32,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2), ctx.Arg(inst, 3)});
			return true;
		case IR::ValueOpcode::BitFieldUExtract:
			ctx.Emit(inst, OpBitFieldUExtract, IR::Type::U32,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2)});
			return true;
		case IR::ValueOpcode::BitFieldSExtract:
			ctx.Emit(inst, OpBitFieldSExtract, IR::Type::U32,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2)});
			return true;
		case IR::ValueOpcode::SelectU1:
			ctx.Emit(inst, OpSelect, IR::Type::U1,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2)});
			return true;
		case IR::ValueOpcode::SelectU32:
			ctx.Emit(inst, OpSelect, IR::Type::U32,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2)});
			return true;
		case IR::ValueOpcode::SelectF32:
			ctx.Emit(inst, OpSelect, IR::Type::F32,
			         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2)});
			return true;
		case IR::ValueOpcode::IAdd32: return binary(OpIAdd, IR::Type::U32);
		case IR::ValueOpcode::ISub32: return binary(OpISub, IR::Type::U32);
		case IR::ValueOpcode::IMul32: return binary(OpIMul, IR::Type::U32);
		case IR::ValueOpcode::UDiv32: return binary(OpUDiv, IR::Type::U32);
		case IR::ValueOpcode::IAdd64:
			ctx.Define(inst, EmitAdd64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::ISub64:
			ctx.Define(inst, EmitSub64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::IMul64:
			ctx.Define(inst, EmitMul64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::IAddCarry32:
			ctx.Emit(inst, OpIAddCarry, IR::Type::U32x2, {ctx.Arg(inst, 0), ctx.Arg(inst, 1)});
			return true;
		case IR::ValueOpcode::SMulHi:
			ctx.Define(inst, EmitMulHigh(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), true));
			return true;
		case IR::ValueOpcode::UMulHi:
			ctx.Define(inst, EmitMulHigh(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), false));
			return true;
		case IR::ValueOpcode::IAbs32: {
			const auto value = ctx.Arg(inst, 0);
			const auto neg   = NewUnary(state, OpSNegate, state.uint_type, value);
			const auto negative =
			    NewBinary(state, OpSLessThan, state.bool_type, value, ConstantU32(state, 0));
			ctx.Define(inst, NewSelect(state, state.uint_type, negative, neg, value));
			return true;
		}
		case IR::ValueOpcode::ShiftLeftLogical32: return binary(OpShiftLeftLogical, IR::Type::U32);
		case IR::ValueOpcode::ShiftRightLogical32:
			return binary(OpShiftRightLogical, IR::Type::U32);
		case IR::ValueOpcode::ShiftRightArithmetic32:
			return binary(OpShiftRightArithmetic, IR::Type::U32);
		case IR::ValueOpcode::ShiftLeftLogical64:
			ctx.Define(inst,
			           EmitShift64(state, OpShiftLeftLogical, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::ShiftRightLogical64:
			ctx.Define(inst,
			           EmitShift64(state, OpShiftRightLogical, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::ShiftRightArithmetic64:
			ctx.Define(inst, EmitShift64(state, OpShiftRightArithmetic, ctx.Arg(inst, 0),
			                             ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::BitwiseAnd32: return binary(OpBitwiseAnd, IR::Type::U32);
		case IR::ValueOpcode::BitwiseOr32: return binary(OpBitwiseOr, IR::Type::U32);
		case IR::ValueOpcode::BitwiseXor32: return binary(OpBitwiseXor, IR::Type::U32);
		case IR::ValueOpcode::BitwiseNot32: return unary(OpNot, IR::Type::U32);
		case IR::ValueOpcode::BitwiseAnd64:
			ctx.Define(inst,
			           EmitBitwise64(state, OpBitwiseAnd, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::BitwiseOr64:
			ctx.Define(inst, EmitBitwise64(state, OpBitwiseOr, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::BitReverse32: return unary(OpBitReverse, IR::Type::U32);
		case IR::ValueOpcode::BitCount32: return unary(OpBitCount, IR::Type::U32);
		case IR::ValueOpcode::BitCount64: {
			const auto pair = ExtractPair(state, ctx.Arg(inst, 0));
			ctx.Define(inst, NewBinary(state, OpIAdd, state.uint_type,
			                           NewUnary(state, OpBitCount, state.uint_type, pair.low),
			                           NewUnary(state, OpBitCount, state.uint_type, pair.high)));
			return true;
		}
		case IR::ValueOpcode::FindILsb32: {
			const auto value = EmitExt(state, state.int_type, GlslFindILsb, {ctx.Arg(inst, 0)});
			ctx.Define(inst, NewUnary(state, OpBitcast, state.uint_type, value));
			return true;
		}
		case IR::ValueOpcode::FindUMsb32: {
			const auto value = EmitExt(state, state.int_type, GlslFindUMsb, {ctx.Arg(inst, 0)});
			ctx.Define(inst, NewUnary(state, OpBitcast, state.uint_type, value));
			return true;
		}
		case IR::ValueOpcode::FindUMsb64:
			ctx.Define(inst, EmitFindMsb64(state, ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::FindSMsb32: {
			const auto value =
			    EmitExt(state, state.int_type, 74u,
			            {NewUnary(state, OpBitcast, state.int_type, ctx.Arg(inst, 0))});
			ctx.Define(inst, NewUnary(state, OpBitcast, state.uint_type, value));
			return true;
		}
		case IR::ValueOpcode::SMin32:
			ctx.Define(inst, EmitMinMaxI32Value(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), false));
			return true;
		case IR::ValueOpcode::SMax32:
			ctx.Define(inst, EmitMinMaxI32Value(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), true));
			return true;
		case IR::ValueOpcode::UMin32:
			ctx.Define(inst, EmitMinMaxU32Value(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), false));
			return true;
		case IR::ValueOpcode::UMax32:
			ctx.Define(inst, EmitMinMaxU32Value(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), true));
			return true;
		case IR::ValueOpcode::SMinTri32:
			ctx.Define(inst, EmitMinMax3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                             ctx.Arg(inst, 2), true, false));
			return true;
		case IR::ValueOpcode::SMaxTri32:
			ctx.Define(inst, EmitMinMax3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                             ctx.Arg(inst, 2), true, true));
			return true;
		case IR::ValueOpcode::UMinTri32:
			ctx.Define(inst, EmitMinMax3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                             ctx.Arg(inst, 2), false, false));
			return true;
		case IR::ValueOpcode::UMaxTri32:
			ctx.Define(inst, EmitMinMax3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                             ctx.Arg(inst, 2), false, true));
			return true;
		case IR::ValueOpcode::SMedTri32:
			ctx.Define(inst,
			           EmitMed3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2), true));
			return true;
		case IR::ValueOpcode::UMedTri32:
			ctx.Define(
			    inst, EmitMed3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2), false));
			return true;
		case IR::ValueOpcode::SLessThan32: return binary(OpSLessThan, IR::Type::U1);
		case IR::ValueOpcode::ULessThan32: return binary(OpULessThan, IR::Type::U1);
		case IR::ValueOpcode::IEqual32: return binary(OpIEqual, IR::Type::U1);
		case IR::ValueOpcode::SLessThanEqual32: return binary(OpSLessThanEqual, IR::Type::U1);
		case IR::ValueOpcode::ULessThanEqual32: return binary(OpULessThanEqual, IR::Type::U1);
		case IR::ValueOpcode::SGreaterThan32: return binary(OpSGreaterThan, IR::Type::U1);
		case IR::ValueOpcode::UGreaterThan32: return binary(OpUGreaterThan, IR::Type::U1);
		case IR::ValueOpcode::INotEqual32: return binary(OpINotEqual, IR::Type::U1);
		case IR::ValueOpcode::SGreaterThanEqual32: return binary(OpSGreaterThanEqual, IR::Type::U1);
		case IR::ValueOpcode::UGreaterThanEqual32: return binary(OpUGreaterThanEqual, IR::Type::U1);
		case IR::ValueOpcode::IEqual64:
			ctx.Define(inst,
			           Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), 0, 0, true, false));
			return true;
		case IR::ValueOpcode::INotEqual64:
			ctx.Define(inst,
			           Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), 0, 0, true, true));
			return true;
		case IR::ValueOpcode::ULessThan64:
			ctx.Define(inst, Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), OpULessThan,
			                           OpULessThan));
			return true;
		case IR::ValueOpcode::SLessThan64:
			ctx.Define(inst, Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), OpSLessThan,
			                           OpULessThan));
			return true;
		case IR::ValueOpcode::UGreaterThan64:
			ctx.Define(inst, Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), OpUGreaterThan,
			                           OpUGreaterThan));
			return true;
		case IR::ValueOpcode::SGreaterThan64:
			ctx.Define(inst, Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), OpSGreaterThan,
			                           OpUGreaterThan));
			return true;
		case IR::ValueOpcode::ULessThanEqual64: {
			const auto greater = Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                               OpUGreaterThan, OpUGreaterThan);
			ctx.Define(inst, NewUnary(state, OpLogicalNot, state.bool_type, greater));
			return true;
		}
		case IR::ValueOpcode::SLessThanEqual64: {
			const auto greater = Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                               OpSGreaterThan, OpUGreaterThan);
			ctx.Define(inst, NewUnary(state, OpLogicalNot, state.bool_type, greater));
			return true;
		}
		case IR::ValueOpcode::UGreaterThanEqual64: {
			const auto less =
			    Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), OpULessThan, OpULessThan);
			ctx.Define(inst, NewUnary(state, OpLogicalNot, state.bool_type, less));
			return true;
		}
		case IR::ValueOpcode::SGreaterThanEqual64: {
			const auto less =
			    Compare64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), OpSLessThan, OpULessThan);
			ctx.Define(inst, NewUnary(state, OpLogicalNot, state.bool_type, less));
			return true;
		}
		case IR::ValueOpcode::LogicalOr: return binary(OpLogicalOr, IR::Type::U1);
		case IR::ValueOpcode::LogicalAnd: return binary(OpLogicalAnd, IR::Type::U1);
		case IR::ValueOpcode::LogicalXor: return binary(OpLogicalNotEqual, IR::Type::U1);
		case IR::ValueOpcode::LogicalNot: return unary(OpLogicalNot, IR::Type::U1);
		case IR::ValueOpcode::FPOrdEqual32: return binary(OpFOrdEqual, IR::Type::U1);
		case IR::ValueOpcode::FPUnordEqual32: return binary(OpFUnordEqual, IR::Type::U1);
		case IR::ValueOpcode::FPOrdNotEqual32: return binary(OpFOrdNotEqual, IR::Type::U1);
		case IR::ValueOpcode::FPUnordNotEqual32: return binary(OpFUnordNotEqual, IR::Type::U1);
		case IR::ValueOpcode::FPOrdLessThan32: return binary(OpFOrdLessThan, IR::Type::U1);
		case IR::ValueOpcode::FPUnordLessThan32: return binary(OpFUnordLessThan, IR::Type::U1);
		case IR::ValueOpcode::FPOrdGreaterThan32: return binary(OpFOrdGreaterThan, IR::Type::U1);
		case IR::ValueOpcode::FPUnordGreaterThan32:
			return binary(OpFUnordGreaterThan, IR::Type::U1);
		case IR::ValueOpcode::FPOrdLessThanEqual32:
			return binary(OpFOrdLessThanEqual, IR::Type::U1);
		case IR::ValueOpcode::FPUnordLessThanEqual32:
			return binary(OpFUnordLessThanEqual, IR::Type::U1);
		case IR::ValueOpcode::FPOrdGreaterThanEqual32:
			return binary(OpFOrdGreaterThanEqual, IR::Type::U1);
		case IR::ValueOpcode::FPUnordGreaterThanEqual32:
			return binary(OpFUnordGreaterThanEqual, IR::Type::U1);
		case IR::ValueOpcode::FPIsNan32:
			ctx.Emit(inst, OpFUnordNotEqual, IR::Type::U1, {ctx.Arg(inst, 0), ctx.Arg(inst, 0)});
			return true;
		case IR::ValueOpcode::FPCmpClass32:
			ctx.Define(inst, EmitClassMaskF32(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::FPAdd32: return binary(OpFAdd, IR::Type::F32);
		case IR::ValueOpcode::FPSub32: return binary(OpFSub, IR::Type::F32);
		case IR::ValueOpcode::FPMul32: return binary(OpFMul, IR::Type::F32);
		case IR::ValueOpcode::FPFma32:
			ctx.Define(inst, EmitExt(state, state.float_type, GlslFma,
			                         {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2)}));
			return true;
		case IR::ValueOpcode::FPMin32:
			ctx.Define(inst, EmitMinMaxF32Value(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), false));
			return true;
		case IR::ValueOpcode::FPMax32:
			ctx.Define(inst, EmitMinMaxF32Value(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), true));
			return true;
		case IR::ValueOpcode::FPMinTri32:
			ctx.Define(inst, EmitFMinMax3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                              ctx.Arg(inst, 2), false));
			return true;
		case IR::ValueOpcode::FPMaxTri32:
			ctx.Define(inst, EmitFMinMax3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1),
			                              ctx.Arg(inst, 2), true));
			return true;
		case IR::ValueOpcode::FPMedTri32:
			ctx.Define(inst,
			           EmitFMed3(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2)));
			return true;
		case IR::ValueOpcode::FPRecip32: {
			const auto source = EmitFlushF32DenormToSignedZero(state, ctx.Arg(inst, 0));
			ctx.Define(inst, NewBinary(state, OpFDiv, state.float_type,
			                           ConstantF32(state, 0x3f800000u), source));
			return true;
		}
		case IR::ValueOpcode::FPRecipIFlag32:
			// Integer-to-float inputs used by IFLAG cannot be denormal.
			ctx.Define(inst, NewBinary(state, OpFDiv, state.float_type,
			                           ConstantF32(state, 0x3f800000u), ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::FPRecipSqrt32:
			ctx.Define(inst, EmitExt(state, state.float_type, GlslInverseSqrt,
			                         {EmitFlushF32DenormToSignedZero(state, ctx.Arg(inst, 0))}));
			return true;
		case IR::ValueOpcode::FPSqrt:
			ctx.Define(inst, EmitExt(state, state.float_type, GlslSqrt,
			                         {EmitFlushF32DenormToSignedZero(state, ctx.Arg(inst, 0))}));
			return true;
		case IR::ValueOpcode::FPSin:
		case IR::ValueOpcode::FPCos: {
			auto source = EmitTrigCycleF32(state, ctx.Arg(inst, 0), op == IR::ValueOpcode::FPSin);
			source =
			    NewBinary(state, OpFMul, state.float_type, source, ConstantF32(state, 0x40c90fdbu));
			ctx.Define(inst, EmitExt(state, state.float_type,
			                         op == IR::ValueOpcode::FPSin ? GlslSin : GlslCos, {source}));
			return true;
		}
		case IR::ValueOpcode::FPExp2:
			ctx.Define(inst, EmitExt(state, state.float_type, GlslExp2,
			                         {EmitFlushF32DenormToSignedZero(state, ctx.Arg(inst, 0))}));
			return true;
		case IR::ValueOpcode::FPLog2:
			ctx.Define(inst, EmitExt(state, state.float_type, GlslLog2,
			                         {EmitFlushF32DenormToSignedZero(state, ctx.Arg(inst, 0))}));
			return true;
		case IR::ValueOpcode::FPLdexp: {
			const auto exponent = NewUnary(state, OpBitcast, state.int_type, ctx.Arg(inst, 1));
			ctx.Define(inst,
			           EmitExt(state, state.float_type, GlslLdexp, {ctx.Arg(inst, 0), exponent}));
			return true;
		}
		case IR::ValueOpcode::FPRoundEven32: return ext_unary(GlslRoundEven);
		case IR::ValueOpcode::FPFloor32: return ext_unary(GlslFloor);
		case IR::ValueOpcode::FPCeil32: return ext_unary(GlslCeil);
		case IR::ValueOpcode::FPTrunc32: return ext_unary(GlslTrunc);
		case IR::ValueOpcode::FPFract32: return ext_unary(GlslFract);
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
