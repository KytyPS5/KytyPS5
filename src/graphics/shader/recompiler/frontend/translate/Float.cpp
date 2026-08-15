#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

bool Translator::TranslatePackedFloat16(const IR::Instruction& inst) {
	IR::ValueOpcode opcode;
	switch (inst.op) {
		case IR::Opcode::PackedAddF16: opcode = IR::ValueOpcode::FPAdd32; break;
		case IR::Opcode::PackedMulF16: opcode = IR::ValueOpcode::FPMul32; break;
		case IR::Opcode::PackedMinF16: opcode = IR::ValueOpcode::FPMin32; break;
		case IR::Opcode::PackedMaxF16: opcode = IR::ValueOpcode::FPMax32; break;
		case IR::Opcode::PackedFmaF16: opcode = IR::ValueOpcode::FPFma32; break;
		default: return false;
	}
	const auto translate_lane = [&](bool high) {
		const auto lhs = ReadF16LaneAsF32(inst.src[0], high);
		const auto rhs = ReadF16LaneAsF32(inst.src[1], high);
		IR::F32    result;
		if (inst.src_count == 3u) {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs, ReadF16LaneAsF32(inst.src[2], high)}));
		} else {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs}));
		}
		return ApplyF32ResultModifiers(inst.dst, result);
	};
	auto raw    = inst.dst;
	raw.omod    = 0u;
	raw.clamp   = false;
	auto result = PackHalf2x16(translate_lane(false), translate_lane(true));
	if (inst.op == IR::Opcode::PackedMinF16 || inst.op == IR::Opcode::PackedMaxF16) {
		const auto quiet_snan = [&](const IR::Operand& operand, bool high) {
			const auto bits     = ReadU16LaneAsU32(operand, high, false);
			const auto exponent = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x7c00u)));
			const auto payload  = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x01ffu)));
			const auto snan     = ir.LogicalAnd(ir.IEqual(exponent, IR::U32(IR::Value(0x7c00u))),
			                                    ir.INotEqual(payload, IR::U32(IR::Value(0u))));
			return std::pair {snan, ir.BitwiseOr(bits, IR::U32(IR::Value(0x0200u)))};
		};
		const auto override_lane = [&](bool high) {
			const auto [lhs_snan, lhs_quiet] = quiet_snan(inst.src[0], high);
			const auto [rhs_snan, rhs_quiet] = quiet_snan(inst.src[1], high);
			const auto normal = high ? ir.ShiftRightLogical(result, IR::U32(IR::Value(16u)))
			                         : ir.BitwiseAnd(result, IR::U32(IR::Value(0xffffu)));
			return ir.Select(lhs_snan, lhs_quiet, ir.Select(rhs_snan, rhs_quiet, normal));
		};
		result = PackU16Lanes(override_lane(false), override_lane(true));
	}
	WriteOperand(raw, result);
	return true;
}

bool Translator::TranslateFloat16Operation(const IR::Instruction& inst) {
	IR::ValueOpcode opcode;
	switch (inst.op) {
		case IR::Opcode::AddF16: opcode = IR::ValueOpcode::FPAdd32; break;
		case IR::Opcode::SubF16: opcode = IR::ValueOpcode::FPSub32; break;
		case IR::Opcode::MulF16: opcode = IR::ValueOpcode::FPMul32; break;
		case IR::Opcode::MinF16: opcode = IR::ValueOpcode::FPMin32; break;
		case IR::Opcode::MaxF16: opcode = IR::ValueOpcode::FPMax32; break;
		case IR::Opcode::FmaF16:
		case IR::Opcode::MadMixF16: opcode = IR::ValueOpcode::FPFma32; break;
		case IR::Opcode::RcpF16: opcode = IR::ValueOpcode::FPRecip32; break;
		case IR::Opcode::SqrtF16: opcode = IR::ValueOpcode::FPSqrt; break;
		case IR::Opcode::InverseSqrtF16: opcode = IR::ValueOpcode::FPRecipSqrt32; break;
		case IR::Opcode::Log2F16: opcode = IR::ValueOpcode::FPLog2; break;
		case IR::Opcode::Exp2F16: opcode = IR::ValueOpcode::FPExp2; break;
		case IR::Opcode::FloorF16: opcode = IR::ValueOpcode::FPFloor32; break;
		case IR::Opcode::CeilF16: opcode = IR::ValueOpcode::FPCeil32; break;
		case IR::Opcode::TruncF16: opcode = IR::ValueOpcode::FPTrunc32; break;
		case IR::Opcode::RoundEvenF16: opcode = IR::ValueOpcode::FPRoundEven32; break;
		case IR::Opcode::Min3F16: opcode = IR::ValueOpcode::FPMinTri32; break;
		case IR::Opcode::Max3F16: opcode = IR::ValueOpcode::FPMaxTri32; break;
		case IR::Opcode::Med3F16: opcode = IR::ValueOpcode::FPMedTri32; break;
		default: return false;
	}
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < inst.src_count; index++) {
		args[index] = inst.op == IR::Opcode::MadMixF16 ? IR::Value(ReadMixF32(inst.src[index]))
		                                               : IR::Value(ReadF16AsF32(inst.src[index]));
	}
	IR::F32 result;
	switch (inst.src_count) {
		case 1: result = IR::F32(ir.Emit(opcode, {args[0]})); break;
		case 2: result = IR::F32(ir.Emit(opcode, {args[0], args[1]})); break;
		case 3: result = IR::F32(ir.Emit(opcode, {args[0], args[1], args[2]})); break;
		default: EXIT("invalid half-float source count: %u", inst.src_count);
	}
	WriteF16(inst.dst, result);
	return true;
}

bool Translator::TranslateFloatOperation(const IR::Instruction& inst) {
	IR::ValueOpcode opcode {};
	switch (inst.op) {
		case IR::Opcode::RcpF32: opcode = IR::ValueOpcode::FPRecip32; break;
		case IR::Opcode::RcpIflagF32: opcode = IR::ValueOpcode::FPRecipIFlag32; break;
		case IR::Opcode::FractF32: opcode = IR::ValueOpcode::FPFract32; break;
		case IR::Opcode::TruncF32: opcode = IR::ValueOpcode::FPTrunc32; break;
		case IR::Opcode::CeilF32: opcode = IR::ValueOpcode::FPCeil32; break;
		case IR::Opcode::RoundEvenF32: opcode = IR::ValueOpcode::FPRoundEven32; break;
		case IR::Opcode::FloorF32: opcode = IR::ValueOpcode::FPFloor32; break;
		case IR::Opcode::Exp2F32: opcode = IR::ValueOpcode::FPExp2; break;
		case IR::Opcode::Log2F32: opcode = IR::ValueOpcode::FPLog2; break;
		case IR::Opcode::InverseSqrtF32: opcode = IR::ValueOpcode::FPRecipSqrt32; break;
		case IR::Opcode::SqrtF32: opcode = IR::ValueOpcode::FPSqrt; break;
		case IR::Opcode::SinF32: opcode = IR::ValueOpcode::FPSin; break;
		case IR::Opcode::CosF32: opcode = IR::ValueOpcode::FPCos; break;
		case IR::Opcode::FAddF32: opcode = IR::ValueOpcode::FPAdd32; break;
		case IR::Opcode::FSubF32: opcode = IR::ValueOpcode::FPSub32; break;
		case IR::Opcode::FMulF32: opcode = IR::ValueOpcode::FPMul32; break;
		case IR::Opcode::FMinF32: opcode = IR::ValueOpcode::FPMin32; break;
		case IR::Opcode::FMaxF32: opcode = IR::ValueOpcode::FPMax32; break;
		case IR::Opcode::FMadF32: opcode = IR::ValueOpcode::FPFma32; break;
		case IR::Opcode::FMin3F32: opcode = IR::ValueOpcode::FPMinTri32; break;
		case IR::Opcode::FMax3F32: opcode = IR::ValueOpcode::FPMaxTri32; break;
		case IR::Opcode::FMed3F32: opcode = IR::ValueOpcode::FPMedTri32; break;
		case IR::Opcode::LdexpF32: opcode = IR::ValueOpcode::FPLdexp; break;
		default: break;
	}
	if (opcode != IR::ValueOpcode {}) {
		std::array<IR::Value, 3> args;
		for (uint32_t index = 0; index < inst.src_count; index++) {
			const auto arg_type = IR::ArgTypeOf(opcode, index);
			args[index] =
			    arg_type == IR::Type::F32
			        ? (inst.op == IR::Opcode::FMadF32 ? IR::Value(ReadMixF32(inst.src[index]))
			                                          : ReadOperand(inst.src[index], IR::Type::F32))
			        : ReadOperand(inst.src[index], arg_type);
		}
		IR::Value result;
		switch (inst.src_count) {
			case 1: result = ir.Emit(opcode, {args[0]}); break;
			case 2: result = ir.Emit(opcode, {args[0], args[1]}); break;
			case 3: result = ir.Emit(opcode, {args[0], args[1], args[2]}); break;
			default: EXIT("invalid floating-point source count: %u", inst.src_count);
		}
		WriteOperand(inst.dst, result);
		return true;
	}

	const auto select_f32 = [&](IR::U1 condition, IR::F32 true_value, IR::F32 false_value) {
		return IR::F32(ir.Emit(IR::ValueOpcode::SelectF32, {condition, true_value, false_value}));
	};
	if (inst.op == IR::Opcode::Dot2AccF32F16) {
		auto a          = inst.src[0];
		a.op_sel        = false;
		a.op_sel_hi     = true;
		auto b          = inst.src[1];
		b.op_sel        = false;
		b.op_sel_hi     = true;
		const auto a_lo = ReadF16LaneAsF32(a, false);
		const auto a_hi = ReadF16LaneAsF32(a, true);
		const auto b_lo = ReadF16LaneAsF32(b, false);
		const auto b_hi = ReadF16LaneAsF32(b, true);
		const auto acc  = ReadMixF32(inst.src[2]);
		const auto lo   = IR::F32(ir.Emit(IR::ValueOpcode::FPFma32, {a_lo, b_lo, acc}));
		WriteOperand(inst.dst, ir.Emit(IR::ValueOpcode::FPFma32, {a_hi, b_hi, lo}));
		return true;
	}
	if (inst.op != IR::Opcode::CubeIdF32 && inst.op != IR::Opcode::CubeScF32 &&
	    inst.op != IR::Opcode::CubeTcF32 && inst.op != IR::Opcode::CubeMaF32) {
		return false;
	}

	const auto x  = ReadMixF32(inst.src[0]);
	const auto y  = ReadMixF32(inst.src[1]);
	const auto z  = ReadMixF32(inst.src[2]);
	const auto nx = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {x}));
	const auto ny = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {y}));
	const auto nz = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {z}));
	const auto ax = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {x}));
	const auto ay = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {y}));
	const auto az = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {z}));
	const auto z_face =
	    ir.LogicalAnd(IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {az, ax})),
	                  IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {az, ay})));
	const auto y_face = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {ay, ax}));
	const auto x_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {x, IR::Value::F32(0.0f)}));
	const auto y_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {y, IR::Value::F32(0.0f)}));
	const auto z_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {z, IR::Value::F32(0.0f)}));
	const auto select_face = [&](IR::F32 x_value, IR::F32 y_value, IR::F32 z_value) {
		return select_f32(z_face, z_value, select_f32(y_face, y_value, x_value));
	};
	IR::F32 result;
	switch (inst.op) {
		case IR::Opcode::CubeIdF32:
			result = select_face(
			    select_f32(x_neg, IR::F32(IR::Value::F32(1.0f)), IR::F32(IR::Value::F32(0.0f))),
			    select_f32(y_neg, IR::F32(IR::Value::F32(3.0f)), IR::F32(IR::Value::F32(2.0f))),
			    select_f32(z_neg, IR::F32(IR::Value::F32(5.0f)), IR::F32(IR::Value::F32(4.0f))));
			break;
		case IR::Opcode::CubeScF32:
			result = select_face(select_f32(x_neg, z, nz), x, select_f32(z_neg, nx, x));
			break;
		case IR::Opcode::CubeTcF32: result = select_face(ny, select_f32(y_neg, nz, z), ny); break;
		case IR::Opcode::CubeMaF32: {
			const auto two = IR::F32(IR::Value::F32(2.0f));
			result         = select_face(IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {x, two})),
			                             IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {y, two})),
			                             IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {z, two})));
			break;
		}
		default: return false;
	}
	WriteOperand(inst.dst, result);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
