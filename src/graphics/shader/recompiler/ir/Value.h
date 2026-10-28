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
	explicit Value(const Inst* value) = delete;
	explicit Value(ScalarReg value);
	explicit Value(VectorReg value);
	explicit Value(bool value);
	explicit Value(uint8_t value);
	explicit Value(uint16_t value);
	explicit Value(uint32_t value);
	explicit Value(uint64_t value);

	static Value F16(uint16_t bits);
	static Value F32(float value);

	[[nodiscard]] bool IsEmpty() const { return type == Type::Void; }
	[[nodiscard]] bool IsImmediate() const { return type != Type::Opaque; }
	[[nodiscard]] bool IsIdentity() const;
	[[nodiscard]] bool IsPhi() const;
	[[nodiscard]] Type GetType() const;

	[[nodiscard]] Inst*     Instruction() const;
	[[nodiscard]] Inst*     TryInstruction() const { return type == Type::Opaque ? inst : nullptr; }
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

	[[nodiscard]] ValueOpcode             GetOpcode() const { return opcode; }
	[[nodiscard]] Type                    GetType() const;
	[[nodiscard]] bool                    MayHaveSideEffects() const;
	[[nodiscard]] bool                    HasUses() const;
	[[nodiscard]] size_t                  UseCount() const;
	[[nodiscard]] size_t                  NumArgs() const;
	[[nodiscard]] size_t                  NumPhiBlocks() const;
	[[nodiscard]] Value                   Arg(size_t index) const;
	[[nodiscard]] Block*                  PhiBlock(size_t index) const;
	[[nodiscard]] Block*                  Parent() const { return parent; }
	[[nodiscard]] const std::vector<Use>& Uses() const { return uses; }
	// SRT walker output index, assigned by the resource plan that owns this instruction.
	[[nodiscard]] uint32_t WalkOutput() const { return walk_output; }
	void SetWalkOutput(uint32_t index) const { walk_output = index; }
	// Backend definitions belong to one emission. Wave64 emulation needs one per half.
	[[nodiscard]] uint32_t Definition(uint32_t half) const { return definitions[half]; }
	void SetDefinition(uint32_t half, uint32_t definition) const {
		definitions[half] = definition;
	}
	void ResetDefinitions() const { definitions = {}; }

	void SetParent(Block* block);
	void SetArg(size_t index, Value value);
	void AddPhiOperand(Block* predecessor, Value value);
	void ReplaceUsesWith(Value replacement, bool preserve = true);
	void Invalidate();
	// Drops the arguments without updating their use lists, for teardown that destroys both sides.
	void DropArgs();

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

	template <typename Self>
	static auto& ArgSlot(Self& self, size_t index);
	void AddUse(Inst* used, size_t operand);
	void RemoveUse(Inst* used, size_t operand);
	void ClearArgs();

	static constexpr uint8_t InlineArity = 4;
	static constexpr uint8_t PhiArity = UINT8_MAX;

	ValueOpcode         opcode;
	uint8_t             num_args;
	bool                live = false;
	mutable uint32_t    walk_output = UINT32_MAX;
	mutable std::array<uint32_t, 2> definitions {};
	uint64_t            flags;
	Block*              parent = nullptr;
	union {
		std::array<Value, InlineArity> fixed_args {};
		std::vector<Value> large_args;
		std::vector<std::pair<Block*, Value>> phi_args;
	};
	std::vector<Use>    uses;
};

static_assert(sizeof(Inst) <= 120, "Inst operand and backend storage unintentionally increased");

// Called per operand by the IR passes and the resource walker; inline, as the build has no LTO.
inline bool Value::IsIdentity() const {
	return type == Type::Opaque && inst->GetOpcode() == ValueOpcode::Identity;
}

inline bool Value::IsPhi() const {
	return type == Type::Opaque && inst->GetOpcode() == ValueOpcode::Phi;
}

inline Inst* Value::ResolveInstruction() const {
	EXIT_IF(type != Type::Opaque);
	return IsIdentity() ? inst->Arg(0).ResolveInstruction() : inst;
}

inline Value Value::Resolve() const {
	return IsIdentity() ? inst->Arg(0).Resolve() : *this;
}

inline uint32_t Value::U32() const {
	EXIT_IF(type != Type::U32);
	return imm_u32;
}

inline size_t Inst::NumArgs() const {
	return num_args == PhiArity ? phi_args.size() : num_args;
}

template <typename Self>
auto& Inst::ArgSlot(Self& self, size_t index) {
	EXIT_IF(index >= self.NumArgs());
	if (self.num_args <= InlineArity) {
		return self.fixed_args[index];
	}
	return self.num_args == PhiArity ? self.phi_args[index].second : self.large_args[index];
}

inline Value Inst::Arg(size_t index) const {
	return ArgSlot(*this, index);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
