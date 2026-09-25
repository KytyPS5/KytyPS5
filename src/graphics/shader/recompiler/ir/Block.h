#pragma once

#include "graphics/shader/recompiler/ir/Reg.h"
#include "graphics/shader/recompiler/ir/Value.h"

#include <array>
#include <initializer_list>
#include <list>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// A standard list provides stable instruction addresses without adding an intrusive-container
// dependency.
class Block {
public:
	using InstructionList = std::list<Inst>;
	using iterator        = InstructionList::iterator;
	using const_iterator  = InstructionList::const_iterator;

	Block() = default;
	~Block();

	Block(const Block&)            = delete;
	Block& operator=(const Block&) = delete;
	Block(Block&&)                 = delete;
	Block& operator=(Block&&)      = delete;

	Inst&    AppendNewInst(ValueOpcode opcode, std::initializer_list<Value> args = {},
	                       uint64_t flags = 0);
	iterator PrependNewInst(iterator insertion_point, ValueOpcode opcode,
	                        std::initializer_list<Value> args = {}, uint64_t flags = 0);
	void     AddBranch(Block* block);

	[[nodiscard]] InstructionList&        Instructions();
	[[nodiscard]] const InstructionList&  Instructions() const;
	[[nodiscard]] std::span<Block* const> ImmPredecessors() const;
	[[nodiscard]] std::span<Block* const> ImmSuccessors() const;

	void               SsaSeal();
	[[nodiscard]] bool IsSsaSealed() const;

	iterator       begin();
	const_iterator begin() const;
	iterator       end();
	const_iterator end() const;
	bool           empty() const;

	std::array<Value, NumScalarRegs> ssa_sreg_values {};
	std::array<Value, NumScalarRegs> ssa_thread_bit_sreg_values {};
	std::array<Value, NumScalarRegs> ssa_sreg_mask_tags {};
	std::array<Value, NumVectorRegs> ssa_vreg_values {};

private:
	InstructionList     instructions;
	std::vector<Block*> predecessors;
	std::vector<Block*> successors;
	bool                ssa_sealed = false;
};

using BlockList = std::vector<Block*>;

} // namespace Libs::Graphics::ShaderRecompiler::IR
