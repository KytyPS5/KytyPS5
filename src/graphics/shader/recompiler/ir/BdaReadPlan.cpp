#include "graphics/shader/recompiler/ir/BdaReadPlan.h"

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <optional>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {
std::optional<uint32_t> Immediate(Value value) {
	value = value.Resolve();
	if (value.IsImmediate() && value.GetType() == Type::U32) return value.U32();
	return {};
}

bool Implies(Value predicate, Value condition, unsigned& budget, unsigned depth = 0) {
	if (budget == 0) return false;
	--budget;
	predicate = predicate.Resolve();
	condition = condition.Resolve();
	if (predicate == condition) return true;
	const auto* inst = predicate.TryInstruction();
	if (depth >= 12 || !inst || inst->GetOpcode() != ValueOpcode::LogicalAnd) return false;
	return Implies(inst->Arg(0), condition, budget, depth + 1) ||
	       Implies(inst->Arg(1), condition, budget, depth + 1);
}

uint32_t PredicateUpper(Value value, Value predicate, unsigned& budget, unsigned depth = 0) {
	if (budget == 0) return UINT32_MAX;
	--budget;
	value            = value.Resolve();
	predicate        = predicate.Resolve();
	const auto* inst = predicate.TryInstruction();
	if (depth >= 12 || !inst) return UINT32_MAX;
	const auto op = inst->GetOpcode();
	if (op == ValueOpcode::LogicalAnd)
		return std::min(PredicateUpper(value, inst->Arg(0), budget, depth + 1),
		                PredicateUpper(value, inst->Arg(1), budget, depth + 1));
	if (op == ValueOpcode::ULessThan32 && inst->Arg(0).Resolve() == value) {
		const auto limit = Immediate(inst->Arg(1));
		if (limit && *limit) return *limit - 1;
	}
	return UINT32_MAX;
}

struct Interval {
	uint32_t low = 0, high = UINT32_MAX;
};
Interval Bound(Value value, Value predicate, unsigned& budget, unsigned depth = 0) {
	if (budget == 0) return {};
	--budget;
	value = value.Resolve();
	if (const auto imm = Immediate(value)) return {*imm, *imm};
	Interval    result;
	const auto* inst = value.TryInstruction();
	if (depth < 24 && inst) {
		const auto arg = [&](unsigned i) {
			return Bound(inst->Arg(i), predicate, budget, depth + 1);
		};
		switch (inst->GetOpcode()) {
			case ValueOpcode::BitwiseAnd32: {
				const auto a = arg(0), b = arg(1);
				result.high = std::min(a.high, b.high);
				break;
			}
			case ValueOpcode::UMin32: {
				const auto a = arg(0), b = arg(1);
				result = {std::min(a.low, b.low), std::min(a.high, b.high)};
				break;
			}
			case ValueOpcode::IAdd32:
			case ValueOpcode::IMul32: {
				const auto     a = arg(0), b = arg(1);
				const bool     add  = inst->GetOpcode() == ValueOpcode::IAdd32;
				const uint64_t high = add ? uint64_t(a.high) + b.high : uint64_t(a.high) * b.high;
				if (high <= UINT32_MAX)
					result = {add ? a.low + b.low : a.low * b.low, static_cast<uint32_t>(high)};
				break; // Wrapping arithmetic conservatively covers every U32.
			}
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftRightLogical32: {
				const auto shift = Immediate(inst->Arg(1));
				if (!shift || *shift >= 32) break;
				const auto a = arg(0);
				if (inst->GetOpcode() == ValueOpcode::ShiftRightLogical32)
					result = {a.low >> *shift, a.high >> *shift};
				else if ((uint64_t(a.high) << *shift) <= UINT32_MAX)
					result = {a.low << *shift, a.high << *shift};
				else
					result.high = UINT32_MAX << *shift;
				break;
			}
			case ValueOpcode::SelectU32:
				if (Implies(predicate, inst->Arg(0), budget))
					result = arg(1);
				else {
					const auto a = arg(1), b = arg(2);
					result = {std::min(a.low, b.low), std::max(a.high, b.high)};
				}
				break;
			default: break;
		}
	}
	result.high = std::min(result.high, PredicateUpper(value, predicate, budget));
	// Contradictory predicates have no active loads. Retaining a broad interval
	// avoids relying on that fact or adding a separate empty-interval convention.
	if (result.low > result.high) return {};
	return result;
}

std::optional<BdaReadWord> ReadWord(const Program& program, Value value) {
	value = ResolveInvariantPhi(program, value);
	if (const auto immediate = Immediate(value))
		return BdaReadWord {BdaReadWord::Kind::Immediate, *immediate};
	const auto* inst = value.TryInstruction();
	if (!inst) return {};
	if (inst->GetOpcode() == ValueOpcode::ReadConst && inst->NumArgs() == 2) {
		const auto slot = Immediate(inst->Arg(1));
		if (slot) return BdaReadWord {BdaReadWord::Kind::Flat, *slot};
	}
	if (inst->GetOpcode() == ValueOpcode::GetUserData && inst->NumArgs() == 1) {
		const uint32_t reg = RegIndex(inst->Arg(0).ScalarRegister());
		if (reg >= program.user_data_base && reg - program.user_data_base < program.user_data_count)
			return BdaReadWord {BdaReadWord::Kind::UserData, reg - program.user_data_base};
	}
	return {};
}
} // namespace

BdaReadPlan BuildBdaReadPlan(const Program& program) {
	BdaReadPlan result;
	if (!program.info.uses_dma) return result;
	for (const auto* block: program.blocks)
		for (const auto& inst: block->Instructions()) {
			const auto address = AddressOpcodeInfoOf(inst.GetOpcode());
			if (address.access == AddressAccess::None) continue;
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) return {};
			const auto& memory = program.memory_info[index];
			if (memory.planning_only) {
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32) continue;
				return {};
			}
			if (memory.kind == ResourceKind::Scratch) continue;
			if (!IsAddressResourceKind(memory.kind)) return {};
			if (address.access != AddressAccess::Read || inst.NumArgs() != 4 ||
			    (address.data_bits != 8 && address.data_bits != 16 && address.data_bits != 32))
				return {};
			const auto* handle = inst.Arg(0).Resolve().TryInstruction();
			if (!handle || handle->GetOpcode() != ValueOpcode::GetAddressResource ||
			    handle->NumArgs() != 2)
				return {};
			const auto low =
			    ReadWord(program, memory.address_is_full ? inst.Arg(1) : handle->Arg(0));
			const auto high =
			    ReadWord(program, memory.address_is_full ? inst.Arg(2) : handle->Arg(1));
			if (!low || !high) return {};
			const bool scalar = memory.kind == ResourceKind::ScalarAddress;
			if (scalar && memory.address_is_full) return {};
			// A shared DAG may expand exponentially when recursively revisited. An
			// exhausted work budget only widens the interval; it never drops a load.
			unsigned budget = 4096;
			auto     offset =
                memory.address_is_full ? Interval {0, 0} : Bound(inst.Arg(1), inst.Arg(3), budget);
			int64_t immediate = static_cast<int32_t>(memory.offset);
			if (scalar) {
				offset.low &= ~3u;
				offset.high &= ~3u;
				immediate = static_cast<int32_t>(memory.offset & ~3u);
			}
			BdaReadSpan span {*low, *high, int64_t(offset.low) + immediate,
			                  int64_t(offset.high) + immediate + address.data_bits / 8, scalar};
			// Union accesses sharing an invariant pointer; gaps may be uploaded but
			// cannot remove a required byte. This also bounds runtime metadata size.
			const auto found = std::ranges::find_if(result.spans, [&](const auto& old) {
				return old.low == span.low && old.high == span.high &&
				       old.scalar_base == span.scalar_base;
			});
			if (found != result.spans.end()) {
				found->begin = std::min(found->begin, span.begin);
				found->end   = std::max(found->end, span.end);
			} else {
				if (result.spans.size() == BdaReadPlan::Capacity) return {};
				result.spans.push_back(span);
			}
		}
	result.complete = !result.spans.empty();
	return result;
}

bool EvaluateBdaReadPlan(const BdaReadPlan& plan, const ResourceSnapshot& snapshot,
                         std::vector<GuestRange>& output) {
	if (!plan.complete || plan.spans.empty() || plan.spans.size() > BdaReadPlan::Capacity)
		return false;
	// (Reused per thread: a dispatch's or table draw's reads are evaluated at every use.)
	thread_local std::vector<GuestRange> ranges;
	ranges.clear();
	const auto word = [&](BdaReadWord source, uint32_t& value) {
		if (source.kind == BdaReadWord::Kind::Immediate) {
			value = source.value;
			return true;
		}
		if (source.kind != BdaReadWord::Kind::Flat && source.kind != BdaReadWord::Kind::UserData)
			return false;
		const auto& values =
		    source.kind == BdaReadWord::Kind::Flat ? snapshot.flattened_srt : snapshot.user_data;
		if (source.value >= values.size()) return false;
		value = values[source.value];
		return true;
	};
	for (const auto& span: plan.spans) {
		uint32_t low, high;
		if (!word(span.low, low) || !word(span.high, high) || span.end <= span.begin) return false;
		uint64_t base = uint64_t(low) | uint64_t(high) << 32u;
		if (span.scalar_base) base &= 0x0000fffffffffffcULL;
		const auto add = [&](int64_t offset, uint64_t& value) {
			if (offset < 0) {
				const uint64_t magnitude = uint64_t(-(offset + 1)) + 1;
				if (base < magnitude) return false;
				value = base - magnitude;
			} else {
				if (uint64_t(offset) > UINT64_MAX - base) return false;
				value = base + uint64_t(offset);
			}
			return true;
		};
		uint64_t begin, end;
		if (!add(span.begin, begin) || !add(span.end, end) || end > TRACKER_ADDRESS_SIZE)
			return false;
		// The emitter implements unaligned loads with aligned U32 reads.
		begin &= ~uint64_t {3};
		end = (end + 3u) & ~uint64_t {3};
		GuestRange range {begin, end - begin};
		if (!range.Valid()) return false;
		ranges.push_back(range);
	}
	std::ranges::sort(ranges);
	output.clear();
	for (const auto range: ranges) {
		if (!output.empty() && range.address <= output.back().End())
			output.back().size = std::max(output.back().End(), range.End()) - output.back().address;
		else
			output.push_back(range);
	}
	return true;
}
} // namespace Libs::Graphics::ShaderRecompiler::IR
