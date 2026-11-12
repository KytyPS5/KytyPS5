#include "graphics/shader/recompiler/ir/passes/DeadLaneMasks.h"

#include "graphics/shader/recompiler/ir/Block.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

std::atomic<int> g_override {-1};

Value Canonical(Value value) {
	for (;;) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
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

bool IsFalse(Value value) {
	return value.IsImmediate() && value.GetType() == Type::U1 && !value.U1();
}

struct CompareRange {
	Value    operand;
	uint64_t low  = 0;
	uint64_t high = 0; // inclusive; low > high is empty
};

std::optional<CompareRange> RangeOf(const Inst& compare) {
	switch (compare.GetOpcode()) {
		case ValueOpcode::ULessThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::IEqual32: break;
		default: return std::nullopt;
	}
	const auto lhs = compare.Arg(0).Resolve();
	const auto rhs = compare.Arg(1).Resolve();
	if (lhs.IsImmediate() == rhs.IsImmediate()) {
		return std::nullopt;
	}
	const bool swapped  = lhs.IsImmediate();
	const auto operand  = swapped ? rhs : lhs;
	const auto constant = swapped ? lhs : rhs;
	if (constant.GetType() != Type::U32) {
		return std::nullopt;
	}
	constexpr uint64_t Max    = 0xffffffffu;
	const uint64_t     c      = constant.U32();
	auto               opcode = compare.GetOpcode();
	if (swapped) {
		switch (opcode) {
			case ValueOpcode::ULessThan32: opcode = ValueOpcode::UGreaterThan32; break;
			case ValueOpcode::UGreaterThan32: opcode = ValueOpcode::ULessThan32; break;
			case ValueOpcode::ULessThanEqual32: opcode = ValueOpcode::UGreaterThanEqual32; break;
			case ValueOpcode::UGreaterThanEqual32: opcode = ValueOpcode::ULessThanEqual32; break;
			default: break;
		}
	}
	switch (opcode) {
		case ValueOpcode::ULessThan32:
			return c == 0 ? CompareRange {operand, 1, 0} : CompareRange {operand, 0, c - 1};
		case ValueOpcode::ULessThanEqual32: return CompareRange {operand, 0, c};
		case ValueOpcode::UGreaterThan32: return CompareRange {operand, c + 1, Max};
		case ValueOpcode::UGreaterThanEqual32: return CompareRange {operand, c, Max};
		case ValueOpcode::IEqual32: return CompareRange {operand, c, c};
		default: return std::nullopt;
	}
}

bool CompareImplies(const Inst& value, const Inst& root) {
	const auto inner = RangeOf(value);
	const auto outer = RangeOf(root);
	if (!inner || !outer || inner->operand != outer->operand) {
		return false;
	}
	return inner->low > inner->high || (outer->low <= inner->low && inner->high <= outer->high);
}

class ControlFlow {
public:
	explicit ControlFlow(const BlockList& blocks) {
		if (blocks.empty()) {
			return;
		}
		std::vector<const Block*> order;
		{
			std::unordered_set<const Block*>             visited {blocks.front()};
			std::vector<std::pair<const Block*, size_t>> stack {{blocks.front(), 0}};
			while (!stack.empty()) {
				const auto* block      = stack.back().first;
				const auto  successors = block->ImmSuccessors();
				if (stack.back().second < successors.size()) {
					const auto* successor = successors[stack.back().second++];
					if (visited.insert(successor).second) {
						stack.emplace_back(successor, 0);
					}
					continue;
				}
				order.push_back(block);
				stack.pop_back();
			}
		}
		std::ranges::reverse(order);
		const auto count = static_cast<uint32_t>(order.size());
		for (uint32_t index = 0; index < count; index++) {
			m_index.emplace(order[index], index);
		}

		std::vector<uint32_t> idom(count, UINT32_MAX);
		idom[0]              = 0;
		const auto intersect = [&](uint32_t a, uint32_t b) {
			while (a != b) {
				while (a > b) {
					a = idom[a];
				}
				while (b > a) {
					b = idom[b];
				}
			}
			return a;
		};
		for (bool changed = true; changed;) {
			changed = false;
			for (uint32_t index = 1; index < count; index++) {
				uint32_t next = UINT32_MAX;
				for (const auto* predecessor: order[index]->ImmPredecessors()) {
					const auto found = m_index.find(predecessor);
					if (found == m_index.end() || idom[found->second] == UINT32_MAX) {
						continue;
					}
					next = next == UINT32_MAX ? found->second : intersect(found->second, next);
				}
				if (next != idom[index]) {
					idom[index] = next;
					changed     = true;
				}
			}
		}

		std::vector<std::vector<uint32_t>> children(count);
		for (uint32_t index = 1; index < count; index++) {
			if (idom[index] != UINT32_MAX) {
				children[idom[index]].push_back(index);
			}
		}
		m_enter.assign(count, 0);
		m_exit.assign(count, 0);
		uint32_t                                 clock = 0;
		std::vector<std::pair<uint32_t, size_t>> stack {{0u, 0}};
		m_enter[0] = clock++;
		while (!stack.empty()) {
			const auto node = stack.back().first;
			if (stack.back().second < children[node].size()) {
				const auto child = children[node][stack.back().second++];
				m_enter[child]   = clock++;
				stack.emplace_back(child, 0);
				continue;
			}
			m_exit[node] = clock++;
			stack.pop_back();
		}
	}

	[[nodiscard]] bool StrictlyDominates(const Block* dominator, const Block* block) const {
		if (dominator == block) {
			return false;
		}
		const auto outer = m_index.find(dominator);
		const auto inner = m_index.find(block);
		if (outer == m_index.end() || inner == m_index.end()) {
			return false;
		}
		return m_enter[outer->second] < m_enter[inner->second] &&
		       m_exit[inner->second] < m_exit[outer->second];
	}

	// True when control can run from the start of from to the start of to, also when they match.
	[[nodiscard]] bool Reaches(const Block* from, const Block* to) {
		const auto source = m_index.find(from);
		const auto target = m_index.find(to);
		if (source == m_index.end() || target == m_index.end()) {
			return true;
		}
		auto [found, inserted] = m_reach.try_emplace(source->second);
		auto& bits             = found->second;
		if (inserted) {
			bits.assign((m_index.size() + 63u) / 64u, 0);
			std::vector<const Block*> work {from};
			bits[source->second / 64u] |= uint64_t {1} << (source->second % 64u);
			while (!work.empty()) {
				const auto* block = work.back();
				work.pop_back();
				for (const auto* successor: block->ImmSuccessors()) {
					const auto index = m_index.find(successor);
					if (index == m_index.end()) {
						continue;
					}
					auto&      word = bits[index->second / 64u];
					const auto bit  = uint64_t {1} << (index->second % 64u);
					if ((word & bit) == 0) {
						word |= bit;
						work.push_back(successor);
					}
				}
			}
		}
		return ((bits[target->second / 64u] >> (target->second % 64u)) & 1u) != 0;
	}

private:
	std::unordered_map<const Block*, uint32_t>          m_index;
	std::vector<uint32_t>                               m_enter;
	std::vector<uint32_t>                               m_exit;
	std::unordered_map<uint32_t, std::vector<uint64_t>> m_reach;
};

struct PairHash {
	size_t operator()(const std::pair<const Inst*, const Inst*>& key) const {
		const auto first  = std::hash<const Inst*> {}(key.first);
		const auto second = std::hash<const Inst*> {}(key.second);
		return first ^ (second + 0x9e3779b97f4a7c15ull + (first << 6) + (first >> 2));
	}
};

class SubsetProver {
public:
	explicit SubsetProver(const ControlFlow& flow): m_flow(flow) {}

	// True when every lane set in value is also set in root.
	bool Implies(Value value, const Inst* root) {
		value = Canonical(value);
		if (value.IsImmediate()) {
			return IsFalse(value);
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		if (inst == root) {
			return true;
		}
		const auto key = std::pair {inst, root};
		if (const auto found = m_memo.find(key); found != m_memo.end()) {
			return found->second;
		}
		// A phi cycle back to itself adds no lanes: assume it holds while proving the cycle.
		m_memo.emplace(key, true);
		const auto mark   = m_journal.size();
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
				// Every incoming edge must see the root instance the phi's readers see.
				result = m_flow.StrictlyDominates(root->Parent(), inst->Parent());
				for (size_t index = 0; index < inst->NumArgs() && result; index++) {
					result = Implies(inst->Arg(index), root);
				}
				break;
			default: result = CompareImplies(*inst, *root); break;
		}
		if (!result) {
			// Anything proven since this node started may have leaned on its assumption.
			for (size_t index = mark; index < m_journal.size(); index++) {
				if (const auto found = m_memo.find(m_journal[index]);
				    found != m_memo.end() && found->second) {
					m_memo.erase(found);
				}
			}
			m_journal.resize(mark);
		}
		m_memo[key] = result;
		m_journal.push_back(key);
		return result;
	}

private:
	const ControlFlow&                                                      m_flow;
	std::unordered_map<std::pair<const Inst*, const Inst*>, bool, PairHash> m_memo;
	std::vector<std::pair<const Inst*, const Inst*>>                        m_journal;
};

bool IsLaneLocal(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::Identity:
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
		case ValueOpcode::FPOrdNotEqual64:
		case ValueOpcode::FPOrdLessThan64:
		case ValueOpcode::FPOrdGreaterThan64:
		case ValueOpcode::FPUnordEqual64:
		case ValueOpcode::FPUnordNotEqual64:
		case ValueOpcode::FPUnordLessThan64:
		case ValueOpcode::FPUnordGreaterThan64:
		case ValueOpcode::FPUnordLessThanEqual64:
		case ValueOpcode::FPUnordGreaterThanEqual64:
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
		case ValueOpcode::FPIsNan64:
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
		case ValueOpcode::FPFract64:
		case ValueOpcode::MakeImageAddress: return true;
		default: return false;
	}
}

// Accesses that touch memory only in the lanes of their trailing U1 operand.
bool IsLaneGated(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::LoadAddressU8:
		case ValueOpcode::LoadAddressU16:
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::LoadDeviceAddressU32:
		case ValueOpcode::StoreAddressU8:
		case ValueOpcode::StoreAddressU16:
		case ValueOpcode::StoreAddressU32:
		case ValueOpcode::LoadBufferU8:
		case ValueOpcode::LoadBufferU16:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::LoadBufferU32x4:
		case ValueOpcode::StoreBufferU8:
		case ValueOpcode::StoreBufferU16:
		case ValueOpcode::StoreBufferU32:
		case ValueOpcode::StoreBufferU32x2:
		case ValueOpcode::StoreBufferU32x3:
		case ValueOpcode::StoreBufferU32x4:
		case ValueOpcode::BufferAtomicSwap32:
		case ValueOpcode::BufferAtomicCmpSwap32:
		case ValueOpcode::BufferAtomicSwap64:
		case ValueOpcode::BufferAtomicIAdd32:
		case ValueOpcode::BufferAtomicIAdd64:
		case ValueOpcode::BufferAtomicISub32:
		case ValueOpcode::BufferAtomicSMin32:
		case ValueOpcode::BufferAtomicUMin32:
		case ValueOpcode::BufferAtomicUMin64:
		case ValueOpcode::BufferAtomicSMax32:
		case ValueOpcode::BufferAtomicUMax32:
		case ValueOpcode::BufferAtomicAnd32:
		case ValueOpcode::BufferAtomicAnd64:
		case ValueOpcode::BufferAtomicOr32:
		case ValueOpcode::BufferAtomicOr64:
		case ValueOpcode::BufferAtomicXor32:
		case ValueOpcode::BufferAtomicFMin32:
		case ValueOpcode::BufferAtomicFMax32:
		case ValueOpcode::LoadSharedU8:
		case ValueOpcode::LoadSharedU16:
		case ValueOpcode::LoadSharedU32:
		case ValueOpcode::LoadSharedU32x2:
		case ValueOpcode::LoadSharedU32x3:
		case ValueOpcode::LoadSharedU32x4:
		case ValueOpcode::WriteSharedU8:
		case ValueOpcode::WriteSharedU16:
		case ValueOpcode::WriteSharedU32:
		case ValueOpcode::WriteSharedU32x2:
		case ValueOpcode::WriteSharedU32x3:
		case ValueOpcode::WriteSharedU32x4:
		case ValueOpcode::SharedAtomicFMin32:
		case ValueOpcode::SharedAtomicFMax32:
		case ValueOpcode::SharedAtomicSwap32:
		case ValueOpcode::SharedAtomicIAdd32:
		case ValueOpcode::SharedAtomicIAdd64:
		case ValueOpcode::SharedAtomicISub32:
		case ValueOpcode::SharedAtomicInc32:
		case ValueOpcode::SharedAtomicDec32:
		case ValueOpcode::SharedAtomicSMin32:
		case ValueOpcode::SharedAtomicUMin32:
		case ValueOpcode::SharedAtomicSMax32:
		case ValueOpcode::SharedAtomicUMax32:
		case ValueOpcode::SharedAtomicUMax64:
		case ValueOpcode::SharedAtomicCmpSwap32:
		case ValueOpcode::SharedAtomicCmpSwap64:
		case ValueOpcode::SharedAtomicAnd32:
		case ValueOpcode::SharedAtomicOr32:
		case ValueOpcode::SharedAtomicOr64:
		case ValueOpcode::SharedAtomicXor32:
		case ValueOpcode::SharedAtomicMaskedOr32:
		case ValueOpcode::ImageRead:
		case ValueOpcode::ImageWrite:
		case ValueOpcode::ImageSampleRaw:
		case ValueOpcode::ImageGatherRaw:
		case ValueOpcode::ImageAtomicCompareSwap32:
		case ValueOpcode::ImageAtomicSwap32:
		case ValueOpcode::ImageAtomicSwap64:
		case ValueOpcode::ImageAtomicIAdd32:
		case ValueOpcode::ImageAtomicIAdd64:
		case ValueOpcode::ImageAtomicSMin32:
		case ValueOpcode::ImageAtomicUMin32:
		case ValueOpcode::ImageAtomicUMin64:
		case ValueOpcode::ImageAtomicSMax32:
		case ValueOpcode::ImageAtomicUMax32:
		case ValueOpcode::ImageAtomicUMax64:
		case ValueOpcode::ImageAtomicAnd32:
		case ValueOpcode::ImageAtomicAnd64:
		case ValueOpcode::ImageAtomicOr32:
		case ValueOpcode::ImageAtomicOr64:
		case ValueOpcode::ImageAtomicXor32:
		case ValueOpcode::ImageAtomicXor64:
		case ValueOpcode::ImageAtomicFMin32:
		case ValueOpcode::ImageAtomicFMax32: return true;
		default: return false;
	}
}

// One lane-set operand of a conjunction: the lanes a U1 value holds, or those it leaves out.
struct Literal {
	const Inst* mask    = nullptr;
	bool        negated = false;

	bool operator==(const Literal&) const = default;
};

// Lanes a value must keep: all of them, or the union of the conjunctions in terms.
struct Demand {
	bool                              all = false;
	std::vector<std::vector<Literal>> terms;
};

constexpr size_t MaxTerms    = 16;
constexpr size_t MaxLiterals = 8;

class DemandAnalysis {
public:
	explicit DemandAnalysis(const Program& program)
	    : m_program(program), m_flow(program.blocks), m_prover(m_flow) {}

	// select(c, a, select(d, b, z)) is select(c, a, z) when d holds no lane outside c.
	uint32_t BypassOverwrites(const std::vector<Inst*>& candidates) {
		uint32_t bypassed = 0;
		for (auto* select: candidates) {
			const auto* root = Canonical(select->Arg(0)).TryInstruction();
			for (;;) {
				const auto* inner = select->Arg(2).Resolve().TryInstruction();
				if (inner == nullptr || inner->GetOpcode() != ValueOpcode::SelectU32 ||
				    inner->Flags<uint64_t>() != VgprWriteMaskFlag ||
				    !m_prover.Implies(inner->Arg(0), root)) {
					break;
				}
				select->SetArg(2, inner->Arg(2));
				bypassed++;
			}
		}
		return bypassed;
	}

	// Dropping one leaves its operands' demanded lanes as they were, so all of these go together.
	std::vector<Inst*> Removable(const std::vector<Inst*>& candidates) {
		Solve();
		std::vector<Inst*> removable;
		for (auto* select: candidates) {
			if (Unobserved(*select)) {
				removable.push_back(select);
			}
		}
		return removable;
	}

private:
	bool Implies(const Inst* lanes, const Inst* root) {
		return m_prover.Implies(Value(const_cast<Inst*>(lanes)), root);
	}

	bool Contradicts(const std::vector<Literal>& term) {
		for (const auto& held: term) {
			for (const auto& left: term) {
				if (!held.negated && left.negated && Implies(held.mask, left.mask)) {
					return true;
				}
			}
		}
		return false;
	}

	// True when the conjunction inner holds no lane outside outer.
	bool Within(const std::vector<Literal>& inner, const std::vector<Literal>& outer) {
		return std::ranges::all_of(outer, [&](const Literal& bound) {
			return std::ranges::any_of(inner, [&](const Literal& literal) {
				return literal.negated == bound.negated &&
				       (bound.negated ? Implies(bound.mask, literal.mask)
				                      : Implies(literal.mask, bound.mask));
			});
		});
	}

	bool AddTerm(Demand& demand, std::vector<Literal> term) {
		if (demand.all) {
			return false;
		}
		if (Contradicts(term)) {
			return false;
		}
		if (term.size() > MaxLiterals) {
			term.resize(MaxLiterals);
		}
		if (term.empty()) {
			demand.all = true;
			demand.terms.clear();
			return true;
		}
		if (std::ranges::any_of(demand.terms,
		                        [&](const auto& held) { return Within(term, held); })) {
			return false;
		}
		std::erase_if(demand.terms, [&](const auto& held) { return Within(held, term); });
		if (demand.terms.size() < MaxTerms) {
			demand.terms.push_back(std::move(term));
			return true;
		}
		// Widen the term sharing the most literals to just the shared ones.
		size_t best   = 0;
		size_t shared = 0;
		for (size_t index = 0; index < demand.terms.size(); index++) {
			const auto count = static_cast<size_t>(
			    std::ranges::count_if(demand.terms[index], [&](const Literal& literal) {
				    return std::ranges::find(term, literal) != term.end();
			    }));
			if (count > shared) {
				best   = index;
				shared = count;
			}
		}
		if (shared == 0) {
			demand.all = true;
			demand.terms.clear();
			return true;
		}
		auto joined = std::move(demand.terms[best]);
		std::erase_if(joined, [&](const Literal& literal) {
			return std::ranges::find(term, literal) == term.end();
		});
		demand.terms.erase(demand.terms.begin() + static_cast<std::ptrdiff_t>(best));
		std::erase_if(demand.terms, [&](const auto& held) { return Within(held, joined); });
		demand.terms.push_back(std::move(joined));
		return true;
	}

	static std::optional<Demand> Restrict(const Demand& demand, Value condition, bool negated) {
		const auto canonical = Canonical(condition);
		if (canonical.IsImmediate()) {
			if (canonical.GetType() != Type::U1) {
				return demand;
			}
			return canonical.U1() != negated ? std::optional<Demand> {demand} : std::nullopt;
		}
		const Literal literal {canonical.TryInstruction(), negated};
		if (literal.mask == nullptr) {
			return demand;
		}
		Demand restricted;
		if (demand.all) {
			restricted.terms.push_back({literal});
			return restricted;
		}
		for (auto term: demand.terms) {
			if (std::ranges::find(term, literal) == term.end()) {
				term.push_back(literal);
			}
			restricted.terms.push_back(std::move(term));
		}
		return restricted;
	}

	void Require(Value value, const Demand& demand) {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr || (!demand.all && demand.terms.empty())) {
			return;
		}
		auto [found, inserted] = m_index.try_emplace(inst, static_cast<uint32_t>(m_values.size()));
		if (inserted) {
			m_values.push_back(inst);
			m_demands.emplace_back();
			m_queued.push_back(0);
		}
		const auto index   = found->second;
		bool       changed = false;
		if (demand.all) {
			if (!m_demands[index].all) {
				m_demands[index] = Demand {.all = true};
				changed          = true;
			}
		} else {
			for (const auto& term: demand.terms) {
				changed |= AddTerm(m_demands[index], term);
			}
		}
		if (changed && m_queued[index] == 0) {
			m_queued[index] = 1;
			m_work.push_back(index);
		}
	}

	void Require(Value value, const std::optional<Demand>& demand) {
		if (demand) {
			Require(value, *demand);
		}
	}

	void RequireAll(const Inst& inst) {
		for (size_t operand = 0; operand < inst.NumArgs(); operand++) {
			Require(inst.Arg(operand), Demand {.all = true});
		}
	}

	bool IsGated(const Inst& inst) const {
		if (!IsLaneGated(inst.GetOpcode())) {
			return false;
		}
		// Scalar-address loads read in every lane.
		if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::None) {
			const auto index = inst.Flags<MemoryFlags>().index;
			return index < m_program.memory_info.size() &&
			       m_program.memory_info[index].kind != ResourceKind::ScalarAddress;
		}
		return true;
	}

	void Seed() {
		for (const auto* block: m_program.blocks) {
			for (const auto& value: {block->condition, block->indirect_target}) {
				if (!value.IsEmpty()) {
					Require(value, Demand {.all = true});
				}
			}
			for (const auto& inst: *block) {
				const auto opcode = inst.GetOpcode();
				if (IsGated(inst)) {
					const auto gate = inst.Arg(inst.NumArgs() - 1);
					Require(gate, Demand {.all = true});
					const auto active = Restrict(Demand {.all = true}, gate, false);
					for (size_t operand = 0; operand + 1 < inst.NumArgs(); operand++) {
						Require(inst.Arg(operand), active);
					}
					continue;
				}
				if (IsLaneLocal(opcode) || opcode == ValueOpcode::Phi ||
				    opcode == ValueOpcode::LogicalAnd || opcode == ValueOpcode::SelectU1 ||
				    opcode == ValueOpcode::SelectU32 || opcode == ValueOpcode::SelectF32 ||
				    opcode == ValueOpcode::SelectF64) {
					continue;
				}
				RequireAll(inst);
			}
		}
	}

	// Lane sets block can reach again may be recomputed before the phi is read.
	Demand AcrossEdge(const Demand& demand, const Block* block, const Block* predecessor) {
		Demand incoming {.all = demand.all};
		for (const auto& term: demand.terms) {
			std::vector<Literal> held;
			bool                 empty = false;
			for (const auto& literal: term) {
				const auto* mask = literal.mask;
				if (mask->GetOpcode() == ValueOpcode::Phi && mask->Parent() == block) {
					const Inst* edge = nullptr;
					for (size_t index = 0; index < mask->NumArgs(); index++) {
						if (mask->PhiBlock(index) != predecessor) {
							continue;
						}
						const auto value = Canonical(mask->Arg(index));
						if (value.IsImmediate()) {
							empty = value.GetType() == Type::U1 && value.U1() == literal.negated;
						} else {
							edge = value.TryInstruction();
						}
						break;
					}
					if (edge != nullptr) {
						held.push_back({edge, literal.negated});
					}
					continue;
				}
				if (m_flow.StrictlyDominates(mask->Parent(), block) ||
				    !m_flow.Reaches(block, mask->Parent())) {
					held.push_back(literal);
				}
			}
			if (empty) {
				continue;
			}
			if (held.empty()) {
				return Demand {.all = true};
			}
			AddTerm(incoming, std::move(held));
		}
		return incoming;
	}

	void Propagate(const Inst& inst, const Demand& demand) {
		const auto opcode = inst.GetOpcode();
		switch (opcode) {
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectF32:
			case ValueOpcode::SelectF64:
				Require(inst.Arg(0), demand);
				Require(inst.Arg(1), Restrict(demand, inst.Arg(0), false));
				Require(inst.Arg(2), Restrict(demand, inst.Arg(0), true));
				return;
			case ValueOpcode::LogicalAnd:
				Require(inst.Arg(0), demand);
				Require(inst.Arg(1), Restrict(demand, inst.Arg(0), false));
				return;
			case ValueOpcode::LogicalOr:
				Require(inst.Arg(0), demand);
				Require(inst.Arg(1), Restrict(demand, inst.Arg(0), true));
				return;
			case ValueOpcode::Phi:
				for (size_t index = 0; index < inst.NumArgs(); index++) {
					Require(inst.Arg(index),
					        AcrossEdge(demand, inst.Parent(), inst.PhiBlock(index)));
				}
				return;
			default: break;
		}
		if (IsLaneLocal(opcode)) {
			for (size_t operand = 0; operand < inst.NumArgs(); operand++) {
				Require(inst.Arg(operand), demand);
			}
			return;
		}
		// Sampling with a converted format reads its coordinates in inactive lanes too.
		if ((opcode == ValueOpcode::ImageSampleRaw || opcode == ValueOpcode::ImageGatherRaw) &&
		    IsGated(inst)) {
			for (size_t operand = 0; operand + 1 < inst.NumArgs(); operand++) {
				Require(inst.Arg(operand), demand);
			}
		}
	}

	void Solve() {
		m_index.clear();
		m_values.clear();
		m_demands.clear();
		m_queued.clear();
		m_work.clear();
		Seed();
		while (!m_work.empty()) {
			const auto index = m_work.front();
			m_work.pop_front();
			m_queued[index]   = 0;
			const auto demand = m_demands[index];
			Propagate(*m_values[index], demand);
		}
	}

	bool Unobserved(const Inst& select) {
		const auto found = m_index.find(&select);
		if (found == m_index.end()) {
			return true;
		}
		const auto& demand = m_demands[found->second];
		if (demand.all) {
			return false;
		}
		const auto* root = Canonical(select.Arg(0)).TryInstruction();
		return std::ranges::all_of(demand.terms, [&](const auto& term) {
			return Contradicts(term) || std::ranges::any_of(term, [&](const Literal& literal) {
				       return !literal.negated && Implies(literal.mask, root);
			       });
		});
	}

	const Program&                            m_program;
	ControlFlow                               m_flow;
	SubsetProver                              m_prover;
	std::unordered_map<const Inst*, uint32_t> m_index;
	std::vector<const Inst*>                  m_values;
	std::vector<Demand>                       m_demands;
	std::vector<uint8_t>                      m_queued;
	std::deque<uint32_t>                      m_work;
};

} // namespace

void OverrideDeadLaneWriteMasks(int mode) {
	g_override.store(mode);
}

uint32_t EliminateUndemandedWriteMasks(Program& program) {
	if (program.stage != ShaderType::Compute || g_override.load() == 0) {
		return 0;
	}
	std::vector<Inst*> candidates;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::SelectU32 &&
			    inst.Flags<uint64_t>() == VgprWriteMaskFlag &&
			    Canonical(inst.Arg(0)).TryInstruction() != nullptr) {
				candidates.push_back(&inst);
			}
		}
	}
	if (candidates.empty()) {
		return 0;
	}
	DemandAnalysis analysis(program);
	const auto     bypassed  = analysis.BypassOverwrites(candidates);
	const auto     removable = analysis.Removable(candidates);
	for (auto* select: removable) {
		select->ReplaceUsesWith(select->Arg(1));
	}
	return bypassed + static_cast<uint32_t>(removable.size());
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
