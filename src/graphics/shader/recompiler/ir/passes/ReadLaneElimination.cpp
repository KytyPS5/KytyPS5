#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"

#include <algorithm>

#include <queue>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct ChainResult {
	Value value;
	Inst* write = nullptr;
};

ChainResult SearchChain(Value value, uint32_t lane, uint32_t wave_size) {
	for (;;) {
		value      = value.Resolve();
		auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::WriteLane) {
			return {value};
		}
		const auto selector = inst->Arg(2).Resolve();
		if (!selector.IsImmediate() || selector.GetType() != Type::U32) {
			return {value};
		}
		if (selector.U32() % wave_size == lane) {
			return {value, inst};
		}
		value = inst->Arg(0);
	}
}

bool IsPossibleToEliminate(Value source, uint32_t lane, uint32_t wave_size) {
	std::queue<Value>         queue;
	std::unordered_set<Inst*> visited;
	queue.push(source);

	while (!queue.empty()) {
		const auto chain = SearchChain(queue.front(), lane, wave_size);
		queue.pop();
		if (chain.write != nullptr) {
			continue;
		}
		auto* inst = chain.value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::Phi || inst->NumArgs() == 0) {
			return false;
		}
		if (!visited.insert(inst).second) {
			continue;
		}
		for (size_t index = inst->NumArgs(); index-- > 0;) {
			queue.push(inst->Arg(index));
		}
	}
	return true;
}

using PhiMap = std::unordered_map<Inst*, Inst*>;

Value GetRealValue(PhiMap& phi_map, Value source, uint32_t lane, uint32_t wave_size) {
	const auto chain = SearchChain(source, lane, wave_size);
	if (chain.write != nullptr) {
		return chain.write->Arg(1);
	}

	auto* inst = chain.value.ResolveInstruction();
	EXIT_IF(inst->GetOpcode() != ValueOpcode::Phi);
	const auto [entry, is_new] = phi_map.try_emplace(inst);
	if (!is_new) {
		return Value(entry->second);
	}

	auto* block           = inst->Parent();
	auto  insertion_point = std::find_if(block->begin(), block->end(),
	                                     [&](const Inst& candidate) { return &candidate == inst; });
	EXIT_IF(insertion_point == block->end());
	auto& phi = *block->PrependNewInst(insertion_point, ValueOpcode::Phi);
	phi.SetFlags(Type::U32);
	entry->second = &phi;

	std::vector<Value> arguments;
	arguments.reserve(inst->NumArgs());
	for (size_t index = 0; index < inst->NumArgs(); index++) {
		arguments.push_back(GetRealValue(phi_map, inst->Arg(index), lane, wave_size));
	}
	const auto first = arguments.front().Resolve();
	if (std::ranges::all_of(arguments,
	                        [&](Value argument) { return argument.Resolve() == first; })) {
		phi.ReplaceUsesWith(first);
	} else {
		for (size_t index = 0; index < arguments.size(); index++) {
			phi.AddPhiOperand(inst->PhiBlock(index), arguments[index]);
		}
	}
	return Value(&phi);
}

// EXEC after s_or/s_orn2_saveexec of the old EXEC with its complement: every lane of the wave.
bool IsAllOnes(Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		return value.GetType() == Type::U32 && value.U32() == 0xffffffffu;
	}
	const auto* inst = value.TryInstruction();
	if (inst != nullptr && inst->GetOpcode() == ValueOpcode::SelectU32) {
		// A wave64 picks the mask word of the lane's half.
		return IsAllOnes(inst->Arg(1)) && IsAllOnes(inst->Arg(2));
	}
	if (inst == nullptr || inst->GetOpcode() != ValueOpcode::BitwiseOr32) {
		return false;
	}
	for (size_t index = 0; index < 2; index++) {
		const auto* complement = inst->Arg(index).Resolve().TryInstruction();
		if (complement != nullptr && complement->GetOpcode() == ValueOpcode::BitwiseNot32 &&
		    complement->Arg(0).Resolve() == inst->Arg(1 - index).Resolve()) {
			return true;
		}
	}
	return false;
}

// The lane's EXEC bit of such a mask, as the translator reads a mask
// (((mask >> (lane & 31)) & 1) != 0), or as a thread bit (!exec || exec).
bool IsEveryLane(Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		return value.GetType() == Type::U1 && value.U1();
	}
	const auto* test = value.TryInstruction();
	if (test == nullptr) {
		return false;
	}
	if (test->GetOpcode() == ValueOpcode::LogicalOr) {
		for (size_t index = 0; index < 2; index++) {
			const auto* negation = test->Arg(index).Resolve().TryInstruction();
			if (negation != nullptr && negation->GetOpcode() == ValueOpcode::LogicalNot &&
			    negation->Arg(0).Resolve() == test->Arg(1 - index).Resolve()) {
				return true;
			}
		}
		return false;
	}
	if (test->GetOpcode() != ValueOpcode::INotEqual32 || test->Arg(1).Resolve() != Value(0u)) {
		return false;
	}
	const auto* bit = test->Arg(0).Resolve().TryInstruction();
	if (bit == nullptr || bit->GetOpcode() != ValueOpcode::BitwiseAnd32 ||
	    bit->Arg(1).Resolve() != Value(1u)) {
		return false;
	}
	const auto* shift = bit->Arg(0).Resolve().TryInstruction();
	if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftRightLogical32) {
		return false;
	}
	const auto* amount = shift->Arg(1).Resolve().TryInstruction();
	return amount != nullptr && amount->GetOpcode() == ValueOpcode::BitwiseAnd32 &&
	       amount->Arg(1).Resolve() == Value(31u) && IsAllOnes(shift->Arg(0));
}

bool IsRowShiftRight(const DppMoveFlags& flags, uint32_t amount) {
	return !flags.dpp8 && flags.control == 0x110u + amount && flags.row_mask == 0xfu &&
	       flags.bank_mask == 0xfu && !flags.fetch_inactive && !flags.bound_control;
}

bool IsReduction(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::UMax32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::SMin32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseXor32: return true;
		default: return false;
	}
}

// The row scan: value = DppUpdate(op(DppMove(previous), previous), previous, every lane) for
// row_shr 8, 4, 2 and 1. Sets the operation and returns the value scanned.
std::optional<Value> MatchRowScan(Value value, ValueOpcode& operation) {
	for (const uint32_t amount: {8u, 4u, 2u, 1u}) {
		const auto* update = value.Resolve().TryInstruction();
		if (update == nullptr || update->GetOpcode() != ValueOpcode::DppUpdateU32 ||
		    !IsRowShiftRight(update->Flags<DppMoveFlags>(), amount) ||
		    !IsEveryLane(update->Arg(2))) {
			return std::nullopt;
		}
		const auto  previous = update->Arg(1).Resolve();
		const auto* step     = update->Arg(0).Resolve().TryInstruction();
		if (step == nullptr || !IsReduction(step->GetOpcode()) ||
		    (operation != ValueOpcode::Void && step->GetOpcode() != operation)) {
			return std::nullopt;
		}
		operation    = step->GetOpcode();
		bool matched = false;
		for (size_t index = 0; index < 2 && !matched; index++) {
			const auto* move = step->Arg(index).Resolve().TryInstruction();
			matched = move != nullptr && move->GetOpcode() == ValueOpcode::DppMoveU32 &&
			          IsRowShiftRight(move->Flags<DppMoveFlags>(), amount) &&
			          move->Arg(0).Resolve() == previous && IsEveryLane(move->Arg(1)) &&
			          step->Arg(1 - index).Resolve() == previous;
		}
		if (!matched) {
			return std::nullopt;
		}
		value = previous;
	}
	return value.Resolve();
}

} // namespace

uint32_t ReductionIdentity(ValueOpcode operation) {
	switch (operation) {
		case ValueOpcode::UMin32:
		case ValueOpcode::BitwiseAnd32: return 0xffffffffu;
		case ValueOpcode::SMin32: return 0x7fffffffu;
		case ValueOpcode::SMax32: return 0x80000000u;
		default: return 0u;
	}
}

std::optional<LaneReduction> MatchLaneReduction(const Inst& read_lane, uint32_t wave_size) {
	if (read_lane.GetOpcode() != ValueOpcode::ReadLane) {
		return std::nullopt;
	}
	const auto selector = read_lane.Arg(1).Resolve();
	if (!selector.IsImmediate() || selector.GetType() != Type::U32 || selector.U32() >= wave_size ||
	    selector.U32() % 16u != 15u) {
		return std::nullopt;
	}
	const auto    lane  = selector.U32();
	const auto    value = read_lane.Arg(0).Resolve();
	LaneReduction result;
	if (const auto source = MatchRowScan(value, result.operation)) {
		result.source     = *source;
		result.first_lane = lane - 15u;
		result.lanes      = 16u;
		return result;
	}
	// The row pair: select(every lane, op(scan, select(every lane, V_PERMLANEX16(scan, lane 15
	// of the other row), old)), scan), read at lane 32h+31.
	if (lane % 32u != 31u) {
		return std::nullopt;
	}
	const auto* merged = value.TryInstruction();
	if (merged == nullptr || merged->GetOpcode() != ValueOpcode::SelectU32 ||
	    !IsEveryLane(merged->Arg(0))) {
		return std::nullopt;
	}
	const auto  scan      = merged->Arg(2).Resolve();
	const auto* operation = merged->Arg(1).Resolve().TryInstruction();
	if (operation == nullptr || !IsReduction(operation->GetOpcode())) {
		return std::nullopt;
	}
	for (size_t index = 0; index < 2; index++) {
		const auto* other = operation->Arg(index).Resolve().TryInstruction();
		if (other == nullptr || other->GetOpcode() != ValueOpcode::SelectU32 ||
		    !IsEveryLane(other->Arg(0)) || operation->Arg(1 - index).Resolve() != scan) {
			continue;
		}
		const auto* permlane = other->Arg(1).Resolve().TryInstruction();
		if (permlane == nullptr || permlane->GetOpcode() != ValueOpcode::Permlane16U32 ||
		    !permlane->Flags<PermlaneFlags>().x16 || permlane->Arg(0).Resolve() != scan ||
		    permlane->Arg(1).Resolve() != Value(0xffffffffu) ||
		    permlane->Arg(2).Resolve() != Value(0xffffffffu) || !IsEveryLane(permlane->Arg(3))) {
			continue;
		}
		result.operation = operation->GetOpcode();
		if (const auto source = MatchRowScan(scan, result.operation)) {
			result.source     = *source;
			result.first_lane = lane - 31u;
			result.lanes      = 32u;
			return result;
		}
	}
	return std::nullopt;
}

ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size) {
	ReadLaneStats stats;
	if (wave_size != 32u && wave_size != 64u) {
		return stats;
	}

	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::ReadLane) {
				continue;
			}
			const auto selector = inst.Arg(1).Resolve();
			if (!selector.IsImmediate() || selector.GetType() != Type::U32) {
				continue;
			}

			const auto lane  = selector.U32() % wave_size;
			const auto chain = SearchChain(inst.Arg(0), lane, wave_size);
			if (chain.write != nullptr) {
				inst.ReplaceUsesWith(chain.write->Arg(1));
				stats.rewritten_reads++;
				continue;
			}
			auto* producer = chain.value.TryInstruction();
			if (producer == nullptr || producer->GetOpcode() != ValueOpcode::Phi ||
			    !IsPossibleToEliminate(chain.value, lane, wave_size)) {
				continue;
			}

			PhiMap phi_map;
			inst.ReplaceUsesWith(GetRealValue(phi_map, chain.value, lane, wave_size));
			stats.rewritten_reads++;
		}
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
