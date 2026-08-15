#include "graphics/shader/recompiler/ir/IREmitter.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

IREmitter::IREmitter(Block* value_block): block(value_block) {
	EXIT_IF(block == nullptr);
}

void IREmitter::SetBlock(Block* value_block) {
	EXIT_IF(value_block == nullptr);
	block = value_block;
}

Value IREmitter::Emit(ValueOpcode opcode, std::initializer_list<Value> args, uint64_t flags) {
	auto& inst = block->AppendNewInst(opcode, args, flags);
	return TypeOf(opcode) == Type::Void ? Value {} : Value(&inst);
}

U32 IREmitter::GetUserData(ScalarReg reg) {
	return U32(Emit(ValueOpcode::GetUserData, {Value(reg)}));
}

U32 IREmitter::GetScalarReg(ScalarReg reg) {
	return U32(Emit(ValueOpcode::GetScalarRegister, {Value(reg)}));
}

void IREmitter::SetScalarReg(ScalarReg reg, U32 value) {
	Emit(ValueOpcode::SetScalarRegister, {Value(reg), value});
}

U1 IREmitter::GetThreadBitScalarReg(ScalarReg reg) {
	return U1(Emit(ValueOpcode::GetThreadBitScalarRegister, {Value(reg)}));
}

void IREmitter::SetThreadBitScalarReg(ScalarReg reg, U1 value) {
	Emit(ValueOpcode::SetThreadBitScalarRegister, {Value(reg), value});
}

U32 IREmitter::GetVectorReg(VectorReg reg) {
	return U32(Emit(ValueOpcode::GetVectorRegister, {Value(reg)}));
}

void IREmitter::SetVectorReg(VectorReg reg, U32 value) {
	Emit(ValueOpcode::SetVectorRegister, {Value(reg), value});
}

U1 IREmitter::GetGotoVariable(uint32_t id) {
	return U1(Emit(ValueOpcode::GetGotoVariable, {Value(id)}));
}

void IREmitter::SetGotoVariable(uint32_t id, U1 value) {
	Emit(ValueOpcode::SetGotoVariable, {Value(id), value});
}

U1 IREmitter::GetScc() {
	return U1(Emit(ValueOpcode::GetScc));
}

void IREmitter::SetScc(U1 value) {
	Emit(ValueOpcode::SetScc, {value});
}

U1 IREmitter::GetExec() {
	return U1(Emit(ValueOpcode::GetExec));
}

void IREmitter::SetExec(U1 value) {
	Emit(ValueOpcode::SetExec, {value});
}

U32 IREmitter::GetExecLo() {
	return U32(Emit(ValueOpcode::GetExecLo));
}

void IREmitter::SetExecLo(U32 value) {
	Emit(ValueOpcode::SetExecLo, {value});
}

U32 IREmitter::GetExecHi() {
	return U32(Emit(ValueOpcode::GetExecHi));
}

void IREmitter::SetExecHi(U32 value) {
	Emit(ValueOpcode::SetExecHi, {value});
}

U1 IREmitter::GetVcc() {
	return U1(Emit(ValueOpcode::GetVcc));
}

void IREmitter::SetVcc(U1 value) {
	Emit(ValueOpcode::SetVcc, {value});
}

U32 IREmitter::GetVccLo() {
	return U32(Emit(ValueOpcode::GetVccLo));
}

void IREmitter::SetVccLo(U32 value) {
	Emit(ValueOpcode::SetVccLo, {value});
}

U32 IREmitter::GetVccHi() {
	return U32(Emit(ValueOpcode::GetVccHi));
}

void IREmitter::SetVccHi(U32 value) {
	Emit(ValueOpcode::SetVccHi, {value});
}

U32 IREmitter::GetM0() {
	return U32(Emit(ValueOpcode::GetM0));
}

void IREmitter::SetM0(U32 value) {
	Emit(ValueOpcode::SetM0, {value});
}

F32 IREmitter::BitCastF32(U32 value) {
	return F32(Emit(ValueOpcode::BitCastF32U32, {value}));
}

U32 IREmitter::BitCastU32(F32 value) {
	return U32(Emit(ValueOpcode::BitCastU32F32, {value}));
}

F16 IREmitter::BitCastF16(U32 value) {
	const auto half = U16(Emit(ValueOpcode::ConvertU16U32, {value}));
	return F16(Emit(ValueOpcode::BitCastF16U16, {half}));
}

U32 IREmitter::BitCastU32(F16 value) {
	const auto half = U16(Emit(ValueOpcode::BitCastU16F16, {value}));
	return U32(Emit(ValueOpcode::ConvertU32U16, {half}));
}

U64 IREmitter::PackUint2x32(U32 low, U32 high) {
	const auto pair = Emit(ValueOpcode::CompositeConstructU32x2, {low, high});
	return U64(Emit(ValueOpcode::PackUint2x32, {pair}));
}

U32 IREmitter::CompositeExtract(Value composite, uint32_t index) {
	return U32(Emit(ValueOpcode::CompositeExtractU32x2, {composite, Value(index)}));
}

U32 IREmitter::IAdd(U32 lhs, U32 rhs) {
	return U32(Emit(ValueOpcode::IAdd32, {lhs, rhs}));
}

U32 IREmitter::ISub(U32 lhs, U32 rhs) {
	return U32(Emit(ValueOpcode::ISub32, {lhs, rhs}));
}

U32 IREmitter::IMul(U32 lhs, U32 rhs) {
	return U32(Emit(ValueOpcode::IMul32, {lhs, rhs}));
}

U32 IREmitter::ShiftLeftLogical(U32 value, U32 shift) {
	return U32(Emit(ValueOpcode::ShiftLeftLogical32, {value, shift}));
}

U32 IREmitter::ShiftRightLogical(U32 value, U32 shift) {
	return U32(Emit(ValueOpcode::ShiftRightLogical32, {value, shift}));
}

U32 IREmitter::ShiftRightArithmetic(U32 value, U32 shift) {
	return U32(Emit(ValueOpcode::ShiftRightArithmetic32, {value, shift}));
}

U32 IREmitter::BitwiseAnd(U32 lhs, U32 rhs) {
	return U32(Emit(ValueOpcode::BitwiseAnd32, {lhs, rhs}));
}

U32 IREmitter::BitwiseOr(U32 lhs, U32 rhs) {
	return U32(Emit(ValueOpcode::BitwiseOr32, {lhs, rhs}));
}

U32 IREmitter::BitwiseXor(U32 lhs, U32 rhs) {
	return U32(Emit(ValueOpcode::BitwiseXor32, {lhs, rhs}));
}

U32 IREmitter::BitwiseNot(U32 value) {
	return U32(Emit(ValueOpcode::BitwiseNot32, {value}));
}

U32 IREmitter::Select(U1 condition, U32 true_value, U32 false_value) {
	return U32(Emit(ValueOpcode::SelectU32, {condition, true_value, false_value}));
}

U1 IREmitter::IEqual(U32 lhs, U32 rhs) {
	return U1(Emit(ValueOpcode::IEqual32, {lhs, rhs}));
}

U1 IREmitter::INotEqual(U32 lhs, U32 rhs) {
	return U1(Emit(ValueOpcode::INotEqual32, {lhs, rhs}));
}

U1 IREmitter::ULessThan(U32 lhs, U32 rhs) {
	return U1(Emit(ValueOpcode::ULessThan32, {lhs, rhs}));
}

U1 IREmitter::UGreaterThan(U32 lhs, U32 rhs) {
	return U1(Emit(ValueOpcode::UGreaterThan32, {lhs, rhs}));
}

U1 IREmitter::LogicalAnd(U1 lhs, U1 rhs) {
	return U1(Emit(ValueOpcode::LogicalAnd, {lhs, rhs}));
}

U1 IREmitter::LogicalOr(U1 lhs, U1 rhs) {
	return U1(Emit(ValueOpcode::LogicalOr, {lhs, rhs}));
}

U1 IREmitter::LogicalNot(U1 value) {
	return U1(Emit(ValueOpcode::LogicalNot, {value}));
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
