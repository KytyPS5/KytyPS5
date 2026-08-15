#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace Libs::Graphics::ShaderRecompiler::IR {

class IREmitter {
public:
	explicit IREmitter(Block* block);

	void SetBlock(Block* block);

	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args = {}, uint64_t flags = 0);

	template <typename T>
	requires(sizeof(T) <= sizeof(uint64_t) && std::is_trivially_copyable_v<T>)
	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args, T flags) {
		uint64_t bits = 0;
		std::memcpy(&bits, &flags, sizeof(flags));
		return Emit(opcode, args, bits);
	}

	U32  GetUserData(ScalarReg reg);
	U32  GetScalarReg(ScalarReg reg);
	void SetScalarReg(ScalarReg reg, U32 value);
	U1   GetThreadBitScalarReg(ScalarReg reg);
	void SetThreadBitScalarReg(ScalarReg reg, U1 value);
	U32  GetVectorReg(VectorReg reg);
	void SetVectorReg(VectorReg reg, U32 value);
	U1   GetGotoVariable(uint32_t id);
	void SetGotoVariable(uint32_t id, U1 value);
	U1   GetScc();
	void SetScc(U1 value);
	U1   GetExec();
	void SetExec(U1 value);
	U32  GetExecLo();
	void SetExecLo(U32 value);
	U32  GetExecHi();
	void SetExecHi(U32 value);
	U1   GetVcc();
	void SetVcc(U1 value);
	U32  GetVccLo();
	void SetVccLo(U32 value);
	U32  GetVccHi();
	void SetVccHi(U32 value);
	U32  GetM0();
	void SetM0(U32 value);

	F32 BitCastF32(U32 value);
	U32 BitCastU32(F32 value);
	F16 BitCastF16(U32 value);
	U32 BitCastU32(F16 value);
	U64 PackUint2x32(U32 low, U32 high);
	U32 CompositeExtract(Value composite, uint32_t index);

	U32 IAdd(U32 lhs, U32 rhs);
	U32 ISub(U32 lhs, U32 rhs);
	U32 IMul(U32 lhs, U32 rhs);
	U32 ShiftLeftLogical(U32 value, U32 shift);
	U32 ShiftRightLogical(U32 value, U32 shift);
	U32 ShiftRightArithmetic(U32 value, U32 shift);
	U32 BitwiseAnd(U32 lhs, U32 rhs);
	U32 BitwiseOr(U32 lhs, U32 rhs);
	U32 BitwiseXor(U32 lhs, U32 rhs);
	U32 BitwiseNot(U32 value);
	U32 Select(U1 condition, U32 true_value, U32 false_value);
	U1  IEqual(U32 lhs, U32 rhs);
	U1  INotEqual(U32 lhs, U32 rhs);
	U1  ULessThan(U32 lhs, U32 rhs);
	U1  UGreaterThan(U32 lhs, U32 rhs);
	U1  LogicalAnd(U1 lhs, U1 rhs);
	U1  LogicalOr(U1 lhs, U1 rhs);
	U1  LogicalNot(U1 value);

private:
	Block* block = nullptr;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR
