#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

SrtRuntime CleanRuntime(SrtRuntime runtime) {
	runtime.read_memory = runtime.read_specialization_memory != nullptr
	                          ? runtime.read_specialization_memory
	                          : +[](void*, uint64_t, std::span<uint32_t>) { return false; };
	return runtime;
}

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

std::string ValueShape(Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		if (value.GetType() == Type::U32) {
			return fmt::format("0x{:08x}", value.U32());
		}
		return fmt::format("immediate:{}", TypeName(value.GetType()));
	}
	const auto* inst = value.TryInstruction();
	return inst != nullptr ? std::string(ValueOpcodeName(inst->GetOpcode()))
	                       : fmt::format("undefined:{}", TypeName(value.GetType()));
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

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
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
		case ValueOpcode::SGreaterThanEqual32:
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
		// Eagerly snapshot ordinary scalar-address reads only along an unavoidable
		// entry chain. A read behind a conditional branch must execute at runtime:
		// its address can be null while the guest skips that branch. Descriptor
		// dependencies were already collected above and still require specialization.
		std::unordered_set<const Block*> unavoidable;
		if (!m_program.blocks.empty()) {
			for (auto* block = m_program.blocks.front();
			     block != nullptr && unavoidable.insert(block).second;) {
				const auto successors = block->ImmSuccessors();
				if (successors.size() != 1u ||
				    successors.front()->ImmPredecessors().size() != 1u) break;
				block = successors.front();
			}
		}
		for (auto* block: m_program.blocks) {
			if (!unavoidable.contains(block)) continue;
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

} // namespace

namespace {

struct BoundedOffset {
	const Inst* index = nullptr;
	uint32_t scale = 0;
	uint32_t bias = 0;
};

bool ParseBoundedOffset(Value value, BoundedOffset& result,
                        std::unordered_set<const Inst*>& visiting) {
	value = value.Resolve();
	if (value.GetType() != Type::U32) return false;
	if (value.IsImmediate()) {
		result.bias = value.U32();
		return true;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || !visiting.insert(inst).second) return false;
	const auto finish = [&](bool success) { visiting.erase(inst); return success; };
	if (inst->GetOpcode() == ValueOpcode::GetBuiltin) {
		const auto kind = inst->NumArgs() == 2u ? inst->Arg(0).Resolve() : Value {};
		const auto axis = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
		if (!kind.IsImmediate() || kind.GetType() != Type::U32 ||
		    kind.U32() != static_cast<uint32_t>(StageInputKind::WorkgroupId) ||
		    !axis.IsImmediate() || axis.GetType() != Type::U32 || axis.U32() >= 3u)
			return finish(false);
		result.index = inst;
		result.scale = 1u;
		return finish(true);
	}
	if (inst->GetOpcode() == ValueOpcode::Phi ||
	    inst->GetOpcode() == ValueOpcode::SelectU32 ||
	    inst->GetOpcode() == ValueOpcode::ReadFirstLane ||
	    inst->GetOpcode() == ValueOpcode::ReadLane) {
		result.index = inst;
		result.scale = 1u;
		return finish(true);
	}
	if (inst->GetOpcode() == ValueOpcode::BitFieldUExtract && inst->NumArgs() == 3u) {
		// The extracted value is itself a dense selector. Its source may stay
		// lane-varying; FiniteMaximum validates the immediate bit range below.
		result.index = inst;
		result.scale = 1u;
		return finish(true);
	}
	if (inst->NumArgs() != 2u) return finish(false);
	const Inst* arithmetic = inst;
	auto op = inst->GetOpcode();
	if (op == ValueOpcode::CompositeExtractU32x2) {
		const auto component = inst->Arg(1).Resolve();
		arithmetic = inst->Arg(0).Resolve().TryInstruction();
		if (!component.IsImmediate() || component.GetType() != Type::U32 || component.U32() != 0u ||
		    arithmetic == nullptr || arithmetic->GetOpcode() != ValueOpcode::IAddCarry32 ||
		    arithmetic->NumArgs() != 2u) return finish(false);
		// S_ADD_U32 lowers through carry pairs. Its low word has exactly the
		// same modulo-U32 value as IAdd32; the high/carry word is not affine.
		op = ValueOpcode::IAdd32;
	}
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::ISub32 &&
	    op != ValueOpcode::IMul32 && op != ValueOpcode::ShiftLeftLogical32)
		return finish(false);
	BoundedOffset left, right;
	if (!ParseBoundedOffset(arithmetic->Arg(0), left, visiting) ||
	    !ParseBoundedOffset(arithmetic->Arg(1), right, visiting)) return finish(false);
	if (left.index != nullptr && right.index != nullptr && left.index != right.index)
		return finish(false);
	result.index = left.index != nullptr ? left.index : right.index;
	switch (op) {
		case ValueOpcode::IAdd32:
			result.scale = left.scale + right.scale;
			result.bias = left.bias + right.bias;
			break;
		case ValueOpcode::ISub32:
			result.scale = left.scale - right.scale;
			result.bias = left.bias - right.bias;
			break;
		case ValueOpcode::IMul32:
			if (left.index != nullptr && right.index != nullptr) return finish(false);
			result.scale = left.scale * right.bias + right.scale * left.bias;
			result.bias = left.bias * right.bias;
			break;
		case ValueOpcode::ShiftLeftLogical32:
			if (right.index != nullptr) return finish(false);
			result.scale = left.scale << (right.bias & 31u);
			result.bias = left.bias << (right.bias & 31u);
			break;
		default: return finish(false);
	}
	return finish(true);
}

class BoundedReadProof {
public:
	explicit BoundedReadProof(const Program& program): m_program(program) {}

	std::optional<BoundedSrtReadProof> Run(const Inst& read) {
		if (m_program.stage != ShaderType::Compute || m_program.blocks.empty() ||
		    m_program.blocks.size() != m_program.block_info.size())
			return {};
		const auto opcode = read.GetOpcode();
		const bool address_read = opcode == ValueOpcode::LoadAddressU32 && read.NumArgs() == 4u;
		const bool buffer_read = opcode == ValueOpcode::ReadConstBuffer && read.NumArgs() == 2u;
		if (!address_read && !buffer_read) return {};
		if (buffer_read) {
			const char* trace_env = std::getenv("KYTY_SHADER_PHASE_TRACE");
			if (trace_env != nullptr && *trace_env != '\0' && std::string_view(trace_env) != "0")
				std::fprintf(stderr, "shader SRT: buffer read pc=0x%08" PRIx32
				                         " uses=%zu\n",
				             read.Flags<MemoryFlags>().pc, read.Uses().size());
		}
		const bool trace_image_read = buffer_read && std::ranges::any_of(read.Uses(), [](const Use& use) {
			return use.user != nullptr && use.user->GetOpcode() == ValueOpcode::GetImageResource;
		});
		const auto trace_reject = [&](const char* reason) {
			const char* value = std::getenv("KYTY_SHADER_PHASE_TRACE");
			if (trace_image_read && value != nullptr && *value != '\0' &&
			    std::string_view(value) != "0")
				std::fprintf(stderr, "shader SRT: image buffer proof pc=0x%08" PRIx32
				                         " rejected at %s\n",
				             read.Flags<MemoryFlags>().pc, reason);
		};
		const auto flags = read.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			trace_reject("memory index");
			return {};
		}
		const auto& memory = m_program.memory_info[flags.index];
		const auto expected_kind = address_read ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer;
		if (memory.kind != expected_kind || memory.planning_only ||
		    memory.data_dwords != 1u || memory.data_bits != 32u ||
			(address_read && (!Immediate(read.Arg(2), 0u) || read.Arg(3).Resolve() != Value(true)))) {
			trace_reject("memory shape");
			return {};
		}
		const auto* address = read.Arg(0).Resolve().TryInstruction();
		const uint32_t source_dwords = address_read ? 2u : 4u;
		const auto expected_handle = address_read ? ValueOpcode::GetAddressResource : ValueOpcode::GetBufferResource;
		if (address == nullptr || address->GetOpcode() != expected_handle ||
		    address->NumArgs() != source_dwords) {
			trace_reject("buffer handle");
			return {};
		}
		for (uint32_t word = 0; word < source_dwords; ++word)
			if (!ValidateRuntimeValue(m_program, address->Arg(word))) {
				trace_reject("buffer root");
				return {};
			}
		BoundedOffset offset;
		std::unordered_set<const Inst*> visiting;
		if (!ParseBoundedOffset(read.Arg(1), offset, visiting) || offset.index == nullptr) {
			trace_reject("offset");
			return {};
		}
		// Dispatcher emission still preserves the complete guest CFG. Validate it
		// here as well so the canonical Phi/guard proof below can be reused without
		// assuming that the whole program was structurizable.
		if (!BuildGraph()) return {};
		const bool workgroup = offset.index->GetOpcode() == ValueOpcode::GetBuiltin;
		if (m_program.dispatcher_fallback && workgroup) return {};
		if (workgroup) {
			// Acyclic shaders can execute this uniform-per-workgroup scalar read
			// directly through the existing BDA emitter. Keep CPU snapshots for
			// cyclic programs whose cooperative wave scheduling needs immutable
			// coefficients; eagerly reading GPU-produced acyclic inputs would fail.
			std::unordered_set<const Block*> active;
			std::unordered_set<const Block*> complete;
			if (!GraphHasCycle(m_program.blocks.front(), active, complete)) return {};
		}
		const auto maximum = workgroup ? std::optional<uint32_t>{} :
		                                FiniteMaximum(Value(const_cast<Inst*>(offset.index)));
		if (m_program.dispatcher_fallback && buffer_read &&
		    read.Arg(1).Resolve().TryInstruction() != nullptr) {
			const char* trace_env = std::getenv("KYTY_SHADER_PHASE_TRACE");
			if (trace_env != nullptr && *trace_env != '\0' && std::string_view(trace_env) != "0") {
				std::fprintf(stderr,
				             "shader SRT: buffer selector bound pc=0x%08" PRIx32 " maximum=%s\n",
				             flags.pc, maximum.has_value() ? "known" : "unknown");
			}
		}
		if (workgroup || maximum.has_value()) {
			const auto trace = [] {
				const char* value = std::getenv("KYTY_SHADER_PHASE_TRACE");
				return value != nullptr && *value != '\0' && std::string_view(value) != "0";
			}();
			const bool dispatcher_descriptor_table =
			    m_program.dispatcher_fallback &&
			    IsDispatcherDescriptorTableRead(read, Value(const_cast<Inst*>(offset.index)));
			if (trace && m_program.dispatcher_fallback && buffer_read && !dispatcher_descriptor_table)
				std::fprintf(stderr, "shader SRT: dispatcher descriptor table proof rejected pc=0x%08" PRIx32
				                         " before root checks\n",
				             flags.pc);
			// Dispatcher emission preserves the guest CFG but does not provide the structured
			// loop guarantees used by the wider proof below. A finite table read in a
			// single-entry unconditional prefix has no cyclic dominance dependency.
			if (m_program.dispatcher_fallback &&
			    (workgroup ||
			     (!DispatcherEntryPrefixPrecedes(*offset.index, read) && !dispatcher_descriptor_table))) {
				return {};
			}
			// The dense enclosure includes every possible GPU value. Keep the key
			// live; only its constant bound is evaluated on the host. Existing
			// materialization budgets and coherent-read checks apply to every entry.
			if (maximum == UINT32_MAX) return {}; // max+1 must not wrap to an empty table.
			if (!m_program.dispatcher_fallback &&
			    (!Dominates(offset.index->Parent(), read.Parent()) ||
			     (offset.index->Parent() == read.Parent() && !Precedes(*offset.index, read))))
				return {};
			// Only memory roots in the guaranteed entry prefix may be evaluated
			// eagerly. A dispatch bound does not make a conditional pointer load
			// unconditional, even when the coefficient itself is read later.
			const Block* prefix = m_program.blocks.front();
			std::unordered_set<const Block*> visited;
			while (m_program.block_info[m_ids.at(prefix)].terminator.kind == CFG::TerminatorKind::Branch) {
				if (!visited.insert(prefix).second) return {};
				const auto target = m_program.block_info[m_ids.at(prefix)].terminator.true_block;
				const auto* next = m_by_id.at(target);
				if (next->ImmPredecessors().size() != 1u || next->ImmPredecessors().front() != prefix) break;
				prefix = next;
			}
			// Buffer-backed tables can inherit their source descriptor from an
			// unavoidable scalar load on the path to this read. Snapshotting may
			// then fail closed when that guest address is unavailable, while the
			// direct-address form retains the stricter unconditional entry prefix.
			const Block* source_scope = buffer_read ? read.Parent() : prefix;
			for (uint32_t word = 0; word < source_dwords; ++word) {
				const bool safe = m_program.dispatcher_fallback
				                      ? (dispatcher_descriptor_table
				                             ? RuntimeReadsDominate(address->Arg(word), read.Parent(), &read)
				                             : RuntimeReadsPrecedeEntry(address->Arg(word), read))
				                      : RuntimeReadsDominate(address->Arg(word), source_scope, &read);
				if (trace && m_program.dispatcher_fallback && buffer_read &&
				    dispatcher_descriptor_table && !safe)
					std::fprintf(stderr,
					             "shader SRT: dispatcher descriptor table root rejected pc=0x%08" PRIx32
					             " word=%u\n",
					             flags.pc, word);
				if (!safe) return {};
			}
			return BoundedSrtReadProof {
			    .index = Value(const_cast<Inst*>(offset.index)),
			    .count = workgroup ? Value {} : Value(*maximum + 1u),
			    .address_low = address->Arg(0).Resolve(),
			    .address_high = address->Arg(1).Resolve(),
			    .descriptor_word2 = source_dwords == 4u ? address->Arg(2).Resolve() : Value {},
			    .descriptor_word3 = source_dwords == 4u ? address->Arg(3).Resolve() : Value {},
			    .source_dwords = source_dwords,
			    .offset_scale = offset.scale,
			    .offset_bias = offset.bias,
			    .memory_offset = memory.offset,
			    .workgroup_axis = workgroup ? offset.index->Arg(1).Resolve().U32() : UINT32_MAX};
		}
		const auto* phi = offset.index;
		if (phi->GetOpcode() != ValueOpcode::Phi) return {};
		const auto* header = phi->Parent();
		if (header == nullptr || !m_ids.contains(header) || !m_ids.contains(read.Parent()) ||
		    !Reachable(read.Parent()) || phi->NumArgs() != 2u || phi->NumPhiBlocks() != 2u ||
		    header->ImmPredecessors().size() != 2u) return {};
		const Block* initial = nullptr;
		const Block* latch = nullptr;
		Value latch_update;
		for (size_t incoming = 0; incoming < 2u; ++incoming) {
			const auto* predecessor = phi->PhiBlock(incoming);
			if (predecessor == nullptr || !m_ids.contains(predecessor) ||
			    std::ranges::find(header->ImmPredecessors(), predecessor) ==
			        header->ImmPredecessors().end()) return {};
			const auto value = phi->Arg(incoming).Resolve();
			if (Immediate(value, 0u) && !Dominates(header, predecessor)) {
				if (initial != nullptr) return {};
				initial = predecessor;
			} else {
				const auto* next = value.TryInstruction();
				if (latch != nullptr || next == nullptr ||
				    !Dominates(header, predecessor) || next->Parent() == nullptr ||
				    !Dominates(next->Parent(), predecessor)) return {};
				BoundedOffset update;
				std::unordered_set<const Inst*> update_visiting;
				if (!ParseBoundedOffset(value, update, update_visiting) || update.index != phi ||
				    update.scale != 1u || update.bias != 1u) return {};
				latch = predecessor;
				latch_update = value;
			}
		}
		if (initial == nullptr || latch == nullptr || initial == latch) return {};
		const auto index = Value(const_cast<Inst*>(phi));
		const auto make_proof = [&](Value count, bool count_signed = false) {
			return BoundedSrtReadProof {
			    .index = index,
			    .count = count,
			    .address_low = address->Arg(0).Resolve(),
			    .address_high = address->Arg(1).Resolve(),
			    .descriptor_word2 = source_dwords == 4u ? address->Arg(2).Resolve() : Value {},
			    .descriptor_word3 = source_dwords == 4u ? address->Arg(3).Resolve() : Value {},
			    .source_dwords = source_dwords,
			    .offset_scale = offset.scale,
			    .offset_bias = offset.bias,
			    .memory_offset = memory.offset,
			    .count_signed = count_signed};
		};
		// A canonical post-test loop consumes index i, increments it once in the
		// latch, and repeats while next < a positive constant. Requiring the read
		// to dominate that latch proves the accessed dense range is [0, bound).
		// Runtime and non-positive signed bounds need max(1, N) materialization
		// semantics and remain unsupported here.
		const auto& latch_info = m_program.block_info[m_ids.at(latch)];
		if (latch_info.terminator.kind == CFG::TerminatorKind::ConditionalBranch &&
		    latch_update.TryInstruction() != nullptr &&
		    latch_update.TryInstruction()->Parent() == latch && Dominates(read.Parent(), latch)) {
			auto latch_condition = latch_info.condition.Resolve();
			bool latch_invert = false;
			std::unordered_set<const Inst*> latch_condition_visited;
			while (const auto* inst = latch_condition.TryInstruction()) {
				if (!latch_condition_visited.insert(inst).second) return {};
				if (inst->GetOpcode() != ValueOpcode::LogicalNot || inst->NumArgs() != 1u) break;
				latch_invert = !latch_invert;
				latch_condition = inst->Arg(0).Resolve();
			}
			const auto* compare = latch_condition.TryInstruction();
			const auto repeat_id = latch_invert ? latch_info.terminator.false_block
			                                    : latch_info.terminator.true_block;
			if (compare != nullptr && compare->NumArgs() == 2u &&
			    repeat_id == m_program.block_info[m_ids.at(header)].id &&
			    compare->Arg(0).Resolve() == latch_update) {
				const auto bound = compare->Arg(1).Resolve();
				const bool unsigned_less = compare->GetOpcode() == ValueOpcode::ULessThan32;
				const bool signed_less = compare->GetOpcode() == ValueOpcode::SLessThan32;
				if (bound.IsImmediate() && bound.GetType() == Type::U32 && bound.U32() != 0u &&
				    (unsigned_less || (signed_less && bound.U32() <= INT32_MAX))) {
					const Block* source_scope = buffer_read ? read.Parent() : header;
					for (uint32_t word = 0; word < source_dwords; ++word)
						if (!RuntimeReadsDominate(address->Arg(word), source_scope,
						                          buffer_read ? &read : nullptr)) return {};
					return make_proof(bound);
				}
			}
		}
		// SSA construction may put the induction Phi in a separate empty header.
		// Follow only an unavoidable, single-entry unconditional chain to its
		// guard: every visit to the Phi must execute the same comparison.
		const Block* guard = header;
		std::unordered_set<const Block*> guard_chain;
		while (m_program.block_info[m_ids.at(guard)].terminator.kind ==
		       CFG::TerminatorKind::Branch) {
			if (!guard_chain.insert(guard).second) return {};
			const auto next_id = m_program.block_info[m_ids.at(guard)].terminator.true_block;
			const auto* next = m_by_id.at(next_id);
			if (next->ImmPredecessors().size() != 1u ||
			    next->ImmPredecessors().front() != guard) return {};
			guard = next;
		}
		const auto& info = m_program.block_info[m_ids.at(guard)];
		if (info.terminator.kind != CFG::TerminatorKind::ConditionalBranch) return {};
		auto condition = info.condition.Resolve();
		bool invert = false;
		std::unordered_set<const Inst*> condition_visited;
		while (const auto* inst = condition.TryInstruction()) {
			if (!condition_visited.insert(inst).second) return {};
			if (inst->GetOpcode() != ValueOpcode::LogicalNot || inst->NumArgs() != 1u) break;
			invert = !invert;
			condition = inst->Arg(0).Resolve();
		}
		const auto* compare = condition.TryInstruction();
		if (compare == nullptr || compare->NumArgs() != 2u) return {};
		Value count;
		bool count_signed = false;
		if (compare->GetOpcode() == ValueOpcode::ULessThan32 && compare->Arg(0).Resolve() == index)
			count = compare->Arg(1).Resolve();
		else if (compare->GetOpcode() == ValueOpcode::UGreaterThan32 && compare->Arg(1).Resolve() == index)
			count = compare->Arg(0).Resolve();
		else if (compare->GetOpcode() == ValueOpcode::SLessThan32 && compare->Arg(0).Resolve() == index) {
			count = compare->Arg(1).Resolve();
			count_signed = true;
		}
		else return {};
		if (count.GetType() != Type::U32 || !ValidateRuntimeValue(m_program, count)) return {};
		// Roots may be loaded in the preheader or in this unavoidable guard
		// chain. Both execute even when N is zero; success-only pointer loads
		// remain ineligible for eager snapshot evaluation.
		if (!RuntimeReadsDominate(count, guard)) return {};
		const Block* source_scope = buffer_read ? read.Parent() : guard;
		for (uint32_t word = 0; word < source_dwords; ++word)
			if (!RuntimeReadsDominate(address->Arg(word), source_scope,
			                          buffer_read ? &read : nullptr)) return {};
		const auto success_id = invert ? info.terminator.false_block : info.terminator.true_block;
		const auto* success = m_by_id.at(success_id);
		// Removing the successful i<N edge must make the actual read unreachable.
		// Block dominance alone is insufficient when the guard's false path merges.
		if (Reachable(read.Parent(), nullptr, guard, success)) return {};
		return make_proof(count, count_signed);
	}

private:
	std::optional<uint32_t> FiniteMaximum(Value value) {
		value = value.Resolve();
		if (value.GetType() != Type::U32) return {};
		if (value.IsImmediate()) return value.U32();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || !m_ids.contains(inst->Parent())) return {};
		if (const auto found = m_finite_values.find(inst); found != m_finite_values.end())
			return found->second;
		if (!m_finite_visiting.insert(inst).second) {
			// Dispatcher SSA can represent a finite selection as a cyclic Phi/Select
			// expression after structurization fails. These nodes only choose an
			// already-existing value; they do not manufacture a new range. Use the
			// least unsigned value for the revisited edge and let the acyclic arms
			// establish the actual maximum. Other cyclic arithmetic remains rejected.
			const auto opcode = inst->GetOpcode();
			return m_program.dispatcher_fallback &&
			               (opcode == ValueOpcode::Phi || opcode == ValueOpcode::SelectU32)
			           ? std::optional<uint32_t> {0u}
			           : std::nullopt;
		}
		const auto finish = [&](std::optional<uint32_t> result) {
			m_finite_visiting.erase(inst);
			m_finite_values.emplace(inst, result);
			return result;
		};
		if (inst->GetOpcode() == ValueOpcode::ReadFirstLane && inst->NumArgs() == 2u &&
		    inst->Arg(1).GetType() == Type::U1) {
			// Every source lane must be bounded, including lane0 for empty EXEC.
			// Matching a predicated write's mask alone does not justify erasing
			// its inactive old value; no GPU predicate is evaluated by this proof.
			if (const auto strict = FiniteMaximum(inst->Arg(0)); strict.has_value())
				return finish(strict);
			const auto active = inst->Arg(1).Resolve();
			if (!ProveActiveMaskNonempty(*inst, active)) return finish({});
			std::unordered_set<const Inst*> active_visiting;
			return finish(FiniteMaximumActive(inst->Arg(0), active, active_visiting));
		}
		if (inst->GetOpcode() == ValueOpcode::ReadLane && inst->NumArgs() == 2u) {
			const auto lane = FiniteMaximum(inst->Arg(1));
			if (!lane || *lane >= m_program.wave_size) return finish({});
			return finish(FiniteMaximum(inst->Arg(0)));
		}
		if (inst->GetOpcode() == ValueOpcode::ShiftRightLogical32 && inst->NumArgs() == 2u) {
			const auto shift = inst->Arg(1).Resolve();
			if (!shift.IsImmediate() || shift.GetType() != Type::U32) return finish({});
			const auto bits = shift.U32() & 31u;
			const auto source = FiniteMaximum(inst->Arg(0));
			return finish(source ? *source >> bits : UINT32_MAX >> bits);
		}
		if (inst->GetOpcode() == ValueOpcode::BitwiseAnd32 && inst->NumArgs() == 2u) {
			const auto left = inst->Arg(0).Resolve();
			const auto right = inst->Arg(1).Resolve();
			if (left.IsImmediate() && left.GetType() == Type::U32) return finish(left.U32());
			if (right.IsImmediate() && right.GetType() == Type::U32) return finish(right.U32());
			const auto left_bound = FiniteMaximum(left);
			const auto right_bound = FiniteMaximum(right);
			return finish(left_bound && right_bound ? std::optional(std::min(*left_bound, *right_bound))
			                                         : std::nullopt);
		}
		if (inst->GetOpcode() == ValueOpcode::SelectU32 && inst->NumArgs() == 3u) {
			const auto condition = inst->Arg(0).Resolve();
			if (condition.GetType() != Type::U1) return finish({});
			if (condition.IsImmediate())
				return finish(FiniteMaximum(inst->Arg(condition.U1() ? 1u : 2u)));
			const auto yes = FiniteMaximum(inst->Arg(1));
			const auto no = FiniteMaximum(inst->Arg(2));
			return finish(yes && no ? std::optional(std::max(*yes, *no)) : std::nullopt);
		}
		if (inst->GetOpcode() == ValueOpcode::BitFieldUExtract && inst->NumArgs() == 3u) {
			const auto offset = inst->Arg(1).Resolve();
			const auto width  = inst->Arg(2).Resolve();
			if (!offset.IsImmediate() || offset.GetType() != Type::U32 ||
			    !width.IsImmediate() || width.GetType() != Type::U32 ||
			    offset.U32() > 32u || width.U32() > 32u - offset.U32()) {
				return finish({});
			}
			if (width.U32() == 32u) return finish(UINT32_MAX);
			return finish(width.U32() == 0u ? 0u : (uint32_t {1} << width.U32()) - 1u);
		}
		if (inst->GetOpcode() == ValueOpcode::Phi && inst->NumArgs() != 0u &&
		    inst->NumArgs() == inst->NumPhiBlocks() &&
		    (m_program.dispatcher_fallback ||
		     inst->NumArgs() == inst->Parent()->ImmPredecessors().size())) {
			std::unordered_set<const Block*> incoming;
			uint32_t maximum = 0u;
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
				const auto* predecessor = inst->PhiBlock(arg);
				if (!m_ids.contains(predecessor) || !incoming.insert(predecessor).second ||
				    std::ranges::find(inst->Parent()->ImmPredecessors(), predecessor) ==
				        inst->Parent()->ImmPredecessors().end()) return finish({});
				const auto bound = FiniteMaximum(inst->Arg(arg));
				if (!bound) return finish({});
				maximum = std::max(maximum, *bound);
			}
			return finish(maximum);
		}
		return finish({});
	}

	std::optional<uint32_t> FiniteMaximumActive(
	    Value value, Value active, std::unordered_set<const Inst*>& visiting) {
		value = value.Resolve();
		active = active.Resolve();
		if (value.GetType() != Type::U32) return {};
		if (value.IsImmediate()) return value.U32();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || !m_ids.contains(inst->Parent()) || !visiting.insert(inst).second)
			return {};
		const auto finish = [&](std::optional<uint32_t> result) {
			visiting.erase(inst);
			return result;
		};
		if (inst->GetOpcode() == ValueOpcode::ReadFirstLane) {
			// A nested lane selection has its own mask and proof context.
			return finish(FiniteMaximum(value));
		}
		if (inst->GetOpcode() == ValueOpcode::SelectU32 && inst->NumArgs() == 3u) {
			const auto condition = inst->Arg(0).Resolve();
			if (condition.GetType() != Type::U1) return finish({});
			if (condition.IsImmediate())
				return finish(FiniteMaximumActive(
				    inst->Arg(condition.U1() ? 1u : 2u), active, visiting));
			if (condition == active)
				return finish(FiniteMaximumActive(inst->Arg(1), active, visiting));
			const auto yes = FiniteMaximumActive(inst->Arg(1), active, visiting);
			const auto no = FiniteMaximumActive(inst->Arg(2), active, visiting);
			return finish(yes && no ? std::optional(std::max(*yes, *no)) : std::nullopt);
		}
		if (inst->GetOpcode() == ValueOpcode::Phi && inst->NumArgs() != 0u &&
		    inst->NumArgs() == inst->NumPhiBlocks() &&
		    inst->NumArgs() == inst->Parent()->ImmPredecessors().size()) {
			std::unordered_set<const Block*> incoming;
			uint32_t maximum = 0u;
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
				const auto* predecessor = inst->PhiBlock(arg);
				if (!m_ids.contains(predecessor) || !incoming.insert(predecessor).second ||
				    std::ranges::find(inst->Parent()->ImmPredecessors(), predecessor) ==
				        inst->Parent()->ImmPredecessors().end()) return finish({});
				const auto bound = FiniteMaximumActive(inst->Arg(arg), active, visiting);
				if (!bound) return finish({});
				maximum = std::max(maximum, *bound);
			}
			return finish(maximum);
		}
		return finish({});
	}

	struct MaskWordProof {
		bool uniform = false;
		bool subset = false;
	};

	MaskWordProof ProveMaskWord(Value value, uint32_t component,
	                            std::unordered_set<const Inst*>& visiting) const {
		value = value.Resolve();
		if (value.GetType() != Type::U32) return {};
		if (value.IsImmediate()) return {true, value.U32() == 0u};
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || !visiting.insert(inst).second) return {};
		const auto finish = [&](MaskWordProof result) {
			visiting.erase(inst);
			return result;
		};
		if (inst->GetOpcode() == ValueOpcode::CompositeExtractU32x4 && inst->NumArgs() == 2u) {
			const auto index = inst->Arg(1).Resolve();
			const auto* ballot = inst->Arg(0).Resolve().TryInstruction();
			const bool entry_ballot = index.IsImmediate() && index.GetType() == Type::U32 &&
			                          ballot != nullptr && ballot->GetOpcode() == ValueOpcode::Ballot &&
			                          ballot->NumArgs() == 1u && ballot->Arg(0).Resolve() == Value(true) &&
			                          ballot->Parent() == m_program.blocks.front();
			return finish({entry_ballot, entry_ballot && index.U32() == component});
		}
		if ((inst->GetOpcode() == ValueOpcode::BitwiseAnd32 ||
		     inst->GetOpcode() == ValueOpcode::BitwiseOr32) && inst->NumArgs() == 2u) {
			const auto left = ProveMaskWord(inst->Arg(0), component, visiting);
			const auto right = ProveMaskWord(inst->Arg(1), component, visiting);
			if (!left.uniform || !right.uniform) return finish({});
			const bool subset = inst->GetOpcode() == ValueOpcode::BitwiseAnd32
			                        ? left.subset || right.subset
			                        : left.subset && right.subset;
			return finish({true, subset});
		}
		return finish({});
	}

	bool ParseThreadBit(Value active, Value& low, Value& high) const {
		active = active.Resolve();
		const auto* nonzero = active.TryInstruction();
		if (nonzero == nullptr || nonzero->GetOpcode() != ValueOpcode::INotEqual32 ||
		    nonzero->NumArgs() != 2u) return false;
		Value selected;
		if (Immediate(nonzero->Arg(0), 0u)) selected = nonzero->Arg(1).Resolve();
		else if (Immediate(nonzero->Arg(1), 0u)) selected = nonzero->Arg(0).Resolve();
		else return false;
		const auto* one = selected.TryInstruction();
		if (one == nullptr || one->GetOpcode() != ValueOpcode::BitwiseAnd32 || one->NumArgs() != 2u)
			return false;
		Value shifted;
		if (Immediate(one->Arg(0), 1u)) shifted = one->Arg(1).Resolve();
		else if (Immediate(one->Arg(1), 1u)) shifted = one->Arg(0).Resolve();
		else return false;
		const auto* shift = shifted.TryInstruction();
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftRightLogical32 ||
		    shift->NumArgs() != 2u) return false;
		const auto* bit = shift->Arg(1).Resolve().TryInstruction();
		if (bit == nullptr || bit->GetOpcode() != ValueOpcode::BitwiseAnd32 || bit->NumArgs() != 2u)
			return false;
		Value lane;
		if (Immediate(bit->Arg(0), 31u)) lane = bit->Arg(1).Resolve();
		else if (Immediate(bit->Arg(1), 31u)) lane = bit->Arg(0).Resolve();
		else return false;
		const auto* lane_inst = lane.TryInstruction();
		const auto* choose = shift->Arg(0).Resolve().TryInstruction();
		if (lane_inst == nullptr || lane_inst->GetOpcode() != ValueOpcode::LaneId ||
		    choose == nullptr || choose->GetOpcode() != ValueOpcode::SelectU32 ||
		    choose->NumArgs() != 3u) return false;
		const auto* upper = choose->Arg(0).Resolve().TryInstruction();
		if (upper == nullptr || upper->GetOpcode() != ValueOpcode::UGreaterThanEqual32 ||
		    upper->NumArgs() != 2u || upper->Arg(0).Resolve() != lane ||
		    !Immediate(upper->Arg(1), 32u)) return false;
		high = choose->Arg(1).Resolve();
		low = choose->Arg(2).Resolve();
		return true;
	}

	bool IsZeroMaskGuard(Value condition, Value low, Value high) const {
		condition = condition.Resolve();
		const auto* equal = condition.TryInstruction();
		if (equal == nullptr || equal->GetOpcode() != ValueOpcode::IEqual32 ||
		    equal->NumArgs() != 2u) return false;
		Value combined;
		if (Immediate(equal->Arg(0), 0u)) combined = equal->Arg(1).Resolve();
		else if (Immediate(equal->Arg(1), 0u)) combined = equal->Arg(0).Resolve();
		else return false;
		const auto* bit_or = combined.TryInstruction();
		if (bit_or == nullptr || bit_or->GetOpcode() != ValueOpcode::BitwiseOr32 ||
		    bit_or->NumArgs() != 2u) return false;
		const auto left = bit_or->Arg(0).Resolve();
		const auto right = bit_or->Arg(1).Resolve();
		return (left == low.Resolve() && right == high.Resolve()) ||
		       (left == high.Resolve() && right == low.Resolve());
	}

	bool GraphHasCycle(const Block* block, std::unordered_set<const Block*>& active,
	                   std::unordered_set<const Block*>& complete) const {
		if (complete.contains(block)) return false;
		if (!active.insert(block).second) return true;
		for (const auto* successor : block->ImmSuccessors())
			if (GraphHasCycle(successor, active, complete)) return true;
		active.erase(block);
		complete.insert(block);
		return false;
	}

	bool PostDominates(const Block* required, const Block* start) const {
		if (required == start) return true;
		std::vector<const Block*> work {start};
		std::unordered_set<const Block*> visited;
		while (!work.empty()) {
			const auto* block = work.back();
			work.pop_back();
			if (block == required || !visited.insert(block).second) continue;
			const auto& term = m_program.block_info[m_ids.at(block)].terminator;
			if (term.kind == CFG::TerminatorKind::Return) return false;
			for (const auto* successor : block->ImmSuccessors()) work.push_back(successor);
		}
		return true;
	}

	bool IsBallotZeroGuard(Value condition, Value active) const {
		condition = condition.Resolve();
		active = active.Resolve();
		const auto* equal = condition.TryInstruction();
		if (equal == nullptr || equal->GetOpcode() != ValueOpcode::IEqual32 ||
		    equal->NumArgs() != 2u) return false;
		Value combined;
		if (Immediate(equal->Arg(0), 0u)) combined = equal->Arg(1).Resolve();
		else if (Immediate(equal->Arg(1), 0u)) combined = equal->Arg(0).Resolve();
		else return false;
		const auto* bit_or = combined.TryInstruction();
		if (bit_or == nullptr || bit_or->GetOpcode() != ValueOpcode::BitwiseOr32 ||
		    bit_or->NumArgs() != 2u) return false;
		const auto left = bit_or->Arg(0).Resolve();
		const auto right = bit_or->Arg(1).Resolve();
		const auto extract = [&](Value value, uint32_t component) -> const Inst* {
			const auto* inst = value.TryInstruction();
			if (inst == nullptr || inst->GetOpcode() != ValueOpcode::CompositeExtractU32x4 ||
			    inst->NumArgs() != 2u) return nullptr;
			const auto index = inst->Arg(1).Resolve();
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() != component)
				return nullptr;
			const auto* ballot = inst->Arg(0).Resolve().TryInstruction();
			if (ballot == nullptr || ballot->GetOpcode() != ValueOpcode::Ballot ||
			    ballot->NumArgs() != 1u || ballot->Arg(0).Resolve() != active)
				return nullptr;
			return ballot;
		};
		const auto* left_ballot = extract(left, 0u);
		const auto* right_ballot = extract(right, 1u);
		return left_ballot != nullptr && left_ballot == right_ballot;
	}

	bool ProveActiveMaskNonempty(const Inst& read_first_lane, Value active) const {
		if (read_first_lane.Parent() == nullptr || !m_ids.contains(read_first_lane.Parent()))
			return false;
		std::unordered_set<const Block*> graph_active;
		std::unordered_set<const Block*> graph_complete;
		if (GraphHasCycle(m_program.blocks.front(), graph_active, graph_complete)) return false;
		active = active.Resolve();
		Value low, high;
		bool classic_words = false;
		if (ParseThreadBit(active, low, high)) {
			std::unordered_set<const Inst*> low_visiting;
			std::unordered_set<const Inst*> high_visiting;
			const auto low_proof = ProveMaskWord(low, 0u, low_visiting);
			const auto high_proof = ProveMaskWord(high, 1u, high_visiting);
			classic_words = low_proof.uniform && low_proof.subset && high_proof.uniform &&
			                high_proof.subset;
		}
		// The empty/nonempty guard must dominate the ReadFirstLane use. Requiring
		// the guard to post-dominate the shader entry rejects real shaders that
		// exit early on unrelated paths before this descriptor selection.
		// Accept either the classic thread-bit/low|high EXEC guard or a Ballot(active)
		// emptiness check on the exact ReadFirstLane mask.
		for (const auto* guard : m_program.blocks) {
			const auto& info = m_program.block_info[m_ids.at(guard)];
			if (info.terminator.kind != CFG::TerminatorKind::ConditionalBranch) continue;
			const bool classic =
			    classic_words && IsZeroMaskGuard(info.condition, low, high);
			const bool ballot = IsBallotZeroGuard(info.condition, active);
			if (!classic && !ballot) continue;
			const auto* nonempty = m_by_id.at(info.terminator.false_block);
			if (!Dominates(guard, read_first_lane.Parent()) ||
			    !Dominates(nonempty, read_first_lane.Parent()) ||
			    !PostDominates(read_first_lane.Parent(), nonempty)) continue;
			return true;
		}
		return false;
	}

	static bool Immediate(Value value, uint32_t expected) {
		value = value.Resolve();
		return value.IsImmediate() && value.GetType() == Type::U32 && value.U32() == expected;
	}
	static bool Precedes(const Inst& definition, const Inst& use) {
		if (definition.Parent() == nullptr || definition.Parent() != use.Parent()) return false;
		for (const auto& inst : *use.Parent()) {
			if (&inst == &use) return false;
			if (&inst == &definition) return true;
		}
		return false;
	}
	bool DispatcherEntryPrefixPrecedes(const Inst& definition, const Inst& use) const {
		if (definition.Parent() == nullptr || use.Parent() == nullptr ||
		    !m_ids.contains(definition.Parent()) || !m_ids.contains(use.Parent())) return false;
		const Block* current = m_program.blocks.front();
		bool definition_block_seen = false;
		std::unordered_set<const Block*> visited;
		for (;;) {
			if (!visited.insert(current).second) return false;
			if (current == definition.Parent()) {
				if (current == use.Parent()) return Precedes(definition, use);
				definition_block_seen = true;
			}
			if (current == use.Parent()) return definition_block_seen;
			const auto& terminator = m_program.block_info[m_ids.at(current)].terminator;
			if (terminator.kind != CFG::TerminatorKind::Branch ||
			    !m_by_id.contains(terminator.true_block)) return false;
			const auto* next = m_by_id.at(terminator.true_block);
			if (next->ImmPredecessors().size() != 1u ||
			    next->ImmPredecessors().front() != current) return false;
			current = next;
		}
	}
	bool IsDispatcherDescriptorTableRead(const Inst& read, Value selector) const {
		struct Handle {
			const Inst* handle;
			uint32_t    width;
		};
		std::vector<Handle> handles;
		for (const auto& use: read.Uses()) {
			if (use.user == nullptr) continue;
			const auto opcode = use.user->GetOpcode();
			const uint32_t width = opcode == ValueOpcode::GetImageResource ? 8u
			                         : opcode == ValueOpcode::GetBufferResource ? 4u : 0u;
			if (width == 0u || use.operand >= width) continue;
			if (std::ranges::none_of(handles, [&](const Handle& candidate) {
				    return candidate.handle == use.user;
			    }))
				handles.push_back({use.user, width});
		}
		if (handles.empty() || selector.Resolve().TryInstruction() == nullptr) return false;
		const auto trace = [] {
			const char* value = std::getenv("KYTY_SHADER_PHASE_TRACE");
			return value != nullptr && *value != '\0' && std::string_view(value) != "0";
		}();
		for (const auto& candidate: handles) {
			const auto* handle = candidate.handle;
			const auto width = candidate.width;
			uint32_t scale = 0;
			uint32_t bias = 0;
			bool address_table = false;
			bool valid = true;
			if (handle->NumArgs() != width) continue;
			for (uint32_t word = 0; word < width; ++word) {
				const auto* column = handle->Arg(word).Resolve().TryInstruction();
				const bool address_read = column != nullptr &&
				                          column->GetOpcode() == ValueOpcode::LoadAddressU32 &&
				                          column->NumArgs() == 4u;
				const bool buffer_read = column != nullptr &&
				                         column->GetOpcode() == ValueOpcode::ReadConstBuffer &&
				                         column->NumArgs() == 2u;
				if (!address_read && !buffer_read) {
					valid = false;
					break;
				}
				BoundedOffset offset;
				std::unordered_set<const Inst*> visiting;
				const bool parsed = ParseBoundedOffset(column->Arg(1), offset, visiting);
				const bool same_selector =
				    parsed && offset.index != nullptr &&
				    EquivalentValue(m_program, Value(const_cast<Inst*>(offset.index)), selector);
				if (!parsed || !same_selector) {
					if (trace && buffer_read)
						std::fprintf(stderr,
						             "shader SRT: descriptor table column rejected word=%u parsed=%s "
						             "index=%s same_selector=%s\n",
						             word, parsed ? "yes" : "no",
						             offset.index == nullptr
						                 ? "none"
						                 : ValueOpcodeName(offset.index->GetOpcode()).data(),
						             same_selector ? "yes" : "no");
					valid = false;
					break;
				}
				if (word == 0u) {
					scale = offset.scale;
					bias = offset.bias;
					address_table = address_read;
				} else if (offset.scale != scale ||
				           (address_table && offset.bias != bias)) {
					valid = false;
					break;
				}
			}
			if (valid) return true;
		}
		return false;
	}
	bool RuntimeReadsPrecedeEntry(Value root, const Inst& before) const {
		struct PendingValue {
			Value value;
			bool  position_covered = false;
		};
		std::vector<PendingValue> work {{root, false}};
		std::unordered_set<const Inst*> visited_covered;
		std::unordered_set<const Inst*> visited_uncovered;
		while (!work.empty()) {
			const auto pending = work.back();
			work.pop_back();
			const auto value = pending.value.Resolve();
			const auto* inst = value.TryInstruction();
			if (inst == nullptr) continue;
			if (pending.position_covered) {
				if (visited_uncovered.contains(inst) || !visited_covered.insert(inst).second) continue;
			} else if (!visited_uncovered.insert(inst).second) {
				continue;
			}
			bool covered = pending.position_covered;
			if (inst->GetOpcode() == ValueOpcode::ReadConst) {
				const auto slot = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size() ||
				    !DispatcherEntryPrefixPrecedes(*inst, before)) return false;
				work.push_back({m_program.srt_reads[slot.U32()].value, true});
				covered = true;
			}
			if (!covered && (inst->GetOpcode() == ValueOpcode::LoadAddressU32 ||
			                 inst->GetOpcode() == ValueOpcode::ReadConstBuffer) &&
			    !DispatcherEntryPrefixPrecedes(*inst, before)) return false;
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg)
				work.push_back({inst->Arg(arg), covered});
		}
		return true;
	}
	bool RuntimeReadsDominate(Value root, const Block* header, const Inst* before = nullptr) const {
		struct PendingValue {
			Value value;
			bool position_covered = false;
		};
		std::vector<PendingValue> work {{root, false}};
		std::unordered_set<const Inst*> visited_covered;
		std::unordered_set<const Inst*> visited_uncovered;
		while (!work.empty()) {
			const auto pending = work.back();
			work.pop_back();
			const auto value = pending.value.Resolve();
			const auto* inst = value.TryInstruction();
			if (inst == nullptr) continue;
			if (pending.position_covered) {
				if (visited_uncovered.contains(inst) || !visited_covered.insert(inst).second) continue;
			} else if (!visited_uncovered.insert(inst).second) {
				continue;
			}
			if (inst->GetOpcode() == ValueOpcode::ReadConst) {
				const auto slot = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) return false;
				// BuildSrtPlan may remove the raw instruction from its block after
				// inserting this flat snapshot use. The surviving ReadConst owns the
				// original execution position for the complete source expression.
				if (inst->Parent() == nullptr || !Dominates(inst->Parent(), header)) return false;
				if (before != nullptr && (!Dominates(inst->Parent(), before->Parent()) ||
				    (inst->Parent() == before->Parent() && !Precedes(*inst, *before)))) return false;
				work.push_back({m_program.srt_reads[slot.U32()].value, true});
			}
			if (inst->GetOpcode() == ValueOpcode::LoadAddressU32 ||
			    inst->GetOpcode() == ValueOpcode::ReadConstBuffer) {
				if (!pending.position_covered) {
					if (inst->Parent() == nullptr || !Dominates(inst->Parent(), header)) return false;
					if (before != nullptr && (!Dominates(inst->Parent(), before->Parent()) ||
					    (inst->Parent() == before->Parent() && !Precedes(*inst, *before)))) return false;
				}
			}
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg)
				work.push_back({inst->Arg(arg), pending.position_covered});
		}
		return true;
	}
	bool BuildBlockIndex() {
		for (uint32_t i = 0; i < m_program.blocks.size(); ++i) {
			if (m_program.blocks[i] == nullptr || !m_ids.emplace(m_program.blocks[i], i).second ||
			    m_program.block_info[i].id == UINT32_MAX ||
			    !m_by_id.emplace(m_program.block_info[i].id, m_program.blocks[i]).second) return false;
		}
		return true;
	}
	bool BuildGraph() {
		if (!BuildBlockIndex()) return false;
		for (uint32_t i = 0; i < m_program.blocks.size(); ++i) {
			const auto& term = m_program.block_info[i].terminator;
			std::vector<const Block*> expected;
			if (term.kind == CFG::TerminatorKind::Branch || term.kind == CFG::TerminatorKind::ConditionalBranch) {
				if (!m_by_id.contains(term.true_block)) return false;
				expected.push_back(m_by_id.at(term.true_block));
			}
			if (term.kind == CFG::TerminatorKind::ConditionalBranch) {
				if (!m_by_id.contains(term.false_block) || term.true_block == term.false_block) return false;
				expected.push_back(m_by_id.at(term.false_block));
			}
			if (term.kind != CFG::TerminatorKind::Branch && term.kind != CFG::TerminatorKind::ConditionalBranch &&
			    term.kind != CFG::TerminatorKind::Return) return false;
			const auto actual = m_program.blocks[i]->ImmSuccessors();
			if (actual.size() != expected.size()) return false;
			for (const auto* successor : actual)
				if (std::ranges::find(expected, successor) == expected.end()) return false;
		}
		return true;
	}
	bool Reachable(const Block* target, const Block* exclude = nullptr,
	               const Block* edge_from = nullptr, const Block* edge_to = nullptr) const {
		std::vector<const Block*> work {m_program.blocks.front()};
		std::unordered_set<const Block*> visited;
		while (!work.empty()) {
			const auto* block = work.back();
			work.pop_back();
			if (block == exclude || !visited.insert(block).second) continue;
			if (block == target) return true;
			for (const auto* successor : block->ImmSuccessors())
				if (block != edge_from || successor != edge_to) work.push_back(successor);
		}
		return false;
	}
	bool Dominates(const Block* dominator, const Block* block) const {
		return m_ids.contains(dominator) && m_ids.contains(block) && Reachable(block) &&
		       (dominator == block || !Reachable(block, dominator));
	}
	const Program& m_program;
	std::unordered_map<const Block*, uint32_t> m_ids;
	std::unordered_map<uint32_t, const Block*> m_by_id;
	std::unordered_map<const Inst*, std::optional<uint32_t>> m_finite_values;
	std::unordered_set<const Inst*> m_finite_visiting;
};

} // namespace

SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     std::span<const uint8_t> clean_flat_slots, SrtWalker* clean_evaluator,
                     Value active_mask, std::span<const BoundedSrtLayout> bounded_layouts,
                     std::span<const uint32_t> bounded_flat,
                     std::optional<uint32_t> bounded_candidate)
    : m_program(program), m_runtime(runtime), m_clean_flat_slots(clean_flat_slots),
      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()),
      m_bounded_layouts(bounded_layouts), m_bounded_flat(bounded_flat),
      m_bounded_candidate(bounded_candidate),
      m_context(AcquireContext(program)) {}

SrtWalker::~SrtWalker() { --m_program.evaluation_depth; }

bool SrtWalker::Evaluate(Value value, uint32_t& result) {
	uint64_t wide = 0;
	if (!EvaluateWide(value, wide)) {
		return false;
	}
	result = static_cast<uint32_t>(wide);
	return true;
}

ResourcePlan::EvaluationContext& SrtWalker::AcquireContext(const ResourcePlan& program) {
	if (program.evaluation_depth == program.evaluation_contexts.size()) {
		program.evaluation_contexts.emplace_back();
	}
	auto& context = program.evaluation_contexts[program.evaluation_depth++];
	context.generation += 2;
	return context;
}

float SrtWalker::Float32(uint64_t bits) {
	return std::bit_cast<float>(static_cast<uint32_t>(bits));
}

bool SrtWalker::EvaluateWide(Value value, uint64_t& result) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		switch (value.GetType()) {
			case Type::U1: result = value.U1(); return true;
			case Type::U8: result = value.U8(); return true;
			case Type::U16: result = value.U16(); return true;
			case Type::U32: result = value.U32(); return true;
			case Type::U64: result = value.U64(); return true;
			case Type::F32: result = std::bit_cast<uint32_t>(value.F32Value()); return true;
			default: return false;
		}
	}
	auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return false;
	}
	if (!m_active_mask.IsEmpty() && IsRuntimeSelect(inst->GetOpcode()) &&
	    inst->NumArgs() == 3 && inst->Arg(0).Resolve() == m_active_mask) {
		return EvaluateWide(inst->Arg(1), result);
	}
	const auto index = inst->EvaluationIndex(m_program.evaluation_value_count);
	if (index >= m_context.values.size()) {
		m_context.values.resize(m_program.evaluation_value_count);
	}
	if (m_context.values[index].generation == m_context.generation) {
		result = m_context.values[index].value;
		return true;
	}
	// The low generation bit marks an instruction that is still being evaluated.
	if (m_context.values[index].generation == (m_context.generation | 1u)) {
		return false;
	}
	m_context.values[index].generation = m_context.generation | 1u;
	uint64_t out = 0;
	const bool evaluated = EvaluateInst(*inst, out);
	// Recursive evaluation may grow the dense memo vector.
	auto& memo = m_context.values[index];
	if (!evaluated) {
		memo.generation = 0;
		return false;
	}
	memo.value      = out;
	memo.generation = m_context.generation;
	result = out;
	return true;
}

bool SrtWalker::Arg(const Inst& inst, size_t index, uint64_t& result) {
	return EvaluateWide(inst.Arg(index), result);
}

bool SrtWalker::EvaluatePhi(const Inst& inst, uint64_t& result) {
	const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
	return !value.IsEmpty() && EvaluateWide(value, result);
}

bool SrtWalker::EvaluateExtract(const Inst& inst, uint64_t& result) {
	const auto index = inst.Arg(1).Resolve();
	if (!index.IsImmediate() || index.GetType() != Type::U32) {
		return false;
	}
	const auto component = index.U32();
	if (component >= 2u) {
		return false;
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
		return false;
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
	return false;
}

bool SrtWalker::EvaluateRawRead(const Inst& inst, uint64_t& result) {
	const auto flags = inst.Flags<MemoryFlags>();
	if (flags.index >= m_program.memory_info.size()) {
		return false;
	}
	const auto& mem    = m_program.memory_info[flags.index];
	const auto* handle = inst.Arg(0).ResolveInstruction();
	if (handle == nullptr) {
		return false;
	}
	uint64_t low    = 0;
	uint64_t high   = 0;
	uint64_t offset = 0;
	if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
		return false;
	}
	const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
	const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
	uint64_t   address   = 0;
	if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
		uint64_t records = 0;
		uint64_t word3   = 0;
		if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
			return false;
		}
		if (immediate < 0) {
			return false;
		}
		const auto byte_offset =
		    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
		const auto aligned = byte_offset & ~uint64_t {3};
		const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
		const auto size = stride == 0u
		                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
		                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
		if (aligned > size || size - aligned < sizeof(uint32_t)) {
			return false;
		}
		address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
	} else {
		const auto relative = (immediate & ~int64_t {3}) +
		                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
		if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
			return false;
		}
	}
	uint32_t word = 0;
	if (m_runtime.read_memory != nullptr) {
		if (!m_runtime.read_memory(m_runtime.userdata, address, {&word, 1})) {
			return false;
		}
	} else {
		std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
	}
	result = word;
	return true;
}

bool SrtWalker::EvaluateInst(const Inst& inst, uint64_t& result) {
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
				return false;
			}
			result = m_runtime.user_data[reg - m_program.user_data_base];
			return true;
		}
		case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
		case ValueOpcode::Phi: return EvaluatePhi(inst, result);
		case ValueOpcode::ReadFirstLane: {
			const auto clean_runtime = CleanRuntime(m_runtime);
			SrtWalker  clean_active(m_program, clean_runtime, {}, nullptr, inst.Arg(1));
			SrtWalker  active(m_program, m_runtime, m_clean_flat_slots, &clean_active,
			                  inst.Arg(1));
			return active.EvaluateWide(inst.Arg(0), result);
		}
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32: return Arg(inst, 0, result);
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
		case ValueOpcode::CompositeConstructU64:
			if (!binary()) {
				return false;
			}
			result = static_cast<uint32_t>(a) |
			         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
			return true;
		case ValueOpcode::ReadConst: {
			const auto slot = inst.Arg(1).Resolve();
			if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return false;
			}
			if (slot.U32() < m_clean_flat_slots.size() &&
			    m_clean_flat_slots[slot.U32()] != 0u && m_clean_evaluator != nullptr) {
				return m_clean_evaluator->EvaluateWide(m_program.srt_reads[slot.U32()].value,
				                                       result);
			}
			return EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
		}
		case ValueOpcode::ReadBoundedSrtU32: {
			if (!m_bounded_candidate.has_value()) {
				return false;
			}
			const auto id = inst.Flags<uint32_t>();
			if (id >= m_bounded_layouts.size()) {
				return false;
			}
			const auto& layout = m_bounded_layouts[id];
			if (*m_bounded_candidate >= layout.count ||
			    layout.flat_offset > m_bounded_flat.size() ||
			    *m_bounded_candidate >= m_bounded_flat.size() - layout.flat_offset) {
				return false;
			}
			result = m_bounded_flat[layout.flat_offset + *m_bounded_candidate];
			return true;
		}
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::ReadConstBuffer:
			if (IsRawRead(m_program, inst)) {
				return EvaluateRawRead(inst, result);
			}
			break;
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
				result = std::bit_cast<uint32_t>(static_cast<float>(static_cast<uint32_t>(a)));
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
				result = std::bit_cast<uint32_t>(Float32(a) * Float32(b));
				return true;
			}
			return false;
		case ValueOpcode::FPTrunc32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(std::trunc(Float32(a)));
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
		case ValueOpcode::SelectF32: {
			auto& predicate = m_clean_evaluator != nullptr ? *m_clean_evaluator : *this;
			if (predicate.EvaluateWide(inst.Arg(0), a)) {
				return Arg(inst, a != 0u ? 1u : 2u, result);
			}
			return false;
		}
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
		case ValueOpcode::SGreaterThanEqual32:
			if (binary()) {
				result = std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >=
				         std::bit_cast<int32_t>(static_cast<uint32_t>(b));
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
		case ValueOpcode::UndefU64: return false;
		default: break;
	}
	return false;
}
bool SrtWalker::EvaluateDescriptor(uint32_t source, DescriptorValue& result) {
	if (source >= m_program.descriptor_sources.size()) {
		return false;
	}
	const auto& descriptor = m_program.descriptor_sources[source];
	result = {};
	result.dword_count = descriptor.dword_count;
	for (uint32_t index = 0; index < descriptor.dword_count; ++index) {
		if (!Evaluate(descriptor.dwords[index], result.dwords[index])) {
			return false;
		}
	}
	return true;
}

std::span<const uint8_t> SrtWalker::FindActiveSources() {
	if (m_program.control_flow.empty()) {
		return {};
	}
	auto& active = m_program.active_sources;
	active.assign(m_program.descriptor_sources.size(), 1u);
	for (const auto& block: m_program.control_flow) {
		for (const auto source: block.sources) {
			active.at(source) = 0u;
		}
	}
	auto& visited = m_program.visited_blocks;
	auto& pending = m_program.pending_blocks;
	visited.assign(m_program.control_flow.size(), 0u);
	pending.clear();
	pending.push_back(0u);
	while (!pending.empty()) {
		const auto index = pending.back();
		pending.pop_back();
		if (visited.at(index)) {
			continue;
		}
		visited[index] = 1u;
		const auto& block = m_program.control_flow[index];
		for (const auto source: block.sources) {
			active[source] = 1u;
		}
		uint32_t condition = 0;
		if (!block.condition.IsEmpty() && m_runtime.read_specialization_memory != nullptr &&
		    Evaluate(block.condition, condition)) {
			pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
		} else {
			pending.insert(pending.end(), block.successors.begin(), block.successors.end());
		}
	}
	return active;
}

bool SrtWalker::RefreshFlatBuffer(std::vector<uint32_t>& flat) {
	if (!m_program.srt_plan_complete) {
		return false;
	}
	flat.resize(m_program.srt_reads.size());
	for (const auto& read: m_program.srt_reads) {
		const bool clean = read.flat_offset < m_clean_flat_slots.size() &&
		                   m_clean_flat_slots[read.flat_offset] != 0u;
		if (clean && (m_clean_evaluator == nullptr || m_runtime.read_specialization_memory == nullptr)) {
			return false;
		}
		auto& evaluator = clean ? *m_clean_evaluator : *this;
		if (read.flat_offset >= flat.size() || !evaluator.Evaluate(read.value, flat[read.flat_offset])) {
			return false;
		}
	}
	return true;
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result) {
	SrtWalker evaluator(program, runtime);
	return evaluator.EvaluateDescriptor(source, result);
}

bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results) {
	SrtWalker evaluator(program, runtime);
	std::vector<DescriptorValue> next;
	next.reserve(sources.size());
	for (const auto source : sources) {
		DescriptorValue value;
		if (!evaluator.EvaluateDescriptor(source, value)) return false;
		next.push_back(value);
	}
	results = std::move(next);
	return true;
}

bool EvaluateBoundedDescriptorSource(const ResourcePlan& program, uint32_t source,
                                     const SrtRuntime& runtime,
                                     std::span<const BoundedSrtLayout> layouts,
                                     std::span<const uint32_t> flattened_srt,
                                     uint32_t candidate, DescriptorValue& result) {
	SrtWalker evaluator(program, runtime, {}, nullptr, {}, layouts, flattened_srt, candidate);
	return evaluator.EvaluateDescriptor(source, result);
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat,
                            std::span<const uint8_t> clean_flat_slots) {
	auto clean_runtime = CleanRuntime(runtime);
	SrtWalker clean(program, clean_runtime, clean_flat_slots);
	SrtWalker evaluator(program, runtime, clean_flat_slots, &clean);
	std::vector<DescriptorValue> next;
	next.reserve(sources.size());
	for (const auto source: sources) {
		DescriptorValue value;
		if (!evaluator.EvaluateDescriptor(source, value)) {
			return false;
		}
		next.push_back(value);
	}
	std::vector<uint32_t> next_flat;
	if (!evaluator.RefreshFlatBuffer(next_flat)) {
		return false;
	}
	results = std::move(next);
	flat    = std::move(next_flat);
	return true;
}

std::optional<BoundedSrtReadProof> ProveBoundedSrtRead(const Program& program,
                                                     const Inst& read) {
	return BoundedReadProof(program).Run(read);
}

void BuildSrtPlan(Program& program) {
	if (program.resource_tracking_complete) {
		EXIT("shader SRT planning failed: cannot rebuild SRT after resource tracking");
	}
	program.srt_plan_complete = false;
	PlanBuilder(program).Run();
	program.srt_plan_complete = true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
