#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Research: SSA construction leaves webs of phis that only ever carry one value -- a register
// kept live around loops and branches that never write it -- and single-phi trivial-phi removal
// cannot see through a cycle of them. Resource tracking then finds a phi where a descriptor read
// should be. A set of phis closed under its phi operands whose other operands are all one value
// X evaluates to X everywhere.
bool RemoveRedundantPhiWebs(const BlockList& blocks) {
	constexpr size_t MaxWeb  = 4096;
	bool             changed = false;
	for (auto* block: blocks) {
		for (auto& inst: block->Instructions()) {
			if (inst.GetOpcode() != ValueOpcode::Phi || !inst.HasUses()) {
				continue;
			}
			std::vector<Inst*>        web;
			std::unordered_set<Inst*> seen;
			std::vector<Inst*>        pending {&inst};
			Value                     external;
			bool                      single = true;
			while (!pending.empty() && single) {
				auto* phi = pending.back();
				pending.pop_back();
				if (!seen.insert(phi).second) {
					continue;
				}
				// Branch conditions (U1) are block data, not instruction uses: a phi feeding one
				// cannot be replaced through its uses.
				if (phi->GetType() == Type::U1) {
					single = false;
					break;
				}
				web.push_back(phi);
				for (size_t i = 0; i < phi->NumArgs() && single; i++) {
					const auto value = phi->Arg(i).Resolve();
					auto*      def   = value.TryInstruction();
					if (def != nullptr && def->GetOpcode() == ValueOpcode::Phi) {
						pending.push_back(def);
					} else if (external.IsEmpty()) {
						external = value;
					} else if (!(external == value)) {
						single = false;
					}
				}
				if (web.size() > MaxWeb) {
					single = false;
				}
			}
			if (!single || external.IsEmpty()) {
				continue;
			}
			for (auto* phi: web) {
				phi->ReplaceUsesWith(external);
			}
			changed = true;
		}
	}
	return changed;
}

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
