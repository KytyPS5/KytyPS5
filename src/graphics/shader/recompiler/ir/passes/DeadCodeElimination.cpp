#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

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

void EliminateDeadCode(const BlockList& blocks) {
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
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
