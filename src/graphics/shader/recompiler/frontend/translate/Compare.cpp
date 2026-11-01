#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <bit>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::EmitCompareResult(const Decoder::Instruction& inst, IR::U1 value, bool scalar) {
	if (scalar) {
		ir.SetScc(value);
		return;
	}
	WriteVectorMask(inst.dst, value);
}

void Translator::EmitCompareConstant(const Decoder::Instruction& inst, bool value, bool scalar) {
	EmitCompareResult(inst, IR::U1(IR::Value(value)), scalar);
}

void Translator::EmitIntegerCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                    IR::Type type, bool scalar, bool swap_operands) {
	// The equality compares share their IR opcode with the unsigned ones.
	const bool signed_64 = opcode == IR::ValueOpcode::SLessThan64 ||
	                       opcode == IR::ValueOpcode::SLessThanEqual64 ||
	                       inst.opcode == Decoder::Opcode::V_CMP_EQ_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMPX_EQ_I64 ||
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
	auto lhs = read(inst.src0);
	auto rhs = read(inst.src1);
	if (swap_operands) {
		std::swap(lhs, rhs);
	}
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), scalar);
}

void Translator::EmitInteger16Compare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                      bool signed_value) {
	const auto lhs = ReadU16AsU32(inst.src0, signed_value);
	const auto rhs = ReadU16AsU32(inst.src1, signed_value);
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), false);
}

void Translator::EmitFloatCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                  bool half) {
	const auto type = IR::ArgTypeOf(opcode, 0);
	const auto lhs =
	    half ? IR::Value(ReadF16AsF32(inst.src0)) : ReadOperand(inst.src0, type);
	const auto rhs =
	    half ? IR::Value(ReadF16AsF32(inst.src1)) : ReadOperand(inst.src1, type);
	const IR::FPCompareFlags flags{
	    .flush_input_denorms = !half && type == IR::Type::F32 && flush_f32_inputs};
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs}, flags)), false);
}

void Translator::EmitFloatOrderedCompare(const Decoder::Instruction& inst, bool ordered,
                                         bool is64) {
	const auto type      = is64 ? IR::Type::F64 : IR::Type::F32;
	const auto is_nan_op = is64 ? IR::ValueOpcode::FPIsNan64 : IR::ValueOpcode::FPIsNan32;
	const auto lhs       = ReadOperand(inst.src0, type);
	const auto rhs       = ReadOperand(inst.src1, type);
	const auto unordered = ir.LogicalOr(IR::U1(ir.Emit(is_nan_op, {lhs})),
	                                    IR::U1(ir.Emit(is_nan_op, {rhs})));
	EmitCompareResult(inst, ordered ? ir.LogicalNot(unordered) : unordered, false);
}

void Translator::EmitFloatClassCompare(const Decoder::Instruction& inst, bool half) {
	const auto value = half ? IR::Value(Read16LaneBits(inst.src0, false))
	                        : ReadOperand(inst.src0, IR::Type::F32);
	const auto mask = half ? IR::Value(Read16LaneBits(inst.src1, false))
	                      : ReadOperand(inst.src1, IR::Type::U32);
	const auto opcode = half ? IR::ValueOpcode::FPCmpClass16 : IR::ValueOpcode::FPCmpClass32;
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {value, mask})), false);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
