#include "graphics/shader/recompiler/backend/spirv/softFloat64.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

// The 64-bit float operations. On a host with 64-bit floats they are native instructions. On a
// host without them a 64-bit float is a 64-bit integer, and each operation calls a function made
// from softFloat64.h, defined once per shader before the entry point.

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

// softFloat64.h's operations as SPIR-V instructions on 64-bit integers.
struct SpirvOps {
	using V = uint32_t;
	using W = uint32_t;
	using B = uint32_t;

	EmitterState& s;

	V K(uint64_t value) { return ConstantU64(s, value); }
	V Add(V a, V b) { return Binary(s, spv::OpIAdd, TypeU64(s), a, b); }
	V Sub(V a, V b) { return Binary(s, spv::OpISub, TypeU64(s), a, b); }
	V Mul(V a, V b) { return Binary(s, spv::OpIMul, TypeU64(s), a, b); }
	V And(V a, V b) { return Binary(s, spv::OpBitwiseAnd, TypeU64(s), a, b); }
	V Or(V a, V b) { return Binary(s, spv::OpBitwiseOr, TypeU64(s), a, b); }
	V Xor(V a, V b) { return Binary(s, spv::OpBitwiseXor, TypeU64(s), a, b); }
	V Shl(V a, V n) { return Binary(s, spv::OpShiftLeftLogical, TypeU64(s), a, n); }
	V Shr(V a, V n) { return Binary(s, spv::OpShiftRightLogical, TypeU64(s), a, n); }
	B Eq(V a, V b) { return Binary(s, spv::OpIEqual, TypeBool(s), a, b); }
	B Ult(V a, V b) { return Binary(s, spv::OpULessThan, TypeBool(s), a, b); }
	B Slt(V a, V b) { return Binary(s, spv::OpSLessThan, TypeBool(s), a, b); }
	B AndB(B a, B b) { return Binary(s, spv::OpLogicalAnd, TypeBool(s), a, b); }
	B OrB(B a, B b) { return Binary(s, spv::OpLogicalOr, TypeBool(s), a, b); }
	B NotB(B a) { return Unary(s, spv::OpLogicalNot, TypeBool(s), a); }
	V Sel(B condition, V a, V b) { return Select(s, TypeU64(s), condition, a, b); }
	V Zext(W value) { return Unary(s, spv::OpUConvert, TypeU64(s), value); }
	W Trunc(V value) { return Unary(s, spv::OpUConvert, TypeU32(s), value); }
};

struct Signature {
	const char*            name;
	bool                   bool_result;
	bool                   u32_result;
	bool                   u32_argument;
	uint32_t               arguments;
};

Signature SignatureOf(SoftFloat64Op op) {
	switch (op) {
		case SoftFloat64Op::Add: return {"soft_f64_add", false, false, false, 2};
		case SoftFloat64Op::Mul: return {"soft_f64_mul", false, false, false, 2};
		case SoftFloat64Op::Fma: return {"soft_f64_fma", false, false, false, 3};
		case SoftFloat64Op::Recip: return {"soft_f64_recip", false, false, false, 1};
		case SoftFloat64Op::Min: return {"soft_f64_min", false, false, false, 2};
		case SoftFloat64Op::Max: return {"soft_f64_max", false, false, false, 2};
		case SoftFloat64Op::Floor: return {"soft_f64_floor", false, false, false, 1};
		case SoftFloat64Op::Ceil: return {"soft_f64_ceil", false, false, false, 1};
		case SoftFloat64Op::Trunc: return {"soft_f64_trunc", false, false, false, 1};
		case SoftFloat64Op::Fract: return {"soft_f64_fract", false, false, false, 1};
		case SoftFloat64Op::OrdEqual: return {"soft_f64_ord_equal", true, false, false, 2};
		case SoftFloat64Op::OrdLessThanEqual:
			return {"soft_f64_ord_less_equal", true, false, false, 2};
		case SoftFloat64Op::OrdGreaterThanEqual:
			return {"soft_f64_ord_greater_equal", true, false, false, 2};
		case SoftFloat64Op::FromS32: return {"soft_f64_from_s32", false, false, true, 1};
		case SoftFloat64Op::FromU32: return {"soft_f64_from_u32", false, false, true, 1};
		case SoftFloat64Op::FromF32: return {"soft_f64_from_f32", false, false, true, 1};
		case SoftFloat64Op::ToF32: return {"soft_f64_to_f32", false, true, false, 1};
		case SoftFloat64Op::Count: break;
	}
	EXIT("invalid soft float64 operation %u\n", static_cast<unsigned>(op));
	return {};
}

uint32_t ResultType(EmitterState& state, const Signature& signature) {
	if (signature.bool_result) {
		return TypeBool(state);
	}
	return signature.u32_result ? TypeU32(state) : TypeU64(state);
}

uint32_t Body(SoftFloat64::Math<SpirvOps>& math, SoftFloat64Op op,
              const std::array<uint32_t, 3>& args) {
	switch (op) {
		case SoftFloat64Op::Add: return math.Add(args[0], args[1]);
		case SoftFloat64Op::Mul: return math.Mul(args[0], args[1]);
		case SoftFloat64Op::Fma: return math.Fma(args[0], args[1], args[2]);
		case SoftFloat64Op::Recip: return math.Recip(args[0]);
		case SoftFloat64Op::Min: return math.Min(args[0], args[1]);
		case SoftFloat64Op::Max: return math.Max(args[0], args[1]);
		case SoftFloat64Op::Floor: return math.Floor(args[0]);
		case SoftFloat64Op::Ceil: return math.Ceil(args[0]);
		case SoftFloat64Op::Trunc: return math.Trunc(args[0]);
		case SoftFloat64Op::Fract: return math.Fract(args[0]);
		case SoftFloat64Op::OrdEqual: return math.OrdEqual(args[0], args[1]);
		case SoftFloat64Op::OrdLessThanEqual: return math.OrdLessThanEqual(args[0], args[1]);
		case SoftFloat64Op::OrdGreaterThanEqual:
			return math.OrdGreaterThanEqual(args[0], args[1]);
		case SoftFloat64Op::FromS32: return math.FromS32(args[0]);
		case SoftFloat64Op::FromU32: return math.FromU32(args[0]);
		case SoftFloat64Op::FromF32: return math.FromF32(args[0]);
		case SoftFloat64Op::ToF32: return math.ToF32(args[0]);
		case SoftFloat64Op::Count: break;
	}
	EXIT("invalid soft float64 operation %u\n", static_cast<unsigned>(op));
	return 0;
}

void DefineFunction(EmitterState& state, SoftFloat64Op op) {
	const auto signature = SignatureOf(op);
	const auto argument_type = signature.u32_argument ? TypeU32(state) : TypeU64(state);
	const auto result_type = ResultType(state, signature);
	std::vector<uint32_t> parameter_types(signature.arguments, argument_type);
	const auto function_type =
	    state.builder.Type(spv::OpTypeFunction, result_type, std::span<const uint32_t>(parameter_types));
	const auto function = state.soft_float64_functions[static_cast<size_t>(op)];
	state.builder.AddName(function, signature.name);
	state.builder.AddFunction(spv::OpFunction, result_type, function,
	                          spv::FunctionControlMaskNone, function_type);
	std::array<uint32_t, 3> args {};
	for (uint32_t i = 0; i < signature.arguments; i++) {
		args[i] = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpFunctionParameter, argument_type, args[i]);
	}
	EmitLabel(state, state.builder.AllocateId());
	SpirvOps                    ops {state};
	SoftFloat64::Math<SpirvOps> math(ops);
	const auto                  result = Body(math, op, args);
	state.builder.AddFunction(spv::OpReturnValue, result);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

template <spv::Op opcode, IR::Type type, typename... Args>
uint32_t NativeOrSoft(EmitterState& state, SoftFloat64Op op, Args... args) {
	if (state.soft_float64) {
		return EmitSoftFloat64(state, op, {args...});
	}
	return EmitNative<opcode, type>(state, args...);
}

template <GLSLstd450 opcode>
uint32_t GlslOrSoft(EmitterState& state, SoftFloat64Op op, uint32_t arg0) {
	if (state.soft_float64) {
		return EmitSoftFloat64(state, op, {arg0});
	}
	return EmitGlsl<opcode, IR::Type::F64>(state, arg0);
}

constexpr uint32_t Bit(SoftFloat64Op op) {
	return 1u << static_cast<uint32_t>(op);
}

// The compares that softFloat64.h has no function for are made from these three.
constexpr uint32_t kDerivedCompare =
    Bit(SoftFloat64Op::OrdEqual) | Bit(SoftFloat64Op::OrdLessThanEqual) |
    Bit(SoftFloat64Op::OrdGreaterThanEqual);

// The software operation for a 64-bit float opcode (see softFloat64.h).
std::optional<SoftFloat64Op> SoftFloat64OpOf(IR::ValueOpcode opcode) {
	using Op = SoftFloat64Op;
	switch (opcode) {
		case IR::ValueOpcode::FPAdd64: return Op::Add;
		case IR::ValueOpcode::FPMul64: return Op::Mul;
		case IR::ValueOpcode::FPFma64: return Op::Fma;
		case IR::ValueOpcode::FPRecip64: return Op::Recip;
		case IR::ValueOpcode::FPMin64: return Op::Min;
		case IR::ValueOpcode::FPMax64: return Op::Max;
		case IR::ValueOpcode::FPFloor64: return Op::Floor;
		case IR::ValueOpcode::FPCeil64: return Op::Ceil;
		case IR::ValueOpcode::FPTrunc64: return Op::Trunc;
		case IR::ValueOpcode::FPFract64: return Op::Fract;
		case IR::ValueOpcode::FPOrdEqual64: return Op::OrdEqual;
		case IR::ValueOpcode::FPOrdLessThanEqual64: return Op::OrdLessThanEqual;
		case IR::ValueOpcode::FPOrdGreaterThanEqual64: return Op::OrdGreaterThanEqual;
		case IR::ValueOpcode::ConvertF64S32: return Op::FromS32;
		case IR::ValueOpcode::ConvertF64U32: return Op::FromU32;
		case IR::ValueOpcode::ConvertF64F32: return Op::FromF32;
		case IR::ValueOpcode::ConvertF32F64: return Op::ToF32;
		default: return std::nullopt;
	}
}

bool IsDerivedCompare(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::FPOrdNotEqual64:
		case IR::ValueOpcode::FPOrdLessThan64:
		case IR::ValueOpcode::FPOrdGreaterThan64:
		case IR::ValueOpcode::FPUnordEqual64:
		case IR::ValueOpcode::FPUnordNotEqual64:
		case IR::ValueOpcode::FPUnordLessThan64:
		case IR::ValueOpcode::FPUnordGreaterThan64:
		case IR::ValueOpcode::FPUnordLessThanEqual64:
		case IR::ValueOpcode::FPUnordGreaterThanEqual64: return true;
		default: return false;
	}
}

// A compare made from the software equal, less-or-equal and greater-or-equal. Each of the three
// is false when an operand is NaN. So a pair is ordered when one of <= and >= holds, an ordered
// strict compare is the non-strict one without equality, and an unordered compare is the
// negation of the opposite ordered one.
enum class DerivedCompare { OrdNotEqual, OrdLessThan, OrdGreaterThan };

uint32_t SoftCompare(EmitterState& state, DerivedCompare compare, uint32_t a, uint32_t b) {
	const auto equal = EmitSoftFloat64(state, SoftFloat64Op::OrdEqual, {a, b});
	const auto not_equal = Unary(state, spv::OpLogicalNot, TypeBool(state), equal);
	const auto and_not_equal = [&](uint32_t value) {
		return Binary(state, spv::OpLogicalAnd, TypeBool(state), value, not_equal);
	};
	switch (compare) {
		case DerivedCompare::OrdLessThan:
			return and_not_equal(EmitSoftFloat64(state, SoftFloat64Op::OrdLessThanEqual, {a, b}));
		case DerivedCompare::OrdGreaterThan:
			return and_not_equal(EmitSoftFloat64(state, SoftFloat64Op::OrdGreaterThanEqual, {a, b}));
		case DerivedCompare::OrdNotEqual: {
			const auto ordered = Binary(
			    state, spv::OpLogicalOr, TypeBool(state),
			    EmitSoftFloat64(state, SoftFloat64Op::OrdLessThanEqual, {a, b}),
			    EmitSoftFloat64(state, SoftFloat64Op::OrdGreaterThanEqual, {a, b}));
			return and_not_equal(ordered);
		}
	}
	return 0;
}

uint32_t Not(EmitterState& state, uint32_t value) {
	return Unary(state, spv::OpLogicalNot, TypeBool(state), value);
}

} // namespace

void DefineSoftFloat64(EmitterState& state) {
	if (!state.soft_float64) {
		return;
	}
	// The operations the shader uses: a bit for each SoftFloat64Op.
	uint32_t used = 0;
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			if (const auto op = SoftFloat64OpOf(inst.GetOpcode())) {
				used |= Bit(*op);
			} else if (IsDerivedCompare(inst.GetOpcode())) {
				used |= kDerivedCompare;
			}
		}
	}
	for (uint32_t i = 0; i < static_cast<uint32_t>(SoftFloat64Op::Count); i++) {
		if ((used & (1u << i)) != 0u) {
			state.soft_float64_functions[i] = state.builder.AllocateId();
			DefineFunction(state, static_cast<SoftFloat64Op>(i));
		}
	}
}

uint32_t EmitSoftFloat64(EmitterState& state, SoftFloat64Op op,
                         std::initializer_list<uint32_t> args) {
	const auto function = state.soft_float64_functions[static_cast<size_t>(op)];
	EXIT_IF(function == 0);
	const auto signature = SignatureOf(op);
	EXIT_IF(args.size() != signature.arguments);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, ResultType(state, signature), result, function,
	                          std::span<const uint32_t>(args.begin(), args.size()));
	return result;
}

uint32_t EmitBitCastU64F64(EmitterState& state, uint32_t arg0) {
	if (state.soft_float64) {
		return Unary(state, spv::OpCopyObject, TypeU64(state), arg0);
	}
	return EmitNative<spv::OpBitcast, IR::Type::U64>(state, arg0);
}

uint32_t EmitBitCastF64U64(EmitterState& state, uint32_t arg0) {
	if (state.soft_float64) {
		return Unary(state, spv::OpCopyObject, TypeU64(state), arg0);
	}
	return EmitNative<spv::OpBitcast, IR::Type::F64>(state, arg0);
}

uint32_t EmitConvertF64S32(EmitterState& state, uint32_t arg0) {
	return NativeOrSoft<spv::OpConvertSToF, IR::Type::F64>(state, SoftFloat64Op::FromS32, arg0);
}

uint32_t EmitConvertF64U32(EmitterState& state, uint32_t arg0) {
	return NativeOrSoft<spv::OpConvertUToF, IR::Type::F64>(state, SoftFloat64Op::FromU32, arg0);
}

uint32_t EmitFPOrdEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return NativeOrSoft<spv::OpFOrdEqual, IR::Type::U1>(state, SoftFloat64Op::OrdEqual, arg0,
	                                                     arg1);
}

uint32_t EmitFPOrdLessThanEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return NativeOrSoft<spv::OpFOrdLessThanEqual, IR::Type::U1>(
	    state, SoftFloat64Op::OrdLessThanEqual, arg0, arg1);
}

uint32_t EmitFPOrdGreaterThanEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return NativeOrSoft<spv::OpFOrdGreaterThanEqual, IR::Type::U1>(
	    state, SoftFloat64Op::OrdGreaterThanEqual, arg0, arg1);
}

// A NaN has every exponent bit set and a mantissa that is not zero: without the sign, its bits are
// above those of infinity.
uint32_t EmitFPIsNan64(EmitterState& state, uint32_t arg0) {
	if (state.soft_float64) {
		const auto magnitude = Binary(state, spv::OpBitwiseAnd, TypeU64(state), arg0,
		                              ConstantU64(state, 0x7fff'ffff'ffff'ffffull));
		return Binary(state, spv::OpUGreaterThan, TypeBool(state), magnitude,
		              ConstantU64(state, 0x7ff0'0000'0000'0000ull));
	}
	return EmitNative<spv::OpIsNan, IR::Type::U1>(state, arg0);
}

uint32_t EmitFPOrdNotEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return SoftCompare(state, DerivedCompare::OrdNotEqual, arg0, arg1);
	return EmitNative<spv::OpFOrdNotEqual, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPOrdLessThan64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return SoftCompare(state, DerivedCompare::OrdLessThan, arg0, arg1);
	return EmitNative<spv::OpFOrdLessThan, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPOrdGreaterThan64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return SoftCompare(state, DerivedCompare::OrdGreaterThan, arg0, arg1);
	return EmitNative<spv::OpFOrdGreaterThan, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPUnordEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return Not(state, EmitFPOrdNotEqual64(state, arg0, arg1));
	return EmitNative<spv::OpFUnordEqual, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPUnordNotEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return Not(state, EmitFPOrdEqual64(state, arg0, arg1));
	return EmitNative<spv::OpFUnordNotEqual, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPUnordLessThan64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return Not(state, EmitFPOrdGreaterThanEqual64(state, arg0, arg1));
	return EmitNative<spv::OpFUnordLessThan, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPUnordGreaterThan64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return Not(state, EmitFPOrdLessThanEqual64(state, arg0, arg1));
	return EmitNative<spv::OpFUnordGreaterThan, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPUnordLessThanEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return Not(state, EmitFPOrdGreaterThan64(state, arg0, arg1));
	return EmitNative<spv::OpFUnordLessThanEqual, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPUnordGreaterThanEqual64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	if (state.soft_float64) return Not(state, EmitFPOrdLessThan64(state, arg0, arg1));
	return EmitNative<spv::OpFUnordGreaterThanEqual, IR::Type::U1>(state, arg0, arg1);
}

uint32_t EmitFPAdd64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return NativeOrSoft<spv::OpFAdd, IR::Type::F64>(state, SoftFloat64Op::Add, arg0, arg1);
}

uint32_t EmitFPMul64(EmitterState& state, uint32_t arg0, uint32_t arg1) {
	return NativeOrSoft<spv::OpFMul, IR::Type::F64>(state, SoftFloat64Op::Mul, arg0, arg1);
}

uint32_t EmitFPFma64(EmitterState& state, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
	if (state.soft_float64) {
		return EmitSoftFloat64(state, SoftFloat64Op::Fma, {arg0, arg1, arg2});
	}
	return EmitGlsl<GLSLstd450Fma, IR::Type::F64>(state, arg0, arg1, arg2);
}

uint32_t EmitFPFloor64(EmitterState& state, uint32_t arg0) {
	return GlslOrSoft<GLSLstd450Floor>(state, SoftFloat64Op::Floor, arg0);
}

uint32_t EmitFPCeil64(EmitterState& state, uint32_t arg0) {
	return GlslOrSoft<GLSLstd450Ceil>(state, SoftFloat64Op::Ceil, arg0);
}

uint32_t EmitFPTrunc64(EmitterState& state, uint32_t arg0) {
	return GlslOrSoft<GLSLstd450Trunc>(state, SoftFloat64Op::Trunc, arg0);
}

uint32_t EmitFPFract64(EmitterState& state, uint32_t arg0) {
	return GlslOrSoft<GLSLstd450Fract>(state, SoftFloat64Op::Fract, arg0);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
