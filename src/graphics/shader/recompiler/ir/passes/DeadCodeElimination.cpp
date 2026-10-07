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

void EliminateDeadCode(const BlockList& blocks) {
	std::vector<Inst*> pending;
	for (auto* block: blocks) {
		for (auto& inst: *block) {
			// Retained resource-planning instructions live outside the block list.
			inst.live = inst.MayHaveSideEffects() || std::ranges::any_of(inst.Uses(),
			    [](const Use& use) { return use.user != nullptr && use.user->Parent() == nullptr; });
			if (inst.live) pending.push_back(&inst);
		}
	}
	while (!pending.empty()) {
		const auto* inst = pending.back();
		pending.pop_back();
		for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
			auto* input = inst->Arg(arg).TryInstruction();
			if (input != nullptr && input->Parent() != nullptr && !input->live) {
				input->live = true;
				pending.push_back(input);
			}
		}
	}
	// Unlink every dead operand before freeing nodes in a dead Phi cycle.
	for (auto* block: blocks)
		for (auto& inst: *block)
			if (!inst.live) inst.Invalidate();
	for (auto* block: blocks)
		std::erase_if(block->Instructions(), [](const Inst& inst) { return !inst.live; });
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
