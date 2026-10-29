#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"

#include <algorithm>
#include <map>
#include <optional>
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

// The initial contents of a VGPR that only v_writelane writes (MarkUndefinedLaneSpills).
bool IsUndefinedSpill(Value value) {
	const auto* inst = value.Resolve().TryInstruction();
	return inst != nullptr && inst->GetOpcode() == ValueOpcode::UndefU32;
}

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
	bool written = false;

	while (!queue.empty()) {
		const auto chain = SearchChain(queue.front(), lane, wave_size);
		queue.pop();
		if (chain.write != nullptr) {
			written = true;
			continue;
		}
		if (IsUndefinedSpill(chain.value)) {
			// A path on which the lane was never written reads an undefined value.
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
	return written;
}

using PhiMap = std::unordered_map<Inst*, Inst*>;

Value GetRealValue(PhiMap& phi_map, Value source, uint32_t lane, uint32_t wave_size) {
	const auto chain = SearchChain(source, lane, wave_size);
	if (chain.write != nullptr) {
		return chain.write->Arg(1);
	}
	if (IsUndefinedSpill(chain.value)) {
		return chain.value;
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
	// An undefined argument stays undefined: the written value of the other paths need not be
	// available on its edge. The Phi then names the spilled value wherever it was written.
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

// A full-EXEC prefix scan with both half-wave endpoint reads is a reduction. Wave64 first joins
// the two rows of each half; a wave32 half is one row.
// Fragment subgroups may have no physical invocation at either endpoint.
class WaveReductionMatcher {
public:
	WaveReductionMatcher(const Block* block, ValueOpcode operation)
	    : block(block), operation(operation) {}

	Value Match(const Inst& reduction, uint32_t wave_size) const {
		const auto* low  = Get(reduction.Arg(0), ValueOpcode::ReadLane);
		const auto* high = Get(reduction.Arg(1), ValueOpcode::ReadLane);
		if (low == nullptr || high == nullptr || low->Arg(0).Resolve() != high->Arg(0).Resolve()) {
			return {};
		}
		const auto lane = [&](Value value, uint32_t expected) {
			value = value.Resolve();
			return value.IsImmediate() && value.GetType() == Type::U32 &&
			       value.U32() % wave_size == expected;
		};
		const auto half = wave_size / 2u;
		if (!((lane(low->Arg(1), half - 1u) && lane(high->Arg(1), wave_size - 1u)) ||
		      (lane(low->Arg(1), wave_size - 1u) && lane(high->Arg(1), half - 1u)))) {
			return {};
		}
		if (wave_size == 32u) return MatchScan(low->Arg(0));
		const auto* joined = Get(low->Arg(0), operation);
		if (joined == nullptr) return {};
		for (size_t index = 0; index < 2; index++) {
			const auto* cross = Get(joined->Arg(index), ValueOpcode::Permlane16U32);
			if (cross == nullptr) continue;
			const auto flags  = cross->Flags<PermlaneFlags>();
			const auto prefix = joined->Arg(1 - index).Resolve();
			if (!flags.x16 || flags.fetch_inactive || !flags.bound_control ||
			    cross->Arg(1).Resolve() != Value(UINT32_MAX) ||
			    cross->Arg(2).Resolve() != Value(UINT32_MAX) || !FullExec(cross->Arg(3)) ||
			    cross->Arg(0).Resolve() != prefix) {
				continue;
			}
			if (const auto seed = MatchScan(prefix); !seed.IsEmpty()) return seed;
		}
		return {};
	}

private:
	// The row scan's input.
	Value MatchScan(Value prefix) const {
		for (const uint32_t shift: {8u, 4u, 2u, 1u}) {
			prefix = MatchRow(prefix, shift);
		}
		return prefix;
	}

	Inst* Get(Value value, ValueOpcode opcode) const {
		auto* inst = value.Resolve().TryInstruction();
		return inst != nullptr && inst->Parent() == block && inst->GetOpcode() == opcode
		           ? inst : nullptr;
	}

	// Constant propagation folds the x | ~x EXEC of s_orn2_saveexec to true.
	static bool FullExec(Value predicate) { return predicate.Resolve() == Value(true); }

	static bool IsRowShift(const Inst& inst, uint32_t shift) {
		const auto flags = inst.Flags<DppMoveFlags>();
		return flags.control == 0x110u + shift && flags.row_mask == 0xf &&
		       flags.bank_mask == 0xf && !flags.fetch_inactive && !flags.bound_control && !flags.dpp8;
	}

	Value MatchRow(Value value, uint32_t shift) const {
		const auto* update = Get(value, ValueOpcode::DppUpdateU32);
		if (update == nullptr || !IsRowShift(*update, shift) || !FullExec(update->Arg(2))) return {};
		const auto* combined = Get(update->Arg(0), operation);
		if (combined == nullptr) return {};
		for (size_t index = 0; index < 2; index++) {
			const auto* move = Get(combined->Arg(index), ValueOpcode::DppMoveU32);
			if (move == nullptr || !IsRowShift(*move, shift) || !FullExec(move->Arg(1))) continue;
			const auto previous = combined->Arg(1 - index).Resolve();
			if (move->Arg(0).Resolve() == previous && update->Arg(1).Resolve() == previous) {
				return previous;
			}
		}
		return {};
	}

	const Block* block;
	ValueOpcode  operation;
};

// Associative, commutative operations with a subgroup reduction; lanes combine in any order.
bool IsWaveReduction(ValueOpcode operation) {
	switch (operation) {
		case ValueOpcode::IAdd32:
		case ValueOpcode::SMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMin32:
		case ValueOpcode::UMax32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32: return true;
		default: return false;
	}
}

} // namespace

std::vector<Inst*> MarkUndefinedLaneSpills(Program& program) {
	std::vector<Inst*> undefined;
	if (program.blocks.empty() || !program.blocks.front()->ImmPredecessors().empty()) {
		return undefined;
	}
	// Registers whose every write is a v_writelane of their own value: SGPR spills.
	std::map<uint32_t, bool> lane_only;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::SetVectorRegister) {
				continue;
			}
			const auto  reg   = inst.Arg(0).VectorRegister();
			const auto* write = inst.Arg(1).Resolve().TryInstruction();
			const auto* read  = write != nullptr && write->GetOpcode() == ValueOpcode::WriteLane
			                        ? write->Arg(0).Resolve().TryInstruction()
			                        : nullptr;
			const bool lane_write = read != nullptr &&
			                        read->GetOpcode() == ValueOpcode::GetVectorRegister &&
			                        read->Arg(0).VectorRegister() == reg;
			const auto [entry, inserted] = lane_only.try_emplace(static_cast<uint32_t>(reg), lane_write);
			entry->second = entry->second && lane_write;
		}
	}
	auto* entry_block = program.blocks.front();
	const auto start  = entry_block->begin();
	for (const auto& [reg, only_lanes]: lane_only) {
		if (!only_lanes) {
			continue;
		}
		auto& value = *entry_block->PrependNewInst(start, ValueOpcode::UndefU32);
		entry_block->PrependNewInst(start, ValueOpcode::SetVectorRegister,
		                            {Value(static_cast<VectorReg>(reg)), Value(&value)});
		undefined.push_back(&value);
	}
	return undefined;
}

void ReleaseUndefinedLaneSpills(const std::vector<Inst*>& undefined) {
	for (auto* value: undefined) {
		value->ReplaceUsesWith(Value(0u));
	}
}

ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size) {
	ReadLaneStats stats;
	if (wave_size != 32u && wave_size != 64u) {
		return stats;
	}

	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			const auto opcode = inst.GetOpcode();
			if (IsWaveReduction(opcode)) {
				const auto seed = WaveReductionMatcher(block, opcode).Match(inst, wave_size);
				if (!seed.IsEmpty()) {
					const auto position = std::find_if(block->begin(), block->end(),
					                                  [&](const Inst& candidate) { return &candidate == &inst; });
					auto&      reduction =
					    *block->PrependNewInst(position, ValueOpcode::SubgroupReduceU32, {seed});
					reduction.SetFlags(opcode);
					inst.ReplaceUsesWith(Value(&reduction));
					stats.rewritten_reductions++;
				}
				continue;
			}
			if (opcode != ValueOpcode::ReadLane) {
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
