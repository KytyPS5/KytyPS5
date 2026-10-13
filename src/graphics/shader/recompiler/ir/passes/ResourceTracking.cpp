#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {
std::atomic<int> g_bindless_override {-1};
} // namespace

bool BindlessImageHeapsEnabled() {
	return g_bindless_override.load(std::memory_order_relaxed) != 0;
}

void OverrideBindlessImageHeaps(int enabled) {
	g_bindless_override.store(enabled < 0 ? -1 : (enabled != 0 ? 1 : 0), std::memory_order_relaxed);
}

namespace {

constexpr uint32_t SamplerBorderClampMask    = (1u << 2u) | (1u << 5u) | (1u << 8u);
constexpr uint32_t SamplerDword3ReservedMask = 0x3ffff000u;

std::optional<uint32_t> ConstantScalarWrite(const Decoder::Instruction& inst) {
	const auto constant = [](const Decoder::Operand& operand) {
		return operand.kind == Decoder::OperandKind::LiteralConstant ||
		       operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
		       operand.kind == Decoder::OperandKind::FloatInlineConstant;
	};
	switch (inst.opcode) {
		case Decoder::Opcode::S_MOV_B32:
		case Decoder::Opcode::S_MOVK_I32:
			if (constant(inst.src0)) return inst.src0.value;
			break;
		case Decoder::Opcode::S_BFM_B32:
			// D = ((1 << S0[4:0]) - 1) << S1[4:0]
			if (constant(inst.src0) && constant(inst.src1)) {
				return ((1u << (inst.src0.value & 31u)) - 1u) << (inst.src1.value & 31u);
			}
			break;
		default: break;
	}
	return std::nullopt;
}

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

// Every value `value & mask` can take, when the expression proves a handful of them.
bool PossibleMaskedValues(Value value, uint32_t mask, std::vector<uint32_t>& out,
                          uint32_t depth = 0) {
	constexpr size_t MaxValues = 8;
	const auto       add       = [&](uint32_t candidate) {
		if (std::ranges::find(out, candidate) == out.end()) {
			out.push_back(candidate);
		}
		return out.size() <= MaxValues;
	};
	value = value.Resolve();
	if (mask == 0) {
		return add(0);
	}
	if (value.IsImmediate()) {
		return value.GetType() == Type::U32 && add(value.U32() & mask);
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || depth > 16) {
		return false;
	}
	const auto immediate = [&](size_t index, uint32_t& result) {
		const auto arg = inst->Arg(index).Resolve();
		if (!arg.IsImmediate() || arg.GetType() != Type::U32) {
			return false;
		}
		result = arg.U32();
		return true;
	};
	const auto combine = [&](Value a, uint32_t mask_a, Value b, uint32_t mask_b, auto&& op) {
		std::vector<uint32_t> left;
		std::vector<uint32_t> right;
		if (!PossibleMaskedValues(a, mask_a, left, depth + 1) ||
		    !PossibleMaskedValues(b, mask_b, right, depth + 1)) {
			return false;
		}
		for (const auto x: left) {
			for (const auto y: right) {
				if (!add(op(x, y) & mask)) {
					return false;
				}
			}
		}
		return true;
	};
	const auto through = [&](Value source, uint32_t source_mask, auto&& map) {
		std::vector<uint32_t> values;
		if (!PossibleMaskedValues(source, source_mask, values, depth + 1)) {
			return false;
		}
		return std::ranges::all_of(values, [&](uint32_t x) { return add(map(x) & mask); });
	};
	uint32_t a = 0;
	uint32_t b = 0;
	switch (inst->GetOpcode()) {
		case ValueOpcode::SelectU32:
			return PossibleMaskedValues(inst->Arg(1), mask, out, depth + 1) &&
			       PossibleMaskedValues(inst->Arg(2), mask, out, depth + 1);
		case ValueOpcode::BitwiseOr32:
			return combine(inst->Arg(0), mask, inst->Arg(1), mask,
			               [](uint32_t x, uint32_t y) { return x | y; });
		case ValueOpcode::BitwiseAnd32:
			if (immediate(1, a)) {
				return through(inst->Arg(0), mask & a, [](uint32_t x) { return x; });
			}
			return combine(inst->Arg(0), mask, inst->Arg(1), mask,
			               [](uint32_t x, uint32_t y) { return x & y; });
		case ValueOpcode::ShiftLeftLogical32:
			if (!immediate(1, a)) {
				return false;
			}
			a &= 31u;
			return through(inst->Arg(0), mask >> a, [a](uint32_t x) { return x << a; });
		case ValueOpcode::ShiftRightLogical32:
			if (!immediate(1, a)) {
				return false;
			}
			a &= 31u;
			return through(inst->Arg(0), mask << a, [a](uint32_t x) { return x >> a; });
		case ValueOpcode::BitFieldUExtract: {
			if (!immediate(1, a) || !immediate(2, b) || a >= 32u || b == 0u || b > 32u - a) {
				return false;
			}
			const uint32_t field = b == 32u ? UINT32_MAX : (1u << b) - 1u;
			return through(inst->Arg(0), (mask & field) << a, [a](uint32_t x) { return x >> a; });
		}
		case ValueOpcode::BitFieldInsert: {
			if (!immediate(2, a) || !immediate(3, b) || a >= 32u || b == 0u || b > 32u - a) {
				return false;
			}
			const uint32_t field = (b == 32u ? UINT32_MAX : (1u << b) - 1u) << a;
			return combine(
			    inst->Arg(0), mask & ~field, inst->Arg(1), (mask & field) >> a,
			    [a, field](uint32_t x, uint32_t y) { return (x & ~field) | ((y << a) & field); });
		}
		default: return false;
	}
}

bool IsNoncanonicalFlatAddress(Value high, Value active) {
	high   = high.Resolve();
	active = active.Resolve();
	// A predicated VGPR write supplies its new value on exactly the lanes which
	// execute this access. Do not discard a selection under a different mask.
	while (const auto* select = high.TryInstruction()) {
		if (select->GetOpcode() != ValueOpcode::SelectU32 || select->Arg(0).Resolve() != active)
			break;
		high = select->Arg(1).Resolve();
	}
	// The aperture match reads address bits [63:48] only, so a high DWORD whose top half is
	// proven to be an aperture tag is local whatever its low half holds.
	std::vector<uint32_t> tags;
	if (PossibleMaskedValues(high, 0xffff0000u, tags) && !tags.empty() &&
	    std::ranges::all_of(tags, [](uint32_t tag) {
		    return tag == (Decoder::PrivateApertureHigh & 0xffff0000u) ||
		           tag == (Decoder::SharedApertureHigh & 0xffff0000u);
	    })) {
		return true;
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
			if (count++ == 0)
				lower = upper = value.U32();
			else {
				lower = std::min(lower, value.U32());
				upper = std::max(upper, value.U32());
			}
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
			if ((PossibleU32Bits(value) & ~SamplerDword3ReservedMask) == 0) return Value(0u);
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

bool RelativeWriteDecode() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_NO_RELATIVE_WRITE_DECODE");
		return text == nullptr || text[0] == '\0' || std::strcmp(text, "0") == 0;
	}();
	return enabled;
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
		uint32_t low      = 0;
		uint32_t high     = 0;
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
		const auto variable =
		    std::min({m_nodes[condition].variable, m_nodes[yes].variable, m_nodes[no].variable});
		const auto arm = [&](uint32_t value, bool high) {
			const auto node = m_nodes[value];
			return node.variable == variable ? (high ? node.high : node.low) : value;
		};
		const auto low    = Select(arm(condition, false), arm(yes, false), arm(no, false));
		const auto high   = Select(arm(condition, true), arm(yes, true), arm(no, true));
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
		if (const auto found = m_predicates.find(key); found != m_predicates.end())
			return found->second;
		const auto variable = std::min(m_nodes[left].variable, m_nodes[right].variable);
		uint32_t   result;
		if (variable == UINT32_MAX) {
			result = Unknown(Type::U1);
		} else {
			const auto lhs  = m_nodes[left];
			const auto rhs  = m_nodes[right];
			const auto low  = Compare(inst, lhs.variable == variable ? lhs.low : left,
			                          rhs.variable == variable ? rhs.low : right);
			const auto high = Compare(inst, lhs.variable == variable ? lhs.high : left,
			                          rhs.variable == variable ? rhs.high : right);
			result          = Select(NodeFor(variable, 0u, 1u), high, low);
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
		auto       result = UINT32_MAX;
		const auto arg    = [&](uint32_t index) { return Evaluate(inst->Arg(index), current); };
		switch (inst->GetOpcode()) {
			case ValueOpcode::Phi: {
				const auto invariant = ResolveInvariantPhi(m_program, value);
				if (!invariant.IsEmpty()) {
					result = Evaluate(invariant, current);
				} else if (inst->Parent() == m_induction.Parent()) {
					if (current) {
						for (uint32_t i = 0; i < inst->NumArgs(); ++i) {
							if (inst->PhiBlock(i) == m_incoming)
								result = Evaluate(inst->Arg(i), false);
						}
					}
				} else if (inst->NumArgs() != 0u) {
					result = arg(0);
					for (uint32_t i = 1; i < inst->NumArgs(); ++i) {
						if (arg(i) != result) {
							result = UINT32_MAX;
							break;
						}
					}
				}
				break;
			}
			case ValueOpcode::LogicalNot: result = Select(arg(0), 0u, 1u); break;
			case ValueOpcode::LogicalAnd: {
				const auto left = arg(0);
				result          = left == 0u ? 0u : Select(left, arg(1), 0u);
				break;
			}
			case ValueOpcode::LogicalOr: {
				const auto left = arg(0);
				result          = left == 1u ? 1u : Select(left, 1u, arg(1));
				break;
			}
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectU32: {
				const auto condition = arg(0);
				result               = condition == 0u   ? arg(2)
				                       : condition == 1u ? arg(1)
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

	const Program&                                                            m_program;
	const Inst&                                                               m_induction;
	const Inst&                                                               m_bound;
	const Block*                                                              m_incoming  = nullptr;
	uint32_t                                                                  m_variables = 0;
	std::vector<Node>                                                         m_nodes {{}, {}};
	std::vector<std::pair<Value, uint32_t>>                                   m_literals;
	std::array<std::map<const Inst*, uint32_t>, 2>                            m_values;
	std::map<std::array<uint32_t, 3>, uint32_t>                               m_nodes_by_key;
	std::map<std::array<uint32_t, 3>, uint32_t>                               m_choices;
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
		m_info.uses_dma   = false;
		m_info.writes_dma = false;
		m_info.dma_pointers.clear();
		m_shader_writes = HasShaderMemoryWrites(program);
	}

	ResourceTrackingStatus Run() {
		if (m_program.resource_tracking_complete) {
			Fail(0, "resources already tracked");
		}
		FoldBoundedLoopSelectors();
		PlanScalarReads();
		EliminateDeadCode(m_program.blocks);
		m_cyclic_entries.program = &m_program;
		PlanIndirectDescriptors();
		PlanIndirectBuffers();
		DemoteRefusedTables();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (!Collect(inst)) {
					return std::move(m_status);
				}
			}
		}
		LinkImageAliases();
		m_cyclic_entries = {};
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
			// A GPU-selected inline sampler: its words collapse onto the candidate ordinal.
			if (plan.handle->NumArgs() == 4u) {
				plan.handle->SetArg(0, plan.key);
				for (uint32_t word = 1; word < 4u; ++word) {
					plan.handle->SetArg(word, Value(0u));
				}
				continue;
			}
			// A handle reached through a phi keeps it; the projection below rewrites its arms.
			if (plan.reads[0] == nullptr ||
			    plan.handle->Arg(0).Resolve().TryInstruction() == plan.reads[0]) {
				plan.handle->SetArg(0, plan.key);
			}
			for (uint32_t dword = 0; dword < 4u; dword++) {
				plan.handle->SetArg(dword + 1u, plan.roots[dword + 4u]);
			}
			for (uint32_t dword = 5u; dword < plan.roots.size(); dword++) {
				plan.handle->SetArg(dword, plan.key);
			}
			if (plan.reads[0] != nullptr) {
				for (uint32_t dword = 0; dword < plan.memory.size(); dword++) {
					if (!plan.kept[dword]) {
						m_program.memory_info[plan.memory[dword]].planning_only = true;
					}
				}
			}
		}
		// Project descriptor-only SSA onto its key. Existing Phis preserve dominance;
		// native descriptor resolution excludes their stale incoming values at each use.
		for (const auto& plan: m_indirect_descriptors) {
			if (plan.reads[0] == nullptr) {
				continue;
			}
			for (uint32_t dword = 0; dword < plan.reads.size(); dword++) {
				if (plan.kept[dword]) {
					continue;
				}
				const auto* read  = plan.reads[dword];
				const auto  owner = std::ranges::find_if(
				    m_indirect_descriptors, [&](const IndirectDescriptorPlan& candidate) {
					    return candidate.reads[0] == read;
				    });
				const auto key  = owner != m_indirect_descriptors.end() ? owner->key : Value(0u);
				const auto uses = read->Uses();
				for (const auto& use: uses) {
					use.user->SetArg(use.operand, key);
				}
			}
		}
		// Only the tables this pass actually bound. A DWORD x2/x3/x4 access over the same shape
		// goes to the in-shader V# decode instead, and that mechanism reads the descriptor
		// dwords straight off the handle - rewriting arg 0 there replaces the base address low
		// word with a record index and every load returns zero.
		for (const auto& plan: m_indirect_buffers) {
			if (m_applied_indirect_buffers.contains(plan.handle)) {
				plan.handle->SetArg(0, plan.key);
			}
		}
		m_program.descriptor_sources         = std::move(m_sources);
		m_program.info                       = std::move(m_info);
		m_program.resource_tracking_complete = true;
		return m_status;
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
		const Inst*      handle;
		uint32_t         pc;
		DescriptorSource source;
		Value            planning_handle;
	};

	struct IndirectDescriptorPlan {
		Inst*                      handle = nullptr;
		uint32_t                   source = 0;
		Value                      key;
		std::array<Value, 8>       roots {};
		std::array<uint32_t, 8>    memory {};
		std::array<const Inst*, 8> reads {};
		std::array<bool, 8>        kept {};
	};

	// GetBufferResource is emitted once per access, so one recognized table usually owns several
	// handles. Each handle carries the interned source; the reads themselves stay in the IR.
	struct IndirectBufferPlan {
		Inst*    handle = nullptr;
		uint32_t source = 0;
		Value    key;
	};

	struct MemoryAccessors {
		uint32_t    count = 0;
		const Inst* only  = nullptr;
	};

	// An emulator invariant: the IR reached tracking in a shape this pass must never see.
	[[noreturn]] void Fail(uint32_t pc, const std::string& reason) const {
		const auto message =
		    fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
		                m_program.shader_hash, StageName(m_program.stage), pc, reason);
		EXIT("%s", message.c_str());
		std::abort();
	}

	Value NativeDescriptorSource(Value value, uint32_t reg, uint32_t use_pc) const {
		value           = value.Resolve();
		const auto* phi = value.TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi || m_native_cfg.blocks.empty())
			return value;
		std::vector<const Inst*> candidates;
		std::vector<Value>       constants;
		std::vector<const Inst*> visited;
		std::vector<const Inst*> pending {phi};
		while (!pending.empty()) {
			const auto* inst = pending.back();
			pending.pop_back();
			if (std::ranges::find(visited, inst) != visited.end()) continue;
			visited.push_back(inst);
			if (inst->GetOpcode() == ValueOpcode::Phi) {
				for (size_t i = 0; i < inst->NumArgs(); ++i) {
					const auto  operand = inst->Arg(i).Resolve();
					const auto* arg     = operand.TryInstruction();
					if (arg != nullptr) {
						pending.push_back(arg);
					} else if (operand.IsImmediate() && operand.GetType() == Type::U32) {
						constants.push_back(operand);
					}
				}
			} else if (inst->GetOpcode() == ValueOpcode::ReadConst ||
			           inst->GetOpcode() == ValueOpcode::LoadAddressU32 ||
			           inst->GetOpcode() == ValueOpcode::ReadConstBuffer ||
			           inst->GetOpcode() == ValueOpcode::GetUserData) {
				candidates.push_back(inst);
			}
		}
		if (candidates.empty() && constants.empty()) return value;
		const auto source_at = [&](uint32_t pc) {
			Value      source;
			const auto native =
			    std::ranges::lower_bound(m_decoded.instructions, pc, {}, &Decoder::Instruction::pc);
			for (const auto* candidate: candidates) {
				if (pc == UINT32_MAX) {
					if (candidate->GetOpcode() != ValueOpcode::GetUserData ||
					    RegIndex(candidate->Arg(0).ScalarRegister()) != reg)
						continue;
				} else {
					if (candidate->GetOpcode() != ValueOpcode::ReadConst &&
					    candidate->GetOpcode() != ValueOpcode::LoadAddressU32 &&
					    candidate->GetOpcode() != ValueOpcode::ReadConstBuffer)
						continue;
					const auto flags = candidate->Flags<MemoryFlags>();
					if (flags.pc != pc || flags.index >= m_program.memory_info.size()) continue;
					if (native == m_decoded.instructions.end() || native->pc != pc ||
					    native->dst.kind != Decoder::OperandKind::Sgpr ||
					    native->dst.reg + m_program.memory_info[flags.index].component_index != reg)
						continue;
				}
				const Value current(const_cast<Inst*>(candidate));
				if (!source.IsEmpty() && !EquivalentValue(m_program, source, current))
					return Value {};
				source = current;
			}
			if (source.IsEmpty() && pc != UINT32_MAX && native != m_decoded.instructions.end() &&
			    native->pc == pc && native->dst.kind == Decoder::OperandKind::Sgpr &&
			    native->dst.reg == reg) {
				const auto written  = ConstantScalarWrite(*native);
				const auto constant = std::ranges::find_if(constants, [&](Value leaf) {
					return written.has_value() && leaf.U32() == *written;
				});
				if (constant != constants.end()) source = *constant;
			}
			return source;
		};
		const auto use = std::ranges::find_if(m_native_cfg.blocks, [&](const auto& block) {
			return block.start_pc <= use_pc && use_pc < block.end_pc;
		});
		if (use == m_native_cfg.blocks.end()) return value;
		struct Position {
			uint32_t block;
			uint32_t before;
		};
		std::vector<Position> positions {{use->id, use_pc}};
		std::vector<bool>     reached(m_native_cfg.blocks.size());
		Value                 selected;
		uint32_t              selected_pc = UINT32_MAX;
		const auto            select      = [&](uint32_t pc) {
			const auto source = source_at(pc);
			if (source.IsEmpty()) return false;
			if (!selected.IsEmpty()) {
				const auto op =
				    source.IsImmediate() ? ValueOpcode::Void : source.TryInstruction()->GetOpcode();
				if ((op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) &&
				    selected_pc != pc)
					return false;
				if (!EquivalentValue(m_program, selected, source)) return false;
			}
			selected    = source;
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

	// Iterative: descriptor dwords can sit on SSA chains tens of thousands of values deep, which
	// would overflow the native stack if lowered recursively.
	Value LowerDescriptorValue(Value value) {
		struct Frame {
			const Inst*                            inst;
			std::map<const Inst*, Value>::iterator cached;
			size_t                                 arg       = 0;
			Inst*                                  projected = nullptr;
		};
		std::vector<Frame> stack;
		Value              lowered;
		// Returns true with `lowered` set, or pushes a frame whose operands must be lowered first.
		const auto enter = [&](Value operand) {
			operand          = operand.Resolve();
			const auto* inst = operand.TryInstruction();
			lowered          = operand;
			if (inst == nullptr) return true;
			// The scalar planner owns reads and lowers their address handles separately.
			uint32_t memory_index = 0;
			if (ScalarReadMemory(*inst, memory_index) != nullptr) return true;
			const auto [cached, inserted] = m_descriptor_values.emplace(inst, operand);
			if (!inserted) {
				lowered = cached->second;
				return true;
			}
			if (inst->GetOpcode() == ValueOpcode::Phi) {
				cached->second = LowerDescriptorPhi(*inst, operand);
				lowered        = cached->second;
				return true;
			}
			stack.push_back({inst, cached});
			return false;
		};
		if (enter(value)) return lowered;
		bool pending = false;
		for (;;) {
			auto&       frame = stack.back();
			const auto* inst  = frame.inst;
			if (pending) {
				pending = false;
				if (lowered != inst->Arg(frame.arg).Resolve()) {
					if (frame.projected == nullptr) {
						frame.projected = &m_program.value_storage.emplace_back(
						    inst->GetOpcode(), inst->Flags<uint64_t>());
						for (size_t i = 0; i < inst->NumArgs(); ++i)
							frame.projected->SetArg(i, inst->Arg(i));
					}
					frame.projected->SetArg(frame.arg, lowered);
				}
				++frame.arg;
			}
			if (frame.arg < inst->NumArgs()) {
				// A pushed frame invalidates `frame` and lowers its own operands first.
				pending = enter(inst->Arg(frame.arg));
				continue;
			}
			if (frame.projected != nullptr) frame.cached->second = Value(frame.projected);
			lowered = frame.cached->second;
			stack.pop_back();
			if (stack.empty()) return lowered;
			pending = true;
		}
	}

	Value LowerDescriptorPhi(const Inst& phi, Value value) {
		const auto* inst = &phi;
		if (inst->NumArgs() != 2u || inst->NumPhiBlocks() != 2u || inst->GetType() != Type::U32) {
			return value;
		}
		const auto* merge  = inst->Parent();
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
			    (incoming != branch && (incoming->ImmPredecessors().size() != 1u ||
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
		    !ValidateRuntimeValue(m_program, info.condition,
		                          m_shader_writes ? RuntimeValueType::ImmutableInteger
		                                          : RuntimeValueType::Integer,
		                          nullptr, &m_cyclic_entries) ||
		    !ValidateRuntimeValue(m_program, inst->Arg(0), RuntimeValueType::Any, nullptr,
		                          &m_cyclic_entries) ||
		    !ValidateRuntimeValue(m_program, inst->Arg(1), RuntimeValueType::Any, nullptr,
		                          &m_cyclic_entries)) {
			return value;
		}
		// Retain a host expression; replacing the GPU Phi would break SSA dominance.
		const auto true_arg = term.true_block == targets[0] ? 0u : 1u;
		auto&      selected = m_program.value_storage.emplace_back(ValueOpcode::SelectU32);
		selected.SetArg(0, info.condition);
		selected.SetArg(1, inst->Arg(true_arg));
		selected.SetArg(2, inst->Arg(true_arg ^ 1u));
		return Value(&selected);
	}

	// A guest shader this pass cannot model. Records the first reason and unwinds, leaving the
	// program untracked so the caller can drop the draw instead of terminating the process.
	bool Reject(uint32_t pc, const std::string& reason) const {
		if (m_status.ok) {
			m_status.ok     = false;
			m_status.pc     = pc;
			m_status.reason = reason;
		}
		return false;
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
			const auto value     = base_reg != UINT32_MAX
			                           ? NativeDescriptorSource(handle.Arg(i), base_reg + i, pc)
			                           : handle.Arg(i);
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
		           ? native->src0.reg
		           : UINT32_MAX;
	}

	// Iterative: slices run thousands of instructions deep. Post-order fixes m_scalar_reads slots.
	void CollectScalarRead(Value root, uint32_t root_pc) {
		struct Frame {
			Inst*                                   inst;
			bool                                    hoist;
			std::vector<std::pair<Value, uint32_t>> operands;
			size_t                                  next;
		};
		std::vector<Frame> stack;
		const auto         enter = [&](Value value, uint32_t use_pc) {
			value = value.Resolve();
			if (value.IsImmediate()) return;
			auto* inst = value.TryInstruction();
			if (inst == nullptr) Fail(use_pc, "invalid typed planning value");
			const auto cycle = std::ranges::find(m_srt_visiting, inst);
			if (cycle != m_srt_visiting.end()) {
				if (std::any_of(cycle, m_srt_visiting.end(), [](const Inst* value) {
					    return value->GetOpcode() == ValueOpcode::Phi;
				    }))
					return;
				Fail(use_pc, "cyclic typed planning value without a phi");
			}
			if (m_srt_visited.contains(inst)) return;
			m_srt_visiting.push_back(inst);
			uint32_t    memory_index = 0;
			const auto* memory       = ScalarReadMemory(*inst, memory_index);
			Frame       frame {inst, false, {}, 0};
			if (memory != nullptr) {
				const auto* handle = inst->Arg(0).Resolve().TryInstruction();
				const auto  width  = memory->kind == ResourceKind::ScalarBuffer ? 4u : 2u;
				if (handle == nullptr ||
				    handle->GetOpcode() != (width == 4u ? ValueOpcode::GetBufferResource
				                                        : ValueOpcode::GetAddressResource))
					Fail(use_pc, "scalar read has an invalid resource handle");
				DescriptorSource source;
				MakeSource(*handle, width, false, false, ScalarReadBase(*inst), source,
				           inst->Flags<MemoryFlags>().pc);
				const auto offset = inst->Arg(1).Resolve();
				// A constant offset is not enough to hoist: a flattened slot holds one word for the
				// whole dispatch, and a descriptor carried around a loop names a different record
				// every iteration.
				frame.hoist = offset.IsImmediate() && offset.GetType() == Type::U32 &&
				              !ReadsThroughCarriedDescriptor(*inst, source);
				for (uint32_t word = 0; word < width; ++word)
					frame.operands.emplace_back(source.dwords[word], inst->Flags<MemoryFlags>().pc);
				for (size_t arg = 1; arg < inst->NumArgs(); ++arg)
					frame.operands.emplace_back(inst->Arg(arg), use_pc);
			} else {
				for (size_t arg = 0; arg < inst->NumArgs(); ++arg)
					frame.operands.emplace_back(inst->Arg(arg), use_pc);
			}
			stack.push_back(std::move(frame));
		};
		enter(root, root_pc);
		while (!stack.empty()) {
			auto& frame = stack.back();
			if (frame.next < frame.operands.size()) {
				const auto [value, use_pc] = frame.operands[frame.next++];
				enter(value, use_pc);
				continue;
			}
			m_srt_visiting.pop_back();
			m_srt_visited.insert(frame.inst);
			if (frame.hoist) m_scalar_reads.push_back(frame.inst);
			stack.pop_back();
		}
	}

	// True when the descriptor a raw read goes through is carried around a loop, so the record it
	// names differs per iteration. An invariant phi folds to one value and names one record.
	bool CarriedValue(Value value, uint32_t depth, std::vector<const Inst*>& seen) const {
		constexpr uint32_t MaxCarriedDepth = 24;
		if (depth > MaxCarriedDepth) {
			return false;
		}
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || std::ranges::find(seen, inst) != seen.end()) {
			return false;
		}
		seen.push_back(inst);
		if (inst->GetOpcode() == ValueOpcode::Phi) {
			return ResolveInvariantPhi(m_program, value).IsEmpty();
		}
		for (uint32_t arg = 0; arg < inst->NumArgs(); arg++) {
			if (CarriedValue(inst->Arg(arg), depth + 1, seen)) {
				return true;
			}
		}
		return false;
	}

	// Judged on the descriptor as resolved to its native source: a join the native control flow
	// settles is one descriptor, while one carried around a loop stays a phi.
	bool ReadsThroughCarriedDescriptor(const Inst& read, const DescriptorSource& source) const {
		// Only a scalar buffer read is left to the shader: its descriptor is a V# the indirect
		// table machinery binds per record, so reading it in place is served. A raw address read
		// has no such path - un-flattening one only costs the tracker the descriptor it resolves
		// today, so those keep the existing behaviour.
		if (read.GetOpcode() != ValueOpcode::ReadConstBuffer) {
			return false;
		}
		for (uint32_t word = 0; word < source.dword_count; word++) {
			std::vector<const Inst*> seen;
			if (CarriedValue(source.dwords[word], 0, seen)) {
				return true;
			}
		}
		return false;
	}

	struct HostEvaluated {
		std::unordered_set<const Inst*> strict;
		std::unordered_set<const Inst*> decodable;
	};

	bool RelativeStoreHandle(const Inst& handle, DmaPointer* pointer = nullptr) const {
		const auto op    = handle.GetOpcode();
		const auto words = op == ValueOpcode::GetBufferResource    ? 4u
		                   : op == ValueOpcode::GetAddressResource ? 2u
		                                                           : 0u;
		if (words == 0u || handle.NumArgs() < words) {
			return false;
		}
		const auto user_reg = [](const Inst* inst, uint32_t& reg) {
			if (inst == nullptr || inst->GetOpcode() != ValueOpcode::GetUserData ||
			    inst->Arg(0).GetType() != Type::ScalarReg) {
				return false;
			}
			reg = RegIndex(inst->Arg(0).ScalarRegister());
			return true;
		};
		uint32_t   base_lo       = UINT32_MAX;
		uint32_t   base_hi       = UINT32_MAX;
		const auto record_offset = [&](const Inst& load, int64_t& offset) {
			uint32_t    index  = 0;
			const auto* memory = ScalarReadMemory(load, index);
			const auto* record = load.Arg(0).Resolve().TryInstruction();
			const auto  extra  = load.Arg(1).Resolve();
			uint32_t    lo     = UINT32_MAX;
			uint32_t    hi     = UINT32_MAX;
			if (memory == nullptr || record == nullptr ||
			    record->GetOpcode() != ValueOpcode::GetAddressResource || record->NumArgs() != 2u ||
			    !user_reg(record->Arg(0).Resolve().TryInstruction(), lo) ||
			    !user_reg(record->Arg(1).Resolve().TryInstruction(), hi) || !extra.IsImmediate() ||
			    extra.GetType() != Type::U32) {
				return false;
			}
			if (base_lo == UINT32_MAX) {
				base_lo = lo;
				base_hi = hi;
			} else if (lo != base_lo || hi != base_hi) {
				return false;
			}
			offset = (static_cast<int64_t>(static_cast<int32_t>(memory->offset)) & ~int64_t {3}) +
			         static_cast<int64_t>(extra.U32() & ~3u);
			return true;
		};
		struct Leaves {
			std::vector<uint32_t> regs;
			std::vector<int64_t>  loads;
		};
		const auto collect = [&](Value root, Leaves& out) {
			std::vector<const Inst*>        pending;
			std::unordered_set<const Inst*> seen;
			const auto                      visit = [&](Value value) {
				value = value.Resolve();
				if (value.IsImmediate()) {
					return true;
				}
				const auto* inst = value.TryInstruction();
				if (inst == nullptr) {
					return false;
				}
				pending.push_back(inst);
				return true;
			};
			if (!visit(root)) {
				return false;
			}
			while (!pending.empty()) {
				const auto* inst = pending.back();
				pending.pop_back();
				if (!seen.insert(inst).second) {
					continue;
				}
				if (seen.size() > 128u) {
					return false;
				}
				const auto inst_op = inst->GetOpcode();
				if (inst_op == ValueOpcode::GetUserData) {
					uint32_t reg = 0;
					if (!user_reg(inst, reg)) {
						return false;
					}
					out.regs.push_back(reg);
					continue;
				}
				if (inst_op == ValueOpcode::LoadAddressU32) {
					int64_t offset = 0;
					if (!record_offset(*inst, offset)) {
						return false;
					}
					out.loads.push_back(offset);
					continue;
				}
				// Only arithmetic on those leaves: anything else could move the base anywhere.
				if (inst_op == ValueOpcode::Phi || inst_op == ValueOpcode::ReadConst ||
				    inst_op == ValueOpcode::ReadConstBuffer ||
				    BufferAccessOf(inst_op) != BufferAccess::None ||
				    AddressOpcodeInfoOf(inst_op).access != AddressAccess::None) {
					return false;
				}
				for (uint32_t arg = 0; arg < inst->NumArgs(); ++arg) {
					if (!visit(inst->Arg(arg))) {
						return false;
					}
				}
			}
			return true;
		};
		Leaves low;
		Leaves high;
		if (!collect(handle.Arg(0), low) || !collect(handle.Arg(1), high) || low.loads.empty() ||
		    base_lo == UINT32_MAX) {
			return false;
		}
		const auto has = [](const auto& list, auto item) {
			return std::ranges::find(list, item) != list.end();
		};
		const auto offset = low.loads.front();
		const bool shape =
		    offset >= 0 && offset <= INT32_MAX &&
		    std::ranges::all_of(low.loads, [&](int64_t item) { return item == offset; }) &&
		    std::ranges::all_of(low.regs, [&](uint32_t reg) { return reg == base_lo; }) &&
		    has(low.regs, base_lo) &&
		    std::ranges::all_of(
		        high.loads, [&](int64_t item) { return item == offset || item == offset + 4; }) &&
		    has(high.loads, offset + 4) &&
		    std::ranges::all_of(high.regs,
		                        [&](uint32_t reg) { return reg == base_lo || reg == base_hi; }) &&
		    has(high.regs, base_hi);
		if (!shape) {
			return false;
		}
		if (pointer != nullptr) {
			*pointer                 = {};
			pointer->user_data       = true;
			pointer->store           = true;
			pointer->lo              = base_lo;
			pointer->hi              = base_hi;
			pointer->relative_offset = static_cast<uint32_t>(offset);
			if (const auto* top = handle.Arg(1).Resolve().TryInstruction();
			    top != nullptr && top->GetOpcode() == ValueOpcode::BitwiseOr32) {
				for (uint32_t arg = 0; arg < top->NumArgs(); ++arg) {
					const auto mask = top->Arg(arg).Resolve();
					if (mask.IsImmediate() && mask.GetType() == Type::U32) {
						pointer->high_mask = mask.U32();
					}
				}
			}
			if (words == 4u) {
				const auto records    = handle.Arg(2).Resolve();
				int64_t    records_at = 0;
				if (records.IsImmediate() && records.GetType() == Type::U32) {
					pointer->records = records.U32();
				} else if (const auto* load = records.TryInstruction();
				           load != nullptr && load->GetOpcode() == ValueOpcode::LoadAddressU32 &&
				           record_offset(*load, records_at) && records_at >= 0 &&
				           records_at <= INT32_MAX) {
					pointer->records_offset = static_cast<uint32_t>(records_at);
				}
			}
		}
		return true;
	}

	HostEvaluated HostEvaluatedValues() const {
		HostEvaluated result;
		const auto    close = [](std::unordered_set<const Inst*>& needed, auto&& roots) {
			std::vector<const Inst*> pending;
			const auto               need = [&](Value value) {
				if (value.IsEmpty()) return;
				const auto* inst = value.Resolve().TryInstruction();
				if (inst != nullptr && needed.insert(inst).second) pending.push_back(inst);
			};
			roots(need);
			while (!pending.empty()) {
				const auto* inst = pending.back();
				pending.pop_back();
				for (uint32_t arg = 0; arg < inst->NumArgs(); ++arg)
					need(inst->Arg(arg));
			}
		};
		// A BDA store miss is dropped, so a handle written through must be bound by the host.
		std::unordered_set<const Inst*> written;
		{
			std::vector<const Inst*> pending;
			for (auto* block: m_program.blocks) {
				for (auto& inst: *block) {
					const auto op     = inst.GetOpcode();
					const auto buffer = BufferAccessOf(op);
					if (buffer == BufferAccess::Write || buffer == BufferAccess::Atomic ||
					    AddressOpcodeInfoOf(op).access == AddressAccess::Write) {
						if (const auto* handle = inst.Arg(0).Resolve().TryInstruction())
							pending.push_back(handle);
					}
				}
			}
			while (!pending.empty()) {
				const auto* inst = pending.back();
				pending.pop_back();
				if (!written.insert(inst).second) continue;
				const auto op = inst->GetOpcode();
				if (op == ValueOpcode::GetAddressResource || op == ValueOpcode::GetBufferResource)
					continue;
				for (uint32_t arg = 0; arg < inst->NumArgs(); ++arg) {
					const auto operand = inst->Arg(arg).Resolve();
					if (operand.GetType() == Type::BufferResource ||
					    operand.GetType() == Type::AddressResource) {
						if (const auto* handle = operand.TryInstruction())
							pending.push_back(handle);
					}
				}
			}
		}
		const auto handles = [&](bool decodable) {
			return [&, decodable](auto&& need) {
				for (auto* block: m_program.blocks) {
					for (auto& inst: *block) {
						const auto op                = inst.GetOpcode();
						const bool buffer_or_address = op == ValueOpcode::GetAddressResource ||
						                               op == ValueOpcode::GetBufferResource;
						const bool image_or_sampler  = op == ValueOpcode::GetImageResource ||
						                               op == ValueOpcode::GetSamplerResource;
						const bool strict = image_or_sampler ||
						                    (buffer_or_address && written.contains(&inst) &&
						                     !(RelativeWriteDecode() && RelativeStoreHandle(inst)));
						if (decodable ? buffer_or_address && !strict : strict) {
							for (uint32_t arg = 0; arg < inst.NumArgs(); ++arg)
								need(inst.Arg(arg));
						}
					}
				}
				if (!decodable) {
					for (const auto* block: m_program.blocks)
						need(block->indirect_target);
				}
			};
		};
		close(result.strict, handles(false));
		close(result.decodable, handles(true));
		return result;
	}

	bool ReadsUnfoldableScalar(const Inst& handle) const {
		std::vector<const Inst*>        pending {&handle};
		std::unordered_set<const Inst*> seen;
		while (!pending.empty() && seen.size() < 256u) {
			const auto* inst = pending.back();
			pending.pop_back();
			if (!seen.insert(inst).second) continue;
			if (inst != &handle && inst->GetOpcode() == ValueOpcode::LoadAddressU32 &&
			    std::ranges::find(m_program.unfoldable_pcs, inst->Flags<MemoryFlags>().pc) !=
			        m_program.unfoldable_pcs.end())
				return true;
			for (uint32_t arg = 0; arg < inst->NumArgs(); ++arg)
				if (const auto* operand = inst->Arg(arg).Resolve().TryInstruction())
					pending.push_back(operand);
		}
		return false;
	}

	void PlanScalarReads() {
		m_program.srt_plan_complete = false;
		m_program.srt_reads.clear();
		m_cyclic_entries.program = &m_program;
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				const auto op    = inst.GetOpcode();
				const auto image = ImageOpcodeInfoOf(op);
				if (BufferAccessOf(op) == BufferAccess::None &&
				    AddressOpcodeInfoOf(op).access == AddressAccess::None &&
				    image.access == ImageAccess::None)
					continue;
				const auto flags = inst.Flags<MemoryFlags>();
				if (flags.index >= m_program.memory_info.size())
					Fail(flags.pc, "memory metadata index is out of range");
				if (inst.NumArgs() < (image.needs_sampler ? 2u : 1u))
					Fail(flags.pc, "memory operation has no resource handle");
				const auto& memory = m_program.memory_info[flags.index];
				if ((op == ValueOpcode::LoadAddressU32 &&
				     memory.kind == ResourceKind::ScalarBuffer) ||
				    (op == ValueOpcode::ReadConstBuffer &&
				     memory.kind == ResourceKind::ScalarAddress))
					Fail(flags.pc, "scalar read has incompatible scalar memory metadata");
				for (uint32_t arg = 0; arg < (image.needs_sampler ? 2u : 1u); ++arg) {
					const auto* handle = inst.Arg(arg).Resolve().TryInstruction();
					if (handle == nullptr) continue;
					const auto     kind    = handle->GetOpcode();
					const bool     sampler = kind == ValueOpcode::GetSamplerResource;
					const uint32_t width = kind == ValueOpcode::GetImageResource               ? 8u
					                       : kind == ValueOpcode::GetAddressResource           ? 2u
					                       : kind == ValueOpcode::GetBufferResource || sampler ? 4u
					                                                                           : 0u;
					if (width == 0u) continue;
					const auto base =
					    width == 2u
					        ? (memory.kind == ResourceKind::ScalarAddress ? ScalarReadBase(inst)
					                                                      : UINT32_MAX)
					        : (sampler ? memory.sampler : memory.resource) * 4u;
					DescriptorSource source;
					MakeSource(*handle, width, sampler,
					           sampler && (memory.image_sample_flags &
					                       Decoder::ImageSampleFlagAdjust) != 0,
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
				    ScalarReadMemory(inst, index) != nullptr &&
				    inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst), RuntimeValueType::Any, nullptr,
				                         &m_cyclic_entries))
					CollectScalarRead(Value(&inst), inst.Flags<MemoryFlags>().pc);
			}
		}
		m_cyclic_entries       = {};
		const auto host_needed = HostEvaluatedValues();
		m_program.data_flat_slots.clear();
		for (auto* read: m_scalar_reads) {
			const auto flags = read->Flags<MemoryFlags>();
			if (std::ranges::find(m_program.unfoldable_pcs, flags.pc) !=
			    m_program.unfoldable_pcs.end()) {
				continue;
			}
			auto& memory         = m_program.memory_info[flags.index];
			memory.planning_only = true;
			const auto* handle   = read->Arg(0).Resolve().TryInstruction();
			auto        resolved = std::ranges::find_if(m_resolved_handles, [&](const auto& entry) {
				return entry.handle == handle && entry.pc == flags.pc;
			});
			EXIT_IF(resolved == m_resolved_handles.end());
			uint32_t slot = 0;
			for (; slot < m_program.srt_reads.size(); ++slot) {
				const auto* other = m_program.srt_reads[slot].value.Resolve().TryInstruction();
				if (other->GetOpcode() != read->GetOpcode() ||
				    m_program.memory_info[other->Flags<MemoryFlags>().index] != memory)
					continue;
				const auto* other_handle = other->Arg(0).Resolve().TryInstruction();
				bool        same         = true;
				for (uint32_t word = 0; word < resolved->source.dword_count; ++word)
					same &= EquivalentValue(m_program, resolved->source.dwords[word],
					                        other_handle->Arg(word));
				for (size_t arg = 1; arg < read->NumArgs(); ++arg)
					same &= EquivalentValue(m_program, read->Arg(arg), other->Arg(arg));
				if (same) break;
			}
			const bool keep = slot == m_program.srt_reads.size();
			// 0: the host needs the word; 1: data only; 2: a buffer or address handle word.
			const uint8_t kind = read->GetOpcode() != ValueOpcode::LoadAddressU32 ||
			                             host_needed.strict.contains(read)
			                         ? 0u
			                     : host_needed.decodable.contains(read) ? 2u
			                                                            : 1u;
			if (keep) {
				m_program.srt_reads.push_back({Value(read), slot});
				m_program.data_flat_slots.push_back(kind);
			} else {
				auto& merged = m_program.data_flat_slots[slot];
				merged       = merged == 0u || kind == 0u ? 0u : std::max(merged, kind);
			}
			auto*      block = read->Parent();
			auto&      list  = block->Instructions();
			const auto where =
			    std::ranges::find_if(list, [&](const Inst& inst) { return &inst == read; });
			const auto resource =
			    Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(
			    where, ValueOpcode::ReadConst, {resource, Value(slot)}, read->Flags<uint64_t>()));
			const auto uses = read->Uses();
			for (const auto& use: uses)
				use.user->SetArg(use.operand, flat);
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
						unchanged &=
						    handle->Arg(word).Resolve() == resolved->source.dwords[word].Resolve();
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

	// Reports the first dword that cannot be materialized and, when the caller asks, why. The
	// reason sink is written only on the rejecting path.
	bool ValidateSource(const DescriptorSource& descriptor, uint32_t& bad_dword,
	                    RuntimeValueFailure* failure = nullptr) const {
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			bad_dword = i;
			if (descriptor.dwords[i].Resolve().GetType() != Type::U32) {
				if (failure != nullptr) {
					*failure        = {};
					failure->reason = RuntimeValueReject::NonScalarType;
				}
				return false;
			}
			if (!ValidateRuntimeValue(m_program, descriptor.dwords[i], RuntimeValueType::Any,
			                          failure, &m_cyclic_entries)) {
				return false;
			}
		}
		return true;
	}

	static bool IsDynamicMemoryOrigin(Value value, uint32_t depth = 0) {
		if (depth > 8) {
			return false;
		}
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ReadConstBuffer || op == ValueOpcode::LoadAddressU32 ||
		    BufferAccessOf(op) != BufferAccess::None ||
		    AddressOpcodeInfoOf(op).access != AddressAccess::None) {
			return true;
		}
		for (size_t i = 0; i < inst->NumArgs(); i++) {
			if (IsDynamicMemoryOrigin(inst->Arg(i), depth + 1)) {
				return true;
			}
		}
		return false;
	}

	static bool IsDynamicDescriptor(const DescriptorSource& descriptor) {
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			if (IsDynamicMemoryOrigin(descriptor.dwords[i])) {
				return true;
			}
		}
		return false;
	}

	uint32_t InternSource(const DescriptorSource& descriptor) {
		for (uint32_t candidate = 0; candidate < m_sources.size(); candidate++) {
			const auto& current = m_sources[candidate];
			if (current.dword_count != descriptor.dword_count ||
			    current.indirect_descriptor.has_value() !=
			        descriptor.indirect_descriptor.has_value() ||
			    current.indirect_buffer != descriptor.indirect_buffer) {
				// A table source carries the heap V# in its dwords, so without the indirect
				// description it is indistinguishable from the heap binding itself and the two
				// collapse into one resource that describes neither.
				continue;
			}
			if (current.indirect_descriptor.has_value()) {
				const auto& a = *current.indirect_descriptor;
				const auto& b = *descriptor.indirect_descriptor;
				// Every scalar of the description has to match, not only the ones that used to
				// vary: two plans differing just in where the record sits describe different
				// tables, and interning them together hands one the other's layout.
				if (a.bindless != b.bindless) {
					continue;
				}
				if (a.material_source != b.material_source || a.table_source != b.table_source ||
				    a.selector_stride != b.selector_stride ||
				    a.selector_offset != b.selector_offset ||
				    a.selector_shift != b.selector_shift || a.table_offset != b.table_offset ||
				    a.workgroup_axis != b.workgroup_axis || a.sources != b.sources ||
				    a.material_offset != b.material_offset || a.heap_stride != b.heap_stride ||
				    a.record_offset != b.record_offset || a.key_mask != b.key_mask ||
				    a.indexed_heap != b.indexed_heap || a.affine_selector != b.affine_selector ||
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

	// The heap reads die with the handle rewrite, but the index feeding them need not: a guest that
	// reuses one register for the record offset and for something else leaves it in a phi, and
	// plain arithmetic costs nothing to keep emitting. What must not survive is another memory
	// access through this index - it is not in the plan, so it would keep the heap handle alive
	// after every other reader became planning-only, and no dense resource stands behind it.
	static bool UsedOnlyByReadsAndArithmetic(const Inst&                  value,
	                                         std::span<const Inst* const> users) {
		return !value.Uses().empty() && std::ranges::all_of(value.Uses(), [&](const Use& use) {
			if (std::ranges::find(users, use.user) != users.end()) {
				return true;
			}
			const auto op = use.user->GetOpcode();
			return BufferAccessOf(op) == BufferAccess::None &&
			       AddressOpcodeInfoOf(op).access == AddressAccess::None &&
			       ImageOpcodeInfoOf(op).access == ImageAccess::None;
		});
	}

	// PlanIndirectImages reaches a handle through an image operation's operand 0, so a handle used
	// any other way would keep reads alive that its plan marks planning-only.
	static bool UsedOnlyAsImageHandle(const Inst& handle) {
		return !handle.Uses().empty() && std::ranges::all_of(handle.Uses(), [](const Use& use) {
			return use.operand == 0u &&
			       ImageOpcodeInfoOf(use.user->GetOpcode()).access != ImageAccess::None;
		});
	}

	// A sibling names the same heap record in the same order, so it plans identically and is
	// rewritten too. Only then does the shared record become dead and safe to skip emitting.
	static bool IsIndirectImageSibling(const Inst& user, std::span<const Inst* const> heap_reads) {
		if (user.GetOpcode() != ValueOpcode::GetImageResource ||
		    user.NumArgs() != heap_reads.size()) {
			return false;
		}
		for (uint32_t dword = 0; dword < heap_reads.size(); dword++) {
			if (user.Arg(dword).Resolve().TryInstruction() != heap_reads[dword]) {
				return false;
			}
		}
		return UsedOnlyAsImageHandle(user);
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

	// RE9 samples one bindless texture several times, so its heap record feeds several handles.
	static bool ReadIsPrivateToSiblings(const Inst& read, std::span<const Inst* const> heap_reads) {
		return !read.Uses().empty() && std::ranges::all_of(read.Uses(), [&](const Use& use) {
			return IsIndirectImageSibling(*use.user, heap_reads);
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

	static bool IsMemoryAccess(ValueOpcode op) {
		return BufferAccessOf(op) != BufferAccess::None ||
		       AddressOpcodeInfoOf(op).access != AddressAccess::None ||
		       ImageOpcodeInfoOf(op).access != ImageAccess::None;
	}

	bool MemoryIndexBelongsTo(uint32_t index, const Inst& owner) const {
		if (!m_memory_accessors.empty()) {
			const auto& accessors = m_memory_accessors[index];
			return accessors.count == 0u || (accessors.count == 1u && accessors.only == &owner);
		}
		for (const auto* block: m_program.blocks) {
			for (const auto& inst: *block) {
				if (!IsMemoryAccess(inst.GetOpcode()) || &inst == &owner) {
					continue;
				}
				if (inst.Flags<MemoryFlags>().index == index) {
					return false;
				}
			}
		}
		return true;
	}

	// Image planning neither adds nor renumbers memory accesses, so this holds while it runs.
	void IndexMemoryAccessors() {
		m_memory_accessors.assign(m_program.memory_info.size(), {});
		for (const auto* block: m_program.blocks) {
			for (const auto& inst: *block) {
				if (!IsMemoryAccess(inst.GetOpcode())) {
					continue;
				}
				const auto index = inst.Flags<MemoryFlags>().index;
				if (index < m_memory_accessors.size()) {
					m_memory_accessors[index].count++;
					m_memory_accessors[index].only = &inst;
				}
			}
		}
	}

	bool MakeRuntimeTableSource(const Inst& read, DescriptorSource& descriptor) {
		const auto* handle = read.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr) return false;
		const auto width = handle->GetOpcode() == ValueOpcode::GetBufferResource    ? 4u
		                   : handle->GetOpcode() == ValueOpcode::GetAddressResource ? 2u
		                                                                            : 0u;
		if (width == 0u) {
			return false;
		}
		const auto flags = read.Flags<MemoryFlags>();
		const auto kind  = m_program.memory_info[flags.index].kind;
		const auto base  = kind == ResourceKind::ScalarAddress || kind == ResourceKind::ScalarBuffer
		                       ? ScalarReadBase(read)
		                       : UINT32_MAX;
		MakeSource(*handle, width, false, false, base, descriptor, flags.pc);
		uint32_t bad_dword = 0;
		return ValidateSource(descriptor, bad_dword);
	}

	// Interns the V# of a table the shader indexes at runtime. The table descriptor itself is
	// host-resolvable even though the record it selects is not, which is what lets the host
	// enumerate the records at all.
	bool MakeRuntimeBufferSource(const Inst& handle, uint32_t pc, uint32_t& source,
	                             DescriptorSource& descriptor) {
		if (handle.GetOpcode() != ValueOpcode::GetBufferResource) {
			return false;
		}
		MakeSource(handle, 4u, false, false, UINT32_MAX, descriptor, pc);
		uint32_t bad_dword = 0;
		if (!ValidateSource(descriptor, bad_dword)) {
			return false;
		}
		source = InternSource(descriptor);
		return true;
	}

	// The workgroup id is the same in every lane of a wave; the per-invocation compute builtins
	// name the invocation and are not.
	static bool WaveUniformBuiltin(const Inst& inst) {
		uint32_t kind = 0;
		return ImmediateU32(inst.Arg(0), kind) &&
		       static_cast<StageInputKind>(kind) == StageInputKind::WorkgroupId;
	}

	// A value every lane computes identically. ReadLane and ReadFirstLane force that; the scalar
	// unit reaches it by construction, which is how a compute shader indexes a descriptor table
	// without ever broadcasting a lane.
	bool WaveUniformValue(const Value& value, uint32_t depth = 0) const {
		constexpr uint32_t MaxUniformDepth = 32;
		if (depth > MaxUniformDepth) {
			return false;
		}
		const auto resolved = value.Resolve();
		if (resolved.IsImmediate()) {
			return true;
		}
		const auto* inst = resolved.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		const auto uniform_args = [&](uint32_t first) {
			for (uint32_t arg = first; arg < inst->NumArgs(); arg++) {
				if (!WaveUniformValue(inst->Arg(arg), depth + 1)) {
					return false;
				}
			}
			return true;
		};
		switch (inst->GetOpcode()) {
			case ValueOpcode::ReadFirstLane:
			case ValueOpcode::ReadLane:
			case ValueOpcode::GetUserData: return true;
			case ValueOpcode::Phi: {
				// A loop carries the selector when the shader walks a list of records. Every lane
				// runs the same iteration, so the phi is uniform exactly when each value entering
				// it is. Assuming a phi already on the stack holds: the cycle is uniform if every
				// operator around it preserves uniformity, which is all this switch admits.
				if (!m_uniform_phis.insert(inst).second) {
					return true;
				}
				const bool uniform = uniform_args(0);
				m_uniform_phis.erase(inst);
				return uniform;
			}
			case ValueOpcode::GetBuiltin: return WaveUniformBuiltin(*inst);
			case ValueOpcode::ReadConstBuffer: {
				uint32_t index = 0;
				// A scalar read lands in an SGPR, so every lane sees the same word.
				return ScalarReadMemory(*inst, index) != nullptr && uniform_args(1);
			}
			case ValueOpcode::IAdd32:
			case ValueOpcode::ISub32:
			case ValueOpcode::IMul32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftRightArithmetic32:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32: return uniform_args(0);
			default: return false;
		}
	}

	// True when re-executing this value means resolving a phi around a loop, so the one value the
	// host can compute belongs to a single iteration rather than to the descriptor as a whole. An
	// invariant phi folds to one value and names one record like anything else.
	bool SelectorIsLoopCarried(Value value, uint32_t depth = 0) const {
		constexpr uint32_t MaxCarriedDepth = 32;
		if (depth > MaxCarriedDepth) {
			return false;
		}
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		if (inst->GetOpcode() == ValueOpcode::Phi) {
			return ResolveInvariantPhi(m_program, value).IsEmpty();
		}
		for (uint32_t arg = 0; arg < inst->NumArgs(); arg++) {
			if (SelectorIsLoopCarried(inst->Arg(arg), depth + 1)) {
				return true;
			}
		}
		return false;
	}

	// Both forms broadcast one lane to the whole wave, so the offset they feed is wave-uniform.
	static bool WaveUniformSelector(const Value& selector) {
		const auto* inst = selector.TryInstruction();
		return inst != nullptr && (inst->GetOpcode() == ValueOpcode::ReadFirstLane ||
		                           inst->GetOpcode() == ValueOpcode::ReadLane);
	}

	// `scalar_uniform` widens what counts as a wave-uniform selector to a chain the scalar unit
	// computed. Only the buffer table passes it: an image table stays on the narrower rule, so a
	// descriptor that already materializes directly is never moved onto a per-draw enumeration.
	// `aligned_offset` says the value is a scalar-memory byte offset, so a mask that only rounds it
	// down to a power of two names a record step rather than a set of records. Only the image table
	// passes it; a buffer table keeps refusing every masked selector.
	bool MatchMaterialOffset(Value value, Value& selector, DescriptorSource::SelectorKind& kind,
	                         uint32_t& stride, uint32_t& offset, uint32_t& mask,
	                         bool scalar_uniform = false, bool aligned_offset = false) const {
		const auto uniform = [&](const Value& candidate) {
			return WaveUniformSelector(candidate) ||
			       (scalar_uniform && WaveUniformValue(candidate));
		};
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
		const auto* arithmetic = value.TryInstruction();
		if (arithmetic == nullptr || arithmetic->NumArgs() != 2u) {
			return false;
		}
		if (arithmetic->GetOpcode() == ValueOpcode::BitwiseAnd32) {
			if (ImmediateU32(arithmetic->Arg(0), mask)) {
				selector = arithmetic->Arg(1).Resolve();
			} else if (ImmediateU32(arithmetic->Arg(1), mask)) {
				selector = arithmetic->Arg(0).Resolve();
			} else {
				return false;
			}
			// Hardware forces Dword alignment - addr = (m_base + offset) & ~0x3 - so on a byte
			// offset a mask whose remaining cleared bits are the contiguous run below a power of
			// two only rounds down to that boundary, and the offsets it reaches are that
			// boundary's multiples: a record step. RE9 writes the same round-down as `s & -15` and
			// `s & -13` in one shader, which agree on every bit hardware keeps; read as bitmaps
			// they would name two different sets of textures for one material.
			const uint32_t kept  = mask & ~uint32_t {3};
			const uint32_t clear = ~kept;
			if (aligned_offset && clear != 3u && clear != UINT32_MAX &&
			    (clear & (clear + 1u)) == 0u) {
				stride = clear + 1u;
				kind   = DescriptorSource::SelectorKind::Stride;
				return uniform(selector);
			}
			kind = DescriptorSource::SelectorKind::Mask;
			// A base added to a masked offset can wrap back into bounds, so the reachable set
			// would no longer stop at the table's end; leave that shape unrecognized.
			return mask != 0u && offset == 0u && uniform(selector);
		}
		// A power-of-two record stride is a shift and any other is a multiply; both name the same
		// table step, so take it from whichever the shader used, as the heap index already does.
		if (arithmetic->GetOpcode() == ValueOpcode::ShiftLeftLogical32) {
			uint32_t shift = 0;
			if (!ImmediateU32(arithmetic->Arg(1), shift) || shift >= 32u) {
				return false;
			}
			stride   = uint32_t {1} << shift;
			selector = arithmetic->Arg(0).Resolve();
			kind     = DescriptorSource::SelectorKind::Stride;
			return uniform(selector);
		}
		if (arithmetic->GetOpcode() != ValueOpcode::IMul32) {
			return false;
		}
		if (ImmediateU32(arithmetic->Arg(0), stride)) {
			selector = arithmetic->Arg(1).Resolve();
		} else if (ImmediateU32(arithmetic->Arg(1), stride)) {
			selector = arithmetic->Arg(0).Resolve();
		} else {
			return false;
		}
		kind = DescriptorSource::SelectorKind::Stride;
		return stride != 0u && uniform(selector);
	}

	// Any U32 the GPU derived, scaled and offset by constants: the stride and offset the chain
	// adds.
	bool MatchAffineMaterialOffset(Value value, uint32_t& stride, uint32_t& offset) const {
		stride = 1u;
		offset = 0u;
		value  = value.Resolve();
		while (const auto* inst = value.TryInstruction()) {
			if (inst->NumArgs() != 2u) break;
			uint32_t constant = 0;
			Value    remaining;
			if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
			    ImmediateU32(inst->Arg(1), constant) && constant < 32u) {
				stride *= 1u << constant;
				remaining = inst->Arg(0);
			} else if (inst->GetOpcode() == ValueOpcode::IAdd32 ||
			           inst->GetOpcode() == ValueOpcode::IMul32) {
				if (ImmediateU32(inst->Arg(0), constant))
					remaining = inst->Arg(1);
				else if (ImmediateU32(inst->Arg(1), constant))
					remaining = inst->Arg(0);
				else
					break;
				if (inst->GetOpcode() == ValueOpcode::IAdd32)
					offset += stride * constant;
				else
					stride *= constant;
			} else {
				break;
			}
			value = remaining.Resolve();
		}
		// A masked selector names a set of records, not a step; MatchMaterialOffset decides those.
		const auto* masked = value.TryInstruction();
		if (masked != nullptr && masked->GetOpcode() == ValueOpcode::BitwiseAnd32) {
			return false;
		}
		return stride != 0u && value.GetType() == Type::U32 && !value.IsImmediate();
	}

	// A heap record index is a shift when the stride is a power of two and a multiply otherwise;
	// both name the same thing, so the stride comes from whichever the shader used.
	bool MatchHeapRecordIndex(const Inst* index_math, uint32_t& stride, Value& key) const {
		if (index_math == nullptr || index_math->NumArgs() != 2u) {
			return false;
		}
		uint32_t immediate = 0;
		if (index_math->GetOpcode() == ValueOpcode::ShiftLeftLogical32) {
			if (!ImmediateU32(index_math->Arg(1), immediate) || immediate >= 32u) {
				return false;
			}
			stride = uint32_t {1} << immediate;
			key    = index_math->Arg(0).Resolve();
			return true;
		}
		if (index_math->GetOpcode() != ValueOpcode::IMul32) {
			return false;
		}
		if (ImmediateU32(index_math->Arg(0), immediate)) {
			key = index_math->Arg(1).Resolve();
		} else if (ImmediateU32(index_math->Arg(1), immediate)) {
			key = index_math->Arg(0).Resolve();
		} else {
			return false;
		}
		stride = immediate;
		return stride != 0u;
	}

	enum class LaneQuantifier { Any, All };
	struct EdgePredicate {
		Value          condition;
		bool           positive;
		LaneQuantifier lanes = LaneQuantifier::All;
	};

	bool NonzeroOnEntry(Value value, const Block* block) const {
		// Each unique predecessor must execute before this use. Stop at joins: an
		// unrelated comparison is not a bound on FindILsb's zero-input sentinel.
		for (size_t depth = 0; block != nullptr && depth < m_program.blocks.size(); ++depth) {
			if (block->ImmPredecessors().size() != 1u) {
				return false;
			}
			const auto* previous = block->ImmPredecessors()[0];
			const auto  edge     = ConditionalEdge(previous, block);
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

	// The shader narrows a material word as (word >> shift) & mask; the host narrows it as
	// (word & key_mask) >> shift, so the mask is recorded shifted into place.
	Value MaterialKeyWord(Value key, DescriptorSource::IndirectDescriptor& indirect) const {
		const auto* inst  = key.Resolve().TryInstruction();
		uint32_t    mask  = UINT32_MAX;
		uint32_t    shift = 0;
		if (inst != nullptr && inst->GetOpcode() == ValueOpcode::BitwiseAnd32 &&
		    inst->NumArgs() == 2u) {
			uint32_t immediate = 0;
			Value    masked;
			if (ImmediateU32(inst->Arg(0), immediate)) {
				masked = inst->Arg(1).Resolve();
			} else if (ImmediateU32(inst->Arg(1), immediate)) {
				masked = inst->Arg(0).Resolve();
			}
			if (immediate != 0u && masked.TryInstruction() != nullptr) {
				mask = immediate;
				key  = masked;
				inst = masked.TryInstruction();
			}
		}
		if (inst != nullptr && inst->GetOpcode() == ValueOpcode::ShiftRightLogical32 &&
		    ImmediateU32(inst->Arg(1), shift) && shift != 0u && shift < 32u) {
			key = inst->Arg(0);
		} else {
			shift = 0;
		}
		indirect.selector_shift = shift;
		indirect.key_mask       = mask == UINT32_MAX ? UINT32_MAX : mask << shift;
		return key.Resolve();
	}

	uint32_t WorkgroupAxis(Value key) const {
		const auto* builtin = key.Resolve().TryInstruction();
		uint32_t    axis    = UINT32_MAX;
		if (m_program.stage != ShaderType::Compute || builtin == nullptr ||
		    builtin->GetOpcode() != ValueOpcode::GetBuiltin || builtin->NumArgs() != 2u ||
		    builtin->Arg(0) != Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)) ||
		    !ImmediateU32(builtin->Arg(1), axis) || axis >= 3u)
			return UINT32_MAX;
		return axis;
	}

	// The byte offset an image heap read computes: a record index scaled by the heap's record
	// stride, plus whatever constants the shader folded in. The stride travels back out because a
	// heap of plain T# strides by 32 but an engine that keeps a T# inside a larger per-record
	// struct does not, and the record step is what the host enumeration has to walk.
	bool MatchTableOffset(Value value, Value& key, uint32_t& offset, uint32_t& stride) const {
		offset = 0;
		stride = 0;
		for (;;) {
			const auto* inst = value.Resolve().TryInstruction();
			if (inst == nullptr || inst->NumArgs() != 2u) {
				return false;
			}
			uint32_t immediate;
			if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
			    ImmediateU32(inst->Arg(1), immediate) && immediate < 32u) {
				stride = uint32_t {1} << immediate;
				key    = inst->Arg(0).Resolve();
				return key.GetType() == Type::U32;
			}
			// A power-of-two stride is a shift and any other is a multiply; both name the same
			// record step.
			if (inst->GetOpcode() == ValueOpcode::IMul32) {
				Value factor;
				if (ImmediateU32(inst->Arg(0), stride)) {
					factor = inst->Arg(1).Resolve();
				} else if (ImmediateU32(inst->Arg(1), stride)) {
					factor = inst->Arg(0).Resolve();
				} else {
					return false;
				}
				key = factor;
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
				const auto kind   = inst->Flags<CFG::BranchCondition>();
				const bool scalar = kind == CFG::BranchCondition::SccZero ||
				                    kind == CFG::BranchCondition::SccNonZero;
				const bool zero =
				    kind == CFG::BranchCondition::ExecZero || kind == CFG::BranchCondition::VccZero;
				const bool nonzero = kind == CFG::BranchCondition::ExecNonZero ||
				                     kind == CFG::BranchCondition::VccNonZero;
				if (!scalar && !zero && !nonzero) break;
				// SCC is uniform. Negating a lane reduction exchanges all and any.
				edge.lanes =
				    scalar || (zero == edge.positive) ? LaneQuantifier::All : LaneQuantifier::Any;
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
		mask                            = mask.Resolve();
		const auto incoming_is_nonempty = [&](const Block* from, const Block* to, Value incoming,
		                                      const Block* header) {
			const auto accepts = [&](const EdgePredicate& edge) {
				return edge.positive && Implies(edge.condition, incoming);
			};
			const auto edge = ConditionalEdge(from, to);
			if (edge && accepts(*edge)) return true;
			const auto* definition = incoming.Resolve().TryInstruction();
			return GuardedOnEntry(
			    from,
			    [&](const Block* block) {
				    // A witness before this definition may belong to an earlier loop iteration.
				    return block == header ||
				           (definition != nullptr && block == definition->Parent());
			    },
			    accepts);
		};
		const auto* phi = mask.TryInstruction();
		if (phi != nullptr && phi->GetOpcode() == ValueOpcode::Phi && phi->GetType() == Type::U1 &&
		    phi->NumArgs() != 0u) {
			for (size_t arm = 0; arm < phi->NumArgs(); ++arm) {
				if (!incoming_is_nonempty(phi->PhiBlock(arm), phi->Parent(), phi->Arg(arm),
				                          phi->Parent()))
					return false;
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
			if (phi->PhiBlock(back) != update_block || !MaskOnlyLosesBits(phi->Arg(back), value))
				continue;
			const auto initial = phi->Arg(back ^ 1u).Resolve();
			return ValidateRuntimeValue(m_program, initial, RuntimeValueType::Integer, nullptr,
			                            &m_cyclic_entries)
			           ? initial
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
			const auto  bit_guard = PositiveLaneWitness(phi->PhiBlock(bit_arm));
			const auto* selected  = phi->Arg(bit_arm).Resolve().TryInstruction();
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
				const auto  sentinel_guard = PositiveLaneWitness(phi->PhiBlock(sentinel_arm));
				const auto* sentinel       = phi->Arg(sentinel_arm).Resolve().TryInstruction();
				uint32_t    bound          = 0;
				if (sentinel == nullptr || sentinel->GetOpcode() != ValueOpcode::SelectU32 ||
				    sentinel->NumArgs() != 3u || !Implies(sentinel_guard, sentinel->Arg(0)) ||
				    !ImmediateU32(sentinel->Arg(1), bound) || bound != 32u)
					continue;
				const auto  loop_active = sentinel->Arg(0).Resolve();
				const auto* active_phi  = loop_active.TryInstruction();
				if (active_phi == nullptr || active_phi->GetOpcode() != ValueOpcode::Phi ||
				    active_phi->NumArgs() != 2u)
					continue;
				const auto  other_arm = 3u - bit_arm - sentinel_arm;
				const auto* carried   = phi->Arg(other_arm).Resolve().TryInstruction();
				const auto* bit_block = phi->PhiBlock(bit_arm);
				if (carried == nullptr || carried->GetOpcode() != ValueOpcode::Phi ||
				    carried->NumArgs() != 2u || carried->Parent() != active_phi->Parent() ||
				    selected->Arg(2).Resolve() != phi->Arg(sentinel_arm).Resolve() ||
				    sentinel->Arg(2).Resolve() != phi->Arg(other_arm).Resolve())
					continue;
				const uint32_t back = carried->PhiBlock(0) == bit_block ? 0u : 1u;
				if (carried->PhiBlock(back) != bit_block ||
				    carried->Arg(back).Resolve() != phi->Arg(bit_arm).Resolve())
					continue;
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
		Value       guard;
		Value       local;
		const auto* first = key.Resolve().TryInstruction();
		if (first != nullptr && first->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    first->NumArgs() == 2u) {
			guard = first->Arg(1).Resolve();
			// Empty EXEC selects lane zero, which may not have loaded a material key.
			// An executing store also witnesses nonempty EXEC at the lane read.
			if (executing.IsEmpty() ? !HasActiveLane(guard, first->Parent())
			                        : !Implies(executing, guard))
				return nullptr;
			local           = first->Arg(0).Resolve();
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
		    selected->NumArgs() != 3u || !Implies(guard, selected->Arg(0)))
			return nullptr;
		return selected;
	}

	bool MatchUniformizedMaterialKey(Value key, const Inst& image,
	                                 DescriptorSource::IndirectDescriptor& indirect,
	                                 DescriptorSource&                     material_source) {
		const auto* selected = UniformizedMaterialValue(key, image);
		if (selected == nullptr) return false;
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
		    !MakeRuntimeTableSource(*read, material_source))
			return false;
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

	bool MatchUniformizedBufferKey(Value key, const Inst& image,
	                               DescriptorSource::IndirectDescriptor& indirect,
	                               DescriptorSource&                     material_source) {
		const auto* selected = UniformizedMaterialValue(key, image);
		if (selected == nullptr) return false;
		const auto* loaded = MaterialKeyWord(selected->Arg(1), indirect).TryInstruction();
		if (indirect.selector_shift == 0u && indirect.key_mask == UINT32_MAX) return false;
		if (loaded == nullptr || loaded->GetOpcode() != ValueOpcode::SelectU32 ||
		    !EquivalentValue(m_program, selected->Arg(0), loaded->Arg(0)))
			return false;
		const auto* read = loaded->Arg(1).Resolve().TryInstruction();
		if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadBufferU32) return false;
		const auto& memory = m_program.memory_info[read->Flags<MemoryFlags>().index];
		uint32_t    offset = 1u, scalar = 1u;
		if (memory.kind != ResourceKind::Buffer || memory.typed || memory.formatted ||
		    !memory.idxen || memory.offen || !ImmediateU32(read->Arg(2), offset) || offset != 0u ||
		    !ImmediateU32(read->Arg(3), scalar) || scalar != 0u ||
		    !EquivalentValue(m_program, selected->Arg(0), read->Arg(4)) ||
		    !MakeRuntimeTableSource(*read, material_source))
			return false;
		// GPU index arithmetic may wrap. Every record in the bounded V# is a candidate.
		indirect.material_source = InternSource(material_source);
		indirect.selector_first  = Value(0u);
		indirect.key_count       = material_source.dwords[2];
		indirect.selector_offset = memory.offset;
		return true;
	}

	const Inst* BoundedLoop(Value key, const Block* use, const auto& accepts_bound) const {
		const auto* phi = key.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi || phi->GetType() != Type::U32 ||
		    phi->NumArgs() != 2u || phi->NumPhiBlocks() != 2u)
			return {};
		const Block* increment_block = nullptr;
		uint32_t     initial_arm     = 0;
		for (uint32_t initial = 0; initial < 2u; ++initial) {
			const auto  zero = phi->Arg(initial).Resolve();
			const auto* step = phi->Arg(initial ^ 1u).Resolve().TryInstruction();
			if (!zero.IsImmediate() || zero.GetType() != Type::U32 || zero.U32() != 0u ||
			    step == nullptr || step->GetOpcode() != ValueOpcode::IAdd32 ||
			    step->Parent() == nullptr)
				continue;
			// Structurization can forward the native increment through a synthetic continue block.
			const auto* incoming = phi->PhiBlock(initial ^ 1u);
			const auto* next     = phi->Parent();
			for (size_t edges = 0; incoming != step->Parent() && edges < m_program.blocks.size();
			     ++edges) {
				if (incoming == nullptr ||
				    incoming->terminator.kind != CFG::TerminatorKind::Branch ||
				    incoming->terminator.true_block != next ||
				    incoming->ImmPredecessors().size() != 1u)
					break;
				next     = incoming;
				incoming = incoming->ImmPredecessors()[0];
			}
			if (incoming != step->Parent()) continue;
			uint32_t increment = 0;
			if ((step->Arg(0).Resolve() == key && ImmediateU32(step->Arg(1), increment) &&
			     increment == 1u) ||
			    (step->Arg(1).Resolve() == key && ImmediateU32(step->Arg(0), increment) &&
			     increment == 1u)) {
				increment_block = step->Parent();
				initial_arm     = initial;
				break;
			}
		}
		if (increment_block == nullptr) return {};

		const auto stops = [&](const Block* block) { return block == phi->Parent(); };
		for (const auto& use_of_key: phi->Uses()) {
			const auto* compare = use_of_key.user;
			if ((compare->GetOpcode() != ValueOpcode::SLessThan32 &&
			     compare->GetOpcode() != ValueOpcode::ULessThan32) ||
			    use_of_key.operand != 0u || !accepts_bound(*compare, initial_arm))
				continue;
			if (!GuardedOnEntry(
			        use != nullptr ? use : increment_block, stops, [&](const EdgePredicate& edge) {
				        return edge.positive && Implies(edge.condition, Value(use_of_key.user));
			        }))
				continue;
			LoopBoundProof proof(m_program, *phi, *compare);
			// The image bound and the increment guard are separate obligations: a
			// skipped image alone does not prevent signed induction wraparound.
			if (GuardedOnEntry(increment_block, stops, [&](const EdgePredicate& edge) {
				    return proof.Excludes(edge.condition, edge.positive);
			    }))
				return compare;
		}
		return {};
	}

	Value BoundedLoopCount(Value key, const Block* use) const {
		const auto* phi = key.Resolve().TryInstruction();
		if (m_shader_writes || phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->GetType() != Type::U32 || phi->NumArgs() != 2u)
			return {};
		const Block* increment_block = nullptr;
		for (uint32_t initial = 0; initial < 2u; ++initial) {
			const auto  zero = phi->Arg(initial).Resolve();
			const auto* step = phi->Arg(initial ^ 1u).Resolve().TryInstruction();
			if (!zero.IsImmediate() || zero.GetType() != Type::U32 || zero.U32() != 0u ||
			    step == nullptr || step->GetOpcode() != ValueOpcode::IAdd32 ||
			    step->Parent() == nullptr)
				continue;
			// Structurization can forward the native increment through a synthetic continue block.
			const auto* incoming = phi->PhiBlock(initial ^ 1u);
			const auto* next     = phi->Parent();
			for (size_t edges = 0; incoming != step->Parent() && edges < m_program.blocks.size();
			     ++edges) {
				if (incoming == nullptr ||
				    incoming->terminator.kind != CFG::TerminatorKind::Branch ||
				    incoming->terminator.true_block != next ||
				    incoming->ImmPredecessors().size() != 1u)
					break;
				next     = incoming;
				incoming = incoming->ImmPredecessors()[0];
			}
			if (incoming != step->Parent()) continue;
			uint32_t increment = 0;
			if ((step->Arg(0).Resolve() == key && ImmediateU32(step->Arg(1), increment) &&
			     increment == 1u) ||
			    (step->Arg(1).Resolve() == key && ImmediateU32(step->Arg(0), increment) &&
			     increment == 1u)) {
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
			    !ValidateRuntimeValue(m_program, compare->Arg(1), RuntimeValueType::Any, nullptr,
			                          &m_cyclic_entries))
				continue;
			if (!guarded_on_entry(use, [&](const EdgePredicate& edge) {
				    return edge.positive && Implies(edge.condition, Value(use_of_key.user));
			    }))
				continue;
			LoopBoundProof proof(m_program, *phi, *compare);
			// The image bound and the increment guard are separate obligations: a
			// skipped image alone does not prevent signed induction wraparound.
			if (guarded_on_entry(increment_block, [&](const EdgePredicate& edge) {
				    return proof.Excludes(edge.condition, edge.positive);
			    }))
				return compare->Arg(1);
		}
		return {};
	}

	const Inst* BoundedLoop(Value key, const Block* use) const {
		return BoundedLoop(key, use, [&](const Inst& compare, uint32_t) {
			return ValidateRuntimeValue(m_program, compare.Arg(1), RuntimeValueType::Integer,
			                            nullptr, &m_cyclic_entries);
		});
	}

	// A material key the shader reads with a scalar load indexed by a bounded loop: the host
	// enumerates the loop's records at the instruction's own byte stride.
	bool MatchScalarMaterialKey(Value key, DescriptorSource::IndirectDescriptor& indirect,
	                            DescriptorSource& material_source) {
		const auto* read = key.Resolve().TryInstruction();
		if (read == nullptr || read->GetOpcode() != ValueOpcode::ReadConstBuffer) return false;
		uint32_t    memory_index = 0;
		const auto* memory       = ScalarReadMemory(*read, memory_index);
		Value       index;
		uint32_t    offset = 0, stride = 0;
		if (memory == nullptr || memory->kind != ResourceKind::ScalarBuffer ||
		    memory->offset > INT32_MAX || !MemoryIndexBelongsTo(memory_index, *read) ||
		    !MatchTableOffset(read->Arg(1), index, offset, stride) ||
		    ((offset | stride | memory->offset) & 3u) != 0u ||
		    uint64_t {offset} + memory->offset > UINT32_MAX)
			return false;
		const auto* bound = BoundedLoop(index, read->Parent());
		if (bound == nullptr || bound->GetOpcode() != ValueOpcode::ULessThan32 ||
		    !MakeRuntimeTableSource(*read, material_source))
			return false;
		indirect.material_source = InternSource(material_source);
		indirect.material_offset = memory->offset;
		indirect.selector_stride = stride;
		indirect.selector_offset = offset + memory->offset;
		indirect.selector_first  = Value(0u);
		indirect.key_count       = bound->Arg(1);
		return true;
	}

	void FoldBoundedLoopSelectors() {
		for (auto* header: m_program.blocks) {
			for (auto& induction: *header) {
				const Inst* carried = nullptr;
				uint32_t    maximum = 0;
				const auto* compare = BoundedLoop(
				    Value(&induction), nullptr, [&](const Inst& test, uint32_t initial) {
					    if (test.GetOpcode() != ValueOpcode::ULessThan32) return false;
					    carried = test.Arg(1).Resolve().TryInstruction();
					    if (carried == nullptr || carried->GetOpcode() != ValueOpcode::Phi ||
					        carried->GetType() != Type::U32 || carried->Parent() != header ||
					        carried->NumArgs() != 2u || carried->NumPhiBlocks() != 2u ||
					        carried->PhiBlock(0) != induction.PhiBlock(0) ||
					        carried->PhiBlock(1) != induction.PhiBlock(1))
						    return false;
					    maximum = SelectorMaximum(carried->Arg(initial));
					    if (maximum == 0u || maximum == UINT32_MAX) return false;
					    const std::array assumptions {
					        std::pair {static_cast<const Inst*>(&induction), maximum - 1u},
					        std::pair {carried, maximum}};
					    // Close the carried-bound invariant before using either assumption.
					    return SelectorMaximum(carried->Arg(initial ^ 1u), assumptions) <= maximum;
				    });
				if (compare == nullptr) continue;
				const std::array assumptions {
				    std::pair {static_cast<const Inst*>(&induction), maximum - 1u},
				    std::pair {carried, maximum}};
				// Native scalar branches advance the zero/+1 induction uniformly, so
				// even an any-lane bound test limits it by the largest carried bound.
				// Per-lane edges instead require the comparison on every executing lane.
				for (auto* block: m_program.blocks) {
					if (!GuardedOnEntry(
					        block, [&](const Block* at) { return at == header; },
					        [&](const EdgePredicate& edge) {
						        return edge.positive &&
						               Implies(edge.condition, Value(const_cast<Inst*>(compare)));
					        }))
						continue;
					for (auto& inst: *block) {
						if (inst.GetOpcode() == ValueOpcode::SelectU32 &&
						    SelectorPredicateFalse(inst.Arg(0), assumptions))
							inst.ReplaceUsesWith(inst.Arg(2));
					}
				}
			}
		}
	}

	// One image descriptor: eight dwords of T#.
	static constexpr uint32_t DescriptorBytes = 8u * sizeof(uint32_t);

	bool TryMakeIndirectImage(Inst& handle, uint32_t base_reg, uint32_t pc,
	                          IndirectDescriptorPlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetImageResource || handle.NumArgs() != 8u) {
			return false;
		}
		Inst*    table_handle = nullptr;
		Value    key;
		uint32_t table_offset = 0;
		uint32_t heap_stride  = 0;
		for (uint32_t dword = 0; dword < plan.reads.size(); ++dword) {
			auto* read =
			    NativeDescriptorSource(handle.Arg(dword), base_reg + dword, pc).TryInstruction();
			if (read == nullptr) {
				return false;
			}
			uint32_t    memory_index = 0;
			const auto* memory       = ScalarReadMemory(*read, memory_index);
			if (memory == nullptr || memory->offset > INT32_MAX || (memory->offset & 3u) != 0u ||
			    !MemoryIndexBelongsTo(memory_index, *read)) {
				return false;
			}
			auto*    current_handle = read->Arg(0).Resolve().TryInstruction();
			Value    current_key;
			uint32_t offset         = 0;
			uint32_t current_stride = 0;
			if (current_handle == nullptr ||
			    current_handle->GetOpcode() != (memory->kind == ResourceKind::ScalarAddress
			                                        ? ValueOpcode::GetAddressResource
			                                        : ValueOpcode::GetBufferResource) ||
			    (memory->kind == ResourceKind::ScalarAddress &&
			     read->Parent() != handle.Parent()) ||
			    (table_handle != nullptr &&
			     !EquivalentValue(m_program, Value(table_handle), Value(current_handle))) ||
			    !MatchTableOffset(read->Arg(1), current_key, offset, current_stride) ||
			    memory->offset > UINT32_MAX - offset) {
				return false;
			}
			offset += memory->offset;
			if (dword == 0u) {
				key          = current_key;
				table_offset = offset;
				heap_stride  = current_stride;
			} else if (!EquivalentValue(m_program, key, current_key) ||
			           current_stride != heap_stride ||
			           static_cast<uint64_t>(table_offset) + dword * sizeof(uint32_t) != offset) {
				return false;
			}
			table_handle       = current_handle;
			plan.memory[dword] = memory_index;
			plan.reads[dword]  = read;
		}
		// One record may feed several handles: RE9 samples the same bindless texture more than
		// once. Every reader must be a sibling that plans identically, or the record is not dead
		// after the rewrite.
		bool kept_reads = false;
		for (uint32_t dword = 0; dword < plan.reads.size(); dword++) {
			if (plan.reads[dword] == nullptr) {
				return false;
			}
			plan.kept[dword] = !ReadIsPrivateToSiblings(*plan.reads[dword], plan.reads);
			kept_reads |= plan.kept[dword];
		}

		DescriptorSource table_source;
		if (!MakeRuntimeTableSource(*plan.reads[0], table_source)) {
			return false;
		}
		DescriptorSource                     material_source;
		DescriptorSource::IndirectDescriptor indirect;
		indirect.table_offset   = table_offset;
		indirect.heap_stride    = heap_stride;
		indirect.workgroup_axis = WorkgroupAxis(key);
		if (indirect.workgroup_axis != UINT32_MAX) {
			// The descriptor also supplies dimensions to shader arithmetic. Keep its reads;
			// only the image handle is projected onto the bounded workgroup key.
			plan.reads.fill(nullptr);
		} else if (table_source.dword_count == 2u) {
			const auto* selector = key.Resolve().TryInstruction();
			const bool  bitscan  = selector != nullptr &&
			                       selector->GetOpcode() == ValueOpcode::FindILsb32 &&
			                       selector->NumArgs() == 1u && !m_shader_writes &&
			                       NonzeroOnEntry(selector->Arg(0), handle.Parent());
			if (bitscan) {
				indirect.key_count = Value(32u);
			} else {
				indirect.key_count = BoundedLoopCount(key, handle.Parent());
				if (indirect.key_count.IsEmpty() && !m_shader_writes) {
					if (const auto* bound = BoundedLoop(key, handle.Parent()))
						indirect.key_count = bound->Arg(1);
				}
			}
			if (indirect.key_count.IsEmpty() &&
			    !MatchUniformizedMaterialKey(key, handle, indirect, material_source)) {
				return false;
			}
			// A raw-address heap reaches its records through the shader's own byte arithmetic
			// and its bounds are whatever the enumeration's own limit says, so this branch stays
			// at the 32-byte record step it was proven on.
			if (heap_stride != DescriptorBytes || (table_offset & 3u) != 0u ||
			    (bitscan && table_offset > UINT32_MAX - (32u * 32u - 1u)))
				return false;
		} else {
			// The descriptor has to sit inside the record the stride steps over: a heap of plain
			// T# strides by 32 at offset 0, and an engine that keeps a T# inside a larger record
			// strides by the record and offsets into it. Anything looser would let consecutive
			// keys name overlapping bytes, and the host enumeration would walk a table it has
			// misread.
			if (heap_stride < DescriptorBytes || table_offset > heap_stride - DescriptorBytes) {
				return false;
			}
			const auto match_material = [&] {
				// RE Engine packs one material word: the low bits name the heap record and the rest
				// are unrelated fields, so the shader masks before indexing. Read through the mask
				// to reach the material word, and record it - the host must narrow the key the same
				// way or it probes the heap at a packed word times the record stride, far out of
				// bounds. `key` itself stays the masked value, which is what the emitted search
				// compares.
				auto*       material_read         = MaterialKeyWord(key, indirect).TryInstruction();
				uint32_t    material_memory_index = 0;
				const auto* memory = material_read != nullptr
				                         ? ScalarReadMemory(*material_read, material_memory_index)
				                         : nullptr;
				if (memory == nullptr || memory->kind != ResourceKind::ScalarBuffer ||
				    memory->offset > INT32_MAX ||
				    !MemoryIndexBelongsTo(material_memory_index, *material_read)) {
					return false;
				}
				// The material word need not sit at the start of its record. The enumeration
				// reaches it through selector_offset, which folds this immediate in below the way
				// the hardware aligns it; the raw value is kept to tell apart tables that differ
				// only here.
				indirect.material_offset = memory->offset;
				Value                          selector;
				DescriptorSource::SelectorKind selector_kind =
				    DescriptorSource::SelectorKind::Stride;
				uint32_t selector_mask = 0;
				if (!MatchMaterialOffset(material_read->Arg(1), selector, selector_kind,
				                         indirect.selector_stride, indirect.selector_offset,
				                         selector_mask, false, true) ||
				    selector_kind != DescriptorSource::SelectorKind::Stride) {
					// A table the bindless heap serves anyway keeps that path.
					if ((BindlessImageHeapsEnabled() && indirect.selector_shift == 0u) ||
					    heap_stride != DescriptorBytes || table_offset != 0u ||
					    !MatchAffineMaterialOffset(material_read->Arg(1), indirect.selector_stride,
					                               indirect.selector_offset)) {
						return false;
					}
					indirect.affine_selector = true;
				}
				const auto step = std::gcd<uint64_t>(indirect.selector_stride, uint64_t {1} << 32u);
				indirect.selector_offset =
				    (static_cast<uint32_t>(indirect.selector_offset % step) & ~3u) +
				    (memory->offset & ~3u);
				// Only the heap reads are marked planning-only, so the material read stays emitted
				// and its other readers keep working: RE Engine also compares the key against a -1
				// sentinel.
				const auto* shift = plan.reads[0]->Arg(1).Resolve().TryInstruction();
				if (!UsedOnlyByReadsAndArithmetic(*shift, plan.reads)) {
					return false;
				}
				const auto* material_handle = material_read->Arg(0).Resolve().TryInstruction();
				if (material_handle == nullptr ||
				    material_handle->GetOpcode() != ValueOpcode::GetBufferResource ||
				    !MakeRuntimeTableSource(*material_read, material_source)) {
					return false;
				}
				indirect.material_source = InternSource(material_source);
				// The heap is indexed by the key the shader computes, however it unpacks the
				// material word; the shift and mask only shape the host enumeration.
				indirect.bindless = BindlessImageHeapsEnabled();
				return true;
			};
			const auto unmatched = indirect;
			if (MatchScalarMaterialKey(key, indirect, material_source)) {
				// The key and T# payload stay on the GPU. The bindless heap serves the loop's
				// records directly, so their numeric classes may differ; an access it cannot serve
				// falls back to the host enumerating the guarded scalar loop.
				plan.reads.fill(nullptr);
				indirect.bindless = BindlessImageHeapsEnabled();
			} else if (!match_material()) {
				indirect        = unmatched;
				material_source = {};
			}
			const bool uniformized_key =
			    indirect.material_source == UINT32_MAX &&
			    MatchUniformizedBufferKey(key, handle, indirect, material_source);
			if (uniformized_key) {
				indirect.bindless = BindlessImageHeapsEnabled();
			} else if (indirect.material_source == UINT32_MAX) {
				const auto* shift = plan.reads[0]->Arg(1).Resolve().TryInstruction();
				if (!BindlessImageHeapsEnabled() || shift == nullptr ||
				    !UsedOnlyByReadsAndArithmetic(*shift, plan.reads)) {
					return false;
				}
				indirect              = {};
				indirect.table_offset = table_offset;
				indirect.heap_stride  = heap_stride;
				indirect.bindless     = true;
				material_source       = {};
			}
		}
		if (kept_reads && plan.reads[0] != nullptr && !indirect.bindless &&
		    indirect.workgroup_axis == UINT32_MAX) {
			// A record may also reach other image handles through phis; they plan on their own.
			for (uint32_t dword = 0; dword < plan.reads.size(); dword++) {
				if (plan.kept[dword] && !UsesOnlyImageDescriptors(*plan.reads[dword])) {
					return false;
				}
			}
			plan.kept.fill(false);
		}
		indirect.table_source = InternSource(table_source);
		DescriptorSource image_source;
		image_source.dword_count = 8u;
		image_source.dwords.fill(Value(0u));
		std::copy_n(material_source.dwords.begin(), material_source.dword_count,
		            image_source.dwords.begin());
		std::copy_n(table_source.dwords.begin(), table_source.dword_count,
		            image_source.dwords.begin() + 4u);
		image_source.indirect_descriptor = indirect;
		plan.handle                      = &handle;
		plan.source                      = InternSource(image_source);
		plan.key                         = key;
		plan.roots                       = image_source.dwords;
		return true;
	}

	struct WaterfallWord {
		const Inst* load = nullptr;
		uint32_t    byte = 0;
		Value       lane;
	};

	bool MatchWaterfallWord(Value value, WaterfallWord& word) const {
		const auto* read = value.Resolve().TryInstruction();
		if (read == nullptr || read->GetOpcode() != ValueOpcode::ReadLane || read->NumArgs() != 2u)
			return false;
		word.lane          = read->Arg(1).Resolve();
		auto        lifted = read->Arg(0).Resolve();
		const auto* select = lifted.TryInstruction();
		Value       select_condition;
		if (select != nullptr && select->GetOpcode() == ValueOpcode::SelectU32) {
			select_condition = select->Arg(0).Resolve();
			lifted           = select->Arg(1).Resolve();
		}
		const auto* load      = lifted.TryInstruction();
		uint32_t    component = 0;
		if (load != nullptr && (load->GetOpcode() == ValueOpcode::CompositeExtractU32x4 ||
		                        load->GetOpcode() == ValueOpcode::CompositeExtractU32x2)) {
			if (!ImmediateU32(load->Arg(1), component)) return false;
			load = load->Arg(0).Resolve().TryInstruction();
		}
		if (load == nullptr || load->NumArgs() != 5u ||
		    (load->GetOpcode() != ValueOpcode::LoadBufferU32 &&
		     load->GetOpcode() != ValueOpcode::LoadBufferU32x2 &&
		     load->GetOpcode() != ValueOpcode::LoadBufferU32x4))
			return false;
		const auto flags = load->Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) return false;
		const auto& memory  = m_program.memory_info[flags.index];
		uint32_t    voffset = 0;
		uint32_t    soffset = 0;
		if (memory.kind != ResourceKind::Buffer || !memory.idxen || memory.offen || memory.typed ||
		    memory.formatted || memory.data_bits != 32u || component >= memory.data_dwords ||
		    !ImmediateU32(load->Arg(2), voffset) || voffset != 0u ||
		    !ImmediateU32(load->Arg(3), soffset) || soffset != 0u ||
		    memory.offset > UINT32_MAX - component * sizeof(uint32_t))
			return false;
		if (!select_condition.IsEmpty() &&
		    !EquivalentValue(m_program, select_condition, load->Arg(4)))
			return false;
		word.load = load;
		word.byte = memory.offset + component * static_cast<uint32_t>(sizeof(uint32_t));
		return true;
	}

	// A waterfall loop's V_READLANE of an indexed BUFFER_LOAD is the heap record at that lane.
	bool TryMakeWaterfallImage(Inst& handle, IndirectDescriptorPlan& plan) {
		if (!BindlessImageHeapsEnabled() || handle.GetOpcode() != ValueOpcode::GetImageResource ||
		    handle.NumArgs() != 8u)
			return false;
		for (const auto& use: handle.Uses()) {
			const auto op = use.user->GetOpcode();
			if (op != ValueOpcode::ImageSampleRaw && op != ValueOpcode::ImageRead) return false;
			const auto flags = use.user->Flags<MemoryFlags>();
			if (flags.index >= m_program.memory_info.size()) return false;
			const auto& memory = m_program.memory_info[flags.index];
			if ((memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0u ||
			    memory.image_r128)
				return false;
		}
		const auto same_heap = [&](const Inst& left, const Inst& right) {
			const auto* a = left.Arg(0).Resolve().TryInstruction();
			const auto* b = right.Arg(0).Resolve().TryInstruction();
			if (a == nullptr || b == nullptr || a->GetOpcode() != ValueOpcode::GetBufferResource ||
			    b->GetOpcode() != ValueOpcode::GetBufferResource)
				return false;
			for (uint32_t arg = 0; arg < 4u; arg++) {
				if (!EquivalentValue(m_program, a->Arg(arg), b->Arg(arg))) return false;
			}
			return true;
		};
		std::array<WaterfallWord, 8> words;
		for (uint32_t dword = 0; dword < words.size(); dword++) {
			if (!MatchWaterfallWord(handle.Arg(dword), words[dword])) return false;
			const auto& first = words[0];
			const auto& word  = words[dword];
			if (dword == 0u) {
				if ((word.byte & 3u) != 0u) return false;
				continue;
			}
			if (static_cast<uint64_t>(first.byte) + dword * sizeof(uint32_t) != word.byte ||
			    !EquivalentValue(m_program, first.lane, word.lane) ||
			    !same_heap(*first.load, *word.load) ||
			    !EquivalentValue(m_program, first.load->Arg(1), word.load->Arg(1)) ||
			    !EquivalentValue(m_program, first.load->Arg(4), word.load->Arg(4)))
				return false;
		}
		DescriptorSource table_source;
		if (!MakeRuntimeTableSource(*words[0].load, table_source) || table_source.dword_count != 4u)
			return false;

		DescriptorSource::IndirectDescriptor indirect;
		indirect.table_source = InternSource(table_source);
		indirect.table_offset = words[0].byte;
		// The record step is the heap V#'s stride, known only once the descriptor is bound.
		indirect.heap_stride  = 1u;
		indirect.indexed_heap = true;
		indirect.bindless     = true;

		// A lane the load skipped never fetched a record, so its key names none.
		auto*      block = handle.Parent();
		const auto where = std::ranges::find_if(block->Instructions(),
		                                        [&](const Inst& inst) { return &inst == &handle; });
		const auto* load = words[0].load;
		Value       index  = load->Arg(1).Resolve();
		const auto  active = load->Arg(4).Resolve();
		if (!(active.IsImmediate() && active.GetType() == Type::U1 && active.U1())) {
			index = Value(&*block->PrependNewInst(where, ValueOpcode::SelectU32,
			                                      {active, index, Value(0xffffffffu)}));
		}
		const Value key(
		    &*block->PrependNewInst(where, ValueOpcode::ReadLane, {index, words[0].lane}));

		DescriptorSource image_source;
		image_source.dword_count = 8u;
		image_source.dwords.fill(Value(0u));
		std::copy_n(table_source.dwords.begin(), 4u, image_source.dwords.begin() + 4u);
		image_source.indirect_descriptor = indirect;
		plan.handle                      = &handle;
		plan.source                      = InternSource(image_source);
		plan.key                         = key;
		plan.roots                       = image_source.dwords;
		// The record reads stay: the loop compares them to retire lanes.
		plan.reads.fill(nullptr);
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
				    SelectorMaximum(inst->Arg(arg ^ 1u), bounds, depth + 1u) < target)
					return true;
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
				           ? maximum(2)
				           : std::max(maximum(1), maximum(2));
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftLeftLogical32: {
				uint32_t shift;
				if (!ImmediateU32(inst->Arg(1), shift) || shift >= 32u) break;
				if (inst->GetOpcode() == ValueOpcode::ShiftRightLogical32)
					return maximum(0) >> shift;
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
		bool       found   = false;
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
			    if (op != ValueOpcode::ImageSampleRaw)
				    return sampler || op != ValueOpcode::ImageGatherRaw;
			    return sampler && (m_program.memory_info[use.user->Flags<MemoryFlags>().index]
			                           .image_sample_flags &
			                       Decoder::ImageSampleFlagAdjust) != 0u;
		    }))
			return false;
		bool has_phi = false;
		for (size_t word = 0; word < handle.NumArgs(); ++word)
			has_phi |= handle.Arg(word).Resolve().IsPhi();
		if (!has_phi) return false;
		// Normalize synchronized descriptor words to one GPU ordinal.
		// Build the complete graph before changing IR so an unsupported leaf is transactional.
		struct Choice {
			DescriptorSource      descriptor;
			const Inst*           branch = nullptr;
			std::vector<uint32_t> children;
			Value                 key;
		};
		std::vector<Choice> choices;
		const auto          visit = [&](const auto& self, DescriptorSource descriptor) -> uint32_t {
			for (auto& word: descriptor.dwords)
				word = word.Resolve();
			for (uint32_t i = 0; i < choices.size(); ++i)
				if (choices[i].descriptor.dwords == descriptor.dwords) return i;
			const auto index = static_cast<uint32_t>(choices.size());
			choices.push_back({descriptor});
			uint32_t bad = 0;
			if (ValidateSource(descriptor, bad)) {
				// Finite inline samplers have literal leaves; live descriptor tables use the
				// existing direct sampler planning path.
				if (sampler &&
				    !std::ranges::all_of(
				        std::span(descriptor.dwords).first(descriptor.dword_count), [](Value word) {
					        return word.IsImmediate() && word.GetType() == Type::U32;
				        }))
					return UINT32_MAX;
				return index;
			}
			const auto* branch = descriptor.dwords[bad].TryInstruction();
			if (branch == nullptr || branch->Parent() == nullptr ||
			    branch->GetOpcode() != ValueOpcode::Phi ||
			    branch->NumPhiBlocks() != branch->NumArgs())
				return UINT32_MAX;
			choices[index].branch = branch;
			const auto count      = branch->NumArgs();
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
						if (!ValidateRuntimeValue(m_program, descriptor.dwords[word],
						                          RuntimeValueType::Any, nullptr,
						                          &m_cyclic_entries))
							return UINT32_MAX;
						continue;
					}
					if (selected->NumArgs() != count || selected->NumPhiBlocks() != count)
						return UINT32_MAX;
					uint32_t incoming = 0;
					while (incoming < count &&
					       selected->PhiBlock(incoming) != branch->PhiBlock(arm))
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
		for (uint32_t word = 0; word < root.dword_count; ++word)
			root.dwords[word] = handle.Arg(word);
		if (visit(visit, root) == UINT32_MAX || choices.front().branch == nullptr) return false;
		DescriptorSource finite_source;
		finite_source.dword_count = root.dword_count;
		finite_source.dwords.fill(Value(0u));
		finite_source.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor {});
		auto& sources = finite_source.indirect_descriptor->sources;
		for (auto& choice: choices) {
			if (choice.branch != nullptr) continue;
			const auto source = InternSource(choice.descriptor);
			auto       found  = std::ranges::find(sources, source);
			choice.key        = Value(static_cast<uint32_t>(found - sources.begin()));
			if (found == sources.end()) sources.push_back(source);
		}
		if (sources.empty()) return false;
		for (auto& choice: choices) {
			if (choice.branch == nullptr) continue;
			auto*      block = choice.branch->Parent();
			const auto where = std::ranges::find_if(
			    block->Instructions(), [&](const Inst& inst) { return &inst == choice.branch; });
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
		plan.key    = choices.front().key;
		plan.roots  = finite_source.dwords;
		plan.reads.fill(nullptr);
		return true;
	}

	const IndirectDescriptorPlan* FindIndirectDescriptor(const Inst& handle) const {
		const auto found = std::find_if(
		    m_indirect_descriptors.begin(), m_indirect_descriptors.end(),
		    [&](const IndirectDescriptorPlan& plan) { return plan.handle == &handle; });
		return found == m_indirect_descriptors.end() ? nullptr : &*found;
	}

	bool IsIndirectPlanningMemory(uint32_t index) const {
		for (const auto& plan: m_indirect_descriptors) {
			if (plan.reads[0] == nullptr) {
				continue;
			}
			for (uint32_t dword = 0; dword < plan.memory.size(); dword++) {
				if (plan.memory[dword] == index && !plan.kept[dword]) {
					return true;
				}
			}
		}
		return false;
	}

	void PlanIndirectDescriptors() {
		IndexMemoryAccessors();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None ||
				    inst.NumArgs() == 0u) {
					continue;
				}
				const auto  flags  = inst.Flags<MemoryFlags>();
				const auto& memory = m_program.memory_info[flags.index];
				auto*       handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle != nullptr && FindIndirectDescriptor(*handle) == nullptr) {
					IndirectDescriptorPlan plan;
					if (TryMakeIndirectImage(*handle, memory.resource * 4u, flags.pc, plan) ||
					    TryMakeFiniteDescriptor(*handle, plan) ||
					    TryMakeWaterfallImage(*handle, plan)) {
						m_indirect_descriptors.push_back(std::move(plan));
					}
				}
				// A sampler the shader picks out of a finite set of literal samplers.
				if (inst.GetOpcode() != ValueOpcode::ImageSampleRaw ||
				    (memory.image_sample_flags & Decoder::ImageSampleFlagAdjust) != 0u) {
					continue;
				}
				auto* sampler = inst.Arg(1).Resolve().TryInstruction();
				if (sampler == nullptr || FindIndirectDescriptor(*sampler) != nullptr) {
					continue;
				}
				DescriptorSource descriptor;
				MakeSource(*sampler, 4u, true, false, memory.sampler * 4u, descriptor, flags.pc);
				uint32_t               bad_dword = 0;
				IndirectDescriptorPlan plan;
				if (!ValidateSource(descriptor, bad_dword) &&
				    TryMakeFiniteDescriptor(*sampler, plan)) {
					m_indirect_descriptors.push_back(std::move(plan));
				}
			}
		}
		m_memory_accessors.clear();
	}

	// Recognizes a buffer V# assembled from four consecutive scalar reads of one descriptor table
	// at a wave-uniform record index. Only the index resists host evaluation; the table's own V#
	// is CPU-derivable, so the host can enumerate the records it may select.
	bool TryMakeIndirectBuffer(Inst& handle, uint32_t pc, uint32_t base_reg,
	                           IndirectBufferPlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetBufferResource || handle.NumArgs() != 4u) {
			return false;
		}
		Inst*    heap_handle = nullptr;
		Value    heap_offset;
		uint32_t record_offset = 0;
		for (uint32_t dword = 0; dword < 4u; dword++) {
			auto* read = handle.Arg(dword).Resolve().TryInstruction();
			if (read == nullptr) {
				return false;
			}
			uint32_t    memory_index = 0;
			const auto* memory       = ScalarReadMemory(*read, memory_index);
			if (memory == nullptr) {
				return false;
			}
			if (dword == 0u) {
				record_offset = memory->offset;
				heap_offset   = read->Arg(1).Resolve();
				if (record_offset > UINT32_MAX - 3u * sizeof(uint32_t)) {
					return false;
				}
			} else if (memory->offset != record_offset + dword * sizeof(uint32_t) ||
			           !EquivalentValue(m_program, heap_offset, read->Arg(1))) {
				return false;
			}
			auto* current = read->Arg(0).Resolve().TryInstruction();
			if (current == nullptr || (heap_handle != nullptr && current != heap_handle)) {
				return false;
			}
			heap_handle = current;
		}

		Value    selector;
		auto     selector_kind   = DescriptorSource::SelectorKind::Stride;
		uint32_t selector_stride = 0;
		uint32_t selector_offset = 0;
		uint32_t selector_mask   = 0;
		// Only the image heap enumerates a masked selector; a buffer table keeps the stride shape.
		// A compute shader indexes this table entirely in the scalar unit, so accept a selector
		// that is uniform by construction as well as one a lane broadcast made uniform.
		if (!MatchMaterialOffset(heap_offset, selector, selector_kind, selector_stride,
		                         selector_offset, selector_mask, true) ||
		    selector_kind != DescriptorSource::SelectorKind::Stride) {
			return false;
		}

		// A selector the host can re-execute names one record directly, so leave that descriptor
		// on the ordinary path: recognizing the table must not change a shader that already
		// materializes. A loop-carried selector only looks re-executable because the validator
		// resolves its phi to the value the loop is entered with - the first iteration's record,
		// not the descriptor the later ones select. Recognize the table for those instead.
		DescriptorSource direct;
		MakeSource(handle, 4u, false, false, base_reg, direct, pc);
		uint32_t bad_dword = 0;
		if (ValidateSource(direct, bad_dword) && !SelectorIsLoopCarried(heap_offset)) {
			return false;
		}

		DescriptorSource heap_source;
		uint32_t         heap_source_index = 0;
		if (!MakeRuntimeBufferSource(*heap_handle, pc, heap_source_index, heap_source)) {
			return false;
		}

		// The source stands for the table, not for any one record: its dwords are the table V#,
		// which the host can re-execute, and the record index lives in the indirect description.
		DescriptorSource buffer_source;
		buffer_source.dword_count = 4u;
		std::copy_n(heap_source.dwords.begin(), 4u, buffer_source.dwords.begin());
		buffer_source.indirect_buffer = DescriptorSource::IndirectBuffer {
		    heap_source_index, selector_stride, selector_offset, record_offset, 0u};

		plan.handle = &handle;
		plan.source = InternSource(buffer_source);
		// Arg 0 becomes the record index once the plan is applied, so the key the shader computed
		// stays reachable at emission - that is what lets a table too large to enumerate report
		// the records it actually selected.
		plan.key = selector;
		return true;
	}

	const IndirectBufferPlan* FindIndirectBuffer(const Inst& handle) const {
		if (m_indirect_buffers.empty()) {
			return nullptr;
		}
		const auto found =
		    std::find_if(m_indirect_buffers.begin(), m_indirect_buffers.end(),
		                 [&](const IndirectBufferPlan& plan) { return plan.handle == &handle; });
		return found == m_indirect_buffers.end() ? nullptr : &*found;
	}

	void PlanIndirectBuffers() {
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (BufferAccessOf(inst.GetOpcode()) == BufferAccess::None ||
				    inst.NumArgs() == 0u) {
					continue;
				}
				auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || FindIndirectBuffer(*handle) != nullptr) {
					continue;
				}
				const auto         flags = inst.Flags<MemoryFlags>();
				IndirectBufferPlan plan;
				if (flags.index < m_program.memory_info.size() &&
				    TryMakeIndirectBuffer(*handle, flags.pc,
				                          m_program.memory_info[flags.index].resource * 4u, plan)) {
					m_indirect_buffers.push_back(plan);
				}
			}
		}
	}

	void DemoteRefusedTables() {
		if (m_indirect_buffers.empty()) {
			return;
		}
		// A table the materialization would refuse right now is demoted before the first
		// translation binds it, so the shader is translated once instead of refused and rebuilt.
		// Decided by the table's contents, never by its layout: a table of the same shape that
		// fits stays an enumerated binding.
		if (m_program.refuse_indirect_buffer_table) {
			std::unordered_set<uint32_t> checked;
			for (const auto& plan: m_indirect_buffers) {
				if (m_demoted_tables.contains(plan.source) || !checked.insert(plan.source).second) {
					continue;
				}
				if (m_program.refuse_indirect_buffer_table(m_program, m_sources, plan.source)) {
					m_demoted_tables.insert(plan.source);
				}
			}
		}
		if (m_program.unfoldable_pcs.empty()) {
			return;
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (BufferAccessOf(inst.GetOpcode()) == BufferAccess::None ||
				    inst.NumArgs() == 0u ||
				    std::ranges::find(m_program.unfoldable_pcs, inst.Flags<MemoryFlags>().pc) ==
				        m_program.unfoldable_pcs.end()) {
					continue;
				}
				const auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (const auto* plan = handle != nullptr ? FindIndirectBuffer(*handle) : nullptr;
				    plan != nullptr) {
					m_demoted_tables.insert(plan->source);
				}
			}
		}
	}

	bool IsDemotedTable(const IndirectBufferPlan& plan) const {
		return m_demoted_tables.contains(plan.source);
	}

	bool RejectFormattedDemotion(uint32_t pc) const {
		return Reject(pc, "a refused buffer table's formatted access has no in-shader format "
		                  "decode");
	}

	// A handle on a non-invariant loop phi: the one shape whose fixpoint a walk can fail to prove.
	bool CarriesLoopPhi(const Inst& handle) const {
		std::vector<const Inst*>        pending;
		std::unordered_set<const Inst*> seen;
		const auto                      push = [&](const Inst& inst) {
			for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
				if (const auto* operand = inst.Arg(arg).Resolve().TryInstruction();
				    operand != nullptr) {
					pending.push_back(operand);
				}
			}
		};
		push(handle);
		while (!pending.empty() && seen.size() < 256u) {
			const auto* inst = pending.back();
			pending.pop_back();
			if (!seen.insert(inst).second) {
				continue;
			}
			if (inst->GetOpcode() == ValueOpcode::Phi) {
				if (ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(inst))).IsEmpty()) {
					return true;
				}
				continue;
			}
			push(*inst);
		}
		return false;
	}

	bool TakeDynamicBuffer(const Inst* root, const MemoryInfo& memory, uint32_t index,
	                       BufferAccess buffer, uint32_t pc) {
		if (root == nullptr || root->GetOpcode() != ValueOpcode::GetBufferResource) {
			return false;
		}
		DescriptorSource descriptor;
		MakeSource(*root, 4u, false, false, memory.resource * 4u, descriptor, pc);
		if (!IsDynamicDescriptor(descriptor)) {
			return false;
		}
		m_program.memory_info[index].dynamic_buffer = true;
		m_info.uses_dma                             = true;
		if (DmaPointer pointer;
		    buffer != BufferAccess::Read && RelativeWriteDecode() &&
		    RelativeStoreHandle(*root, &pointer) &&
		    std::ranges::find(m_info.dma_pointers, pointer) == m_info.dma_pointers.end()) {
			m_info.dma_pointers.push_back(pointer);
		}
		if (buffer != BufferAccess::Read) {
			// The store-pointer helper is only emitted, and its writable set only published,
			// when the program declares that it writes through DMA.
			m_program.has_address_writes = true;
			m_info.writes_dma            = true;
		}
		return true;
	}

	bool GetHandle(Value value, ValueOpcode expected, uint32_t width, uint32_t pc,
	               uint32_t base_reg, Inst*& handle, uint32_t& source, bool sampler = false,
	               bool sample_adjust = false) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != expected) {
			return Reject(pc,
			              fmt::format("memory operation requires {}", ValueOpcodeName(expected)));
		}
		DescriptorSource descriptor;
		MakeSource(*handle, width, sampler, sample_adjust, base_reg, descriptor, pc);
		uint32_t            bad_dword = 0;
		RuntimeValueFailure failure;
		if (!ValidateSource(descriptor, bad_dword, &failure)) {
			const auto detail =
			    fmt::format("{} dword {} is not a valid runtime value ({})",
			                ValueOpcodeName(expected), bad_dword, DescribeRuntimeFailure(failure));
			// A raw all-DWORD buffer descriptor the walk cannot fold is not a failure: the caller
			// reclassifies it as a GPU-selected buffer and reads it on the device. Keep why the
			// fold failed, because if no mechanism takes the access that is the whole diagnosis.
			if (expected == ValueOpcode::GetBufferResource &&
			    std::all_of(descriptor.dwords.begin(), descriptor.dwords.begin() + width,
			                [](Value word) { return word.Resolve().GetType() == Type::U32; })) {
				m_deferred_reject = detail;
				return false;
			}
			return Reject(pc, detail);
		}
		m_deferred_reject.clear();
		source = InternSource(descriptor);
		return true;
	}

	// An unmapped BDA page reads zero and drops stores until its fault is processed.
	void RecordDmaPointer(const Inst& handle, bool store) {
		DmaPointer pointer;
		pointer.store = store;
		for (uint32_t word = 0; word < 2u; word++) {
			const auto* source = handle.Arg(word).Resolve().TryInstruction();
			if (source == nullptr) {
				return;
			}
			uint32_t index = 0;
			bool     user  = false;
			if (source->GetOpcode() == ValueOpcode::ReadConst) {
				const auto slot = source->Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32) {
					return;
				}
				index = slot.U32();
			} else if (source->GetOpcode() == ValueOpcode::GetUserData &&
			           source->Arg(0).GetType() == Type::ScalarReg) {
				index = RegIndex(source->Arg(0).ScalarRegister());
				user  = true;
			} else {
				return;
			}
			if (word == 0u) {
				pointer.user_data = user;
				pointer.lo        = index;
			} else if (user != pointer.user_data) {
				return;
			} else {
				pointer.hi = index;
			}
		}
		if (std::ranges::find(m_info.dma_pointers, pointer) == m_info.dma_pointers.end()) {
			m_info.dma_pointers.push_back(pointer);
		}
	}

	bool ValidateAddressHandle(Value value, uint32_t pc) const {
		const auto* handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource) {
			return Reject(pc, "address operation requires GetAddressResource");
		}
		if (handle->NumArgs() != 2) {
			Fail(pc, "GetAddressResource must have two address dwords");
		}
		return true;
	}

	uint32_t AddBuffer(uint32_t source, const MemoryInfo& memory, const Inst& inst, uint32_t pc) {
		const auto op    = inst.GetOpcode();
		const auto index = inst.NumArgs() > 1 ? inst.Arg(1).Resolve() : Value(0u);
		const bool indexed =
		    op != ValueOpcode::ReadConstBuffer && !(index.IsImmediate() && index.U32() == 0u);
		for (uint32_t i = 0; i < m_info.buffers.size(); i++) {
			if (m_info.buffers[i].source == source) {
				Merge(m_info.buffers[i], memory, op, pc);
				m_info.buffers[i].unindexed = m_info.buffers[i].unindexed && !indexed;
				return i;
			}
		}
		if (m_info.buffers.size() >= ShaderInfo::MaxBuffers) {
			return UINT32_MAX;
		}
		BufferResource resource;
		resource.source       = source;
		resource.first_use_pc = pc;
		resource.unindexed    = !indexed;
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
		const bool atomic64 =
		    ImageOpcodeInfoOf(op).access == ImageAccess::Atomic && memory.data_bits == 64u;
		const bool dynamic_mip =
		    (resource_class == ImageResourceClass::Storage && memory.image_has_mip) ||
		    (op == ValueOpcode::ImageGatherRaw &&
		     (memory.image_sample_flags & Decoder::ImageSampleFlagLod) != 0u);
		const auto mip   = dynamic_mip ? ImageMipMode::Dynamic : ImageMipMode::None;
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
		m_info.samplers.push_back({source, pc});
		const auto  root       = static_cast<uint32_t>(m_info.samplers.size() - 1);
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

	bool AddSampledPair(uint32_t image, uint32_t sampler, uint32_t pc) {
		for (auto& pair: m_info.sampled_pairs) {
			if (pair.image == image && pair.sampler == sampler) {
				pair.first_use_pc = std::min(pair.first_use_pc, pc);
				return true;
			}
		}
		if (m_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
			return Reject(pc, "sampled image/sampler pair limit exceeded");
		}
		m_info.sampled_pairs.push_back({image, sampler, pc});
		return true;
	}

	bool AddHandlePatch(Inst* handle, uint32_t resource, uint32_t pc) {
		for (const auto& patch: m_handle_patches) {
			if (patch.handle == handle) {
				if (patch.resource != resource) {
					return Reject(pc, fmt::format("{} is reused with incompatible resource classes",
					                              ValueOpcodeName(handle->GetOpcode())));
				}
				return true;
			}
		}
		m_handle_patches.push_back({handle, resource});
		return true;
	}

	bool AddMemoryPatch(uint32_t index, uint32_t resource, uint32_t sampler, bool has_sampler,
	                    uint32_t pc) {
		for (auto& patch: m_memory_patches) {
			if (patch.index != index) {
				continue;
			}
			if (patch.resource != resource ||
			    (has_sampler && patch.has_sampler && patch.sampler != sampler)) {
				return Reject(pc, "memory metadata is reused with incompatible resources");
			}
			if (has_sampler) {
				patch.sampler     = sampler;
				patch.has_sampler = true;
			}
			return true;
		}
		m_memory_patches.push_back({index, resource, sampler, has_sampler});
		return true;
	}

	bool Collect(Inst& inst) {
		const auto op = inst.GetOpcode();
		if (op == ValueOpcode::BvhIntersect) {
			m_info.uses_dma = true;
			return true;
		}
		const auto buffer       = BufferAccessOf(op);
		const auto address_info = AddressOpcodeInfoOf(op);
		const auto image_info   = ImageOpcodeInfoOf(op);
		if (buffer == BufferAccess::None && address_info.access == AddressAccess::None &&
		    image_info.access == ImageAccess::None) {
			return true;
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
			return true;
		}
		Inst*    handle   = nullptr;
		uint32_t source   = 0;
		uint32_t resource = 0;

		if (buffer != BufferAccess::None) {
			// 4. A scalar, DWORD x1 or formatted X load the others refused decodes its V# itself.
			const auto take_indirect_load = [&]() {
				if (memory.kind != (op == ValueOpcode::ReadConstBuffer ? ResourceKind::ScalarBuffer
				                                                       : ResourceKind::Buffer) ||
				    !memory.SupportsIndirectBufferLoad(op)) {
					return false;
				}
				m_program.memory_info[flags.index].kind = ResourceKind::IndirectBuffer;
				m_info.uses_dma                         = true;
				return true;
			};
			if (!GetHandle(inst.Arg(0), ValueOpcode::GetBufferResource, 4, flags.pc,
			               memory.resource * 4u, handle, source)) {
				// Three mechanisms, layered narrow-first: each takes only what the ones above it
				// cannot express, so no access that resolves today moves onto a wider one.
				// 1. A raw, unformatted DWORD x2/x3/x4 load decodes the V# over a device pointer.
				const bool raw_multi_dword = op == ValueOpcode::LoadBufferU32x2 ||
				                             op == ValueOpcode::LoadBufferU32x3 ||
				                             op == ValueOpcode::LoadBufferU32x4;
				if (memory.kind == ResourceKind::Buffer && !memory.formatted && raw_multi_dword &&
				    memory.SupportsIndirectBufferLoad(op)) {
					m_program.memory_info[flags.index].kind = ResourceKind::IndirectBuffer;
					m_info.uses_dma                         = true;
					return true;
				}
				auto*       root     = inst.Arg(0).Resolve().TryInstruction();
				const auto* indirect = root != nullptr ? FindIndirectBuffer(*root) : nullptr;
				if (indirect == nullptr || IsDemotedTable(*indirect)) {
					if (indirect != nullptr && (memory.formatted || memory.typed)) {
						// Only 4 decodes a format in the shader.
						return take_indirect_load() || RejectFormattedDemotion(flags.pc);
					}
					// 3. Any other descriptor sourced from memory: decode the V# with full
					// addressing, reaching the stores, atomics, subword and formatted accesses 1
					// and 2 cannot.
					if (TakeDynamicBuffer(root, memory, flags.index, buffer, flags.pc)) {
						return true;
					}
					if (indirect == nullptr && take_indirect_load()) {
						return true;
					}
					return Reject(flags.pc,
					              fmt::format("{}; no mechanism takes it: GPU-selected access "
					                          "requires a raw DWORD x2/x3/x4 load, a recognisable "
					                          "table, or descriptor dwords sourced from memory",
					                          m_deferred_reject.empty()
					                              ? "buffer descriptor is not a valid runtime value"
					                              : m_deferred_reject));
				}
				// 2. A recognisable table resolves on the host into real, deduped bindings.
				handle = root;
				source = indirect->source;
				m_applied_indirect_buffers.insert(root);
			} else if (auto* carried = inst.Arg(0).Resolve().TryInstruction(); carried != nullptr) {
				// A plan outlives a successful GetHandle only when its selector carries a phi the
				// recognizer could not fold: it declines whenever the descriptor validates and the
				// selector does not vary. The validator accepted that phi as the value the loop is
				// entered with and left the evaluator to prove a fixpoint a cursor never reaches,
				// so bind the table rather than the one record the first iteration selects.
				if (const auto* planned = FindIndirectBuffer(*carried); planned != nullptr) {
					if (IsDemotedTable(*planned)) {
						if (memory.formatted || memory.typed) {
							return RejectFormattedDemotion(flags.pc);
						}
						if (TakeDynamicBuffer(carried, memory, flags.index, buffer, flags.pc)) {
							return true;
						}
						return Reject(flags.pc, "a refused buffer table's descriptor is not "
						                        "sourced from memory");
					}
					handle = carried;
					source = planned->source;
					m_applied_indirect_buffers.insert(carried);
				}
			}
			if (handle != nullptr && !m_applied_indirect_buffers.contains(handle) &&
			    ReadsUnfoldableScalar(*handle) &&
			    TakeDynamicBuffer(handle, memory, flags.index, buffer, flags.pc)) {
				return true;
			}
			// A descriptor materialization named as unprovable is decoded in the shader on rebuild.
			if (handle != nullptr && !m_applied_indirect_buffers.contains(handle) &&
			    std::ranges::find(m_program.unfoldable_pcs, flags.pc) !=
			        m_program.unfoldable_pcs.end() &&
			    CarriesLoopPhi(*handle) &&
			    (TakeDynamicBuffer(handle, memory, flags.index, buffer, flags.pc) ||
			     take_indirect_load())) {
				return true;
			}
			resource = AddBuffer(source, memory, inst, flags.pc);
			if (resource == UINT32_MAX) {
				return Reject(flags.pc, "buffer resource limit exceeded");
			}
			return AddHandlePatch(handle, resource, flags.pc) &&
			       AddMemoryPatch(flags.index, resource, 0, false, flags.pc);
		}
		if (address_info.access != AddressAccess::None) {
			if (!IsAddressResourceKind(memory.kind)) {
				return Reject(flags.pc, "address operation has invalid resource kind");
			}
			if (memory.kind == ResourceKind::Flat && memory.address_is_full &&
			    IsNoncanonicalFlatAddress(inst.Arg(2), inst.Arg(inst.NumArgs() - 1))) {
				ValidateAddressHandle(inst.Arg(0), flags.pc);
				if (memory.data_bits != 32u || m_program.scratch_dwords == 0) {
					return Reject(
					    flags.pc,
					    "local FLAT access requires DWORD data and per-thread scratch storage");
				}
				m_program.memory_info[flags.index].kind = ResourceKind::FlatLocal;
				return true;
			}
			if (memory.kind == ResourceKind::Scratch) {
				handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetScratchResource ||
				    handle->NumArgs() != 0) {
					Fail(flags.pc, "scratch operation requires GetScratchResource");
				}
				if (m_program.scratch_dwords == 0) {
					return Reject(flags.pc,
					              "scratch operation requires a nonzero AGC per-thread size");
				}
				return true;
			}
			if (!ValidateAddressHandle(inst.Arg(0), flags.pc)) {
				return false;
			}
			ValidateAddressHandle(inst.Arg(0), flags.pc);
			RecordDmaPointer(*inst.Arg(0).Resolve().TryInstruction(),
			                 address_info.access == AddressAccess::Write);
			if (address_info.access == AddressAccess::Write) {
				m_program.has_address_writes = true;
				// The store goes through the page table, so the emitter needs its own store-pointer
				// helper and the context has to publish a writable BDA set for the draw.
				m_info.writes_dma = true;
			}
			m_info.uses_dma = true;
			return true;
		}

		if (memory.kind != ResourceKind::Image ||
		    image_info.resource_class == ImageResourceClass::None) {
			return Reject(flags.pc, "image operation has invalid resource kind");
		}
		handle               = inst.Arg(0).Resolve().TryInstruction();
		const auto* indirect = handle != nullptr ? FindIndirectDescriptor(*handle) : nullptr;
		if (indirect != nullptr) {
			source = indirect->source;
		} else if (!GetHandle(inst.Arg(0), ValueOpcode::GetImageResource, 8, flags.pc,
		                      memory.resource * 4u, handle, source)) {
			return false;
		}
		resource = AddImage(source, memory, op, flags.pc);
		if (resource == UINT32_MAX) {
			return Reject(flags.pc, "image resource limit exceeded");
		}
		if (!AddHandlePatch(handle, resource, flags.pc)) {
			return false;
		}
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
			const auto* selected_sampler =
			    sampler_handle != nullptr ? FindIndirectDescriptor(*sampler_handle) : nullptr;
			if (selected_sampler != nullptr) {
				sampler_source = selected_sampler->source;
			} else if (!GetHandle(inst.Arg(1), ValueOpcode::GetSamplerResource, 4, flags.pc,
			                      memory.sampler * 4u, sampler_handle, sampler_source, true,
			                      sample_adjust)) {
				return false;
			}
			sampler = AddSampler(sampler_source, flags.pc);
			if (sampler == UINT32_MAX) {
				return Reject(flags.pc, "sampler resource limit exceeded");
			}
			if (!AddHandlePatch(sampler_handle, sampler, flags.pc) ||
			    !AddSampledPair(resource, sampler, flags.pc)) {
				return false;
			}
			m_info.samplers[sampler].gather_lod |=
			    op == ValueOpcode::ImageGatherRaw &&
			    (memory.image_sample_flags & Decoder::ImageSampleFlagLod) != 0u;
		}
		return AddMemoryPatch(flags.index, resource, sampler, image_info.needs_sampler, flags.pc);
	}

	const DescriptorSource* Source(uint32_t source) const {
		return source < m_sources.size() ? &m_sources[source] : nullptr;
	}

	void LinkImageAliases() {
		for (auto& buffer: m_info.buffers) {
			const auto* buffer_source = Source(buffer.source);
			if (buffer_source == nullptr || buffer_source->dword_count != 4 ||
			    buffer_source->indirect_buffer.has_value()) {
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

	Program&                            m_program;
	const Decoder::Program&             m_decoded;
	const CFG::Graph&                   m_native_cfg;
	std::vector<Program::ScalarWrite>   m_scalar_writes;
	std::vector<ResolvedHandle>         m_resolved_handles;
	std::vector<const Inst*>            m_srt_visiting;
	std::unordered_set<const Inst*>     m_srt_visited;
	std::vector<Inst*>                  m_scalar_reads;
	ShaderInfo                          m_info;
	std::vector<DescriptorSource>       m_sources;
	std::vector<HandlePatch>            m_handle_patches;
	std::vector<MemoryPatch>            m_memory_patches;
	mutable CyclicPhiEntryCache         m_cyclic_entries;
	std::vector<MemoryAccessors>        m_memory_accessors;
	std::vector<IndirectDescriptorPlan> m_indirect_descriptors;
	std::map<const Inst*, Value>        m_descriptor_values;
	bool                                m_shader_writes = false;
	std::vector<IndirectBufferPlan>     m_indirect_buffers;
	std::unordered_set<const Inst*>     m_applied_indirect_buffers;
	std::unordered_set<uint32_t>        m_demoted_tables;
	// Why the last descriptor fold failed, kept only across the reclassification attempt that
	// the failure hands off to.
	std::string m_deferred_reject;
	// Phis whose uniformity is being assumed while their own operands are checked.
	mutable std::unordered_set<const Inst*> m_uniform_phis;
	mutable ResourceTrackingStatus          m_status;
};

} // namespace

ResourceTrackingStatus TrackResources(Program& program, const Decoder::Program& decoded,
                                      const CFG::Graph& native_cfg) {
	return Tracker(program, decoded, native_cfg).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
