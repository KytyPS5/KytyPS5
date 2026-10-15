#include "graphics/shader/recompiler/ir/passes/PhiWebs.h"

#include "graphics/shader/recompiler/ir/Block.h"

#include <unordered_set>
#include <vector>

// A shader that keeps a descriptor dword in a register across a loop (and across the register
// spills around a call) ends up with a phi per join point, each fed by the others and by the
// original load. Phi-by-phi folding cannot see through that: no single phi has one operand. The
// whole web does, though: when every value entering it from outside is the same, every phi in
// it is that value. Resource tracking needs this to evaluate such a descriptor before the
// dispatch.

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

bool IsPhi(const Inst* inst) {
	return inst != nullptr && inst->GetOpcode() == ValueOpcode::Phi;
}

// Two reads of the same constant memory slot produce the same value wherever they are.
bool SameValue(Value a, Value b) {
	a = a.Resolve();
	b = b.Resolve();
	if (a == b) return true;
	const auto* x = a.TryInstruction();
	const auto* y = b.TryInstruction();
	if (x == nullptr || y == nullptr || x->GetOpcode() != ValueOpcode::ReadConst ||
	    y->GetOpcode() != ValueOpcode::ReadConst || x->NumArgs() != y->NumArgs()) {
		return false;
	}
	for (size_t index = 0; index < x->NumArgs(); index++) {
		if (x->Arg(index).Resolve() != y->Arg(index).Resolve()) return false;
	}
	return true;
}

} // namespace

uint32_t SimplifyPhiWebs(Program& program) {
	std::vector<Inst*> phis;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (IsPhi(&inst)) phis.push_back(&inst);
		}
	}

	std::unordered_set<const Inst*> visited;
	uint32_t                        replaced = 0;
	for (auto* start: phis) {
		if (!visited.insert(start).second) continue;
		std::vector<Inst*> web {start};
		// Each root needs its complete incoming closure. A previously scanned web
		// can share a phi without sharing all of this root's external values.
		std::unordered_set<const Inst*> in_web {start};
		std::vector<Value> leaves;
		for (size_t at = 0; at < web.size(); at++) {
			auto* phi = web[at];
			for (size_t index = 0; index < phi->NumArgs(); index++) {
				const auto value = phi->Arg(index).Resolve();
				auto*      inst  = const_cast<Inst*>(value.TryInstruction());
				if (IsPhi(inst)) {
					if (in_web.insert(inst).second) {
						web.push_back(inst);
						visited.insert(inst);
					}
				} else {
					leaves.push_back(value);
				}
			}
		}
		if (leaves.empty()) continue;
		bool same = true;
		for (size_t index = 1; index < leaves.size() && same; index++) {
			same = SameValue(leaves[0], leaves[index]);
		}
		if (!same) continue;

		// Constants are used as they are; a constant-memory read is hoisted into the entry block,
		// where it dominates every phi in the web.
		auto representative = leaves[0].Resolve();
		if (!representative.IsImmediate()) {
			const auto* source = representative.TryInstruction();
			if (source == nullptr || source->GetOpcode() != ValueOpcode::ReadConst ||
			    source->NumArgs() != 2u) {
				// Only constants and constant-memory reads are known to be placeable anywhere.
				continue;
			}
			auto* entry = program.blocks.front();
			// Place the clone right after the value it reads through (the SRT base), which has
			// to be in the entry block as well.
			auto where = entry->begin();
			if (const auto* base = source->Arg(0).Resolve().TryInstruction(); base != nullptr) {
				while (where != entry->end() && &*where != base) ++where;
				if (where == entry->end()) continue;
				++where;
			}
			const auto at = entry->PrependNewInst(where, ValueOpcode::ReadConst,
			                                      {source->Arg(0), source->Arg(1)},
			                                      source->Flags<uint64_t>());
			representative = Value(&*at);
		}
		for (auto* phi: web) {
			phi->ReplaceUsesWith(representative);
			replaced++;
		}
	}
	return replaced;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
