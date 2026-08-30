#include "graphics/shader/recompiler/frontend/translate/Translator.h"

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
		ir.SetExecMaskTag(IR::U1(IR::Value(true)));
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
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), false, cmpx);
}

void Translator::EmitFloatOrderedCompare(const Decoder::Instruction& inst, bool ordered) {
	const auto lhs       = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
	const auto rhs       = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
	const auto unordered = ir.LogicalOr(IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {lhs})),
	                                    IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {rhs})));
	EmitCompareResult(inst, ordered ? ir.LogicalNot(unordered) : unordered, false, false);
}

void Translator::EmitFloatClassCompare(const Decoder::Instruction& inst, bool cmpx) {
	const auto value = ReadOperand(inst.src0, IR::Type::F32);
	const auto mask  = ReadOperand(inst.src1, IR::Type::U32);
	EmitCompareResult(inst, IR::U1(ir.Emit(IR::ValueOpcode::FPCmpClass32, {value, mask})), false,
	                  cmpx);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
