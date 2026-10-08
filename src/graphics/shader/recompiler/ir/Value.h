#pragma once

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/Reg.h"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Block;
class Inst;

class Value {
public:
	Value() = default;
	explicit Value(Inst* value);
	explicit Value(ScalarReg value);
	explicit Value(VectorReg value);
	explicit Value(bool value);
	explicit Value(uint8_t value);
	explicit Value(uint16_t value);
	explicit Value(uint32_t value);
	explicit Value(uint64_t value);

	static Value F16(uint16_t bits);
	static Value F32(float value);

	[[nodiscard]] bool IsEmpty() const;
	[[nodiscard]] bool IsImmediate() const;
	[[nodiscard]] bool IsIdentity() const;
	[[nodiscard]] bool IsPhi() const;
	[[nodiscard]] Type GetType() const;

	[[nodiscard]] Inst*     Instruction() const;
	[[nodiscard]] Inst*     TryInstruction() const;
	[[nodiscard]] Inst*     ResolveInstruction() const;
	[[nodiscard]] Value     Resolve() const;
	[[nodiscard]] ScalarReg ScalarRegister() const;
	[[nodiscard]] VectorReg VectorRegister() const;
	[[nodiscard]] bool      U1() const;
	[[nodiscard]] uint8_t   U8() const;
	[[nodiscard]] uint16_t  U16() const;
	[[nodiscard]] uint32_t  U32() const;
	[[nodiscard]] uint64_t  U64() const;
	[[nodiscard]] uint16_t  F16Bits() const;
	[[nodiscard]] float     F32Value() const;

	bool operator==(const Value& other) const;

private:
	Type type = Type::Void;
	union {
		Inst*     inst;
		ScalarReg scalar_reg;
		VectorReg vector_reg;
		bool      imm_u1;
		uint8_t   imm_u8;
		uint16_t  imm_u16;
		uint32_t  imm_u32;
		uint64_t  imm_u64;
	};

	explicit Value(Type type, uint64_t bits);
};
static_assert(std::is_trivially_copyable_v<Value>);

template <Type type_>
class TypedValue: public Value {
public:
	TypedValue() = default;

	template <Type other_type>
	requires(TypesOverlap(type_, other_type))
	TypedValue(const TypedValue<other_type>& value): Value(value) {}

	explicit TypedValue(Value value): Value(value) {
		EXIT_IF(!AreTypesCompatible(value.GetType(), type_));
	}
};

using U1     = TypedValue<Type::U1>;
using U8     = TypedValue<Type::U8>;
using U16    = TypedValue<Type::U16>;
using U32    = TypedValue<Type::U32>;
using U64    = TypedValue<Type::U64>;
using F16    = TypedValue<Type::F16>;
using F32    = TypedValue<Type::F32>;
using U32F32 = TypedValue<Type::U32 | Type::F32>;

struct Use {
	Inst*  user    = nullptr;
	size_t operand = 0;

	bool operator==(const Use&) const = default;
};

class Inst {
public:
	explicit Inst(ValueOpcode opcode, uint64_t flags = 0);
	~Inst();

	Inst(const Inst&)            = delete;
	Inst& operator=(const Inst&) = delete;
	Inst(Inst&&)                 = delete;
	Inst& operator=(Inst&&)      = delete;

	[[nodiscard]] ValueOpcode             GetOpcode() const;
	[[nodiscard]] Type                    GetType() const;
	[[nodiscard]] bool                    MayHaveSideEffects() const;
	[[nodiscard]] bool                    HasUses() const;
	[[nodiscard]] size_t                  UseCount() const;
	[[nodiscard]] size_t                  NumArgs() const;
	[[nodiscard]] size_t                  NumPhiBlocks() const;
	[[nodiscard]] Value                   Arg(size_t index) const;
	[[nodiscard]] Block*                  PhiBlock(size_t index) const;
	[[nodiscard]] Block*                  Parent() const;
	[[nodiscard]] const std::vector<Use>& Uses() const;
	// Runtime indices belong to the resource plan that owns this instruction.
	[[nodiscard]] uint32_t EvaluationIndex(uint32_t& count) const {
		if (evaluation_index == UINT32_MAX) {
			evaluation_index = count++;
		}
		return evaluation_index;
	}

	void SetParent(Block* block);
	void SetArg(size_t index, Value value);
	void AddPhiOperand(Block* predecessor, Value value);
	void ReplaceUsesWith(Value replacement, bool preserve = true);
	void Invalidate();

	template <typename T>
	requires(sizeof(T) <= sizeof(uint64_t) && std::is_trivially_copyable_v<T>)
	[[nodiscard]] T Flags() const {
		T result {};
		std::memcpy(&result, &flags, sizeof(result));
		return result;
	}

	template <typename T>
	requires(sizeof(T) <= sizeof(uint64_t) && std::is_trivially_copyable_v<T>)
	void SetFlags(T value) {
		flags = 0;
		std::memcpy(&flags, &value, sizeof(value));
	}

private:
	friend void EliminateDeadCode(const std::vector<Block*>& blocks);

	void AddUse(Inst* used, size_t operand);
	void RemoveUse(Inst* used, size_t operand);
	void ClearArgs();

	static constexpr uint8_t InlineArity = 4;
	static constexpr uint8_t PhiArity = UINT8_MAX;

	ValueOpcode         opcode;
	uint8_t             num_args;
	bool                live = false;
	mutable uint32_t    evaluation_index = UINT32_MAX;
	uint64_t            flags;
	Block*              parent = nullptr;
	union {
		std::array<Value, InlineArity> fixed_args {};
		std::vector<Value> large_args;
		std::vector<std::pair<Block*, Value>> phi_args;
	};
	std::vector<Use>    uses;
};

static_assert(sizeof(Inst) <= 112, "Inst operand storage unintentionally increased");

// Hot accessors of the resource walker are defined inline; they need the full Inst.
inline bool Value::IsEmpty() const {
	return type == Type::Void;
}

inline bool Value::IsImmediate() const {
	return type != Type::Opaque;
}

inline bool Value::IsIdentity() const {
	return type == Type::Opaque && inst->GetOpcode() == ValueOpcode::Identity;
}

inline bool Value::IsPhi() const {
	return type == Type::Opaque && inst->GetOpcode() == ValueOpcode::Phi;
}

inline Inst* Value::TryInstruction() const {
	return type == Type::Opaque ? inst : nullptr;
}

inline Value Value::Resolve() const {
	// Identity chains are followed iteratively; the walker resolves every operand it visits.
	Value value = *this;
	while (value.IsIdentity()) {
		value = value.inst->Arg(0);
	}
	return value;
}

inline Inst* Value::ResolveInstruction() const {
	EXIT_IF(type != Type::Opaque);
	return Resolve().inst;
}

inline bool Value::U1() const {
	EXIT_IF(type != Type::U1);
	return imm_u1;
}

inline uint8_t Value::U8() const {
	EXIT_IF(type != Type::U8);
	return imm_u8;
}

inline uint16_t Value::U16() const {
	EXIT_IF(type != Type::U16);
	return imm_u16;
}

inline uint32_t Value::U32() const {
	EXIT_IF(type != Type::U32);
	return imm_u32;
}

inline uint64_t Value::U64() const {
	EXIT_IF(type != Type::U64);
	return imm_u64;
}

inline uint16_t Value::F16Bits() const {
	EXIT_IF(type != Type::F16);
	return imm_u16;
}

inline float Value::F32Value() const {
	EXIT_IF(type != Type::F32);
	return std::bit_cast<float>(imm_u32);
}

inline ValueOpcode Inst::GetOpcode() const {
	return opcode;
}

inline size_t Inst::NumArgs() const {
	return num_args == PhiArity ? phi_args.size() : num_args;
}

inline bool Value::operator==(const Value& other) const {
	if (type != other.type) {
		return false;
	}
	switch (type) {
		case Type::Void: return true;
		case Type::Opaque: return inst == other.inst;
		case Type::ScalarReg: return scalar_reg == other.scalar_reg;
		case Type::VectorReg: return vector_reg == other.vector_reg;
		case Type::U1: return imm_u1 == other.imm_u1;
		case Type::U8: return imm_u8 == other.imm_u8;
		case Type::U16:
		case Type::F16: return imm_u16 == other.imm_u16;
		case Type::U32:
		case Type::F32: return imm_u32 == other.imm_u32;
		case Type::U64: return imm_u64 == other.imm_u64;
		default: return false;
	}
}

inline Value Inst::Arg(size_t index) const {
	EXIT_IF(index >= NumArgs());
	if (num_args <= InlineArity) {
		return fixed_args[index];
	}
	return num_args == PhiArity ? phi_args[index].second : large_args[index];
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
