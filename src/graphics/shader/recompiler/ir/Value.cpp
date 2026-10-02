#include "graphics/shader/recompiler/ir/Value.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace Libs::Graphics::ShaderRecompiler::IR {

Value::Value(Inst* value): type(Type::Opaque), inst(value) {}
Value::Value(ScalarReg value): type(Type::ScalarReg), scalar_reg(value) {}
Value::Value(VectorReg value): type(Type::VectorReg), vector_reg(value) {}
Value::Value(bool value): type(Type::U1), imm_u1(value) {}
Value::Value(uint8_t value): type(Type::U8), imm_u8(value) {}
Value::Value(uint16_t value): type(Type::U16), imm_u16(value) {}
Value::Value(uint32_t value): type(Type::U32), imm_u32(value) {}
Value::Value(uint64_t value): type(Type::U64), imm_u64(value) {}

Value::Value(Type value_type, uint64_t bits): type(value_type), imm_u64(bits) {}

Value Value::F16(uint16_t bits) {
	return Value(Type::F16, bits);
}

Value Value::F32(float value) {
	return Value(Type::F32, std::bit_cast<uint32_t>(value));
}

bool Value::IsEmpty() const {
	return type == Type::Void;
}

bool Value::IsImmediate() const {
	return type != Type::Opaque;
}

bool Value::IsIdentity() const {
	return type == Type::Opaque && inst->GetOpcode() == ValueOpcode::Identity;
}

bool Value::IsPhi() const {
	return type == Type::Opaque && inst->GetOpcode() == ValueOpcode::Phi;
}

Type Value::GetType() const {
	if (IsPhi()) {
		return inst->Flags<Type>();
	}
	if (IsIdentity()) {
		return inst->Arg(0).GetType();
	}
	return type == Type::Opaque ? inst->GetType() : type;
}

Inst* Value::Instruction() const {
	EXIT_IF(type != Type::Opaque);
	return inst;
}

Inst* Value::TryInstruction() const {
	return type == Type::Opaque ? inst : nullptr;
}

Inst* Value::ResolveInstruction() const {
	EXIT_IF(type != Type::Opaque);
	return IsIdentity() ? inst->Arg(0).ResolveInstruction() : inst;
}

Value Value::Resolve() const {
	return IsIdentity() ? inst->Arg(0).Resolve() : *this;
}

ScalarReg Value::ScalarRegister() const {
	EXIT_IF(type != Type::ScalarReg);
	return scalar_reg;
}

VectorReg Value::VectorRegister() const {
	EXIT_IF(type != Type::VectorReg);
	return vector_reg;
}

bool Value::U1() const {
	EXIT_IF(type != Type::U1);
	return imm_u1;
}

uint8_t Value::U8() const {
	EXIT_IF(type != Type::U8);
	return imm_u8;
}

uint16_t Value::U16() const {
	EXIT_IF(type != Type::U16);
	return imm_u16;
}

uint32_t Value::U32() const {
	EXIT_IF(type != Type::U32);
	return imm_u32;
}

uint64_t Value::U64() const {
	EXIT_IF(type != Type::U64);
	return imm_u64;
}

uint16_t Value::F16Bits() const {
	EXIT_IF(type != Type::F16);
	return imm_u16;
}

float Value::F32Value() const {
	EXIT_IF(type != Type::F32);
	return std::bit_cast<float>(imm_u32);
}

bool Value::operator==(const Value& other) const {
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

Inst::Inst(ValueOpcode value_opcode, uint64_t value_flags)
    : opcode(value_opcode),
      num_args(value_opcode == ValueOpcode::Phi ? PhiArity
                                               : static_cast<uint8_t>(NumArgsOf(value_opcode))),
      flags(value_flags) {
	if (num_args == PhiArity) {
		std::destroy_at(&fixed_args);
		std::construct_at(&phi_args);
	} else if (num_args > InlineArity) {
		std::destroy_at(&fixed_args);
		std::construct_at(&large_args, num_args);
	}
}

Inst::~Inst() {
	ClearArgs();
}

ValueOpcode Inst::GetOpcode() const {
	return opcode;
}

Type Inst::GetType() const {
	if (opcode == ValueOpcode::Phi) {
		return static_cast<Type>(flags);
	}
	if (opcode == ValueOpcode::Identity && num_args != 0) {
		return Arg(0).GetType();
	}
	return TypeOf(opcode);
}

bool Inst::MayHaveSideEffects() const {
	return HasSideEffects(opcode);
}

bool Inst::HasUses() const {
	return !uses.empty();
}

size_t Inst::UseCount() const {
	return uses.size();
}

size_t Inst::NumArgs() const {
	return num_args == PhiArity ? phi_args.size() : num_args;
}

size_t Inst::NumPhiBlocks() const {
	return num_args == PhiArity ? phi_args.size() : 0;
}

Value Inst::Arg(size_t index) const {
	EXIT_IF(index >= NumArgs());
	if (num_args <= InlineArity) {
		return fixed_args[index];
	}
	return num_args == PhiArity ? phi_args[index].second : large_args[index];
}

Block* Inst::PhiBlock(size_t index) const {
	EXIT_IF(opcode != ValueOpcode::Phi || index >= phi_args.size());
	return phi_args[index].first;
}

Block* Inst::Parent() const {
	return parent;
}

const std::vector<Use>& Inst::Uses() const {
	return uses;
}

void Inst::SetParent(Block* block) {
	parent = block;
}

void Inst::SetArg(size_t index, Value value) {
	const auto old = Arg(index);
	if (auto* old_inst = old.TryInstruction(); old_inst != nullptr) {
		RemoveUse(old_inst, index);
	}
	if (num_args <= InlineArity) {
		fixed_args[index] = value;
	} else if (num_args == PhiArity) {
		phi_args[index].second = value;
	} else {
		large_args[index] = value;
	}
	if (auto* new_inst = value.TryInstruction(); new_inst != nullptr) {
		AddUse(new_inst, index);
	}
}

void Inst::AddPhiOperand(Block* predecessor, Value value) {
	EXIT_IF(opcode != ValueOpcode::Phi);
	const auto index = phi_args.size();
	phi_args.emplace_back(predecessor, value);
	if (auto* value_inst = value.TryInstruction(); value_inst != nullptr) {
		AddUse(value_inst, index);
	}
}

void Inst::ReplaceUsesWith(Value replacement, bool preserve) {
	const auto old_uses = uses;
	for (const auto& use: old_uses) {
		use.user->SetArg(use.operand, replacement);
	}
	Invalidate();
	if (preserve) {
		opcode = ValueOpcode::Identity;
		num_args = 1;
		SetArg(0, replacement);
	}
}

void Inst::Invalidate() {
	ClearArgs();
	opcode = ValueOpcode::Void;
}

void Inst::AddUse(Inst* used, size_t operand) {
	const auto found = std::ranges::find_if(
	    used->uses, [&](const Use& use) { return use.user == this && use.operand == operand; });
	EXIT_IF(found != used->uses.end());
	used->uses.push_back({this, operand});
}

void Inst::RemoveUse(Inst* used, size_t operand) {
	const auto found = std::ranges::find_if(
	    used->uses, [&](const Use& use) { return use.user == this && use.operand == operand; });
	EXIT_IF(found == used->uses.end());
	used->uses.erase(found);
}

void Inst::ClearArgs() {
	for (size_t index = 0; index < NumArgs(); index++) {
		if (auto* value_inst = Arg(index).TryInstruction(); value_inst != nullptr) {
			RemoveUse(value_inst, index);
		}
	}
	if (num_args == PhiArity) {
		std::destroy_at(&phi_args);
		std::construct_at(&fixed_args);
	} else if (num_args > InlineArity) {
		std::destroy_at(&large_args);
		std::construct_at(&fixed_args);
	} else {
		fixed_args.fill(Value {});
	}
	num_args = 0;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
