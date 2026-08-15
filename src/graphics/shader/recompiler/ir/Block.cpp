#include "graphics/shader/recompiler/ir/Block.h"

#include <algorithm>
#include <limits>

namespace Libs::Graphics::ShaderRecompiler::IR {

Block::~Block() {
	// Instructions own reverse-use links. Unlink every argument while all definitions are still
	// alive, then let the list destroy the detached instructions.
	for (auto& inst: instructions) {
		inst.Invalidate();
	}
}

Inst& Block::AppendNewInst(ValueOpcode opcode, std::initializer_list<Value> args, uint64_t flags) {
	return *PrependNewInst(end(), opcode, args, flags);
}

Block::iterator Block::PrependNewInst(iterator insertion_point, ValueOpcode opcode,
                                      std::initializer_list<Value> args, uint64_t flags) {
	const auto expected = NumArgsOf(opcode);
	EXIT_IF(expected != std::numeric_limits<size_t>::max() && expected != args.size());
	const auto inst = instructions.emplace(insertion_point, opcode, flags);
	inst->SetParent(this);
	size_t index = 0;
	for (const auto arg: args) {
		inst->SetArg(index++, arg);
	}
	return inst;
}

void Block::AddBranch(Block* block) {
	EXIT_IF(block == nullptr || std::ranges::find(successors, block) != successors.end());
	EXIT_IF(std::ranges::find(block->predecessors, this) != block->predecessors.end());
	successors.push_back(block);
	block->predecessors.push_back(this);
}

Block::InstructionList& Block::Instructions() {
	return instructions;
}

const Block::InstructionList& Block::Instructions() const {
	return instructions;
}

std::span<Block* const> Block::ImmPredecessors() const {
	return predecessors;
}

std::span<Block* const> Block::ImmSuccessors() const {
	return successors;
}

void Block::SsaSeal() {
	ssa_sealed = true;
}

bool Block::IsSsaSealed() const {
	return ssa_sealed;
}

Block::iterator Block::begin() {
	return instructions.begin();
}

Block::const_iterator Block::begin() const {
	return instructions.begin();
}

Block::iterator Block::end() {
	return instructions.end();
}

Block::const_iterator Block::end() const {
	return instructions.end();
}

bool Block::empty() const {
	return instructions.empty();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
