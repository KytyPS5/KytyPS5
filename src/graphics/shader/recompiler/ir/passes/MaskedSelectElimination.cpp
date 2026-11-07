#include "graphics/shader/recompiler/ir/passes/MaskedSelectElimination.h"

#include <ranges>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

// Where a value is needed: in no lane (its users are dead), only in the lanes where `condition`
// holds, or in any lane.
struct Need {
	enum class Kind { Never, Where, Always };
	Kind  kind = Kind::Always;
	Value condition;
};

// Whether `predicate`, possibly a conjunction of EXEC masks, implies `condition` in every lane.
bool Implies(Value predicate, Value condition, uint32_t depth = 0) {
	predicate = predicate.Resolve();
	if (predicate == condition.Resolve()) return true;
	const auto* inst = predicate.TryInstruction();
	return depth < 16u && inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalAnd &&
	       (Implies(inst->Arg(0), condition, depth + 1u) ||
	        Implies(inst->Arg(1), condition, depth + 1u));
}

bool IsSelect(ValueOpcode opcode) {
	return opcode == ValueOpcode::SelectU1 || opcode == ValueOpcode::SelectU32 ||
	       opcode == ValueOpcode::SelectF32;
}

// An operation whose value in a lane depends only on its operands in that lane. The section of
// ValueOpcodes.inc from BitCastU16F16 to FPFract64 holds such operations, apart from the DPP and
// WQM ones.
bool IsLaneLocal(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::DppMoveU32:
		case ValueOpcode::DppUpdateU32:
		case ValueOpcode::WqmU64: return false;
		case ValueOpcode::Identity:
		case ValueOpcode::SelectU32: return true;
		default: return opcode >= ValueOpcode::BitCastU16F16 && opcode <= ValueOpcode::FPFract64;
	}
}

// A buffer, shared-memory or address access whose last operand is its predicate: the emitter
// performs it only in the lanes where the predicate holds.
bool IsPredicatedAccess(const Inst& inst) {
	const auto opcode = inst.GetOpcode();
	const auto shared = SharedAccessOf(opcode);
	const bool access = BufferAccessOf(opcode) != BufferAccess::None ||
	                    shared == SharedAccess::Read || shared == SharedAccess::Write ||
	                    shared == SharedAccess::Atomic ||
	                    AddressOpcodeInfoOf(opcode).access != AddressAccess::None;
	return access && inst.NumArgs() != 0 && inst.Arg(inst.NumArgs() - 1u).GetType() == Type::U1;
}

class Eliminator {
public:
	uint32_t Run(const BlockList& blocks) {
		uint32_t removed = 0;
		for (auto* block: blocks) {
			for (auto& inst: *block) {
				if (IsSelect(inst.GetOpcode())) removed += ForwardArms(inst);
			}
		}
		// Users come after their values (PHIs aside, which need their operands everywhere), so a
		// backward walk finds every user's need first. A user not yet seen counts as Always.
		for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
			auto& instructions = (*block)->Instructions();
			for (auto inst = instructions.rbegin(); inst != instructions.rend(); ++inst) {
				const auto need = NeedOf(*inst);
				m_needs[&*inst] = need;
				if (IsSelect(inst->GetOpcode()) && need.kind == Need::Kind::Where &&
				    Implies(need.condition, inst->Arg(0))) {
					inst->ReplaceUsesWith(inst->Arg(1));
					removed++;
				}
			}
		}
		return removed;
	}

private:
	// A later write of a register under the same EXEC: SELECT(c, x, SELECT(d, y, z)) takes z where c
	// is false when d implies c, and SELECT(c, SELECT(d, x, w), y) takes x where c is true when c
	// implies d. Returns the number of arms that skip a select.
	static uint32_t ForwardArms(Inst& inst) {
		uint32_t forwarded = 0;
		for (bool changed = true; changed;) {
			changed = false;
			const auto condition = inst.Arg(0);
			if (const auto* other = inst.Arg(2).Resolve().TryInstruction();
			    other != nullptr && other->GetOpcode() == inst.GetOpcode() &&
			    Implies(other->Arg(0), condition)) {
				inst.SetArg(2, other->Arg(2));
				changed = true;
			}
			if (const auto* other = inst.Arg(1).Resolve().TryInstruction();
			    other != nullptr && other->GetOpcode() == inst.GetOpcode() &&
			    Implies(condition, other->Arg(0))) {
				inst.SetArg(1, other->Arg(1));
				changed = true;
			}
			forwarded += changed ? 1u : 0u;
		}
		return forwarded;
	}

	Need UseNeed(const Use& use) const {
		const auto& user = *use.user;
		if (user.Parent() == nullptr) return {};
		if (!user.HasUses() && !user.MayHaveSideEffects()) return {Need::Kind::Never, {}};
		const auto opcode = user.GetOpcode();
		if (IsSelect(opcode) && use.operand == 1u) return {Need::Kind::Where, user.Arg(0)};
		if (IsLaneLocal(opcode)) {
			const auto found = m_needs.find(&user);
			return found == m_needs.end() ? Need {} : found->second;
		}
		if (IsPredicatedAccess(user) && use.operand + 1u != user.NumArgs()) {
			return {Need::Kind::Where, user.Arg(user.NumArgs() - 1u)};
		}
		return {};
	}

	// The weakest of the uses' conditions, when every other one implies it.
	Need NeedOf(const Inst& inst) const {
		Need result {Need::Kind::Never, {}};
		for (const auto& use: inst.Uses()) {
			const auto need = UseNeed(use);
			if (need.kind == Need::Kind::Always) return need;
			if (need.kind == Need::Kind::Never) continue;
			if (result.kind == Need::Kind::Never || Implies(result.condition, need.condition)) {
				result = need;
			} else if (!Implies(need.condition, result.condition)) {
				return {};
			}
		}
		return result;
	}

	std::unordered_map<const Inst*, Need> m_needs;
};

} // namespace

uint32_t EliminateMaskedSelects(const BlockList& blocks) {
	return Eliminator().Run(blocks);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
