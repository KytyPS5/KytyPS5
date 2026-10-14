#include "graphics/shader/recompiler/ir/Value.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace Libs::Graphics::ShaderRecompiler::IR {


Value Value::F16(uint16_t bits) {
	return Value(Type::F16, bits);
}

Value Value::F32(float value) {
	return Value(Type::F32, std::bit_cast<uint32_t>(value));
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

bool Inst::MayHaveSideEffects() const {
	return HasSideEffects(opcode);
}

bool Inst::HasUses() const {
	return !uses.empty();
}

size_t Inst::UseCount() const {
	return uses.size();
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
