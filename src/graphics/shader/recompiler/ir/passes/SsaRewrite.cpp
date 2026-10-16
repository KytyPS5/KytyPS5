#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct SccTag {
	auto operator<=>(const SccTag&) const = default;
};
struct ThreadBitScalarReg {
	ScalarReg reg {};
	auto      operator<=>(const ThreadBitScalarReg&) const = default;
};
struct ScalarMaskTag {
	ScalarReg reg {};
	auto      operator<=>(const ScalarMaskTag&) const = default;
};
struct ExecTag {
	auto operator<=>(const ExecTag&) const = default;
};
struct ExecLoTag {
	auto operator<=>(const ExecLoTag&) const = default;
};
struct ExecHiTag {
	auto operator<=>(const ExecHiTag&) const = default;
};
struct VccTag {
	auto operator<=>(const VccTag&) const = default;
};
struct VccLoTag {
	auto operator<=>(const VccLoTag&) const = default;
};
struct VccHiTag {
	auto operator<=>(const VccHiTag&) const = default;
};
struct M0Tag {
	auto operator<=>(const M0Tag&) const = default;
};
struct GotoVariable {
	uint32_t index                                  = 0;
	auto     operator<=>(const GotoVariable&) const = default;
};

using Variable =
    std::variant<ScalarReg, ThreadBitScalarReg, ScalarMaskTag, VectorReg, GotoVariable, SccTag,
                 ExecTag, ExecLoTag, ExecHiTag, VccTag, VccLoTag, VccHiTag, M0Tag>;
using ValueMap = std::unordered_map<Block*, Value>;

// Routing variables are Boolean. Dense block indices avoid a hash node allocation for
// every (routing variable, block) pair. Pages also keep sparsely used variables small.
struct GotoValueMap {
	static constexpr uint32_t PageValues = 64u;
	std::unordered_map<uint32_t, std::array<uintptr_t, PageValues>> pages;
	Value Get(uint32_t index) const {
		const auto page = pages.find(index / PageValues);
		if (page == pages.end()) return {};
		const auto value = page->second[index % PageValues];
		if (value == 0u) return {};
		if (value <= 2u) return Value(value == 2u);
		return Value(reinterpret_cast<Inst*>(value));
	}
	void Set(uint32_t index, Value value) {
		EXIT_IF(value.GetType() != Type::U1);
		pages[index / PageValues][index % PageValues] = value.IsImmediate() ? (value.U1() ? 2u : 1u)
		                                                 : reinterpret_cast<uintptr_t>(value.Instruction());
	}
};
struct DefTable {
	explicit DefTable(const BlockList& blocks) {
		for (uint32_t i = 0; i < blocks.size(); ++i) block_indices.emplace(blocks[i], i);
	}
	std::unordered_map<Block*, uint32_t> block_indices;
	const Value& Get(Block* block, ScalarReg reg) {
		if (RegIndex(reg) >= NumScalarRegs) {
			EXIT("SSA scalar read index is out of range: %u", RegIndex(reg));
		}
		return block->ssa_sreg_values[RegIndex(reg)];
	}
	void Set(Block* block, ScalarReg reg, Value value) {
		if (RegIndex(reg) >= NumScalarRegs) {
			EXIT("SSA scalar write index is out of range: %u", RegIndex(reg));
		}
		block->ssa_sreg_values[RegIndex(reg)] = value;
	}
	const Value& Get(Block* block, ThreadBitScalarReg value) {
		EXIT_IF(RegIndex(value.reg) >= NumScalarRegs);
		return block->ssa_thread_bit_sreg_values[RegIndex(value.reg)];
	}
	void Set(Block* block, ThreadBitScalarReg value, Value definition) {
		EXIT_IF(RegIndex(value.reg) >= NumScalarRegs);
		block->ssa_thread_bit_sreg_values[RegIndex(value.reg)] = definition;
	}
	const Value& Get(Block* block, ScalarMaskTag value) {
		EXIT_IF(RegIndex(value.reg) >= NumScalarRegs);
		return block->ssa_sreg_mask_tags[RegIndex(value.reg)];
	}
	void Set(Block* block, ScalarMaskTag value, Value definition) {
		EXIT_IF(RegIndex(value.reg) >= NumScalarRegs);
		block->ssa_sreg_mask_tags[RegIndex(value.reg)] = definition;
	}

	const Value& Get(Block* block, VectorReg reg) {
		EXIT_IF(RegIndex(reg) >= NumVectorRegs);
		return block->ssa_vreg_values[RegIndex(reg)];
	}
	void Set(Block* block, VectorReg reg, Value value) {
		EXIT_IF(RegIndex(reg) >= NumVectorRegs);
		block->ssa_vreg_values[RegIndex(reg)] = value;
	}
	Value Get(Block* block, GotoVariable value) {
		return goto_variables[value.index].Get(block_indices.at(block));
	}
	void Set(Block* block, GotoVariable value, Value definition) {
		goto_variables[value.index].Set(block_indices.at(block), definition);
	}

	const Value& Get(Block* block, SccTag) { return scc[block]; }
	void         Set(Block* block, SccTag, Value value) { scc[block] = value; }
	const Value& Get(Block* block, ExecTag) { return exec[block]; }
	void         Set(Block* block, ExecTag, Value value) { exec[block] = value; }
	const Value& Get(Block* block, ExecLoTag) { return exec_lo[block]; }
	void         Set(Block* block, ExecLoTag, Value value) { exec_lo[block] = value; }
	const Value& Get(Block* block, ExecHiTag) { return exec_hi[block]; }
	void         Set(Block* block, ExecHiTag, Value value) { exec_hi[block] = value; }
	const Value& Get(Block* block, VccTag) { return vcc[block]; }
	void         Set(Block* block, VccTag, Value value) { vcc[block] = value; }
	const Value& Get(Block* block, VccLoTag) { return vcc_lo[block]; }
	void         Set(Block* block, VccLoTag, Value value) { vcc_lo[block] = value; }
	const Value& Get(Block* block, VccHiTag) { return vcc_hi[block]; }
	void         Set(Block* block, VccHiTag, Value value) { vcc_hi[block] = value; }
	const Value& Get(Block* block, M0Tag) { return m0[block]; }
	void         Set(Block* block, M0Tag, Value value) { m0[block] = value; }

	ValueMap                               scc;
	ValueMap                               exec;
	ValueMap                               exec_lo;
	ValueMap                               exec_hi;
	ValueMap                               vcc;
	ValueMap                               vcc_lo;
	ValueMap                               vcc_hi;
	ValueMap                               m0;
	std::unordered_map<uint32_t, GotoValueMap> goto_variables;
};

Value InitialValue(ScalarReg) {
	return Value(0u);
}
Value InitialValue(ThreadBitScalarReg) {
	return Value(false);
}
Value InitialValue(ScalarMaskTag) {
	return Value(false);
}
Value InitialValue(VectorReg) {
	return Value(0u);
}
Value InitialValue(GotoVariable) {
	return Value(false);
}
Value InitialValue(SccTag) {
	return Value(false);
}
Value InitialValue(ExecTag) {
	return Value(false);
}
Value InitialValue(ExecLoTag) {
	return Value(0u);
}
Value InitialValue(ExecHiTag) {
	return Value(0u);
}
Value InitialValue(VccTag) {
	return Value(false);
}
Value InitialValue(VccLoTag) {
	return Value(0u);
}
Value InitialValue(VccHiTag) {
	return Value(0u);
}
Value InitialValue(M0Tag) {
	return Value(0u);
}

enum class ReadStep { Start, SetValue, PushPhiArgument };

struct ReadState {
	Block*   block = nullptr;
	Value    result;
	Inst*    phi  = nullptr;
	size_t   pred = 0;
	ReadStep step = ReadStep::Start;
};

class Pass {
public:
	explicit Pass(const BlockList& blocks): definitions(blocks) {}
	template <typename T>
	void Write(T variable, Block* block, Value value) {
		definitions.Set(block, variable, value);
	}

	template <typename T>
	Value Read(T variable, Block* root) {
		stack.assign({{}, {.block = root}});
		const auto prepare_phi = [&]() {
			auto&      state        = stack.back();
			const auto predecessors = state.block->ImmPredecessors();
			if (state.pred == predecessors.size()) {
				const auto result = TryRemoveTrivialPhi(*state.phi, InitialValue(variable));
				const auto block  = state.block;
				stack.pop_back();
				stack.back().result = result;
				Write(variable, block, result);
				return;
			}
			state.step = ReadStep::PushPhiArgument;
			stack.push_back({.block = predecessors[state.pred]});
		};

		do {
			auto& state = stack.back();
			auto* block = state.block;
			switch (state.step) {
				case ReadStep::Start: {
					if (const auto& def = definitions.Get(block, variable); !def.IsEmpty()) {
						state.result = def;
					} else if (!block->IsSsaSealed()) {
						auto& phi = *block->PrependNewInst(block->begin(), ValueOpcode::Phi);
						phi.SetFlags(InitialValue(variable).GetType());
						incomplete_phis[block][Variable(variable)] = &phi;
						state.result                               = Value(&phi);
					} else if (const auto predecessors = block->ImmPredecessors();
					           predecessors.size() == 1) {
						state.step = ReadStep::SetValue;
						stack.push_back({.block = predecessors.front()});
						break;
					} else {
						auto& phi = *block->PrependNewInst(block->begin(), ValueOpcode::Phi);
						phi.SetFlags(InitialValue(variable).GetType());
						Write(variable, block, Value(&phi));
						state.phi = &phi;
						prepare_phi();
						break;
					}
					[[fallthrough]];
				}
				case ReadStep::SetValue: {
					const auto result = state.result;
					Write(variable, block, result);
					stack.pop_back();
					stack.back().result = result;
					break;
				}
				case ReadStep::PushPhiArgument: {
					const auto predecessors = block->ImmPredecessors();
					state.phi->AddPhiOperand(predecessors[state.pred], state.result);
					state.pred++;
					prepare_phi();
					break;
				}
			}
		} while (stack.size() > 1);
		return stack.back().result;
	}

	void Seal(Block* block) {
		if (const auto found = incomplete_phis.find(block); found != incomplete_phis.end()) {
			for (auto& [variable, phi]: found->second) {
				std::visit([&](auto value) { AddPhiOperands(value, *phi, block); }, variable);
			}
		}
		block->SsaSeal();
	}

private:
	template <typename T>
	Value AddPhiOperands(T variable, Inst& phi, Block* block) {
		for (auto* predecessor: block->ImmPredecessors()) {
			phi.AddPhiOperand(predecessor, Read(variable, predecessor));
		}
		return TryRemoveTrivialPhi(phi, InitialValue(variable));
	}

	Value TryRemoveTrivialPhi(Inst& phi, Value initial_value) {
		std::vector<Inst*> pending {&phi};
		std::unordered_set<Inst*> queued {&phi};
		while (!pending.empty()) {
			auto* current = pending.back();
			pending.pop_back();
			queued.erase(current);
			if (current->GetOpcode() != ValueOpcode::Phi) continue;
			Value same;
			bool trivial = true;
			for (size_t index = 0; index < current->NumArgs(); ++index) {
				const auto operand = current->Arg(index).Resolve();
				if (operand == same.Resolve() || operand == Value(current)) continue;
				if (!same.IsEmpty()) { trivial = false; break; }
				same = operand;
			}
			if (!trivial) continue;
			if (same.IsEmpty()) same = initial_value;
			// Only one use list stays live; a long phi chain no longer consumes the call stack.
			const auto users = current->Uses();
			current->ReplaceUsesWith(same);
			for (const auto& use: users) {
				if (use.user->GetOpcode() == ValueOpcode::Phi && queued.insert(use.user).second)
					pending.push_back(use.user);
			}
		}
		return Value(&phi).Resolve();
	}

	std::unordered_map<Block*, std::map<Variable, Inst*>> incomplete_phis;
	DefTable                                              definitions;
	std::vector<ReadState>                               stack;
};

void VisitInstruction(Pass& pass, Block* block, Inst& inst) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::SetScalarRegister:
			pass.Write(inst.Arg(0).ScalarRegister(), block, inst.Arg(1));
			break;
		case ValueOpcode::SetThreadBitScalarRegister:
			pass.Write(ThreadBitScalarReg {inst.Arg(0).ScalarRegister()}, block, inst.Arg(1));
			break;
		case ValueOpcode::SetScalarMaskTag:
			pass.Write(ScalarMaskTag {inst.Arg(0).ScalarRegister()}, block, inst.Arg(1));
			break;
		case ValueOpcode::SetVectorRegister:
			pass.Write(inst.Arg(0).VectorRegister(), block, inst.Arg(1));
			break;
		case ValueOpcode::SetGotoVariable:
			pass.Write(GotoVariable {inst.Arg(0).U32()}, block, inst.Arg(1));
			break;
		case ValueOpcode::SetScc: pass.Write(SccTag {}, block, inst.Arg(0)); break;
		case ValueOpcode::SetExec: pass.Write(ExecTag {}, block, inst.Arg(0)); break;
		case ValueOpcode::SetExecLo: pass.Write(ExecLoTag {}, block, inst.Arg(0)); break;
		case ValueOpcode::SetExecHi: pass.Write(ExecHiTag {}, block, inst.Arg(0)); break;
		case ValueOpcode::SetVcc: pass.Write(VccTag {}, block, inst.Arg(0)); break;
		case ValueOpcode::SetVccLo: pass.Write(VccLoTag {}, block, inst.Arg(0)); break;
		case ValueOpcode::SetVccHi: pass.Write(VccHiTag {}, block, inst.Arg(0)); break;
		case ValueOpcode::SetM0: pass.Write(M0Tag {}, block, inst.Arg(0)); break;
		case ValueOpcode::GetScalarRegister:
			inst.ReplaceUsesWith(pass.Read(inst.Arg(0).ScalarRegister(), block));
			break;
		case ValueOpcode::GetThreadBitScalarRegister:
			inst.ReplaceUsesWith(
			    pass.Read(ThreadBitScalarReg {inst.Arg(0).ScalarRegister()}, block));
			break;
		case ValueOpcode::GetScalarMaskTag:
			inst.ReplaceUsesWith(pass.Read(ScalarMaskTag {inst.Arg(0).ScalarRegister()}, block));
			break;
		case ValueOpcode::GetVectorRegister:
			inst.ReplaceUsesWith(pass.Read(inst.Arg(0).VectorRegister(), block));
			break;
		case ValueOpcode::GetGotoVariable:
			inst.ReplaceUsesWith(pass.Read(GotoVariable {inst.Arg(0).U32()}, block));
			break;
		case ValueOpcode::GetScc: inst.ReplaceUsesWith(pass.Read(SccTag {}, block)); break;
		case ValueOpcode::GetExec: inst.ReplaceUsesWith(pass.Read(ExecTag {}, block)); break;
		case ValueOpcode::GetExecLo: inst.ReplaceUsesWith(pass.Read(ExecLoTag {}, block)); break;
		case ValueOpcode::GetExecHi: inst.ReplaceUsesWith(pass.Read(ExecHiTag {}, block)); break;
		case ValueOpcode::GetVcc: inst.ReplaceUsesWith(pass.Read(VccTag {}, block)); break;
		case ValueOpcode::GetVccLo: inst.ReplaceUsesWith(pass.Read(VccLoTag {}, block)); break;
		case ValueOpcode::GetVccHi: inst.ReplaceUsesWith(pass.Read(VccHiTag {}, block)); break;
		case ValueOpcode::GetM0: inst.ReplaceUsesWith(pass.Read(M0Tag {}, block)); break;
		default: break;
	}
}

} // namespace

void RewriteToSsa(const BlockList& blocks) {
	Pass pass(blocks);
	// Visit predecessors before successors wherever the graph permits it. Sealing an
	// acyclic block as soon as its incoming definitions exist avoids placeholder phis
	// for every routing variable in every inlined call arm.
	BlockList order;
	std::unordered_set<Block*> seen;
	struct Cursor { Block* block; size_t successor; };
	std::vector<Cursor> pending;
	for (auto* root: blocks) {
		if (!seen.insert(root).second) continue;
		const auto first = order.size();
		pending.push_back({root, 0u});
		while (!pending.empty()) {
			auto& cursor = pending.back();
			const auto successors = cursor.block->ImmSuccessors();
			if (cursor.successor < successors.size()) {
				auto* next = successors[cursor.successor++];
				if (seen.insert(next).second) pending.push_back({next, 0u});
			} else {
				order.push_back(cursor.block);
				pending.pop_back();
			}
		}
		std::reverse(order.begin() + first, order.end());
	}
	std::unordered_set<Block*> processed;
	const auto ready = [&](Block* block) {
		return std::ranges::all_of(block->ImmPredecessors(),
		                          [&](Block* predecessor) { return processed.contains(predecessor); });
	};
	for (auto* block: order) {
		if (!block->IsSsaSealed() && ready(block)) pass.Seal(block);
		for (auto& inst: *block) VisitInstruction(pass, block, inst);
		processed.insert(block);
		for (auto* successor: block->ImmSuccessors()) {
			if (processed.contains(successor) && !successor->IsSsaSealed() && ready(successor))
				pass.Seal(successor);
		}
	}
	for (auto* block: blocks) {
		if (!block->IsSsaSealed()) pass.Seal(block);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
