#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
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

SrtRuntime CleanRuntime(SrtRuntime runtime) {
	runtime.read_memory = runtime.read_specialization_memory != nullptr
	                          ? runtime.read_specialization_memory
	                          : +[](void*, uint64_t, uint32_t*) { return false; };
	return runtime;
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

std::string Diagnostic(const ResourcePlan& program, uint32_t pc, const std::string& message) {
	return fmt::format("shader SRT: hash=0x{:016x} stage={} pc=0x{:08x} {}", program.shader_hash,
	                   StageName(program.stage), pc, message);
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

// A vector-path buffer load whose address carries no lane identity: no index, no
// per-lane offset, one plain dword. Hardware reaches it as base + soffset + imm, which
// is the same shape the scalar reads below already re-execute, so the host can
// reproduce it. The gate used to refuse every MUBUF load on its resource kind alone,
// which dropped whole shaders whose only sin was fetching one uniform word through the
// vector path - see the two decoupled-lookback scan passes in SILENT HILL 2.
//
// A DWORDX4 load of that shape reads four such words at once, which is how a whole V# stored in a
// buffer reaches the scalar registers; each component is then taken apart by a CompositeExtract.
bool IsUniformBufferRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if ((op != ValueOpcode::LoadBufferU32 && op != ValueOpcode::LoadBufferU32x4) ||
	    inst.NumArgs() != 5) {
		return false;
	}
	const auto flags = inst.Flags<MemoryFlags>();
	if (flags.index >= values.memory_info.size()) {
		return false;
	}
	const auto& mem = values.memory_info[flags.index];
	const auto  dwords = op == ValueOpcode::LoadBufferU32x4 ? 4u : 1u;
	if (mem.kind != ResourceKind::Buffer || mem.idxen || mem.offen || mem.typed ||
	    mem.formatted || mem.data_bits != 32u || mem.data_dwords != dwords ||
	    mem.component_index != 0u) {
		return false;
	}
	// The translator plants literal zeroes for the index and offset operands when the
	// instruction does not use them; require that rather than trusting the flags alone.
	for (size_t arg = 1; arg <= 2; arg++) {
		const auto value = inst.Arg(arg).Resolve();
		if (!value.IsImmediate() || value.GetType() != Type::U32 || value.U32() != 0u) {
			return false;
		}
	}
	return true;
}

// Which argument carries the read's dynamic byte offset. The scalar reads put it
// second; a MUBUF load puts the index and the per-lane offset there and the scalar
// offset fourth.
size_t RawReadOffsetArg(ValueOpcode op) {
	return op == ValueOpcode::LoadBufferU32 || op == ValueOpcode::LoadBufferU32x4 ? 3u : 1u;
}

// A MUBUF load carries the exec mask as its last operand. That is lane state, not part
// of the address, and walking it would reject the read for depending on lane identity.
size_t RawReadAddressArgs(const Inst& inst) {
	const auto op = inst.GetOpcode();
	return op == ValueOpcode::LoadBufferU32 || op == ValueOpcode::LoadBufferU32x4 ? 4u
	                                                                               : inst.NumArgs();
}

// A single-dword read, which planning can patch to a flat U32 slot. A uniform DWORDX4 load is
// deliberately not one: its value is a U32x4, and only its extracts are re-executed.
bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op == ValueOpcode::LoadBufferU32) {
		return IsUniformBufferRead(values, inst);
	}
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

// Opcodes the host evaluator can re-execute unconditionally. Must stay in lockstep with
// Evaluator::EvaluateInst below: an opcode accepted here with no case there passes the
// compile-time gate and then fails at pipeline-build time with nothing to name. A debug guard at
// the tail of that switch asserts the two lists agree. LaneId is deliberately absent: it is legal
// only inside a readfirstlane or a ballot, so both halves gate it on that scope instead -
// RuntimeValidator on a non-empty active mask, Evaluator on the lane scope those opcodes open.
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
		case ValueOpcode::CompositeExtractU32x4:
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
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
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
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
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
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	RuntimeValidator(const ResourcePlan& program, RuntimeValueType type,
	                 RuntimeValueFailure* failure)
	    : m_program(program), m_type(type), m_failure(failure) {}

	bool Run(Value value) { return Validate(value); }

private:
	// Records why the walk stopped and always returns false, so it can stand in for a bare false.
	// The first rejection wins: nothing here retries a value, so the first refusal is the
	// instruction that actually failed. A caller that wants no reason pays one null test.
	bool Reject(RuntimeValueReject reason) {
		if (m_failure != nullptr && m_failure->reason == RuntimeValueReject::None) {
			m_failure->reason = reason;
		}
		return false;
	}

	bool Reject(RuntimeValueReject reason, ValueOpcode opcode) {
		if (m_failure != nullptr && m_failure->reason == RuntimeValueReject::None) {
			m_failure->reason     = reason;
			m_failure->opcode     = opcode;
			m_failure->has_opcode = true;
		}
		return false;
	}

	// The two entry operands are the whole story for a merge, so they travel with the reason.
	bool RejectMerge(Value entry, Value other, ValueOpcode opcode) {
		const auto operand_opcode = [](Value value) {
			const auto* inst = value.Resolve().TryInstruction();
			return inst != nullptr ? inst->GetOpcode() : ValueOpcode::Void;
		};
		if (m_failure != nullptr && m_failure->reason == RuntimeValueReject::None) {
			Reject(RuntimeValueReject::CyclicValueMerge, opcode);
			m_failure->entry_opcode      = operand_opcode(entry);
			m_failure->other_opcode      = operand_opcode(other);
			m_failure->has_entry_opcodes = true;
			return false;
		}
		return Reject(RuntimeValueReject::CyclicValueMerge, opcode);
	}

	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		const auto count = RawReadAddressArgs(inst);
		for (size_t index = 0; index < count; index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		value = value.Resolve();
		// Host floating-point evaluation does not model shader rounding/denormal modes.
		if (m_type == RuntimeValueType::Integer &&
		    TypesOverlap(value.GetType(), Type::F16 | Type::F32 | Type::F32x2)) {
			const auto* source = value.TryInstruction();
			return source != nullptr
			           ? Reject(RuntimeValueReject::FloatInIntegerChain, source->GetOpcode())
			           : Reject(RuntimeValueReject::FloatInIntegerChain);
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
				default: return Reject(RuntimeValueReject::UnsupportedOperand);
			}
		}
		// Integer-only dependency checks do not depend on the active EXEC mask.
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		const auto active_mask_at_entry = m_active_mask;
		// Uniform acceptance holds only under the EXEC mask it was proven with.
		if (require_uniform) {
			const auto cached = m_validated_uniform.find(inst);
			if (cached != m_validated_uniform.end() &&
			    std::find(cached->second.begin(), cached->second.end(), active_mask_at_entry) !=
			        cached->second.end()) {
				return true;
			}
		}
		if (!m_visiting.insert(inst).second) {
			if (!require_uniform) return true;
			return Reject(RuntimeValueReject::CyclicValue, inst->GetOpcode());
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
			if (valid && require_uniform) m_validated_uniform[inst].push_back(active_mask_at_entry);
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
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
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
			return finish(Reject(RuntimeValueReject::UndefinedValue, op));
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			return finish(true);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type == RuntimeValueType::Integer && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (!invariant.IsEmpty()) {
				return finish(Validate(invariant));
			}
			// Accept the value the loop is entered with; Evaluator proves it is a fixpoint
			// before anything is bound.
			CyclicPhiFailure cyclic;
			const auto       entry = ResolveCyclicPhiEntry(m_program, value, nullptr, &cyclic);
			if (entry.IsEmpty()) {
				// Name which of the two shapes stopped the walk: a web nothing enters needs a
				// runtime descriptor, while disagreeing entry values only need proving equal.
				if (cyclic.reason != CyclicPhiReject::Merge) {
					return finish(Reject(RuntimeValueReject::CyclicValueNoEntry, op));
				}
				return finish(RejectMerge(cyclic.entry, cyclic.other, op));
			}
			return finish(Validate(entry));
		}
		if (op == ValueOpcode::LaneId) {
			// A descriptor lives in scalar registers, so the only route a lane index has into one
			// is the readfirstlane that lifts a vector value back into them - in practice the
			// emulator's own model of "bit N of a scalar mask, for this lane", which hardware
			// never spells per-lane at all. There the lane term has to cancel, and Evaluator
			// proves it does by re-executing the operand for every lane. Outside that scope
			// nothing bounds the lane, so the value stays refused.
			if (inst->NumArgs() != 0) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			if (m_active_mask.IsEmpty()) {
				return finish(Reject(RuntimeValueReject::UnsupportedOpcode, op));
			}
			return finish(true);
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
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
		if (op == ValueOpcode::Ballot) {
			// The other way a vector value reaches the scalar registers: a VCMP into an SGPR, or
			// EXEC read as a scalar, collects one bit per lane. The predicate is re-executed for
			// every lane, so a lane index inside it is bound, and every lane counts - hence an
			// all-true mask rather than the enclosing readfirstlane's.
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::U1) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = Value(true);
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			return finish(true);
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer ||
		    op == ValueOpcode::LoadBufferU32) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
		} else if (op == ValueOpcode::LoadBufferU32x4) {
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsUniformBufferRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != ValueOpcode::GetBufferResource) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
		} else if (op == ValueOpcode::CompositeExtractU32x4) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 4u ||
			    (source->GetOpcode() != ValueOpcode::Ballot &&
			     source->GetOpcode() != ValueOpcode::LoadBufferU32x4)) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u ||
			    (source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 &&
			     source->GetOpcode() != ValueOpcode::IAddCarry32)) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
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
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 &&
		           !((op == ValueOpcode::LoadBufferU32 || op == ValueOpcode::LoadBufferU32x4) &&
		             IsUniformBufferRead(m_program, *inst)) &&
		           !IsRuntimeUniformOp(op)) {
			return finish(Reject(RuntimeValueReject::UnsupportedOpcode, op));
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	RuntimeValueFailure*            m_failure = nullptr;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
	std::unordered_map<const Inst*, std::vector<Value>> m_validated_uniform;
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
		const auto args = RawReadAddressArgs(*inst);
		for (size_t index = 0; index < args; index++) {
			Collect(inst->Arg(index), use_pc);
		}
		m_visiting.pop_back();
		m_visited.push_back(inst);
		if (!IsRawRead(m_program, *inst)) {
			return;
		}
		const auto offset = inst->Arg(RawReadOffsetArg(inst->GetOpcode())).Resolve();
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

// One pass of a readfirstlane's per-lane sweep. `dependent` stays clear for an operand that never
// asks for the lane, which is every shader that does not go through the mask model, so those pay
// one walk exactly as before.
struct LaneScope {
	uint32_t lane      = 0;
	bool     dependent = false;
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

private:
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
				default: return false;
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
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
			return false;
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
		if (m_barred.contains(&inst)) {
			return false;
		}
		// Loop-carried: assume the entry value and require every operand to reproduce it, which
		// makes it the value the phi holds on every iteration.
		std::vector<const Inst*> web;
		const auto entry = ResolveCyclicPhiEntry(m_program, Value(const_cast<Inst*>(&inst)), &web);
		uint64_t   candidate = 0;
		if (entry.IsEmpty() || !EvaluateWide(entry, candidate)) {
			return false;
		}
		// A separate walk, because this one's stack already holds the operands the phi was
		// reached through and re-entering them would read as a cycle.
		Evaluator trial(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
		                m_active_mask);
		// Assumptions made further up the stack stay in force for any phi reached from here, and
		// so does the lane the sweep is currently on.
		trial.m_assumed = m_assumed;
		trial.m_barred  = m_barred;
		trial.m_lane    = m_lane;
		for (const auto* member: web) {
			// The whole web holds the candidate on one iteration, so assume all of it at once.
			const auto assumed = trial.m_assumed.emplace(member, candidate);
			if (!assumed.second && assumed.first->second != candidate) {
				return false;
			}
		}
		trial.m_cache = trial.m_assumed;
		for (const auto* member: web) {
			for (size_t index = 0; index < member->NumArgs(); index++) {
				uint64_t carried = 0;
				if (!trial.EvaluateWide(member->Arg(index), carried) || carried != candidate) {
					return false;
				}
			}
		}
		result = candidate;
		return true;
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result) {
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return false;
		}
		const auto component = index.U32();
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU32x4) {
			return EvaluateExtractU32x4(inst, component, result);
		}
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

	bool EvaluateExtractU32x4(const Inst& inst, uint32_t component, uint64_t& result) {
		const auto* source = inst.Arg(0).ResolveInstruction();
		if (source == nullptr || component >= 4u) {
			return false;
		}
		if (source->GetOpcode() == ValueOpcode::Ballot) {
			// The ballot packs lanes 0-31 and 32-63 into one word; the upper two dwords are zero.
			uint64_t mask = 0;
			if (!Arg(inst, 0, mask)) {
				return false;
			}
			result = component < 2u ? static_cast<uint32_t>(mask >> (component * 32u)) : 0u;
			return true;
		}
		if (source->GetOpcode() == ValueOpcode::LoadBufferU32x4 &&
		    IsUniformBufferRead(m_program, *source)) {
			// A U32x4 does not fit the walk's 64-bit value, so read the one dword asked for.
			return EvaluateRawRead(*source, result, component * sizeof(uint32_t));
		}
		return false;
	}

	bool EvaluateRawRead(const Inst& inst, uint64_t& result, uint32_t component_bytes = 0u) {
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
		if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) ||
		    !Arg(inst, RawReadOffsetArg(inst.GetOpcode()), offset)) {
			return false;
		}
		const auto base = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
		const auto immediate =
		    static_cast<int64_t>(static_cast<int32_t>(mem.offset)) + component_bytes;
		uint64_t address = 0;
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer ||
		    inst.GetOpcode() == ValueOpcode::LoadBufferU32 ||
		    inst.GetOpcode() == ValueOpcode::LoadBufferU32x4) {
			uint64_t records = 0;
			uint64_t word3   = 0;
			if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
				return false;
			}
			if (immediate < 0) {
				return false;
			}
			const bool vector_load = inst.GetOpcode() != ValueOpcode::ReadConstBuffer;
			const auto stride      = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			// A vector load adds the thread ID to its index when V# asks, so no one value holds.
			if (vector_load && ((static_cast<uint32_t>(word3) >> 23u) & 1u) != 0u) {
				return false;
			}
			if (vector_load && stride != 0u && (static_cast<uint32_t>(high) >> 31u) != 0u) {
				const auto element = static_cast<uint64_t>(immediate);
				if (static_cast<uint32_t>(records) == 0u || element + sizeof(uint32_t) > stride) {
					return false;
				}
				const auto index_stride = uint64_t {8}
				                          << ((static_cast<uint32_t>(word3) >> 21u) & 3u);
				const auto byte_offset = (element & ~uint64_t {3}) * index_stride + (element & 3u) +
				                         static_cast<uint32_t>(offset);
				address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
			} else {
				const auto byte_offset =
				    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
				const auto aligned = byte_offset & ~uint64_t {3};
				const auto size =
				    stride == 0u ? static_cast<uint64_t>(static_cast<uint32_t>(records))
				                 : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
				// A descriptor chain that reads past its own descriptor is not trustworthy, so the
				// walk refuses rather than substituting hardware's zero. shader_cfg_tests asserts
				// this: "real S_BUFFER_LOAD walk ignored descriptor bounds".
				if (aligned > size || size - aligned < sizeof(uint32_t)) {
					return false;
				}
				address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
			}
		} else {
			const auto relative = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
			if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
				return false;
			}
		}
		uint32_t word = 0;
		if (m_runtime.read_memory != nullptr) {
			if (!m_runtime.read_memory(m_runtime.userdata, address, &word)) {
				return false;
			}
		} else {
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		result = word;
		return true;
	}

	// The executor half of the lockstep contract stated at IsRuntimeUniformOp: every opcode that
	// predicate accepts needs a case here, and every case here must be a total function of scalar
	// values with no lane identity, no host-incoherent memory and no shader rounding mode.
	bool EvaluateInst(const Inst& inst, uint64_t& result) {
		uint64_t   a       = 0;
		uint64_t   b       = 0;
		uint64_t   c       = 0;
		const auto binary  = [&]() { return Arg(inst, 0, a) && Arg(inst, 1, b); };
		const auto ternary = [&]() {
			return Arg(inst, 0, a) && Arg(inst, 1, b) && Arg(inst, 2, c);
		};
		const auto s32     = [](uint64_t value) {
			return std::bit_cast<int32_t>(static_cast<uint32_t>(value));
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
			case ValueOpcode::LaneId:
				// Only the sweep below binds a lane. Anywhere else the value is per-lane with
				// nothing to pin it down, and RuntimeValidator refused it for the same reason.
				if (m_lane == nullptr) {
					return false;
				}
				m_lane->dependent = true;
				result            = m_lane->lane;
				return true;
			case ValueOpcode::ReadFirstLane: {
				// A readfirstlane is how a vector value reaches the scalar registers a descriptor
				// is assembled from, so its operand is wave-uniform on the hardware that wrote it
				// and any lane index inside it has to cancel. Re-execute the operand for every
				// lane and take the answer they all give: lanes that disagree mean no single
				// descriptor stands for the wave, and a wrong descriptor is worse than a dropped
				// dispatch, so that refusal stands.
				const auto clean_runtime = CleanRuntime(m_runtime);
				LaneScope  scope;
				uint64_t   common = 0;
				for (scope.lane = 0; scope.lane < m_program.wave_size; scope.lane++) {
					scope.dependent = false;
					Evaluator clean_active(m_program, clean_runtime, {}, nullptr, inst.Arg(1));
					clean_active.m_lane = &scope;
					Evaluator active(m_program, m_runtime, m_clean_flat_slots, &clean_active,
					                 inst.Arg(1));
					active.m_lane = &scope;
					// A lane-0 walk resolves selects on its own mask; outer assumptions need not hold.
					active.m_barred = m_barred;
					for (const auto& assumed: m_assumed) {
						active.m_barred.insert(assumed.first);
					}
					uint64_t lane_value = 0;
					if (!active.EvaluateWide(inst.Arg(0), lane_value)) {
						return false;
					}
					if (scope.lane == 0) {
						common = lane_value;
					} else if (lane_value != common) {
						return false;
					}
					if (!scope.dependent) {
						break;
					}
				}
				result = common;
				return true;
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return Arg(inst, 0, result);
			case ValueOpcode::Ballot: {
				// One bit per lane of the predicate, each lane re-executed with its own index. A
				// predicate that never asks for the lane is the same on every lane, so one walk
				// fills the whole wave. The walk shares ReadFirstLane's model: all wave_size lanes.
				const auto lanes = m_program.wave_size;
				if (lanes == 0u || lanes > 64u) {
					return false;
				}
				const auto wave = lanes == 64u ? ~uint64_t {0} : (uint64_t {1} << lanes) - 1u;
				LaneScope  scope;
				uint64_t   mask = 0;
				for (scope.lane = 0; scope.lane < lanes; scope.lane++) {
					scope.dependent = false;
					Evaluator lane_walk(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
					                    Value(true));
					lane_walk.m_lane   = &scope;
					lane_walk.m_barred = m_barred;
					for (const auto& assumed: m_assumed) {
						lane_walk.m_barred.insert(assumed.first);
					}
					uint64_t taken = 0;
					if (!lane_walk.EvaluateWide(inst.Arg(0), taken)) {
						return false;
					}
					if (scope.lane == 0u && !scope.dependent) {
						mask = taken != 0u ? wave : 0u;
						break;
					}
					if (taken != 0u) {
						mask |= uint64_t {1} << scope.lane;
					}
				}
				result = mask;
				return true;
			}
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2:
			case ValueOpcode::CompositeExtractU32x4: return EvaluateExtract(inst, result);
			case ValueOpcode::CompositeConstructU64:
			case ValueOpcode::CompositeConstructU32x2:
				// A U32x2 packs into the same 64-bit word the extract cases read back.
				if (!binary()) {
					return false;
				}
				result = static_cast<uint32_t>(a) |
				         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
				return true;
			case ValueOpcode::IAddCarry32:
				// Low half the truncated sum, high half the carry out: the 64-bit sum is both, and
				// matches what EvaluateExtract computes when this feeds a CompositeExtractU32x2.
				if (!binary()) {
					return false;
				}
				result = static_cast<uint64_t>(static_cast<uint32_t>(a)) + static_cast<uint32_t>(b);
				return true;
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					return false;
				}
				// A flat slot is also walked on its own, outside any readfirstlane, so it must hold
				// without a lane bound. RuntimeValidator drops the active mask here for the same
				// reason; leaving the scope in place would accept a slot the separate walk cannot.
				auto* lane = m_lane;
				m_lane     = nullptr;
				const bool read =
				    slot.U32() < m_clean_flat_slots.size() &&
				            m_clean_flat_slots[slot.U32()] != 0u && m_clean_evaluator != nullptr
				        ? m_clean_evaluator->EvaluateWide(m_program.srt_reads[slot.U32()].value,
				                                          result)
				        : EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
				m_lane = lane;
				return read;
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
			case ValueOpcode::LoadBufferU32:
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
			case ValueOpcode::SMulHi:
				if (binary()) {
					const auto product = static_cast<int64_t>(s32(a)) * static_cast<int64_t>(s32(b));
					result             = static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32u);
					return true;
				}
				return false;
			case ValueOpcode::UMulHi:
				if (binary()) {
					const auto product = static_cast<uint64_t>(static_cast<uint32_t>(a)) *
					                     static_cast<uint64_t>(static_cast<uint32_t>(b));
					result = static_cast<uint32_t>(product >> 32u);
					return true;
				}
				return false;
			case ValueOpcode::SMin32:
				if (binary()) {
					result = static_cast<uint32_t>(std::min(s32(a), s32(b)));
					return true;
				}
				return false;
			case ValueOpcode::UMin32:
				if (binary()) {
					result = std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
					return true;
				}
				return false;
			case ValueOpcode::SMax32:
				if (binary()) {
					result = static_cast<uint32_t>(std::max(s32(a), s32(b)));
					return true;
				}
				return false;
			case ValueOpcode::UMax32:
				if (binary()) {
					result = std::max(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
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
			case ValueOpcode::FPRecipIFlag32:
				// The backend lowers this to an exact reciprocal, so match it here.
				if (Arg(inst, 0, a)) {
					result = Float32Bits(1.0f / Float32(a));
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
			case ValueOpcode::IEqual64:
				if (binary()) {
					result = a == b;
					return true;
				}
				return false;
			case ValueOpcode::INotEqual64:
				if (binary()) {
					result = a != b;
					return true;
				}
				return false;
			case ValueOpcode::SLessThan32:
				if (binary()) {
					result = s32(a) < s32(b);
					return true;
				}
				return false;
			case ValueOpcode::SLessThan64:
				if (binary()) {
					result = std::bit_cast<int64_t>(a) < std::bit_cast<int64_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::ULessThan64:
				if (binary()) {
					result = a < b;
					return true;
				}
				return false;
			case ValueOpcode::SLessThanEqual32:
				if (binary()) {
					result = s32(a) <= s32(b);
					return true;
				}
				return false;
			case ValueOpcode::ULessThanEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) <= static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::SGreaterThan32:
				if (binary()) {
					result = s32(a) > s32(b);
					return true;
				}
				return false;
			case ValueOpcode::UGreaterThan64:
				if (binary()) {
					result = a > b;
					return true;
				}
				return false;
			case ValueOpcode::SGreaterThanEqual32:
				if (binary()) {
					result = s32(a) >= s32(b);
					return true;
				}
				return false;
			case ValueOpcode::UGreaterThanEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) >= static_cast<uint32_t>(b);
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
		// Lockstep guard. Only the two scalar reads above fall out of the switch deliberately, and
		// neither is a runtime-uniform op, so reaching here with one means the compile-time gate
		// accepts an opcode this executor cannot run: the shader would pass planning and then die
		// at pipeline build with no opcode named. Compiled out of a final build.
		EXIT_IF(IsRuntimeUniformOp(inst.GetOpcode()));
		return false;
	}

	const ResourcePlan&                       m_program;
	const SrtRuntime&                         m_runtime;
	std::span<const uint8_t>                  m_clean_flat_slots;
	Evaluator*                                m_clean_evaluator = nullptr;
	Value                                     m_active_mask;
	// Non-null only inside a readfirstlane's per-lane sweep, which is the one place a lane index
	// has a value. Owned by the sweep, shared with any walk it starts.
	LaneScope*                                m_lane    = nullptr;
	std::unordered_map<const Inst*, uint64_t> m_cache;
	// Loop-carried phi values taken on trust, inherited by any trial this walk starts.
	std::unordered_map<const Inst*, uint64_t> m_assumed;
	std::unordered_set<const Inst*>           m_barred;
	std::vector<const Inst*>                  m_visiting;
	bool                                      m_reserved = false;
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
                                std::vector<uint8_t>&    active_sources) {
	if (!program.srt_plan_complete) {
		return false;
	}
	if (std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; }) &&
	    runtime.read_specialization_memory == nullptr) {
		return false;
	}
	const auto           clean_runtime = CleanRuntime(runtime);
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
			return false;
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		if (!evaluate_flat || active[source_index]) {
			for (uint32_t index = 0; index < source->dword_count; index++) {
				if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
					return false;
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
			if (read.flat_offset >= flattened.size() ||
			    !selected.Evaluate(read.value, flattened[read.flat_offset])) {
				return false;
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

std::string_view RuntimeValueRejectName(RuntimeValueReject reason) {
	switch (reason) {
		case RuntimeValueReject::None: return "no recorded reason";
		case RuntimeValueReject::UnsupportedOpcode: return "unsupported opcode";
		case RuntimeValueReject::UnsupportedOperand: return "unsupported operand";
		case RuntimeValueReject::FloatInIntegerChain: return "float value in an integer chain";
		case RuntimeValueReject::MalformedInstruction: return "malformed instruction";
		case RuntimeValueReject::UndefinedValue: return "undefined value";
		case RuntimeValueReject::CyclicValue:
			return "cyclic value the loop carries rather than holds";
		case RuntimeValueReject::CyclicValueNoEntry:
			return "loop-carried value no operand enters the phi web from outside";
		case RuntimeValueReject::CyclicValueMerge:
			return "loop-carried value whose two entry operands disagree";
		case RuntimeValueReject::NonScalarType: return "not a 32-bit scalar";
	}
	return "unknown reason";
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type,
                          RuntimeValueFailure* failure) {
	if (failure != nullptr) {
		*failure = {};
	}
	return RuntimeValidator(program, type, failure).Run(value);
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
	const auto clean = CleanRuntime(runtime);
	Evaluator  evaluator(program, clean);
	for (size_t i = 0; i < values.size(); ++i) {
		if (!evaluator.Evaluate(values[i], results[i])) {
			return false;
		}
	}
	return true;
}

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result) {
	std::vector<DescriptorValue> results;
	if (!EvaluateDescriptorSources(program, std::span {&source, 1}, runtime, results)) {
		return false;
	}
	result = results.front();
	return true;
}

bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results) {
	std::vector<uint32_t> ignored;
	std::vector<uint8_t>  active;
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {},
	                                  active);
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources) {
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true,
	                                  clean_flat_slots, active_sources);
}

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat) {
	std::vector<DescriptorValue> ignored;
	std::vector<uint8_t>         active;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {}, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
