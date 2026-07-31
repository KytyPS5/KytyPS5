#include "graphics/shader/recompiler/ir/ReadLaneElimination.h"

#include "graphics/shader/recompiler/ir/SrtWalker.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

constexpr uint32_t FirstTemporaryScalarRegister = 128;

struct LaneKey {
	uint32_t reg  = 0;
	uint32_t lane = 0;

	auto operator<=>(const LaneKey&) const = default;
};

using LaneSet = std::set<LaneKey>;

bool PairDwordOpcode(Opcode op) {
	switch (op) {
		case Opcode::MoveU64:
		case Opcode::WqmB64:
		case Opcode::SaveexecB64:
		case Opcode::BitwiseAndU64:
		case Opcode::BitwiseAndNotU64:
		case Opcode::BitwiseOrU64:
		case Opcode::BitwiseOrNotU64:
		case Opcode::BitwiseXorU64:
		case Opcode::BitwiseNandU64:
		case Opcode::BitwiseNorU64:
		case Opcode::BitwiseXnorU64:
		case Opcode::BitwiseNotU64:
		case Opcode::BitFieldMaskU64:
		case Opcode::BitFieldExtractU64:
		case Opcode::BitReplicateB64B32:
		case Opcode::ShiftLeftLogicalU64:
		case Opcode::ShiftRightLogicalU64:
		case Opcode::SelectU64: return true;
		default: return false;
	}
}

bool ResolveLane(const Program& program, const Instruction& inst, uint32_t source_index,
                 uint32_t& lane) {
	if (source_index >= inst.src_count || (program.wave_size != 32 && program.wave_size != 64)) {
		return false;
	}
	const auto& selector = inst.src[source_index];
	if (selector.kind == OperandKind::ImmediateU32) {
		lane = selector.imm % program.wave_size;
		return true;
	}
	uint32_t folded = 0;
	if (!FoldScalarConstant(program.provenance, inst.scalar_sources[source_index], folded)) {
		return false;
	}
	lane = folded % program.wave_size;
	return true;
}

bool UniformWriteSource(const Instruction& inst) {
	if (inst.src_count == 0) {
		return false;
	}
	const auto& source = inst.src[0];
	if (source.kind == OperandKind::ImmediateU32 || source.kind == OperandKind::PcRelativeU32) {
		return true;
	}
	return source.kind == OperandKind::Register &&
	       (source.reg.file == RegisterFile::Scalar || source.reg.file == RegisterFile::Scc ||
	        source.reg.file == RegisterFile::M0);
}

bool WriteLaneKey(const Program& program, const Instruction& inst, LaneKey& key) {
	if (inst.op != Opcode::WriteLaneU32 || inst.dst.kind != OperandKind::Register ||
	    inst.dst.reg.file != RegisterFile::Vector || !UniformWriteSource(inst)) {
		return false;
	}
	uint32_t lane = 0;
	if (!ResolveLane(program, inst, 1, lane)) {
		return false;
	}
	key = {inst.dst.reg.index, lane};
	return true;
}

bool ReadLaneKey(const Program& program, const Instruction& inst, LaneKey& key) {
	if (inst.op != Opcode::ReadLaneU32 || inst.src_count < 2 ||
	    inst.src[0].kind != OperandKind::Register || inst.src[0].reg.file != RegisterFile::Vector) {
		return false;
	}
	uint32_t lane = 0;
	if (!ResolveLane(program, inst, 1, lane)) {
		return false;
	}
	key = {inst.src[0].reg.index, lane};
	return true;
}

void InvalidateRegister(LaneSet& valid, uint32_t reg) {
	const auto first = valid.lower_bound({reg, 0});
	const auto last  = valid.lower_bound({reg + 1u, 0});
	valid.erase(first, last);
}

void ApplyInstruction(const Program& program, const Instruction& inst, LaneSet& valid) {
	if (inst.op == Opcode::WriteLaneU32 && inst.dst.kind == OperandKind::Register &&
	    inst.dst.reg.file == RegisterFile::Vector) {
		LaneKey key;
		if (WriteLaneKey(program, inst, key)) {
			valid.insert(key);
			return;
		}
		uint32_t lane = 0;
		if (ResolveLane(program, inst, 1, lane)) {
			valid.erase({inst.dst.reg.index, lane});
		} else {
			InvalidateRegister(valid, inst.dst.reg.index);
		}
		return;
	}

	if (inst.op == Opcode::MoveRelDestU32 && inst.dst.kind == OperandKind::Register &&
	    inst.dst.reg.file == RegisterFile::Vector) {
		valid.clear();
		return;
	}

	if (inst.dst.kind == OperandKind::Register && inst.dst.reg.file == RegisterFile::Vector) {
		uint32_t dwords = std::max(inst.memory.data_dwords, 1u);
		if (PairDwordOpcode(inst.op) || inst.op == Opcode::UMadU64U32) {
			dwords = std::max(dwords, 2u);
		}
		for (uint32_t i = 0; i < dwords && inst.dst.reg.index <= UINT32_MAX - i; i++) {
			InvalidateRegister(valid, inst.dst.reg.index + i);
		}
	}
	if (inst.dst2.kind == OperandKind::Register && inst.dst2.reg.file == RegisterFile::Vector) {
		InvalidateRegister(valid, inst.dst2.reg.index);
	}
}

LaneSet TransferBlock(const Program& program, const BasicBlock& block, LaneSet state) {
	for (const auto& inst: block.instructions) {
		ApplyInstruction(program, inst, state);
	}
	return state;
}

LaneSet Intersect(const LaneSet& left, const LaneSet& right) {
	LaneSet result;
	std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
	                      std::inserter(result, result.end()));
	return result;
}

uint32_t NextTemporaryScalarRegister(const Program& program) {
	uint32_t   next     = FirstTemporaryScalarRegister;
	const auto consider = [&next](const Operand& operand) {
		if (operand.kind == OperandKind::Register && operand.reg.file == RegisterFile::Scalar &&
		    operand.reg.index >= next && operand.reg.index != UINT32_MAX) {
			next = operand.reg.index + 1u;
		}
	};
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			consider(inst.dst);
			consider(inst.dst2);
			for (uint32_t i = 0; i < inst.src_count; i++) {
				consider(inst.src[i]);
			}
		}
	}
	return next;
}

Operand ScalarRegisterOperand(uint32_t reg) {
	Operand operand;
	operand.kind      = OperandKind::Register;
	operand.reg.file  = RegisterFile::Scalar;
	operand.reg.index = reg;
	return operand;
}

Instruction ShadowWrite(const Instruction& write, uint32_t temporary) {
	Instruction shadow;
	shadow.pc        = write.pc;
	shadow.op        = Opcode::MoveU32;
	shadow.dst       = ScalarRegisterOperand(temporary);
	shadow.src[0]    = write.src[0];
	shadow.src_count = 1;
	return shadow;
}

Instruction ShadowRead(const Instruction& read, uint32_t temporary) {
	Instruction rewritten;
	rewritten.pc        = read.pc;
	rewritten.op        = Opcode::MoveU32;
	rewritten.dst       = read.dst;
	rewritten.src[0]    = ScalarRegisterOperand(temporary);
	rewritten.src_count = 1;
	return rewritten;
}

} // namespace

ReadLaneEliminationStats EliminateReadLane(Program& program) {
	ReadLaneEliminationStats stats;
	if (program.blocks.empty() || (program.wave_size != 32 && program.wave_size != 64)) {
		return stats;
	}

	LaneSet universe;
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			LaneKey key;
			if (WriteLaneKey(program, inst, key)) {
				universe.insert(key);
			}
		}
	}
	if (universe.empty()) {
		return stats;
	}

	const size_t         block_count = program.blocks.size();
	std::vector<LaneSet> entry(block_count, universe);
	std::vector<LaneSet> exit(block_count, universe);
	entry[0].clear();
	for (size_t block = 0; block < block_count; block++) {
		exit[block] = TransferBlock(program, program.blocks[block], entry[block]);
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (size_t block_index = 0; block_index < block_count; block_index++) {
			LaneSet     next_entry;
			const auto& block = program.blocks[block_index];
			if (block_index != 0 && !block.predecessors.empty()) {
				next_entry = universe;
				for (const auto predecessor: block.predecessors) {
					if (predecessor >= block_count) {
						next_entry.clear();
						break;
					}
					next_entry = Intersect(next_entry, exit[predecessor]);
				}
			}
			auto next_exit = TransferBlock(program, block, next_entry);
			if (next_entry != entry[block_index] || next_exit != exit[block_index]) {
				entry[block_index] = std::move(next_entry);
				exit[block_index]  = std::move(next_exit);
				changed            = true;
			}
		}
	}

	LaneSet forwarded;
	for (size_t block_index = 0; block_index < block_count; block_index++) {
		auto state = entry[block_index];
		for (const auto& inst: program.blocks[block_index].instructions) {
			LaneKey key;
			if (ReadLaneKey(program, inst, key) && state.contains(key)) {
				forwarded.insert(key);
			}
			ApplyInstruction(program, inst, state);
		}
	}
	if (forwarded.empty()) {
		return stats;
	}

	std::map<LaneKey, uint32_t> temporaries;
	auto                        next_temporary = NextTemporaryScalarRegister(program);
	for (const auto& key: forwarded) {
		if (next_temporary == UINT32_MAX) {
			return {};
		}
		temporaries.emplace(key, next_temporary++);
	}

	for (size_t block_index = 0; block_index < block_count; block_index++) {
		const auto original  = std::move(program.blocks[block_index].instructions);
		auto&      rewritten = program.blocks[block_index].instructions;
		rewritten.clear();
		rewritten.reserve(original.size() + temporaries.size());
		auto state = entry[block_index];
		for (const auto& inst: original) {
			LaneKey read_key;
			if (ReadLaneKey(program, inst, read_key) && state.contains(read_key)) {
				const auto temporary = temporaries.find(read_key);
				if (temporary != temporaries.end()) {
					rewritten.push_back(ShadowRead(inst, temporary->second));
					stats.rewritten_reads++;
					ApplyInstruction(program, inst, state);
					continue;
				}
			}

			rewritten.push_back(inst);
			LaneKey write_key;
			if (WriteLaneKey(program, inst, write_key)) {
				const auto temporary = temporaries.find(write_key);
				if (temporary != temporaries.end()) {
					rewritten.push_back(ShadowWrite(inst, temporary->second));
					stats.shadow_writes++;
				}
			}
			ApplyInstruction(program, inst, state);
		}
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
