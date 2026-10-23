#include "graphics/shader/recompiler/ir/passes/DeadLaneMasks.h"

#include "graphics/shader/recompiler/ir/Block.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

using BlockIndex = uint32_t;

std::atomic<int> g_override {-1};

Value Canonical(Value value) {
	for (;;) {
		value            = value.Resolve();
		const auto* inst = value.IsImmediate() ? nullptr : value.ResolveInstruction();
		if (inst == nullptr) {
			return value;
		}
		const auto opcode = inst->GetOpcode();
		if (opcode == ValueOpcode::SelectU1 && inst->Arg(0).Resolve().IsImmediate()) {
			value = inst->Arg(inst->Arg(0).Resolve().U1() ? 1 : 2);
			continue;
		}
		if (opcode == ValueOpcode::LogicalAnd || opcode == ValueOpcode::LogicalOr) {
			const auto lhs     = inst->Arg(0).Resolve();
			const auto rhs     = inst->Arg(1).Resolve();
			const bool neutral = opcode == ValueOpcode::LogicalAnd;
			if (lhs.IsImmediate() && lhs.U1() == neutral) {
				value = rhs;
				continue;
			}
			if (rhs.IsImmediate() && rhs.U1() == neutral) {
				value = lhs;
				continue;
			}
		}
		return value;
	}
}

class SubsetProver {
public:
	// True when every lane set in value is also set in root.
	bool Implies(Value value, const Inst* root) {
		value = Canonical(value);
		if (value.IsImmediate()) {
			return value.GetType() == Type::U1 && !value.U1();
		}
		const auto* inst = value.ResolveInstruction();
		if (inst == nullptr) {
			return false;
		}
		if (inst == root) {
			return true;
		}
		const auto key = std::pair {inst, root};
		if (const auto found = memo.find(key); found != memo.end()) {
			return found->second;
		}
		// A phi cycle back to itself adds no lanes: assume it holds while proving the cycle.
		memo.emplace(key, true);
		const auto mark   = journal.size();
		bool       result = false;
		switch (inst->GetOpcode()) {
			case ValueOpcode::LogicalAnd:
				result = Implies(inst->Arg(0), root) || Implies(inst->Arg(1), root);
				break;
			case ValueOpcode::LogicalOr:
				result = Implies(inst->Arg(0), root) && Implies(inst->Arg(1), root);
				break;
			case ValueOpcode::SelectU1:
				result = Implies(inst->Arg(1), root) && Implies(inst->Arg(2), root);
				break;
			case ValueOpcode::Phi:
				result = true;
				for (size_t index = 0; index < inst->NumArgs() && result; index++) {
					result = Implies(inst->Arg(index), root);
				}
				break;
			default: result = false; break;
		}
		if (!result) {
			// Anything proven since this node started may have leaned on its assumption.
			for (size_t index = mark; index < journal.size(); index++) {
				if (const auto found = memo.find(journal[index]);
				    found != memo.end() && found->second) {
					memo.erase(found);
				}
			}
			journal.resize(mark);
		}
		memo[key] = result;
		journal.push_back(key);
		return result;
	}

private:
	std::map<std::pair<const Inst*, const Inst*>, bool> memo;
	std::vector<std::pair<const Inst*, const Inst*>>    journal;
};

struct ExecRead {
	uint32_t position = 0;
	Value    value;
};

struct Candidate {
	Inst*       select   = nullptr;
	BlockIndex  block    = 0;
	uint32_t    position = 0;
	const Inst* root     = nullptr;
};

bool ReadsOtherLanes(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::DppMoveU32:
		case ValueOpcode::DppUpdateU32:
		case ValueOpcode::WqmU64:
		case ValueOpcode::ReadLane:
		case ValueOpcode::Permlane16U32:
		case ValueOpcode::PermuteU32:
		case ValueOpcode::BpermuteU32:
		case ValueOpcode::SwizzleU32: return true;
		default: return false;
	}
}

} // namespace

void OverrideDeadLaneWriteMasks(int mode) {
	g_override.store(mode);
}

uint32_t EliminateDeadLaneWriteMasks(Program& program, const std::vector<Inst*>& exec_reads) {
	if (program.stage != ShaderType::Compute || g_override.load() == 0) {
		return 0;
	}
	const auto&                                  blocks = program.blocks;
	const auto                                   count  = static_cast<BlockIndex>(blocks.size());
	std::unordered_map<const Block*, BlockIndex> index_of;
	for (BlockIndex index = 0; index < count; index++) {
		index_of.emplace(blocks[index], index);
	}

	const std::unordered_set<const Inst*> exec_read_set(exec_reads.begin(), exec_reads.end());
	std::vector<std::vector<ExecRead>>    reads(count);
	std::vector<Candidate>                candidates;
	for (BlockIndex index = 0; index < count; index++) {
		uint32_t position = 0;
		for (auto& inst: *blocks[index]) {
			position++;
			if (exec_read_set.contains(&inst)) {
				// The SSA rewrite left each read as an identity of the EXEC value it saw.
				if (inst.GetOpcode() != ValueOpcode::Identity) {
					return 0;
				}
				reads[index].push_back({position, inst.Arg(0)});
				continue;
			}
			if (inst.GetOpcode() == ValueOpcode::SelectU32 &&
			    inst.Flags<uint64_t>() == VgprWriteMaskFlag) {
				const auto condition = Canonical(inst.Arg(0));
				if (condition.IsImmediate()) {
					continue;
				}
				if (const auto* root = condition.ResolveInstruction(); root != nullptr) {
					candidates.push_back({&inst, index, position, root});
				}
			}
		}
	}
	if (candidates.empty()) {
		return 0;
	}

	// Blocks reachable over at least one edge.
	const auto                         words = (count + 63u) / 64u;
	std::vector<std::vector<uint64_t>> reach(count, std::vector<uint64_t>(words, 0));
	for (bool changed = true; changed;) {
		changed = false;
		for (BlockIndex index = count; index-- > 0;) {
			auto& row = reach[index];
			for (const auto* successor: blocks[index]->ImmSuccessors()) {
				const auto found = index_of.find(successor);
				if (found == index_of.end()) {
					return 0;
				}
				const auto target = found->second;
				const auto bit    = uint64_t {1} << (target % 64u);
				changed |= (row[target / 64u] & bit) == 0;
				row[target / 64u] |= bit;
				for (uint32_t word = 0; word < words; word++) {
					const auto merged = row[word] | reach[target][word];
					changed |= merged != row[word];
					row[word] = merged;
				}
			}
		}
	}
	const auto reaches = [&](BlockIndex from, BlockIndex to) {
		return ((reach[from][to / 64u] >> (to % 64u)) & 1u) != 0;
	};

	SubsetProver                                       prover;
	std::map<std::pair<BlockIndex, const Inst*>, bool> block_holds;
	std::map<std::pair<BlockIndex, const Inst*>, bool> later_holds;
	const auto all_reads_imply = [&](BlockIndex block, const Inst* root) {
		const auto key = std::pair {block, root};
		if (const auto found = block_holds.find(key); found != block_holds.end()) {
			return found->second;
		}
		const bool holds = std::ranges::all_of(
		    reads[block], [&](const ExecRead& read) { return prover.Implies(read.value, root); });
		block_holds.emplace(key, holds);
		return holds;
	};
	const auto later_blocks_imply = [&](BlockIndex block, const Inst* root) {
		const auto key = std::pair {block, root};
		if (const auto found = later_holds.find(key); found != later_holds.end()) {
			return found->second;
		}
		bool holds = true;
		for (BlockIndex other = 0; other < count && holds; other++) {
			if (reaches(block, other)) {
				holds = all_reads_imply(other, root);
			}
		}
		later_holds.emplace(key, holds);
		return holds;
	};

	std::vector<Candidate> accepted;
	for (const auto& candidate: candidates) {
		bool holds = later_blocks_imply(candidate.block, candidate.root);
		// The rest of the write's own block; all of it when a loop brings control back.
		const bool looped = reaches(candidate.block, candidate.block);
		for (const auto& read: reads[candidate.block]) {
			if (!holds) {
				break;
			}
			if (looped || read.position > candidate.position) {
				holds = prover.Implies(read.value, candidate.root);
			}
		}
		if (holds) {
			accepted.push_back(candidate);
		}
	}
	if (accepted.empty()) {
		return 0;
	}

	std::unordered_map<const Inst*, std::vector<const Inst*>> taint;
	std::vector<const Inst*>                                  worklist;
	const auto add_taint = [&](const Inst* inst, const Inst* root) {
		auto& roots = taint[inst];
		if (std::ranges::find(roots, root) == roots.end()) {
			roots.push_back(root);
			worklist.push_back(inst);
		}
	};
	for (const auto& candidate: accepted) {
		add_taint(candidate.select, candidate.root);
	}
	while (!worklist.empty()) {
		const auto* inst = worklist.back();
		worklist.pop_back();
		const auto roots = taint[inst];
		if (roots.size() > 8u) {
			return 0;
		}
		for (const auto& use: inst->Uses()) {
			const auto* user   = use.user;
			const auto  opcode = user->GetOpcode();
			if (ReadsOtherLanes(opcode)) {
				return 0;
			}
			if (opcode == ValueOpcode::Ballot || opcode == ValueOpcode::ConditionRef) {
				for (const auto* root: roots) {
					if (!prover.Implies(user->Arg(0), root)) {
						return 0;
					}
				}
				continue;
			}
			if (opcode == ValueOpcode::ReadFirstLane) {
				return 0;
			}
			if (user->GetType() == Type::Void) {
				continue;
			}
			for (const auto* root: roots) {
				if (user->GetType() == Type::U1 &&
				    prover.Implies(Value(const_cast<Inst*>(user)), root)) {
					continue;
				}
				add_taint(user, root);
			}
		}
	}

	for (const auto& candidate: accepted) {
		candidate.select->ReplaceUsesWith(candidate.select->Arg(1));
	}
	return static_cast<uint32_t>(accepted.size());
}

namespace {

bool IsLaneLocal(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::Phi:
		case ValueOpcode::Identity:
		case ValueOpcode::GetUserData:
		case ValueOpcode::GetShaderBase:
		case ValueOpcode::GetSrtResource:
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource:
		case ValueOpcode::MakeImageAddress:
		case ValueOpcode::WriteLane:
		case ValueOpcode::ReadConst:
		case ValueOpcode::ReadConstBuffer:
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
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdEqual64:
		case ValueOpcode::FPOrdLessThanEqual64:
		case ValueOpcode::FPOrdGreaterThanEqual64:
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
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipIFlag32:
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

using DemandTerm = std::vector<const Inst*>;
using Demand     = std::vector<DemandTerm>;

constexpr size_t MaxDemandTerms     = 16;
constexpr size_t MaxDemandConjuncts = 4;

// Program order, not address order: the caps below must keep the same conjuncts on every run.
void Normalize(Demand& demand, const std::unordered_map<const Inst*, uint32_t>& position) {
	const auto at = [&](const Inst* inst) {
		const auto found = position.find(inst);
		return found != position.end() ? found->second : UINT32_MAX;
	};
	const auto before = [&](const Inst* a, const Inst* b) { return at(a) < at(b); };
	for (auto& term: demand) {
		std::ranges::sort(term, before);
		term.erase(std::unique(term.begin(), term.end()), term.end());
		if (term.size() > MaxDemandConjuncts) {
			term.resize(MaxDemandConjuncts);
		}
	}
	std::ranges::sort(demand, [&](const DemandTerm& a, const DemandTerm& b) {
		return std::ranges::lexicographical_compare(a, b, before);
	});
	demand.erase(std::unique(demand.begin(), demand.end()), demand.end());
	const bool every_lane = std::ranges::any_of(demand, [](const auto& t) { return t.empty(); });
	if (every_lane || demand.size() > MaxDemandTerms) {
		demand.assign(1, DemandTerm {});
	}
}

} // namespace

uint32_t EliminateUndemandedWriteMasks(Program& program) {
	if (program.stage != ShaderType::Compute || g_override.load() == 0) {
		return 0;
	}
	std::vector<const Inst*>                  insts;
	std::unordered_map<const Inst*, uint32_t> position;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			position.emplace(&inst, static_cast<uint32_t>(insts.size()));
			insts.push_back(&inst);
		}
	}
	std::unordered_map<const Inst*, Demand> demand;
	const auto                              every_lane = Demand {DemandTerm {}};
	// A mask argument as a conjunct: nothing for a constant true, no lanes for a constant false.
	const auto with = [](Demand base, Value mask, bool& none) {
		mask = Canonical(mask);
		none = false;
		if (mask.IsImmediate()) {
			none = mask.GetType() == Type::U1 && !mask.U1();
			return base;
		}
		const auto* inst = mask.ResolveInstruction();
		for (auto& term: base) {
			if (inst != nullptr) {
				term.push_back(inst);
			}
		}
		return base;
	};
	const auto contribution = [&](const Use& use, Demand& out) {
		const auto* user        = use.user;
		const auto  opcode      = user->GetOpcode();
		const auto  user_demand = [&]() -> const Demand& {
			static const Demand empty;
			const auto          found = demand.find(user);
			return found != demand.end() ? found->second : empty;
		};
		bool none = false;
		if (opcode == ValueOpcode::Reference || opcode == ValueOpcode::ReferenceU32) {
			return;
		}
		if (opcode == ValueOpcode::SelectU32 && user->Flags<uint64_t>() == VgprWriteMaskFlag) {
			const auto part =
			    use.operand == 1 ? with(user_demand(), user->Arg(0), none) : user_demand();
			if (!none) {
				out.insert(out.end(), part.begin(), part.end());
			}
			return;
		}
		if (opcode == ValueOpcode::LogicalAnd && use.operand < 2) {
			const auto part = with(user_demand(), user->Arg(1 - use.operand), none);
			if (!none) {
				out.insert(out.end(), part.begin(), part.end());
			}
			return;
		}
		if (IsLaneLocal(opcode)) {
			const auto& part = user_demand();
			out.insert(out.end(), part.begin(), part.end());
			return;
		}
		if (opcode == ValueOpcode::ReadFirstLane && use.operand == 0) {
			const auto part = with(every_lane, user->Arg(1), none);
			if (!none) {
				out.insert(out.end(), part.begin(), part.end());
			}
			return;
		}
		if (ReadsOtherLanes(opcode) || opcode == ValueOpcode::Ballot ||
		    opcode == ValueOpcode::ConditionRef || opcode == ValueOpcode::ReadFirstLane ||
		    user->NumArgs() == 0) {
			out.insert(out.end(), every_lane.begin(), every_lane.end());
			return;
		}
		const auto last = user->NumArgs() - 1;
		if (use.operand != last && user->Arg(last).GetType() == Type::U1) {
			const auto part = with(every_lane, user->Arg(last), none);
			if (!none) {
				out.insert(out.end(), part.begin(), part.end());
			}
			return;
		}
		out.insert(out.end(), every_lane.begin(), every_lane.end());
	};

	std::vector<const Inst*>        worklist(insts.begin(), insts.end());
	std::unordered_set<const Inst*> queued(insts.begin(), insts.end());
	size_t                          steps = 0;
	while (!worklist.empty()) {
		if (++steps > insts.size() * 64u + 4096u) {
			return 0; // Not converging; leave every select in place.
		}
		const auto* inst = worklist.back();
		worklist.pop_back();
		queued.erase(inst);
		Demand next;
		for (const auto& use: inst->Uses()) {
			contribution(use, next);
		}
		auto& current = demand[inst];
		// Join with what is already known, so capping cannot make the iteration oscillate.
		next.insert(next.end(), current.begin(), current.end());
		Normalize(next, position);
		if (next == current) {
			continue;
		}
		current = std::move(next);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			const auto arg = inst->Arg(index).Resolve();
			if (arg.IsImmediate()) {
				continue;
			}
			if (const auto* producer = arg.ResolveInstruction();
			    producer != nullptr && queued.insert(producer).second) {
				worklist.push_back(producer);
			}
		}
	}

	SubsetProver       prover;
	std::vector<Inst*> accepted;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::SelectU32 ||
			    inst.Flags<uint64_t>() != VgprWriteMaskFlag) {
				continue;
			}
			const auto condition = Canonical(inst.Arg(0));
			if (condition.IsImmediate()) {
				continue;
			}
			const auto* root  = condition.ResolveInstruction();
			const auto  found = demand.find(&inst);
			if (root == nullptr || found == demand.end()) {
				continue;
			}
			const bool holds = std::ranges::all_of(found->second, [&](const DemandTerm& term) {
				return std::ranges::any_of(term, [&](const Inst* conjunct) {
					return prover.Implies(Value(const_cast<Inst*>(conjunct)), root);
				});
			});
			if (holds) {
				accepted.push_back(&inst);
			}
		}
	}
	for (auto* select: accepted) {
		select->ReplaceUsesWith(select->Arg(1));
	}
	return static_cast<uint32_t>(accepted.size());
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
