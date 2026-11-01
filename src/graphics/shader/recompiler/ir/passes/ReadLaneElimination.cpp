#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"

#include <algorithm>
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
		const auto lane = [&](Value value) {
			value = value.Resolve();
			return value.IsImmediate() && value.GetType() == Type::U32
			           ? value.U32() % wave_size : wave_size;
		};
		const auto first  = lane(low->Arg(1));
		const auto second = lane(high->Arg(1));
		const auto half   = wave_size / 2u;
		if (first == wave_size || second == wave_size || (first < half) == (second < half)) {
			return {};
		}
		const bool endpoints =
		    std::min(first, second) == half - 1u && std::max(first, second) == wave_size - 1u;
		for (const uint32_t row_control: {RowShr, RowXmask}) {
			if (row_control == RowShr && !endpoints) continue;
			if (wave_size == 32u) {
				if (const auto seed = MatchScan(low->Arg(0), row_control); !seed.IsEmpty()) return seed;
				continue;
			}
			const auto* joined = Get(low->Arg(0), operation);
			if (joined == nullptr) continue;
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
				if (const auto seed = MatchScan(prefix, row_control); !seed.IsEmpty()) return seed;
			}
		}
		return {};
	}

private:
	static constexpr uint32_t RowShr   = 0x110u;
	static constexpr uint32_t RowXmask = 0x160u;

	// The row scan's input.
	Value MatchScan(Value prefix, uint32_t row_control) const {
		for (const uint32_t shift: {8u, 4u, 2u, 1u}) {
			prefix = MatchRow(prefix, row_control + shift);
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

	static bool IsRowStep(const Inst& inst, uint32_t control) {
		const auto flags = inst.Flags<DppMoveFlags>();
		return flags.control == control && flags.row_mask == 0xf &&
		       flags.bank_mask == 0xf && !flags.fetch_inactive && !flags.bound_control && !flags.dpp8;
	}

	Value MatchRow(Value value, uint32_t control) const {
		const auto* update = Get(value, ValueOpcode::DppUpdateU32);
		if (update == nullptr || !IsRowStep(*update, control) || !FullExec(update->Arg(2))) return {};
		const auto* combined = Get(update->Arg(0), operation);
		if (combined == nullptr) return {};
		for (size_t index = 0; index < 2; index++) {
			const auto* move = Get(combined->Arg(index), ValueOpcode::DppMoveU32);
			if (move == nullptr || !IsRowStep(*move, control) || !FullExec(move->Arg(1))) continue;
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

// The subgroup reduction that replaces a proven whole-wave scan of operation.
std::optional<ValueOpcode> WaveReduction(ValueOpcode operation) {
	switch (operation) {
		case ValueOpcode::IAdd32: return ValueOpcode::SubgroupIAddU32;
		case ValueOpcode::UMin32: return ValueOpcode::SubgroupUMinU32;
		case ValueOpcode::UMax32: return ValueOpcode::SubgroupUMaxU32;
		case ValueOpcode::SMin32: return ValueOpcode::SubgroupSMinU32;
		case ValueOpcode::SMax32: return ValueOpcode::SubgroupSMaxU32;
		case ValueOpcode::BitwiseAnd32: return ValueOpcode::SubgroupAndU32;
		case ValueOpcode::BitwiseOr32: return ValueOpcode::SubgroupOrU32;
		case ValueOpcode::BitwiseXor32: return ValueOpcode::SubgroupXorU32;
		default: return std::nullopt;
	}
}

} // namespace

ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size) {
	ReadLaneStats stats;
	if (wave_size != 32u && wave_size != 64u) {
		return stats;
	}

	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			const auto opcode = inst.GetOpcode();
			if (const auto reduce = WaveReduction(opcode)) {
				const auto seed = WaveReductionMatcher(block, opcode).Match(inst, wave_size);
				if (!seed.IsEmpty()) {
					const auto position = std::find_if(block->begin(), block->end(),
					                                  [&](const Inst& candidate) { return &candidate == &inst; });
					auto&      reduction = *block->PrependNewInst(position, *reduce, {seed});
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
