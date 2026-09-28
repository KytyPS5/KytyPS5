#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

void RemoveIdentities(const BlockList& blocks) {
	for (auto* block: blocks) {
		auto& instructions = block->Instructions();
		for (auto inst = instructions.begin(); inst != instructions.end();) {
			if (inst->GetOpcode() != ValueOpcode::Identity) {
				inst++;
				continue;
			}
			const auto replacement = inst->Arg(0);
			inst->ReplaceUsesWith(replacement, false);
			inst = instructions.erase(inst);
		}
	}
}

static bool RemoveDeadPhiWebs(const BlockList& blocks) {
	std::unordered_set<Inst*> phis;
	std::unordered_set<Inst*> live;
	std::vector<Inst*>        stack;
	for (auto* block: blocks) {
		for (auto& inst: block->Instructions()) {
			if (inst.GetOpcode() == ValueOpcode::Phi) phis.insert(&inst);
		}
	}
	for (auto* phi: phis) {
		for (const auto& use: phi->Uses()) {
			if (use.user != nullptr && use.user->GetOpcode() != ValueOpcode::Phi) {
				if (live.insert(phi).second) stack.push_back(phi);
				break;
			}
		}
	}
	while (!stack.empty()) {
		auto* phi = stack.back();
		stack.pop_back();
		for (size_t index = 0; index < phi->NumArgs(); ++index) {
			auto* dependency = phi->Arg(index).Resolve().TryInstruction();
			if (dependency != nullptr && phis.contains(dependency) &&
			    live.insert(dependency).second) {
				stack.push_back(dependency);
			}
		}
	}
	if (live.size() == phis.size()) return false;
	for (auto* phi: phis) {
		if (!live.contains(phi)) phi->Invalidate();
	}
	for (auto* block: blocks) {
		auto& instructions = block->Instructions();
		for (auto inst = instructions.begin(); inst != instructions.end();) {
			if (inst->GetOpcode() == ValueOpcode::Phi && !live.contains(&*inst)) {
				inst = instructions.erase(inst);
			} else {
				++inst;
			}
		}
	}
	return true;
}

void EliminateDeadCode(const BlockList& blocks) {
	bool changed;
	do {
		do {
			changed = false;
			for (auto block = blocks.rbegin(); block != blocks.rend(); block++) {
				auto& instructions = (*block)->Instructions();
				auto  inst         = instructions.end();
				while (inst != instructions.begin()) {
					--inst;
					if (inst->HasUses() || inst->MayHaveSideEffects()) {
						continue;
					}
					inst->Invalidate();
					inst    = instructions.erase(inst);
					changed = true;
				}
			}
		} while (changed);
	} while (RemoveDeadPhiWebs(blocks));
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
