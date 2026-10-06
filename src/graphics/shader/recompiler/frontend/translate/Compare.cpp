#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <bit>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::EmitCompareResult(const Decoder::Instruction& inst, IR::U1 value, bool scalar,
                                   bool cmpx) {
	if (scalar) {
		ir.SetScc(value);
		return;
	}
	const auto masked = ir.LogicalAnd(ir.GetExec(), value);
	if (cmpx) {
		const auto mask = BallotMask(masked);
		ir.SetExec(masked);
		ir.SetExecLo(mask[0]);
		ir.SetExecHi(mask[1]);
		return;
	}
	WriteMask(inst.dst, masked);
}

void Translator::EmitCompareConstant(const Decoder::Instruction& inst, bool value, bool scalar,
                                     bool cmpx) {
	EmitCompareResult(inst, IR::U1(IR::Value(value)), scalar, cmpx);
}

void Translator::EmitIntegerCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                    IR::Type type, bool scalar, bool cmpx) {
	const bool signed_64 = opcode == IR::ValueOpcode::SLessThan64 ||
	                       opcode == IR::ValueOpcode::SLessThanEqual64 ||
	                       inst.opcode == Decoder::Opcode::V_CMP_EQ_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMP_NE_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMPX_NE_I64;
	const auto read = [&](const Decoder::Operand& operand) {
		// RDNA2 expands signed 64-bit integer literals by sign extension.
		if (signed_64 && operand.kind == Decoder::OperandKind::LiteralConstant) {
			return IR::Value(static_cast<uint64_t>(
			    static_cast<int64_t>(std::bit_cast<int32_t>(operand.value))));
		}
		return ReadOperand(operand, type);
	};
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {read(inst.src0), read(inst.src1)})), scalar,
	                  cmpx);
}

void Translator::EmitInteger16Compare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                      bool signed_value, bool cmpx) {
	const auto lhs = ReadU16AsU32(inst.src0, signed_value);
	const auto rhs = ReadU16AsU32(inst.src1, signed_value);
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), false, cmpx);
}

void Translator::EmitFloatCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                  bool half, bool cmpx) {
	const auto type = IR::ArgTypeOf(opcode, 0);
	const auto lhs =
	    half ? IR::Value(ReadF16AsF32(inst.src0)) : ReadOperand(inst.src0, type);
	const auto rhs =
	    half ? IR::Value(ReadF16AsF32(inst.src1)) : ReadOperand(inst.src1, type);
	const IR::FPCompareFlags flags{
	    .flush_input_denorms = !half && type == IR::Type::F32 && flush_f32_inputs};
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs}, flags)), false, cmpx);
}

void Translator::EmitFloatOrderedCompare(const Decoder::Instruction& inst, bool ordered, bool cmpx) {
	const auto lhs       = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
	const auto rhs       = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
	const auto unordered = ir.LogicalOr(IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {lhs})),
	                                    IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {rhs})));
	EmitCompareResult(inst, ordered ? ir.LogicalNot(unordered) : unordered, false, cmpx);
}

void Translator::EmitFloatClassCompare(const Decoder::Instruction& inst, bool cmpx, bool half) {
	const auto value = half ? IR::Value(Read16LaneBits(inst.src0, false))
	                        : ReadOperand(inst.src0, IR::Type::F32);
	const auto mask = half ? IR::Value(Read16LaneBits(inst.src1, false))
	                      : ReadOperand(inst.src1, IR::Type::U32);
	const auto opcode = half ? IR::ValueOpcode::FPCmpClass16 : IR::ValueOpcode::FPCmpClass32;
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {value, mask})), false, cmpx);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
