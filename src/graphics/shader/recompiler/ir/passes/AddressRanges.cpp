#include "graphics/shader/recompiler/ir/passes/AddressRanges.h"

#include <algorithm>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

using Term   = AddressRange::Term;
using Source = AddressRange::Source;

struct Known {
	Source   source = Source::None;
	uint32_t word   = 0;

	bool operator==(const Known& other) const = default;
};

// [lo, hi] + scale * term + value_scale * value; `exact` while every value adds the value term.
struct Offset {
	uint64_t lo          = 0;
	uint64_t hi          = 0;
	Term     term        = Term::None;
	uint64_t scale       = 0;
	Known    value       = {};
	uint64_t value_scale = 0;
	bool     exact       = true;

	[[nodiscard]] bool HasTerms() const {
		return term != Term::None || value.source != Source::None;
	}
};

constexpr uint64_t ValueLimit = uint64_t {1} << 32u;
constexpr uint32_t MaxDepth   = 64;

[[nodiscard]] std::optional<Offset> Bounded(uint64_t lo, uint64_t hi) {
	return Offset {.lo = lo, .hi = hi};
}

[[nodiscard]] std::optional<Term> BuiltinTerm(uint32_t kind, uint32_t component) {
	const auto axis = [&](Term first) -> std::optional<Term> {
		return component < 3u
		           ? std::optional(static_cast<Term>(static_cast<uint32_t>(first) + component))
		           : std::nullopt;
	};
	switch (static_cast<StageInputKind>(kind)) {
		case StageInputKind::InstanceIndex: return Term::InstanceIndex;
		case StageInputKind::GlobalInvocationId: return axis(Term::GlobalInvocationX);
		case StageInputKind::WorkgroupId: return axis(Term::WorkgroupX);
		default: return std::nullopt;
	}
}

// Terms are never negative, so the larger multiple bounds both arms.
[[nodiscard]] std::optional<Offset> Union(const Offset& a, const Offset& b) {
	if (a.term != Term::None && b.term != Term::None && a.term != b.term) {
		return std::nullopt;
	}
	if (a.value.source != Source::None && b.value.source != Source::None && a.value != b.value) {
		return std::nullopt;
	}
	return Offset {
	    .lo          = std::min(a.lo, b.lo),
	    .hi          = std::max(a.hi, b.hi),
	    .term        = a.term != Term::None ? a.term : b.term,
	    .scale       = std::max(a.scale, b.scale),
	    .value       = a.value.source != Source::None ? a.value : b.value,
	    .value_scale = std::max(a.value_scale, b.value_scale),
	    .exact       = a.exact && b.exact && a.value == b.value && a.value_scale == b.value_scale,
	};
}

[[nodiscard]] std::optional<Offset> Scale(Offset a, uint64_t factor) {
	if (a.hi * factor >= ValueLimit || (a.HasTerms() && factor >= ValueLimit)) {
		return std::nullopt;
	}
	a.lo *= factor;
	a.hi *= factor;
	a.scale *= factor;
	a.value_scale *= factor;
	return a;
}

class Analyzer {
public:
	// Lanes without the load's active flag do not load, so a select on it needs only its true arm.
	void SetContext(const Value& active, const Block* block) {
		const auto resolved = active.Resolve();
		m_active            = resolved.IsImmediate() ? nullptr : resolved.TryInstruction();
		m_block             = block;
	}

	[[nodiscard]] std::optional<Offset> Of(const Value& value, uint32_t depth = 0) {
		const auto resolved = value.Resolve();
		if (resolved.IsImmediate()) {
			return Bounded(resolved.U32(), resolved.U32());
		}
		const auto* inst = resolved.TryInstruction();
		if (inst == nullptr || depth > MaxDepth) {
			return std::nullopt;
		}
		const auto key = std::tuple {inst, m_active, m_block};
		if (const auto found = m_memo.find(key); found != m_memo.end()) {
			return found->second;
		}
		if (!m_visiting.insert(inst).second) {
			return std::nullopt;
		}
		const auto result = Compute(*inst, depth + 1);
		m_visiting.erase(inst);
		m_memo.emplace(key, result);
		return result;
	}

private:
	[[nodiscard]] static std::optional<uint32_t> Immediate(const Value& value) {
		const auto resolved = value.Resolve();
		return resolved.IsImmediate() && resolved.GetType() == Type::U32
		           ? std::optional(resolved.U32())
		           : std::nullopt;
	}

	// A bound on `var` (or `next` = var + step) implied by `condition` == `positive`.
	[[nodiscard]] std::optional<uint64_t> Implied(const Value& condition, bool positive,
	                                              const Inst* var, const Inst* next, uint64_t step,
	                                              bool allow_signed, uint32_t depth) const {
		const auto* test = condition.Resolve().TryInstruction();
		if (test == nullptr || depth > MaxDepth) {
			return std::nullopt;
		}
		const auto both = [&](bool side_positive) {
			const auto a =
			    Implied(test->Arg(0), side_positive, var, next, step, allow_signed, depth + 1);
			const auto b =
			    Implied(test->Arg(1), side_positive, var, next, step, allow_signed, depth + 1);
			return a && b ? std::optional(std::min(*a, *b)) : (a ? a : b);
		};
		switch (test->GetOpcode()) {
			case ValueOpcode::ConditionRef:
				return Implied(test->Arg(0), positive, var, next, step, allow_signed, depth + 1);
			case ValueOpcode::LogicalNot:
				return Implied(test->Arg(0), !positive, var, next, step, allow_signed, depth + 1);
			case ValueOpcode::LogicalAnd: return positive ? both(true) : std::nullopt;
			case ValueOpcode::LogicalOr: return positive ? std::nullopt : both(false);
			default: break;
		}
		bool is_signed = false;
		bool strict    = false;
		bool swap      = false;
		switch (test->GetOpcode()) {
			case ValueOpcode::SLessThan32: is_signed = true; [[fallthrough]];
			case ValueOpcode::ULessThan32:
				strict = positive;
				swap   = !positive;
				break;
			case ValueOpcode::SLessThanEqual32: is_signed = true; [[fallthrough]];
			case ValueOpcode::ULessThanEqual32:
				strict = !positive;
				swap   = !positive;
				break;
			case ValueOpcode::SGreaterThan32: is_signed = true; [[fallthrough]];
			case ValueOpcode::UGreaterThan32:
				strict = positive;
				swap   = positive;
				break;
			case ValueOpcode::SGreaterThanEqual32: is_signed = true; [[fallthrough]];
			case ValueOpcode::UGreaterThanEqual32:
				strict = !positive;
				swap   = positive;
				break;
			default: return std::nullopt;
		}
		if (is_signed && !allow_signed) {
			return std::nullopt;
		}
		const auto lhs   = test->Arg(swap ? 1 : 0).Resolve();
		const auto limit = Immediate(test->Arg(swap ? 0 : 1));
		if (!limit || (is_signed && *limit >= (ValueLimit >> 1u))) {
			return std::nullopt;
		}
		if (strict && *limit == 0) {
			return std::nullopt;
		}
		const uint64_t bound = strict ? *limit - 1u : *limit;
		const auto*    x     = lhs.TryInstruction();
		if (x == var) {
			return bound;
		}
		if (x == next && next != nullptr) {
			return bound >= step ? std::optional(bound - step) : std::nullopt;
		}
		return std::nullopt;
	}

	// Tests on the edge from -> to and on every path into `from`, up single predecessors.
	[[nodiscard]] std::optional<uint64_t> GuardBound(const Block* from, const Block* to,
	                                                 const Inst* var, const Inst* next,
	                                                 uint64_t step, bool allow_signed) const {
		std::optional<uint64_t> result;
		const auto              take = [&](const Block* block, const Block* target) {
			const auto& terminator = block->terminator;
			if (terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
			    terminator.true_block == terminator.false_block ||
			    (terminator.true_block != target && terminator.false_block != target)) {
				return;
			}
			const auto bound = Implied(block->condition, terminator.true_block == target, var, next,
			                           step, allow_signed, 0);
			if (bound) {
				result = result ? std::min(*result, *bound) : bound;
			}
		};
		if (to != nullptr) {
			take(from, to);
		}
		const auto* current = from;
		for (uint32_t steps = 0; steps < MaxDepth && current != nullptr; ++steps) {
			const auto predecessors = current->ImmPredecessors();
			if (predecessors.size() != 1) {
				break;
			}
			take(predecessors[0], current);
			current = predecessors[0];
		}
		return result;
	}

	// phi(init, phi + step), bounded by its back edge's tests and by those guarding this use.
	[[nodiscard]] std::optional<Offset> Induction(const Inst& phi, uint32_t depth) {
		if (phi.NumArgs() != 2) {
			return std::nullopt;
		}
		for (uint32_t back = 0; back < 2; ++back) {
			const auto* next = phi.Arg(back).Resolve().TryInstruction();
			if (next == nullptr || next->GetOpcode() != ValueOpcode::IAdd32) {
				continue;
			}
			std::optional<uint32_t> step;
			for (uint32_t side = 0; side < 2; ++side) {
				if (next->Arg(side).Resolve().TryInstruction() == &phi) {
					step = Immediate(next->Arg(side ^ 1u));
				}
			}
			const auto init = Of(phi.Arg(back ^ 1u), depth);
			if (!step || *step == 0 || *step >= (ValueLimit >> 1u) || !init || init->HasTerms() ||
			    init->hi >= (ValueLimit >> 1u)) {
				continue;
			}
			const auto* latch  = phi.PhiBlock(back);
			const auto* header = phi.Parent();
			// Bounded below 2^31 by its back edge, the counter cannot wrap, so signed tests count.
			const auto              on_back = GuardBound(latch, header, &phi, next, *step, true);
			std::optional<uint64_t> hi;
			if (on_back && *on_back + *step < (ValueLimit >> 1u)) {
				hi = std::max(init->hi, *on_back + *step);
			}
			const auto at_use = GuardBound(m_block, nullptr, &phi, next, *step, hi.has_value());
			if (at_use) {
				hi = hi ? std::min(*hi, std::max(*at_use, init->lo)) : std::max(*at_use, init->lo);
			}
			if (hi) {
				return Bounded(init->lo, *hi);
			}
		}
		return std::nullopt;
	}

	[[nodiscard]] std::optional<Offset> Compute(const Inst& inst, uint32_t depth) {
		switch (inst.GetOpcode()) {
			case ValueOpcode::Identity:
			case ValueOpcode::ReadFirstLane: return Of(inst.Arg(0), depth);
			case ValueOpcode::GetBuiltin: {
				const auto kind      = Immediate(inst.Arg(0));
				const auto component = Immediate(inst.Arg(1));
				const auto term = kind && component ? BuiltinTerm(*kind, *component) : std::nullopt;
				return term ? std::optional(Offset {.term = *term, .scale = 1}) : std::nullopt;
			}
			case ValueOpcode::ReadConst: {
				const auto slot = Immediate(inst.Arg(1));
				return slot
				           ? std::optional(Offset {.value = {Source::Srt, *slot}, .value_scale = 1})
				           : std::nullopt;
			}
			case ValueOpcode::GetUserData: {
				if (inst.Arg(0).GetType() != Type::ScalarReg) {
					return std::nullopt;
				}
				const auto reg = static_cast<uint32_t>(RegIndex(inst.Arg(0).ScalarRegister()));
				return Offset {.value = {Source::UserData, reg}, .value_scale = 1};
			}
			case ValueOpcode::IAdd32: {
				const auto a = Of(inst.Arg(0), depth);
				const auto b = Of(inst.Arg(1), depth);
				if (!a || !b ||
				    (a->term != Term::None && b->term != Term::None && a->term != b->term) ||
				    (a->value.source != Source::None && b->value.source != Source::None &&
				     a->value != b->value) ||
				    a->hi + b->hi >= ValueLimit || a->scale + b->scale >= ValueLimit ||
				    a->value_scale + b->value_scale >= ValueLimit) {
					return std::nullopt;
				}
				return Offset {
				    .lo          = a->lo + b->lo,
				    .hi          = a->hi + b->hi,
				    .term        = a->term != Term::None ? a->term : b->term,
				    .scale       = a->scale + b->scale,
				    .value       = a->value.source != Source::None ? a->value : b->value,
				    .value_scale = a->value_scale + b->value_scale,
				    .exact       = a->exact && b->exact,
				};
			}
			case ValueOpcode::IMul32: {
				for (uint32_t side = 0; side < 2; ++side) {
					if (const auto factor = Immediate(inst.Arg(side))) {
						const auto other = Of(inst.Arg(side ^ 1u), depth);
						return other ? Scale(*other, *factor) : std::nullopt;
					}
				}
				return std::nullopt;
			}
			case ValueOpcode::ShiftLeftLogical32: {
				const auto shift = Immediate(inst.Arg(1));
				const auto a     = Of(inst.Arg(0), depth);
				return a && shift && *shift < 32u ? Scale(*a, uint64_t {1} << *shift)
				                                  : std::nullopt;
			}
			case ValueOpcode::ShiftRightLogical32: {
				const auto shift = Immediate(inst.Arg(1));
				if (!shift || *shift >= 32u) {
					return std::nullopt;
				}
				const auto a = Of(inst.Arg(0), depth);
				if (a && !a->HasTerms()) {
					return Bounded(a->lo >> *shift, a->hi >> *shift);
				}
				return Bounded(0, (ValueLimit - 1) >> *shift);
			}
			case ValueOpcode::BitwiseAnd32: {
				for (uint32_t side = 0; side < 2; ++side) {
					if (const auto mask = Immediate(inst.Arg(side))) {
						const auto other = Of(inst.Arg(side ^ 1u), depth);
						const auto hi    = other && !other->HasTerms()
						                       ? std::min<uint64_t>(*mask, other->hi)
						                       : *mask;
						return Bounded(0, hi);
					}
				}
				return std::nullopt;
			}
			case ValueOpcode::BitFieldUExtract: {
				const auto width = Immediate(inst.Arg(2));
				return width && *width < 32u ? Bounded(0, (uint64_t {1} << *width) - 1u)
				                             : std::nullopt;
			}
			case ValueOpcode::UMin32: {
				std::optional<uint64_t> hi;
				for (uint32_t side = 0; side < 2; ++side) {
					const auto a = Of(inst.Arg(side), depth);
					if (a && !a->HasTerms()) {
						hi = hi ? std::min(*hi, a->hi) : a->hi;
					}
				}
				return hi ? Bounded(0, *hi) : std::nullopt;
			}
			case ValueOpcode::SelectU32: {
				if (m_active != nullptr && inst.Arg(0).Resolve().TryInstruction() == m_active) {
					return Of(inst.Arg(1), depth);
				}
				const auto a = Of(inst.Arg(1), depth);
				const auto b = Of(inst.Arg(2), depth);
				return a && b ? Union(*a, *b) : std::nullopt;
			}
			case ValueOpcode::Phi: {
				if (const auto counter = Induction(inst, depth)) {
					return counter;
				}
				std::optional<Offset> result;
				for (size_t i = 0; i < inst.NumArgs(); ++i) {
					const auto a = Of(inst.Arg(i), depth);
					result       = a ? (result ? Union(*result, *a) : a) : std::nullopt;
					if (!result) {
						return std::nullopt;
					}
				}
				return result;
			}
			default: return std::nullopt;
		}
	}

	std::map<std::tuple<const Inst*, const Inst*, const Block*>, std::optional<Offset>> m_memo;
	std::unordered_set<const Inst*>                                                     m_visiting;
	const Inst*  m_active = nullptr;
	const Block* m_block  = nullptr;
};

[[nodiscard]] std::optional<Known> BaseWord(const Value& value, uint32_t* constant) {
	const auto resolved = value.Resolve();
	if (resolved.IsImmediate()) {
		*constant = resolved.U32();
		return Known {Source::Constant, 0};
	}
	const auto* inst = resolved.TryInstruction();
	if (inst == nullptr) {
		return std::nullopt;
	}
	if (inst->GetOpcode() == ValueOpcode::ReadConst) {
		const auto slot = inst->Arg(1).Resolve();
		return slot.IsImmediate() ? std::optional(Known {Source::Srt, slot.U32()}) : std::nullopt;
	}
	if (inst->GetOpcode() == ValueOpcode::GetUserData &&
	    inst->Arg(0).GetType() == Type::ScalarReg) {
		return Known {Source::UserData,
		              static_cast<uint32_t>(RegIndex(inst->Arg(0).ScalarRegister()))};
	}
	return std::nullopt;
}

[[nodiscard]] bool ResolveBase(const Value& handle_value, AddressRange& range) {
	const auto* handle = handle_value.Resolve().TryInstruction();
	if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource ||
	    handle->NumArgs() != 2) {
		return false;
	}
	std::array<uint32_t, 2> constants {};
	const auto              low  = BaseWord(handle->Arg(0), &constants[0]);
	const auto              high = BaseWord(handle->Arg(1), &constants[1]);
	if (!low || !high || low->source != high->source) {
		return false;
	}
	range.base_source = low->source;
	if (low->source == Source::Constant) {
		range.base = uint64_t {constants[0]} | (uint64_t {constants[1]} << 32u);
	} else {
		range.base_words = {low->word, high->word};
	}
	return true;
}

[[nodiscard]] uint32_t LoadBytes(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::LoadAddressU8: return 1;
		case ValueOpcode::LoadAddressU16: return 2;
		case ValueOpcode::LoadAddressU32: return 4;
		default: return 0;
	}
}

} // namespace

void CollectAddressRanges(Program& program) {
	auto& ranges = program.info.address_ranges;
	ranges.clear();
	Analyzer analyzer;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto bytes = LoadBytes(inst.GetOpcode());
			if (bytes == 0) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) {
				continue;
			}
			const auto& memory = program.memory_info[index];
			const bool  scalar = memory.kind == ResourceKind::ScalarAddress;
			if (memory.planning_only || memory.address_is_full ||
			    (!scalar && memory.kind != ResourceKind::Global &&
			     memory.kind != ResourceKind::Flat)) {
				continue;
			}
			AddressRange range;
			range.align_base = scalar;
			if (!ResolveBase(inst.Arg(0), range)) {
				continue;
			}
			analyzer.SetContext(inst.Arg(3), block);
			const auto offset = analyzer.Of(inst.Arg(1));
			if (!offset) {
				continue;
			}
			auto immediate = static_cast<int64_t>(static_cast<int32_t>(memory.offset));
			auto lo        = offset->lo;
			if (scalar) {
				immediate &= ~int64_t {3};
				lo &= ~uint64_t {3};
			}
			range.begin        = static_cast<int64_t>(lo) + immediate;
			range.end          = static_cast<int64_t>(offset->hi) + immediate + bytes;
			range.term         = offset->term;
			range.scale        = offset->scale;
			range.offset_max   = offset->hi;
			range.value_source = offset->value.source;
			range.value_word   = offset->value.word;
			range.value_scale  = offset->value_scale;
			range.value_exact  = offset->exact && offset->value.source != Source::None;
			const auto same    = std::ranges::find_if(ranges, [&](const AddressRange& other) {
				return other.base_source == range.base_source &&
				       other.base_words == range.base_words && other.base == range.base &&
				       other.align_base == range.align_base && other.term == range.term &&
				       other.scale == range.scale && other.value_source == range.value_source &&
				       other.value_word == range.value_word &&
				       other.value_scale == range.value_scale;
			});
			if (same == ranges.end()) {
				ranges.push_back(range);
			} else {
				same->begin       = std::min(same->begin, range.begin);
				same->end         = std::max(same->end, range.end);
				same->offset_max  = std::max(same->offset_max, range.offset_max);
				same->value_exact = same->value_exact && range.value_exact;
			}
		}
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
