#include "graphics/shader/recompiler/backend/spirv/SpirvForwarding.h"

#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>

#include <algorithm>
#include <span>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

constexpr int    MaxRounds    = 6;

struct Inst {
	uint32_t offset  = 0; // Word index of the first word.
	uint32_t op      = 0;
	uint32_t count   = 0;
	uint32_t type    = 0;
	uint32_t result  = 0;
	uint32_t first   = 0; // Word index of the first operand.
	bool     body    = false;
	bool     removed = false;
};

// Arithmetic, logic and conversions: no side effects, no literals among the operands, and each
// lane's result depends only on the same lane's operands.
bool IsLanePure(uint32_t op) {
	switch (op) {
		case spv::OpIAdd:
		case spv::OpISub:
		case spv::OpIMul:
		case spv::OpUDiv:
		case spv::OpSDiv:
		case spv::OpUMod:
		case spv::OpSRem:
		case spv::OpSMod:
		case spv::OpFAdd:
		case spv::OpFSub:
		case spv::OpFMul:
		case spv::OpFDiv:
		case spv::OpFRem:
		case spv::OpFMod:
		case spv::OpFNegate:
		case spv::OpSNegate:
		case spv::OpBitwiseAnd:
		case spv::OpBitwiseOr:
		case spv::OpBitwiseXor:
		case spv::OpNot:
		case spv::OpShiftLeftLogical:
		case spv::OpShiftRightLogical:
		case spv::OpShiftRightArithmetic:
		case spv::OpBitFieldInsert:
		case spv::OpBitFieldSExtract:
		case spv::OpBitFieldUExtract:
		case spv::OpBitReverse:
		case spv::OpBitCount:
		case spv::OpLogicalAnd:
		case spv::OpLogicalOr:
		case spv::OpLogicalNot:
		case spv::OpLogicalEqual:
		case spv::OpLogicalNotEqual:
		case spv::OpSelect:
		case spv::OpIEqual:
		case spv::OpINotEqual:
		case spv::OpUGreaterThan:
		case spv::OpSGreaterThan:
		case spv::OpUGreaterThanEqual:
		case spv::OpSGreaterThanEqual:
		case spv::OpULessThan:
		case spv::OpSLessThan:
		case spv::OpULessThanEqual:
		case spv::OpSLessThanEqual:
		case spv::OpFOrdEqual:
		case spv::OpFUnordEqual:
		case spv::OpFOrdNotEqual:
		case spv::OpFUnordNotEqual:
		case spv::OpFOrdLessThan:
		case spv::OpFUnordLessThan:
		case spv::OpFOrdGreaterThan:
		case spv::OpFUnordGreaterThan:
		case spv::OpFOrdLessThanEqual:
		case spv::OpFUnordLessThanEqual:
		case spv::OpFOrdGreaterThanEqual:
		case spv::OpFUnordGreaterThanEqual:
		case spv::OpIsNan:
		case spv::OpIsInf:
		case spv::OpConvertFToU:
		case spv::OpConvertFToS:
		case spv::OpConvertSToF:
		case spv::OpConvertUToF:
		case spv::OpUConvert:
		case spv::OpSConvert:
		case spv::OpFConvert:
		case spv::OpBitcast:
		case spv::OpCompositeConstruct:
		case spv::OpCompositeExtract: return true;
		default: return false;
	}
}

class Pass {
public:
	Pass(std::vector<uint32_t>& module, size_t min_body_words): m_words(module), m_min_body_words(min_body_words) {}

	void Run() {
		Parse();
		if (m_body_words < m_min_body_words) {
			return;
		}
		CancelBitcasts();
		ForwardSelects();
		RemoveDead();
		CancelBitcasts();
		RemoveDead();
		Write();
	}

private:
	static constexpr uint32_t None = 0xFFFFFFFFu;

	struct Use {
		uint32_t inst;
		uint32_t operand; // Operand index.
	};

	void Parse() {
		auto&      w     = m_words;
		const auto bound = w[3];
		m_type.assign(bound, 0);
		m_owner.assign(bound, None);
		bool in_function = false;
		for (size_t i = 5; i < w.size();) {
			Inst inst;
			inst.offset = static_cast<uint32_t>(i);
			inst.op     = w[i] & spv::OpCodeMask;
			inst.count  = w[i] >> spv::WordCountShift;
			if (inst.count == 0 || i + inst.count > w.size()) {
				m_ok = false;
				return;
			}
			bool has_result = false, has_type = false;
			spv::HasResultAndType(static_cast<spv::Op>(inst.op), &has_result, &has_type);
			uint32_t at = inst.offset + 1;
			if (has_type) inst.type = w[at++];
			if (has_result) inst.result = w[at++];
			inst.first = at;
			if (inst.op == spv::OpFunction) in_function = true;
			inst.body = in_function && inst.op != spv::OpFunction && inst.op != spv::OpFunctionEnd &&
			            inst.op != spv::OpFunctionParameter;
			if (inst.op == spv::OpFunctionEnd) in_function = false;
			if (inst.body) m_body_words += inst.count;
			if (inst.result != 0 && inst.result < bound) {
				if (inst.type != 0) m_type[inst.result] = inst.type;
				if (inst.body) m_owner[inst.result] = static_cast<uint32_t>(m_insts.size());
			}
			m_insts.push_back(inst);
			i += inst.count;
		}
		if (!m_ok) m_body_words = 0;
		m_repl.assign(bound, 0);
		m_gone.assign(bound, 0);
	}

	uint32_t& Operand(const Inst& inst, uint32_t index) { return m_words[inst.first + index]; }
	uint32_t  Operands(const Inst& inst) const { return inst.offset + inst.count - inst.first; }

	// Operand indices that hold ids. exact=false: other operands may be literals and the list is a
	// superset, good for counting uses but not for rewriting them.
	void IdOperands(const Inst& inst, std::vector<uint32_t>& out, bool& exact) {
		out.clear();
		exact          = true;
		const auto n   = Operands(inst);
		const auto all = [&](uint32_t from) {
			for (uint32_t i = from; i < n; i++) out.push_back(i);
		};
		if (IsLanePure(inst.op)) {
			if (inst.op == spv::OpCompositeExtract) {
				out.push_back(0);
			} else {
				all(0);
			}
			return;
		}
		switch (inst.op) {
			case spv::OpPhi:
			case spv::OpFunctionCall:
			case spv::OpAccessChain:
			case spv::OpInBoundsAccessChain:
			case spv::OpBranch:
			case spv::OpReturnValue: all(0); return;
			case spv::OpBranchConditional:
				all(0);
				out.resize(std::min<size_t>(out.size(), 3));
				return;
			case spv::OpLoad:
				out.push_back(0);
				exact = n == 1 || (n >= 2 && (Operand(inst, 1) & ~uint32_t(spv::MemoryAccessAlignedMask)) == 0);
				return;
			case spv::OpStore:
				out.push_back(0);
				out.push_back(1);
				exact = n == 2 || (n >= 3 && (Operand(inst, 2) & ~uint32_t(spv::MemoryAccessAlignedMask)) == 0);
				return;
			case spv::OpExtInst: all(2); return;
			default: exact = false; all(0); return;
		}
	}

	bool RemovableIfUnused(const Inst& inst) {
		if (inst.result == 0) return false;
		if (IsLanePure(inst.op) || inst.op == spv::OpPhi || inst.op == spv::OpAccessChain ||
		    inst.op == spv::OpInBoundsAccessChain) {
			return true;
		}
		if (inst.op == spv::OpLoad) {
			const auto n = Operands(inst);
			return n == 1 || (n >= 2 && (Operand(inst, 1) & ~uint32_t(spv::MemoryAccessAlignedMask)) == 0);
		}
		if (inst.op == spv::OpExtInst) {
			const auto number = Operand(inst, 1);
			return number != 35 && number != 51; // Modf and Frexp write through a pointer.
		}
		return false;
	}

	// Uses of each id, compressed by id: use_begin[id] .. use_begin[id + 1].
	void BuildUses() {
		const auto bound = static_cast<uint32_t>(m_type.size());
		m_use_begin.assign(bound + 2, 0);
		m_inexact.assign(bound, 0);
		std::vector<uint32_t> ids;
		bool                  exact = true;
		for (int pass = 0; pass < 2; pass++) {
			if (pass == 1) {
				uint32_t sum = 0;
				for (uint32_t id = 0; id <= bound; id++) {
					const auto n    = m_use_begin[id];
					m_use_begin[id] = sum;
					sum += n;
				}
				m_use_list.resize(sum);
				m_use_fill.assign(m_use_begin.begin(), m_use_begin.end());
			}
			for (uint32_t index = 0; index < m_insts.size(); index++) {
				const auto& inst = m_insts[index];
				if (!inst.body || inst.removed) continue;
				IdOperands(inst, ids, exact);
				for (const auto operand: ids) {
					const auto id = Operand(inst, operand);
					if (id >= bound) continue;
					if (pass == 0) {
						m_use_begin[id]++;
						if (!exact) m_inexact[id] = 1;
					} else {
						m_use_list[m_use_fill[id]++] = {index, operand};
					}
				}
			}
		}
	}

	[[nodiscard]] std::span<const Use> UsesOf(uint32_t id) const {
		if (id >= m_type.size()) return {};
		return {m_use_list.data() + m_use_begin[id], m_use_list.data() + m_use_begin[id + 1]};
	}

	uint32_t Resolve(uint32_t id) const {
		while (id < m_repl.size() && m_repl[id] != 0) id = m_repl[id];
		return id;
	}

	void CancelBitcasts() {
		// Bitcast(T, Bitcast(U, x)) with type(x) == T is x. Every use of the outer result must be at
		// an operand position known to be an id.
		BuildUses();
		std::vector<uint32_t> defs(m_type.size(), None);
		bool                  any = false;
		for (uint32_t index = 0; index < m_insts.size(); index++) {
			auto& inst = m_insts[index];
			if (!inst.body || inst.removed || inst.op != spv::OpBitcast) continue;
			const auto source = Resolve(Operand(inst, 0));
			if (source < defs.size() && defs[source] != None) {
				const auto inner = Resolve(Operand(m_insts[defs[source]], 0));
				if (inner < m_type.size() && m_type[inner] == inst.type && !m_inexact[inst.result]) {
					m_repl[inst.result] = inner;
					inst.removed        = true;
					any                 = true;
					continue;
				}
			}
			defs[inst.result] = index;
			Operand(inst, 0)  = source;
		}
		if (!any) return;
		std::vector<uint32_t> ids;
		bool                  exact = true;
		for (auto& inst: m_insts) {
			if (!inst.body || inst.removed) continue;
			IdOperands(inst, ids, exact);
			if (!exact) continue;
			for (const auto operand: ids) {
				auto& id = Operand(inst, operand);
				if (id < m_repl.size() && m_repl[id] != 0) id = Resolve(id);
			}
		}
		for (uint32_t id = 0; id < m_repl.size(); id++) {
			if (m_repl[id] != 0) m_gone[id] = 1;
		}
		std::fill(m_repl.begin(), m_repl.end(), 0);
	}

	bool Implies(uint32_t q, uint32_t p, int depth = 0) {
		if (q == p) return true;
		if (q >= m_owner.size() || m_owner[q] == None) return false;
		const auto& d = m_insts[m_owner[q]];
		if (d.removed || d.op != spv::OpLogicalAnd || depth >= 16) return false;
		const auto key = (static_cast<uint64_t>(q) << 32) | p;
		if (const auto found = m_implies.find(key); found != m_implies.end()) return found->second;
		const bool result = Implies(Operand(d, 0), p, depth + 1) || Implies(Operand(d, 1), p, depth + 1);
		m_implies[key]    = result;
		return result;
	}

	void ForwardSelects() {
		struct Allowed {
			bool                  ok = false;
			std::vector<uint32_t> predicates;
		};
		for (int round = 0; round < MaxRounds; round++) {
			BuildUses();
			m_implies.clear();
			// allowed[v]: the predicates of the selects that end every use chain of the pure value v.
			std::vector<uint32_t> allowed_index(m_type.size(), None);
			std::vector<Allowed>  pool;
			for (size_t k = m_insts.size(); k-- > 0;) {
				const auto& inst = m_insts[k];
				if (!inst.body || inst.removed || inst.result == 0 || !IsLanePure(inst.op)) continue;
				Allowed a;
				a.ok = true;
				for (const auto& use: UsesOf(inst.result)) {
					const auto& user = m_insts[use.inst];
					if (user.op == spv::OpSelect) {
						if (use.operand == 1 && Operand(user, 0) != inst.result &&
						    Operand(user, 2) != inst.result) {
							a.predicates.push_back(Operand(user, 0));
						} else {
							a.ok = false;
							break;
						}
					} else if (user.op == spv::OpLogicalAnd && Operand(user, 0) != Operand(user, 1)) {
						a.predicates.push_back(Operand(user, 1 - use.operand));
					} else if (IsLanePure(user.op) && user.result != 0) {
						if (allowed_index[user.result] == None || !pool[allowed_index[user.result]].ok) {
							a.ok = false;
							break;
						}
						const auto& other = pool[allowed_index[user.result]].predicates;
						a.predicates.insert(a.predicates.end(), other.begin(), other.end());
					} else {
						a.ok = false;
						break;
					}
				}
				std::sort(a.predicates.begin(), a.predicates.end());
				a.predicates.erase(std::unique(a.predicates.begin(), a.predicates.end()), a.predicates.end());
				allowed_index[inst.result] = static_cast<uint32_t>(pool.size());
				pool.push_back(std::move(a));
			}
			uint32_t forwarded = 0, skipped = 0;
			for (uint32_t index = 0; index < m_insts.size(); index++) {
				const auto& sel = m_insts[index];
				if (!sel.body || sel.removed || sel.op != spv::OpSelect) continue;
				const auto q = Operand(sel, 0), value = Operand(sel, 1);
				for (const auto& use: UsesOf(sel.result)) {
					auto& user = m_insts[use.inst];
					if (user.op == spv::OpSelect) {
						if (use.operand == 1 && Operand(user, 0) != sel.result &&
						    Implies(Operand(user, 0), q) && Operand(user, 2) != sel.result) {
							Operand(user, 1) = value;
							forwarded++;
						}
					} else if (IsLanePure(user.op) && user.result != 0 && allowed_index[user.result] != None) {
						const auto& a = pool[allowed_index[user.result]];
						if (a.ok && !a.predicates.empty() &&
						    std::ranges::all_of(a.predicates, [&](uint32_t q2) { return Implies(q2, q); })) {
							Operand(user, use.operand) = value;
							forwarded++;
						}
					}
				}
			}
			// Select(Q2, n2, Select(Q1, n1, o1)) with Q1 => Q2: the lanes of Q1 are overwritten by n2,
			// the others take o1.
			for (uint32_t index = 0; index < m_insts.size(); index++) {
				const auto& sel = m_insts[index];
				if (!sel.body || sel.removed || sel.op != spv::OpSelect) continue;
				const auto q2  = Operand(sel, 0);
				const auto old = Operand(sel, 2);
				if (old >= m_owner.size() || m_owner[old] == None) continue;
				const auto& inner = m_insts[m_owner[old]];
				if (!inner.removed && inner.op == spv::OpSelect && Implies(Operand(inner, 0), q2)) {
					Operand(sel, 2) = Operand(inner, 2);
					skipped++;
				}
			}
			if (forwarded + skipped == 0) break;
		}
	}

	void RemoveDead() {
		BuildUses();
		const auto            bound = static_cast<uint32_t>(m_type.size());
		std::vector<uint32_t> count(bound, 0);
		for (uint32_t id = 0; id < bound; id++) count[id] = m_use_begin[id + 1] - m_use_begin[id];
		std::vector<uint32_t> stack;
		for (uint32_t index = 0; index < m_insts.size(); index++) {
			const auto& inst = m_insts[index];
			if (inst.body && !inst.removed && RemovableIfUnused(inst) && count[inst.result] == 0) {
				stack.push_back(index);
			}
		}
		std::vector<uint32_t> ids;
		bool                  exact = true;
		while (!stack.empty()) {
			const auto index = stack.back();
			stack.pop_back();
			auto& inst = m_insts[index];
			if (inst.removed) continue;
			inst.removed = true;
			if (inst.result < bound) m_gone[inst.result] = 1;
			IdOperands(inst, ids, exact);
			for (const auto operand: ids) {
				const auto id = Operand(inst, operand);
				if (id >= bound || count[id] == 0) continue;
				if (--count[id] == 0 && m_owner[id] != None && !m_insts[m_owner[id]].removed &&
				    RemovableIfUnused(m_insts[m_owner[id]])) {
					stack.push_back(m_owner[id]);
				}
			}
		}
	}

	void Write() {
		std::vector<uint32_t> out;
		out.reserve(m_words.size());
		out.insert(out.end(), m_words.begin(), m_words.begin() + 5);
		for (const auto& inst: m_insts) {
			if (inst.removed) continue;
			// Decorations and names of the ids that are gone.
			if ((inst.op == spv::OpDecorate || inst.op == spv::OpName) && m_words[inst.first] < m_gone.size() &&
			    m_gone[m_words[inst.first]]) {
				continue;
			}
			out.insert(out.end(), m_words.begin() + inst.offset, m_words.begin() + inst.offset + inst.count);
		}
		m_words = std::move(out);
	}

	std::vector<uint32_t>&             m_words;
	size_t                             m_min_body_words;
	std::vector<Inst>                  m_insts;
	std::vector<uint32_t>              m_type;
	std::vector<uint32_t>              m_owner;
	std::vector<uint32_t>              m_use_begin;
	std::vector<uint32_t>              m_use_fill;
	std::vector<Use>                   m_use_list;
	std::vector<uint32_t>              m_repl;
	std::vector<uint8_t>               m_inexact;
	std::vector<uint8_t>               m_gone;
	std::unordered_map<uint64_t, bool> m_implies;
	size_t                             m_body_words = 0;
	bool                               m_ok         = true;
};

} // namespace

void ForwardMaskedSelects(std::vector<uint32_t>& module, size_t min_body_words) {
	if (module.size() < min_body_words) {
		return;
	}
	Pass(module, min_body_words).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
