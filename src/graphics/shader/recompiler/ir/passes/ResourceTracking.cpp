#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <fmt/format.h>
#include <map>
#include <optional>
#include <span>
#include <tuple>
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

bool IsNoncanonicalFlatAddress(Value high, Value active) {
	high = high.Resolve();
	active = active.Resolve();
	// A predicated VGPR write supplies its new value on exactly the lanes which
	// execute this access. Do not discard a selection under a different mask.
	while (const auto* select = high.TryInstruction()) {
		if (select->GetOpcode() != ValueOpcode::SelectU32 || select->Arg(0).Resolve() != active) break;
		high = select->Arg(1).Resolve();
	}
	uint32_t lower = 0, upper = UINT32_MAX;
	if (high.IsImmediate() && high.GetType() == Type::U32) {
		lower = upper = high.U32();
	} else if (const auto* inst = high.TryInstruction();
	           inst != nullptr && inst->GetOpcode() == ValueOpcode::UMedTri32) {
		// The median lies between any two constant inputs, regardless of the third.
		uint32_t count = 0;
		for (size_t index = 0; index < inst->NumArgs(); ++index) {
			const auto value = inst->Arg(index).Resolve();
			if (!value.IsImmediate() || value.GetType() != Type::U32) continue;
			if (count++ == 0) lower = upper = value.U32();
			else { lower = std::min(lower, value.U32()); upper = std::max(upper, value.U32()); }
		}
		if (count < 2) return false;
	}
	// Leave one high DWORD on each side for the signed instruction offset/carry.
	return lower > 0x00008000u && upper < 0xffff7fffu;
}

Value CanonicalizeSampleAdjustDword3(Value value) {
	for (;;) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::BitwiseOr32) {
			if ((PossibleU32Bits(value) & ~SamplerDword3ReservedMask) == 0)
				return Value(0u);
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

	bool SelectsRange(const Inst& selected, Value active, const Inst& candidate,
	                  const Inst& carried, const Inst& mask,
	                  std::span<const std::pair<Value, bool>> exit_conditions) {
		// Inactive lanes retain a previously selected index. Initially every lane is
		// active; the bounded assignment establishes the invariant on each backedge.
		m_exclude_bound = false;
		std::vector<uint32_t> range_values;
		const auto in_range = [&] {
			const auto value = Unknown(Type::U32);
			range_values.push_back(value);
			return value;
		};
		const auto contains = [&](const auto& self, uint32_t node) -> uint32_t {
			if (std::ranges::find(range_values, node) != range_values.end()) return 1u;
			const auto entry = m_nodes[node];
			if (entry.variable == UINT32_MAX) return 0u;
			return Select(NodeFor(entry.variable, 0u, 1u), self(self, entry.high),
			              self(self, entry.low));
		};
		const auto state = [&](bool in_body) {
			m_values[1].clear();
			const auto enabled = Unknown(Type::U1);
			for (const auto& phi: *m_induction.Parent()) {
				if (phi.GetOpcode() != ValueOpcode::Phi) continue;
				m_values[1][&phi] = EquivalentValue(m_program, Value(const_cast<Inst*>(&phi)),
				                                      Value(const_cast<Inst*>(&mask)))
				                         ? enabled : Unknown(phi.GetType());
			}
			m_values[1][&carried] = Select(enabled, Unknown(Type::U32), in_range());
			if (in_body) m_values[1][&m_bound] = 1u;
			m_values[1][&candidate] = Select(Evaluate(Value(const_cast<Inst*>(&m_bound)), true),
			                                  in_range(), Unknown(Type::U32));
		};
		for (uint32_t arm = 0; arm < carried.NumArgs(); ++arm) {
			const auto initial = mask.Arg(arm).Resolve();
			if (initial.IsImmediate() && initial.GetType() == Type::U1 && initial.U1()) continue;
			state(true);
			const auto retained = contains(contains, Evaluate(carried.Arg(arm), true));
			if (Select(Evaluate(mask.Arg(arm), true), 1u, retained) != 1u) return false;
		}
		for (uint32_t arm = 0; arm < selected.NumArgs(); ++arm) {
			state(false);
			const auto value = Evaluate(selected.Arg(arm), true);
			m_values[1][&selected] = value;
			auto allowed = Evaluate(exit_conditions[arm].first, true);
			if (!exit_conditions[arm].second) allowed = Select(allowed, 0u, 1u);
			allowed = Select(allowed, Evaluate(active, true), 0u);
			if (Select(allowed, contains(contains, value), 1u) != 1u) return false;
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
		if (left == right) {
			if (inst.GetOpcode() == ValueOpcode::IEqual32) return 1u;
			if (inst.GetOpcode() == ValueOpcode::INotEqual32) return 0u;
		}
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
		if (current && m_exclude_bound && inst == &m_bound) return 0u;
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
	bool m_exclude_bound = true;
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
		}
		FoldBoundedLoopSelectors();
		PlanScalarReads();
		EliminateDeadCode(m_program.blocks);
		PlanIndirectDescriptors();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				Collect(inst);
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
		for (const auto& plan: m_indirect_descriptors) {
			auto key = plan.key;
			if (plan.table_read != nullptr) {
				const auto* anchor = plan.table_indexed ? plan.handle : plan.table_read;
				auto* block = anchor->Parent();
				const auto where = std::ranges::find_if(block->Instructions(),
				    [&](const Inst& inst) { return &inst == anchor; });
				const auto emit = [&](ValueOpcode op, std::initializer_list<Value> args) {
					return Value(&*block->PrependNewInst(where, op, args));
				};
				const auto& read = *plan.table_read;
				const auto& memory = m_program.memory_info[read.Flags<MemoryFlags>().index];
				Value offset;
				Value valid(true);
				if (plan.table_indexed) {
					const auto* table = read.Arg(0).ResolveInstruction();
					const auto stride = emit(ValueOpcode::BitFieldUExtract,
					    {table->Arg(1), Value(16u), Value(14u)});
					const auto format = emit(ValueOpcode::BitFieldUExtract,
					    {table->Arg(3), Value(12u), Value(7u)});
					offset = emit(ValueOpcode::IAdd32,
					    {emit(ValueOpcode::IMul32, {read.Arg(1), stride}), Value(memory.offset)});
					valid = emit(ValueOpcode::LogicalAnd,
					    {emit(ValueOpcode::ULessThan32, {read.Arg(1), table->Arg(2)}),
					     emit(ValueOpcode::INotEqual32, {format, Value(0u)})});
				} else {
					const auto dynamic = emit(ValueOpcode::BitwiseAnd32, {read.Arg(1), Value(~3u)});
					offset = emit(ValueOpcode::IAdd32, {dynamic, Value(memory.offset & ~3u)});
					valid = emit(ValueOpcode::ULessThanEqual32, {dynamic, offset});
				}
				key = emit(ValueOpcode::SelectU32, {valid, offset, Value(UINT32_MAX)});
				if (!plan.table_indexed) {
					// NativeDescriptorSource proved this read reaches the image. Preserve its
					// SSA Phi edges while retaining descriptor DWORDs for other consumers.
					std::map<const Inst*, Value> projected;
					const auto project = [&](auto&& self, Value value) -> Value {
						value = value.Resolve();
						const auto* phi = value.TryInstruction();
						if (phi == plan.table_read) return key;
						if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi) return Value(UINT32_MAX);
						if (const auto found = projected.find(phi); found != projected.end()) return found->second;
						auto* parent = phi->Parent();
						const auto position = std::ranges::find_if(parent->Instructions(),
						    [&](const Inst& inst) { return &inst == phi; });
						auto& selected = *parent->PrependNewInst(position, ValueOpcode::Phi);
						selected.SetFlags(Type::U32);
						projected.emplace(phi, Value(&selected));
						for (uint32_t arm = 0; arm < phi->NumArgs(); ++arm)
							selected.AddPhiOperand(phi->PhiBlock(arm), self(self, phi->Arg(arm)));
						return Value(&selected);
					};
					key = project(project, plan.handle->Arg(0));
				}
				plan.handle->SetArg(0, key);
				for (uint32_t word = 1; word < plan.handle->NumArgs(); ++word)
					plan.handle->SetArg(word, Value(0u));
				continue;
			}
			if (plan.handle->NumArgs() == 4u) {
				plan.handle->SetArg(0, plan.key);
				for (uint32_t word = 1; word < 4u; ++word) plan.handle->SetArg(word, Value(0u));
			} else {
				if (plan.reads[0] == nullptr) plan.handle->SetArg(0, key);
				for (uint32_t dword = 0; dword < 4u; dword++) {
					plan.handle->SetArg(dword + 1u, plan.roots[dword + 4u]);
				}
				for (uint32_t dword = 5u; dword < plan.roots.size(); dword++) {
					plan.handle->SetArg(dword, plan.key);
				}
			}
			if (plan.reads[0] != nullptr) {
				for (uint32_t word = 0; word < plan.handle->NumArgs(); ++word)
					m_program.memory_info[plan.memory[word]].planning_only = true;
			}
		}
		// Project descriptor-only SSA onto its key. Existing Phis preserve dominance;
		// native descriptor resolution excludes their stale incoming values at each use.
		for (const auto& plan: m_indirect_descriptors) {
			if (plan.handle->GetOpcode() != ValueOpcode::GetImageResource || plan.reads[0] == nullptr)
				continue;
			for (const auto* read: plan.reads) {
				const auto first = std::ranges::find_if(m_indirect_descriptors, [&](const auto& candidate) {
					return candidate.reads[0] == read;
				});
				const auto key = first != m_indirect_descriptors.end() ? first->key : Value(0u);
				const auto uses = read->Uses();
				for (const auto& use: uses) use.user->SetArg(use.operand, key);
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

	struct IndirectDescriptorPlan {
		Inst*                      handle = nullptr;
		uint32_t                   source = 0;
		Value                      key;
		std::array<Value, 8>       roots {};
		std::array<uint32_t, 8>    memory {};
		std::array<const Inst*, 8> reads {};
		bool split_offsets = false;
		bool retain_reads = false;
		bool table_indexed = false;
		const Inst* table_read = nullptr;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& reason) const {
		const auto message =
		    fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
		                m_program.shader_hash, StageName(m_program.stage), pc, reason);
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

	Value LowerDescriptorValue(Value value) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return value;
		// The scalar planner owns reads and lowers their address handles separately.
		uint32_t memory_index = 0;
		if (ScalarReadMemory(*inst, memory_index) != nullptr) return value;
		const auto [cached, inserted] = m_descriptor_values.emplace(inst, value);
		if (!inserted) return cached->second;
		if (inst->GetOpcode() != ValueOpcode::Phi) {
			Inst* projected = nullptr;
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
				const auto lowered = LowerDescriptorValue(inst->Arg(arg));
				if (lowered == inst->Arg(arg).Resolve()) continue;
				if (projected == nullptr) {
					projected = &m_program.value_storage.emplace_back(inst->GetOpcode(), inst->Flags<uint64_t>());
					for (size_t i = 0; i < inst->NumArgs(); ++i) projected->SetArg(i, inst->Arg(i));
				}
				projected->SetArg(arg, lowered);
			}
			if (projected != nullptr) cached->second = Value(projected);
			return cached->second;
		}
		if (inst->NumArgs() != 2u || inst->NumPhiBlocks() != 2u || inst->GetType() != Type::U32) {
			return value;
		}
		const auto* merge = inst->Parent();
		const auto* branch = inst->PhiBlock(0);
		if (merge == nullptr || branch == nullptr || inst->PhiBlock(1) == nullptr ||
		    branch == inst->PhiBlock(1)) {
			return value;
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
		std::array<const Block*, 2> targets;
		for (uint32_t arm = 0; arm < 2; arm++) {
			const auto* incoming = inst->PhiBlock(arm);
			if (incoming == merge ||
			    (incoming != branch &&
			     (incoming->ImmPredecessors().size() != 1u ||
			      incoming->ImmPredecessors()[0] != branch ||
			      incoming->ImmSuccessors().size() != 1u ||
			      incoming->ImmSuccessors()[0] != merge))) {
				return value;
			}
			targets[arm] = incoming == branch ? merge : incoming;
		}
		const auto& info = *branch;
		const auto& term = info.terminator;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch ||
		    !((term.true_block == targets[0] && term.false_block == targets[1]) ||
		      (term.false_block == targets[0] && term.true_block == targets[1])) ||
		    !ValidateRuntimeValue(m_program, info.condition, m_shader_writes
		        ? RuntimeValueType::ImmutableInteger : RuntimeValueType::Integer) ||
		    !ValidateRuntimeValue(m_program, inst->Arg(0)) ||
		    !ValidateRuntimeValue(m_program, inst->Arg(1))) {
			return value;
		}
		// Retain a host expression; replacing the GPU Phi would break SSA dominance.
		const auto true_arg = term.true_block == targets[0] ? 0u : 1u;
		auto&      selected = m_program.value_storage.emplace_back(ValueOpcode::SelectU32);
		selected.SetArg(0, info.condition);
		selected.SetArg(1, inst->Arg(true_arg));
		selected.SetArg(2, inst->Arg(true_arg ^ 1u));
		cached->second = Value(&selected);
		return cached->second;
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
		}
		descriptor.dword_count = width;
		for (uint32_t i = 0; i < width; i++) {
			const auto value = base_reg != UINT32_MAX
			    ? NativeDescriptorSource(handle.Arg(i), base_reg + i, pc) : handle.Arg(i);
			descriptor.dwords[i] = LowerDescriptorValue(value);
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

	void CollectScalarRead(Value value, uint32_t use_pc) {
		value = value.Resolve();
		if (value.IsImmediate()) return;
		auto* inst = value.TryInstruction();
		if (inst == nullptr) Fail(use_pc, "invalid typed planning value");
		const auto cycle = std::ranges::find(m_srt_visiting, inst);
		if (cycle != m_srt_visiting.end()) {
			if (std::any_of(cycle, m_srt_visiting.end(), [](const Inst* value) {
				return value->GetOpcode() == ValueOpcode::Phi;
			})) return;
			Fail(use_pc, "cyclic typed planning value without a phi");
		}
		if (std::ranges::find(m_srt_visited, inst) != m_srt_visited.end()) return;
		m_srt_visiting.push_back(inst);
		uint32_t memory_index = 0;
		const auto* memory = ScalarReadMemory(*inst, memory_index);
		DescriptorSource source;
		if (memory != nullptr) {
			const auto* handle = inst->Arg(0).Resolve().TryInstruction();
			const auto width = memory->kind == ResourceKind::ScalarBuffer ? 4u : 2u;
			if (handle == nullptr || handle->GetOpcode() !=
			        (width == 4u ? ValueOpcode::GetBufferResource : ValueOpcode::GetAddressResource))
				Fail(use_pc, "scalar read has an invalid resource handle");
			MakeSource(*handle, width, false, false, ScalarReadBase(*inst), source,
			           inst->Flags<MemoryFlags>().pc);
			for (uint32_t word = 0; word < width; ++word)
				CollectScalarRead(source.dwords[word], inst->Flags<MemoryFlags>().pc);
			for (size_t arg = 1; arg < inst->NumArgs(); ++arg)
				CollectScalarRead(inst->Arg(arg), use_pc);
		} else {
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg)
				CollectScalarRead(inst->Arg(arg), use_pc);
		}
		m_srt_visiting.pop_back();
		m_srt_visited.push_back(inst);
		if (memory == nullptr) return;
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) return;
		m_scalar_reads.push_back(inst);
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
		for (auto* read: m_scalar_reads) {
			const auto flags = read->Flags<MemoryFlags>();
			auto& memory = m_program.memory_info[flags.index];
			const auto* handle = read->Arg(0).Resolve().TryInstruction();
			auto resolved = std::ranges::find_if(m_resolved_handles, [&](const auto& entry) {
				return entry.handle == handle && entry.pc == flags.pc;
			});
			EXIT_IF(resolved == m_resolved_handles.end());
			uint32_t bad_dword = 0;
			if (!ValidateSource(resolved->source, bad_dword)) continue;
			memory.planning_only = true;
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
			for (auto* block: m_program.blocks) {
				if (block->condition.Resolve() == Value(read)) block->condition = flat;
				if (block->indirect_target.Resolve() == Value(read)) block->indirect_target = flat;
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
			auto& current = m_sources[candidate];
			if (current.dword_count != descriptor.dword_count ||
			    current.indirect_descriptor.has_value() != descriptor.indirect_descriptor.has_value()) {
				continue;
			}
			if (current.indirect_descriptor.has_value()) {
				const auto& a = *current.indirect_descriptor;
				const auto& b = *descriptor.indirect_descriptor;
				if (a.selector != b.selector || a.table_source != b.table_source ||
				    a.table_offset != b.table_offset || a.table_immediate != b.table_immediate ||
				    a.table_stride != b.table_stride || a.workgroup_axis != b.workgroup_axis ||
				    a.sources != b.sources || a.key_limit != b.key_limit ||
				    !EquivalentValue(m_program, a.key_count, b.key_count) ||
				    a.selector_first.IsEmpty() != b.selector_first.IsEmpty() ||
				    (!a.selector_first.IsEmpty() &&
				     !EquivalentValue(m_program, a.selector_first, b.selector_first)) ||
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
				if (current.indirect_descriptor) {
					current.indirect_descriptor->table_scalar |= descriptor.indirect_descriptor->table_scalar;
					current.indirect_descriptor->table_record_bytes = std::max(
					    current.indirect_descriptor->table_record_bytes,
					    descriptor.indirect_descriptor->table_record_bytes);
				}
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

	static bool UsesOnlyImageDescriptors(const Inst& value) {
		if (value.Uses().empty()) return false;
		std::vector<const Inst*> values {&value};
		for (size_t index = 0; index < values.size(); ++index) {
			for (const auto& use: values[index]->Uses()) {
				if (use.user->GetOpcode() == ValueOpcode::GetImageResource) continue;
				if (use.user->GetOpcode() != ValueOpcode::Phi) return false;
				if (std::ranges::find(values, use.user) == values.end()) values.push_back(use.user);
			}
		}
		return true;
	}

	const MemoryInfo* ScalarReadMemory(const Inst& read, uint32_t& index) const {
		const bool address = read.GetOpcode() == ValueOpcode::LoadAddressU32;
		if (!(address ? read.NumArgs() == 4u
		              : read.GetOpcode() == ValueOpcode::ReadConstBuffer && read.NumArgs() == 2u)) {
			return nullptr;
		}
		if (address) {
			const auto high = read.Arg(2).Resolve();
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
		return memory.kind == (address ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer) &&
		               memory.data_bits == 32u && memory.data_dwords == 1u
		           ? &memory
		           : nullptr;
	}

	bool MemoryIndexBelongsTo(uint32_t index, const Inst& owner) const {
		for (const auto* block: m_program.blocks) {
			for (const auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if ((BufferAccessOf(op) == BufferAccess::None &&
				     AddressOpcodeInfoOf(op).access == AddressAccess::None &&
				     ImageOpcodeInfoOf(op).access == ImageAccess::None) ||
				    &inst == &owner) {
					continue;
				}
				if (inst.Flags<MemoryFlags>().index == index) {
					return false;
				}
			}
		}
		return true;
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

	enum class LaneQuantifier { Any, All };
	struct EdgePredicate {
		Value condition;
		bool positive;
		LaneQuantifier lanes = LaneQuantifier::All;
		// An EXEC branch leaves only the lanes that satisfy it active.
		bool exec = false;
	};

	bool NonzeroOnEntry(Value value, const Block* block) const {
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

	uint32_t WorkgroupAxis(Value key) const {
		const auto* builtin = key.Resolve().TryInstruction();
		uint32_t axis = UINT32_MAX;
		if (m_program.stage != ShaderType::Compute || builtin == nullptr ||
		    builtin->GetOpcode() != ValueOpcode::GetBuiltin || builtin->NumArgs() != 2u ||
		    builtin->Arg(0) != Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)) ||
		    !ImmediateU32(builtin->Arg(1), axis) || axis >= 3u) return UINT32_MAX;
		return axis;
	}

	bool MatchTableOffset(Value value, Value& key, uint32_t& offset, uint32_t& stride) const {
		offset = 0;
		for (;;) {
			const auto* inst = value.Resolve().TryInstruction();
			if (inst == nullptr || inst->NumArgs() != 2u) {
				return false;
			}
			uint32_t immediate;
			if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
			    ImmediateU32(inst->Arg(1), immediate) && immediate < 32u) {
				key = inst->Arg(0).Resolve();
				stride = 1u << immediate;
				return key.GetType() == Type::U32;
			}
			if (inst->GetOpcode() == ValueOpcode::IMul32) {
				if (ImmediateU32(inst->Arg(0), stride)) key = inst->Arg(1).Resolve();
				else if (ImmediateU32(inst->Arg(1), stride)) key = inst->Arg(0).Resolve();
				else return false;
				return stride != 0u && key.GetType() == Type::U32;
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
		const auto& term = from->terminator;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch ||
		    (term.true_block == to) == (term.false_block == to))
			return {};
		EdgePredicate edge {from->condition, term.true_block == to};
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
				edge.exec = kind == CFG::BranchCondition::ExecZero ||
				            kind == CFG::BranchCondition::ExecNonZero;
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

	bool GuardedOnEntry(const Block* block, const auto& stops, const auto& accepts) const {
		std::vector<const Block*> pending {block};
		for (size_t i = 0; i < pending.size(); ++i) {
			const auto* current = pending[i];
			if (stops(current) || current->ImmPredecessors().empty()) return false;
			for (const auto* previous: current->ImmPredecessors()) {
				const auto edge = ConditionalEdge(previous, current);
				if (edge && accepts(*edge)) continue;
				if (std::ranges::find(pending, previous) == pending.end())
					pending.push_back(previous);
			}
		}
		return true;
	}

	bool HasActiveLane(Value mask, const Block* use) const {
		mask = mask.Resolve();
		const auto incoming_is_nonempty = [&](const Block* from, const Block* to,
		                                     Value incoming, const Block* header) {
			const auto accepts = [&](const EdgePredicate& edge) {
				return edge.positive && Implies(edge.condition, incoming);
			};
			const auto edge = ConditionalEdge(from, to);
			if (edge && accepts(*edge)) return true;
			const auto* definition = incoming.Resolve().TryInstruction();
			return GuardedOnEntry(from, [&](const Block* block) {
				// A witness before this definition may belong to an earlier loop iteration.
				return block == header || (definition != nullptr && block == definition->Parent());
			}, accepts);
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
		guard = SimplifyGuard(guard);
		required = required.Resolve();
		if (EquivalentValue(m_program, guard, required)) return true;
		const auto* inst = guard.TryInstruction();
		return inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalAnd &&
		       (Implies(inst->Arg(0), required) || Implies(inst->Arg(1), required));
	}

	Value EqualLocalKey(Value guard, Value key) const {
		guard = SimplifyGuard(guard);
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

	bool MatchAffineOffset(Value value, Value guard, AffineOffset& out,
	                       uint32_t depth = 0) const {
		if (depth > 16u) return false;
		value = value.Resolve();
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
			     !EquivalentValue(m_program, left.index, right.index))) return false;
			out.index = left.index.IsEmpty() ? right.index : left.index;
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
		out.index = value;
		out.stride = 1u;
		return value.GetType() == Type::U32;
	}

	bool ImpliesNonzero(Value guard, Value value) const {
		guard = SimplifyGuard(guard);
		const auto* inst = guard.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			return ImpliesNonzero(inst->Arg(0), value) ||
			       ImpliesNonzero(inst->Arg(1), value);
		}
		if (inst->GetOpcode() != ValueOpcode::INotEqual32 || inst->NumArgs() != 2u)
			return false;
		uint32_t zero = 1u;
		return (ImmediateU32(inst->Arg(0), zero) && zero == 0u &&
		        EquivalentValue(m_program, inst->Arg(1), value)) ||
		       (ImmediateU32(inst->Arg(1), zero) && zero == 0u &&
		        EquivalentValue(m_program, inst->Arg(0), value));
	}

	bool ImpliesIndexBelow32(Value guard, Value index) const {
		guard = SimplifyGuard(guard);
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
		if (EquivalentValue(m_program, update->Arg(0), mask)) bit = update->Arg(1);
		else if (EquivalentValue(m_program, update->Arg(1), mask)) bit = update->Arg(0);
		else return false;
		const auto* shift = bit.Resolve().TryInstruction();
		uint32_t one = 0;
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    shift->NumArgs() != 2u || !ImmediateU32(shift->Arg(0), one) || one != 1u)
			return false;
		Value position = shift->Arg(1).Resolve();
		const auto* masked = position.TryInstruction();
		uint32_t lane_mask = 0;
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

	// The mask of a bit scan is the scanned value, or the entry value of a loop phi that only
	// ever clears its bits. `width` receives the number of bits the mask can set; the mask
	// itself is returned only when the host can evaluate it.
	Value ScannedMask(Value value, uint32_t& width) const {
		width = 32u;
		value = value.Resolve();
		// The scalar mask may reach s_ff1 through a lane read of a wave-emulated SGPR.
		if (const auto* lane = value.TryInstruction();
		    lane != nullptr && lane->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    lane->NumArgs() == 2u && !ValidateRuntimeValue(m_program, value))
			value = lane->Arg(0).Resolve();
		const auto* phi = value.TryInstruction();
		if (phi != nullptr && phi->GetOpcode() == ValueOpcode::Phi && phi->NumArgs() == 2u &&
		    phi->GetType() == Type::U32) {
			for (uint32_t back = 0; back < 2u; ++back) {
				std::vector<const Inst*> proving;
				uint32_t                 budget = 256u;
				if (!ClearsBitsOf(phi->Arg(back), value, *phi, back, proving, budget)) continue;
				value = ArmValue(phi->Arg(back ^ 1u), *phi, back ^ 1u);
				break;
			}
		}
		width = MaskWidth(value);
		return ValidateRuntimeValue(m_program, value, RuntimeValueType::Integer) ? value : Value {};
	}

	// Bits a mask can set: a bit field extract or an immediate AND bounds it statically.
	static uint32_t MaskWidth(Value value) {
		const auto* inst      = value.Resolve().TryInstruction();
		uint32_t    immediate = 0;
		if (inst == nullptr) return 32u;
		if (inst->GetOpcode() == ValueOpcode::BitFieldUExtract && inst->NumArgs() == 3u &&
		    ImmediateU32(inst->Arg(2), immediate))
			return std::min(immediate, 32u);
		if (inst->GetOpcode() == ValueOpcode::BitwiseAnd32 && inst->NumArgs() == 2u) {
			for (uint32_t arg = 0; arg < 2u; ++arg) {
				if (ImmediateU32(inst->Arg(arg), immediate)) return std::bit_width(immediate);
			}
			return std::min(MaskWidth(inst->Arg(0)), MaskWidth(inst->Arg(1)));
		}
		if (inst->GetOpcode() == ValueOpcode::ReadFirstLane && inst->NumArgs() == 2u)
			return MaskWidth(inst->Arg(0));
		return 32u;
	}

	// Active lanes reaching the incoming block of a phi arm satisfy every positive edge
	// predicate that holds for all lanes or sets EXEC on their way from the loop header;
	// selects on those predicates take their true arm.
	bool HoldsOnArm(Value predicate, const Inst& phi, uint32_t arm) const {
		const auto* incoming = phi.PhiBlock(arm);
		if (incoming == phi.Parent()) return false;
		return GuardedOnEntry(
		    incoming, [&](const Block* block) { return block == phi.Parent(); },
		    [&](const EdgePredicate& edge) {
			    return edge.positive && (edge.lanes == LaneQuantifier::All || edge.exec) &&
			           Implies(edge.condition, predicate);
		    });
	}

	// The value a phi arm carries: lane selects whose predicate holds on the arm resolve to the
	// selected value. A sibling header phi stays as is: in the loop body it holds its
	// current-iteration value, not the value of its back arm.
	Value ArmValue(Value value, const Inst& phi, uint32_t arm, uint32_t depth = 0) const {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 16u) return value;
		if (inst->GetOpcode() == ValueOpcode::SelectU32 && inst->NumArgs() == 3u) {
			const auto predicate = ArmValue(inst->Arg(0), phi, arm, depth + 1u);
			if (predicate.IsImmediate() && predicate.GetType() == Type::U1)
				return ArmValue(inst->Arg(predicate.U1() ? 1u : 2u), phi, arm, depth + 1u);
			if (HoldsOnArm(inst->Arg(0), phi, arm))
				return ArmValue(inst->Arg(1), phi, arm, depth + 1u);
		}
		return value;
	}

	// value is mask, or mask with bits cleared, along every lane select and phi merge. Only a
	// merge still being proven, the scanned phi first, may assume its own claim.
	bool ClearsBitsOf(Value value, Value mask, const Inst& phi, uint32_t arm,
	                  std::vector<const Inst*>& proving, uint32_t& budget) const {
		if (value.Resolve() == mask) return true;
		value = ArmValue(value, phi, arm);
		if (value == mask) return true;
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || budget == 0u) return false;
		--budget;
		const auto clears = [&](Value operand) {
			return ClearsBitsOf(operand, mask, phi, arm, proving, budget);
		};
		switch (inst->GetOpcode()) {
			case ValueOpcode::ReadFirstLane: return inst->NumArgs() == 2u && clears(inst->Arg(0));
			case ValueOpcode::Phi: {
				if (std::ranges::find(proving, inst) != proving.end()) return true;
				proving.push_back(inst);
				bool cleared = inst->NumArgs() != 0u;
				for (size_t index = 0; cleared && index < inst->NumArgs(); ++index)
					cleared = clears(inst->Arg(index));
				proving.pop_back();
				return cleared;
			}
			case ValueOpcode::BitwiseAnd32:
				return inst->NumArgs() == 2u && (clears(inst->Arg(0)) || clears(inst->Arg(1)));
			case ValueOpcode::SelectU32:
				return inst->NumArgs() == 3u && clears(inst->Arg(1)) && clears(inst->Arg(2));
			default: return MaskOnlyLosesBits(value, mask);
		}
	}

	// True when `condition` (negated when !positive) implies key < bound for a runtime bound.
	// `facts` holds operands known true on this path, so a negated conjunction collapses.
	bool ProvesKeyBelow(Value condition, bool positive, Value key, Value& bound,
	                    std::vector<Value> facts = {}, uint32_t depth = 0) const {
		if (depth > 12u) return false;
		const auto* test = condition.Resolve().TryInstruction();
		if (test == nullptr) return false;
		const auto op = test->GetOpcode();
		if (op == ValueOpcode::ConditionRef) {
			return ProvesKeyBelow(test->Arg(0), positive, key, bound, facts, depth + 1u);
		}
		if (op == ValueOpcode::LogicalNot) {
			return ProvesKeyBelow(test->Arg(0), !positive, key, bound, facts, depth + 1u);
		}
		if (op == ValueOpcode::LogicalAnd) {
			if (positive) {
				auto with_a = facts, with_b = facts;
				with_a.push_back(test->Arg(0).Resolve());
				with_b.push_back(test->Arg(1).Resolve());
				return ProvesKeyBelow(test->Arg(0), true, key, bound, with_b, depth + 1u) ||
				       ProvesKeyBelow(test->Arg(1), true, key, bound, with_a, depth + 1u);
			}
			const auto known = [&](Value value) {
				return std::ranges::any_of(
				    facts, [&](Value fact) { return EquivalentValue(m_program, fact, value); });
			};
			// A false conjunction with one true operand negates the other.
			return (known(test->Arg(0)) &&
			        ProvesKeyBelow(test->Arg(1), false, key, bound, facts, depth + 1u)) ||
			       (known(test->Arg(1)) &&
			        ProvesKeyBelow(test->Arg(0), false, key, bound, facts, depth + 1u));
		}
		if (op == ValueOpcode::LogicalOr && !positive) {
			return ProvesKeyBelow(test->Arg(0), false, key, bound, facts, depth + 1u) ||
			       ProvesKeyBelow(test->Arg(1), false, key, bound, facts, depth + 1u);
		}
		if (test->NumArgs() != 2u) return false;
		const auto bounded = [&](Value compared, Value limit) {
			if (!EquivalentValue(m_program, compared, key) ||
			    !ValidateRuntimeValue(m_program, limit, RuntimeValueType::Integer))
				return false;
			bound = limit.Resolve();
			return true;
		};
		if ((op == ValueOpcode::ULessThan32 && positive) ||
		    (op == ValueOpcode::UGreaterThanEqual32 && !positive)) {
			return bounded(test->Arg(0), test->Arg(1));
		}
		if ((op == ValueOpcode::UGreaterThan32 && positive) ||
		    (op == ValueOpcode::ULessThanEqual32 && !positive)) {
			return bounded(test->Arg(1), test->Arg(0));
		}
		// value == 0 (s_cmp_lg_u32 0, flags taken false): a flag set by key >= bound is clear.
		if ((op == ValueOpcode::INotEqual32 && !positive) ||
		    (op == ValueOpcode::IEqual32 && positive)) {
			for (uint32_t arg = 0; arg < 2u; ++arg) {
				uint32_t zero = 1u;
				if (ImmediateU32(test->Arg(arg), zero) && zero == 0u &&
				    ProvesZeroKeyBelow(test->Arg(arg ^ 1u), key, bound, facts, depth + 1u))
					return true;
			}
		}
		return false;
	}

	// value == 0 implies key < bound: a nonzero flag selected by key >= bound, possibly
	// or-ed with other flags or selected under a known-true lane predicate.
	bool ProvesZeroKeyBelow(Value value, Value key, Value& bound, const std::vector<Value>& facts,
	                        uint32_t depth) const {
		if (depth > 12u) return false;
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::BitwiseOr32 && inst->NumArgs() == 2u) {
			return ProvesZeroKeyBelow(inst->Arg(0), key, bound, facts, depth + 1u) ||
			       ProvesZeroKeyBelow(inst->Arg(1), key, bound, facts, depth + 1u);
		}
		if (inst->GetOpcode() != ValueOpcode::SelectU32 || inst->NumArgs() != 3u) return false;
		uint32_t set = 0, clear = 1;
		if (ImmediateU32(inst->Arg(1), set) && set != 0u && ImmediateU32(inst->Arg(2), clear) &&
		    clear == 0u)
			return ProvesKeyBelow(inst->Arg(0), false, key, bound, facts, depth + 1u);
		const auto condition = inst->Arg(0).Resolve();
		return std::ranges::any_of(
		           facts,
		           [&](Value fact) { return EquivalentValue(m_program, fact, condition); }) &&
		       ProvesZeroKeyBelow(inst->Arg(1), key, bound, facts, depth + 1u);
	}

	// Every path to `use` crosses an edge proving key < bound (the loop's count exit).
	Value ScanBound(Value key, const Block* use) const {
		Value      bound;
		const bool guarded = GuardedOnEntry(
		    use, [](const Block*) { return false; },
		    [&](const EdgePredicate& edge) {
			    Value limit;
			    if (!ProvesKeyBelow(edge.condition, edge.positive, key, limit)) return false;
			    if (bound.IsEmpty()) bound = limit;
			    return EquivalentValue(m_program, bound, limit);
		    });
		return guarded ? bound : Value {};
	}

	Value InitialCandidateMask(Value value, const Block* update_block) const {
		const auto* phi = value.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->NumArgs() != 2u || phi->GetType() != Type::U32) return {};
		for (uint32_t back = 0; back < 2u; ++back) {
			if (phi->PhiBlock(back) != update_block ||
			    !MaskOnlyLosesBits(phi->Arg(back), value)) continue;
			const auto initial = phi->Arg(back ^ 1u).Resolve();
			return ValidateRuntimeValue(m_program, initial, RuntimeValueType::Integer)
			           ? initial : Value {};
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
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->NumArgs() != 3u || phi->GetType() != Type::U32 ||
		    !ImpliesIndexBelow32(guard, index)) return {};
		for (uint32_t bit_arm = 0; bit_arm < 3u; ++bit_arm) {
			const auto bit_guard = PositiveLaneWitness(phi->PhiBlock(bit_arm));
			const auto* selected = phi->Arg(bit_arm).Resolve().TryInstruction();
			if (selected == nullptr || selected->GetOpcode() != ValueOpcode::SelectU32 ||
			    selected->NumArgs() != 3u ||
			    !Implies(bit_guard, selected->Arg(0))) continue;
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
				    sentinel->NumArgs() != 3u ||
				    !Implies(sentinel_guard, sentinel->Arg(0)) ||
				    !ImmediateU32(sentinel->Arg(1), bound) || bound != 32u) continue;
				const auto loop_active = sentinel->Arg(0).Resolve();
				const auto* active_phi = loop_active.TryInstruction();
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

	const Inst* UniformizedMaterialValue(Value key, const Inst& use, Value executing = {}) const {
		Value guard;
		Value local;
		const auto* first = key.Resolve().TryInstruction();
		if (first != nullptr && first->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    first->NumArgs() == 2u) {
			guard = first->Arg(1).Resolve();
			// Empty EXEC selects lane zero, which may not have loaded a material key.
			// An executing store also witnesses nonempty EXEC at the lane read.
			if (executing.IsEmpty() ? !HasActiveLane(guard, first->Parent())
			                        : !Implies(executing, guard)) return nullptr;
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
			guard = PositiveLaneWitness(use.Parent());
			if (guard.IsEmpty()) return nullptr;
			local = EqualLocalKey(guard, key);
		}
		const auto* selected = local.Resolve().TryInstruction();
		if (selected == nullptr || selected->GetOpcode() != ValueOpcode::SelectU32 ||
		    selected->NumArgs() != 3u || !Implies(guard, selected->Arg(0))) return nullptr;
		return selected;
	}

	bool MatchUniformizedMaterialKey(Value key, const Inst& image,
	                                DescriptorSource::IndirectDescriptor& indirect,
	                                DescriptorSource& material_source) {
		const auto* selected = UniformizedMaterialValue(key, image);
		if (selected == nullptr) return false;
		const auto active = selected->Arg(0).Resolve();
		const auto* read = selected->Arg(1).Resolve().TryInstruction();
		if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadAddressU32 ||
		    read->NumArgs() != 4u || !EquivalentValue(m_program, read->Arg(3), active))
			return false;
		uint32_t high = 1u;
		if (!ImmediateU32(read->Arg(2), high) || high != 0u) return false;
		const auto memory_index = read->Flags<MemoryFlags>().index;
		if (memory_index >= m_program.memory_info.size()) return false;
		const auto& memory = m_program.memory_info[memory_index];
		if (memory.kind != ResourceKind::Global || memory.data_bits != 32u ||
		    memory.data_dwords != 1u || !MemoryIndexBelongsTo(memory_index, *read)) return false;
		const auto* material_handle = read->Arg(0).Resolve().TryInstruction();
		if (material_handle == nullptr ||
		    material_handle->GetOpcode() != ValueOpcode::GetAddressResource ||
		    !MakeRuntimeTableSource(*read, material_source)) return false;
		AffineOffset offset;
		if (!MatchAffineOffset(read->Arg(1), active, offset) || offset.index.IsEmpty() ||
		    offset.stride == 0u || offset.offset + memory.offset > UINT32_MAX ||
		    offset.offset + memory.offset + 31u * offset.stride + 4u >
		        static_cast<uint64_t>(UINT32_MAX) + 1u) return false;
		const auto mask = BoundedSetBitMask(offset.index, active);
		if (mask.IsEmpty()) return false;
		indirect.selector.emplace(DescriptorSource::IndirectDescriptor::SelectorRead{.source = InternSource(material_source),
		                              .stride = static_cast<uint32_t>(offset.stride),
		                              .offset = static_cast<uint32_t>(offset.offset + memory.offset)});
		indirect.key_count = Value(32u);
		indirect.selector_mask = mask;
		return true;
	}

	const Inst* BoundedLoop(Value key, const Block* use, const auto& accepts_bound) const {
		const auto* phi = key.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi || phi->GetType() != Type::U32 ||
		    phi->NumArgs() != 2u || phi->NumPhiBlocks() != 2u)
			return {};
		const Block* increment_block = nullptr;
		uint32_t initial_arm = 0;
		for (uint32_t initial = 0; initial < 2u; ++initial) {
			const auto zero = phi->Arg(initial).Resolve();
			const auto* step = phi->Arg(initial ^ 1u).Resolve().TryInstruction();
			if (!zero.IsImmediate() || zero.GetType() != Type::U32 || zero.U32() != 0u ||
			    step == nullptr || step->GetOpcode() != ValueOpcode::IAdd32 ||
			    step->Parent() == nullptr) continue;
			// Structurization can forward the native increment through a synthetic continue block.
			const auto* incoming = phi->PhiBlock(initial ^ 1u);
			const auto* next = phi->Parent();
			for (size_t edges = 0; incoming != step->Parent() && edges < m_program.blocks.size(); ++edges) {
				if (incoming == nullptr || incoming->terminator.kind != CFG::TerminatorKind::Branch ||
				    incoming->terminator.true_block != next || incoming->ImmPredecessors().size() != 1u)
					break;
				next = incoming;
				incoming = incoming->ImmPredecessors()[0];
			}
			if (incoming != step->Parent()) continue;
			uint32_t increment = 0;
			if ((step->Arg(0).Resolve() == key &&
			     ImmediateU32(step->Arg(1), increment) && increment == 1u) ||
			    (step->Arg(1).Resolve() == key &&
			     ImmediateU32(step->Arg(0), increment) && increment == 1u)) {
				increment_block = step->Parent();
				initial_arm = initial;
				break;
			}
		}
		if (increment_block == nullptr) return {};

		const auto stops = [&](const Block* block) { return block == phi->Parent(); };
		for (const auto& use_of_key: phi->Uses()) {
			const auto* compare = use_of_key.user;
			if ((compare->GetOpcode() != ValueOpcode::SLessThan32 &&
			     compare->GetOpcode() != ValueOpcode::ULessThan32) || use_of_key.operand != 0u ||
			    !accepts_bound(*compare, initial_arm)) continue;
			if (!GuardedOnEntry(use != nullptr ? use : increment_block, stops, [&](const EdgePredicate& edge) {
				return edge.positive && Implies(edge.condition, Value(use_of_key.user));
			})) continue;
			LoopBoundProof proof(m_program, *phi, *compare);
			// The image bound and the increment guard are separate obligations: a
			// skipped image alone does not prevent signed induction wraparound.
			if (GuardedOnEntry(increment_block, stops, [&](const EdgePredicate& edge) {
				return proof.Excludes(edge.condition, edge.positive);
			})) return compare;
		}
		return {};
	}

	const Inst* BoundedLoop(Value key, const Block* use) const {
		return BoundedLoop(key, use, [&](const Inst& compare, uint32_t) {
			return ValidateRuntimeValue(m_program, compare.Arg(1), RuntimeValueType::Integer);
		});
	}

	bool MatchScalarMaterialKey(Value key, DescriptorSource::IndirectDescriptor& indirect,
	                            DescriptorSource& material_source) {
		const auto* read = key.Resolve().TryInstruction();
		if (read == nullptr || read->GetOpcode() != ValueOpcode::ReadConstBuffer) return false;
		uint32_t memory_index = 0;
		const auto* memory = ScalarReadMemory(*read, memory_index);
		Value index;
		uint32_t offset = 0, stride = 0;
		if (memory == nullptr || memory->kind != ResourceKind::ScalarBuffer ||
		    memory->offset > INT32_MAX || !MemoryIndexBelongsTo(memory_index, *read) ||
		    !MatchTableOffset(read->Arg(1), index, offset, stride) ||
		    ((offset | stride | memory->offset) & 3u) != 0u ||
		    uint64_t {offset} + memory->offset > UINT32_MAX) return false;
		const auto* bound = BoundedLoop(index, read->Parent());
		if (bound == nullptr || bound->GetOpcode() != ValueOpcode::ULessThan32 ||
		    !MakeRuntimeTableSource(*read, material_source)) return false;
		indirect.selector.emplace(DescriptorSource::IndirectDescriptor::SelectorRead {
		    .source = InternSource(material_source), .stride = stride,
		    .offset = offset + memory->offset});
		indirect.selector_first = Value(0u);
		indirect.key_count = bound->Arg(1);
		return true;
	}

	void FoldBoundedLoopSelectors() {
		for (auto* header: m_program.blocks) {
			for (auto& induction: *header) {
				const Inst* carried = nullptr;
				uint32_t maximum = 0;
				const auto* compare = BoundedLoop(Value(&induction), nullptr,
				    [&](const Inst& test, uint32_t initial) {
					if (test.GetOpcode() != ValueOpcode::ULessThan32) return false;
					carried = test.Arg(1).Resolve().TryInstruction();
					if (carried == nullptr || carried->GetOpcode() != ValueOpcode::Phi ||
					    carried->GetType() != Type::U32 || carried->Parent() != header ||
					    carried->NumArgs() != 2u || carried->NumPhiBlocks() != 2u ||
					    carried->PhiBlock(0) != induction.PhiBlock(0) ||
					    carried->PhiBlock(1) != induction.PhiBlock(1)) return false;
					maximum = SelectorMaximum(carried->Arg(initial));
					if (maximum == 0u || maximum == UINT32_MAX) return false;
					const std::array assumptions {std::pair {static_cast<const Inst*>(&induction), maximum - 1u},
					                             std::pair {carried, maximum}};
					// Close the carried-bound invariant before using either assumption.
					return SelectorMaximum(carried->Arg(initial ^ 1u), assumptions) <= maximum;
				});
				if (compare == nullptr) continue;
				const std::array assumptions {std::pair {static_cast<const Inst*>(&induction), maximum - 1u},
				                             std::pair {carried, maximum}};
				// Native scalar branches advance the zero/+1 induction uniformly, so
				// even an any-lane bound test limits it by the largest carried bound.
				// Per-lane edges instead require the comparison on every executing lane.
				for (auto* block: m_program.blocks) {
					if (!GuardedOnEntry(block, [&](const Block* at) { return at == header; },
					    [&](const EdgePredicate& edge) {
						return edge.positive && Implies(edge.condition, Value(const_cast<Inst*>(compare)));
					})) continue;
					for (auto& inst: *block) {
						if (inst.GetOpcode() == ValueOpcode::SelectU32 &&
						    SelectorPredicateFalse(inst.Arg(0), assumptions))
							inst.ReplaceUsesWith(inst.Arg(2));
					}
				}
			}
		}
	}

	bool BoundedSelectedIndex(Value index, Value active,
	                          DescriptorSource::IndirectDescriptor& indirect) const {
		const auto* selected = index.Resolve().TryInstruction();
		if (selected == nullptr || selected->GetOpcode() != ValueOpcode::Phi ||
		    selected->NumArgs() == 0u) return false;
		std::vector<const Inst*> values {selected};
		for (size_t i = 0; i < values.size(); ++i) {
			const auto* value = values[i];
			if (value->GetOpcode() != ValueOpcode::Phi &&
			    value->GetOpcode() != ValueOpcode::SelectU32) continue;
			for (size_t arm = value->GetOpcode() == ValueOpcode::SelectU32 ? 1u : 0u;
			     arm < value->NumArgs(); ++arm) {
				const auto* next = value->Arg(arm).Resolve().TryInstruction();
				if (next != nullptr && std::ranges::find(values, next) == values.end())
					values.push_back(next);
			}
		}
		for (const auto* candidate: values) {
			if (candidate->GetOpcode() != ValueOpcode::IAdd32) continue;
			for (uint32_t arg = 0; arg < 2u; ++arg) {
				const auto* bound = BoundedLoop(candidate->Arg(arg), candidate->Parent());
				const auto first = candidate->Arg(arg ^ 1u);
				if (bound == nullptr || !ValidateRuntimeValue(m_program, first, RuntimeValueType::Integer))
					continue;
				const auto* induction = candidate->Arg(arg).ResolveInstruction();
				const Inst* carried = nullptr;
				for (const auto* value: values) {
					if (value->GetOpcode() == ValueOpcode::Phi && value->Parent() == induction->Parent()) {
						if (carried != nullptr) { carried = nullptr; break; }
						carried = value;
					}
				}
				if (carried == nullptr || carried->NumArgs() != 2u ||
				    carried->PhiBlock(0) != induction->PhiBlock(0) ||
				    carried->PhiBlock(1) != induction->PhiBlock(1)) continue;
				std::vector<std::pair<Value, bool>> exits;
				for (size_t arm = 0; arm < selected->NumArgs(); ++arm) {
					const auto edge = ConditionalEdge(selected->PhiBlock(arm), selected->Parent());
					if (!edge || edge->lanes != LaneQuantifier::All) break;
					exits.emplace_back(edge->condition, edge->positive);
				}
				if (exits.size() != selected->NumArgs()) continue;
				for (const auto& mask: *induction->Parent()) {
					if (mask.GetOpcode() != ValueOpcode::Phi || mask.GetType() != Type::U1 ||
					    mask.NumArgs() != 2u || mask.PhiBlock(0) != carried->PhiBlock(0) ||
					    mask.PhiBlock(1) != carried->PhiBlock(1)) continue;
					const auto initial = induction->Arg(0).Resolve().IsImmediate() ? 0u : 1u;
					const auto enabled = mask.Arg(initial).Resolve();
					if (!enabled.IsImmediate() || enabled.GetType() != Type::U1 || !enabled.U1()) continue;
					LoopBoundProof proof(m_program, *induction, *bound);
					if (!proof.SelectsRange(*selected, active, *candidate, *carried, mask, exits)) continue;
					indirect.selector_first = first;
					indirect.key_count = bound->Arg(1);
					return true;
				}
			}
		}
		return false;
	}

	bool MatchDescriptorTable(Inst& handle, const DescriptorSource& descriptor,
	                          IndirectDescriptorPlan& plan,
	                          DescriptorSource& table_source, Value& key, uint32_t& table_offset,
	                          uint32_t& table_stride, uint32_t& table_immediate) {
		Inst* table_handle = nullptr;
		Value table_guard;
		for (uint32_t dword = 0; dword < handle.NumArgs(); ++dword) {
			auto* read = descriptor.dwords[dword].Resolve().TryInstruction();
			if (read == nullptr) {
				return false;
			}
			uint32_t memory_index = 0;
			const auto* memory = ScalarReadMemory(*read, memory_index);
			uint32_t component = 0;
			const bool indexed = memory == nullptr;
			if (indexed) {
				if (handle.NumArgs() != 8u) return false;
				const auto* selected = UniformizedMaterialValue(descriptor.dwords[dword], handle);
				if (selected == nullptr) return false;
				const auto* extract = selected->Arg(1).Resolve().TryInstruction();
				if (extract == nullptr || extract->GetOpcode() != ValueOpcode::CompositeExtractU32x4 ||
				    !ImmediateU32(extract->Arg(1), component) || component >= 4u) return false;
				read = extract->Arg(0).Resolve().TryInstruction();
				if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadBufferU32x4) return false;
				memory_index = read->Flags<MemoryFlags>().index;
				if (memory_index >= m_program.memory_info.size()) return false;
				memory = &m_program.memory_info[memory_index];
				uint32_t offset = 1u, scalar = 1u;
				if (memory->kind != ResourceKind::Buffer || memory->typed || memory->formatted ||
				    !memory->idxen || memory->offen || memory->data_bits != 32u ||
				    memory->data_dwords != 4u ||
				    !ImmediateU32(read->Arg(2), offset) || offset != 0u ||
				    !ImmediateU32(read->Arg(3), scalar) || scalar != 0u ||
				    !EquivalentValue(m_program, read->Arg(4), selected->Arg(0)) ||
				    (dword != 0u && !EquivalentValue(m_program, table_guard, read->Arg(4)))) return false;
				table_guard = read->Arg(4);
			}
			if (memory->offset > INT32_MAX || (memory->offset & 3u) != 0u ||
			    !MemoryIndexBelongsTo(memory_index, *read)) {
				return false;
			}
			auto* current_handle = read->Arg(0).Resolve().TryInstruction();
			Value current_key;
			uint32_t offset = 0;
			uint32_t stride = 0;
			if (current_handle == nullptr ||
			    current_handle->GetOpcode() != (memory->kind == ResourceKind::ScalarAddress
			                                      ? ValueOpcode::GetAddressResource
			                                      : ValueOpcode::GetBufferResource) ||
			    (memory->kind == ResourceKind::ScalarAddress && read->Parent() != handle.Parent()) ||
			    (table_handle != nullptr &&
			     !EquivalentValue(m_program, Value(table_handle), Value(current_handle))) ||
			    (!indexed && !MatchTableOffset(read->Arg(1), current_key, offset, stride))) {
				return false;
			}
			if (indexed) {
				current_key = read->Arg(1);
				offset = component * 4u;
			}
			if (dword == 0u) {
				if (component != 0u) return false;
				plan.table_indexed = indexed;
				key = current_key;
				table_offset = offset;
				table_immediate = memory->offset;
				table_stride = stride;
			} else if (plan.table_indexed != indexed || table_stride != stride ||
			           !EquivalentValue(m_program, key, current_key) ||
			           uint64_t {table_offset} + table_immediate + dword * sizeof(uint32_t) !=
			               uint64_t {offset} + memory->offset) {
				return false;
			}
			plan.split_offsets |= !indexed && offset != table_offset;
			table_handle = current_handle;
			if (handle.NumArgs() == 8u) {
				if (memory->kind == ResourceKind::ScalarAddress)
					plan.retain_reads |= !UsesOnlyImageDescriptors(*read);
			} else if (std::ranges::any_of(read->Uses(), [](const Use& use) {
				return use.user->GetOpcode() != ValueOpcode::GetBufferResource ||
				       std::ranges::any_of(use.user->Uses(), [](const Use& consumer) {
					       return consumer.user->GetOpcode() != ValueOpcode::StoreBufferU32;
				       });
			    })) {
				return false;
			}
			plan.memory[dword] = memory_index;
			plan.reads[dword] = read;
		}

		if (!MakeRuntimeTableSource(*plan.reads[0], table_source)) {
			return false;
		}
		return true;
	}

	bool TryMakeIndirectImage(Inst& handle, const DescriptorSource& descriptor,
	                          IndirectDescriptorPlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetImageResource || handle.NumArgs() != 8u) return false;
		DescriptorSource table_source;
		Value key;
		uint32_t table_offset = 0;
		uint32_t table_stride = 0;
		uint32_t table_immediate = 0;
		if (!MatchDescriptorTable(handle, descriptor, plan, table_source, key, table_offset,
		                          table_stride, table_immediate)) return false;
		DescriptorSource material_source;
		DescriptorSource::IndirectDescriptor indirect;
		indirect.table_offset = table_offset;
		indirect.table_immediate = table_immediate;
		indirect.table_stride = table_stride;
		indirect.workgroup_axis = WorkgroupAxis(key);
		if (indirect.workgroup_axis != UINT32_MAX && !plan.table_indexed) {
			// The descriptor also supplies dimensions to shader arithmetic. Keep its reads;
			// only the image handle is projected onto the bounded workgroup key.
			plan.retain_reads = true;
		} else if (table_source.dword_count == 2u) {
			if (table_stride != 32u) return false;
			const auto* selector = key.Resolve().TryInstruction();
			const bool bitscan = selector != nullptr && selector->GetOpcode() == ValueOpcode::FindILsb32 &&
			    selector->NumArgs() == 1u && !m_shader_writes &&
			    NonzeroOnEntry(selector->Arg(0), handle.Parent());
			if (bitscan) {
				// The scan stops at the mask's set bits, its width and the loop's count exit.
				uint32_t width         = 32u;
				indirect.selector_mask = ScannedMask(selector->Arg(0), width);
				const auto bound       = ScanBound(key, handle.Parent());
				indirect.key_count     = bound.IsEmpty() ? Value(width) : bound;
				indirect.key_limit     = width;
			} else if (!m_shader_writes) {
				if (const auto* bound = BoundedLoop(key, handle.Parent()))
					indirect.key_count = bound->Arg(1);
			}
			if (indirect.key_count.IsEmpty() &&
			    !MatchUniformizedMaterialKey(key, handle, indirect, material_source)) {
				return false;
			}
			if ((table_offset & 3u) != 0u ||
			    (bitscan && table_offset > UINT32_MAX - (32u * 32u - 1u))) return false;
		} else if (!plan.table_indexed &&
		           MatchScalarMaterialKey(key, indirect, material_source)) {
			// The host enumerates the guarded scalar loop; its key and T# payload stay on the GPU.
			plan.retain_reads = true;
		} else {
			// A bounded V# supplies the complete image table. Leave every GPU selector
			// and descriptor read in the shader; the host only translates table bytes.
			if (plan.split_offsets) return false;
			indirect.table_stride = 0u;
			indirect.table_offset = 0u;
			indirect.table_immediate = 0u;
			indirect.workgroup_axis = UINT32_MAX;
			plan.table_read = plan.reads[0];
			if (plan.table_indexed) indirect.table_record_bytes = table_immediate + 32u;
			else indirect.table_scalar = true;
			plan.retain_reads = true;
		}

		if (plan.retain_reads) plan.reads.fill(nullptr);
		indirect.table_source = InternSource(table_source);
		DescriptorSource image_source;
		image_source.dword_count = 8u;
		image_source.dwords.fill(Value(0u));
		std::copy_n(material_source.dwords.begin(), material_source.dword_count,
		            image_source.dwords.begin());
		std::copy_n(table_source.dwords.begin(), table_source.dword_count,
		            image_source.dwords.begin() + 4u);
		image_source.indirect_descriptor = indirect;
		plan.handle = &handle;
		plan.source = InternSource(image_source);
		plan.key = key;
		plan.roots = image_source.dwords;
		return true;
	}

	bool TryMakeIndirectBuffer(Inst& handle, const DescriptorSource& descriptor,
	                           IndirectDescriptorPlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetBufferResource || handle.NumArgs() != 4u ||
		    handle.Uses().empty()) return false;
		for (const auto& use: handle.Uses()) {
			const auto& inst = *use.user;
			if (inst.GetOpcode() != ValueOpcode::StoreBufferU32) return false;
			const auto& memory = m_program.memory_info[inst.Flags<MemoryFlags>().index];
			uint32_t scalar = 1u;
			if (memory.kind != ResourceKind::Buffer || memory.typed || memory.formatted ||
			    memory.idxen || !memory.offen || !ImmediateU32(inst.Arg(3), scalar) || scalar != 0u)
				return false;
		}
		DescriptorSource table_source;
		Value key;
		uint32_t table_offset = 0;
		uint32_t table_stride = 0;
		uint32_t table_immediate = 0;
		if (!MatchDescriptorTable(handle, descriptor, plan, table_source, key, table_offset,
		                          table_stride, table_immediate) || table_stride != 16u ||
		    table_source.dword_count != 4u || table_offset != 0u || table_immediate != 0u) return false;
		const auto* lane_read = key.Resolve().TryInstruction();
		if (lane_read == nullptr || lane_read->GetOpcode() != ValueOpcode::ReadFirstLane)
			return false;
		const Inst* selected = nullptr;
		for (const auto& use: handle.Uses()) {
			const auto* value = UniformizedMaterialValue(
			    key, *use.user, use.user->Arg(use.user->NumArgs() - 1u));
			if (value == nullptr || (selected != nullptr && selected != value)) return false;
			selected = value;
		}
		const auto* read = selected->Arg(1).Resolve().TryInstruction();
		uint32_t lane = 0;
		if (read != nullptr && read->GetOpcode() == ValueOpcode::CompositeExtractU32x2) {
			if (!ImmediateU32(read->Arg(1), lane) || lane >= 2u) return false;
			read = read->Arg(0).Resolve().TryInstruction();
			if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadBufferU32x2) return false;
		} else if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadBufferU32) {
			return false;
		}
		const auto& memory = m_program.memory_info[read->Flags<MemoryFlags>().index];
		uint32_t offset = 1u, scalar = 1u;
		if (memory.kind != ResourceKind::Buffer || memory.typed || memory.formatted ||
		    !memory.idxen || memory.offen || memory.offset > UINT32_MAX - lane * 4u ||
		    !ImmediateU32(read->Arg(2), offset) || offset != 0u ||
		    !ImmediateU32(read->Arg(3), scalar) || scalar != 0u ||
		    !EquivalentValue(m_program, selected->Arg(0), read->Arg(4))) return false;
		DescriptorSource material_source;
		DescriptorSource::IndirectDescriptor indirect;
		indirect.table_stride = table_stride;
		if (!MakeRuntimeTableSource(*read, material_source) ||
		    !BoundedSelectedIndex(read->Arg(1), read->Arg(4), indirect)) return false;
		indirect.selector.emplace(DescriptorSource::IndirectDescriptor::SelectorRead{.source = InternSource(material_source),
		                              .offset = memory.offset + lane * 4u});
		indirect.table_source = InternSource(table_source);
		DescriptorSource source;
		source.dword_count = 4u;
		source.dwords.fill(Value(0u));
		source.indirect_descriptor = indirect;
		plan.handle = &handle;
		plan.source = InternSource(source);
		plan.key = key;
		return true;
	}

	using SelectorBounds = std::span<const std::pair<const Inst*, uint32_t>>;

	bool SelectorPredicateFalse(Value value, SelectorBounds bounds, uint32_t depth = 0) const {
		if (depth > 32u) return false;
		value = value.Resolve();
		if (value.IsImmediate()) return value.GetType() == Type::U1 && !value.U1();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd)
			return SelectorPredicateFalse(inst->Arg(0), bounds, depth + 1u) ||
			       SelectorPredicateFalse(inst->Arg(1), bounds, depth + 1u);
		if (inst->GetOpcode() == ValueOpcode::IEqual32) {
			for (uint32_t arg = 0; arg < 2u; ++arg) {
				uint32_t target;
				if (ImmediateU32(inst->Arg(arg), target) &&
				    SelectorMaximum(inst->Arg(arg ^ 1u), bounds, depth + 1u) < target) return true;
			}
		}
		return false;
	}

	uint32_t SelectorMaximum(Value value, SelectorBounds bounds = {}, uint32_t depth = 0) const {
		if (depth > 32u) return UINT32_MAX;
		value = value.Resolve();
		if (value.IsImmediate() && value.GetType() == Type::U32) return value.U32();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return UINT32_MAX;
		for (const auto& [phi, maximum]: bounds)
			if (inst == phi) return maximum;
		const auto maximum = [&](uint32_t arg) {
			return SelectorMaximum(inst->Arg(arg), bounds, depth + 1u);
		};
		switch (inst->GetOpcode()) {
			case ValueOpcode::FindILsb32:
				return NonzeroOnEntry(inst->Arg(0), inst->Parent()) ? 31u : UINT32_MAX;
			case ValueOpcode::UMin32:
			case ValueOpcode::BitwiseAnd32: return std::min(maximum(0), maximum(1));
			case ValueOpcode::SelectU32:
				return SelectorPredicateFalse(inst->Arg(0), bounds, depth + 1u)
				           ? maximum(2) : std::max(maximum(1), maximum(2));
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftLeftLogical32: {
				uint32_t shift;
				if (!ImmediateU32(inst->Arg(1), shift) || shift >= 32u) break;
				if (inst->GetOpcode() == ValueOpcode::ShiftRightLogical32) return maximum(0) >> shift;
				const auto result = uint64_t {maximum(0)} << shift;
				if (result <= UINT32_MAX) return static_cast<uint32_t>(result);
				break;
			}
			default: break;
		}
		return UINT32_MAX;
	}

	bool ImpossibleSwitchEdge(const Block* from, const Block* to) const {
		const auto& term = from->terminator;
		if (term.kind != CFG::TerminatorKind::IndirectBranch || !term.indexed) return false;
		const auto maximum = SelectorMaximum(from->indirect_target);
		bool found = false;
		for (const auto& branch: term.cases) {
			if (branch.target != to) continue;
			if (branch.value <= maximum) return false;
			found = true;
		}
		return found;
	}

	bool TryMakeFiniteDescriptor(Inst& handle, IndirectDescriptorPlan& plan) {
		const bool sampler = handle.GetOpcode() == ValueOpcode::GetSamplerResource;
		if ((!sampler && handle.GetOpcode() != ValueOpcode::GetImageResource) ||
		    handle.NumArgs() != (sampler ? 4u : 8u))
			return false;
		if (std::ranges::any_of(handle.Uses(), [&](const Use& use) {
			const auto op = use.user->GetOpcode();
			if (op != ValueOpcode::ImageSampleRaw) return sampler || op != ValueOpcode::ImageGatherRaw;
			return sampler && (m_program.memory_info[use.user->Flags<MemoryFlags>().index]
			                       .image_sample_flags & Decoder::ImageSampleFlagAdjust) != 0u;
		})) return false;
		bool has_phi = false;
		for (size_t word = 0; word < handle.NumArgs(); ++word) has_phi |= handle.Arg(word).Resolve().IsPhi();
		if (!has_phi) return false;
		// Normalize synchronized descriptor words to one GPU ordinal.
		// Build the complete graph before changing IR so an unsupported leaf is transactional.
		struct Choice {
			DescriptorSource descriptor;
			const Inst* branch = nullptr;
			std::vector<uint32_t> children;
			Value key;
		};
		std::vector<Choice> choices;
		const auto visit = [&](const auto& self, DescriptorSource descriptor) -> uint32_t {
			for (auto& word: descriptor.dwords) word = word.Resolve();
			for (uint32_t i = 0; i < choices.size(); ++i)
				if (choices[i].descriptor.dwords == descriptor.dwords) return i;
			const auto index = static_cast<uint32_t>(choices.size());
			choices.push_back({descriptor});
			uint32_t bad = 0;
			if (ValidateSource(descriptor, bad)) {
				// Finite inline samplers have literal leaves; live descriptor tables use the
				// existing direct sampler planning path.
				if (sampler && !std::ranges::all_of(
				    std::span(descriptor.dwords).first(descriptor.dword_count),
				    [](Value word) { return word.IsImmediate() && word.GetType() == Type::U32; }))
					return UINT32_MAX;
				return index;
			}
			const auto* branch = descriptor.dwords[bad].TryInstruction();
			if (branch == nullptr || branch->Parent() == nullptr ||
			    branch->GetOpcode() != ValueOpcode::Phi ||
			    branch->NumPhiBlocks() != branch->NumArgs()) return UINT32_MAX;
			choices[index].branch = branch;
			const auto count = branch->NumArgs();
			for (uint32_t arm = 0; arm < count; ++arm) {
				// A nonzero bit scan cannot choose its zero-input switch sentinel.
				// Preserve the Phi edge; its ordinal is never observed at runtime.
				if (ImpossibleSwitchEdge(branch->PhiBlock(arm), branch->Parent())) {
					choices[index].children.push_back(UINT32_MAX);
					continue;
				}
				auto child = descriptor;
				for (uint32_t word = 0; word < child.dword_count; ++word) {
					const auto* selected = descriptor.dwords[word].TryInstruction();
					if (selected == nullptr || selected->GetOpcode() != branch->GetOpcode() ||
					    selected->Parent() != branch->Parent()) {
						if (!ValidateRuntimeValue(m_program, descriptor.dwords[word])) return UINT32_MAX;
						continue;
					}
					if (selected->NumArgs() != count || selected->NumPhiBlocks() != count)
						return UINT32_MAX;
					uint32_t incoming = 0;
					while (incoming < count && selected->PhiBlock(incoming) != branch->PhiBlock(arm))
						++incoming;
					if (incoming == count) return UINT32_MAX;
					child.dwords[word] = selected->Arg(incoming);
				}
				const auto next = self(self, child);
				if (next == UINT32_MAX) return UINT32_MAX;
				choices[index].children.push_back(next);
			}
			return index;
		};
		DescriptorSource root;
		root.dword_count = handle.NumArgs();
		for (uint32_t word = 0; word < root.dword_count; ++word) root.dwords[word] = handle.Arg(word);
		if (visit(visit, root) == UINT32_MAX || choices.front().branch == nullptr) return false;
		DescriptorSource finite_source;
		finite_source.dword_count = root.dword_count;
		finite_source.dwords.fill(Value(0u));
		finite_source.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{});
		auto& sources = finite_source.indirect_descriptor->sources;
		for (auto& choice: choices) {
			if (choice.branch != nullptr) continue;
			const auto source = InternSource(choice.descriptor);
			auto found = std::ranges::find(sources, source);
			choice.key = Value(static_cast<uint32_t>(found - sources.begin()));
			if (found == sources.end()) sources.push_back(source);
		}
		if (sources.empty()) return false;
		for (auto& choice: choices) {
			if (choice.branch == nullptr) continue;
			auto* block = choice.branch->Parent();
			const auto where = std::ranges::find_if(block->Instructions(), [&](const Inst& inst) {
				return &inst == choice.branch;
			});
			auto& key = *block->PrependNewInst(where, choice.branch->GetOpcode());
			key.SetFlags(Type::U32);
			choice.key = Value(&key);
		}
		for (const auto& choice: choices) {
			if (choice.branch == nullptr) continue;
			auto* key = choice.key.Instruction();
			for (uint32_t arm = 0; arm < choice.children.size(); ++arm) {
				const auto child = choice.children[arm];
				const auto value = child == UINT32_MAX ? Value(0u) : choices[child].key;
				key->AddPhiOperand(choice.branch->PhiBlock(arm), value);
			}
		}
		plan.handle = &handle;
		plan.source = InternSource(finite_source);
		plan.key = choices.front().key;
		plan.roots = finite_source.dwords;
		plan.reads.fill(nullptr);
		return true;
	}

	const IndirectDescriptorPlan* FindIndirectDescriptor(const Inst& handle) const {
		const auto found =
		    std::find_if(m_indirect_descriptors.begin(), m_indirect_descriptors.end(),
		                 [&](const IndirectDescriptorPlan& plan) {
			    return plan.handle == &handle;
		    });
		return found == m_indirect_descriptors.end() ? nullptr : &*found;
	}

	bool IsIndirectPlanningMemory(uint32_t index) const {
		return std::any_of(m_indirect_descriptors.begin(), m_indirect_descriptors.end(),
		                   [&](const IndirectDescriptorPlan& plan) {
			return plan.reads[0] != nullptr &&
			       std::find(plan.memory.begin(), plan.memory.begin() + plan.handle->NumArgs(), index) !=
			           plan.memory.begin() + plan.handle->NumArgs();
		});
	}

	void PlanIndirectDescriptors() {
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if ((ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None &&
				     inst.GetOpcode() != ValueOpcode::StoreBufferU32) ||
				    inst.NumArgs() == 0u) {
					continue;
				}
				const auto flags = inst.Flags<MemoryFlags>();
				const auto& memory = m_program.memory_info[flags.index];
				const auto plan_handle = [&](Value value, uint32_t width, uint32_t base, bool sampler) {
					auto* handle = value.Resolve().TryInstruction();
					if (handle == nullptr || FindIndirectDescriptor(*handle) != nullptr) return;
					DescriptorSource descriptor;
					MakeSource(*handle, width, sampler, false, base, descriptor, flags.pc);
					uint32_t bad_dword = 0;
					if (ValidateSource(descriptor, bad_dword)) return;
					IndirectDescriptorPlan plan;
					if (sampler ? TryMakeFiniteDescriptor(*handle, plan)
					             : (TryMakeIndirectImage(*handle, descriptor, plan) ||
					                TryMakeFiniteDescriptor(*handle, plan) ||
					                TryMakeIndirectBuffer(*handle, descriptor, plan))) {
						for (const auto& previous: m_indirect_descriptors) {
							if (plan.reads[0] != nullptr && previous.reads[0] == plan.reads[0] &&
							    !EquivalentValue(m_program, previous.key, plan.key))
								Fail(flags.pc, "shared descriptor read has incompatible keys");
						}
						m_indirect_descriptors.push_back(std::move(plan));
					}
				};
				plan_handle(inst.Arg(0), inst.GetOpcode() == ValueOpcode::StoreBufferU32 ? 4u : 8u,
				            memory.resource * 4u, false);
				if (inst.GetOpcode() == ValueOpcode::ImageSampleRaw &&
				    (memory.image_sample_flags & Decoder::ImageSampleFlagAdjust) == 0u)
					plan_handle(inst.Arg(1), 4u, memory.sampler * 4u, true);
			}
		}
	}

	bool GetHandle(Value value, ValueOpcode expected, uint32_t width, uint32_t pc,
	               uint32_t base_reg, Inst*& handle, uint32_t& source, bool sampler = false,
	               bool sample_adjust = false) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != expected) {
			Fail(pc, fmt::format("memory operation requires {}", ValueOpcodeName(expected)));
		}
		DescriptorSource descriptor;
		MakeSource(*handle, width, sampler, sample_adjust, base_reg, descriptor, pc);
		uint32_t bad_dword = 0;
		if (!ValidateSource(descriptor, bad_dword)) {
			if (expected == ValueOpcode::GetBufferResource &&
			    std::all_of(descriptor.dwords.begin(), descriptor.dwords.begin() + width,
			                [](Value word) { return word.Resolve().GetType() == Type::U32; })) {
				return false;
			}
			Fail(pc, fmt::format("{} dword {} is not a valid runtime value",
			                     ValueOpcodeName(expected), bad_dword));
		}
		source = InternSource(descriptor);
		return true;
	}

	void ValidateAddressHandle(Value value, uint32_t pc) const {
		const auto* handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource) {
			Fail(pc, "address operation requires GetAddressResource");
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
		const bool atomic64 = ImageOpcodeInfoOf(op).access == ImageAccess::Atomic &&
		                      memory.data_bits == 64u;
		const bool dynamic_mip =
		    (resource_class == ImageResourceClass::Storage && memory.image_has_mip) ||
		    (op == ValueOpcode::ImageGatherRaw &&
		     (memory.image_sample_flags & Decoder::ImageSampleFlagLod) != 0u);
		const auto mip = dynamic_mip ? ImageMipMode::Dynamic : ImageMipMode::None;
		const bool depth = (memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0;
		for (uint32_t i = 0; i < m_info.images.size(); i++) {
			auto& image = m_info.images[i];
			if (image.source == source && image.resource_class == resource_class &&
			    image.dimension == memory.image_dimension && image.mip_mode == mip &&
			    image.depth_compare == depth && image.r128 == memory.image_r128 &&
			    image.atomic64 == atomic64) {
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
		image.atomic64       = atomic64;
		image.sample_only    = true;
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
		image.sample_only  = image.sample_only && op == ValueOpcode::ImageSampleRaw;
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
		m_info.samplers.push_back({source, pc});
		const auto root = static_cast<uint32_t>(m_info.samplers.size() - 1);
		const auto* descriptor = Source(source);
		if (descriptor != nullptr && descriptor->indirect_descriptor) {
			const auto& sources = descriptor->indirect_descriptor->sources;
			if (sources.empty()) Fail(pc, "finite sampler has no candidates");
			std::vector<uint32_t> resources {root};
			for (size_t i = 1; i < sources.size(); ++i) {
				const auto candidate = AddSampler(sources[i], pc);
				if (candidate == UINT32_MAX) return UINT32_MAX;
				resources.push_back(candidate);
			}
			m_info.samplers[root].indirect_resources = std::move(resources);
		}
		return root;
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
		const auto op           = inst.GetOpcode();
		if (op == ValueOpcode::BvhIntersect) {
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
		}
		if (inst.NumArgs() == 0) {
			Fail(flags.pc, "memory operation has no resource handle");
		}
		const auto& memory = m_program.memory_info[flags.index];
		if (memory.planning_only || IsIndirectPlanningMemory(flags.index)) {
			return;
		}
		Inst*    handle   = nullptr;
		uint32_t source   = 0;
		uint32_t resource = 0;

		if (buffer != BufferAccess::None) {
			handle = inst.Arg(0).Resolve().TryInstruction();
			const auto* indirect = handle == nullptr ? nullptr : FindIndirectDescriptor(*handle);
			if (indirect != nullptr) {
				source = indirect->source;
			} else if (!GetHandle(inst.Arg(0), ValueOpcode::GetBufferResource, 4, flags.pc,
			               memory.resource * 4u, handle, source)) {
				if (memory.kind != (op == ValueOpcode::ReadConstBuffer ? ResourceKind::ScalarBuffer
				                                                        : ResourceKind::Buffer) ||
				    !memory.SupportsIndirectBufferLoad(op)) {
					Fail(flags.pc,
					     "buffer descriptor is not a valid runtime value; GPU-selected access "
					     "requires a scalar, raw DWORD x1/x2/x3/x4, or formatted X load");
				}
				m_program.memory_info[flags.index].kind = ResourceKind::IndirectBuffer;
				m_info.uses_dma                         = true;
				return;
			}
			resource = AddBuffer(source, memory, op, flags.pc);
			if (resource == UINT32_MAX) {
				Fail(flags.pc, "buffer resource limit exceeded");
			}
			AddHandlePatch(handle, resource, flags.pc);
			AddMemoryPatch(flags.index, resource, 0, false, flags.pc);
			return;
		}
		if (address_info.access != AddressAccess::None) {
			if (!IsAddressResourceKind(memory.kind)) {
				Fail(flags.pc, "address operation has invalid resource kind");
			}
			if (memory.kind == ResourceKind::Flat && memory.address_is_full &&
			    IsNoncanonicalFlatAddress(inst.Arg(2), inst.Arg(inst.NumArgs() - 1))) {
				ValidateAddressHandle(inst.Arg(0), flags.pc);
				if (memory.data_bits != 32u || m_program.scratch_dwords == 0) {
					Fail(flags.pc, "local FLAT access requires DWORD data and per-thread scratch storage");
				}
				m_program.memory_info[flags.index].kind = ResourceKind::FlatLocal;
				return;
			}
			if (memory.kind == ResourceKind::Scratch) {
				handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetScratchResource ||
				    handle->NumArgs() != 0) {
					Fail(flags.pc, "scratch operation requires GetScratchResource");
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
		}
		handle               = inst.Arg(0).Resolve().TryInstruction();
		const auto* indirect = handle != nullptr ? FindIndirectDescriptor(*handle) : nullptr;
		if (indirect != nullptr) {
			source = indirect->source;
		} else {
			GetHandle(inst.Arg(0), ValueOpcode::GetImageResource, 8, flags.pc,
			          memory.resource * 4u, handle, source);
		}
		resource = AddImage(source, memory, op, flags.pc);
		if (resource == UINT32_MAX) {
			Fail(flags.pc, "image resource limit exceeded");
		}
		AddHandlePatch(handle, resource, flags.pc);
		uint32_t sampler = 0;
		if (image_info.needs_sampler) {
			if (inst.NumArgs() < 2) {
				Fail(flags.pc, "sampled image operation has no sampler handle");
			}
			Inst*      sampler_handle = nullptr;
			uint32_t   sampler_source = 0;
			const bool sample_adjust =
			    (memory.image_sample_flags & Decoder::ImageSampleFlagAdjust) != 0;
			sampler_handle = inst.Arg(1).Resolve().TryInstruction();
			const auto* selected_sampler = sampler_handle != nullptr
			    ? FindIndirectDescriptor(*sampler_handle) : nullptr;
			if (selected_sampler != nullptr) {
				sampler_source = selected_sampler->source;
			} else {
				GetHandle(inst.Arg(1), ValueOpcode::GetSamplerResource, 4, flags.pc,
				          memory.sampler * 4u, sampler_handle, sampler_source, true, sample_adjust);
			}
			sampler = AddSampler(sampler_source, flags.pc);
			if (sampler == UINT32_MAX) {
				Fail(flags.pc, "sampler resource limit exceeded");
			}
			m_info.samplers[sampler].gather_lod |=
			    op == ValueOpcode::ImageGatherRaw &&
			    (memory.image_sample_flags & Decoder::ImageSampleFlagLod) != 0u;
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
				    image_source->indirect_descriptor.has_value()) {
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
	std::vector<const Inst*>                   m_srt_visiting;
	std::vector<const Inst*>                   m_srt_visited;
	std::vector<Inst*>                         m_scalar_reads;
	ShaderInfo                                 m_info;
	std::vector<DescriptorSource>              m_sources;
	std::vector<HandlePatch>                   m_handle_patches;
	std::vector<MemoryPatch>                   m_memory_patches;
	std::vector<IndirectDescriptorPlan>             m_indirect_descriptors;
	std::map<const Inst*, Value>              m_descriptor_values;
	bool                                       m_shader_writes = false;
};

} // namespace

void TrackResources(Program& program, const Decoder::Program& decoded, const CFG::Graph& native_cfg) {
	Tracker(program, decoded, native_cfg).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
