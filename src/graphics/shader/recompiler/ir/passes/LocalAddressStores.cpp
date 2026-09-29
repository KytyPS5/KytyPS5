#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"

#include <algorithm>
#include <bit>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct Interval {
	uint32_t lo, hi;
};
using Ranges = std::vector<Interval>;

Ranges Unknown() {
	return {{0, UINT32_MAX}};
}

Ranges Normalize(Ranges ranges) {
	std::sort(ranges.begin(), ranges.end(),
	          [](const Interval& a, const Interval& b) { return a.lo < b.lo; });
	Ranges result;
	for (const auto range: ranges) {
		if (!result.empty() && uint64_t {range.lo} <= uint64_t {result.back().hi} + 1u) {
			result.back().hi = std::max(result.back().hi, range.hi);
		} else {
			result.push_back(range);
		}
	}
	return result.size() > 8 ? Unknown() : result;
}

bool Is(Value value, uint32_t immediate) {
	value = value.Resolve();
	return value.IsImmediate() && value.GetType() == Type::U32 && value.U32() == immediate;
}

const Inst* Op(Value value, ValueOpcode opcode) {
	const auto* inst = value.Resolve().TryInstruction();
	return inst != nullptr && inst->GetOpcode() == opcode ? inst : nullptr;
}

Value Other(const Inst& inst, uint32_t immediate) {
	if (Is(inst.Arg(0), immediate)) return inst.Arg(1).Resolve();
	if (Is(inst.Arg(1), immediate)) return inst.Arg(0).Resolve();
	return {};
}

// Only recover the current lane's bit of a wave32 ballot. Reading another lane,
// another ballot word, or a wave64 mask does not establish this lane's predicate.
Value BallotPredicate(Value value) {
	const auto* compare = Op(value, ValueOpcode::INotEqual32);
	if (!compare) return {};
	const auto* bit = Op(Other(*compare, 0), ValueOpcode::BitwiseAnd32);
	if (!bit) return {};
	const auto* shift = Op(Other(*bit, 1), ValueOpcode::ShiftRightLogical32);
	if (!shift) return {};
	const auto* lane = Op(shift->Arg(1), ValueOpcode::BitwiseAnd32);
	if (!lane || !Op(Other(*lane, 31), ValueOpcode::LaneId)) return {};
	const auto* word = Op(shift->Arg(0), ValueOpcode::CompositeExtractU32x4);
	if (!word || !Is(word->Arg(1), 0)) return {};
	const auto* ballot = Op(word->Arg(0), ValueOpcode::Ballot);
	return ballot ? ballot->Arg(0).Resolve() : Value {};
}

struct Evaluator {
	const std::unordered_set<const Inst*>&  loop_phis;
	std::unordered_set<const Inst*>         truth;
	std::unordered_map<const Inst*, Ranges> bounds, cache;
	std::unordered_set<const Inst*>         visiting;

	void Assume(Value value, uint32_t depth = 0) {
		const auto* inst = value.Resolve().TryInstruction();
		if (!inst || depth > 64 || !truth.insert(inst).second) return;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			Assume(inst->Arg(0), depth + 1);
			Assume(inst->Arg(1), depth + 1);
		}
		if (const auto restored = BallotPredicate(value); !restored.IsEmpty()) {
			Assume(restored, depth + 1);
		}
		if (inst->GetOpcode() != ValueOpcode::SLessThan32) return;
		const auto limit = inst->Arg(1).Resolve();
		if (!limit.IsImmediate() || limit.GetType() != Type::U32 || limit.U32() == 0 ||
		    limit.U32() > 0x7fffu)
			return;
		const auto* signed_half = Op(inst->Arg(0), ValueOpcode::BitFieldSExtract);
		if (!signed_half || !Is(signed_half->Arg(1), 0) || !Is(signed_half->Arg(2), 16)) return;
		const auto* half = Op(signed_half->Arg(0), ValueOpcode::BitFieldUExtract);
		if (!half || !Is(half->Arg(1), 16) || !Is(half->Arg(2), 16)) return;
		const auto* source = half->Arg(0).Resolve().TryInstruction();
		if (source) {
			// A true signed high-half comparison admits small positive words and
			// negative words, with a hole between them. Do not collapse that hole.
			bounds[source] = {{0, (limit.U32() << 16u) - 1u}, {0x80000000u, UINT32_MAX}};
		}
	}

	Ranges Get(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		if (value.IsImmediate() && value.GetType() == Type::U32)
			return {{value.U32(), value.U32()}};
		const auto* inst = value.TryInstruction();
		if (!inst || depth > 64) return Unknown();
		if (const auto found = bounds.find(inst); found != bounds.end()) return found->second;
		if (const auto found = cache.find(inst); found != cache.end()) return found->second;
		if (!visiting.insert(inst).second) return Unknown();
		auto result = Compute(*inst, depth + 1);
		visiting.erase(inst);
		cache.emplace(inst, result);
		return result;
	}

	Ranges Compute(const Inst& inst, uint32_t depth) {
		const auto op = inst.GetOpcode();
		if (op == ValueOpcode::SelectU32) {
			if (truth.contains(inst.Arg(0).Resolve().TryInstruction()))
				return Get(inst.Arg(1), depth);
			auto       result = Get(inst.Arg(1), depth);
			const auto other  = Get(inst.Arg(2), depth);
			result.insert(result.end(), other.begin(), other.end());
			return Normalize(std::move(result));
		}
		if (op == ValueOpcode::Phi) {
			if (loop_phis.contains(&inst)) return Unknown();
			Ranges result;
			for (size_t i = 0; i < inst.NumArgs(); ++i) {
				const auto ranges = Get(inst.Arg(i), depth);
				result.insert(result.end(), ranges.begin(), ranges.end());
			}
			return result.empty() ? Unknown() : Normalize(std::move(result));
		}
		if (op == ValueOpcode::BitFieldUExtract) {
			const auto offset = inst.Arg(1).Resolve(), count = inst.Arg(2).Resolve();
			if (!offset.IsImmediate() || !count.IsImmediate() || offset.U32() >= 32 ||
			    count.U32() == 0 || count.U32() > 32 - offset.U32())
				return Unknown();
			return Extract(Get(inst.Arg(0), depth), offset.U32(), count.U32());
		}
		if (op == ValueOpcode::UMedTri32) {
			const auto a = Get(inst.Arg(0), depth), b = Get(inst.Arg(1), depth),
			           c      = Get(inst.Arg(2), depth);
			const auto median = [](uint32_t x, uint32_t y, uint32_t z) {
				return std::max(std::min(x, y), std::min(std::max(x, y), z));
			};
			Ranges result;
			for (auto x: a)
				for (auto y: b)
					for (auto z: c)
						result.push_back({median(x.lo, y.lo, z.lo), median(x.hi, y.hi, z.hi)});
			return Normalize(std::move(result));
		}
		if (op != ValueOpcode::IAdd32 && op != ValueOpcode::BitwiseAnd32 &&
		    op != ValueOpcode::BitwiseOr32 && op != ValueOpcode::ShiftLeftLogical32 &&
		    op != ValueOpcode::UMin32)
			return Unknown();
		const auto a = Get(inst.Arg(0), depth), b = Get(inst.Arg(1), depth);
		if (op == ValueOpcode::BitwiseAnd32) {
			for (size_t i = 0; i < 2; ++i) {
				const auto mask = inst.Arg(i).Resolve();
				if (mask.IsImmediate() && mask.GetType() == Type::U32 && mask.U32() != 0 &&
				    (mask.U32() & (mask.U32() + 1u)) == 0)
					return Extract(i == 0 ? b : a, 0, std::bit_width(mask.U32()));
			}
			return Unknown();
		}
		Ranges result;
		for (auto x: a)
			for (auto y: b) {
				if (op == ValueOpcode::IAdd32) {
					const uint64_t lo = uint64_t {x.lo} + y.lo, hi = uint64_t {x.hi} + y.hi;
					if (hi - lo >= UINT32_MAX) return Unknown();
					if ((lo >> 32) != (hi >> 32)) {
						result.push_back({0, static_cast<uint32_t>(hi)});
						result.push_back({static_cast<uint32_t>(lo), UINT32_MAX});
					} else
						result.push_back({static_cast<uint32_t>(lo), static_cast<uint32_t>(hi)});
				} else if (op == ValueOpcode::ShiftLeftLogical32) {
					if (y.lo != y.hi || y.hi >= 32 || (uint64_t {x.hi} << y.hi) > UINT32_MAX)
						return Unknown();
					result.push_back({x.lo << y.lo, x.hi << y.lo});
				} else if (op == ValueOpcode::UMin32) {
					result.push_back({std::min(x.lo, y.lo), std::min(x.hi, y.hi)});
				} else {
					const auto bits = [](Interval r) {
						const auto width = std::bit_width(r.lo ^ r.hi);
						const auto varying =
						    width == 32 ? UINT32_MAX : (uint32_t {1} << width) - 1u;
						return Interval {r.lo & ~varying, r.lo | varying};
					};
					const auto xb = bits(x), yb = bits(y);
					result.push_back({xb.lo | yb.lo, xb.hi | yb.hi});
				}
			}
		return Normalize(std::move(result));
	}

	static Ranges Extract(const Ranges& ranges, uint32_t offset, uint32_t width) {
		const uint64_t period = uint64_t {1} << width;
		const auto     mask   = static_cast<uint32_t>(period - 1u);
		Ranges         result;
		for (auto range: ranges) {
			const uint64_t lo = range.lo >> offset, hi = range.hi >> offset;
			if (hi - lo >= period - 1u) return {{0, mask}};
			if (lo / period != hi / period) {
				result.push_back({0, static_cast<uint32_t>(hi) & mask});
				result.push_back({static_cast<uint32_t>(lo) & mask, mask});
			} else
				result.push_back(
				    {static_cast<uint32_t>(lo) & mask, static_cast<uint32_t>(hi) & mask});
		}
		return Normalize(std::move(result));
	}
};

bool ExcludesLocal(Value predicate, Value high) {
	const auto* inverse = Op(predicate, ValueOpcode::LogicalNot);
	const auto* either  = inverse ? Op(inverse->Arg(0), ValueOpcode::LogicalOr) : nullptr;
	if (!either) return false;
	const auto* a = Op(either->Arg(0), ValueOpcode::IEqual32);
	const auto* b = Op(either->Arg(1), ValueOpcode::IEqual32);
	if (!a || !b) return false;
	high = high.Resolve();
	return (Other(*a, SharedApertureHigh) == high && Other(*b, PrivateApertureHigh) == high) ||
	       (Other(*b, SharedApertureHigh) == high && Other(*a, PrivateApertureHigh) == high);
}

} // namespace

uint32_t SimplifyLocalAddressStores(Program& program) {
	if (program.wave_size != 32) return 0;
	// Cut every CFG cycle at a DFS backedge destination. Do not rely on block
	// storage order to identify loop-carried values: it need not be topological.
	std::unordered_map<const Block*, uint32_t> colors;
	std::unordered_set<const Block*>           loop_headers;
	for (const auto* root: program.blocks) {
		if (colors[root] != 0) continue;
		std::vector<std::pair<const Block*, size_t>> stack {{root, 0}};
		colors[root] = 1;
		while (!stack.empty()) {
			auto& [block, next]   = stack.back();
			const auto successors = block->ImmSuccessors();
			if (next == successors.size()) {
				colors[block] = 2;
				stack.pop_back();
				continue;
			}
			const auto* successor = successors[next++];
			if (colors[successor] == 1) loop_headers.insert(successor);
			if (colors[successor] == 0) {
				colors[successor] = 1;
				stack.emplace_back(successor, 0);
			}
		}
	}
	std::unordered_set<const Inst*> loop_phis;
	for (const auto* block: loop_headers)
		for (const auto& inst: *block)
			if (inst.GetOpcode() == ValueOpcode::Phi) loop_phis.insert(&inst);
	uint32_t removed = 0;
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end();) {
			auto& inst = *it++;
			if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::Write ||
			    inst.NumArgs() != 5)
				continue;
			const auto flags = inst.Flags<MemoryFlags>();
			if (flags.index >= program.memory_info.size() ||
			    program.memory_info[flags.index].kind != ResourceKind::Global)
				continue;
			const auto* mask = Op(inst.Arg(4), ValueOpcode::LogicalAnd);
			if (!mask) continue;
			for (size_t side = 0; side < 2; ++side) {
				if (!ExcludesLocal(mask->Arg(side), inst.Arg(2))) continue;
				Evaluator evaluator {loop_phis};
				evaluator.Assume(mask->Arg(side ^ 1));
				const auto ranges = evaluator.Get(inst.Arg(2));
				if (!std::all_of(ranges.begin(), ranges.end(), [](Interval r) {
					    return r.lo == r.hi &&
					           (r.lo == SharedApertureHigh || r.lo == PrivateApertureHigh);
				    }))
					break;
				// The operation's own predicate excludes every address it can use.
				// Leave the separately emitted shared/private operations unchanged.
				inst.Invalidate();
				++removed;
				break;
			}
		}
	}
	return removed;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
