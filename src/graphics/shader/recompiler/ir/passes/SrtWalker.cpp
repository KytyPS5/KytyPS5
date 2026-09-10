#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

std::string Diagnostic(const ResourcePlan& program, uint32_t pc, const std::string& message) {
	return fmt::format("shader SRT: hash=0x{:016x} stage={} pc=0x{:08x} {}", program.shader_hash,
	                   StageName(program.stage), pc, message);
}

// Diagnostic kept from the Astro's Playroom SRT investigation. ResolveInvariantPhi's own
// divergence log (Program.cpp) fires for every non-invariant Phi anywhere a recursive
// Validate/Evaluate walk happens to touch across the whole shader -- in a 127-block, 13-loop
// compute kernel that is thousands of lines of noise, most of it from unrelated soft-fail
// validation attempts (branch-condition reachability, flat-SRT candidate collection) that never
// abort anything. This instead dumps the exact expression tree of the one dword that is actually
// about to fail materialization -- DAG-aware, so a node reached twice prints a short `#id`
// back-reference instead of re-expanding, which also terminates on a cycle. This is what found
// the real cause (a per-workgroup-varying tile-light-array address), and is kept as permanent,
// gated infrastructure for the next shader that fails this way; its one call site guards on
// Log::IsSilent() first since materialization is retried every dispatch for a shader Phase D
// lets keep running.
std::string DumpValueTree(const ResourcePlan& program, Value value,
                          std::unordered_map<const Inst*, uint32_t>& ids, uint32_t& budget) {
	value = value.Resolve();
	if (budget == 0) {
		return "...(dump budget exhausted)";
	}
	budget--;
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: return fmt::format("imm_u1({})", value.U1());
				case Type::U8: return fmt::format("imm_u8(0x{:02x})", value.U8());
				case Type::U16: return fmt::format("imm_u16(0x{:04x})", value.U16());
				case Type::U32: return fmt::format("imm_u32(0x{:08x})", value.U32());
				case Type::U64: return fmt::format("imm_u64(0x{:016x})", value.U64());
				default:
					return fmt::format("imm(type=0x{:x})", static_cast<uint32_t>(value.GetType()));
			}
		}
		return fmt::format("<non-instruction value, type=0x{:x}>",
		                   static_cast<uint32_t>(value.GetType()));
	}
	if (const auto found = ids.find(inst); found != ids.end()) {
		return fmt::format("#{}", found->second);
	}
	const auto id = static_cast<uint32_t>(ids.size());
	ids.emplace(inst, id);
	const auto op    = inst->GetOpcode();
	std::string extra;
	if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
		const auto index = inst->Flags<MemoryFlags>().index;
		extra            = fmt::format(" memory_info[{}]", index);
		if (index < program.memory_info.size()) {
			const auto& memory = program.memory_info[index];
			extra += fmt::format("{{resource={} sampler={} offset={} planning_only={}}}",
			                     memory.resource, memory.sampler, memory.offset,
			                     memory.planning_only);
		}
	} else if (inst->NumArgs() == 0) {
		extra = fmt::format(" flags=0x{:x}", inst->Flags<uint64_t>());
	}
	if (op == ValueOpcode::Phi) {
		std::string edges;
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			if (index != 0) {
				edges += ", ";
			}
			edges += fmt::format("[block={}]", fmt::ptr(inst->PhiBlock(index)));
			edges += DumpValueTree(program, inst->Arg(index), ids, budget);
		}
		return fmt::format("#{}=Phi{}({})", id, extra, edges);
	}
	// ReadConst is an indirection through ResourcePlan::srt_reads, not a value in its own right --
	// EvaluateInst's own ReadConst case (above) evaluates srt_reads[slot].value instead of
	// anything reachable from ReadConst's own Arg(0)/Arg(1). Follow that same indirection here,
	// or the dump shows the meaningless wrapper instead of the tree that actually gets evaluated.
	if (op == ValueOpcode::ReadConst && inst->NumArgs() == 2) {
		const auto slot = inst->Arg(1).Resolve();
		if (slot.IsImmediate() && slot.GetType() == Type::U32 &&
		    slot.U32() < program.srt_reads.size()) {
			return fmt::format(
			    "#{}=ReadConst{} slot={} -> {}", id, extra, slot.U32(),
			    DumpValueTree(program, program.srt_reads[slot.U32()].value, ids, budget));
		}
	}
	std::string args;
	for (size_t index = 0; index < inst->NumArgs(); index++) {
		if (index != 0) {
			args += ", ";
		}
		args += DumpValueTree(program, inst->Arg(index), ids, budget);
	}
	return fmt::format("#{}={}{}({})", id, ValueOpcodeName(op), extra, args);
}

bool AddSignedAddress(uint64_t base, int64_t offset, uint64_t& result) {
	if (base > AddressMask) {
		return false;
	}
	if (offset < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(offset);
		if (magnitude > base) {
			return false;
		}
		result = base - magnitude;
		return true;
	}
	const auto magnitude = static_cast<uint64_t>(offset);
	if (magnitude > AddressMask - base) {
		return false;
	}
	result = base + magnitude;
	return true;
}

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto kind = values.memory_info[index].kind;
	return (op == ValueOpcode::LoadAddressU32 && kind == ResourceKind::ScalarAddress) ||
	       (op == ValueOpcode::ReadConstBuffer && kind == ResourceKind::ScalarBuffer);
}

bool IsDescriptorHandle(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource: return true;
		default: return false;
	}
}

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UMin32:
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
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type)
	    : m_program(program), m_type(type) {}

	bool Run(Value value) { return Validate(value); }

private:
	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		value = value.Resolve();
		// Host floating-point evaluation does not model shader rounding/denormal modes.
		if (m_type == RuntimeValueType::Integer &&
		    TypesOverlap(value.GetType(), Type::F16 | Type::F32 | Type::F32x2)) {
			return false;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			if (!require_uniform) return true;
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64:
				case Type::F32: return true;
				default: return false;
			}
		}
		// Integer-only dependency checks do not depend on the active EXEC mask.
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		if (!m_visiting.insert(inst).second) {
			return !require_uniform;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer) {
				const auto active_mask = m_active_mask;
				m_active_mask          = {};
				const bool valid       = Validate(m_program.srt_reads[slot.U32()].value);
				m_active_mask          = active_mask;
				if (!valid) return finish(false);
			}
		}
		if (!require_uniform) return finish(ValidateArguments(*inst, false));
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			// Empty EXEC reads lane zero, so ignored operands still require integer types.
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(2), false)) {
				return finish(false);
			}
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU8 ||
		    op == ValueOpcode::UndefU16 || op == ValueOpcode::UndefU32 ||
		    op == ValueOpcode::UndefU64 || op == ValueOpcode::Void) {
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type == RuntimeValueType::Integer && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				return finish(false);
			}
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = inst->Arg(1).Resolve();
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u ||
			    (source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 &&
			     source->GetOpcode() != ValueOpcode::IAddCarry32)) {
				return finish(false);
			}
		}
		if (IsDescriptorHandle(op)) {
			size_t expected = 4u;
			if (op == ValueOpcode::GetImageResource) {
				expected = 8u;
			} else if (op == ValueOpcode::GetAddressResource) {
				expected = 2u;
			}
			if (inst->NumArgs() != expected) {
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};

class PlanBuilder {
public:
	explicit PlanBuilder(Program& program): m_program(program) {}

	void Run() {
		m_program.srt_reads.clear();
		m_program.dynamic_reads.clear();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
					const auto flags = inst.Flags<MemoryFlags>();
					if (flags.index < m_program.memory_info.size()) {
						const auto kind       = m_program.memory_info[flags.index].kind;
						const bool crosswired = (op == ValueOpcode::LoadAddressU32 &&
						                         kind == ResourceKind::ScalarBuffer) ||
						                        (op == ValueOpcode::ReadConstBuffer &&
						                         kind == ResourceKind::ScalarAddress);
						if (crosswired) {
							Fail(flags.pc,
							     fmt::format("{} has incompatible scalar memory metadata",
							                 ValueOpcodeName(op)));
						}
					}
				}
				if (IsDescriptorHandle(inst.GetOpcode())) {
					for (size_t index = 0; index < inst.NumArgs(); index++) {
						Collect(inst.Arg(index), 0);
					}
				}
			}
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 && IsRawRead(m_program, inst) &&
				    inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst))) {
					Collect(Value(&inst), inst.Flags<MemoryFlags>().pc);
				}
			}
		}
		PatchReads();
	}

private:
	struct Patch {
		Inst*    inst = nullptr;
		uint32_t slot = 0;
		bool     keep = false;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& message) const {
		const auto diagnostic = Diagnostic(m_program, pc, message);
		EXIT("shader SRT planning failed: %s", diagnostic.c_str());
		std::abort();
	}

	void Collect(Value value, uint32_t use_pc) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			Fail(use_pc, "invalid typed planning value");
		}
		const auto cycle = std::ranges::find(m_visiting, inst);
		if (cycle != m_visiting.end()) {
			const auto contains_phi = std::any_of(cycle, m_visiting.end(), [](const Inst* value) {
				return value->GetOpcode() == ValueOpcode::Phi;
			});
			if (contains_phi) {
				return;
			}
			Fail(use_pc, fmt::format("cyclic typed planning value {} without a phi",
			                         ValueOpcodeName(inst->GetOpcode())));
		}
		if (std::ranges::find(m_visited, inst) != m_visited.end()) {
			return;
		}
		m_visiting.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			Collect(inst->Arg(index), use_pc);
		}
		m_visiting.pop_back();
		m_visited.push_back(inst);
		if (!IsRawRead(m_program, *inst)) {
			return;
		}
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) {
			if (std::ranges::find(m_program.dynamic_reads, value) ==
			    m_program.dynamic_reads.end()) {
				m_program.dynamic_reads.push_back(value);
			}
			return;
		}
		for (uint32_t slot = 0; slot < m_program.srt_reads.size(); slot++) {
			if (EquivalentValue(m_program, value, m_program.srt_reads[slot].value)) {
				m_patches.push_back({inst, slot, false});
				return;
			}
		}
		const auto slot = static_cast<uint32_t>(m_program.srt_reads.size());
		m_program.srt_reads.push_back({value, slot});
		m_patches.push_back({inst, slot, true});
	}

	void PatchReads() {
		for (const auto& patch: m_patches) {
			auto* block = patch.inst->Parent();
			auto& list  = block->Instructions();
			auto  where =
			    std::ranges::find_if(list, [&](const Inst& inst) { return &inst == patch.inst; });
			const auto resource =
			    Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(where, ValueOpcode::ReadConst,
			                                                {resource, Value(patch.slot)}));
			const auto uses = patch.inst->Uses();
			for (const auto& use: uses) {
				use.user->SetArg(use.operand, flat);
			}
			for (auto& info: m_program.block_info) {
				if (info.condition.Resolve() == Value(patch.inst)) {
					info.condition = flat;
				}
				if (info.indirect_target.Resolve() == Value(patch.inst)) {
					info.indirect_target = flat;
				}
			}
			if (patch.keep) {
				const auto memory = patch.inst->Flags<MemoryFlags>().index;
				if (memory < m_program.memory_info.size()) {
					m_program.memory_info[memory].planning_only = true;
				}
				block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(patch.inst)});
			}
		}
	}

	Program&           m_program;
	std::vector<Inst*> m_visiting;
	std::vector<Inst*> m_visited;
	std::vector<Patch> m_patches;
};

class Evaluator {
public:
	Evaluator(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, Evaluator* clean_evaluator = nullptr,
	          Value active_mask = {})
	    : m_program(program), m_runtime(runtime), m_clean_flat_slots(clean_flat_slots),
	      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()) {}

	bool Evaluate(Value value, uint32_t& result) {
		uint64_t wide = 0;
		if (!EvaluateWide(value, wide)) {
			return false;
		}
		result = static_cast<uint32_t>(wide);
		return true;
	}

	// Set once, by whichever failure happened first (deepest in the recursion, since evaluation
	// is depth-first) -- later, shallower "a sub-evaluation failed" sites never overwrite it, so
	// this stays the root cause rather than the outermost symptom.
	const std::string& FailReason() const { return m_fail_reason; }

private:
	bool Fail(std::string reason) {
		if (m_fail_reason.empty()) {
			m_fail_reason = std::move(reason);
		}
		return false;
	}

	static float Float32(uint64_t bits) {
		return std::bit_cast<float>(static_cast<uint32_t>(bits));
	}

	static uint64_t Float32Bits(float value) { return std::bit_cast<uint32_t>(value); }

	bool EvaluateWide(Value value, uint64_t& result) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: result = value.U1(); return true;
				case Type::U8: result = value.U8(); return true;
				case Type::U16: result = value.U16(); return true;
				case Type::U32: result = value.U32(); return true;
				case Type::U64: result = value.U64(); return true;
				case Type::F32: result = Float32Bits(value.F32Value()); return true;
				default:
					return Fail(fmt::format("unsupported immediate type {} in SRT evaluation",
					                        static_cast<int>(value.GetType())));
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return Fail("SRT evaluation reached a value that is neither an immediate nor an "
			            "instruction");
		}
		if (!m_reserved) {
			m_cache.reserve(m_program.value_storage.size());
			m_visiting.reserve(m_program.value_storage.size());
			m_reserved = true;
		}
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(inst->GetOpcode()) &&
		    inst->NumArgs() == 3 && inst->Arg(0).Resolve() == m_active_mask) {
			return EvaluateWide(inst->Arg(1), result);
		}
		if (const auto found = m_cache.find(inst); found != m_cache.end()) {
			result = found->second;
			return true;
		}
		if (std::ranges::find(m_visiting, inst) != m_visiting.end()) {
			return Fail(fmt::format("cyclic value dependency evaluating {}",
			                        ValueOpcodeName(inst->GetOpcode())));
		}
		m_visiting.push_back(inst);
		uint64_t out = 0;
		const bool evaluated = EvaluateInst(*inst, out);
		m_visiting.pop_back();
		if (!evaluated) {
			return false;
		}
		m_cache.emplace(inst, out);
		result = out;
		return true;
	}

	bool Arg(const Inst& inst, size_t index, uint64_t& result) {
		return EvaluateWide(inst.Arg(index), result);
	}

	bool EvaluatePhi(const Inst& inst, uint64_t& result) {
		const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
		if (!value.IsEmpty()) {
			return EvaluateWide(value, result);
		}
		if (Config::ApproximateDivergentPhiEnabled()) {
			return EvaluateApproximateDivergentPhi(inst, result);
		}
		return Fail("Phi is not invariant across control flow (its incoming values differ)");
	}

	// History: 2026-09-10 tried approximating a non-invariant Phi with its first incoming edge
	// instead of failing, to let ASTRO's Playroom's compute shaders that fail the invariance
	// check actually run instead of being skipped. REVERTED after one test: the first
	// APPROXIMATING hits were immediately followed by a process crash. That crash was later
	// (session 23) found to be a KytyPS5 memory-safety bug in EvaluateWide's raw-read
	// path -- an unvalidated `memcpy` from a guest-computed address, now fixed by wiring a
	// validated `read_memory` reader at the real caller (pipelineCache.cpp) -- not evidence that
	// approximating this class of value is unsafe in general. Real AMD hardware/compilers face
	// the identical constraint: MIMG/MUBUF descriptors must be uniform SGPRs, and a divergent
	// one is forced uniform via `v_readfirstlane_b32` (mesa/src/amd/compiler's
	// aco_lower_to_hw_instr.cpp -- confirmed present in this session's local reference corpus,
	// `workflow/ps5_arch_map_amd_open_stack.md`). This is that same mechanism, opt-in
	// (`--approximate-divergent-phi`, default off -- see emulatorConfig.h's comment on the flag
	// for the full risk framing) rather than default-on, because unlike ACO's compile-time-
	// certain case this session has not verified every affected shader's divergent branches are
	// actually interchangeable resource selections rather than genuine per-invocation math.
	//
	// Never fabricates a value: every candidate leaf is evaluated through the SAME runtime path
	// as normal materialization (not guessed structurally), and the whole approximation is
	// refused -- falling back to the ordinary hard failure -- if every candidate is
	// zero/uninitialized-looking or fails to evaluate on its own terms. `m_fail_reason` is
	// saved/restored around each trial evaluation so a rejected candidate's failure reason never
	// masks a real, later failure (see FailReason()'s own comment on why it must stay the true
	// root cause).
	// Helper to check if two MemoryInfo objects differ only in the 'resource' field (base address).
	// Used by EvaluateApproximateDivergentPhi's guard to ensure all divergent descriptor candidates
	// are structurally equivalent except for which descriptor they resolve to.
	bool AreMemoryInfoEquivalentExceptResource(const MemoryInfo& a, const MemoryInfo& b) {
		// Compare all fields except 'resource'. The 'resource' field is the descriptor index that
		// may differ across control flow branches when descriptors are interchangeable (same format,
		// stride, element count, etc., pointing at different base addresses).
		return a.kind == b.kind &&
		       a.sampler == b.sampler &&
		       a.offset == b.offset &&
		       a.secondary_offset == b.secondary_offset &&
		       a.dmask == b.dmask &&
		       a.data_dwords == b.data_dwords &&
		       a.data_bits == b.data_bits &&
		       a.component_index == b.component_index &&
		       a.component_count == b.component_count &&
		       a.data_format == b.data_format &&
		       a.number_format == b.number_format &&
		       a.image_sample_flags == b.image_sample_flags &&
		       a.image_dimension == b.image_dimension &&
		       a.image_address_components == b.image_address_components &&
		       a.image_nsa_dwords == b.image_nsa_dwords &&
		       std::equal(std::begin(a.image_nsa_addr), std::end(a.image_nsa_addr),
		                  std::begin(b.image_nsa_addr)) &&
		       a.memory_segment == b.memory_segment &&
		       a.address_is_full == b.address_is_full &&
		       a.data_signed == b.data_signed &&
		       a.typed == b.typed &&
		       a.formatted == b.formatted &&
		       a.image_has_mip == b.image_has_mip &&
		       a.image_r128 == b.image_r128 &&
		       a.glc == b.glc &&
		       a.slc == b.slc &&
		       a.idxen == b.idxen &&
		       a.offen == b.offen &&
		       a.planning_only == b.planning_only;
	}

	bool EvaluateApproximateDivergentPhi(const Inst& phi, uint64_t& result) {
		constexpr size_t                MaxCandidates = 8;
		std::vector<Value>              leaves;
		std::vector<Value>              pending {Value(const_cast<Inst*>(&phi))};
		std::unordered_set<const Inst*> visited_phis;
		while (!pending.empty() && leaves.size() < MaxCandidates) {
			const auto current = pending.back().Resolve();
			pending.pop_back();
			const auto* inst = current.TryInstruction();
			if (inst != nullptr && inst->GetOpcode() == ValueOpcode::Phi) {
				if (!visited_phis.insert(inst).second) {
					continue;
				}
				for (size_t index = 0; index < inst->NumArgs(); index++) {
					pending.push_back(inst->Arg(index));
				}
				continue;
			}
			if (std::ranges::find(leaves, current) == leaves.end()) {
				leaves.push_back(current);
			}
		}

		// Guard: reject the approximation unless all candidates are structurally equivalent
		// except for the base address/resource field (i.e., interchangeable descriptors with
		// same format/layout but different addresses) and none resolve to immediate zero.
		if (!leaves.empty()) {
			// Extract MemoryInfo for each leaf that is a runtime read (GetAddressU32/ReadConstBuffer).
			std::vector<uint32_t> memory_info_indices;
			for (const auto& leaf: leaves) {
				const auto resolved = leaf.Resolve();
				const auto* inst = resolved.TryInstruction();
				if (inst == nullptr) {
					// Non-instruction leaf (immediate value). Reject unless it's immediate zero,
					// which is caught later. A divergent Phi with both immediate and instruction
					// candidates is heterogeneous and not a simple "pick the descriptor" scenario.
					if (!resolved.IsImmediate() || resolved.GetType() != Type::U32 || resolved.U32() != 0) {
						static Log::RateLimit limiter {"GuardedDivergentPhiRejectedHeterogeneous", 256};
						if (const auto hit = limiter.Hit()) {
							LOGF("shader SRT: hash=0x%016llx divergent Phi guard REJECTED: candidate is "
							     "non-instruction non-zero immediate [%llu]\n",
							     static_cast<unsigned long long>(m_program.shader_hash), *hit);
						}
						return Fail("Phi is not invariant across control flow, and approximation was "
						            "refused: heterogeneous candidates (mix of immediates and instructions)");
					}
					continue;  // Skip immediate-zero candidates; they'll be caught as rejected_zero later.
				}

				// Check if this is a runtime memory read (descriptor-based).
				const auto op = inst->GetOpcode();
				if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) {
					// Not a memory read. Reject the approximation for heterogeneous candidates.
					static Log::RateLimit limiter {"GuardedDivergentPhiRejectedNonMemory", 256};
					if (const auto hit = limiter.Hit()) {
						LOGF("shader SRT: hash=0x%016llx divergent Phi guard REJECTED: candidate is "
						     "non-memory instruction %s [%llu]\n",
						     static_cast<unsigned long long>(m_program.shader_hash),
						     ValueOpcodeName(op), *hit);
					}
					return Fail(fmt::format(
					    "Phi is not invariant across control flow, and approximation was refused: "
					    "candidate is {} (not a memory read)",
					    ValueOpcodeName(op)));
				}

				const auto index = inst->Flags<MemoryFlags>().index;
				if (index >= m_program.memory_info.size()) {
					// Invalid memory info index. Reject.
					static Log::RateLimit limiter {"GuardedDivergentPhiRejectedInvalidMemory", 256};
					if (const auto hit = limiter.Hit()) {
						LOGF("shader SRT: hash=0x%016llx divergent Phi guard REJECTED: memory info "
						     "index %u out of range (%zu known) [%llu]\n",
						     static_cast<unsigned long long>(m_program.shader_hash), index,
						     m_program.memory_info.size(), *hit);
					}
					return Fail(fmt::format(
					    "Phi is not invariant across control flow, and approximation was refused: "
					    "invalid memory-info index {}",
					    index));
				}
				memory_info_indices.push_back(index);
			}

			// Verify all MemoryInfo candidates are equivalent except for resource field.
			if (!memory_info_indices.empty()) {
				const auto base_index = memory_info_indices[0];
				for (size_t i = 1; i < memory_info_indices.size(); ++i) {
					const auto current_index = memory_info_indices[i];
					if (!AreMemoryInfoEquivalentExceptResource(m_program.memory_info[base_index],
					                                           m_program.memory_info[current_index])) {
						// Candidates differ in fields other than resource. Reject.
						static Log::RateLimit limiter {
						    "GuardedDivergentPhiRejectedStructuralDifference", 256};
						if (const auto hit = limiter.Hit()) {
							LOGF("shader SRT: hash=0x%016llx divergent Phi guard REJECTED: descriptors "
							     "are not structurally equivalent (memory_info[%u] vs [%u] differ beyond "
							     "resource field) [%llu]\n",
							     static_cast<unsigned long long>(m_program.shader_hash), base_index,
							     current_index, *hit);
						}
						return Fail(fmt::format(
						    "Phi is not invariant across control flow, and approximation was refused: "
						    "descriptor candidates differ structurally (memory_info[{}] vs [{}])",
						    base_index, current_index));
					}
				}
			}
		}

		uint32_t rejected_zero = 0;
		uint32_t rejected_fail = 0;
		for (const auto& leaf: leaves) {
			const auto saved_fail_reason = m_fail_reason;
			uint64_t   candidate         = 0;
			const bool evaluated         = EvaluateWide(leaf, candidate);
			m_fail_reason                = saved_fail_reason;
			if (!evaluated) {
				rejected_fail++;
				continue;
			}
			if (candidate == 0) {
				rejected_zero++;
				continue;
			}
			static Log::RateLimit limiter {"ApproximatedDivergentPhi", 256};
			if (const auto hit = limiter.Hit()) {
				LOGF("shader SRT: hash=0x%016llx APPROXIMATING non-invariant Phi: picked first "
				     "viable candidate of %zu (%u rejected as zero, %u rejected as "
				     "unevaluable) [%llu]\n",
				     static_cast<unsigned long long>(m_program.shader_hash), leaves.size(),
				     rejected_zero, rejected_fail, *hit);
			}
			result = candidate;
			return true;
		}
		return Fail(fmt::format("Phi is not invariant across control flow, and no approximation "
		                        "candidate was viable ({} candidates: {} zero, {} failed to "
		                        "evaluate)",
		                        leaves.size(), rejected_zero, rejected_fail));
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result) {
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return Fail("composite extract index is not an immediate u32");
		}
		const auto component = index.U32();
		if (component >= 2u) {
			return Fail(fmt::format("composite extract component {} out of range", component));
		}
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
			uint64_t packed = 0;
			if (!Arg(inst, 0, packed)) {
				return false;
			}
			result = static_cast<uint32_t>(packed >> (component * 32u));
			return true;
		}
		const auto* source = inst.Arg(0).ResolveInstruction();
		if (source == nullptr) {
			return Fail("composite extract source is not an instruction");
		}
		if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			return EvaluateWide(source->Arg(component), result);
		}
		if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
			uint64_t lhs = 0;
			uint64_t rhs = 0;
			if (!Arg(*source, 0, lhs) || !Arg(*source, 1, rhs)) {
				return false;
			}
			const auto sum =
			    static_cast<uint64_t>(static_cast<uint32_t>(lhs)) + static_cast<uint32_t>(rhs);
			result =
			    component == 0u ? static_cast<uint32_t>(sum) : static_cast<uint32_t>(sum >> 32u);
			return true;
		}
		return Fail(fmt::format("composite extract of {} is not supported",
		                        ValueOpcodeName(source->GetOpcode())));
	}

	bool EvaluateRawRead(const Inst& inst, uint64_t& result) {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			return Fail(fmt::format("raw read memory-info index {} out of range ({} known)",
			                        flags.index, m_program.memory_info.size()));
		}
		const auto& mem    = m_program.memory_info[flags.index];
		const auto* handle = inst.Arg(0).ResolveInstruction();
		if (handle == nullptr) {
			return Fail("raw read descriptor handle is not an instruction");
		}
		uint64_t low    = 0;
		uint64_t high   = 0;
		uint64_t offset = 0;
		if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
			return false;
		}
		const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
		// A V#/T# whose base pointer resolved to null is an unbound (optional) descriptor
		// slot; on real hardware a read through it returns all-zero rather than faulting.
		// Resolve it to 0 instead of failing the whole materialisation (which would drop the
		// dispatch). Correct emulation, not a soft-ladder.
		if (base == 0) {
			result = 0;
			return true;
		}
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		uint64_t   address   = 0;
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			uint64_t records = 0;
			uint64_t word3   = 0;
			if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
				return Fail("constant buffer descriptor handle has an unexpected shape");
			}
			if (immediate < 0) {
				return Fail(fmt::format("constant buffer read has a negative offset {}", immediate));
			}
			const auto byte_offset =
			    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
			const auto aligned = byte_offset & ~uint64_t {3};
			const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			const auto size = stride == 0u
			                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
			                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
			if (aligned > size || size - aligned < sizeof(uint32_t)) {
				return Fail(fmt::format(
				    "constant buffer read at offset {} (aligned {}) is out of bounds (size {})",
				    byte_offset, aligned, size));
			}
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto relative = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
			if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
				return Fail(fmt::format("scalar address read overflowed: base=0x{:012x} relative={}",
				                        base, relative));
			}
		}
		uint32_t word = 0;
		if (m_runtime.read_memory != nullptr) {
			if (!m_runtime.read_memory(m_runtime.userdata, address, &word)) {
				return Fail(fmt::format("read_memory callback rejected address 0x{:012x}", address));
			}
		} else {
			// This recompiler layer has no dependency on src/kernel and cannot validate `address`
			// itself -- `read_memory` is exactly the seam callers use to supply a checked reader
			// (see pipelineCache.cpp's `ReadShaderGuestMemoryDirect`, wired to every real
			// draw/dispatch). A caller that leaves `read_memory` null is asserting `address` is
			// known-valid host memory (true for several unit tests here that read from a local
			// buffer at a fixed address); it is NOT true in general for a value the SRT evaluator
			// computed from guest-controlled descriptor dwords. Session 22 (2026-09-10) hit this
			// exact gap in production -- `read_memory` was unset, a non-invariant Phi resolved to
			// a near-null address, and this memcpy segfaulted the whole process. Do not remove the
			// `read_memory` wiring at a real caller without understanding this comment.
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		result = word;
		return true;
	}

	bool EvaluateInst(const Inst& inst, uint64_t& result) {
		uint64_t   a       = 0;
		uint64_t   b       = 0;
		uint64_t   c       = 0;
		const auto binary  = [&]() { return Arg(inst, 0, a) && Arg(inst, 1, b); };
		const auto ternary = [&]() {
			return Arg(inst, 0, a) && Arg(inst, 1, b) && Arg(inst, 2, c);
		};
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base ||
				    reg - m_program.user_data_base >= m_runtime.user_data.size()) {
					return Fail(fmt::format(
					    "user data register {} is outside the runtime's {} provided registers "
					    "(base {})",
					    reg, m_runtime.user_data.size(), m_program.user_data_base));
				}
				result = m_runtime.user_data[reg - m_program.user_data_base];
				return true;
			}
			case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
			case ValueOpcode::Phi: return EvaluatePhi(inst, result);
			case ValueOpcode::ReadFirstLane: {
				Evaluator active(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
				                 inst.Arg(1));
				return active.EvaluateWide(inst.Arg(0), result);
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return Arg(inst, 0, result);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
			// CompositeConstructU32x2 has no dedicated case here below this comment; it and
			// CompositeConstructU64 pack two dwords into one wide value identically, so a
			// shader that feeds a x2-composite straight into e.g. a Phi or ReadFirstLane
			// rather than through CompositeExtract* (the only path EvaluateExtract() covers)
			// used to fall through to `default: break` below and fail with no diagnostic --
			// this is the confirmed cause of the Astro's Playroom SRT evaluation crash
			// (IsRuntimeUniformOp whitelists both CompositeConstructU32x2 and IAddCarry32 as
			// host-evaluable at plan-build time, but neither had a case here).
			case ValueOpcode::CompositeConstructU32x2:
			case ValueOpcode::CompositeConstructU64:
				if (!binary()) {
					return Fail(fmt::format("{} missing an operand",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				result = static_cast<uint32_t>(a) |
				         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
				return true;
			// Same story as above: IAddCarry32 evaluated directly (not as the operand of a
			// CompositeExtract*) means "give me the primary destination", the low 32-bit sum --
			// the carry-out lives in the second component, reachable only via EvaluateExtract().
			case ValueOpcode::IAddCarry32:
				if (!binary()) {
					return Fail("IAddCarry32 missing an operand");
				}
				result = static_cast<uint32_t>(a + b);
				return true;
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32) {
					return Fail("ReadConst slot index is not an immediate u32");
				}
				if (slot.U32() >= m_program.srt_reads.size()) {
					return Fail(fmt::format("ReadConst slot {} out of range ({} SRT reads planned)",
					                        slot.U32(), m_program.srt_reads.size()));
				}
				if (slot.U32() < m_clean_flat_slots.size() &&
				    m_clean_flat_slots[slot.U32()] != 0u && m_clean_evaluator != nullptr) {
					return m_clean_evaluator->EvaluateWide(m_program.srt_reads[slot.U32()].value,
					                                       result);
				}
				return EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				if (IsRawRead(m_program, inst)) {
					return EvaluateRawRead(inst, result);
				}
				return Fail(fmt::format("{} is not a recognized raw scalar read",
				                        ValueOpcodeName(inst.GetOpcode())));
			case ValueOpcode::IAdd32:
				if (binary()) {
					result = static_cast<uint32_t>(a + b);
					return true;
				}
				return false;
			case ValueOpcode::IAdd64:
				if (binary()) {
					result = a + b;
					return true;
				}
				return false;
			case ValueOpcode::ISub32:
				if (binary()) {
					result = static_cast<uint32_t>(a - b);
					return true;
				}
				return false;
			case ValueOpcode::ISub64:
				if (binary()) {
					result = a - b;
					return true;
				}
				return false;
			case ValueOpcode::IMul32:
				if (binary()) {
					result = static_cast<uint32_t>(a * b);
					return true;
				}
				return false;
			case ValueOpcode::IMul64:
				if (binary()) {
					result = a * b;
					return true;
				}
				return false;
			case ValueOpcode::UMin32:
				if (binary()) {
					result = std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
					return true;
				}
				return false;
			case ValueOpcode::ConvertF32U32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(static_cast<float>(static_cast<uint32_t>(a)));
					return true;
				}
				return false;
			case ValueOpcode::ConvertU32F32:
				if (Arg(inst, 0, a)) {
					const auto value = Float32(a);
					if (!std::isfinite(value) || value < 0.0f ||
					    static_cast<double>(value) > UINT32_MAX) {
						return false;
					}
					result = static_cast<uint32_t>(value);
					return true;
				}
				return false;
			case ValueOpcode::FPMul32:
				if (binary()) {
					result = Float32Bits(Float32(a) * Float32(b));
					return true;
				}
				return false;
			case ValueOpcode::FPTrunc32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(std::trunc(Float32(a)));
					return true;
				}
				return false;
			case ValueOpcode::FPIsNan32:
				if (Arg(inst, 0, a)) {
					result = std::isnan(Float32(a));
					return true;
				}
				return false;
			case ValueOpcode::FPOrdLessThanEqual32:
				if (binary()) {
					result = Float32(a) <= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::FPOrdGreaterThanEqual32:
				if (binary()) {
					result = Float32(a) >= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd32:
				if (binary()) {
					result = static_cast<uint32_t>(a & b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd64:
				if (binary()) {
					result = a & b;
					return true;
				}
				return false;
			case ValueOpcode::BitwiseOr32:
				if (binary()) {
					result = static_cast<uint32_t>(a | b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseXor32:
				if (binary()) {
					result = static_cast<uint32_t>(a ^ b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseNot32:
				if (Arg(inst, 0, a)) {
					result = ~static_cast<uint32_t>(a);
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) << (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical64:
				if (binary()) {
					result = a << (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) >> (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical64:
				if (binary()) {
					result = a >> (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic32:
				if (binary()) {
					result = static_cast<uint32_t>(
					    std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >> (b & 31u));
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic64:
				if (binary()) {
					result = static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
					return true;
				}
				return false;
			case ValueOpcode::BitFieldUExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					const auto mask = width == 32u  ? UINT32_MAX
					                  : width == 0u ? 0u
					                                : (uint32_t {1} << width) - 1u;
					result = width == 0u ? 0u : (static_cast<uint32_t>(a) >> offset) & mask;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldSExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					if (width == 0u) {
						result = 0;
						return true;
					}
					const auto mask = width == 32u ? UINT32_MAX : (uint32_t {1} << width) - 1u;
					auto       bits = (static_cast<uint32_t>(a) >> offset) & mask;
					if (width < 32u && (bits & (uint32_t {1} << (width - 1u))) != 0u) {
						bits |= ~mask;
					}
					result = bits;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldInsert: {
				uint64_t d = 0;
				if (!ternary() || !Arg(inst, 3, d)) {
					return false;
				}
				const auto offset = static_cast<uint32_t>(c);
				const auto width  = static_cast<uint32_t>(d);
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					result = static_cast<uint32_t>(a);
					return true;
				}
				const auto mask =
				    width == 32u ? UINT32_MAX : ((uint32_t {1} << width) - 1u) << offset;
				result = (static_cast<uint32_t>(a) & ~mask) |
				         ((static_cast<uint32_t>(b) << offset) & mask);
				return true;
			}
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32:
				if (ternary()) {
					result = a != 0u ? b : c;
					return true;
				}
				return false;
			case ValueOpcode::IEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) == static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::INotEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) != static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::ULessThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::UGreaterThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) > static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::LogicalAnd:
				if (binary()) {
					result = (a != 0u) && (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalOr:
				if (binary()) {
					result = (a != 0u) || (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalXor:
				if (binary()) {
					result = (a != 0u) != (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalNot:
				if (Arg(inst, 0, a)) {
					result = a == 0u;
					return true;
				}
				return false;
			case ValueOpcode::UndefU1:
			case ValueOpcode::UndefU8:
			case ValueOpcode::UndefU16:
			case ValueOpcode::UndefU32:
			case ValueOpcode::UndefU64:
				return Fail(fmt::format("{} is explicitly undefined and has no runtime value",
				                        ValueOpcodeName(inst.GetOpcode())));
			default:
				// The generic sink: every opcode IsRuntimeUniformOp() accepts as host-evaluable
				// must have a case above, or it silently fails here with no diagnostic --
				// exactly what happened for CompositeConstructU32x2/IAddCarry32. Named so the
				// next divergence between the two lists shows up as a message, not a gdb session.
				return Fail(fmt::format("no runtime evaluator for opcode {}",
				                        ValueOpcodeName(inst.GetOpcode())));
		}
	}

	const ResourcePlan&                       m_program;
	const SrtRuntime&                         m_runtime;
	std::span<const uint8_t>                  m_clean_flat_slots;
	Evaluator*                                m_clean_evaluator = nullptr;
	Value                                     m_active_mask;
	std::unordered_map<const Inst*, uint64_t> m_cache;
	std::vector<const Inst*>                  m_visiting;
	bool                                      m_reserved = false;
	std::string                               m_fail_reason;
};

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

bool EvaluateRuntimeSourcesImpl(const ResourcePlan& program, std::span<const uint32_t> sources,
                                const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                                std::vector<uint32_t>& flat, bool evaluate_flat,
                                std::span<const uint8_t> clean_flat_slots,
                                std::vector<uint8_t>& active_sources,
                                std::string* fail_reason) {
	const auto fail = [&](std::string reason) {
		if (fail_reason != nullptr) {
			*fail_reason = Diagnostic(program, 0, reason);
		}
		return false;
	};
	if (!program.srt_plan_complete) {
		return fail("SRT plan is not complete (BuildSrtPlan was not run, or failed)");
	}
	if (std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; }) &&
	    runtime.read_specialization_memory == nullptr) {
		return fail("clean (specialization-invariant) flat slots were requested but no "
		            "read_specialization_memory reader was provided");
	}
	SrtRuntime clean_runtime  = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	Evaluator            clean_evaluator(program, clean_runtime);
	Evaluator            evaluator(program, runtime, clean_flat_slots, &clean_evaluator);
	std::vector<uint8_t> active;
	if (evaluate_flat) {
		active.assign(program.descriptor_sources.size(), 1u);
	}
	if (evaluate_flat && !program.control_flow.empty()) {
		for (const auto& block: program.control_flow) {
			for (const auto source: block.sources) {
				active.at(source) = 0u;
			}
		}
		std::vector<uint8_t>  visited(program.control_flow.size());
		std::vector<uint32_t> pending {0};
		while (!pending.empty()) {
			const auto index = pending.back();
			pending.pop_back();
			if (visited.at(index)) {
				continue;
			}
			visited[index]    = 1u;
			const auto& block = program.control_flow[index];
			for (const auto source: block.sources) {
				active[source] = 1u;
			}
			uint32_t condition = 0;
			// A missing clean reader must never fall through to the evaluator's raw-memory path.
			if (!block.condition.IsEmpty() && runtime.read_specialization_memory != nullptr &&
			    clean_evaluator.Evaluate(block.condition, condition)) {
				pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
			} else {
				pending.insert(pending.end(), block.successors.begin(), block.successors.end());
			}
		}
	}
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto* source = Source(program, source_index);
		if (source == nullptr) {
			return fail(fmt::format("descriptor source index {} is out of range ({} known)",
			                        source_index, program.descriptor_sources.size()));
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		if (!evaluate_flat || active[source_index]) {
			for (uint32_t index = 0; index < source->dword_count; index++) {
				if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
					// Phase D (pipelineCache.cpp) deliberately retries MaterializeResources every
					// dispatch for a shader that fails here rather than caching the failure, so
					// this runs on every dispatch of every affected shader for as long as the
					// game keeps running. Building the tree (up to 400 nodes, each formatted) is
					// real work; only pay for it when logging is actually on.
					if (!::Log::IsSilent()) {
						std::unordered_map<const Inst*, uint32_t> ids;
						uint32_t                                  budget = 400;
						LOGF("shader SRT: hash=0x%016llx descriptor source %u dword %u tree: %s\n",
						    static_cast<unsigned long long>(program.shader_hash), source_index,
						    index, DumpValueTree(program, source->dwords[index], ids, budget).c_str());
					}
					return fail(fmt::format("descriptor source {} dword {}: {}", source_index,
					                        index, evaluator.FailReason()));
				}
			}
		} else {
			// Contractually zero, not a failure (see this function's header comment: "Inactive
			// descriptors are zero") -- the reachability walk above found no live control-flow
			// path to whatever block declares this source, so `value` is left default-constructed.
			// Correct, but was silent: this is one of the stages 22 sessions of the ASTRO's
			// Playroom investigation never saw, because nothing said a descriptor had been zeroed
			// this way. Rate-limited, not per-hit-gated on Log::IsSilent() first like the failure
			// path above, because MaterializeResources retries every dispatch for the whole life
			// of the affected shader and this is cheap to format either way.
			//
			// Checked before the rate limit, not after: this site alone fires 100,000+ times per
			// real run across every shader hash, which exhausts the shared cap long before the
			// one hash under investigation gets a turn. --shader-log-filter-hash lets a targeted
			// session spend the whole budget on the hash that matters instead.
			if (Config::ShaderLogHashAllowed(program.shader_hash)) {
				static Log::RateLimit limiter {"SrtInactiveDescriptorSource", 256};
				if (const auto hit = limiter.Hit()) {
					LOGF("shader SRT: hash=0x%016llx descriptor source %u marked inactive by "
					     "reachability walk, evaluated as zero [%llu]\n",
					     static_cast<unsigned long long>(program.shader_hash), source_index, *hit);
				}
			}
		}
		evaluated.push_back(value);
	}
	std::vector<uint32_t> flattened;
	if (evaluate_flat) {
		flattened.resize(program.srt_reads.size());
		for (const auto& read: program.srt_reads) {
			const bool clean    = read.flat_offset < clean_flat_slots.size() &&
			                      clean_flat_slots[read.flat_offset] != 0u;
			auto&      selected = clean ? clean_evaluator : evaluator;
			if (read.flat_offset >= flattened.size()) {
				return fail(fmt::format("flat SRT slot {} is out of range ({} slots)",
				                        read.flat_offset, flattened.size()));
			}
			if (!selected.Evaluate(read.value, flattened[read.flat_offset])) {
				return fail(fmt::format("flat SRT slot {}: {}", read.flat_offset,
				                        selected.FailReason()));
			}
		}
	}
	results = std::move(evaluated);
	active_sources = std::move(active);
	if (evaluate_flat) {
		flat = std::move(flattened);
	}
	return true;
}

} // namespace

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}

void BuildSrtPlan(Program& program) {
	if (program.resource_tracking_complete) {
		EXIT("shader SRT planning failed: cannot rebuild SRT after resource tracking");
	}
	program.srt_plan_complete = false;
	PlanBuilder(program).Run();
	program.srt_plan_complete = true;
}

bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                            const SrtRuntime& runtime, std::span<uint32_t> results) {
	if (values.size() != results.size()) {
		return false;
	}
	auto clean = runtime;
	clean.read_memory = runtime.read_specialization_memory != nullptr
	                        ? runtime.read_specialization_memory
	                        : +[](void*, uint64_t, uint32_t*) { return false; };
	Evaluator evaluator(program, clean);
	for (size_t i = 0; i < values.size(); ++i) {
		if (!evaluator.Evaluate(values[i], results[i])) {
			return false;
		}
	}
	return true;
}

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result,
                              std::string* fail_reason) {
	std::vector<DescriptorValue> results;
	if (!EvaluateDescriptorSources(program, std::span {&source, 1}, runtime, results,
	                               fail_reason)) {
		return false;
	}
	result = results.front();
	return true;
}

bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                               std::string* fail_reason) {
	std::vector<uint32_t> ignored;
	std::vector<uint8_t>  active;
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {},
	                                  active, fail_reason);
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources, std::string* fail_reason) {
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true,
	                                  clean_flat_slots, active_sources, fail_reason);
}

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat) {
	std::vector<DescriptorValue> ignored;
	std::vector<uint8_t>         active;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {}, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
