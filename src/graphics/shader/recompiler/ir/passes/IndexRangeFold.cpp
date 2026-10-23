#include "graphics/shader/recompiler/ir/passes/IndexRangeFold.h"

#include <algorithm>
#include <bit>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t Top = 0xffffffffu;

uint32_t CoveringMask(uint32_t x) {
	return x == 0u ? 0u : Top >> std::countl_zero(x);
}

struct Guard {
	const Inst*          value = nullptr;
	Value                limit;
	bool                 inclusive = false;
	std::vector<uint8_t> holds;
};

class IndexRange {
public:
	explicit IndexRange(Program& program): m_program(program) {}

	uint32_t Run() {
		if (!BuildGraph()) {
			return 0;
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() != ValueOpcode::IEqual32) {
					continue;
				}
				const auto a     = inst.Arg(0).Resolve();
				const auto b     = inst.Arg(1).Resolve();
				const bool left  = b.IsImmediate() && !a.IsImmediate();
				const bool right = a.IsImmediate() && !b.IsImmediate();
				if ((left || right) && (left ? b : a).GetType() == Type::U32 &&
				    (left ? b : a).U32() != 0u) {
					m_assumed.insert(&inst);
				}
			}
		}
		if (m_assumed.empty()) {
			return 0;
		}
		BuildGuards();
		BuildOrder();
		for (uint32_t round = 0; round < MaxRounds; round++) {
			if (!Solve()) {
				return 0;
			}
			std::vector<const Inst*> refuted;
			for (const auto* compare: m_assumed) {
				const auto a       = compare->Arg(0).Resolve();
				const auto b       = compare->Arg(1).Resolve();
				const auto operand = a.IsImmediate() ? b : a;
				const auto limit   = (a.IsImmediate() ? a : b).U32();
				if (Bound(operand, BlockOf(compare)) >= limit) {
					refuted.push_back(compare);
				}
			}
			if (refuted.empty()) {
				uint32_t folded = 0;
				for (const auto* compare: m_assumed) {
					const_cast<Inst*>(compare)->ReplaceUsesWith(Value(false));
					folded++;
				}
				return folded;
			}
			for (const auto* compare: refuted) {
				m_assumed.erase(compare);
			}
			if (m_assumed.empty()) {
				return 0;
			}
		}
		return 0;
	}

private:
	static constexpr uint32_t MaxRounds     = 16;
	static constexpr uint32_t MaxSweeps     = 512;
	static constexpr uint32_t ExactRaises   = 4;
	static constexpr uint32_t WidenedRaises = 40;

	bool BuildGraph() {
		const auto count = m_program.blocks.size();
		if (count == 0u) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			if (!m_index.emplace(m_program.blocks[i], i).second) {
				return false;
			}
		}
		m_successors.assign(count, {});
		m_predecessors.assign(count, {});
		for (uint32_t i = 0; i < count; i++) {
			const auto&               terminator = m_program.blocks[i]->terminator;
			std::vector<const Block*> targets;
			switch (terminator.kind) {
				case CFG::TerminatorKind::Branch: targets.push_back(terminator.true_block); break;
				case CFG::TerminatorKind::ConditionalBranch:
					targets = {terminator.true_block, terminator.false_block};
					break;
				case CFG::TerminatorKind::IndirectBranch:
					for (const auto& branch: terminator.cases) {
						targets.push_back(branch.target);
					}
					break;
				case CFG::TerminatorKind::Return: break;
				default: return false;
			}
			for (const auto* target: targets) {
				const auto found = m_index.find(target);
				if (found == m_index.end()) {
					return false;
				}
				m_successors[i].push_back(found->second);
				m_predecessors[found->second].push_back(i);
			}
		}
		return true;
	}

	uint32_t BlockOf(const Inst* inst) const {
		const auto found = m_index.find(inst->Parent());
		return found == m_index.end() ? UINT32_MAX : found->second;
	}

	bool IsUniform(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 64) {
			return false;
		}
		if (const auto found = m_uniform.find(inst); found != m_uniform.end()) {
			return found->second;
		}
		switch (inst->GetOpcode()) {
			case ValueOpcode::GetUserData:
			case ValueOpcode::ReadFirstLane:
			case ValueOpcode::ReadLane:
			case ValueOpcode::ReadConst:
			case ValueOpcode::ReadConstBuffer: m_uniform[inst] = true; return true;
			case ValueOpcode::Phi:
			case ValueOpcode::IAdd32:
			case ValueOpcode::ISub32:
			case ValueOpcode::IMul32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32:
			case ValueOpcode::UMin32:
			case ValueOpcode::UMax32:
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::SelectU32:
			case ValueOpcode::ULessThan32:
			case ValueOpcode::ULessThanEqual32:
			case ValueOpcode::UGreaterThan32:
			case ValueOpcode::UGreaterThanEqual32:
			case ValueOpcode::IEqual32:
			case ValueOpcode::INotEqual32:
			case ValueOpcode::LogicalAnd:
			case ValueOpcode::LogicalOr:
			case ValueOpcode::LogicalNot: break;
			default: m_uniform[inst] = false; return false;
		}
		m_uniform[inst] = true;
		for (size_t i = 0; i < inst->NumArgs(); i++) {
			if (!IsUniform(inst->Arg(i), depth + 1)) {
				m_uniform[inst] = false;
				return false;
			}
		}
		return true;
	}

	void AddGuard(Value value, Value limit, bool inclusive, uint32_t from, uint32_t to) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetType() != Type::U32 || !IsUniform(value)) {
			return;
		}
		const auto definition = BlockOf(inst);
		if (definition == UINT32_MAX) {
			return;
		}
		const auto           count = m_program.blocks.size();
		std::vector<uint8_t> in(count, 1u);
		in[0]           = 0u;
		const auto edge = [&](uint32_t pred, uint32_t succ) -> uint8_t {
			if (pred == from && succ == to) return 1u;
			return pred == definition ? 0u : in[pred];
		};
		for (bool changed = true; changed;) {
			changed = false;
			for (uint32_t b = 1; b < count; b++) {
				uint8_t value_in = m_predecessors[b].empty() ? 0u : 1u;
				for (const auto pred: m_predecessors[b]) {
					value_in &= edge(pred, b);
				}
				if (value_in != in[b]) {
					in[b]   = value_in;
					changed = true;
				}
			}
		}
		Guard guard {.value = inst, .limit = limit.Resolve(), .inclusive = inclusive};
		guard.holds.assign(count, 0u);
		for (uint32_t b = 0; b < count; b++) {
			guard.holds[b] = in[b] != 0u && b != definition ? 1u : 0u;
		}
		m_guards_by_value[inst].push_back(static_cast<uint32_t>(m_guards.size()));
		m_guards.push_back(std::move(guard));
	}

	void CollectFacts(Value value, bool positive, uint32_t from, uint32_t to, uint32_t depth = 0) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 16) {
			return;
		}
		const auto a = inst->NumArgs() > 0 ? inst->Arg(0) : Value {};
		const auto b = inst->NumArgs() > 1 ? inst->Arg(1) : Value {};
		switch (inst->GetOpcode()) {
			case ValueOpcode::LogicalNot: CollectFacts(a, !positive, from, to, depth + 1); return;
			case ValueOpcode::LogicalAnd:
				if (positive) {
					CollectFacts(a, true, from, to, depth + 1);
					CollectFacts(b, true, from, to, depth + 1);
				}
				return;
			case ValueOpcode::LogicalOr:
				if (!positive) {
					CollectFacts(a, false, from, to, depth + 1);
					CollectFacts(b, false, from, to, depth + 1);
				}
				return;
			case ValueOpcode::ULessThan32:
				positive ? AddGuard(a, b, false, from, to) : AddGuard(b, a, true, from, to);
				return;
			case ValueOpcode::ULessThanEqual32:
				positive ? AddGuard(a, b, true, from, to) : AddGuard(b, a, false, from, to);
				return;
			case ValueOpcode::UGreaterThan32:
				positive ? AddGuard(b, a, false, from, to) : AddGuard(a, b, true, from, to);
				return;
			case ValueOpcode::UGreaterThanEqual32:
				positive ? AddGuard(b, a, true, from, to) : AddGuard(a, b, false, from, to);
				return;
			default: return;
		}
	}

	void BuildGuards() {
		for (uint32_t h = 0; h < m_program.blocks.size(); h++) {
			const auto& info = *m_program.blocks[h];
			if (info.terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
			    info.condition.IsEmpty() ||
			    info.terminator.true_block == info.terminator.false_block) {
				continue;
			}
			for (const bool taken: {true, false}) {
				const auto to =
				    m_index.at(taken ? info.terminator.true_block : info.terminator.false_block);
				bool  positive = taken;
				bool  usable   = false;
				Value value    = info.condition;
				while (const auto* inst = value.Resolve().TryInstruction()) {
					if (inst->GetOpcode() == ValueOpcode::LogicalNot && !usable) {
						positive = !positive;
					} else if (inst->GetOpcode() == ValueOpcode::ConditionRef && !usable) {
						const auto kind    = inst->Flags<CFG::BranchCondition>();
						const bool scalar  = kind == CFG::BranchCondition::SccZero ||
						                     kind == CFG::BranchCondition::SccNonZero ||
						                     kind == CFG::BranchCondition::ScalarInstruction;
						const bool zero    = kind == CFG::BranchCondition::ExecZero ||
						                     kind == CFG::BranchCondition::VccZero;
						const bool nonzero = kind == CFG::BranchCondition::ExecNonZero ||
						                     kind == CFG::BranchCondition::VccNonZero;
						if (!scalar && !zero && !nonzero) break;
						usable = scalar || zero != positive;
						if (!usable) break;
					} else {
						break;
					}
					value = inst->Arg(0);
				}
				if (usable) {
					CollectFacts(value, positive, h, to);
				}
			}
		}
	}

	bool IsKnown(Value condition, bool truth, uint32_t depth = 0) const {
		condition = condition.Resolve();
		if (condition.IsImmediate()) {
			return condition.GetType() == Type::U1 && condition.U1() == truth;
		}
		const auto* inst = condition.TryInstruction();
		if (inst == nullptr || depth > 16) {
			return false;
		}
		if (!truth && m_assumed.contains(inst)) {
			return true;
		}
		switch (inst->GetOpcode()) {
			case ValueOpcode::LogicalNot: return IsKnown(inst->Arg(0), !truth, depth + 1);
			case ValueOpcode::LogicalAnd:
				return truth ? IsKnown(inst->Arg(0), true, depth + 1) &&
				                   IsKnown(inst->Arg(1), true, depth + 1)
				             : IsKnown(inst->Arg(0), false, depth + 1) ||
				                   IsKnown(inst->Arg(1), false, depth + 1);
			case ValueOpcode::LogicalOr:
				return truth ? IsKnown(inst->Arg(0), true, depth + 1) ||
				                   IsKnown(inst->Arg(1), true, depth + 1)
				             : IsKnown(inst->Arg(0), false, depth + 1) &&
				                   IsKnown(inst->Arg(1), false, depth + 1);
			default: return false;
		}
	}

	uint32_t Plain(Value value) const {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return value.GetType() == Type::U32 ? value.U32() : Top;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return Top;
		}
		if (const auto found = m_slots.find(inst); found != m_slots.end()) {
			return m_bound[found->second];
		}
		return inst->GetType() != Type::U32 || !m_index.contains(inst->Parent()) ? Top : 0u;
	}

	uint32_t Bound(Value value, uint32_t block) const {
		auto        result = Plain(value);
		const auto* inst   = value.Resolve().TryInstruction();
		if (inst == nullptr || block == UINT32_MAX) {
			return result;
		}
		const auto guards = m_guards_by_value.find(inst);
		if (guards == m_guards_by_value.end()) {
			return result;
		}
		for (const auto index: guards->second) {
			const auto& guard = m_guards[index];
			if (guard.holds[block] == 0u) {
				continue;
			}
			const auto limit = Plain(guard.limit);
			result = std::min(result, guard.inclusive ? limit : (limit == 0u ? 0u : limit - 1u));
		}
		return result;
	}

	uint32_t Transfer(const Inst& inst, uint32_t block) const {
		const auto arg       = [&](size_t i) { return Bound(inst.Arg(i), block); };
		const auto immediate = [&](size_t i, uint32_t& out) {
			const auto value = inst.Arg(i).Resolve();
			if (!value.IsImmediate() || value.GetType() != Type::U32) return false;
			out = value.U32();
			return true;
		};
		switch (inst.GetOpcode()) {
			case ValueOpcode::IAdd32: {
				const auto sum = static_cast<uint64_t>(arg(0)) + arg(1);
				return sum > Top ? Top : static_cast<uint32_t>(sum);
			}
			case ValueOpcode::ShiftLeftLogical32: {
				uint32_t   shift = 0;
				const auto value = arg(0);
				if (value == 0u) return 0u;
				if (!immediate(1, shift)) return Top;
				const auto shifted = static_cast<uint64_t>(value) << (shift & 31u);
				return shifted > Top ? Top : static_cast<uint32_t>(shifted);
			}
			case ValueOpcode::ShiftRightLogical32: {
				uint32_t shift = 0;
				return immediate(1, shift) ? arg(0) >> (shift & 31u) : arg(0);
			}
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::UMin32: return std::min(arg(0), arg(1));
			case ValueOpcode::UMax32: return std::max(arg(0), arg(1));
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32: return CoveringMask(std::max(arg(0), arg(1)));
			case ValueOpcode::BitFieldUExtract: {
				uint32_t count = 0;
				return immediate(2, count) && count < 32u ? (1u << count) - 1u : Top;
			}
			case ValueOpcode::SelectU32:
				if (IsKnown(inst.Arg(0), false)) return arg(2);
				if (IsKnown(inst.Arg(0), true)) return arg(1);
				return std::max(arg(1), arg(2));
			case ValueOpcode::Phi: {
				uint32_t result = 0;
				for (size_t i = 0; i < inst.NumArgs(); i++) {
					const auto found = m_index.find(inst.PhiBlock(i));
					result           = std::max(
					    result,
					    Bound(inst.Arg(i), found == m_index.end() ? UINT32_MAX : found->second));
				}
				return result;
			}
			default: return Top;
		}
	}

	void AddReads(Value value, std::vector<const Inst*>& reads) const {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr) {
			return;
		}
		reads.push_back(inst);
		if (const auto guards = m_guards_by_value.find(inst); guards != m_guards_by_value.end()) {
			for (const auto index: guards->second) {
				if (const auto* limit = m_guards[index].limit.Resolve().TryInstruction()) {
					reads.push_back(limit);
				}
			}
		}
	}

	void BuildOrder() {
		for (uint32_t b = 0; b < m_program.blocks.size(); b++) {
			for (const auto& inst: *m_program.blocks[b]) {
				if (inst.GetType() == Type::U32) {
					m_slots.emplace(&inst, static_cast<uint32_t>(m_order.size()));
					m_order.push_back({&inst, b});
				}
			}
		}
		m_readers.assign(m_order.size(), {});
		std::vector<const Inst*> reads;
		for (uint32_t slot = 0; slot < m_order.size(); slot++) {
			const auto& inst = *m_order[slot].inst;
			reads.clear();
			for (size_t i = 0; i < inst.NumArgs(); i++) {
				AddReads(inst.Arg(i), reads);
			}
			for (const auto* read: reads) {
				if (const auto found = m_slots.find(read); found != m_slots.end()) {
					auto& readers = m_readers[found->second];
					if (readers.empty() || readers.back() != slot) {
						readers.push_back(slot);
					}
				}
			}
		}
	}

	bool Solve() {
		m_bound.assign(m_order.size(), 0u);
		m_raises.assign(m_order.size(), 0u);
		std::vector<uint8_t> dirty(m_order.size(), 1u);
		for (uint32_t sweep = 0; sweep < MaxSweeps; sweep++) {
			bool changed = false;
			for (uint32_t slot = 0; slot < m_order.size(); slot++) {
				if (dirty[slot] == 0u) {
					continue;
				}
				dirty[slot]       = 0u;
				const auto& entry = m_order[slot];
				auto        next  = Transfer(*entry.inst, entry.block);
				auto&       bound = m_bound[slot];
				if (next <= bound) {
					continue;
				}
				const auto raises = ++m_raises[slot];
				if (raises > WidenedRaises) {
					next = Top;
				} else if (raises > ExactRaises) {
					next = CoveringMask(next);
				}
				bound   = next;
				changed = true;
				for (const auto reader: m_readers[slot]) {
					dirty[reader] = 1u;
				}
			}
			if (!changed) {
				return true;
			}
		}
		return false;
	}

	struct Ordered {
		const Inst* inst  = nullptr;
		uint32_t    block = 0;
	};

	Program&                                               m_program;
	std::unordered_map<const Block*, uint32_t>             m_index;
	std::vector<std::vector<uint32_t>>                     m_successors;
	std::vector<std::vector<uint32_t>>                     m_predecessors;
	std::vector<Guard>                                     m_guards;
	std::unordered_map<const Inst*, std::vector<uint32_t>> m_guards_by_value;
	std::vector<Ordered>                                   m_order;
	std::vector<std::vector<uint32_t>>                     m_readers;
	std::unordered_map<const Inst*, uint32_t>              m_slots;
	std::vector<uint32_t>                                  m_bound;
	std::vector<uint32_t>                                  m_raises;
	std::unordered_set<const Inst*>                        m_assumed;
	std::unordered_map<const Inst*, bool>                  m_uniform;
};

} // namespace

uint32_t FoldUnreachableIndexCompares(Program& program) {
	return IndexRange(program).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
