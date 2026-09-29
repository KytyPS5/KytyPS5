#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <bit>
#include <fmt/format.h>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t SamplerBorderClampMask    = (1u << 2u) | (1u << 5u) | (1u << 8u);
constexpr uint32_t SamplerDword3ReservedMask = 0x3ffff000u;

uint32_t PossibleU32Bits(Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		return value.GetType() == Type::U32 ? value.U32() : UINT32_MAX;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return UINT32_MAX;
	}
	switch (inst->GetOpcode()) {
		case ValueOpcode::BitwiseAnd32:
			return PossibleU32Bits(inst->Arg(0)) & PossibleU32Bits(inst->Arg(1));
		case ValueOpcode::BitwiseOr32:
			return PossibleU32Bits(inst->Arg(0)) | PossibleU32Bits(inst->Arg(1));
		case ValueOpcode::ShiftLeftLogical32: {
			const auto shift = inst->Arg(1).Resolve();
			return shift.IsImmediate() && shift.GetType() == Type::U32
			           ? PossibleU32Bits(inst->Arg(0)) << (shift.U32() & 31u)
			           : UINT32_MAX;
		}
		default: return UINT32_MAX;
	}
}

Value CanonicalizeSampleAdjustDword3(Value value) {
	for (;;) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::BitwiseOr32) {
			return value;
		}
		const auto left           = inst->Arg(0).Resolve();
		const auto right          = inst->Arg(1).Resolve();
		const bool left_reserved  = (PossibleU32Bits(left) & ~SamplerDword3ReservedMask) == 0;
		const bool right_reserved = (PossibleU32Bits(right) & ~SamplerDword3ReservedMask) == 0;
		if (left_reserved && right_reserved) {
			return Value(0u);
		}
		if (left_reserved) {
			value = right;
		} else if (right_reserved) {
			value = left;
		} else {
			return value;
		}
	}
}

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

uint32_t ByteExtent(const MemoryInfo& memory) {
	const auto bytes = std::max((memory.data_bits + 7u) / 8u, 1u);
	const auto count = std::max(memory.data_dwords, 1u);
	const auto end   = static_cast<uint64_t>(memory.offset) + static_cast<uint64_t>(bytes) * count;
	return end > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end);
}

// Prove a loop cannot continue after its bound fails, both on entry and after an
// arbitrary previous iteration. Header Phis are substituted simultaneously; all
// unsupported expressions remain unconstrained. This state exists only while tracking.
class LoopBoundProof {
public:
	LoopBoundProof(const Program& program, const Inst& induction, const Inst& bound)
	    : m_program(program), m_induction(induction), m_bound(bound) {}

	bool Excludes(Value condition, bool positive) {
		for (uint32_t incoming = 0; incoming < m_induction.NumArgs(); ++incoming) {
			m_incoming = m_induction.PhiBlock(incoming);
			m_values[1].clear();
			if (Evaluate(condition, true) != (positive ? 0u : 1u)) return false;
		}
		return true;
	}

private:
	struct Node {
		uint32_t variable = UINT32_MAX;
		uint32_t low = 0;
		uint32_t high = 0;
	};

	uint32_t NodeFor(uint32_t variable, uint32_t low, uint32_t high) {
		if (low == high) return low;
		const auto [it, inserted] = m_nodes_by_key.try_emplace(
		    std::array {variable, low, high}, static_cast<uint32_t>(m_nodes.size()));
		if (inserted) m_nodes.push_back({variable, low, high});
		return it->second;
	}

	uint32_t Unknown(Type type) {
		if (type == Type::U1) return NodeFor(m_variables++, 0u, 1u);
		m_nodes.emplace_back();
		return static_cast<uint32_t>(m_nodes.size() - 1u);
	}

	uint32_t Select(uint32_t condition, uint32_t yes, uint32_t no) {
		if (condition == 0u) return no;
		if (condition == 1u || yes == no) return yes;
		if (yes == 1u && no == 0u) return condition;
		const std::array key {condition, yes, no};
		if (const auto found = m_choices.find(key); found != m_choices.end()) return found->second;
		const auto variable = std::min({m_nodes[condition].variable, m_nodes[yes].variable,
		                                m_nodes[no].variable});
		const auto arm = [&](uint32_t value, bool high) {
			const auto node = m_nodes[value];
			return node.variable == variable ? (high ? node.high : node.low) : value;
		};
		const auto low = Select(arm(condition, false), arm(yes, false), arm(no, false));
		const auto high = Select(arm(condition, true), arm(yes, true), arm(no, true));
		const auto result = NodeFor(variable, low, high);
		m_choices.emplace(key, result);
		return result;
	}

	uint32_t Compare(const Inst& inst, uint32_t left, uint32_t right) {
		const auto key = std::tuple {inst.GetOpcode(), inst.Flags<uint64_t>(), left, right};
		if (const auto found = m_predicates.find(key); found != m_predicates.end()) return found->second;
		const auto variable = std::min(m_nodes[left].variable, m_nodes[right].variable);
		uint32_t result;
		if (variable == UINT32_MAX) {
			result = Unknown(Type::U1);
		} else {
			const auto lhs = m_nodes[left];
			const auto rhs = m_nodes[right];
			const auto low = Compare(inst, lhs.variable == variable ? lhs.low : left,
			                         rhs.variable == variable ? rhs.low : right);
			const auto high = Compare(inst, lhs.variable == variable ? lhs.high : left,
			                          rhs.variable == variable ? rhs.high : right);
			result = Select(NodeFor(variable, 0u, 1u), high, low);
		}
		m_predicates.emplace(key, result);
		return result;
	}

	uint32_t Evaluate(Value value, bool current) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			if (value.GetType() == Type::U1) return value.U1() ? 1u : 0u;
			for (const auto& [literal, node]: m_literals) {
				if (literal == value) return node;
			}
			const auto node = Unknown(value.GetType());
			m_literals.emplace_back(value, node);
			return node;
		}
		const auto* inst = value.TryInstruction();
		if (current && inst == &m_bound) return 0u;
		auto& values = m_values[current];
		if (const auto found = values.find(inst); found != values.end()) {
			if (found->second == UINT32_MAX) found->second = Unknown(value.GetType());
			return found->second;
		}
		const auto cached = values.emplace(inst, UINT32_MAX).first;
		auto result = UINT32_MAX;
		const auto arg = [&](uint32_t index) { return Evaluate(inst->Arg(index), current); };
		switch (inst->GetOpcode()) {
			case ValueOpcode::Phi: {
				const auto invariant = ResolveInvariantPhi(m_program, value);
				if (!invariant.IsEmpty()) {
					result = Evaluate(invariant, current);
				} else if (inst->Parent() == m_induction.Parent()) {
					if (current) {
						for (uint32_t i = 0; i < inst->NumArgs(); ++i) {
							if (inst->PhiBlock(i) == m_incoming) result = Evaluate(inst->Arg(i), false);
						}
					}
				} else if (inst->NumArgs() != 0u) {
					result = arg(0);
					for (uint32_t i = 1; i < inst->NumArgs(); ++i) {
						if (arg(i) != result) { result = UINT32_MAX; break; }
					}
				}
				break;
			}
			case ValueOpcode::LogicalNot: result = Select(arg(0), 0u, 1u); break;
			case ValueOpcode::LogicalAnd: {
				const auto left = arg(0);
				result = left == 0u ? 0u : Select(left, arg(1), 0u);
				break;
			}
			case ValueOpcode::LogicalOr: {
				const auto left = arg(0);
				result = left == 1u ? 1u : Select(left, 1u, arg(1));
				break;
			}
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectU32: {
				const auto condition = arg(0);
				result = condition == 0u ? arg(2) : condition == 1u ? arg(1)
				                                                   : Select(condition, arg(1), arg(2));
				break;
			}
			default:
				if (inst->GetType() == Type::U1 && inst->NumArgs() == 2u &&
				    inst->Arg(0).GetType() == Type::U32 && inst->Arg(1).GetType() == Type::U32)
					result = Compare(*inst, arg(0), arg(1));
				break;
		}
		// Only unsupported values and cycles need free variables.
		if (result == UINT32_MAX) {
			if (cached->second == UINT32_MAX) cached->second = Unknown(value.GetType());
			return cached->second;
		}
		cached->second = result;
		return result;
	}

	const Program& m_program;
	const Inst& m_induction;
	const Inst& m_bound;
	const Block* m_incoming = nullptr;
	uint32_t m_variables = 0;
	std::vector<Node> m_nodes {{}, {}};
	std::vector<std::pair<Value, uint32_t>> m_literals;
	std::array<std::map<const Inst*, uint32_t>, 2> m_values;
	std::map<std::array<uint32_t, 3>, uint32_t> m_nodes_by_key;
	std::map<std::array<uint32_t, 3>, uint32_t> m_choices;
	std::map<std::tuple<ValueOpcode, uint64_t, uint32_t, uint32_t>, uint32_t> m_predicates;
};

class Tracker {
public:
	Tracker(Program& program, const Decoder::Program& decoded, const CFG::Graph& native_cfg)
	    : m_program(program), m_decoded(decoded), m_native_cfg(native_cfg),
	      m_scalar_writes(std::move(program.scalar_writes)), m_info(program.info) {
		std::ranges::sort(m_scalar_writes, {}, &Program::ScalarWrite::pc);
		m_info.buffers.clear();
		m_info.images.clear();
		m_info.samplers.clear();
		m_info.sampled_pairs.clear();
		m_info.uses_dma = false;
		m_shader_writes = HasShaderMemoryWrites(program);
	}

	void Run() {
		if (m_program.resource_tracking_complete) {
			Fail(0, "resources already tracked");
			return;
		}
		PlanScalarReads();
		EliminateDeadCode(m_program.blocks);
		PlanIndirectImages();
		if (m_failed) {
			return;
		}
		PlanBindlessSamplers();
		PlanDefaultSamplers();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				Collect(inst);
				if (m_failed) {
					return;
				}
			}
		}
		LinkImageAliases();
		for (const auto& patch: m_handle_patches) {
			patch.handle->SetFlags<uint32_t>(patch.resource);
		}
		for (const auto& patch: m_memory_patches) {
			auto& memory    = m_program.memory_info[patch.index];
			memory.resource = patch.resource;
			if (patch.has_sampler) {
				memory.sampler = patch.sampler;
			}
		}
		for (const auto& plan: m_indirect_images) {
			plan.handle->SetArg(0, plan.key);
			for (uint32_t dword = 0; dword < 4u; dword++) {
				plan.handle->SetArg(dword + 1u, plan.roots[dword + 4u]);
			}
			for (uint32_t dword = 5u; dword < plan.roots.size(); dword++) {
				plan.handle->SetArg(dword, plan.key);
			}
			for (const auto index: plan.memory) {
				if (!IsLiveTableMemory(index)) {
					m_program.memory_info[index].planning_only = true;
				}
			}
			for (const auto index: plan.extra_memory) {
				if (!IsLiveTableMemory(index)) {
					m_program.memory_info[index].planning_only = true;
				}
			}
		}
		// A bindless sampler handle carries its key (read by the shader) and the first three dwords
		// of the heap V#, which keeps the plan values alive; the S# reads it strands are dead.
		for (const auto& plan: m_bindless_samplers) {
			plan.handle->SetArg(0, plan.key);
			for (uint32_t dword = 0; dword < 3u; dword++) {
				plan.handle->SetArg(dword + 1u, plan.table_roots[dword]);
			}
			for (uint32_t i = 0; i < plan.reads.size(); i++) {
				if (plan.exclusive[i]) {
					m_program.memory_info[plan.memory[i]].planning_only = true;
				}
			}
		}
		m_program.descriptor_sources         = std::move(m_sources);
		m_program.info                       = std::move(m_info);
		m_program.resource_tracking_complete = true;
	}

private:
	struct HandlePatch {
		Inst*    handle   = nullptr;
		uint32_t resource = 0;
	};

	struct MemoryPatch {
		uint32_t index       = 0;
		uint32_t resource    = 0;
		uint32_t sampler     = 0;
		bool     has_sampler = false;
	};

	struct ResolvedHandle {
		const Inst* handle;
		uint32_t pc;
		DescriptorSource source;
		Value planning_handle;
	};

	struct IndirectImagePlan {
		Inst*                      handle = nullptr;
		uint32_t                   source = 0;
		Value                      key;
		std::array<Value, 8>       roots {};
		std::array<uint32_t, 8>    memory {};
		std::array<const Inst*, 8> reads {};
		// A descriptor merged by phis: the heap reads behind them.
		std::vector<uint32_t>      extra_memory;
		std::vector<const Inst*>   extra_reads;
	};

	struct BindlessSamplerPlan {
		Inst*                      handle = nullptr;
		uint32_t                   source = 0;
		Value                      key;
		std::array<Value, 3>       table_roots {};
		std::array<uint32_t, 4>    memory {};
		std::array<const Inst*, 4> reads {};
		std::array<bool, 4>        exclusive {};
	};

	void Fail(uint32_t pc, const std::string& reason) const {
		const auto message =
		    fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
		                m_program.shader_hash, StageName(m_program.stage), pc, reason);
		if (Frontend::TranslationNonFatal()) {
			if (!m_failed) {
				LOGF("%s\n", message.c_str());
			}
			m_failed = true;
			return;
		}
		EXIT("%s", message.c_str());
		std::abort();
	}

	Value NativeDescriptorSource(Value value, uint32_t reg, uint32_t use_pc) const {
		value = value.Resolve();
		const auto* phi = value.TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi || m_native_cfg.blocks.empty())
			return value;
		std::vector<const Inst*> candidates;
		std::vector<const Inst*> visited;
		std::vector<const Inst*> pending {phi};
		while (!pending.empty()) {
			const auto* inst = pending.back();
			pending.pop_back();
			if (std::ranges::find(visited, inst) != visited.end()) continue;
			visited.push_back(inst);
			if (inst->GetOpcode() == ValueOpcode::Phi) {
				for (size_t i = 0; i < inst->NumArgs(); ++i) {
					const auto* arg = inst->Arg(i).Resolve().TryInstruction();
					if (arg != nullptr) pending.push_back(arg);
				}
			} else if (inst->GetOpcode() == ValueOpcode::ReadConst ||
			           inst->GetOpcode() == ValueOpcode::LoadAddressU32 ||
			           inst->GetOpcode() == ValueOpcode::ReadConstBuffer ||
			           inst->GetOpcode() == ValueOpcode::GetUserData) {
				candidates.push_back(inst);
			}
		}
		if (candidates.empty()) return value;
		const auto source_at = [&](uint32_t pc) {
			Value source;
			const auto native = std::ranges::lower_bound(m_decoded.instructions, pc, {},
			                                            &Decoder::Instruction::pc);
			for (const auto* candidate: candidates) {
				if (pc == UINT32_MAX) {
					if (candidate->GetOpcode() != ValueOpcode::GetUserData ||
					    RegIndex(candidate->Arg(0).ScalarRegister()) != reg)
						continue;
				} else {
					if (candidate->GetOpcode() != ValueOpcode::ReadConst &&
					    candidate->GetOpcode() != ValueOpcode::LoadAddressU32 &&
					    candidate->GetOpcode() != ValueOpcode::ReadConstBuffer) continue;
					const auto flags = candidate->Flags<MemoryFlags>();
					if (flags.pc != pc || flags.index >= m_program.memory_info.size()) continue;
					if (native == m_decoded.instructions.end() || native->pc != pc ||
					    native->dst.kind != Decoder::OperandKind::Sgpr ||
					    native->dst.reg + m_program.memory_info[flags.index].component_index != reg)
						continue;
				}
				const Value current(const_cast<Inst*>(candidate));
				if (!source.IsEmpty() && !EquivalentValue(m_program, source, current)) return Value {};
				source = current;
			}
			return source;
		};
		const auto use = std::ranges::find_if(m_native_cfg.blocks, [&](const auto& block) {
			return block.start_pc <= use_pc && use_pc < block.end_pc;
		});
		if (use == m_native_cfg.blocks.end()) return value;
		struct Position { uint32_t block; uint32_t before; };
		std::vector<Position> positions {{use->id, use_pc}};
		std::vector<bool> reached(m_native_cfg.blocks.size());
		Value selected;
		uint32_t selected_pc = UINT32_MAX;
		const auto select = [&](uint32_t pc) {
			const auto source = source_at(pc);
			if (source.IsEmpty()) return false;
			if (!selected.IsEmpty()) {
				const auto op = source.TryInstruction()->GetOpcode();
				if ((op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) &&
				    selected_pc != pc) return false;
				if (!EquivalentValue(m_program, selected, source)) return false;
			}
			selected = source;
			selected_pc = pc;
			return true;
		};
		while (!positions.empty()) {
			const auto position = positions.back();
			positions.pop_back();
			const auto& block = m_native_cfg.blocks[position.block];
			// A backedge may revisit the use block after a write later than the original use.
			if (position.before == block.end_pc) {
				if (reached[block.id]) continue;
				reached[block.id] = true;
			}
			auto write = std::ranges::lower_bound(m_scalar_writes, position.before, {},
			                                    &Program::ScalarWrite::pc);
			bool found = false;
			while (write != m_scalar_writes.begin()) {
				--write;
				if (write->pc < block.start_pc) break;
				if (RegIndex(write->reg) != reg) continue;
				if (!select(write->pc)) return value;
				found = true;
				break;
			}
			if (found) continue;
			if (block.id == m_native_cfg.entry_block && !select(UINT32_MAX)) return value;
			for (const auto pred: block.predecessors)
				positions.push_back({pred, m_native_cfg.blocks[pred].end_pc});
		}
		return selected.IsEmpty() ? value : selected;
	}

	mutable bool m_failed = false;

public:
	[[nodiscard]] bool Failed() const { return m_failed; }

private:
	bool UsesOnlyDescriptorSnapshots(Value value) const {
		std::vector<Value>       pending {value};
		std::unordered_set<const Inst*> visited;
		while (!pending.empty()) {
			const auto current = pending.back().Resolve();
			pending.pop_back();
			const auto* inst = current.TryInstruction();
			if (inst == nullptr || !visited.insert(inst).second) continue;
			if (inst->GetOpcode() == ValueOpcode::ReadConst) continue;
			if (BufferAccessOf(inst->GetOpcode()) != BufferAccess::None ||
			    AddressOpcodeInfoOf(inst->GetOpcode()).access != AddressAccess::None ||
			    ImageOpcodeInfoOf(inst->GetOpcode()).access != ImageAccess::None)
				return false;
			for (size_t i = 0; i < inst->NumArgs(); ++i)
				pending.push_back(inst->Arg(i));
		}
		return true;
	}

	Value LowerDescriptorPhi(Value value, bool buffer = false) {
		value           = value.Resolve();
		const auto* phi = value.TryInstruction();
		// Research: a buffer descriptor may lower in a shader that writes memory too;
		// materialization then proves the evaluating reads disjoint from every written buffer
		// (descriptor_phi_under_writes). Images and samplers keep upstream's refusal.
		if ((m_shader_writes && !buffer) || phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->NumArgs() != 2u || phi->NumPhiBlocks() != 2u || phi->GetType() != Type::U32 ||
		    m_program.blocks.size() != m_program.block_info.size()) {
			return value;
		}
		const auto* merge  = phi->Parent();
		const auto* branch = phi->PhiBlock(0);
		if (merge == nullptr || branch == nullptr || phi->PhiBlock(1) == nullptr ||
		    branch == phi->PhiBlock(1)) {
			return value;
		}
		for (const auto& [original, selected]: m_descriptor_selections) {
			if (original == phi) {
				return selected;
			}
		}
		if (branch->ImmSuccessors().size() != 2u) {
			if (branch->ImmPredecessors().size() != 1u) {
				return value;
			}
			branch = branch->ImmPredecessors()[0];
		}
		if (branch == merge || branch->ImmSuccessors().size() != 2u) {
			return value;
		}
		std::array<uint32_t, 2> target_ids;
		for (uint32_t arm = 0; arm < 2; arm++) {
			const auto* incoming = phi->PhiBlock(arm);
			if (incoming == merge ||
			    (incoming != branch && (incoming->ImmPredecessors().size() != 1u ||
			                            incoming->ImmPredecessors()[0] != branch ||
			                            incoming->ImmSuccessors().size() != 1u ||
			                            incoming->ImmSuccessors()[0] != merge))) {
				return value;
			}
			const auto* target = incoming == branch ? merge : incoming;
			const auto  it     = std::ranges::find(m_program.blocks, target);
			if (it == m_program.blocks.end()) {
				return value;
			}
			target_ids[arm] = m_program.block_info[it - m_program.blocks.begin()].id;
		}
		const auto branch_it = std::ranges::find(m_program.blocks, branch);
		if (branch_it == m_program.blocks.end()) {
			return value;
		}
		const auto& info = m_program.block_info[branch_it - m_program.blocks.begin()];
		const auto& term = info.terminator;
		// Writes do not invalidate user data or the descriptor values already captured
		// in SRT slots. Never introduce a host selection over a live GPU memory read.
		if (m_shader_writes &&
		    (!UsesOnlyDescriptorSnapshots(info.condition) ||
		     !UsesOnlyDescriptorSnapshots(phi->Arg(0)) ||
		     !UsesOnlyDescriptorSnapshots(phi->Arg(1)) ||
		     !ValidateRuntimeValue(m_program, phi->Arg(0), RuntimeValueType::Integer) ||
		     !ValidateRuntimeValue(m_program, phi->Arg(1), RuntimeValueType::Integer)))
			return value;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch ||
		    !((term.true_block == target_ids[0] && term.false_block == target_ids[1]) ||
		      (term.false_block == target_ids[0] && term.true_block == target_ids[1])) ||
		    !ValidateRuntimeValue(m_program, info.condition, RuntimeValueType::Integer) ||
		    !ValidateRuntimeValue(m_program, phi->Arg(0)) ||
		    !ValidateRuntimeValue(m_program, phi->Arg(1))) {
			return value;
		}
		// Retain a host expression; replacing the GPU Phi would break SSA dominance.
		const auto true_arg = term.true_block == target_ids[0] ? 0u : 1u;
		auto&      selected = m_program.value_storage.emplace_back(ValueOpcode::SelectU32);
		selected.SetArg(0, info.condition);
		selected.SetArg(1, phi->Arg(true_arg));
		selected.SetArg(2, phi->Arg(true_arg ^ 1u));
		m_descriptor_selections.emplace_back(phi, Value(&selected));
		if (m_shader_writes) {
			m_program.descriptor_phi_under_writes = true;
		}
		return Value(&selected);
	}

	// Research: a descriptor dword computed from a phi (a V# base address picked on a uniform
	// branch, then a flag bit set in it) lowers the phi inside a host copy of the pure integer
	// expression around it. Reads are never copied, so their memory metadata stays their own.
	// The one value outside a web of phis whose other operands are the web itself or relative
	// register writes over it (empty when there is more than one).
	Value ResolveMovRelPhiWeb(Value start) const {
		std::vector<const Inst*> pending {start.Resolve().TryInstruction()};
		std::vector<const Inst*> seen;
		Value                    external;
		while (!pending.empty()) {
			const auto* inst = pending.back();
			pending.pop_back();
			if (inst == nullptr || std::ranges::find(seen, inst) != seen.end()) {
				continue;
			}
			if (seen.size() > 1024u) {
				return {};
			}
			seen.push_back(inst);
			if (inst->GetOpcode() == ValueOpcode::SelectU32 &&
			    inst->Flags<uint64_t>() == MovRelSelectFlags) {
				const auto next = inst->Arg(2).Resolve();
				if (const auto* def = next.TryInstruction(); def != nullptr &&
				    (def->GetOpcode() == ValueOpcode::Phi ||
				     (def->GetOpcode() == ValueOpcode::SelectU32 &&
				      def->Flags<uint64_t>() == MovRelSelectFlags))) {
					pending.push_back(def);
				} else if (external.IsEmpty()) {
					external = next;
				} else if (!(external == next)) {
					return {};
				}
				continue;
			}
			if (inst->GetOpcode() != ValueOpcode::Phi) {
				return {};
			}
			for (size_t i = 0; i < inst->NumArgs(); i++) {
				const auto arg = inst->Arg(i).Resolve();
				if (const auto* def = arg.TryInstruction(); def != nullptr &&
				    (def->GetOpcode() == ValueOpcode::Phi ||
				     (def->GetOpcode() == ValueOpcode::SelectU32 &&
				      def->Flags<uint64_t>() == MovRelSelectFlags))) {
					pending.push_back(def);
				} else if (external.IsEmpty()) {
					external = arg;
				} else if (!(external == arg)) {
					return {};
				}
			}
		}
		return external;
	}

	Value LowerDescriptorValue(Value value, const Block* use, bool buffer, uint32_t depth = 0) {
		value      = value.Resolve();
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return value;
		}
		if (inst->GetOpcode() == ValueOpcode::Phi) {
			// A register carried around a loop that only a relative write could change holds
			// the one value it entered with.
			if (const auto single = ResolveMovRelPhiWeb(value); !single.IsEmpty() && depth < 64u) {
				return LowerDescriptorValue(single, use, buffer, depth + 1u);
			}
			return LowerDescriptorPhi(value, buffer);
		}
		// A relative register write (V_MOVRELD) is modelled as a select on every register it
		// could reach; a descriptor register is never part of such an array.
		if (inst->GetOpcode() == ValueOpcode::SelectU32 &&
		    inst->Flags<uint64_t>() == MovRelSelectFlags && depth < 64u) {
			return LowerDescriptorValue(inst->Arg(2), use, buffer, depth + 1u);
		}
		// A value the host can evaluate is wave-uniform, so reading its first lane is itself.
		if (inst->GetOpcode() == ValueOpcode::ReadFirstLane && inst->NumArgs() >= 1u &&
		    depth < 64u) {
			const auto lowered = LowerDescriptorValue(inst->Arg(0), use, buffer, depth + 1u);
			if (ValidateRuntimeValue(m_program, lowered)) {
				return lowered;
			}
			return value;
		}
		switch (inst->GetOpcode()) {
			case ValueOpcode::BitFieldInsert:
			case ValueOpcode::BitFieldSExtract:
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseNot32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32:
			case ValueOpcode::IAdd32:
			case ValueOpcode::IMul32:
			case ValueOpcode::ISub32:
			case ValueOpcode::SelectU32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftRightArithmetic32:
			case ValueOpcode::ShiftRightLogical32: break;
			default: return value;
		}
		if (depth >= 6u) {
			return value;
		}
		std::vector<Value> args(inst->NumArgs());
		bool               changed = false;
		for (size_t i = 0; i < args.size(); i++) {
			args[i] = LowerDescriptorValue(inst->Arg(i), use, buffer, depth + 1u);
			changed = changed || !(args[i] == inst->Arg(i).Resolve());
		}
		if (!changed) {
			return value;
		}
		auto& copy = m_program.value_storage.emplace_back(inst->GetOpcode(), inst->Flags<uint64_t>());
		for (size_t i = 0; i < args.size(); i++) {
			copy.SetArg(i, args[i]);
		}
		return Value(&copy);
	}

	void MakeSource(const Inst& handle, uint32_t width, bool sampler, bool sample_adjust,
	                uint32_t base_reg, DescriptorSource& descriptor, uint32_t pc) {
		const auto resolved = std::ranges::find_if(m_resolved_handles, [&](const auto& entry) {
			return entry.handle == &handle && entry.pc == pc;
		});
		if (resolved != m_resolved_handles.end()) {
			descriptor = resolved->source;
			return;
		}
		if (handle.NumArgs() != width) {
			Fail(pc, fmt::format("{} has {} descriptor dwords, expected {}",
			                     ValueOpcodeName(handle.GetOpcode()), handle.NumArgs(), width));
			return;
		}
		descriptor.dword_count = width;
		for (uint32_t i = 0; i < width; i++) {
			const auto value = base_reg != UINT32_MAX
			    ? NativeDescriptorSource(handle.Arg(i), base_reg + i, pc) : handle.Arg(i);
			descriptor.dwords[i] = LowerDescriptorValue(
			    value, handle.Parent(), handle.GetOpcode() == ValueOpcode::GetBufferResource);
		}
		if (sample_adjust) {
			descriptor.dwords[3] = CanonicalizeSampleAdjustDword3(descriptor.dwords[3]);
		}
		const auto dword0 = descriptor.dwords[0].Resolve();
		if (sampler && dword0.IsImmediate() && dword0.GetType() == Type::U32 &&
		    (dword0.U32() & SamplerBorderClampMask) == 0) {
			// Border color and its table index are unused unless a clamp axis selects border mode.
			descriptor.dwords[3] = Value(0u);
		}
		m_resolved_handles.push_back({&handle, pc, descriptor, {}});
	}

	uint32_t ScalarReadBase(const Inst& read) const {
		const auto flags = read.Flags<MemoryFlags>();
		if (m_program.memory_info[flags.index].kind == ResourceKind::ScalarBuffer)
			return m_program.memory_info[flags.index].resource * 4u;
		const auto native = std::ranges::lower_bound(m_decoded.instructions, flags.pc, {},
		                                            &Decoder::Instruction::pc);
		return native != m_decoded.instructions.end() && native->pc == flags.pc &&
		               native->src0.kind == Decoder::OperandKind::Sgpr
		           ? native->src0.reg : UINT32_MAX;
	}

	// Large inlined shaders have descriptor dependencies thousands of instructions deep: walk
	// them with an explicit stack (post-order, same cycle rule) instead of one native frame each.
	void CollectScalarRead(Value root, uint32_t root_pc) {
		struct Frame {
			Inst*                                   inst   = nullptr;
			bool                                    memory = false;
			std::vector<std::pair<Value, uint32_t>> dependencies;
			size_t                                  next = 0;
		};
		std::vector<Frame>                        stack;
		std::unordered_map<const Inst*, size_t>   visiting;
		const auto enter = [&](Value value, uint32_t use_pc) {
			value = value.Resolve();
			if (value.IsImmediate()) return;
			auto* inst = value.TryInstruction();
			if (inst == nullptr) {
				Fail(use_pc, "invalid typed planning value");
				return;
			}
			if (const auto cycle = visiting.find(inst); cycle != visiting.end()) {
				if (std::any_of(stack.begin() + static_cast<std::ptrdiff_t>(cycle->second), stack.end(),
				                [](const Frame& frame) {
					                return frame.inst->GetOpcode() == ValueOpcode::Phi;
				                })) {
					return;
				}
				Fail(use_pc, "cyclic typed planning value without a phi");
				return;
			}
			if (m_srt_visited.contains(inst)) return;
			Frame frame;
			frame.inst = inst;
			uint32_t    memory_index = 0;
			const auto* memory       = ScalarReadMemory(*inst, memory_index);
			if (memory != nullptr) {
				frame.memory       = true;
				const auto* handle = inst->Arg(0).Resolve().TryInstruction();
				const auto  width  = memory->kind == ResourceKind::ScalarBuffer ? 4u : 2u;
				if (handle == nullptr ||
				    handle->GetOpcode() != (width == 4u ? ValueOpcode::GetBufferResource
				                                        : ValueOpcode::GetAddressResource)) {
					Fail(use_pc, "scalar read has an invalid resource handle");
					return;
				}
				const auto      pc = inst->Flags<MemoryFlags>().pc;
				DescriptorSource source;
				MakeSource(*handle, width, false, false, ScalarReadBase(*inst), source, pc);
				for (uint32_t word = 0; word < width; ++word) {
					frame.dependencies.emplace_back(source.dwords[word], pc);
				}
				for (size_t arg = 1; arg < inst->NumArgs(); ++arg) {
					frame.dependencies.emplace_back(inst->Arg(arg), use_pc);
				}
			} else {
				for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
					frame.dependencies.emplace_back(inst->Arg(arg), use_pc);
				}
			}
			visiting.emplace(inst, stack.size());
			stack.push_back(std::move(frame));
		};
		enter(root, root_pc);
		while (!stack.empty() && !m_failed) {
			auto& frame = stack.back();
			if (frame.next < frame.dependencies.size()) {
				const auto dependency = frame.dependencies[frame.next++];
				enter(dependency.first, dependency.second);
				continue;
			}
			auto* inst         = frame.inst;
			const bool memory  = frame.memory;
			stack.pop_back();
			visiting.erase(inst);
			m_srt_visited.insert(inst);
			if (!memory) continue;
			const auto offset = inst->Arg(1).Resolve();
			if (!offset.IsImmediate() || offset.GetType() != Type::U32) continue;
			m_scalar_reads.push_back(inst);
		}
	}

	void PlanScalarReads() {
		m_program.srt_plan_complete = false;
		m_program.srt_reads.clear();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				const auto op = inst.GetOpcode();
				const auto image = ImageOpcodeInfoOf(op);
				if (BufferAccessOf(op) == BufferAccess::None &&
				    AddressOpcodeInfoOf(op).access == AddressAccess::None &&
				    image.access == ImageAccess::None) continue;
				const auto flags = inst.Flags<MemoryFlags>();
				if (flags.index >= m_program.memory_info.size())
					Fail(flags.pc, "memory metadata index is out of range");
				if (inst.NumArgs() < (image.needs_sampler ? 2u : 1u))
					Fail(flags.pc, "memory operation has no resource handle");
				const auto& memory = m_program.memory_info[flags.index];
				if ((op == ValueOpcode::LoadAddressU32 && memory.kind == ResourceKind::ScalarBuffer) ||
				    (op == ValueOpcode::ReadConstBuffer && memory.kind == ResourceKind::ScalarAddress))
					Fail(flags.pc, "scalar read has incompatible scalar memory metadata");
				for (uint32_t arg = 0; arg < (image.needs_sampler ? 2u : 1u); ++arg) {
					const auto* handle = inst.Arg(arg).Resolve().TryInstruction();
					if (handle == nullptr) continue;
					const auto kind = handle->GetOpcode();
					const bool sampler = kind == ValueOpcode::GetSamplerResource;
					const uint32_t width = kind == ValueOpcode::GetImageResource ? 8u
					                     : kind == ValueOpcode::GetAddressResource ? 2u
					                     : kind == ValueOpcode::GetBufferResource || sampler ? 4u : 0u;
					if (width == 0u) continue;
					const auto base = width == 2u
					    ? (memory.kind == ResourceKind::ScalarAddress ? ScalarReadBase(inst) : UINT32_MAX)
					    : (sampler ? memory.sampler : memory.resource) * 4u;
					DescriptorSource source;
					MakeSource(*handle, width, sampler,
					           sampler && (memory.image_sample_flags & Decoder::ImageSampleFlagAdjust) != 0,
					           base, source, flags.pc);
					for (uint32_t word = 0; word < width; ++word)
						CollectScalarRead(source.dwords[word], flags.pc);
				}
			}
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				uint32_t index = 0;
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 &&
				    ScalarReadMemory(inst, index) != nullptr && inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst)))
					CollectScalarRead(Value(&inst), inst.Flags<MemoryFlags>().pc);
			}
		}
		for (auto* read: m_scalar_reads) {
			const auto flags = read->Flags<MemoryFlags>();
			auto& memory = m_program.memory_info[flags.index];
			memory.planning_only = true;
			const auto* handle = read->Arg(0).Resolve().TryInstruction();
			auto resolved = std::ranges::find_if(m_resolved_handles, [&](const auto& entry) {
				return entry.handle == handle && entry.pc == flags.pc;
			});
			EXIT_IF(resolved == m_resolved_handles.end());
			uint32_t slot = 0;
			for (; slot < m_program.srt_reads.size(); ++slot) {
				const auto* other = m_program.srt_reads[slot].value.Resolve().TryInstruction();
				if (other->GetOpcode() != read->GetOpcode() ||
				    m_program.memory_info[other->Flags<MemoryFlags>().index] != memory) continue;
				const auto* other_handle = other->Arg(0).Resolve().TryInstruction();
				bool same = true;
				for (uint32_t word = 0; word < resolved->source.dword_count; ++word)
					same &= EquivalentValue(m_program, resolved->source.dwords[word], other_handle->Arg(word));
				for (size_t arg = 1; arg < read->NumArgs(); ++arg)
					same &= EquivalentValue(m_program, read->Arg(arg), other->Arg(arg));
				if (same) break;
			}
			const bool keep = slot == m_program.srt_reads.size();
			if (keep) m_program.srt_reads.push_back({Value(read), slot});
			auto* block = read->Parent();
			auto& list = block->Instructions();
			const auto where = std::ranges::find_if(list, [&](const Inst& inst) {
				return &inst == read;
			});
			const auto resource = Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(where, ValueOpcode::ReadConst,
			    {resource, Value(slot)}, read->Flags<uint64_t>()));
			const auto uses = read->Uses();
			for (const auto& use: uses) use.user->SetArg(use.operand, flat);
			for (auto& entry: m_resolved_handles) {
				for (uint32_t word = 0; word < entry.source.dword_count; ++word)
					if (entry.source.dwords[word].Resolve() == Value(read))
						entry.source.dwords[word] = flat;
			}
			for (auto& info: m_program.block_info) {
				if (info.condition.Resolve() == Value(read)) info.condition = flat;
				if (info.indirect_target.Resolve() == Value(read)) info.indirect_target = flat;
			}
			if (keep) {
				if (resolved->planning_handle.IsEmpty()) {
					bool unchanged = true;
					for (uint32_t word = 0; word < resolved->source.dword_count; ++word)
						unchanged &= handle->Arg(word).Resolve() == resolved->source.dwords[word].Resolve();
					resolved->planning_handle = read->Arg(0);
					if (!unchanged) {
						auto& retained = m_program.value_storage.emplace_back(handle->GetOpcode());
						for (uint32_t word = 0; word < resolved->source.dword_count; ++word)
							retained.SetArg(word, resolved->source.dwords[word]);
						resolved->planning_handle = Value(&retained);
					}
				}
				read->SetArg(0, resolved->planning_handle);
				read->SetParent(nullptr);
				m_program.value_storage.splice(m_program.value_storage.end(), list, where);
			} else {
				list.erase(where);
			}
		}
		m_program.srt_plan_complete = true;
	}

	bool ValidateSource(const DescriptorSource& descriptor, uint32_t& bad_dword) const {
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			bad_dword = i;
			if (descriptor.dwords[i].Resolve().GetType() != Type::U32) {
				return false;
			}
			if (!ValidateRuntimeValue(m_program, descriptor.dwords[i])) {
				return false;
			}
		}
		return true;
	}

	uint32_t InternSource(const DescriptorSource& descriptor) {
		for (uint32_t candidate = 0; candidate < m_sources.size(); candidate++) {
			const auto& current = m_sources[candidate];
			if (current.dword_count != descriptor.dword_count ||
			    current.indirect_image.has_value() != descriptor.indirect_image.has_value()) {
				continue;
			}
			if (current.indirect_image.has_value()) {
				const auto& a = *current.indirect_image;
				const auto& b = *descriptor.indirect_image;
				if (a.material_source != b.material_source || a.table_source != b.table_source ||
				    a.selector_stride != b.selector_stride ||
				    a.selector_offset != b.selector_offset || a.key_shift != b.key_shift ||
				    a.key_mask != b.key_mask || a.bindless != b.bindless ||
				    a.table_offset != b.table_offset ||
				    !EquivalentValue(m_program, a.key_count, b.key_count) ||
				    a.selector_mask.IsEmpty() != b.selector_mask.IsEmpty() ||
				    (!a.selector_mask.IsEmpty() &&
				     !EquivalentValue(m_program, a.selector_mask, b.selector_mask)))
					continue;
			}
			bool same = true;
			for (uint32_t i = 0; i < descriptor.dword_count; i++) {
				same = same && EquivalentValue(m_program, current.dwords[i], descriptor.dwords[i]);
			}
			if (same) {
				return candidate;
			}
		}
		m_sources.push_back(descriptor);
		return static_cast<uint32_t>(m_sources.size() - 1);
	}

	static bool ImmediateU32(Value value, uint32_t& result) {
		value = value.Resolve();
		if (!value.IsImmediate() || value.GetType() != Type::U32) {
			return false;
		}
		result = value.U32();
		return true;
	}

	// A phi web with no user outside itself is never read: the register merge at a join keeps
	// a dead loop-carried copy of a scalar alive, and dead-code elimination cannot break the
	// cycle.
	static bool FeedsOnlyDeadPhis(const Inst& user) {
		std::vector<const Inst*> pending {&user};
		std::vector<const Inst*> visited;
		while (!pending.empty()) {
			const auto* current = pending.back();
			pending.pop_back();
			if (std::ranges::find(visited, current) != visited.end()) {
				continue;
			}
			if (current->GetOpcode() != ValueOpcode::Phi) {
				return false;
			}
			visited.push_back(current);
			for (const auto& use: current->Uses()) {
				pending.push_back(use.user);
			}
		}
		return true;
	}

	// True when value reaches nothing but sampler handles, through phis and selects: once the
	// handles take a fixed sampler, the value is stranded. A phi web that ends nowhere counts too.
	static bool FeedsOnlySamplers(const Inst& value) {
		std::vector<const Inst*> pending {&value};
		std::vector<const Inst*> visited;
		while (!pending.empty()) {
			const auto* current = pending.back();
			pending.pop_back();
			if (std::ranges::find(visited, current) != visited.end()) {
				continue;
			}
			visited.push_back(current);
			for (const auto& use: current->Uses()) {
				const auto op = use.user->GetOpcode();
				if (op == ValueOpcode::GetSamplerResource) {
					continue;
				}
				if (op != ValueOpcode::Phi && op != ValueOpcode::SelectU32) {
					return false;
				}
				pending.push_back(use.user);
			}
		}
		return true;
	}

	static bool UsesOnly(const Inst& value, std::span<const Inst* const> users) {
		return !value.Uses().empty() && std::ranges::all_of(value.Uses(), [&](const Use& use) {
			return std::ranges::find(users, use.user) != users.end() ||
			       FeedsOnlyDeadPhis(*use.user);
		});
	}

	const MemoryInfo* ScalarReadMemory(const Inst& read, uint32_t& index) const {
		const bool address = read.GetOpcode() == ValueOpcode::LoadAddressU32;
		if (!(address ? read.NumArgs() == 4u
		              : read.GetOpcode() == ValueOpcode::ReadConstBuffer && read.NumArgs() == 2u)) {
			return nullptr;
		}
		if (address) {
			const auto high    = read.Arg(2).Resolve();
			const auto enabled = read.Arg(3).Resolve();
			if (!high.IsImmediate() || high.GetType() != Type::U32 || high.U32() != 0u ||
			    !enabled.IsImmediate() || enabled.GetType() != Type::U1 || !enabled.U1()) {
				return nullptr;
			}
		}
		index = read.Flags<MemoryFlags>().index;
		if (index >= m_program.memory_info.size()) {
			return nullptr;
		}
		const auto& memory = m_program.memory_info[index];
		return memory.kind ==
		                   (address ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer) &&
		               memory.data_bits == 32u && memory.data_dwords == 1u
		           ? &memory
		           : nullptr;
	}

	static bool IsMemoryAccess(const Inst& inst) {
		const auto op = inst.GetOpcode();
		return BufferAccessOf(op) != BufferAccess::None ||
		       AddressOpcodeInfoOf(op).access != AddressAccess::None ||
		       ImageOpcodeInfoOf(op).access != ImageAccess::None;
	}

	// Whether no memory access other than owner uses memory_info[index]. Only the indirect-image
	// and bindless-sampler planning asks, after PlanScalarReads and dead-code elimination, and
	// it adds, removes and re-indexes no memory accesses, so each index's users are counted once
	// per shader; scanning the whole program per call made tracking quadratic (seconds per title
	// load).
	bool MemoryIndexBelongsTo(uint32_t index, const Inst& owner) const {
		if (!m_memory_users_counted) {
			for (const auto* block: m_program.blocks) {
				for (const auto& inst: *block) {
					if (IsMemoryAccess(inst)) {
						m_memory_users[inst.Flags<MemoryFlags>().index]++;
					}
				}
			}
			m_memory_users_counted = true;
		}
		const auto     users = m_memory_users.find(index);
		const uint32_t count = users == m_memory_users.end() ? 0u : users->second;
		const bool     owned = IsMemoryAccess(owner) && owner.Flags<MemoryFlags>().index == index;
		return count == (owned ? 1u : 0u);
	}

	bool MakeRuntimeTableSource(const Inst& read, DescriptorSource& descriptor) {
		const auto* handle = read.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr) return false;
		const auto width = handle->GetOpcode() == ValueOpcode::GetBufferResource ? 4u
		                 : handle->GetOpcode() == ValueOpcode::GetAddressResource ? 2u : 0u;
		if (width == 0u) {
			return false;
		}
		const auto flags = read.Flags<MemoryFlags>();
		const auto kind = m_program.memory_info[flags.index].kind;
		const auto base = kind == ResourceKind::ScalarAddress || kind == ResourceKind::ScalarBuffer
		    ? ScalarReadBase(read) : UINT32_MAX;
		MakeSource(*handle, width, false, false, base, descriptor, flags.pc);
		uint32_t bad_dword = 0;
		return ValidateSource(descriptor, bad_dword);
	}

	// A value computed only from scalar sources is the same in every lane: every IR branch
	// condition is scalar (SCC, VCCZ, EXECZ), so a phi of such values is too, and per-lane
	// values only come from vector sources and lane selects on per-lane conditions. Anything
	// not listed counts as per-lane.
	bool IsWaveUniform(Value value) const {
		std::vector<const Inst*>        pending;
		std::unordered_set<const Inst*> visited;
		const auto                      push = [&](Value operand) {
			operand = operand.Resolve();
			if (operand.IsImmediate()) {
				return true;
			}
			const auto* inst = operand.TryInstruction();
			if (inst == nullptr) {
				return false;
			}
			if (visited.insert(inst).second) {
				pending.push_back(inst);
			}
			return true;
		};
		if (!push(value)) {
			return false;
		}
		while (!pending.empty()) {
			const auto* inst = pending.back();
			pending.pop_back();
			switch (inst->GetOpcode()) {
				case ValueOpcode::ReadFirstLane:
				case ValueOpcode::ReadLane:
				case ValueOpcode::GetUserData:
				case ValueOpcode::GetShaderBase:
				case ValueOpcode::GetSrtResource:
				case ValueOpcode::ReadConst:
				case ValueOpcode::UndefU1:
				case ValueOpcode::UndefU32: continue;
				case ValueOpcode::LoadAddressU32: {
					uint32_t index = 0;
					if (ScalarReadMemory(*inst, index) == nullptr) {
						return false;
					}
				} break;
				case ValueOpcode::ReadConstBuffer:
				case ValueOpcode::GetBufferResource:
				case ValueOpcode::GetAddressResource:
				case ValueOpcode::Phi:
				case ValueOpcode::IAdd32:
				case ValueOpcode::ISub32:
				case ValueOpcode::IMul32:
				case ValueOpcode::ShiftLeftLogical32:
				case ValueOpcode::ShiftRightLogical32:
				case ValueOpcode::ShiftRightArithmetic32:
				case ValueOpcode::BitwiseAnd32:
				case ValueOpcode::BitwiseOr32:
				case ValueOpcode::BitwiseXor32:
				case ValueOpcode::BitwiseNot32:
				case ValueOpcode::BitFieldUExtract:
				case ValueOpcode::BitCount32:
				case ValueOpcode::FindILsb32:
				case ValueOpcode::FindUMsb32:
				case ValueOpcode::SMin32:
				case ValueOpcode::UMin32:
				case ValueOpcode::SMax32:
				case ValueOpcode::UMax32:
				case ValueOpcode::SLessThan32:
				case ValueOpcode::ULessThan32:
				case ValueOpcode::IEqual32:
				case ValueOpcode::INotEqual32:
				case ValueOpcode::SLessThanEqual32:
				case ValueOpcode::ULessThanEqual32:
				case ValueOpcode::SGreaterThanEqual32:
				case ValueOpcode::UGreaterThanEqual32:
				case ValueOpcode::LogicalAnd:
				case ValueOpcode::LogicalOr:
				case ValueOpcode::LogicalNot:
				case ValueOpcode::SelectU1:
				case ValueOpcode::SelectU32: break;
				default: return false;
			}
			for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
				if (!push(inst->Arg(arg))) {
					return false;
				}
			}
		}
		return true;
	}

	bool MatchMaterialOffset(Value value, Value& selector, uint32_t& stride,
	                         uint32_t& offset) const {
		value           = value.Resolve();
		offset          = 0;
		auto* candidate = value.TryInstruction();
		if (candidate != nullptr && candidate->GetOpcode() == ValueOpcode::IAdd32 &&
		    candidate->NumArgs() == 2u) {
			uint32_t immediate = 0;
			if (ImmediateU32(candidate->Arg(0), immediate)) {
				value = candidate->Arg(1).Resolve();
			} else if (ImmediateU32(candidate->Arg(1), immediate)) {
				value = candidate->Arg(0).Resolve();
			} else {
				return false;
			}
			offset = immediate;
		}
		const auto* multiply = value.TryInstruction();
		uint32_t    shift    = 0;
		if (multiply != nullptr && multiply->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
		    multiply->NumArgs() == 2u && ImmediateU32(multiply->Arg(1), shift) && shift < 32u) {
			// A power-of-two record size is usually a shift, not a multiply.
			stride                    = 1u << shift;
			selector                  = multiply->Arg(0).Resolve();
			const auto* selector_inst = selector.TryInstruction();
			return selector_inst != nullptr &&
			       (selector_inst->GetOpcode() == ValueOpcode::ReadFirstLane ||
			        IsWaveUniform(selector));
		}
		if (multiply == nullptr || multiply->GetOpcode() != ValueOpcode::IMul32 ||
		    multiply->NumArgs() != 2u) {
			return false;
		}
		if (ImmediateU32(multiply->Arg(0), stride)) {
			selector = multiply->Arg(1).Resolve();
		} else if (ImmediateU32(multiply->Arg(1), stride)) {
			selector = multiply->Arg(0).Resolve();
		} else {
			return false;
		}
		const auto* selector_inst = selector.TryInstruction();
		return stride != 0u && selector_inst != nullptr &&
		       (selector_inst->GetOpcode() == ValueOpcode::ReadFirstLane ||
		        IsWaveUniform(selector));
	}

	enum class LaneQuantifier { Any, All };
	struct EdgePredicate {
		Value condition;
		bool positive;
		LaneQuantifier lanes = LaneQuantifier::All;
	};

	bool NonzeroOnEntry(Value value, const Block* block) const {
		if (m_program.blocks.size() != m_program.block_info.size()) {
			return false;
		}
		// Each unique predecessor must execute before this use. Stop at joins: an
		// unrelated comparison is not a bound on FindILsb's zero-input sentinel.
		for (size_t depth = 0; block != nullptr && depth < m_program.blocks.size(); ++depth) {
			if (block->ImmPredecessors().size() != 1u) {
				return false;
			}
			const auto* previous = block->ImmPredecessors()[0];
			const auto edge = ConditionalEdge(previous, block);
			if (edge && edge->lanes == LaneQuantifier::All) {
				const auto* test = edge->condition.TryInstruction();
				if (test != nullptr && test->NumArgs() == 2u &&
				    ((test->GetOpcode() == ValueOpcode::INotEqual32 && edge->positive) ||
				     (test->GetOpcode() == ValueOpcode::IEqual32 && !edge->positive))) {
					for (uint32_t arg = 0; arg < 2u; ++arg) {
						uint32_t immediate;
						if (ImmediateU32(test->Arg(arg), immediate) && immediate == 0u &&
						    EquivalentValue(m_program, test->Arg(arg ^ 1u), value)) {
							return true;
						}
					}
				}
			}
			block = previous;
		}
		return false;
	}

	// The byte offset of a table read as (key << record_shift) + immediate: 32-byte T# records for
	// image heaps, 16-byte S# records for sampler heaps.
	// A record's byte offset: key << record_shift plus immediate additions. With key_mask, the
	// shifted key may also be masked, (key << shift) & m with the low shift bits of m clear
	// (glass PS: (key << 5) & 0x01ffffe0); that equals (key & (m >> shift)) << shift, and
	// key_mask receives m >> shift (UINT32_MAX without a mask). Without it, a mask is rejected.
	bool MatchTableOffset(Value value, Value& key, uint32_t& offset, uint32_t record_shift = 5u,
	                      uint32_t* key_mask = nullptr) const {
		offset = 0;
		if (key_mask != nullptr) {
			*key_mask = UINT32_MAX;
		}
		for (;;) {
			const auto* inst = value.Resolve().TryInstruction();
			if (inst == nullptr || inst->NumArgs() != 2u) {
				return false;
			}
			uint32_t immediate;
			if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
			    ImmediateU32(inst->Arg(1), immediate) && immediate == record_shift) {
				key = inst->Arg(0).Resolve();
				return key.GetType() == Type::U32;
			}
			if (inst->GetOpcode() == ValueOpcode::BitwiseAnd32 && key_mask != nullptr &&
			    *key_mask == UINT32_MAX) {
				// The mask must apply to the shift itself, not to a sum around it.
				const uint32_t masked = ImmediateU32(inst->Arg(1), immediate)   ? 0u
				                        : ImmediateU32(inst->Arg(0), immediate) ? 1u
				                                                                : 2u;
				const auto* shift = masked < 2u ? inst->Arg(masked).Resolve().TryInstruction()
				                                : nullptr;
				uint32_t    amount = 0;
				if (shift == nullptr || (immediate & ((1u << record_shift) - 1u)) != 0u ||
				    shift->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
				    shift->NumArgs() != 2u || !ImmediateU32(shift->Arg(1), amount) ||
				    amount != record_shift) {
					return false;
				}
				*key_mask = immediate >> record_shift;
				value     = inst->Arg(masked);
				continue;
			}
			if (inst->GetOpcode() != ValueOpcode::IAdd32) {
				return false;
			}
			if (ImmediateU32(inst->Arg(0), immediate)) {
				value = inst->Arg(1);
			} else if (ImmediateU32(inst->Arg(1), immediate)) {
				value = inst->Arg(0);
			} else {
				return false;
			}
			// These additions are shader U32 arithmetic, before the scalar memory offset.
			offset += immediate;
		}
	}

	std::optional<EdgePredicate> ConditionalEdge(const Block* from, const Block* to) const {
		const auto position = std::ranges::find(m_program.blocks, from);
		const auto target = std::ranges::find(m_program.blocks, to);
		if (position == m_program.blocks.end() || target == m_program.blocks.end()) return {};
		const auto& info = m_program.block_info[position - m_program.blocks.begin()];
		const auto& term = info.terminator;
		const auto  id   = m_program.block_info[target - m_program.blocks.begin()].id;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch ||
		    (term.true_block == id) == (term.false_block == id)) return {};
		EdgePredicate edge {info.condition, term.true_block == id};
		while (const auto* inst = edge.condition.Resolve().TryInstruction()) {
			if (inst->GetOpcode() == ValueOpcode::LogicalNot) {
				edge.positive = !edge.positive;
			} else if (inst->GetOpcode() == ValueOpcode::ConditionRef) {
				const auto kind = inst->Flags<CFG::BranchCondition>();
				const bool scalar = kind == CFG::BranchCondition::SccZero ||
				                    kind == CFG::BranchCondition::SccNonZero;
				const bool zero = kind == CFG::BranchCondition::ExecZero ||
				                  kind == CFG::BranchCondition::VccZero;
				const bool nonzero = kind == CFG::BranchCondition::ExecNonZero ||
				                     kind == CFG::BranchCondition::VccNonZero;
				if (!scalar && !zero && !nonzero) break;
				// SCC is uniform. Negating a lane reduction exchanges all and any.
				edge.lanes = scalar || (zero == edge.positive) ? LaneQuantifier::All
				                                              : LaneQuantifier::Any;
			} else {
				break;
			}
			edge.condition = inst->Arg(0);
		}
		edge.condition = edge.condition.Resolve();
		return edge;
	}

	Value PositiveLaneWitness(const Block* use) const {
		if (use == nullptr || use->ImmPredecessors().size() != 1u) return {};
		const auto edge = ConditionalEdge(use->ImmPredecessors()[0], use);
		return edge && edge->positive ? edge->condition : Value {};
	}

	bool HasActiveLane(Value mask, const Block* use) const {
		mask = mask.Resolve();
		const auto incoming_is_nonempty = [&](const Block* from, const Block* to,
		                                     Value incoming, const Block* header) {
			for (size_t depth = 0; depth < m_program.blocks.size(); ++depth) {
				const auto edge = ConditionalEdge(from, to);
				if (edge && edge->positive && Implies(edge->condition, incoming)) return true;
				if (from == header || from->ImmSuccessors().size() != 1u ||
				    from->ImmPredecessors().size() != 1u) return false;
				to = from;
				from = from->ImmPredecessors()[0];
			}
			return false;
		};
		const auto* phi = mask.TryInstruction();
		if (phi != nullptr && phi->GetOpcode() == ValueOpcode::Phi &&
		    phi->GetType() == Type::U1 && phi->NumArgs() != 0u) {
			for (size_t arm = 0; arm < phi->NumArgs(); ++arm) {
				if (!incoming_is_nonempty(phi->PhiBlock(arm), phi->Parent(),
				                          phi->Arg(arm), phi->Parent())) return false;
			}
			return true;
		}
		return use != nullptr && use->ImmPredecessors().size() == 1u &&
		       incoming_is_nonempty(use->ImmPredecessors()[0], use, mask, nullptr);
	}

	Value SimplifyGuard(Value guard) const {
		const auto invariant = ResolveInvariantPhi(m_program, guard);
		return (invariant.IsEmpty() ? guard : invariant).Resolve();
	}

	bool Implies(Value guard, Value required) const {
		guard    = SimplifyGuard(guard);
		required = required.Resolve();
		if (EquivalentValue(m_program, guard, required)) return true;
		const auto* inst = guard.TryInstruction();
		return inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalAnd &&
		       (Implies(inst->Arg(0), required) || Implies(inst->Arg(1), required));
	}

	Value EqualLocalKey(Value guard, Value key) const {
		guard            = SimplifyGuard(guard);
		const auto* inst = guard.TryInstruction();
		if (inst == nullptr) return {};
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			const auto left = EqualLocalKey(inst->Arg(0), key);
			return left.IsEmpty() ? EqualLocalKey(inst->Arg(1), key) : left;
		}
		if (inst->GetOpcode() != ValueOpcode::IEqual32 || inst->NumArgs() != 2u) return {};
		if (EquivalentValue(m_program, inst->Arg(0), key)) return inst->Arg(1).Resolve();
		if (EquivalentValue(m_program, inst->Arg(1), key)) return inst->Arg(0).Resolve();
		return {};
	}

	struct AffineOffset {
		Value    index;
		uint64_t stride = 0;
		uint64_t offset = 0;
	};

	bool MatchAffineOffset(Value value, Value guard, AffineOffset& out, uint32_t depth = 0) const {
		if (depth > 16u) return false;
		value              = value.Resolve();
		uint32_t immediate = 0;
		if (ImmediateU32(value, immediate)) {
			out.offset = immediate;
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::SelectU32 && inst->NumArgs() == 3u &&
		    Implies(guard, inst->Arg(0))) {
			return MatchAffineOffset(inst->Arg(1), guard, out, depth + 1u);
		}
		if (inst->GetOpcode() == ValueOpcode::IAdd32 && inst->NumArgs() == 2u) {
			AffineOffset left, right;
			if (!MatchAffineOffset(inst->Arg(0), guard, left, depth + 1u) ||
			    !MatchAffineOffset(inst->Arg(1), guard, right, depth + 1u) ||
			    (!left.index.IsEmpty() && !right.index.IsEmpty() &&
			     !EquivalentValue(m_program, left.index, right.index)))
				return false;
			out.index  = left.index.IsEmpty() ? right.index : left.index;
			out.stride = left.stride + right.stride;
			out.offset = left.offset + right.offset;
			return out.stride <= UINT32_MAX && out.offset <= UINT32_MAX;
		}
		if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 && inst->NumArgs() == 2u &&
		    ImmediateU32(inst->Arg(1), immediate) && immediate < 32u) {
			if (!MatchAffineOffset(inst->Arg(0), guard, out, depth + 1u)) return false;
			out.stride <<= immediate;
			out.offset <<= immediate;
			return out.stride <= UINT32_MAX && out.offset <= UINT32_MAX;
		}
		out.index  = value;
		out.stride = 1u;
		return value.GetType() == Type::U32;
	}

	bool ImpliesNonzero(Value guard, Value value) const {
		guard            = SimplifyGuard(guard);
		const auto* inst = guard.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			return ImpliesNonzero(inst->Arg(0), value) || ImpliesNonzero(inst->Arg(1), value);
		}
		if (inst->GetOpcode() != ValueOpcode::INotEqual32 || inst->NumArgs() != 2u) return false;
		uint32_t zero = 1u;
		return (ImmediateU32(inst->Arg(0), zero) && zero == 0u &&
		        EquivalentValue(m_program, inst->Arg(1), value)) ||
		       (ImmediateU32(inst->Arg(1), zero) && zero == 0u &&
		        EquivalentValue(m_program, inst->Arg(0), value));
	}

	bool ImpliesIndexBelow32(Value guard, Value index) const {
		guard            = SimplifyGuard(guard);
		const auto* inst = guard.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			return ImpliesIndexBelow32(inst->Arg(0), index) ||
			       ImpliesIndexBelow32(inst->Arg(1), index);
		}
		if (inst->NumArgs() != 2u) return false;
		uint32_t limit = 0;
		return (inst->GetOpcode() == ValueOpcode::SLessThan32 &&
		        EquivalentValue(m_program, inst->Arg(0), index) &&
		        ImmediateU32(inst->Arg(1), limit) && limit == 32u) ||
		       (inst->GetOpcode() == ValueOpcode::SGreaterThan32 &&
		        ImmediateU32(inst->Arg(0), limit) && limit == 32u &&
		        EquivalentValue(m_program, inst->Arg(1), index));
	}

	bool MaskOnlyLosesBits(Value value, Value mask) const {
		const auto* update = value.Resolve().TryInstruction();
		if (update == nullptr || update->NumArgs() != 2u) return false;
		if (update->GetOpcode() == ValueOpcode::BitwiseAnd32) {
			return EquivalentValue(m_program, update->Arg(0), mask) ||
			       EquivalentValue(m_program, update->Arg(1), mask);
		}
		if (update->GetOpcode() != ValueOpcode::BitwiseXor32) return false;
		Value bit;
		if (EquivalentValue(m_program, update->Arg(0), mask))
			bit = update->Arg(1);
		else if (EquivalentValue(m_program, update->Arg(1), mask))
			bit = update->Arg(0);
		else
			return false;
		const auto* shift = bit.Resolve().TryInstruction();
		uint32_t    one   = 0;
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    shift->NumArgs() != 2u || !ImmediateU32(shift->Arg(0), one) || one != 1u)
			return false;
		Value       position  = shift->Arg(1).Resolve();
		const auto* masked    = position.TryInstruction();
		uint32_t    lane_mask = 0;
		if (masked != nullptr && masked->GetOpcode() == ValueOpcode::BitwiseAnd32 &&
		    masked->NumArgs() == 2u) {
			if (ImmediateU32(masked->Arg(0), lane_mask) && lane_mask == 31u)
				position = masked->Arg(1);
			else if (ImmediateU32(masked->Arg(1), lane_mask) && lane_mask == 31u)
				position = masked->Arg(0);
		}
		const auto* first = position.Resolve().TryInstruction();
		return first != nullptr && first->GetOpcode() == ValueOpcode::FindILsb32 &&
		       first->NumArgs() == 1u && EquivalentValue(m_program, first->Arg(0), mask);
	}

	Value InitialCandidateMask(Value value, const Block* update_block) const {
		const auto* phi = value.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi || phi->NumArgs() != 2u ||
		    phi->GetType() != Type::U32)
			return {};
		for (uint32_t back = 0; back < 2u; ++back) {
			if (phi->PhiBlock(back) != update_block ||
			    !MaskOnlyLosesBits(phi->Arg(back), value)) continue;
			const auto initial = phi->Arg(back ^ 1u).Resolve();
			return ValidateRuntimeValue(m_program, initial, RuntimeValueType::Integer) ? initial
			                                                                           : Value {};
		}
		return {};
	}

	bool MaskEdge(const Block* from, const Block* to, Value predicate, bool positive) const {
		const auto edge = ConditionalEdge(from, to);
		// Continuation needs an active lane; an inactive exit must include every lane.
		return edge && edge->positive == positive &&
		       (positive || edge->lanes == LaneQuantifier::All) &&
		       EquivalentValue(m_program, edge->condition, predicate);
	}

	Value BoundedSetBitMask(Value index, Value guard) const {
		const auto* phi = index.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi || phi->NumArgs() != 3u ||
		    phi->GetType() != Type::U32 || !ImpliesIndexBelow32(guard, index))
			return {};
		for (uint32_t bit_arm = 0; bit_arm < 3u; ++bit_arm) {
			const auto bit_guard = PositiveLaneWitness(phi->PhiBlock(bit_arm));
			const auto* selected = phi->Arg(bit_arm).Resolve().TryInstruction();
			if (selected == nullptr || selected->GetOpcode() != ValueOpcode::SelectU32 ||
			    selected->NumArgs() != 3u || !Implies(bit_guard, selected->Arg(0)))
				continue;
			const auto* first = selected->Arg(1).Resolve().TryInstruction();
			if (first == nullptr || first->GetOpcode() != ValueOpcode::FindILsb32 ||
			    first->NumArgs() != 1u || !ImpliesNonzero(bit_guard, first->Arg(0)))
				continue;
			const auto initial_mask = InitialCandidateMask(first->Arg(0), phi->PhiBlock(bit_arm));
			if (initial_mask.IsEmpty()) continue;
			for (uint32_t sentinel_arm = 0; sentinel_arm < 3u; ++sentinel_arm) {
				if (sentinel_arm == bit_arm) continue;
				const auto sentinel_guard = PositiveLaneWitness(phi->PhiBlock(sentinel_arm));
				const auto* sentinel = phi->Arg(sentinel_arm).Resolve().TryInstruction();
				uint32_t bound = 0;
				if (sentinel == nullptr || sentinel->GetOpcode() != ValueOpcode::SelectU32 ||
				    sentinel->NumArgs() != 3u || !Implies(sentinel_guard, sentinel->Arg(0)) ||
				    !ImmediateU32(sentinel->Arg(1), bound) || bound != 32u)
					continue;
				const auto  loop_active = sentinel->Arg(0).Resolve();
				const auto* active_phi  = loop_active.TryInstruction();
				if (active_phi == nullptr || active_phi->GetOpcode() != ValueOpcode::Phi ||
				    active_phi->NumArgs() != 2u) continue;
				const auto other_arm = 3u - bit_arm - sentinel_arm;
				const auto* carried = phi->Arg(other_arm).Resolve().TryInstruction();
				const auto* bit_block = phi->PhiBlock(bit_arm);
				if (carried == nullptr || carried->GetOpcode() != ValueOpcode::Phi ||
				    carried->NumArgs() != 2u || carried->Parent() != active_phi->Parent() ||
				    selected->Arg(2).Resolve() != phi->Arg(sentinel_arm).Resolve() ||
				    sentinel->Arg(2).Resolve() != phi->Arg(other_arm).Resolve()) continue;
				const uint32_t back = carried->PhiBlock(0) == bit_block ? 0u : 1u;
				if (carried->PhiBlock(back) != bit_block ||
				    carried->Arg(back).Resolve() != phi->Arg(bit_arm).Resolve()) continue;
				bool invariant = false;
				for (uint32_t initial = 0; initial < 2u; ++initial) {
					invariant = active_phi->PhiBlock(initial) == carried->PhiBlock(back ^ 1u) &&
					            active_phi->PhiBlock(initial ^ 1u) == bit_block &&
					            Implies(guard, active_phi->Arg(initial)) &&
					            MaskEdge(bit_block, active_phi->Parent(),
					                     active_phi->Arg(initial ^ 1u), true);
					if (invariant) break;
				}
				if (!invariant) continue;
				if (MaskEdge(phi->PhiBlock(other_arm), phi->Parent(), loop_active, false))
					return initial_mask;
			}
		}
		return {};
	}

	bool MatchUniformizedMaterialKey(Value key, const Inst& image,
	                                DescriptorSource::IndirectImage& indirect,
	                                DescriptorSource& material_source) {
		Value guard;
		Value local;
		const auto* first = key.Resolve().TryInstruction();
		if (first != nullptr && first->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    first->NumArgs() == 2u) {
			guard = first->Arg(1).Resolve();
			// Empty EXEC selects lane zero, which may not have loaded a material key.
			if (!HasActiveLane(guard, first->Parent())) return false;
			local = first->Arg(0).Resolve();
			const auto* phi = guard.TryInstruction();
			if (phi != nullptr && phi->GetOpcode() == ValueOpcode::Phi &&
			    phi->GetType() == Type::U1 && phi->NumArgs() == 2u) {
				for (uint32_t initial = 0; initial < 2u; ++initial) {
					if (Implies(phi->Arg(initial ^ 1u), guard)) {
						// The backedge only removes lanes from the initial mask.
						guard = phi->Arg(initial).Resolve();
						break;
					}
				}
			}
		} else {
			guard = PositiveLaneWitness(image.Parent());
			if (guard.IsEmpty()) return false;
			local = EqualLocalKey(guard, key);
		}
		const auto* selected = local.Resolve().TryInstruction();
		if (selected == nullptr || selected->GetOpcode() != ValueOpcode::SelectU32 ||
		    selected->NumArgs() != 3u || !Implies(guard, selected->Arg(0)))
			return false;
		const auto  active = selected->Arg(0).Resolve();
		const auto* read   = selected->Arg(1).Resolve().TryInstruction();
		if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadAddressU32 ||
		    read->NumArgs() != 4u || !EquivalentValue(m_program, read->Arg(3), active))
			return false;
		uint32_t high = 1u;
		if (!ImmediateU32(read->Arg(2), high) || high != 0u) return false;
		const auto memory_index = read->Flags<MemoryFlags>().index;
		if (memory_index >= m_program.memory_info.size()) return false;
		const auto& memory = m_program.memory_info[memory_index];
		if (memory.kind != ResourceKind::Global || memory.data_bits != 32u ||
		    memory.data_dwords != 1u || !MemoryIndexBelongsTo(memory_index, *read))
			return false;
		const auto* material_handle = read->Arg(0).Resolve().TryInstruction();
		if (material_handle == nullptr ||
		    material_handle->GetOpcode() != ValueOpcode::GetAddressResource ||
		    !MakeRuntimeTableSource(*read, material_source)) return false;
		AffineOffset offset;
		if (!MatchAffineOffset(read->Arg(1), active, offset) || offset.index.IsEmpty() ||
		    offset.stride == 0u || offset.offset + memory.offset > UINT32_MAX ||
		    offset.offset + memory.offset + 31u * offset.stride + 4u >
		        static_cast<uint64_t>(UINT32_MAX) + 1u)
			return false;
		const auto mask = BoundedSetBitMask(offset.index, active);
		if (mask.IsEmpty()) return false;
		indirect.material_source = InternSource(material_source);
		indirect.selector_stride = static_cast<uint32_t>(offset.stride);
		indirect.selector_offset = static_cast<uint32_t>(offset.offset + memory.offset);
		indirect.key_count       = Value(32u);
		indirect.selector_mask   = mask;
		return true;
	}

	Value BoundedLoopCount(Value key, const Block* use) const {
		const auto* phi = key.Resolve().TryInstruction();
		if (m_shader_writes || phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->GetType() != Type::U32 || phi->NumArgs() != 2u ||
		    m_program.blocks.size() != m_program.block_info.size()) return {};
		const Block* increment_block = nullptr;
		for (uint32_t initial = 0; initial < 2u; ++initial) {
			const auto  zero = phi->Arg(initial).Resolve();
			const auto* step = phi->Arg(initial ^ 1u).Resolve().TryInstruction();
			if (!zero.IsImmediate() || zero.GetType() != Type::U32 || zero.U32() != 0u ||
			    step == nullptr || step->GetOpcode() != ValueOpcode::IAdd32 ||
			    step->Parent() != phi->PhiBlock(initial ^ 1u)) continue;
			uint32_t increment = 0;
			if ((step->Arg(0).Resolve() == key &&
			     ImmediateU32(step->Arg(1), increment) && increment == 1u) ||
			    (step->Arg(1).Resolve() == key &&
			     ImmediateU32(step->Arg(0), increment) && increment == 1u)) {
				increment_block = step->Parent();
				break;
			}
		}
		if (increment_block == nullptr) return {};

		const auto guarded_on_entry = [&](const Block* block, const auto& accepts) {
			std::vector<const Block*> pending {block};
			for (size_t i = 0; i < pending.size(); ++i) {
				const auto* current = pending[i];
				if (current == phi->Parent() || current->ImmPredecessors().empty()) return false;
				for (const auto* previous: current->ImmPredecessors()) {
					const auto edge = ConditionalEdge(previous, current);
					if (edge && accepts(*edge)) continue;
					if (std::ranges::find(pending, previous) == pending.end())
						pending.push_back(previous);
				}
			}
			return true;
		};
		for (const auto& use_of_key: phi->Uses()) {
			const auto* compare = use_of_key.user;
			if (compare->GetOpcode() != ValueOpcode::SLessThan32 || use_of_key.operand != 0u ||
			    !ValidateRuntimeValue(m_program, compare->Arg(1))) continue;
			if (!guarded_on_entry(use, [&](const EdgePredicate& edge) {
				return edge.positive && Implies(edge.condition, Value(use_of_key.user));
			})) continue;
			LoopBoundProof proof(m_program, *phi, *compare);
			// The image bound and the increment guard are separate obligations: a
			// skipped image alone does not prevent signed induction wraparound.
			if (guarded_on_entry(increment_block, [&](const EdgePredicate& edge) {
				return proof.Excludes(edge.condition, edge.positive);
			})) return compare->Arg(1);
		}
		return {};
	}

	// Research diagnostics: the source line of the check that turned an image down as an
	// indirect image, appended to its "not a valid runtime value" failure.
	bool RejectIndirect(const Inst& handle, int line) {
		m_indirect_rejects[&handle] = line;
		return false;
	}

	// Research: a descriptor picked on control flow -- the same heap read under a different key
	// in each branch, merged by phis (nested when branches join in stages) -- is, for bindless
	// images, one heap read keyed by a mirrored tree of phis over the branch keys. Only buffer
	// (V#) heaps: the key is looked up at run time.
	//
	// A goto-structured region merges every path through it, so a path that skips the sample
	// still feeds the join with whatever its registers held (a loop counter, a material word).
	// Such a branch is free: it gets key 0, and only reads of the table at another offset (a
	// real choice between record fields) turn the image down.
	bool PlanPhiTableRead(Inst& handle, Inst& phi, uint32_t dword, Inst*& table_handle,
	                      Value& key, uint32_t& table_offset, IndirectImagePlan& plan,
	                      std::vector<uint32_t>& live_memory) {
		if (!m_program.bindless_images) {
			return RejectIndirect(handle, __LINE__);
		}
		const bool phi_image_users_only =
		    !phi.Uses().empty() && std::ranges::all_of(phi.Uses(), [](const Use& use) {
			    return use.user->GetOpcode() == ValueOpcode::GetImageResource ||
			           FeedsOnlyDeadPhis(*use.user);
		    });
		if (!phi_image_users_only) {
			return RejectIndirect(handle, __LINE__);
		}
		if (table_handle == nullptr && !ChoosePhiTable(phi, table_handle, table_offset)) {
			return RejectIndirect(handle, __LINE__);
		}
		const Inst* expected = nullptr;
		if (dword != 0u) {
			expected = key.Resolve().TryInstruction();
			if (expected == nullptr || std::ranges::find(m_key_phis, expected) == m_key_phis.end()) {
				return RejectIndirect(handle, __LINE__);
			}
		}
		PhiTreeWalk walk {handle, dword, table_handle, table_offset, live_memory};
		Value       node_key;
		if (!PlanPhiTableNode(walk, phi, expected, false, node_key)) {
			return false;
		}
		if (walk.indices.empty()) {
			return RejectIndirect(handle, __LINE__);
		}
		if (dword == 0u) {
			key = node_key;
			if (walk.free_leaves != 0u) {
				LOGF("shader resource tracking: hash=0x%016" PRIx64 " descriptor phi: %u of %zu "
				     "branches carry no descriptor (key 0)\n",
				     m_program.shader_hash, walk.free_leaves, walk.free_leaves + walk.indices.size());
			}
		}
		plan.memory[dword] = walk.indices.front();
		plan.reads[dword]  = &phi;
		plan.extra_memory.insert(plan.extra_memory.end(), walk.indices.begin(), walk.indices.end());
		plan.extra_reads.insert(plan.extra_reads.end(), walk.reads.begin(), walk.reads.end());
		return true;
	}

	// Whether every branch of a key phi tree that reads the table uses `key` (a later dword
	// read once, under the key its branches share).
	bool KeyPhiUses(const Value& key_phi, const Value& key, uint32_t depth = 0u) const {
		const auto* phi = key_phi.Resolve().TryInstruction();
		if (depth > 8u || phi == nullptr ||
		    std::ranges::find(m_key_phis, phi) == m_key_phis.end()) {
			return false;
		}
		for (size_t i = 0; i < phi->NumArgs(); i++) {
			if (std::ranges::find(m_free_phi_keys, std::pair {phi, i}) != m_free_phi_keys.end()) {
				continue;
			}
			if (!EquivalentValue(m_program, phi->Arg(i), key) &&
			    !KeyPhiUses(phi->Arg(i), key, depth + 1u)) {
				return false;
			}
		}
		return true;
	}

	struct PhiTreeWalk {
		Inst&                    handle;
		uint32_t                 dword;
		Inst*                    table_handle;
		uint32_t                 table_offset;
		std::vector<uint32_t>&   live_memory;
		std::vector<uint32_t>    indices;
		std::vector<const Inst*> reads;
		std::vector<const Inst*> ancestors;
		uint32_t                 free_leaves = 0;
	};

	enum class PhiLeaf { Read, Free, Reject };

	// A leaf of a descriptor phi tree: a read of the table at this dword of the record, a value
	// that is no read of the table (Free), or a read of the table at another offset (Reject).
	PhiLeaf ClassifyPhiLeaf(const PhiTreeWalk& walk, const Inst* read, uint32_t& memory_index,
	                        Value& key) const {
		if (read == nullptr) {
			return PhiLeaf::Free;
		}
		const auto* memory = ScalarReadMemory(*read, memory_index);
		if (memory == nullptr ||
		    (memory->kind != ResourceKind::ScalarBuffer && memory->kind != ResourceKind::Buffer) ||
		    !MemoryIndexBelongsTo(memory_index, *read)) {
			return PhiLeaf::Free;
		}
		auto* current_handle = read->Arg(0).Resolve().TryInstruction();
		if (current_handle == nullptr ||
		    current_handle->GetOpcode() != ValueOpcode::GetBufferResource ||
		    !EquivalentValue(m_program, Value(walk.table_handle), Value(current_handle))) {
			return PhiLeaf::Free;
		}
		uint32_t offset = 0;
		if (memory->offset > INT32_MAX || (memory->offset & 3u) != 0u ||
		    !MatchTableOffset(read->Arg(1), key, offset) || memory->offset > UINT32_MAX - offset ||
		    static_cast<uint64_t>(walk.table_offset) + walk.dword * sizeof(uint32_t) !=
		        offset + memory->offset) {
			return PhiLeaf::Reject;
		}
		return PhiLeaf::Read;
	}

	// The table of a phi-planned descriptor: the buffer most leaves read through (the first
	// leaf can be a free one), and the record offset of its first leaf.
	bool ChoosePhiTable(Inst& phi, Inst*& table_handle, uint32_t& table_offset) const {
		std::vector<const Inst*> pending {&phi};
		std::vector<const Inst*> visited;
		std::vector<std::pair<Inst*, uint32_t>> candidates;
		while (!pending.empty()) {
			const auto* current = pending.back();
			pending.pop_back();
			if (std::ranges::find(visited, current) != visited.end()) {
				continue;
			}
			visited.push_back(current);
			if (current->GetOpcode() == ValueOpcode::Phi) {
				for (size_t arg = current->NumArgs(); arg-- > 0;) {
					if (const auto* next = current->Arg(arg).Resolve().TryInstruction()) {
						pending.push_back(next);
					}
				}
				continue;
			}
			uint32_t    memory_index   = 0;
			const auto* memory         = ScalarReadMemory(*current, memory_index);
			auto*       current_handle = current->Arg(0).Resolve().TryInstruction();
			Value       key;
			uint32_t    offset = 0;
			if (memory == nullptr ||
			    (memory->kind != ResourceKind::ScalarBuffer && memory->kind != ResourceKind::Buffer) ||
			    current_handle == nullptr ||
			    current_handle->GetOpcode() != ValueOpcode::GetBufferResource ||
			    memory->offset > INT32_MAX || !MatchTableOffset(current->Arg(1), key, offset) ||
			    memory->offset > UINT32_MAX - offset) {
				continue;
			}
			candidates.emplace_back(current_handle, offset + memory->offset);
		}
		size_t best = 0;
		size_t best_count = 0;
		for (size_t i = 0; i < candidates.size(); i++) {
			const auto count = std::ranges::count_if(candidates, [&](const auto& other) {
				return EquivalentValue(m_program, Value(candidates[i].first), Value(other.first));
			});
			if (static_cast<size_t>(count) > best_count) {
				best       = i;
				best_count = static_cast<size_t>(count);
			}
		}
		if (best_count == 0u) {
			return false;
		}
		table_handle = candidates[best].first;
		table_offset = candidates[best].second;
		return true;
	}

	// One phi of a descriptor phi tree. For dword 0 it builds the key phi beside `phi`; for the
	// other dwords `expected` is the key phi built for dword 0 at the same place, and every branch
	// must pick the same record for this dword as it did there. `live` marks a subtree whose value
	// is also read by something other than the image.
	bool PlanPhiTableNode(PhiTreeWalk& walk, Inst& phi, const Inst* expected, bool live,
	                      Value& node_key) {
		constexpr size_t MaxDepth = 8u;
		const size_t     count    = phi.NumArgs();
		if (walk.ancestors.size() > MaxDepth || count < 2u || phi.NumPhiBlocks() != count ||
		    std::ranges::find(walk.ancestors, &phi) != walk.ancestors.end()) {
			return RejectIndirect(walk.handle, __LINE__);
		}
		if (expected != nullptr &&
		    (expected->GetOpcode() != ValueOpcode::Phi || expected->Parent() != phi.Parent() ||
		     expected->NumArgs() != count || expected->NumPhiBlocks() != count)) {
			return RejectIndirect(walk.handle, __LINE__);
		}
		walk.ancestors.push_back(&phi);
		const size_t        first_index = walk.indices.size();
		std::vector<Value>  keys(count);
		std::vector<size_t> free_args;
		for (size_t i = 0; i < count; i++) {
			if (expected != nullptr && expected->PhiBlock(i) != phi.PhiBlock(i)) {
				return RejectIndirect(walk.handle, __LINE__);
			}
			auto* read = phi.Arg(i).Resolve().TryInstruction();
			// A read (or an inner phi) with other users keeps its heap load real.
			const bool other_users =
			    live || (read != nullptr && !std::ranges::all_of(read->Uses(), [&](const Use& use) {
				    return use.user == &phi || FeedsOnlyDeadPhis(*use.user);
			    }));
			const bool free_position =
			    expected != nullptr &&
			    std::ranges::find(m_free_phi_keys, std::pair {expected, i}) != m_free_phi_keys.end();
			const auto* expected_arg =
			    expected != nullptr ? expected->Arg(i).Resolve().TryInstruction() : nullptr;
			const bool expected_node = expected_arg != nullptr &&
			                           std::ranges::find(m_key_phis, expected_arg) != m_key_phis.end();
			if (read != nullptr && read->GetOpcode() == ValueOpcode::Phi && !free_position &&
			    (expected == nullptr || expected_node)) {
				const auto before = walk.indices.size();
				if (!PlanPhiTableNode(walk, *read, expected_node ? expected_arg : nullptr,
				                      other_users, keys[i])) {
					return false;
				}
				if (expected == nullptr && walk.indices.size() == before) {
					// A subtree of free branches only.
					keys[i] = Value(0u);
					free_args.push_back(i);
				}
				continue;
			}
			uint32_t memory_index = 0;
			Value    key;
			switch (read != nullptr && read->GetOpcode() == ValueOpcode::Phi
			            ? PhiLeaf::Free
			            : ClassifyPhiLeaf(walk, read, memory_index, key)) {
			case PhiLeaf::Reject: return RejectIndirect(walk.handle, __LINE__);
			case PhiLeaf::Free:
				walk.free_leaves++;
				keys[i] = Value(0u);
				free_args.push_back(i);
				continue;
			case PhiLeaf::Read: break;
			}
			if (expected != nullptr && !free_position &&
			    (expected_node || !EquivalentValue(m_program, expected->Arg(i), key))) {
				return RejectIndirect(walk.handle, __LINE__);
			}
			if (other_users) {
				walk.live_memory.push_back(memory_index);
			}
			keys[i] = key;
			walk.indices.push_back(memory_index);
			walk.reads.push_back(read);
		}
		walk.ancestors.pop_back();
		if (expected != nullptr) {
			node_key = Value(const_cast<Inst*>(expected));
			return true;
		}
		if (walk.indices.size() == first_index) {
			// No branch below reads the table; the caller marks this one free.
			node_key = Value(0u);
			return true;
		}
		auto* block  = phi.Parent();
		auto  insert = std::find_if(block->begin(), block->end(),
		                            [&](const Inst& candidate) { return &candidate == &phi; });
		if (insert == block->end()) {
			return RejectIndirect(walk.handle, __LINE__);
		}
		auto& key_phi = *block->PrependNewInst(insert, ValueOpcode::Phi);
		key_phi.SetFlags(Type::U32);
		for (size_t i = 0; i < count; i++) {
			key_phi.AddPhiOperand(phi.PhiBlock(i), keys[i]);
		}
		m_key_phis.push_back(&key_phi);
		for (const auto i: free_args) {
			m_free_phi_keys.emplace_back(&key_phi, i);
		}
		node_key = Value(&key_phi);
		return true;
	}

	bool TryMakeIndirectImage(Inst& handle, IndirectImagePlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetImageResource || handle.NumArgs() != 8u) {
			return false;
		}
		Inst*    table_handle = nullptr;
		Value    key;
		uint32_t table_offset = 0;
		std::vector<uint32_t> live_memory;
		uint32_t key_mask     = UINT32_MAX;
		for (uint32_t dword = 0; dword < plan.reads.size(); ++dword) {
			// A descriptor can be carried unchanged through nested loops. Resolve only
			// Phi webs whose incoming values agree, leaving genuinely selected descriptors
			// unsupported here. Keep the original reads alive when other GPU users need them.
			// Research: a genuinely selected descriptor (a phi of heap reads under different
			// keys) goes to PlanPhiTableRead instead.
			auto resolved = ResolveInvariantPhi(m_program, handle.Arg(dword));
			if (resolved.IsEmpty()) {
				resolved = handle.Arg(dword).Resolve();
			}
			auto* read = resolved.TryInstruction();
			if (read == nullptr) {
				return RejectIndirect(handle, __LINE__);
			}
			if (read->GetOpcode() == ValueOpcode::Phi) {
				if (!PlanPhiTableRead(handle, *read, dword, table_handle, key, table_offset, plan,
				                      live_memory)) {
					return false;
				}
				continue;
			}
			uint32_t memory_index = 0;
			const auto* memory = ScalarReadMemory(*read, memory_index);
			if (memory == nullptr || memory->offset > INT32_MAX || (memory->offset & 3u) != 0u ||
			    !MemoryIndexBelongsTo(memory_index, *read)) {
				return RejectIndirect(handle, __LINE__);
			}
			auto*    current_handle = read->Arg(0).Resolve().TryInstruction();
			Value    current_key;
			uint32_t offset       = 0;
			uint32_t current_mask = UINT32_MAX;
			if (current_handle == nullptr ||
			    current_handle->GetOpcode() != (memory->kind == ResourceKind::ScalarAddress
			                                        ? ValueOpcode::GetAddressResource
			                                        : ValueOpcode::GetBufferResource) ||
			    (memory->kind == ResourceKind::ScalarAddress &&
			     read->Parent() != handle.Parent()) ||
			    (table_handle != nullptr &&
			     !EquivalentValue(m_program, Value(table_handle), Value(current_handle))) ||
			    !MatchTableOffset(read->Arg(1), current_key, offset, 5u, &current_mask) ||
			    memory->offset > UINT32_MAX - offset) {
				return RejectIndirect(handle, __LINE__);
			}
			offset += memory->offset;
			if (dword == 0u) {
				key          = current_key;
				table_offset = offset;
				key_mask     = current_mask;
			} else if (!(EquivalentValue(m_program, key, current_key) ||
			             KeyPhiUses(key, current_key)) ||
			           current_mask != key_mask ||
			           static_cast<uint64_t>(table_offset) + dword * sizeof(uint32_t) != offset) {
				return RejectIndirect(handle, __LINE__);
			}
			table_handle = current_handle;
			// Samples of the same texture share the (deduplicated) descriptor reads; each image
			// handle gets its own plan over them. Any other user needs the descriptor's value.
			const bool image_users_only =
			    !read->Uses().empty() && std::ranges::all_of(read->Uses(), [](const Use& use) {
				    return use.user->GetOpcode() == ValueOpcode::GetImageResource ||
				           FeedsOnlyDeadPhis(*use.user);
			    });
			// Descriptor fields may also be read by shader arithmetic (for example texture
			// dimensions). Keep those loads as real GPU buffer reads while lowering the
			// image handle to a heap key; only exclusive descriptor reads can be erased.
			if (!image_users_only) {
				live_memory.push_back(memory_index);
			}
			plan.memory[dword] = memory_index;
			plan.reads[dword] = read;
		}

		DescriptorSource table_source;
		// A phi-planned dword stands for several heap reads of the same table; any of them names it.
		const Inst* table_read = plan.reads[0];
		if (table_read != nullptr && table_read->GetOpcode() == ValueOpcode::Phi &&
		    !plan.extra_reads.empty()) {
			table_read = plan.extra_reads.front();
		}
		if (table_read == nullptr || !MakeRuntimeTableSource(*table_read, table_source)) {
			return RejectIndirect(handle, __LINE__);
		}
		DescriptorSource                material_source;
		DescriptorSource::IndirectImage indirect;
		indirect.table_offset = table_offset;
		// Enumerate the keys the table can reach (upstream's plan), or, when bindless images are
		// enabled and the heap is a buffer, look the key up at run time: an enumeration that
		// passes here can still fail per draw (a material table of more records than the probe
		// cap, or more textures than a shader binds), and a compiled shader cannot switch plans.
		const auto enumerable = [&]() -> bool {
			// The host enumerates unmasked keys; a masked key is looked up only at run time.
			if ((m_program.bindless_images && table_source.dword_count == 4u) ||
			    key_mask != UINT32_MAX) {
				return false;
			}
			if (table_source.dword_count == 2u) {
				const auto* selector = key.Resolve().TryInstruction();
				const bool  bitscan  = selector != nullptr &&
				                       selector->GetOpcode() == ValueOpcode::FindILsb32 &&
				                       selector->NumArgs() == 1u && !m_shader_writes &&
				                       NonzeroOnEntry(selector->Arg(0), handle.Parent());
				if (bitscan) {
					indirect.key_count = Value(32u);
				} else {
					indirect.key_count = BoundedLoopCount(key, handle.Parent());
				}
				if (indirect.key_count.IsEmpty() &&
				    !MatchUniformizedMaterialKey(key, handle, indirect, material_source)) {
					return RejectIndirect(handle, __LINE__);
				}
				if ((table_offset & 3u) != 0u ||
				    (bitscan && table_offset > UINT32_MAX - (32u * 32u - 1u)))
					return RejectIndirect(handle, __LINE__);
			} else {
				auto* material_read = key.Resolve().TryInstruction();
				// A key packed in the material word. Peeling from the outside keeps
				// key = (inner >> key_shift) & key_mask exact:
				//   inner = y >> n            ->  shift += n
				//   inner = y & c             ->  mask  &= c >> shift
				//   inner = bfe(y, off, cnt)  ->  mask  &= ((1 << cnt) - 1) >> shift, shift += off
				for (uint32_t level = 0; level < 3u && material_read != nullptr; level++) {
					const auto op        = material_read->GetOpcode();
					uint32_t   immediate = 0;
					uint32_t   count     = 0;
					if (op == ValueOpcode::ShiftRightLogical32 && material_read->NumArgs() == 2u &&
					    ImmediateU32(material_read->Arg(1), immediate) &&
					    indirect.key_shift + immediate < 32u) {
						indirect.key_shift += immediate;
						material_read = material_read->Arg(0).Resolve().TryInstruction();
					} else if (op == ValueOpcode::BitwiseAnd32 && material_read->NumArgs() == 2u &&
					           (ImmediateU32(material_read->Arg(1), immediate) ||
					            ImmediateU32(material_read->Arg(0), immediate))) {
						const auto operand =
						    ImmediateU32(material_read->Arg(1), immediate) ? 0u : 1u;
						indirect.key_mask &= immediate >> indirect.key_shift;
						material_read = material_read->Arg(operand).Resolve().TryInstruction();
					} else if (op == ValueOpcode::BitFieldUExtract &&
					           material_read->NumArgs() == 3u &&
					           ImmediateU32(material_read->Arg(1), immediate) &&
					           ImmediateU32(material_read->Arg(2), count) && count > 0u &&
					           count < 32u && indirect.key_shift + immediate < 32u) {
						indirect.key_mask &= ((1u << count) - 1u) >> indirect.key_shift;
						indirect.key_shift += immediate;
						material_read = material_read->Arg(0).Resolve().TryInstruction();
					} else {
						break;
					}
				}
				uint32_t    material_memory_index = 0;
				const auto* memory = material_read != nullptr
				                         ? ScalarReadMemory(*material_read, material_memory_index) : nullptr;
				if (table_offset != 0u || memory == nullptr || memory->kind != ResourceKind::ScalarBuffer ||
				    memory->offset > INT32_MAX ||
				    !MemoryIndexBelongsTo(material_memory_index, *material_read)) {
					return RejectIndirect(handle, __LINE__);
				}
				Value selector;
				if (!MatchMaterialOffset(material_read->Arg(1), selector, indirect.selector_stride,
				                         indirect.selector_offset)) {
					return RejectIndirect(handle, __LINE__);
				}
				const auto step = std::gcd<uint64_t>(indirect.selector_stride, uint64_t {1} << 32u);
				indirect.selector_offset =
				    (static_cast<uint32_t>(indirect.selector_offset % step) & ~3u) + (memory->offset & ~3u);
				// The key read stays in the shader (only the table reads become planning-only), so
				// it may have other users, such as a check that a texture is assigned at all.
				const auto* shift = plan.reads[0]->Arg(1).Resolve().TryInstruction();
				if (!UsesOnly(*shift, plan.reads)) {
					return RejectIndirect(handle, __LINE__);
				}
				const auto* material_handle = material_read->Arg(0).Resolve().TryInstruction();
				if (material_handle == nullptr ||
				    material_handle->GetOpcode() != ValueOpcode::GetBufferResource ||
				    !MakeRuntimeTableSource(*material_read, material_source)) {
					return RejectIndirect(handle, __LINE__);
				}
				indirect.material_source = InternSource(material_source);
			}
			return true;
		};
		if (!enumerable()) {
			if (!m_program.bindless_images || table_source.dword_count != 4u) {
				return false;
			}
			indirect              = {};
			indirect.table_offset = table_offset;
			indirect.bindless     = true;
			material_source       = {};
		}
		if (!indirect.bindless && !live_memory.empty()) {
			return RejectIndirect(handle, __LINE__);
		}
		m_live_table_memory.insert(m_live_table_memory.end(), live_memory.begin(),
		                           live_memory.end());
		indirect.table_source = InternSource(table_source);
		DescriptorSource image_source;
		image_source.dword_count = 8u;
		image_source.dwords.fill(Value(0u));
		std::copy_n(material_source.dwords.begin(), material_source.dword_count,
		            image_source.dwords.begin());
		std::copy_n(table_source.dwords.begin(), table_source.dword_count,
		            image_source.dwords.begin() + 4u);
		image_source.indirect_image = indirect;
		if (key_mask != UINT32_MAX) {
			// The bindless lookup takes the key itself, so the mask the shader applies to the
			// scaled key becomes an instruction on the key, just before the handle.
			auto*      block = handle.Parent();
			const auto where = std::ranges::find_if(
			    block->Instructions(), [&](const Inst& inst) { return &inst == &handle; });
			key = Value(&*block->PrependNewInst(where, ValueOpcode::BitwiseAnd32,
			                                    {key, Value(key_mask)}));
		}
		plan.handle                 = &handle;
		plan.source                 = InternSource(image_source);
		plan.key                    = key;
		plan.roots                  = image_source.dwords;
		return true;
	}

	const IndirectImagePlan* FindIndirectImage(const Inst& handle) const {
		const auto found =
		    std::find_if(m_indirect_images.begin(), m_indirect_images.end(),
		                 [&](const IndirectImagePlan& plan) { return plan.handle == &handle; });
		return found == m_indirect_images.end() ? nullptr : &*found;
	}

	bool IsLiveTableMemory(uint32_t index) const {
		return std::ranges::find(m_live_table_memory, index) != m_live_table_memory.end();
	}

	bool IsIndirectPlanningMemory(uint32_t index) const {
		if (IsLiveTableMemory(index)) {
			return false;
		}
		return std::any_of(m_indirect_images.begin(), m_indirect_images.end(),
		                   [&](const IndirectImagePlan& plan) {
			return std::ranges::find(plan.memory, index) != plan.memory.end() ||
			       std::ranges::find(plan.extra_memory, index) != plan.extra_memory.end();
		});
	}

	const BindlessSamplerPlan* FindBindlessSampler(const Inst& handle) const {
		const auto found =
		    std::ranges::find(m_bindless_samplers, &handle, &BindlessSamplerPlan::handle);
		return found == m_bindless_samplers.end() ? nullptr : &*found;
	}

	// A bindless sampler: all four S# dwords are scalar reads of one sampler heap (a buffer of
	// 16-byte records) at (key << 4) + record offset, with a GPU-computed key and a heap V# the
	// host can evaluate. The shader keeps the key and indexes the bindless sampler array, which
	// the host mirrors from the heap.
	bool TryMakeBindlessSampler(Inst& handle, BindlessSamplerPlan& plan) {
		Inst*    table_handle = nullptr;
		Value    key;
		uint32_t table_offset = 0;
		for (uint32_t dword = 0; dword < 4u; ++dword) {
			auto* read = ResolveInvariantPhi(m_program, handle.Arg(dword)).TryInstruction();
			if (read == nullptr) {
				return false;
			}
			uint32_t    memory_index = 0;
			const auto* memory       = ScalarReadMemory(*read, memory_index);
			if (memory == nullptr || memory->kind != ResourceKind::ScalarBuffer ||
			    memory->offset > INT32_MAX || (memory->offset & 3u) != 0u ||
			    !MemoryIndexBelongsTo(memory_index, *read)) {
				return false;
			}
			auto*    current_handle = read->Arg(0).Resolve().TryInstruction();
			Value    current_key;
			uint32_t offset = 0;
			if (current_handle == nullptr ||
			    current_handle->GetOpcode() != ValueOpcode::GetBufferResource ||
			    (table_handle != nullptr &&
			     !EquivalentValue(m_program, Value(table_handle), Value(current_handle))) ||
			    !MatchTableOffset(read->Arg(1), current_key, offset, 4u) ||
			    memory->offset > UINT32_MAX - offset) {
				return false;
			}
			offset += memory->offset;
			if (dword == 0u) {
				key          = current_key;
				table_offset = offset;
			} else if (!EquivalentValue(m_program, key, current_key) ||
			           static_cast<uint64_t>(table_offset) + dword * sizeof(uint32_t) != offset) {
				return false;
			}
			table_handle          = current_handle;
			plan.reads[dword]     = read;
			plan.memory[dword]    = memory_index;
			plan.exclusive[dword] =
			    !read->Uses().empty() && std::ranges::all_of(read->Uses(), [](const Use& use) {
				    return use.user->GetOpcode() == ValueOpcode::GetSamplerResource ||
				           FeedsOnlyDeadPhis(*use.user);
			    });
		}
		DescriptorSource table_source;
		if (!MakeRuntimeTableSource(*plan.reads[0], table_source) ||
		    table_source.dword_count != 4u) {
			return false;
		}
		DescriptorSource source;
		source.dword_count = 4u;
		source.dwords.fill(Value(0u));
		for (uint32_t dword = 0; dword < 3u; dword++) {
			source.dwords[dword]    = table_source.dwords[dword];
			plan.table_roots[dword] = table_source.dwords[dword];
		}
		source.bindless_sampler = DescriptorSource::BindlessSampler {.table_offset = table_offset};
		plan.handle             = &handle;
		plan.key                = key;
		plan.source             = InternSource(source);
		return true;
	}

	void PlanBindlessSamplers() {
		// Opt-in while a regression is investigated: with the game's own samplers the title's
		// fog shows a bright arch (DEBUGGING.md, 2026-09-28). KYTY_BINDLESS_SAMPLERS=1 enables them.
		static const bool enabled = std::getenv("KYTY_BINDLESS_SAMPLERS") != nullptr &&
		                            std::getenv("KYTY_BINDLESS_SAMPLERS")[0] == '1';
		if (!m_program.bindless_images || !enabled) {
			return;
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None ||
				    inst.NumArgs() < 2u) {
					continue;
				}
				auto* sampler = inst.Arg(1).Resolve().TryInstruction();
				if (sampler == nullptr || sampler->GetOpcode() != ValueOpcode::GetSamplerResource ||
				    sampler->NumArgs() != 4u || FindBindlessSampler(*sampler) != nullptr) {
					continue;
				}
				const auto       flags  = inst.Flags<MemoryFlags>();
				const auto&      memory = m_program.memory_info[flags.index];
				DescriptorSource descriptor;
				MakeSource(*sampler, 4u, true,
				           (memory.image_sample_flags & Decoder::ImageSampleFlagAdjust) != 0,
				           memory.sampler * 4u, descriptor, flags.pc);
				uint32_t bad_dword = 0;
				if (ValidateSource(descriptor, bad_dword)) {
					continue;
				}
				BindlessSamplerPlan plan;
				if (TryMakeBindlessSampler(*sampler, plan)) {
					m_bindless_samplers.push_back(plan);
				}
			}
		}
	}

	// Research stopgap: a bindless sampler (selected per material from a sampler heap, like
	// the image beside it) has no indirect plan. Before the walk, give it one fixed sampler
	// (trilinear, wrap, full LOD range) and make the heap reads it strands planning-only, as
	// an indirect image's table reads are: registered as buffers, they would bind a
	// descriptor that dead-code elimination then deletes. Only the filtering and edge
	// addressing can differ from the material's own sampler.
	void PlanDefaultSamplers() {
		if (!Frontend::TranslationNonFatal()) {
			return;
		}
		constexpr std::array<uint32_t, 4> DefaultSampler {
		    0x00000000u,                             // wrap on every axis
		    0xfffu << 12u,                           // max_lod 255.9
		    (1u << 20u) | (1u << 22u) | (2u << 26u), // bilinear mag/min, linear mip
		    0x00000000u};
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None ||
				    inst.NumArgs() < 2u) {
					continue;
				}
				auto* sampler = inst.Arg(1).Resolve().TryInstruction();
				if (sampler == nullptr || sampler->GetOpcode() != ValueOpcode::GetSamplerResource ||
				    sampler->NumArgs() != 4u || FindBindlessSampler(*sampler) != nullptr) {
					continue;
				}
				DescriptorSource descriptor;
				descriptor.dword_count = 4u;
				for (uint32_t dword = 0; dword < 4u; dword++) {
					descriptor.dwords[dword] = LowerDescriptorPhi(sampler->Arg(dword));
				}
				uint32_t bad_dword = 0;
				if (ValidateSource(descriptor, bad_dword)) {
					continue;
				}
				// The scalar reads behind the descriptor, through phis and selects.
				std::vector<const Inst*> reads;
				std::vector<const Inst*> pending;
				std::vector<const Inst*> visited;
				for (uint32_t dword = 0; dword < 4u; dword++) {
					if (const auto* arg = sampler->Arg(dword).Resolve().TryInstruction()) {
						pending.push_back(arg);
					}
				}
				while (!pending.empty()) {
					const auto* current = pending.back();
					pending.pop_back();
					if (std::ranges::find(visited, current) != visited.end()) {
						continue;
					}
					visited.push_back(current);
					uint32_t index = 0;
					if (ScalarReadMemory(*current, index) != nullptr) {
						reads.push_back(current);
						continue;
					}
					if (current->GetOpcode() == ValueOpcode::Phi ||
					    current->GetOpcode() == ValueOpcode::SelectU32) {
						for (size_t arg = 0; arg < current->NumArgs(); arg++) {
							if (const auto* next = current->Arg(arg).Resolve().TryInstruction()) {
								pending.push_back(next);
							}
						}
					}
				}
				// A read whose value the shader also uses as data (a register reused after a
				// branch) stays an ordinary load; only the reads the fixed sampler strands are
				// planning-only. Marking a data read too left its phis without a value.
				size_t stranded = 0;
				for (const auto* read: reads) {
					if (!FeedsOnlySamplers(*read)) {
						continue;
					}
					uint32_t index = 0;
					(void)ScalarReadMemory(*read, index);
					m_program.memory_info[index].planning_only = true;
					m_default_sampler_reads.push_back(read);
					stranded++;
				}
				for (uint32_t dword = 0; dword < 4u; dword++) {
					sampler->SetArg(dword, Value(DefaultSampler[dword]));
				}
				LOGF("shader resource tracking: hash=0x%016" PRIx64 " pc=0x%08x bindless sampler: "
				     "using a default sampler (%zu heap reads planning-only)\n",
				     m_program.shader_hash, inst.Flags<MemoryFlags>().pc, stranded);
			}
		}
	}

	void PlanIndirectImages() {
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None ||
				    inst.NumArgs() == 0u) {
					continue;
				}
				auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || FindIndirectImage(*handle) != nullptr) {
					continue;
				}
				IndirectImagePlan plan;
				if (TryMakeIndirectImage(*handle, plan)) {
					m_indirect_images.push_back(std::move(plan));
				}
			}
		}
	}

	bool GetHandle(Value value, ValueOpcode expected, uint32_t width, uint32_t pc,
	               uint32_t base_reg, Inst*& handle, uint32_t& source, bool sampler = false,
	               bool sample_adjust = false) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != expected) {
			Fail(pc, fmt::format("memory operation requires {}", ValueOpcodeName(expected)));
			return false;
		}
		DescriptorSource descriptor;
		MakeSource(*handle, width, sampler, sample_adjust, base_reg, descriptor, pc);
		if (m_failed) {
			return false;
		}
		uint32_t bad_dword = 0;
		m_gpu_records_handle = false;
		if (!ValidateSource(descriptor, bad_dword)) {
			m_last_bad_dword = bad_dword;
			// Research: a buffer whose record count only the shader computes (from data it
			// loads itself) binds from its host-known base to the end of its mapping; every
			// access also applies the hardware range check with the shader's own V# words
			// (MemoryInfo::gpu_records), so out-of-range accesses drop as they do there.
			if (expected == ValueOpcode::GetBufferResource && width == 4u && bad_dword == 2u &&
			    m_program.bindless_images) {
				auto relaxed         = descriptor;
				relaxed.dwords[2]    = Value(0xffffffffu);
				uint32_t relaxed_bad = 0;
				if (ValidateSource(relaxed, relaxed_bad)) {
					m_gpu_records_handle = true;
					source               = InternSource(relaxed);
					return true;
				}
			}
			if (expected == ValueOpcode::GetBufferResource &&
			    std::all_of(descriptor.dwords.begin(), descriptor.dwords.begin() + width,
			                [](Value word) { return word.Resolve().GetType() == Type::U32; })) {
				return false;
			}
			// Research stopgap: a bindless sampler (selected per material from a sampler heap,
			// like the image beside it) has no indirect plan. Sample with one fixed sampler
			// (trilinear, wrap, full LOD range) rather than giving up the whole shader; only the
			// filtering and edge addressing can differ from the material's own sampler.
			if (expected == ValueOpcode::GetSamplerResource && width == 4u &&
			    Frontend::TranslationNonFatal()) {
				constexpr std::array<uint32_t, 4> DefaultSampler {
				    0x00000000u,                             // wrap on every axis
				    0xfffu << 12u,                           // max_lod 255.9
				    (1u << 20u) | (1u << 22u) | (2u << 26u), // bilinear mag/min, linear mip
				    0x00000000u};
				for (uint32_t dword = 0; dword < 4u; dword++) {
					handle->SetArg(dword, Value(DefaultSampler[dword]));
					descriptor.dwords[dword] = Value(DefaultSampler[dword]);
				}
				LOGF("shader resource tracking: hash=0x%016" PRIx64 " pc=0x%08x bindless sampler: "
				     "using a default sampler\n",
				     m_program.shader_hash, pc);
				source = InternSource(descriptor);
				return true;
			}
			const auto reject = m_indirect_rejects.find(handle);
			Fail(pc, fmt::format("{} dword {} is not a valid runtime value{}",
			                     ValueOpcodeName(expected), bad_dword,
			                     reject == m_indirect_rejects.end()
			                         ? std::string {}
			                         : fmt::format(" (indirect image rejected at line {})",
			                                       reject->second)));
			return false;
		}
		source = InternSource(descriptor);
		return true;
	}

	void ValidateAddressHandle(Value value, uint32_t pc) const {
		const auto* handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource) {
			Fail(pc, "address operation requires GetAddressResource");
			return;
		}
		if (handle->NumArgs() != 2) {
			Fail(pc, "GetAddressResource must have two address dwords");
		}
	}

	uint32_t AddBuffer(uint32_t source, const MemoryInfo& memory, ValueOpcode op, uint32_t pc) {
		for (uint32_t i = 0; i < m_info.buffers.size(); i++) {
			if (m_info.buffers[i].source == source) {
				Merge(m_info.buffers[i], memory, op, pc);
				return i;
			}
		}
		if (m_info.buffers.size() >= ShaderInfo::MaxBuffers) {
			return UINT32_MAX;
		}
		BufferResource resource;
		resource.source       = source;
		resource.first_use_pc = pc;
		Merge(resource, memory, op, pc);
		m_info.buffers.push_back(resource);
		return static_cast<uint32_t>(m_info.buffers.size() - 1);
	}

	static void Merge(BufferResource& resource, const MemoryInfo& memory, ValueOpcode op,
	                  uint32_t pc) {
		const auto access        = BufferAccessOf(op);
		const bool atomic        = access == BufferAccess::Atomic;
		const bool write         = access == BufferAccess::Write || atomic;
		resource.first_use_pc    = std::min(resource.first_use_pc, pc);
		resource.max_byte_extent = std::max(resource.max_byte_extent, ByteExtent(memory));
		resource.read            = resource.read || !write || atomic;
		resource.written         = resource.written || write;
		resource.atomic          = resource.atomic || atomic;
		resource.formatted       = resource.formatted || memory.formatted;
		resource.scalar          = resource.scalar || op == ValueOpcode::ReadConstBuffer ||
		                           memory.kind == ResourceKind::ScalarBuffer;
	}

	uint32_t AddImage(uint32_t source, const MemoryInfo& memory, ValueOpcode op, uint32_t pc) {
		const auto resource_class = ImageOpcodeInfoOf(op).resource_class;
		const auto mip   = resource_class == ImageResourceClass::Storage && memory.image_has_mip
		                       ? ImageMipMode::DynamicStorage
		                       : ImageMipMode::None;
		const bool depth = (memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0;
		for (uint32_t i = 0; i < m_info.images.size(); i++) {
			auto& image = m_info.images[i];
			if (image.source == source && image.resource_class == resource_class &&
			    image.dimension == memory.image_dimension && image.mip_mode == mip &&
			    image.depth_compare == depth && image.r128 == memory.image_r128) {
				Merge(image, op, pc);
				return i;
			}
		}
		if (m_info.images.size() >= ShaderInfo::MaxImages) {
			return UINT32_MAX;
		}
		ImageResource image;
		image.source         = source;
		image.first_use_pc   = pc;
		image.resource_class = resource_class;
		image.dimension      = memory.image_dimension;
		image.mip_mode       = mip;
		image.depth_compare  = depth;
		image.r128           = memory.image_r128;
		Merge(image, op, pc);
		m_info.images.push_back(image);
		return static_cast<uint32_t>(m_info.images.size() - 1);
	}

	static void Merge(ImageResource& image, ValueOpcode op, uint32_t pc) {
		const auto access  = ImageOpcodeInfoOf(op).access;
		const bool atomic  = access == ImageAccess::Atomic;
		const bool write   = access == ImageAccess::Write || atomic;
		image.first_use_pc = std::min(image.first_use_pc, pc);
		image.read         = image.read || !write || atomic;
		image.written      = image.written || write;
		image.atomic       = image.atomic || atomic;
	}

	uint32_t AddSampler(uint32_t source, uint32_t pc) {
		for (uint32_t i = 0; i < m_info.samplers.size(); i++) {
			if (m_info.samplers[i].source == source) {
				m_info.samplers[i].first_use_pc = std::min(m_info.samplers[i].first_use_pc, pc);
				return i;
			}
		}
		if (m_info.samplers.size() >= ShaderInfo::MaxSamplers) {
			return UINT32_MAX;
		}
		SamplerResource sampler {.source = source, .first_use_pc = pc};
		const auto*     descriptor = Source(source);
		sampler.bindless = descriptor != nullptr && descriptor->bindless_sampler.has_value();
		m_info.samplers.push_back(sampler);
		return static_cast<uint32_t>(m_info.samplers.size() - 1);
	}

	void AddSampledPair(uint32_t image, uint32_t sampler, uint32_t pc) {
		for (auto& pair: m_info.sampled_pairs) {
			if (pair.image == image && pair.sampler == sampler) {
				pair.first_use_pc = std::min(pair.first_use_pc, pc);
				return;
			}
		}
		if (m_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
			Fail(pc, "sampled image/sampler pair limit exceeded");
			return;
		}
		m_info.sampled_pairs.push_back({image, sampler, pc});
	}

	void AddHandlePatch(Inst* handle, uint32_t resource, uint32_t pc) {
		for (const auto& patch: m_handle_patches) {
			if (patch.handle == handle) {
				if (patch.resource != resource) {
					Fail(pc, fmt::format("{} is reused with incompatible resource classes",
					                     ValueOpcodeName(handle->GetOpcode())));
				}
				return;
			}
		}
		m_handle_patches.push_back({handle, resource});
	}

	void AddMemoryPatch(uint32_t index, uint32_t resource, uint32_t sampler, bool has_sampler,
	                    uint32_t pc) {
		for (auto& patch: m_memory_patches) {
			if (patch.index != index) {
				continue;
			}
			if (patch.resource != resource ||
			    (has_sampler && patch.has_sampler && patch.sampler != sampler)) {
				Fail(pc, "memory metadata is reused with incompatible resources");
			}
			if (has_sampler) {
				patch.sampler     = sampler;
				patch.has_sampler = true;
			}
			return;
		}
		m_memory_patches.push_back({index, resource, sampler, has_sampler});
	}

	void Collect(Inst& inst) {
		const auto op = inst.GetOpcode();
		if (op == ValueOpcode::ShaderTrap) {
			m_info.uses_dma = true;
			return;
		}
		const auto buffer       = BufferAccessOf(op);
		const auto address_info = AddressOpcodeInfoOf(op);
		const auto image_info   = ImageOpcodeInfoOf(op);
		if (buffer == BufferAccess::None && address_info.access == AddressAccess::None &&
		    image_info.access == ImageAccess::None) {
			return;
		}
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			Fail(flags.pc, fmt::format("memory metadata index {} is out of range", flags.index));
			return;
		}
		if (inst.NumArgs() == 0) {
			Fail(flags.pc, "memory operation has no resource handle");
			return;
		}
		const auto& memory = m_program.memory_info[flags.index];
		if (memory.planning_only || IsIndirectPlanningMemory(flags.index)) {
			return;
		}
		Inst*    handle   = nullptr;
		uint32_t source   = 0;
		uint32_t resource = 0;

		if (buffer != BufferAccess::None) {
			if (!GetHandle(inst.Arg(0), ValueOpcode::GetBufferResource, 4, flags.pc,
			               memory.resource * 4u, handle, source)) {
				if (m_failed) {
					return;
				}
				const bool scalar_read =
				    memory.kind == ResourceKind::ScalarBuffer && op == ValueOpcode::ReadConstBuffer;
				const bool vector_read =
				    memory.kind == ResourceKind::Buffer &&
				    (op == ValueOpcode::LoadBufferU32 || op == ValueOpcode::LoadBufferU32x2 ||
				     op == ValueOpcode::LoadBufferU32x3 || op == ValueOpcode::LoadBufferU32x4);
				if ((!scalar_read && !vector_read) ||
				    (scalar_read && (memory.formatted || memory.typed))) {
					Fail(flags.pc,
					     fmt::format("buffer descriptor dword {} is not a valid runtime value; "
					                 "GPU-selected access requires a scalar or vector DWORD load",
					                 m_last_bad_dword));
					return;
				}
				m_program.memory_info[flags.index].kind = ResourceKind::IndirectBuffer;
				m_info.uses_dma                         = true;
				return;
			}
			resource = AddBuffer(source, memory, op, flags.pc);
			if (resource == UINT32_MAX) {
				Fail(flags.pc, "buffer resource limit exceeded");
				return;
			}
			if (m_gpu_records_handle) {
				m_program.memory_info[flags.index].gpu_records = true;
				m_info.buffers[resource].gpu_records            = true;
			}
			AddHandlePatch(handle, resource, flags.pc);
			AddMemoryPatch(flags.index, resource, 0, false, flags.pc);
			return;
		}
		if (address_info.access != AddressAccess::None) {
			if (!IsAddressResourceKind(memory.kind)) {
				Fail(flags.pc, "address operation has invalid resource kind");
				return;
			}
			if (memory.kind == ResourceKind::Scratch) {
				handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetScratchResource ||
				    handle->NumArgs() != 0) {
					Fail(flags.pc, "scratch operation requires GetScratchResource");
					return;
				}
				if (m_program.scratch_dwords == 0) {
					Fail(flags.pc, "scratch operation requires a nonzero AGC per-thread size");
				}
				return;
			}
			ValidateAddressHandle(inst.Arg(0), flags.pc);
			if (address_info.access == AddressAccess::Write) {
				m_program.has_address_writes = true;
			}
			m_info.uses_dma = true;
			return;
		}

		if (memory.kind != ResourceKind::Image ||
		    image_info.resource_class == ImageResourceClass::None) {
			Fail(flags.pc, "image operation has invalid resource kind");
			return;
		}
		handle               = inst.Arg(0).Resolve().TryInstruction();
		const auto* indirect = handle != nullptr ? FindIndirectImage(*handle) : nullptr;
		if (indirect != nullptr) {
			source = indirect->source;
		} else {
			GetHandle(inst.Arg(0), ValueOpcode::GetImageResource, 8, flags.pc,
			          memory.resource * 4u, handle, source);
		}
		if (m_failed) {
			return;
		}
		resource = AddImage(source, memory, op, flags.pc);
		if (resource == UINT32_MAX) {
			Fail(flags.pc, "image resource limit exceeded");
			return;
		}
		AddHandlePatch(handle, resource, flags.pc);
		uint32_t sampler = 0;
		if (image_info.needs_sampler) {
			if (inst.NumArgs() < 2) {
				Fail(flags.pc, "sampled image operation has no sampler handle");
				return;
			}
			Inst*      sampler_handle = nullptr;
			uint32_t   sampler_source = 0;
			const bool sample_adjust =
			    (memory.image_sample_flags & Decoder::ImageSampleFlagAdjust) != 0;
			auto*       candidate = inst.Arg(1).Resolve().TryInstruction();
			const auto* bindless  = candidate != nullptr ? FindBindlessSampler(*candidate) : nullptr;
			if (bindless != nullptr) {
				sampler_handle = candidate;
				sampler_source = bindless->source;
			} else {
				GetHandle(inst.Arg(1), ValueOpcode::GetSamplerResource, 4, flags.pc,
				          memory.sampler * 4u, sampler_handle, sampler_source, true, sample_adjust);
				if (m_failed) {
					return;
				}
			}
			sampler = AddSampler(sampler_source, flags.pc);
			if (sampler == UINT32_MAX) {
				Fail(flags.pc, "sampler resource limit exceeded");
				return;
			}
			AddHandlePatch(sampler_handle, sampler, flags.pc);
			AddSampledPair(resource, sampler, flags.pc);
		}
		AddMemoryPatch(flags.index, resource, sampler, image_info.needs_sampler, flags.pc);
	}

	const DescriptorSource* Source(uint32_t source) const {
		return source < m_sources.size() ? &m_sources[source] : nullptr;
	}

	void LinkImageAliases() {
		for (auto& buffer: m_info.buffers) {
			const auto* buffer_source = Source(buffer.source);
			if (buffer_source == nullptr || buffer_source->dword_count != 4) {
				continue;
			}
			for (uint32_t image = 0; image < m_info.images.size(); image++) {
				const auto* image_source = Source(m_info.images[image].source);
				if (image_source == nullptr || image_source->dword_count != 8 ||
				    image_source->indirect_image.has_value()) {
					continue;
				}
				bool alias = true;
				for (uint32_t dword = 0; dword < 4; dword++) {
					alias = alias && EquivalentValue(m_program, buffer_source->dwords[dword],
					                                 image_source->dwords[dword]);
				}
				if (alias) {
					buffer.image_alias = image;
					break;
				}
			}
		}
	}

	Program&                                   m_program;
	const Decoder::Program&                    m_decoded;
	const CFG::Graph&                          m_native_cfg;
	std::vector<Program::ScalarWrite>          m_scalar_writes;
	std::vector<ResolvedHandle>                m_resolved_handles;
	std::unordered_set<const Inst*>            m_srt_visited;
	std::vector<Inst*>                         m_scalar_reads;
	ShaderInfo                                 m_info;
	std::vector<DescriptorSource>              m_sources;
	std::vector<HandlePatch>                   m_handle_patches;
	std::vector<MemoryPatch>                   m_memory_patches;
	std::vector<IndirectImagePlan>             m_indirect_images;
	// Bindless table reads the shader also uses as values: they stay real buffer reads.
	std::vector<uint32_t>                      m_live_table_memory;
	std::vector<BindlessSamplerPlan>           m_bindless_samplers;
	std::map<const Inst*, int>                 m_indirect_rejects;
	// Key phis built by PlanPhiTableRead, and their free (key 0) operands.
	std::vector<const Inst*>                   m_key_phis;
	std::vector<std::pair<const Inst*, size_t>> m_free_phi_keys;
	uint32_t                                   m_last_bad_dword = 0;
	bool                                       m_gpu_records_handle = false;
	std::vector<const Inst*>                   m_default_sampler_reads;
	std::vector<std::pair<const Inst*, Value>> m_descriptor_selections;
	bool                                       m_shader_writes = false;

	// MemoryIndexBelongsTo's count of memory accesses per memory_info index, built on first use.
	mutable std::unordered_map<uint32_t, uint32_t> m_memory_users;
	mutable bool                                   m_memory_users_counted = false;
};

} // namespace

bool TrackResources(Program& program, const Decoder::Program& decoded, const CFG::Graph& native_cfg) {
	Tracker tracker(program, decoded, native_cfg);
	tracker.Run();
	return !tracker.Failed();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
