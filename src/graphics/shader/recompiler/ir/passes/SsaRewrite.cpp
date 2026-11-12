#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <variant>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct ThreadBitScalarReg {
	ScalarReg reg {};
	auto      operator<=>(const ThreadBitScalarReg&) const = default;
};
struct ScalarMaskTag {
	ScalarReg reg {};
	auto      operator<=>(const ScalarMaskTag&) const = default;
};
// Indexes Block::ssa_flag_values.
enum class Flag : uint8_t { Scc, Exec, ExecLo, ExecHi, Vcc, VccLo, VccHi, M0, Count };
static_assert(std::tuple_size_v<decltype(Block::ssa_flag_values)> ==
              static_cast<size_t>(Flag::Count));
struct GotoVariable {
	uint32_t index                                  = 0;
	auto     operator<=>(const GotoVariable&) const = default;
};

using Variable =
    std::variant<ScalarReg, ThreadBitScalarReg, ScalarMaskTag, VectorReg, GotoVariable, Flag>;
using ValueMap = std::unordered_map<Block*, Value>;

struct DefTable {
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
	const Value& Get(Block* block, GotoVariable value) {
		return goto_variables[value.index][block];
	}
	void Set(Block* block, GotoVariable value, Value definition) {
		goto_variables[value.index][block] = definition;
	}

	const Value& Get(Block* block, Flag flag) {
		return block->ssa_flag_values[static_cast<size_t>(flag)];
	}
	void Set(Block* block, Flag flag, Value value) {
		block->ssa_flag_values[static_cast<size_t>(flag)] = value;
	}

	std::unordered_map<uint32_t, ValueMap> goto_variables;
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
Value InitialValue(Flag flag) {
	// SCC, EXEC and VCC are U1; the 32-bit halves and M0 are U32.
	const bool u1 = flag == Flag::Scc || flag == Flag::Exec || flag == Flag::Vcc;
	return u1 ? Value(false) : Value(0u);
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
		Value same;
		for (size_t index = 0; index < phi.NumArgs(); index++) {
			const auto operand = phi.Arg(index).Resolve();
			if (operand == same.Resolve() || operand == Value(&phi)) {
				continue;
			}
			if (!same.IsEmpty()) {
				return Value(&phi);
			}
			same = operand;
		}
		if (same.IsEmpty()) {
			same = initial_value;
		}
		const auto users = phi.Uses();
		phi.ReplaceUsesWith(same);
		for (const auto& use: users) {
			if (use.user->GetOpcode() == ValueOpcode::Phi) {
				TryRemoveTrivialPhi(*use.user, initial_value);
			}
		}
		return same;
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
		case ValueOpcode::SetScc: pass.Write(Flag::Scc, block, inst.Arg(0)); break;
		case ValueOpcode::SetExec: pass.Write(Flag::Exec, block, inst.Arg(0)); break;
		case ValueOpcode::SetExecLo: pass.Write(Flag::ExecLo, block, inst.Arg(0)); break;
		case ValueOpcode::SetExecHi: pass.Write(Flag::ExecHi, block, inst.Arg(0)); break;
		case ValueOpcode::SetVcc: pass.Write(Flag::Vcc, block, inst.Arg(0)); break;
		case ValueOpcode::SetVccLo: pass.Write(Flag::VccLo, block, inst.Arg(0)); break;
		case ValueOpcode::SetVccHi: pass.Write(Flag::VccHi, block, inst.Arg(0)); break;
		case ValueOpcode::SetM0: pass.Write(Flag::M0, block, inst.Arg(0)); break;
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
		case ValueOpcode::GetScc: inst.ReplaceUsesWith(pass.Read(Flag::Scc, block)); break;
		case ValueOpcode::GetExec: inst.ReplaceUsesWith(pass.Read(Flag::Exec, block)); break;
		case ValueOpcode::GetExecLo: inst.ReplaceUsesWith(pass.Read(Flag::ExecLo, block)); break;
		case ValueOpcode::GetExecHi: inst.ReplaceUsesWith(pass.Read(Flag::ExecHi, block)); break;
		case ValueOpcode::GetVcc: inst.ReplaceUsesWith(pass.Read(Flag::Vcc, block)); break;
		case ValueOpcode::GetVccLo: inst.ReplaceUsesWith(pass.Read(Flag::VccLo, block)); break;
		case ValueOpcode::GetVccHi: inst.ReplaceUsesWith(pass.Read(Flag::VccHi, block)); break;
		case ValueOpcode::GetM0: inst.ReplaceUsesWith(pass.Read(Flag::M0, block)); break;
		default: break;
	}
}

} // namespace

void RewriteToSsa(const BlockList& blocks) {
	Pass                               pass;
	std::unordered_map<Block*, size_t> filled_predecessors;
	for (auto* block: blocks) {
		if (block->ImmPredecessors().empty()) {
			pass.Seal(block);
		}
		for (auto& inst: *block) {
			VisitInstruction(pass, block, inst);
		}
		for (auto* successor: block->ImmSuccessors()) {
			if (++filled_predecessors[successor] == successor->ImmPredecessors().size()) {
				pass.Seal(successor);
			}
		}
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
