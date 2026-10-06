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

uint32_t EmitMinMaxF64(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value) {
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

	// Non-IEEE mode selects the other operand for NaN, including rhs when both are NaN.
	result = Select(state, TypeF64(state), Unary(state, spv::OpIsNan, TypeBool(state), rhs),
	                lhs, result);
	return Select(state, TypeF64(state), Unary(state, spv::OpIsNan, TypeBool(state), lhs),
	              rhs, result);
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

uint32_t EmitFPFma32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
	const auto result = EmitGlsl<GLSLstd450Fma, IR::Type::F32>(state, a, b, c);
	state.builder.AddAnnotation(spv::OpDecorate, result, spv::DecorationNoContraction);
	return result;
}

uint32_t EmitFPMad32(EmitterState& state, uint32_t a, uint32_t b, uint32_t c) {
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
	// GLSL FindUMsb is restricted to 32-bit components.
	const auto unpacked = Unary(state, spv::OpBitcast, TypeU32Vector(state, 2), value);
	const auto pair =
	    ExtractPair(state, EmitExt(state, TypeU32Vector(state, 2), GLSLstd450FindUMsb, {unpacked}));
	const auto high_found =
	    Binary(state, spv::OpINotEqual, TypeBool(state), pair.high, ConstantU32(state, UINT32_MAX));
	return Select(state, TypeU32(state), high_found,
	              Binary(state, spv::OpIAdd, TypeU32(state), pair.high, ConstantU32(state, 32)),
	              pair.low);
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

uint32_t EmitBitCount64(EmitterState& state, uint32_t arg0) {
	// Vulkan bit-count operands remain 32-bit, including when counting a U64.
	const auto unpacked = Unary(state, spv::OpBitcast, TypeU32Vector(state, 2), arg0);
	const auto pair =
	    ExtractPair(state, Unary(state, spv::OpBitCount, TypeU32Vector(state, 2), unpacked));
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

uint32_t EmitFPIsNan32(EmitterState& state, uint32_t arg0) {
	return EmitNative<spv::OpFUnordNotEqual, IR::Type::U1>(state, arg0, arg0);
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
	const auto one = state.builder.Constant(spv::OpConstant, TypeF64(state), 0u, 0x3ff00000u);
	return EmitNative<spv::OpFDiv, IR::Type::F64>(state, one, arg0);
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
