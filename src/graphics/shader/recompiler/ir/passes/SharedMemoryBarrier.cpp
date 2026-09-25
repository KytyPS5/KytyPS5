#include "graphics/shader/recompiler/ir/passes/SharedMemoryBarrier.h"

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

enum class LdsPhase { None, Read, Write };

LdsPhase GetLdsPhase(const Program& program, const Inst& inst) {
	const auto access = SharedAccessOf(inst.GetOpcode());
	if (access != SharedAccess::Read && access != SharedAccess::Write) {
		return LdsPhase::None;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= program.memory_info.size() ||
	    program.memory_info[index].kind != ResourceKind::Lds) {
		return LdsPhase::None;
	}
	return access == SharedAccess::Read ? LdsPhase::Read : LdsPhase::Write;
}

bool DependsOnInvocation(Value value) {
	std::queue<Value>         queue;
	std::unordered_set<Inst*> visited;
	queue.push(value);
	while (!queue.empty()) {
		value = queue.front().Resolve();
		queue.pop();
		auto* inst = value.TryInstruction();
		if (inst == nullptr || !visited.insert(inst).second) {
			continue;
		}
		if (inst->GetOpcode() == ValueOpcode::LaneId) {
			return true;
		}
		if (inst->GetOpcode() == ValueOpcode::GetBuiltin) {
			const auto kind = static_cast<StageInputKind>(inst->Arg(0).U32());
			if (kind == StageInputKind::LocalInvocationId ||
			    kind == StageInputKind::LocalInvocationIndex ||
			    kind == StageInputKind::GlobalInvocationId) {
				return true;
			}
		}
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			queue.push(inst->Arg(index));
		}
	}
	return false;
}

uint32_t InsertBarrier(Block& block, Block::iterator position) {
	block.PrependNewInst(position, ValueOpcode::Barrier);
	return 1;
}

uint32_t InsertBlockBarriers(Program& program, Block& block) {
	uint32_t inserted = 0;
	LdsPhase phase    = LdsPhase::None;
	for (auto inst = block.begin(); inst != block.end(); ++inst) {
		if (inst->GetOpcode() == ValueOpcode::Barrier) {
			phase = LdsPhase::None;
			continue;
		}
		const auto next = GetLdsPhase(program, *inst);
		if (next == LdsPhase::None) {
			continue;
		}
		if (phase != LdsPhase::None && phase != next) {
			inserted += InsertBarrier(block, inst);
		}
		phase = next;
	}
	if (phase != LdsPhase::None) {
		block.AppendNewInst(ValueOpcode::Barrier);
		inserted++;
	}
	return inserted;
}

uint32_t InsertMergeBarrier(Block& block) {
	auto position = std::ranges::find_if(
	    block, [](const Inst& inst) { return inst.GetOpcode() != ValueOpcode::Phi; });
	if (position != block.end() && position->GetOpcode() == ValueOpcode::Barrier) {
		return 0;
	}
	return InsertBarrier(block, position);
}

} // namespace

SharedMemoryBarrierStats InsertSharedMemoryBarriers(Program& program, uint32_t wave_size,
	                                                 const ShaderComputeInputInfo& compute_info) {
	SharedMemoryBarrierStats stats;
	const auto threadgroup_size = compute_info.threads_num[0] * compute_info.threads_num[1] *
	                              compute_info.threads_num[2];
	if (wave_size != 64u || !compute_info.needs_lds_barriers ||
	    compute_info.lds_size_dwords == 0u || threadgroup_size != 64u) {
		return stats;
	}
	if (program.blocks.size() != program.block_info.size()) {
		return stats;
	}
	for (const auto& info: program.block_info) {
		if (info.terminator.kind == CFG::TerminatorKind::ConditionalBranch &&
		    info.terminator.merge_block == UINT32_MAX && DependsOnInvocation(info.condition)) {
			return stats;
		}
	}

	std::unordered_map<uint32_t, Block*> blocks;
	for (size_t index = 0; index < program.blocks.size(); index++) {
		blocks.emplace(program.block_info[index].id, program.blocks[index]);
	}

	uint32_t                               divergence_depth = 0;
	std::unordered_map<uint32_t, uint32_t> divergence_ends;
	std::unordered_set<uint32_t>           synchronized_merges;
	for (size_t index = 0; index < program.blocks.size(); index++) {
		const auto id = program.block_info[index].id;
		if (const auto end = divergence_ends.find(id); end != divergence_ends.end()) {
			EXIT_IF(end->second > divergence_depth);
			divergence_depth -= end->second;
		}

		if (divergence_depth == 0) {
			stats.inserted_barriers += InsertBlockBarriers(program, *program.blocks[index]);
		}

		const auto& info = program.block_info[index];
		const auto& term = info.terminator;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch || term.merge_block == UINT32_MAX ||
		    !DependsOnInvocation(info.condition)) {
			continue;
		}
		if (divergence_depth == 0 && synchronized_merges.insert(term.merge_block).second) {
			const auto merge = blocks.find(term.merge_block);
			if (merge != blocks.end()) {
				stats.inserted_barriers += InsertMergeBarrier(*merge->second);
			}
		}
		divergence_depth++;
		divergence_ends[term.merge_block]++;
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
