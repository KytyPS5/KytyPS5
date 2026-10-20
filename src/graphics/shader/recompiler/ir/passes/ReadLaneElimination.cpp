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

struct Reduction {
	ValueOpcode fold;
	uint32_t    identity;
	Value       lhs;
	Value       rhs;
};

std::optional<Reduction> ReductionOf(const Inst& inst) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::IAdd32:
		case ValueOpcode::UMax32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
			return Reduction {inst.GetOpcode(), 0u, inst.Arg(0), inst.Arg(1)};
		case ValueOpcode::UMin32:
		case ValueOpcode::BitwiseAnd32:
			return Reduction {inst.GetOpcode(), UINT32_MAX, inst.Arg(0), inst.Arg(1)};
		case ValueOpcode::SMin32:
			return Reduction {inst.GetOpcode(), 0x7fffffffu, inst.Arg(0), inst.Arg(1)};
		case ValueOpcode::SMax32:
			return Reduction {inst.GetOpcode(), 0x80000000u, inst.Arg(0), inst.Arg(1)};
		case ValueOpcode::CompositeExtractU32x2: {
			const auto* sum = inst.Arg(0).Resolve().TryInstruction();
			if (inst.Arg(1).Resolve() != Value(0u) || sum == nullptr ||
			    sum->GetOpcode() != ValueOpcode::IAddCarry32) {
				return std::nullopt;
			}
			return Reduction {ValueOpcode::IAdd32, 0u, sum->Arg(0), sum->Arg(1)};
		}
		default: return std::nullopt;
	}
}

// A full-EXEC prefix scan with both half-wave endpoint reads is a reduction.
// Fragment subgroups may have no physical invocation at either endpoint.
class WaveReductionMatcher {
public:
	WaveReductionMatcher(Block* block, ValueOpcode fold, uint32_t identity)
	    : block(block), fold(fold), identity(identity) {}

	Value Match(Value lhs, Value rhs) {
		const auto* low  = Get(lhs, ValueOpcode::ReadLane);
		const auto* high = Get(rhs, ValueOpcode::ReadLane);
		if (low == nullptr || high == nullptr || low->Arg(0).Resolve() != high->Arg(0).Resolve()) {
			return {};
		}
		const auto lane = [](Value value) {
			value = value.Resolve();
			return value.IsImmediate() && value.GetType() == Type::U32 ? value.U32() % 64u : 64u;
		};
		const auto first  = lane(low->Arg(1));
		const auto second = lane(high->Arg(1));
		if (first == 64u || second == 64u || (first < 32u) == (second < 32u)) return {};
		const bool  scan_ends = std::min(first, second) == 31u && std::max(first, second) == 63u;
		const auto* joined    = Get(Unwrap(low->Arg(0)), fold);
		if (joined == nullptr) return {};
		for (const uint32_t row_control: {RowShr, RowXmask}) {
			if (row_control == RowShr && !scan_ends) continue;
			for (size_t index = 0; index < 2; index++) {
				auto        attempt = *this;
				const auto* cross =
				    attempt.Get(attempt.Unwrap(joined->Arg(index)), ValueOpcode::Permlane16U32);
				if (cross == nullptr) continue;
				const auto flags  = cross->Flags<PermlaneFlags>();
				auto       prefix = attempt.Unwrap(joined->Arg(1 - index));
				if (!flags.x16 || flags.fetch_inactive || !flags.bound_control ||
				    cross->Arg(1).Resolve() != Value(UINT32_MAX) ||
				    cross->Arg(2).Resolve() != Value(UINT32_MAX) ||
				    !attempt.FullExec(cross->Arg(3)) || attempt.Unwrap(cross->Arg(0)) != prefix) {
					continue;
				}
				for (const uint32_t shift: {8u, 4u, 2u, 1u}) {
					prefix = attempt.MatchRow(prefix, row_control + shift);
				}
				prefix = attempt.Unwrap(prefix);
				if (prefix.IsEmpty()) break;
				if (attempt.saved_exec.IsEmpty()) return prefix;
				const auto* seed = attempt.Get(prefix, ValueOpcode::SelectU32);
				if (seed != nullptr && seed->Arg(2).Resolve() == Value(identity)) {
					const auto  predicate = seed->Arg(0).Resolve();
					const auto* subset    = attempt.Get(predicate, ValueOpcode::LogicalAnd);
					if (predicate == attempt.saved_exec ||
					    (subset != nullptr && (subset->Arg(0).Resolve() == attempt.saved_exec ||
					                           subset->Arg(1).Resolve() == attempt.saved_exec))) {
						return prefix;
					}
				}
			}
		}
		return {};
	}

private:
	static constexpr uint32_t RowShr   = 0x110u;
	static constexpr uint32_t RowXmask = 0x160u;

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

	static bool IsRowStep(const Inst& inst, uint32_t control) {
		const auto flags = inst.Flags<DppMoveFlags>();
		return flags.control == control && flags.row_mask == 0xf && flags.bank_mask == 0xf &&
		       !flags.fetch_inactive && !flags.bound_control && !flags.dpp8;
	}

	Value MatchRow(Value value, uint32_t control) {
		const auto* update = Get(Unwrap(value), ValueOpcode::DppUpdateU32);
		if (update == nullptr || !IsRowStep(*update, control) || !FullExec(update->Arg(2)))
			return {};
		const auto* combined = Get(update->Arg(0), fold);
		if (combined == nullptr) return {};
		for (size_t index = 0; index < 2; index++) {
			const auto* move = Get(combined->Arg(index), ValueOpcode::DppMoveU32);
			if (move == nullptr || !IsRowStep(*move, control) || !FullExec(move->Arg(1))) continue;
			const auto previous = Unwrap(combined->Arg(1 - index));
			if (Unwrap(move->Arg(0)) == previous && Unwrap(update->Arg(1)) == previous) return previous;
		}
		return {};
	}

	Block*      block;
	ValueOpcode fold;
	uint32_t    identity;
	Value       saved_exec;
};

} // namespace

ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size) {
	ReadLaneStats stats;
	if (wave_size != 32u && wave_size != 64u) {
		return stats;
	}

	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			const auto reduction = wave_size == 64u ? ReductionOf(inst) : std::nullopt;
			if (reduction) {
				const auto seed = WaveReductionMatcher(block, reduction->fold, reduction->identity)
				                      .Match(reduction->lhs, reduction->rhs);
				if (!seed.IsEmpty()) {
					const auto position = std::find_if(block->begin(), block->end(),
					                                  [&](const Inst& candidate) { return &candidate == &inst; });
					auto&      subgroup =
					    *block->PrependNewInst(position, ValueOpcode::SubgroupReduceU32, {seed});
					subgroup.SetFlags(reduction->fold);
					inst.ReplaceUsesWith(Value(&subgroup));
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
