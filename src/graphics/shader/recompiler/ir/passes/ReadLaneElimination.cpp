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

// A full-EXEC prefix scan with both half-wave endpoint reads is a reduction.
// Fragment subgroups may have no physical invocation at either endpoint.
class UMinReductionMatcher {
public:
	explicit UMinReductionMatcher(Block* block): block(block) {}

	Value Match(const Inst& minimum) {
		const auto* low  = Get(minimum.Arg(0), ValueOpcode::ReadLane);
		const auto* high = Get(minimum.Arg(1), ValueOpcode::ReadLane);
		if (low == nullptr || high == nullptr || low->Arg(0).Resolve() != high->Arg(0).Resolve()) {
			return {};
		}
		const auto lane = [](Value value, uint32_t expected) {
			value = value.Resolve();
			return value.IsImmediate() && value.GetType() == Type::U32 &&
			       value.U32() % 64u == expected;
		};
		if (!((lane(low->Arg(1), 31) && lane(high->Arg(1), 63)) ||
		      (lane(low->Arg(1), 63) && lane(high->Arg(1), 31)))) {
			return {};
		}
		const auto* joined = Get(Unwrap(low->Arg(0)), ValueOpcode::UMin32);
		if (joined == nullptr) return {};
		for (size_t index = 0; index < 2; index++) {
			auto attempt = *this;
			const auto* cross = attempt.Get(attempt.Unwrap(joined->Arg(index)), ValueOpcode::Permlane16U32);
			if (cross == nullptr) continue;
			const auto flags = cross->Flags<PermlaneFlags>();
			auto prefix = attempt.Unwrap(joined->Arg(1 - index));
			if (!flags.x16 || flags.fetch_inactive || !flags.bound_control ||
			    cross->Arg(1).Resolve() != Value(UINT32_MAX) ||
			    cross->Arg(2).Resolve() != Value(UINT32_MAX) ||
			    !attempt.FullExec(cross->Arg(3)) || attempt.Unwrap(cross->Arg(0)) != prefix) {
				continue;
			}
			for (const uint32_t shift: {8u, 4u, 2u, 1u}) {
				prefix = attempt.MatchRow(prefix, shift);
			}
			prefix = attempt.Unwrap(prefix);
			if (prefix.IsEmpty() || attempt.saved_exec.IsEmpty()) return prefix;
			const auto* seed = attempt.Get(prefix, ValueOpcode::SelectU32);
			if (seed != nullptr && seed->Arg(2).Resolve() == Value(UINT32_MAX)) {
				const auto predicate = seed->Arg(0).Resolve();
				const auto* subset = attempt.Get(predicate, ValueOpcode::LogicalAnd);
				if (predicate == attempt.saved_exec ||
				    (subset != nullptr && (subset->Arg(0).Resolve() == attempt.saved_exec ||
				                           subset->Arg(1).Resolve() == attempt.saved_exec))) {
					return prefix;
				}
			}
		}
		return {};
	}

private:
	Inst* Get(Value value, ValueOpcode opcode) const {
		auto* inst = value.Resolve().TryInstruction();
		return inst != nullptr && inst->Parent() == block && inst->GetOpcode() == opcode
		           ? inst : nullptr;
	}

	bool FullExec(Value predicate) {
		predicate = predicate.Resolve();
		if (predicate == Value(true)) return true;
		const auto* either = Get(predicate, ValueOpcode::LogicalOr);
		if (either == nullptr) return false;
		for (size_t index = 0; index < 2; index++) {
			const auto* negate = Get(either->Arg(index), ValueOpcode::LogicalNot);
			if (negate == nullptr || negate->Arg(0).Resolve() != either->Arg(1 - index).Resolve()) continue;
			const auto original = negate->Arg(0).Resolve();
			if (saved_exec.IsEmpty()) saved_exec = original;
			return saved_exec == original;
		}
		return false;
	}

	Value Unwrap(Value value) {
		while (const auto* select = Get(value, ValueOpcode::SelectU32)) {
			if (!FullExec(select->Arg(0))) break;
			value = select->Arg(1);
		}
		return value.Resolve();
	}

	static bool IsRowShift(const Inst& inst, uint32_t shift) {
		const auto flags = inst.Flags<DppMoveFlags>();
		return flags.control == 0x110u + shift && flags.row_mask == 0xf &&
		       flags.bank_mask == 0xf && !flags.fetch_inactive && !flags.bound_control && !flags.dpp8;
	}

	Value MatchRow(Value value, uint32_t shift) {
		const auto* update = Get(Unwrap(value), ValueOpcode::DppUpdateU32);
		if (update == nullptr || !IsRowShift(*update, shift) || !FullExec(update->Arg(2))) return {};
		const auto* minimum = Get(update->Arg(0), ValueOpcode::UMin32);
		if (minimum == nullptr) return {};
		for (size_t index = 0; index < 2; index++) {
			const auto* move = Get(minimum->Arg(index), ValueOpcode::DppMoveU32);
			if (move == nullptr || !IsRowShift(*move, shift) || !FullExec(move->Arg(1))) continue;
			const auto previous = Unwrap(minimum->Arg(1 - index));
			if (Unwrap(move->Arg(0)) == previous && Unwrap(update->Arg(1)) == previous) return previous;
		}
		return {};
	}

	Block* block;
	Value  saved_exec;
};
// A fixed lane of a wave OR pattern is one reduction over a lane set.
uint32_t RewritePatternLanes(Program& program, uint32_t wave_size) {
	uint32_t rewritten = 0;
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			if (inst.GetOpcode() != ValueOpcode::ReadLane) {
				continue;
			}
			const auto  selector = inst.Arg(1).Resolve();
			const auto* producer = inst.Arg(0).Resolve().TryInstruction();
			if (!selector.IsImmediate() || selector.GetType() != Type::U32 || producer == nullptr ||
			    producer->GetOpcode() != ValueOpcode::WaveOrPatternU32) {
				continue;
			}
			const auto pattern = producer->Flags<WaveOrPattern>();
			const auto lane =
			    block->PrependNewInst(it, ValueOpcode::WaveOrPatternLaneU32,
			                          {producer->Arg(0), Value(selector.U32() % wave_size)},
			                          static_cast<uint64_t>(pattern));
			inst.ReplaceUsesWith(Value(&*lane));
			rewritten++;
		}
	}
	return rewritten;
}

} // namespace

ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size) {
	ReadLaneStats stats;
	if (wave_size != 32u && wave_size != 64u) {
		return stats;
	}
	stats.rewritten_reads += RewritePatternLanes(program, wave_size);

	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (wave_size == 64u && inst.GetOpcode() == ValueOpcode::UMin32) {
				const auto seed = UMinReductionMatcher(block).Match(inst);
				if (!seed.IsEmpty()) {
					const auto position = std::find_if(block->begin(), block->end(),
					                                  [&](const Inst& candidate) { return &candidate == &inst; });
					auto& reduction = *block->PrependNewInst(position, ValueOpcode::SubgroupUMinU32, {seed});
					inst.ReplaceUsesWith(Value(&reduction));
					stats.rewritten_reductions++;
				}
				continue;
			}
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
