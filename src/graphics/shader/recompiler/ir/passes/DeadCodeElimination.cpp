#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <algorithm>
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
	std::vector<Inst*> dead;
	std::vector<Inst*> web;
	std::vector<Inst*> stack;
	for (auto* block: blocks) {
		for (auto& inst: block->Instructions()) {
			if (inst.GetOpcode() != ValueOpcode::Phi ||
			    std::ranges::find(dead, &inst) != dead.end()) {
				continue;
			}
			web.clear();
			stack.clear();
			stack.push_back(&inst);
			bool external = false;
			while (!stack.empty() && !external) {
				auto* current = stack.back();
				stack.pop_back();
				if (std::ranges::find(web, current) != web.end()) {
					continue;
				}
				web.push_back(current);
				for (const auto& use: current->Uses()) {
					if (use.user->GetOpcode() != ValueOpcode::Phi) {
						external = true;
						break;
					}
					stack.push_back(use.user);
				}
			}
			if (external) {
				continue;
			}
			dead.insert(dead.end(), web.begin(), web.end());
		}
	}
	if (dead.empty()) {
		return false;
	}
	for (auto* inst: dead) {
		inst->Invalidate();
	}
	for (auto* block: blocks) {
		auto& instructions = block->Instructions();
		for (auto inst = instructions.begin(); inst != instructions.end();) {
			if (std::ranges::find(dead, &*inst) != dead.end()) {
				inst = instructions.erase(inst);
			} else {
				inst++;
			}
		}
	}
	return true;
}

void EliminateDeadCode(const BlockList& blocks) {
	do {
		bool changed;
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
