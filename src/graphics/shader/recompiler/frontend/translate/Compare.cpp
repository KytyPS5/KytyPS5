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
	const auto lhs = ReadOperand(inst.src0, type);
	const auto rhs = ReadOperand(inst.src1, type);
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), scalar, cmpx);
}

void Translator::EmitInteger64CompareVia(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                         bool swap, bool negate, bool cmpx) {
	const auto lhs    = ReadOperand(inst.src0, IR::Type::U64);
	const auto rhs    = ReadOperand(inst.src1, IR::Type::U64);
	const auto result = swap ? IR::U1(ir.Emit(opcode, {rhs, lhs})) : IR::U1(ir.Emit(opcode, {lhs, rhs}));
	EmitCompareResult(inst, negate ? ir.LogicalNot(result) : result, false, cmpx);
}

void Translator::EmitInteger16Compare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                      bool signed_value, bool cmpx) {
	const auto lhs = ReadU16AsU32(inst.src0, signed_value);
	const auto rhs = ReadU16AsU32(inst.src1, signed_value);
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), false, cmpx);
}

void Translator::EmitFloatCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                  bool half, bool cmpx) {
	const auto lhs =
	    half ? IR::Value(ReadF16AsF32(inst.src0)) : ReadOperand(inst.src0, IR::Type::F32);
	const auto rhs =
	    half ? IR::Value(ReadF16AsF32(inst.src1)) : ReadOperand(inst.src1, IR::Type::F32);
	const IR::FPCompareFlags flags{.flush_input_denorms = !half && flush_f32_inputs};
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs}, flags)), false, cmpx);
}

void Translator::EmitFloat64Compare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                    bool cmpx) {
	const auto lhs = ReadOperand(inst.src0, IR::Type::F64);
	const auto rhs = ReadOperand(inst.src1, IR::Type::F64);
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), false, cmpx);
}

void Translator::EmitFloatOrderedCompare(const Decoder::Instruction& inst, bool ordered, bool cmpx) {
	const auto lhs       = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
	const auto rhs       = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
	const auto unordered = ir.LogicalOr(IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {lhs})),
	                                    IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {rhs})));
	EmitCompareResult(inst, ordered ? ir.LogicalNot(unordered) : unordered, false, cmpx);
}

void Translator::EmitFloat64Equal(const Decoder::Instruction& inst) {
	// Compare the IEEE encoding using DWORD operations, preserving subnormals without
	// requiring host Float64 support (the existing FP64 denormal-preserving mode).
	const auto zero = IR::U32(IR::Value(0u));
	const auto read = [&](const Decoder::Operand& operand) {
		auto words = ReadU32Pair(operand);
		if (operand.kind == Decoder::OperandKind::LiteralConstant) {
			// An FP64 literal supplies the high DWORD, unlike an integer literal.
			words = {zero, IR::U32(IR::Value(operand.value))};
		} else if (operand.kind == Decoder::OperandKind::FloatInlineConstant) {
			// Selector 248 has its own FP64 value (0x3fc45f306dc9c882, as LLVM encodes it),
			// not the widened FP32 value.
			const auto bits = operand.value == std::bit_cast<uint32_t>(0.15915494309189535f)
			                      ? uint64_t {0x3fc45f306dc9c882ull}
			                      : std::bit_cast<uint64_t>(
			                            static_cast<double>(std::bit_cast<float>(operand.value)));
			words           = {IR::U32(IR::Value(static_cast<uint32_t>(bits))),
			                   IR::U32(IR::Value(static_cast<uint32_t>(bits >> 32u)))};
		}
		if (operand.absolute) {
			words[1] = ir.BitwiseAnd(words[1], IR::U32(IR::Value(0x7fffffffu)));
		}
		if (operand.negate) {
			words[1] = ir.BitwiseXor(words[1], IR::U32(IR::Value(0x80000000u)));
		}
		return words;
	};
	const auto lhs          = read(inst.src0);
	const auto rhs          = read(inst.src1);
	const auto magnitude_hi = [&](const auto& words) {
		return ir.BitwiseAnd(words[1], IR::U32(IR::Value(0x7fffffffu)));
	};
	const auto is_nan = [&](const auto& words) {
		const auto high     = magnitude_hi(words);
		const auto infinity = IR::U32(IR::Value(0x7ff00000u));
		return ir.LogicalOr(ir.UGreaterThan(high, infinity),
		                    ir.LogicalAnd(ir.IEqual(high, infinity), ir.INotEqual(words[0], zero)));
	};
	const auto same_bits = ir.LogicalAnd(ir.IEqual(lhs[0], rhs[0]), ir.IEqual(lhs[1], rhs[1]));
	const auto both_zero =
	    ir.IEqual(ir.BitwiseOr(ir.BitwiseOr(lhs[0], rhs[0]),
	                           ir.BitwiseOr(magnitude_hi(lhs), magnitude_hi(rhs))),
	              zero);
	const auto ordered = ir.LogicalNot(ir.LogicalOr(is_nan(lhs), is_nan(rhs)));
	EmitCompareResult(inst, ir.LogicalAnd(ordered, ir.LogicalOr(same_bits, both_zero)), false,
	                  false);
}

void Translator::EmitFloatClassCompare(const Decoder::Instruction& inst, bool cmpx) {
	const auto value = ReadOperand(inst.src0, IR::Type::F32);
	const auto mask  = ReadOperand(inst.src1, IR::Type::U32);
	EmitCompareResult(inst, IR::U1(ir.Emit(IR::ValueOpcode::FPCmpClass32, {value, mask})), false,
	                  cmpx);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
