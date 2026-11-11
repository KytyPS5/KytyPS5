#include "graphics/shader/recompiler/backend/spirv/SpirvForwarding.h"

#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>

#include <algorithm>
#include <span>
#include <unordered_map>
#include <unordered_set>

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
	Pass(std::vector<uint32_t>& module, size_t min_body_words, uint32_t fault_buffer)
	    : m_words(module), m_min_body_words(min_body_words), m_fault_buffer(fault_buffer) {}

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
		ValueNumbering();
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
		m_ptr_class.assign(bound, None);
		m_def_index.assign(bound, None);
		m_base.assign(bound, 0);
		m_deco.assign(bound, 0);
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
			if (inst.op == spv::OpTypePointer && inst.result < bound) m_ptr_class[inst.result] = w[inst.first];
			if (inst.op == spv::OpExtInstImport && w[inst.first] == 0x4c534c47u) m_glsl = inst.result;
			if (inst.op == spv::OpDecorate && inst.count >= 3 && w[inst.first] < bound) {
				uint64_t h = 0xcbf29ce484222325ull;
				for (uint32_t k = 1; k < inst.count - 1; k++) h = (h ^ w[inst.first + k]) * 0x100000001b3ull;
				m_deco[w[inst.first]] += h | 1;
			}
			if ((inst.op == spv::OpAccessChain || inst.op == spv::OpInBoundsAccessChain) && inst.result < bound) {
				const auto base = w[inst.first];
				m_base[inst.result] = base < bound && m_base[base] != 0 ? m_base[base] : base;
			}
			if (inst.result != 0 && inst.result < bound) {
				m_def_index[inst.result] = static_cast<uint32_t>(m_insts.size());
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

	// ---- Value numbering -------------------------------------------------------------------

	struct Function {
		uint32_t id    = 0;
		uint32_t begin = 0; // First instruction after OpFunction, parameters included.
		uint32_t end   = 0; // The OpFunctionEnd.
		bool     writes = false;
		bool     entry  = false;
		bool     pure   = false;
		std::vector<uint32_t> callees;
	};

	struct Block {
		uint32_t              label = 0;
		uint32_t              begin = 0;
		uint32_t              end   = 0; // One past the terminator.
		std::vector<uint32_t> succ;
	};

	uint32_t PointerClass(uint32_t pointer) const {
		if (pointer >= m_type.size()) return None;
		const auto type = m_type[pointer];
		return type < m_ptr_class.size() ? m_ptr_class[type] : None;
	}

	uint32_t BaseOf(uint32_t pointer) const {
		return pointer < m_base.size() && m_base[pointer] != 0 ? m_base[pointer] : pointer;
	}

	bool IsFaultPointer(uint32_t pointer) const {
		return m_fault_buffer != 0 && BaseOf(pointer) == m_fault_buffer;
	}

	static bool IsGvnPure(uint32_t op) {
		if (IsLanePure(op)) return true;
		switch (op) {
			case spv::OpAccessChain:
			case spv::OpInBoundsAccessChain:
			case spv::OpCopyObject:
			case spv::OpConvertPtrToU:
			case spv::OpConvertUToPtr:
			case spv::OpQuantizeToF16:
			case spv::OpVectorShuffle:
			case spv::OpCompositeInsert: return true;
			default: return false;
		}
	}

	void FindFunctions() {
		m_functions.clear();
		std::unordered_map<uint32_t, uint32_t> index;
		for (uint32_t i = 0; i < m_insts.size(); i++) {
			const auto& inst = m_insts[i];
			if (inst.op == spv::OpFunction) {
				Function f;
				f.id    = inst.result;
				f.begin = i + 1;
				index[f.id] = static_cast<uint32_t>(m_functions.size());
				m_functions.push_back(std::move(f));
			} else if (inst.op == spv::OpFunctionEnd && !m_functions.empty()) {
				m_functions.back().end = i;
			} else if (inst.op == spv::OpEntryPoint) {
				m_entry_ids.insert(Operand(inst, 1));
			}
		}
		for (auto& f: m_functions) {
			f.entry = m_entry_ids.contains(f.id);
			for (uint32_t i = f.begin; i < f.end; i++) {
				const auto& inst = m_insts[i];
				if (inst.removed) continue;
				switch (inst.op) {
					case spv::OpStore: {
						const auto cls = PointerClass(Operand(inst, 0));
						if (cls == None || cls == spv::StorageClassStorageBuffer ||
						    cls == spv::StorageClassPhysicalStorageBuffer || cls == spv::StorageClassUniform) {
							f.writes = true;
						}
						break;
					}
					case spv::OpImageWrite:
					case spv::OpCopyMemory:
					case spv::OpCopyMemorySized: f.writes = true; break;
					case spv::OpExtInst:
						if (Operand(inst, 1) == 35 || Operand(inst, 1) == 51) f.writes = true;
						break;
					case spv::OpFunctionCall: f.callees.push_back(Operand(inst, 0)); break;
					default:
						if (inst.op >= spv::OpAtomicLoad && inst.op <= spv::OpAtomicXor &&
						    !IsFaultPointer(Operand(inst, 0))) {
							f.writes = true;
						}
						break;
				}
			}
		}
		for (bool changed = true; changed;) {
			changed = false;
			for (auto& f: m_functions) {
				if (f.writes) continue;
				for (const auto callee: f.callees) {
					const auto found = index.find(callee);
					if (found == index.end() || m_functions[found->second].writes) {
						f.writes = true;
						changed  = true;
						break;
					}
				}
			}
		}
		FindPureFunctions(index);
	}

	bool Scalarish(uint32_t type, int depth = 0) const {
		if (type >= m_def_index.size() || m_def_index[type] == None) return false;
		const auto& d = m_insts[m_def_index[type]];
		if (d.op == spv::OpTypeInt || d.op == spv::OpTypeFloat || d.op == spv::OpTypeBool) return true;
		return d.op == spv::OpTypeVector && depth < 2 && Scalarish(m_words[d.first], depth + 1);
	}

	// A helper whose result depends only on its arguments and memory no shader invocation writes,
	// and that has no effect but the idempotent fault bits: two calls with the same arguments
	// may be one. Everything the body does must be on the list below.
	bool BodyIsPure(const Function& f, const std::unordered_map<uint32_t, uint32_t>& index) {
		if (f.entry || f.begin == 0 || f.begin > m_insts.size()) return false;
		const auto& head = m_insts[f.begin - 1]; // OpFunction
		if (!Scalarish(head.type)) return false;
		if (Operands(head) < 2 || Operand(head, 1) >= m_def_index.size() || m_def_index[Operand(head, 1)] == None) {
			return false;
		}
		const auto& fn_type = m_insts[m_def_index[Operand(head, 1)]];
		for (uint32_t i = 1; i < Operands(fn_type); i++) {
			if (!Scalarish(Operand(fn_type, i))) return false;
		}
		for (uint32_t i = f.begin; i < f.end; i++) {
			const auto& inst = m_insts[i];
			if (inst.removed) continue;
			switch (inst.op) {
				case spv::OpFunctionParameter:
				case spv::OpLabel:
				case spv::OpPhi:
				case spv::OpBranch:
				case spv::OpBranchConditional:
				case spv::OpSwitch:
				case spv::OpSelectionMerge:
				case spv::OpLoopMerge:
				case spv::OpReturn:
				case spv::OpReturnValue:
				case spv::OpUnreachable:
				case spv::OpVariable: break;
				case spv::OpLoad:
					switch (PointerClass(Operand(inst, 0))) {
						case spv::StorageClassPushConstant:
						case spv::StorageClassUniformConstant:
						case spv::StorageClassUniform:
						case spv::StorageClassStorageBuffer:
						case spv::StorageClassPhysicalStorageBuffer:
						case spv::StorageClassFunction: break;
						default: return false;
					}
					break;
				case spv::OpStore:
					if (PointerClass(Operand(inst, 0)) != spv::StorageClassFunction) return false;
					break;
				case spv::OpExtInst:
					if (Operand(inst, 0) != m_glsl || Operand(inst, 1) == 35 || Operand(inst, 1) == 51) return false;
					break;
				case spv::OpFunctionCall:
					if (!index.contains(Operand(inst, 0))) return false;
					break;
				default:
					if (IsGvnPure(inst.op)) break;
					if (inst.op >= spv::OpAtomicLoad && inst.op <= spv::OpAtomicXor && IsFaultPointer(Operand(inst, 0))) break;
					return false;
			}
		}
		return true;
	}

	void FindPureFunctions(const std::unordered_map<uint32_t, uint32_t>& index) {
		for (auto& f: m_functions) f.pure = BodyIsPure(f, index);
		for (bool changed = true; changed;) {
			changed = false;
			for (auto& f: m_functions) {
				if (!f.pure) continue;
				for (const auto callee: f.callees) {
					if (!m_functions[index.at(callee)].pure) {
						f.pure  = false;
						changed = true;
						break;
					}
				}
			}
		}
		m_pure_functions.clear();
		for (const auto& f: m_functions) {
			if (f.pure) m_pure_functions.insert(f.id);
		}
	}

	// Blocks of one function in module order, with their successors.
	void FindBlocks(const Function& f, std::vector<Block>& blocks,
	                std::unordered_map<uint32_t, uint32_t>& by_label) {
		blocks.clear();
		by_label.clear();
		for (uint32_t i = f.begin; i < f.end; i++) {
			const auto& inst = m_insts[i];
			if (inst.removed) continue;
			if (inst.op == spv::OpLabel) {
				by_label[inst.result] = static_cast<uint32_t>(blocks.size());
				blocks.push_back({inst.result, i + 1, i + 1, {}});
				continue;
			}
			if (blocks.empty()) continue;
			auto& block = blocks.back();
			block.end   = i + 1;
			switch (inst.op) {
				case spv::OpBranch: block.succ.push_back(Operand(inst, 0)); break;
				case spv::OpBranchConditional:
					block.succ.push_back(Operand(inst, 1));
					block.succ.push_back(Operand(inst, 2));
					break;
				case spv::OpSwitch:
					block.succ.push_back(Operand(inst, 1));
					for (uint32_t k = 3; k < Operands(inst); k += 2) block.succ.push_back(Operand(inst, k));
					break;
				default: break;
			}
		}
	}

	void ValueNumbering() {
		BuildUses();
		FindFunctions();
		m_repl.assign(m_type.size(), 0);
		std::vector<Block>                      blocks;
		std::unordered_map<uint32_t, uint32_t>  by_label;
		std::vector<uint32_t>                   ids;
		bool                                    exact = true;
		for (const auto& f: m_functions) {
			FindBlocks(f, blocks, by_label);
			if (blocks.empty()) continue;
			const auto n = static_cast<uint32_t>(blocks.size());
			// Successors as block indices; reverse postorder from the entry.
			std::vector<std::vector<uint32_t>> succ(n), pred(n);
			for (uint32_t b = 0; b < n; b++) {
				for (const auto label: blocks[b].succ) {
					const auto found = by_label.find(label);
					if (found == by_label.end()) continue;
					succ[b].push_back(found->second);
					pred[found->second].push_back(b);
				}
			}
			std::vector<uint32_t> post;
			std::vector<uint8_t>  seen(n, 0);
			std::vector<std::pair<uint32_t, size_t>> stack;
			stack.push_back({0, 0});
			seen[0] = 1;
			while (!stack.empty()) {
				auto& [node, next] = stack.back();
				if (next < succ[node].size()) {
					const auto child = succ[node][next++];
					if (!seen[child]) {
						seen[child] = 1;
						stack.push_back({child, 0});
					}
				} else {
					post.push_back(node);
					stack.pop_back();
				}
			}
			std::vector<uint32_t> rpo(post.rbegin(), post.rend());
			std::vector<uint32_t> number(n, None), idom(n, None);
			for (uint32_t i = 0; i < rpo.size(); i++) number[rpo[i]] = i;
			idom[0] = 0;
			const auto intersect = [&](uint32_t a, uint32_t b) {
				while (a != b) {
					while (number[a] > number[b]) a = idom[a];
					while (number[b] > number[a]) b = idom[b];
				}
				return a;
			};
			for (bool changed = true; changed;) {
				changed = false;
				for (size_t i = 1; i < rpo.size(); i++) {
					const auto b = rpo[i];
					uint32_t   best = None;
					for (const auto p: pred[b]) {
						if (idom[p] == None) continue;
						best = best == None ? p : intersect(p, best);
					}
					if (best != None && idom[b] != best) {
						idom[b] = best;
						changed = true;
					}
				}
			}
			std::vector<std::vector<uint32_t>> children(n);
			for (size_t i = 1; i < rpo.size(); i++) {
				if (idom[rpo[i]] != None) children[idom[rpo[i]]].push_back(rpo[i]);
			}
			// Dominator tree walk with a scoped table.
			std::unordered_multimap<uint64_t, uint32_t> table;
			std::vector<std::vector<std::unordered_multimap<uint64_t, uint32_t>::iterator>> added(n);
			std::vector<std::pair<uint32_t, bool>> work;
			work.push_back({0, true});
			while (!work.empty()) {
				const auto [b, enter] = work.back();
				work.pop_back();
				if (!enter) {
					for (const auto it: added[b]) table.erase(it);
					added[b].clear();
					continue;
				}
				for (uint32_t i = blocks[b].begin; i < blocks[b].end; i++) {
					auto& inst = m_insts[i];
					if (inst.removed) continue;
					IdOperands(inst, ids, exact);
					if (exact) {
						for (const auto operand: ids) {
							auto& id = Operand(inst, operand);
							if (id < m_repl.size() && m_repl[id] != 0) id = Resolve(id);
						}
					}
					if (inst.result == 0 || !Mergeable(inst, f)) continue;
					const auto hash = KeyHash(inst);
					bool       merged = false;
					for (auto [it, end] = table.equal_range(hash); it != end; ++it) {
						if (!SameKey(m_insts[it->second], inst)) continue;
						if (inst.result < m_inexact.size() && !m_inexact[inst.result]) {
							m_repl[inst.result] = m_insts[it->second].result;
							inst.removed        = true;
							m_gone[inst.result] = 1;
							merged              = true;
						}
						break;
					}
					if (!merged) added[b].push_back(table.emplace(hash, i));
				}
				work.push_back({b, false});
				for (auto c = children[b].rbegin(); c != children[b].rend(); ++c) work.push_back({*c, true});
			}
		}
		// Replace the removed values wherever they are still used.
		for (auto& inst: m_insts) {
			if (!inst.body || inst.removed) continue;
			IdOperands(inst, ids, exact);
			if (!exact) continue;
			for (const auto operand: ids) {
				auto& id = Operand(inst, operand);
				if (id < m_repl.size() && m_repl[id] != 0) id = Resolve(id);
			}
		}
		std::fill(m_repl.begin(), m_repl.end(), 0);
	}

	bool Mergeable(const Inst& inst, const Function& f) {
		if (IsGvnPure(inst.op)) return true;
		if (inst.op == spv::OpExtInst) {
			const auto number = Operand(inst, 1);
			return Operand(inst, 0) == m_glsl && number != 35 && number != 51;
		}
		if (inst.op == spv::OpFunctionCall) return m_pure_functions.contains(Operand(inst, 0));
		if (inst.op == spv::OpLoad) {
			const auto n = Operands(inst);
			if (n > 1 && (Operand(inst, 1) & 0x39u) != 0) return false;
			const auto pointer = Operand(inst, 0);
			if (IsFaultPointer(pointer)) return false;
			switch (PointerClass(pointer)) {
				case spv::StorageClassPushConstant:
				case spv::StorageClassUniformConstant:
				case spv::StorageClassUniform: return true;
				case spv::StorageClassStorageBuffer:
				case spv::StorageClassPhysicalStorageBuffer: return !f.writes;
				default: return false;
			}
		}
		return false;
	}

	uint64_t KeyHash(const Inst& inst) {
		uint64_t h = 0x9e3779b97f4a7c15ull ^ inst.op;
		const auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
		mix(inst.type);
		for (uint32_t i = 0; i < Operands(inst); i++) mix(Operand(inst, i));
		mix(inst.result < m_deco.size() ? m_deco[inst.result] : 0);
		return h;
	}

	bool SameKey(const Inst& a, const Inst& b) {
		if (a.op != b.op || a.type != b.type || a.count != b.count) return false;
		const auto da = a.result < m_deco.size() ? m_deco[a.result] : 0;
		const auto db = b.result < m_deco.size() ? m_deco[b.result] : 0;
		if (da != db) return false;
		for (uint32_t i = 0; i < Operands(a); i++) {
			if (Operand(a, i) != Operand(b, i)) return false;
		}
		return true;
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
	uint32_t                           m_fault_buffer = 0;
	uint32_t                           m_glsl         = 0;
	std::vector<uint32_t>              m_ptr_class;
	std::vector<uint32_t>              m_base;
	std::vector<uint64_t>              m_deco;
	std::vector<Function>              m_functions;
	std::unordered_set<uint32_t>       m_entry_ids;
	std::unordered_set<uint32_t>       m_pure_functions;
	std::vector<uint32_t>              m_def_index;
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

void ForwardMaskedSelects(std::vector<uint32_t>& module, size_t min_body_words, uint32_t fault_buffer) {
	if (module.size() < min_body_words) {
		return;
	}
	Pass(module, min_body_words, fault_buffer).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
