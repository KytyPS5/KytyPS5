#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

void RemoveIdentities(Program& program) {
	// Resolve metadata before deleting its identity nodes; retain only folded native constants.
	for (auto& write: program.scalar_writes) {
		const auto value = write.value.Resolve();
		write.value = value.IsImmediate() && value.GetType() == Type::U32 ? value : Value {};
	}
	for (auto* block: program.blocks) {
		block->condition       = block->condition.Resolve();
		block->indirect_target = block->indirect_target.Resolve();
	}
	for (auto* block: program.blocks) {
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
		std::vector<Inst*> pending;
		for (auto* block: blocks) {
			for (auto& inst: *block) {
				// Retained resource-planning instructions live outside the block list.
				inst.live = inst.MayHaveSideEffects() || std::ranges::any_of(inst.Uses(),
				    [](const Use& use) { return use.user->Parent() == nullptr; });
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

		changed = false;
		for (auto* block: blocks) {
			const auto& term = block->terminator;
			if (term.kind != CFG::TerminatorKind::ConditionalBranch || term.loop_header ||
			    block->condition.IsImmediate()) continue;
			for (const bool take_true: {false, true}) {
				const auto* empty = take_true ? term.false_block : term.true_block;
				const auto* merge = take_true ? term.true_block : term.false_block;
				if (empty == nullptr || merge == nullptr || !empty->empty() ||
				    empty->terminator.loop_header || merge->terminator.loop_header ||
				    empty->terminator.kind != CFG::TerminatorKind::Branch ||
				    empty->terminator.true_block != merge ||
				    std::ranges::any_of(*merge, [](const Inst& inst) {
					    return inst.GetOpcode() == ValueOpcode::Phi;
				    })) continue;
				// Keep the CFG and SSA edges intact; only the empty detour's predicate is dead.
				for (auto& inst: *block) {
					if (inst.GetOpcode() == ValueOpcode::Reference &&
					    inst.Arg(0).Resolve() == block->condition.Resolve())
						inst.SetArg(0, Value(take_true));
				}
				block->condition = Value(take_true);
				changed = true;
				break;
			}
		}
	} while (changed);
}

namespace {

// Whether an instruction computes each lane's result from that lane's operands alone, with no
// side effects, so its operands matter only in the lanes its result does.
bool IsLaneLocal(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::Identity:
		case ValueOpcode::BitCastU16F16:
		case ValueOpcode::BitCastF16U16:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64:
		case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32:
		case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32:
		case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32F16:
		case ValueOpcode::ConvertF32F16Lane:
		case ValueOpcode::ConvertF16F32:
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32S32:
		case ValueOpcode::ConvertF64S32:
		case ValueOpcode::ConvertF32F64:
		case ValueOpcode::ConvertF64F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertF64U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3:
		case ValueOpcode::CompositeConstructF32x2:
		case ValueOpcode::CompositeConstructU32x4:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::PackHalf2x16:
		case ValueOpcode::PackSnorm2x16:
		case ValueOpcode::PackUnorm2x16:
		case ValueOpcode::PackFloat2x16Rtz:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::SelectF64:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::UDiv32:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::IAbs32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::SMinTri32:
		case ValueOpcode::UMinTri32:
		case ValueOpcode::SMaxTri32:
		case ValueOpcode::UMaxTri32:
		case ValueOpcode::SMedTri32:
		case ValueOpcode::UMedTri32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual64:
		case ValueOpcode::ULessThanEqual64:
		case ValueOpcode::UGreaterThanEqual64:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdEqual64:
		case ValueOpcode::FPOrdLessThanEqual64:
		case ValueOpcode::FPOrdGreaterThanEqual64:
		case ValueOpcode::FPOrdNotEqual64:
		case ValueOpcode::FPOrdLessThan64:
		case ValueOpcode::FPOrdGreaterThan64:
		case ValueOpcode::FPUnordEqual64:
		case ValueOpcode::FPUnordNotEqual64:
		case ValueOpcode::FPUnordLessThan64:
		case ValueOpcode::FPUnordGreaterThan64:
		case ValueOpcode::FPUnordLessThanEqual64:
		case ValueOpcode::FPUnordGreaterThanEqual64:
		case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPUnordGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPIsNan64:
		case ValueOpcode::FPCmpClass32:
		case ValueOpcode::FPCmpClass16:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPAdd64:
		case ValueOpcode::FPMul64:
		case ValueOpcode::FPFma64:
		case ValueOpcode::FPRecip64:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPMin64:
		case ValueOpcode::FPMax64:
		case ValueOpcode::PackedFPAdd16x2:
		case ValueOpcode::PackedFPMul16x2:
		case ValueOpcode::PackedFPFma16x2:
		case ValueOpcode::PackedFPMin16x2:
		case ValueOpcode::PackedFPMax16x2:
		case ValueOpcode::FPAdd16:
		case ValueOpcode::FPSub16:
		case ValueOpcode::FPMul16:
		case ValueOpcode::FPFma16:
		case ValueOpcode::FPMin16:
		case ValueOpcode::FPMax16:
		case ValueOpcode::FPAbs16:
		case ValueOpcode::FPNeg16:
		case ValueOpcode::ExtractF16Lane:
		case ValueOpcode::InsertF16Lane:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipSqrt32:
		case ValueOpcode::FPSqrt:
		case ValueOpcode::FPSin:
		case ValueOpcode::FPCos:
		case ValueOpcode::FPExp2:
		case ValueOpcode::FPLog2:
		case ValueOpcode::FPLdexp:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::FPFloor64:
		case ValueOpcode::FPCeil64:
		case ValueOpcode::FPTrunc64:
		case ValueOpcode::FPFract32:
		case ValueOpcode::FPFract64: return true;
		default: return false;
	}
}

// Whether a memory operation reads or writes for a lane only when its trailing predicate is
// set, so its other operands matter only in those lanes. Image sampling is left out: its
// coordinates also feed the derivatives of neighboring lanes.
bool IsPredicatedPerLane(const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (inst.NumArgs() == 0 || inst.Arg(inst.NumArgs() - 1).GetType() != Type::U1) {
		return false;
	}
	switch (SharedAccessOf(op)) {
		case SharedAccess::Append:
		case SharedAccess::Consume: return false;
		case SharedAccess::None: break;
		default: return true;
	}
	switch (op) {
		case ValueOpcode::ImageRead:
		case ValueOpcode::ImageWrite: return true;
		default: break;
	}
	return BufferAccessOf(op) != BufferAccess::None ||
	       AddressOpcodeInfoOf(op).access != AddressAccess::None;
}

// The lanes a value is needed in, as a conjunction of conditions: the lanes where all of them
// are set. No conditions means every lane; an undemanded value has not been reached yet.
struct Demand {
	bool                     reached = false;
	std::vector<const Inst*> conditions;
};

void AddConditions(Value condition, std::vector<const Inst*>& conditions) {
	condition = condition.Resolve();
	if (condition.IsImmediate()) {
		return;
	}
	const auto* inst = condition.TryInstruction();
	if (inst == nullptr) {
		return;
	}
	if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
		AddConditions(inst->Arg(0), conditions);
		AddConditions(inst->Arg(1), conditions);
		return;
	}
	if (const auto found = std::ranges::lower_bound(conditions, inst);
	    found == conditions.end() || *found != inst) {
		conditions.insert(found, inst);
	}
}

} // namespace

uint32_t EliminateMaskedSelects(const BlockList& blocks) {
	// A guest vector write keeps an inactive lane's old value: select(exec, new, old). When the
	// result is only needed in lanes where exec is set, because every consumer is masked by it
	// again, the select is just its new value.
	std::unordered_map<const Inst*, Demand> demands;
	const auto meet = [&](Value value, const std::vector<const Inst*>& conditions) {
		value = value.Resolve();
		const auto* inst = value.IsImmediate() ? nullptr : value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		auto& demand = demands[inst];
		if (!demand.reached) {
			demand = {true, conditions};
			return true;
		}
		// Needed wherever either use needs it: only the conditions both require remain.
		const auto before = demand.conditions.size();
		std::erase_if(demand.conditions, [&](const Inst* condition) {
			return !std::ranges::binary_search(conditions, condition);
		});
		return demand.conditions.size() != before;
	};
	const std::vector<const Inst*> every_lane;
	uint32_t                       replaced = 0;
	for (bool changed = true; changed;) {
		changed = false;
		for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
			for (const auto& value: {(*block)->condition, (*block)->indirect_target}) {
				if (!value.IsEmpty()) changed |= meet(value, every_lane);
			}
			auto& instructions = (*block)->Instructions();
			for (auto inst = instructions.rbegin(); inst != instructions.rend(); ++inst) {
				const auto op = inst->GetOpcode();
				const bool external = std::ranges::any_of(inst->Uses(), [](const Use& use) {
					return use.user->Parent() == nullptr;
				});
				if (IsLaneLocal(op) && !external) {
					const auto found = demands.find(&*inst);
					if (found == demands.end() || !found->second.reached) {
						continue;
					}
					const auto conditions = found->second.conditions;
					const bool select = op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
					                    op == ValueOpcode::SelectF32 || op == ValueOpcode::SelectF64;
					for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
						if (select && arg == 1) {
							// The new value is only chosen where the condition is set.
							auto chosen = conditions;
							AddConditions(inst->Arg(0), chosen);
							changed |= meet(inst->Arg(arg), chosen);
						} else {
							changed |= meet(inst->Arg(arg), conditions);
						}
					}
				} else if (IsPredicatedPerLane(*inst) && !external) {
					const auto predicate = inst->Arg(inst->NumArgs() - 1);
					std::vector<const Inst*> active;
					AddConditions(predicate, active);
					for (size_t arg = 0; arg + 1 < inst->NumArgs(); arg++) {
						changed |= meet(inst->Arg(arg), active);
					}
					changed |= meet(predicate, every_lane);
				} else {
					for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
						changed |= meet(inst->Arg(arg), every_lane);
					}
				}
			}
		}
	}
	for (auto* block: blocks) {
		for (auto& inst: *block) {
			const auto op = inst.GetOpcode();
			if (op != ValueOpcode::SelectU1 && op != ValueOpcode::SelectU32 &&
			    op != ValueOpcode::SelectF32 && op != ValueOpcode::SelectF64) {
				continue;
			}
			const auto found = demands.find(&inst);
			if (found == demands.end() || !found->second.reached ||
			    std::ranges::any_of(inst.Uses(), [](const Use& use) {
				    return use.user->Parent() == nullptr;
			    })) {
				continue;
			}
			std::vector<const Inst*> selected;
			AddConditions(inst.Arg(0), selected);
			if (!selected.empty() && std::ranges::includes(found->second.conditions, selected)) {
				inst.ReplaceUsesWith(inst.Arg(1).Resolve());
				replaced++;
			}
		}
	}
	return replaced;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
