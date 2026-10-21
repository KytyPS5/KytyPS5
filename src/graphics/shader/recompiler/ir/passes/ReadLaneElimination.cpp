#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"

#include <algorithm>
#include <array>
#include <optional>
#include <cinttypes>
#include <cstdio>
#include <mutex>

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

} // namespace

ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size) {
	ReadLaneStats stats;
	if (wave_size != 32u && wave_size != 64u) {
		return stats;
	}

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

namespace {

Block::iterator PositionOf(Inst* inst) {
	auto* block = inst->Parent();
	auto  it    = std::find_if(block->begin(), block->end(),
	                           [&](const Inst& candidate) { return &candidate == inst; });
	EXIT_IF(it == block->end());
	return it;
}

// What lane `lane` of a register holds, when every lane holds the same values except where
// V_WRITELANE put a scalar into one lane. A compiler keeps scalars it has no register for in the
// lanes of a vector register, so the value must come back exactly: through the writes to other
// lanes, and through the merges that masked writes and joining paths leave behind.
// Every value that `seed` picks, and every value computed from one.
template <typename Seed>
std::unordered_set<const Inst*> ValuesComputedFrom(const Program& program, Seed&& seed) {
	std::unordered_set<const Inst*> tainted;
	for (bool changed = true; changed;) {
		changed = false;
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				if (tainted.contains(&inst)) {
					continue;
				}
				bool taint = seed(inst);
				for (size_t index = 0; !taint && index < inst.NumArgs(); index++) {
					const auto* argument = inst.Arg(index).Resolve().TryInstruction();
					taint                = argument != nullptr && tainted.contains(argument);
				}
				if (taint) {
					tainted.insert(&inst);
					changed = true;
				}
			}
		}
	}
	return tainted;
}

// Every value that holds, or is computed from, a register with a lane write. Such a value may
// differ from lane to lane, so only the operations LaneOfUniformRegister follows give a lane of
// it back exactly.
std::unordered_set<const Inst*> ValuesWithLaneWrites(const Program& program) {
	return ValuesComputedFrom(
	    program, [](const Inst& inst) { return inst.GetOpcode() == ValueOpcode::WriteLane; });
}

// Every value computed from what differs between invocations: a vector register's first value,
// a builtin, an attribute. A value that differs only through a branch on such a value is not
// found.
std::unordered_set<const Inst*> PerInvocationValues(const Program& program) {
	return ValuesComputedFrom(program, [](const Inst& inst) {
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetVectorRegister:
			case ValueOpcode::GetBuiltin:
			case ValueOpcode::GetAttribute:
			case ValueOpcode::GetInterpolationParameter:
			case ValueOpcode::GetTessellationAttribute:
			case ValueOpcode::GetThreadBitScalarRegister:
			case ValueOpcode::LaneId: return true;
			default: return false;
		}
	});
}

// Once per shader: its permutations are translated one by one, on several threads.
void ReportFirstLaneRead(const Program& program) {
	static std::mutex                   mutex;
	static std::unordered_set<uint64_t> reported;
	{
		std::lock_guard lock(mutex);
		if (!reported.insert(program.shader_hash).second) {
			return;
		}
	}
	std::fprintf(stderr,
	             "shader reads the first lane of a value that differs between invocations, in "
	             "the single-lane model: each invocation reads its own (hash=0x%016" PRIx64
	             " stage=%u)\n",
	             program.shader_hash, static_cast<unsigned>(program.stage));
}

[[noreturn]] void StopAtInexactLaneRead(const Program& program, const char* what) {
	EXIT("shader reads a register lane in a way the single-lane model cannot give back exactly "
	     "(%s): hash=0x%016" PRIx64 " stage=%u\n",
	     what, program.shader_hash, static_cast<unsigned>(program.stage));
}

// The lane a selector names: the hardware reads only its low bits, so it wraps into the wave.
template <typename Position>
Value WaveLane(Block& block, Position at, Value selector, uint32_t wave_size) {
	return Value(&*block.PrependNewInst(at, ValueOpcode::BitwiseAnd32,
	                                    {selector, Value(wave_size - 1u)}));
}

struct LaneLowering {
	const Program&                         program;
	const std::unordered_set<const Inst*>& with_lane_writes;
};

Value LaneOfUniformRegister(const LaneLowering& lowering, std::unordered_map<Inst*, Value>& known,
                            Value source, uint32_t lane) {
	source     = source.Resolve();
	auto* inst = source.TryInstruction();
	if (inst == nullptr) {
		return source;
	}
	if (const auto found = known.find(inst); found != known.end()) {
		return found->second;
	}
	switch (inst->GetOpcode()) {
		case ValueOpcode::WriteLane: {
			const auto selector = inst->Arg(2).Resolve();
			const auto below    = LaneOfUniformRegister(lowering, known, inst->Arg(0), lane);
			Value      result;
			if (selector.IsImmediate() && selector.GetType() == Type::U32) {
				result = selector.U32() % lowering.program.wave_size == lane ? inst->Arg(1) : below;
			} else {
				auto*      block = inst->Parent();
				const auto at    = PositionOf(inst);
				const auto hit   = block->PrependNewInst(
				    at, ValueOpcode::IEqual32,
				    {WaveLane(*block, at, selector, lowering.program.wave_size), Value(lane)});
				result = Value(&*block->PrependNewInst(at, ValueOpcode::SelectU32,
				                                       {Value(&*hit), inst->Arg(1), below}));
			}
			known.insert_or_assign(inst, result);
			return result;
		}
		case ValueOpcode::SelectU32: {
			const auto taken   = LaneOfUniformRegister(lowering, known, inst->Arg(1), lane);
			const auto skipped = LaneOfUniformRegister(lowering, known, inst->Arg(2), lane);
			Value      result  = taken;
			if (taken.Resolve() == inst->Arg(1).Resolve() &&
			    skipped.Resolve() == inst->Arg(2).Resolve()) {
				// No lane write below it: an ordinary value, the same in every lane.
				result = source;
			} else if (taken.Resolve() != skipped.Resolve()) {
				result = Value(&*inst->Parent()->PrependNewInst(
				    PositionOf(inst), ValueOpcode::SelectU32, {inst->Arg(0), taken, skipped}));
			}
			known.insert_or_assign(inst, result);
			return result;
		}
		case ValueOpcode::Phi: {
			if (inst->NumArgs() == 0) {
				return source;
			}
			// The new phi is known before its operands are, so that a loop ends.
			auto& phi = *inst->Parent()->PrependNewInst(PositionOf(inst), ValueOpcode::Phi);
			phi.SetFlags(Type::U32);
			known.insert_or_assign(inst, Value(&phi));
			for (size_t index = 0; index < inst->NumArgs(); index++) {
				phi.AddPhiOperand(inst->PhiBlock(index),
				                  LaneOfUniformRegister(lowering, known, inst->Arg(index), lane));
			}
			return Value(&phi);
		}
		default:
			// An ordinary value is the same in every lane. A value computed from a register with
			// a lane write is not, and how it was computed is not followed.
			if (lane != 0u && lowering.with_lane_writes.contains(inst)) {
				StopAtInexactLaneRead(lowering.program, "a value computed from a lane write");
			}
			return source;
	}
}

} // namespace

uint32_t LowerLaneOpsToSingleLane(Program& program) {
	uint32_t replaced = 0;
	// Per lane that is read: what each register value holds in it.
	std::unordered_map<uint32_t, std::unordered_map<Inst*, Value>> known;
	const auto         with_lane_writes = ValuesWithLaneWrites(program);
	const LaneLowering lowering {program, with_lane_writes};
	// Before any lane operation is rewritten: a LaneId turns into 0 below.
	const bool reads_first_lane = std::ranges::any_of(program.blocks, [](const Block* block) {
		return std::ranges::any_of(*block, [](const Inst& inst) {
			return inst.GetOpcode() == ValueOpcode::ReadFirstLane;
		});
	});
	const auto per_invocation =
	    reads_first_lane ? PerInvocationValues(program) : std::unordered_set<const Inst*> {};
	bool reported = false;
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			switch (inst.GetOpcode()) {
				case ValueOpcode::ReadLane: {
					const auto selector = inst.Arg(1).Resolve();
					if (selector.IsImmediate() && selector.GetType() == Type::U32) {
						const auto lane = selector.U32() % program.wave_size;
						inst.ReplaceUsesWith(
						    LaneOfUniformRegister(lowering, known[lane], inst.Arg(0), lane));
					} else {
						// A lane chosen at run time: every lane must hold the same value.
						const auto* source = inst.Arg(0).Resolve().TryInstruction();
						if (source != nullptr && with_lane_writes.contains(source)) {
							StopAtInexactLaneRead(program, "a lane chosen at run time");
						}
						inst.ReplaceUsesWith(inst.Arg(0));
					}
					replaced++;
					break;
				}
				case ValueOpcode::ReadFirstLane: {
					// Each invocation reads its own value. That is exact for a value every lane
					// holds, and for the usual loop that takes one lane's value at a time. Only a
					// value broadcast from the first lane to the others comes out different.
					// Which one this read is cannot be seen here: it is reported, once per shader.
					const auto* source = inst.Arg(0).Resolve().TryInstruction();
					if (!reported && source != nullptr && per_invocation.contains(source)) {
						reported = true;
						ReportFirstLaneRead(program);
					}
					inst.ReplaceUsesWith(inst.Arg(0));
					replaced++;
					break;
				}
				case ValueOpcode::LaneId:
					inst.ReplaceUsesWith(Value(0u));
					replaced++;
					break;
				case ValueOpcode::Ballot: {
					const auto all = block->PrependNewInst(
					    it, ValueOpcode::SelectU32, {inst.Arg(0), Value(0xffffffffu), Value(0u)});
					const auto word = Value(&*all);
					const auto mask = block->PrependNewInst(
					    it, ValueOpcode::CompositeConstructU32x4, {word, word, word, word});
					inst.ReplaceUsesWith(Value(&*mask));
					replaced++;
					break;
				}
				default: break;
			}
		}
	}
	// What is left of a register with lane writes is used as this invocation's own lane, which
	// is lane 0. After the reads above, so that they still see the writes.
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			if (inst.GetOpcode() != ValueOpcode::WriteLane) {
				continue;
			}
			const auto selector = inst.Arg(2).Resolve();
			if (selector.IsImmediate() && selector.GetType() == Type::U32) {
				inst.ReplaceUsesWith(selector.U32() % program.wave_size == 0u ? inst.Arg(1)
				                                                              : inst.Arg(0));
			} else {
				const auto hit = block->PrependNewInst(
				    it, ValueOpcode::IEqual32,
				    {WaveLane(*block, it, selector, program.wave_size), Value(0u)});
				const auto own = block->PrependNewInst(it, ValueOpcode::SelectU32,
				                                       {Value(&*hit), inst.Arg(1), inst.Arg(0)});
				inst.ReplaceUsesWith(Value(&*own));
			}
			replaced++;
		}
	}
	return replaced;
}

namespace {

// For each lane, the lanes of the reduced operand that its value combines. Lanes whose value is
// not such a combination (a DPP read the hardware answers with zero) are "poisoned".
struct LaneSets {
	Value                    operand;
	ValueOpcode              op = ValueOpcode::Void;
	std::array<uint64_t, 64> lanes {};
	uint64_t                 poisoned = 0;
	bool                     uniform  = false;
};

bool Idempotent(ValueOpcode op) {
	return op == ValueOpcode::UMin32 || op == ValueOpcode::UMax32 || op == ValueOpcode::SMin32 ||
	       op == ValueOpcode::SMax32 || op == ValueOpcode::BitwiseAnd32 || op == ValueOpcode::BitwiseOr32;
}

std::optional<uint32_t> Identity(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::UMin32:
		case ValueOpcode::BitwiseAnd32: return 0xffffffffu;
		case ValueOpcode::UMax32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32: return 0u;
		case ValueOpcode::SMin32: return 0x7fffffffu;
		case ValueOpcode::SMax32: return 0x80000000u;
		default: return std::nullopt;
	}
}

// EXEC with every lane on, as S_ORN2_SAVEEXEC leaves it.
bool AllLanes(Value exec) {
	exec = exec.Resolve();
	if (exec.IsImmediate()) return exec.GetType() == Type::U1 && exec.U1();
	const auto* inst = exec.TryInstruction();
	if (inst == nullptr || inst->GetOpcode() != ValueOpcode::LogicalOr) return false;
	const auto is_not_of = [](Value value, Value of) {
		const auto* inst = value.Resolve().TryInstruction();
		return inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalNot &&
		       inst->Arg(0).Resolve() == of.Resolve();
	};
	return is_not_of(inst->Arg(0), inst->Arg(1)) || is_not_of(inst->Arg(1), inst->Arg(0));
}

// A register write under every lane: the written value.
Value StripFullWrite(Value value) {
	for (;;) {
		value = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::SelectU32 || !AllLanes(inst->Arg(0)))
			return value;
		value = inst->Arg(1);
	}
}

// The lane a DPP source reads for each lane, as the SPIR-V emitter maps it (EmitDppTargetLane);
// UINT32_MAX for a lane with no source.
std::optional<std::array<uint32_t, 64>> DppSources(const DppMoveFlags& flags) {
	std::array<uint32_t, 64> source {};
	const auto control = flags.control;
	for (uint32_t lane = 0; lane < 64u; ++lane) {
		const uint32_t row = lane & ~15u, in_row = lane & 15u;
		if (flags.dpp8) return std::nullopt;
		if (control <= 0xffu) {
			source[lane] = (lane & ~3u) | ((control >> ((lane & 3u) * 2u)) & 3u);
		} else if (control >= 0x101u && control <= 0x10fu) {
			const auto from = in_row + (control & 0xfu);
			source[lane]    = from < 16u ? row | from : UINT32_MAX;
		} else if (control >= 0x111u && control <= 0x11fu) {
			const auto shift = control & 0xfu;
			source[lane]     = in_row >= shift ? row | (in_row - shift) : UINT32_MAX;
		} else if (control >= 0x121u && control <= 0x12fu) {
			source[lane] = row | ((in_row + 16u - (control & 0xfu)) & 15u);
		} else if (control == 0x140u) {
			source[lane] = row | (15u - in_row);
		} else if (control == 0x141u) {
			source[lane] = row | (in_row & 8u) | (7u - (in_row & 7u));
		} else if (control >= 0x150u && control <= 0x15fu) {
			source[lane] = row | (control & 0xfu);
		} else if (control >= 0x160u && control <= 0x16fu) {
			source[lane] = lane ^ (control & 0xfu);
		} else {
			return std::nullopt;
		}
	}
	return source;
}

class ReductionFinder {
public:
	explicit ReductionFinder(uint32_t wave_size): wave_size(wave_size) {}

	std::optional<LaneSets> Analyze(Value value, uint32_t depth = 0) {
		value = StripFullWrite(value);
		auto* inst = value.TryInstruction();
		if (inst != nullptr) {
			if (const auto found = memo.find(inst); found != memo.end()) return found->second;
		}
		auto result = depth > 64u ? std::nullopt : Compute(value, inst, depth);
		if (inst != nullptr) memo.emplace(inst, result);
		return result;
	}

private:
	LaneSets Leaf(Value value) const {
		LaneSets sets {.operand = value};
		for (uint32_t lane = 0; lane < wave_size; ++lane) sets.lanes[lane] = uint64_t {1} << lane;
		return sets;
	}

	static bool Merge(LaneSets& into, const LaneSets& other, ValueOpcode op) {
		if (into.operand != other.operand) return false;
		if (into.op != ValueOpcode::Void && into.op != op) return false;
		if (other.op != ValueOpcode::Void && other.op != op) return false;
		into.op = op;
		for (uint32_t lane = 0; lane < 64u; ++lane) {
			if (!Idempotent(op) && (into.lanes[lane] & other.lanes[lane]) != 0u) return false;
			into.lanes[lane] |= other.lanes[lane];
		}
		into.poisoned |= other.poisoned;
		into.uniform = into.uniform && other.uniform;
		return true;
	}

	std::optional<LaneSets> Combine(Value lhs, Value rhs, ValueOpcode op, uint32_t depth) {
		auto left  = Analyze(lhs, depth + 1u);
		auto right = Analyze(rhs, depth + 1u);
		// One side may be the operand itself.
		if (left && !right) right = Leaf(StripFullWrite(rhs));
		if (right && !left) left = Leaf(StripFullWrite(lhs));
		if (!left || !right) return std::nullopt;
		if (!Merge(*left, *right, op)) return std::nullopt;
		return left;
	}

	std::optional<LaneSets> Compute(Value value, Inst* inst, uint32_t depth) {
		if (inst == nullptr) return std::nullopt;
		switch (const auto op = inst->GetOpcode()) {
			case ValueOpcode::ReadLane: {
				const auto lane = inst->Arg(1).Resolve();
				if (!lane.IsImmediate() || lane.GetType() != Type::U32) return std::nullopt;
				auto sets = Analyze(inst->Arg(0), depth + 1u);
				if (!sets || sets->op == ValueOpcode::Void) return std::nullopt;
				const auto index = lane.U32() % wave_size;
				if ((sets->poisoned >> index) & 1u) return std::nullopt;
				const auto mask = sets->lanes[index];
				sets->lanes.fill(0);
				for (uint32_t i = 0; i < wave_size; ++i) sets->lanes[i] = mask;
				sets->poisoned = 0;
				sets->uniform  = true;
				return sets;
			}
			case ValueOpcode::DppUpdateU32: {
				if (!AllLanes(inst->Arg(2))) return std::nullopt;
				const auto flags   = inst->Flags<DppMoveFlags>();
				const auto sources = DppSources(flags);
				if (!sources) return std::nullopt;
				const auto old_sets = Analyze(inst->Arg(1), depth + 1u);
				auto       old      = old_sets ? *old_sets : Leaf(StripFullWrite(inst->Arg(1)));
				auto       written  = Analyze(inst->Arg(0), depth + 1u);
				if (!written || written->operand != old.operand) return std::nullopt;
				LaneSets result = old;
				result.op       = written->op;
				for (uint32_t lane = 0; lane < wave_size; ++lane) {
					const bool masks = ((flags.row_mask >> (lane / 16u)) & 1u) != 0u &&
					                   ((flags.bank_mask >> ((lane / 4u) & 3u)) & 1u) != 0u;
					const bool valid = (*sources)[lane] < wave_size;
					if (!masks || (!flags.bound_control && !valid)) continue;
					result.lanes[lane] = written->lanes[lane];
					result.poisoned    = (result.poisoned & ~(uint64_t {1} << lane)) |
					                  (written->poisoned & (uint64_t {1} << lane));
				}
				return result;
			}
			case ValueOpcode::DppMoveU32: {
				// The value a DPP source reads, before the instruction combines it: lanes with no
				// source read zero, which is no lane set.
				if (!AllLanes(inst->Arg(1))) return std::nullopt;
				const auto sources = DppSources(inst->Flags<DppMoveFlags>());
				if (!sources) return std::nullopt;
				auto source = Analyze(inst->Arg(0), depth + 1u);
				if (!source) source = Leaf(StripFullWrite(inst->Arg(0)));
				LaneSets result = *source;
				for (uint32_t lane = 0; lane < wave_size; ++lane) {
					const auto from = (*sources)[lane];
					if (from >= wave_size) {
						result.lanes[lane] = 0;
						result.poisoned |= uint64_t {1} << lane;
					} else {
						result.lanes[lane] = source->lanes[from];
						result.poisoned    = (result.poisoned & ~(uint64_t {1} << lane)) |
						                  (((source->poisoned >> from) & 1u) << lane);
					}
				}
				return result;
			}
			case ValueOpcode::Permlane16U32: {
				if (!AllLanes(inst->Arg(3))) return std::nullopt;
				const auto low = inst->Arg(1).Resolve(), high = inst->Arg(2).Resolve();
				if (!low.IsImmediate() || !high.IsImmediate()) return std::nullopt;
				const bool x16 = inst->Flags<PermlaneFlags>().x16;
				auto source = Analyze(inst->Arg(0), depth + 1u);
				if (!source) source = Leaf(StripFullWrite(inst->Arg(0)));
				LaneSets result = *source;
				for (uint32_t lane = 0; lane < wave_size; ++lane) {
					const uint32_t in_row = lane & 15u;
					const uint32_t select = ((in_row < 8u ? low.U32() : high.U32()) >> ((in_row & 7u) * 4u)) & 15u;
					const uint32_t from   = ((lane & ~15u) ^ (x16 ? 16u : 0u)) | select;
					if (from >= wave_size) return std::nullopt;
					result.lanes[lane] = source->lanes[from];
					result.poisoned    = (result.poisoned & ~(uint64_t {1} << lane)) |
					                  (((source->poisoned >> from) & 1u) << lane);
				}
				return result;
			}
			default:
				if (Identity(op).has_value()) return Combine(inst->Arg(0), inst->Arg(1), op, depth);
				(void)value;
				return std::nullopt;
		}
	}

	uint32_t wave_size;
	std::unordered_map<const Inst*, std::optional<LaneSets>> memo;
};

// The operand's lanes outside EXEC hold the identity: a select of it by a lane condition.
bool HoldsIdentityOutsideExec(Value operand, uint32_t identity) {
	const auto* inst = StripFullWrite(operand).TryInstruction();
	if (inst == nullptr || inst->GetOpcode() != ValueOpcode::SelectU32) return false;
	const auto is_identity = [identity](Value value) {
		value = value.Resolve();
		return value.IsImmediate() && value.GetType() == Type::U32 && value.U32() == identity;
	};
	return is_identity(inst->Arg(1)) || is_identity(inst->Arg(2));
}

} // namespace

uint32_t RecognizeWaveReductions(Program& program) {
	const auto wave_size = program.wave_size;
	if (wave_size != 32u && wave_size != 64u) return 0;
	const uint64_t full = wave_size == 64u ? ~uint64_t {0} : 0xffffffffu;
	ReductionFinder finder(wave_size);
	uint32_t found = 0;
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			const auto op = inst.GetOpcode();
			if (op != ValueOpcode::ReadLane && !Identity(op).has_value()) continue;
			if (inst.Uses().empty()) continue;
			const auto sets = finder.Analyze(Value(&inst));
			if (!sets || !sets->uniform || sets->lanes[0] != full) continue;
			if (!HoldsIdentityOutsideExec(sets->operand, *Identity(sets->op))) continue;
			const auto reduced = block->PrependNewInst(it, ValueOpcode::WaveReduceU32, {sets->operand},
			                                           static_cast<uint64_t>(sets->op));
			inst.ReplaceUsesWith(Value(&*reduced));
			found++;
		}
	}
	return found;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
