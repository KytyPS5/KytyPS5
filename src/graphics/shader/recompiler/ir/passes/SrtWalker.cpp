#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

SrtRuntime SrtReadCapture::ObservedRuntime() {
	auto runtime = m_source;
	runtime.userdata = this;
	runtime.read_specialization_memory = m_source.read_specialization_memory != nullptr
	                                         ? ReadStrict : nullptr;
	runtime.read_memory = ReadOrdinary;
	return runtime;
}

bool SrtReadCapture::ReadStrict(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<SrtReadCapture*>(userdata);
	if (!capture.m_source.read_specialization_memory(capture.m_source.userdata, address, values)) {
		return false;
	}
	capture.m_ranges.emplace_back(address, values.size_bytes());
	return true;
}

bool SrtReadCapture::ReadOrdinary(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<SrtReadCapture*>(userdata);
	if (capture.m_source.read_memory != nullptr) {
		if (!capture.m_source.read_memory(capture.m_source.userdata, address, values)) return false;
	} else {
		std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
	}
	capture.m_ranges.emplace_back(address, values.size_bytes());
	return true;
}

SrtRuntime CleanRuntime(SrtRuntime runtime) {
	runtime.read_memory = runtime.read_specialization_memory != nullptr
	                          ? runtime.read_specialization_memory
	                          : +[](void*, uint64_t, std::span<uint32_t>) { return false; };
	return runtime;
}

namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

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

uint32_t DescriptorLoadDwords(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::LoadBufferU32x2: return 2u;
		case ValueOpcode::LoadBufferU32x3: return 3u;
		case ValueOpcode::LoadBufferU32x4: return 4u;
		default: return 0u;
	}
}

// A vector-path buffer load whose address carries no lane identity: no index, no
// per-lane offset, one plain dword. Hardware reaches it as base + soffset + imm, which
// is the same shape the scalar reads below already re-execute, so the host can
// reproduce it. The gate used to refuse every MUBUF load on its resource kind alone,
// which dropped whole shaders whose only sin was fetching one uniform word through the
// vector path - see the two decoupled-lookback scan passes in SILENT HILL 2.
// A DWORDX4 load of that shape reads four such words at once, which is how a whole V# stored
// in a buffer reaches the scalar registers; each component is then taken apart by a
// CompositeExtract.
bool IsUniformBufferRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	const auto multi = DescriptorLoadDwords(op);
	if ((op != ValueOpcode::LoadBufferU32 && multi == 0u) || inst.NumArgs() != 5) {
		return false;
	}
	const auto dwords = multi == 0u ? 1u : multi;
	const auto flags = inst.Flags<MemoryFlags>();
	if (flags.index >= values.memory_info.size()) {
		return false;
	}
	const auto& mem = values.memory_info[flags.index];
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

// The one read the walk lets name a record rather than only a byte offset. RDNA 2 ISA 8.1.5
// builds a buffer address out of three parts and spells the two this adds:
//
//   Index = (inst_idxen ? vgpr_index : 0) + (const_add_tid_enable ? thread_id[5:0] : 0)
//   Offset = (inst_offen ? vgpr_offset : 0) + inst_offset
//
// so an index and a per-lane offset are ordinary terms of the address, not lane identity: the
// only lane term is add_tid, which EvaluateRawRead already refuses outright. RESIDENT EVIL
// REQUIEM reaches its descriptor heaps that way and no other - across the 208 dumped RE9 pixel
// shaders, every one of the 61 BUFFER_LOAD_DWORDX4 instructions is `idxen=1, offen=0`, and not
// one is the unindexed shape the predicate above accepts. So the walk re-executes the two terms
// instead of refusing the read for carrying them, and ValidateArguments then holds them to the
// same wave-uniform standard as the descriptor words and the scalar offset beside them: a value
// no lane can move is a record the whole wave agrees on.
//
// Scoped to the DWORDX4 form on purpose. LoadBufferU32 is also a raw read (IsRawRead), and
// PlanBuilder hoists those into flat SRT slots on the strength of an immediate offset alone -
// one uploaded word standing for the whole dispatch. An index does not survive that; findings 35,
// 37 and 38 are all about a selector that differs per iteration being bound as if it did not. A
// DWORDX4 read is never flattened ("A uniform DWORDX4 load is deliberately not one"), so widening
// it cannot reach the planner.
bool IsDescriptorDwordX4Load(const ResourcePlan& values, const Inst& inst) {
	return DescriptorLoadDwords(inst.GetOpcode()) != 0u && IsUniformBufferRead(values, inst);
}

// Which argument carries the read's dynamic byte offset. The scalar reads put it
// second; a MUBUF load puts the index and the per-lane offset there and the scalar
// offset fourth.
size_t RawReadOffsetArg(ValueOpcode op) {
	return op == ValueOpcode::LoadBufferU32 || DescriptorLoadDwords(op) != 0u ? 3u : 1u;
}

// A MUBUF load carries the exec mask as its last operand. That is lane state, not part
// of the address, and walking it would reject the read for depending on lane identity.
size_t RawReadAddressArgs(const Inst& inst) {
	const auto op = inst.GetOpcode();
	return op == ValueOpcode::LoadBufferU32 || DescriptorLoadDwords(op) != 0u
	           ? 4u
	           : inst.NumArgs();
}

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

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ConditionRef:
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
		case ValueOpcode::UMin32:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::SMin32:
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
		case ValueOpcode::WqmU64:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::IEqual64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::ReadLane:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::BitCount64:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindUMsb64:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPLdexp:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32:
		case ValueOpcode::FPFract32:
		case ValueOpcode::FPSqrt:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipSqrt32:
		case ValueOpcode::FPExp2:
		case ValueOpcode::FPLog2:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPOrdEqual32:
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
		if (m_type != RuntimeValueType::Any &&
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
		if (m_type == RuntimeValueType::ImmutableInteger &&
		    (op == ValueOpcode::ReadConst || BufferAccessOf(op) != BufferAccess::None ||
		     AddressOpcodeInfoOf(op).access != AddressAccess::None ||
		     ImageOpcodeInfoOf(op).access != ImageAccess::None ||
		     SharedAccessOf(op) != SharedAccess::None)) {
			return finish(false);
		}
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			if (m_type != RuntimeValueType::Any) {
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
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(2), false)) {
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
			if (m_type != RuntimeValueType::Any && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (!invariant.IsEmpty()) {
				return finish(Validate(invariant));
			}
			// Accept the value the loop is entered with; the evaluator proves it is a fixpoint
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
		if (op == ValueOpcode::Ballot) {
			// The other way a vector value reaches the scalar registers: a VCMP into an SGPR, or
			// EXEC read as a scalar, collects one bit per lane. The predicate is re-executed for
			// every lane, so a lane index inside it is bound, and every lane counts - hence an
			// all-true mask rather than the enclosing readfirstlane mask.
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::U1) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = Value(true);
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::LaneId) {
			// A descriptor lives in scalar registers, so the only route a lane index has into one
			// is the readfirstlane that lifts a vector value back into them - in practice the
			// emulator's own model of "bit N of a scalar mask, for this lane", which hardware
			// never spells per-lane at all. There the lane term has to cancel, and the evaluator
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
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = inst->Arg(1).Resolve();
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::ReadLane) {
			// V_READLANE_B32 names the lane outright, so unlike readfirstlane there is nothing to
			// guess and no agreement to prove: bind that one lane and re-execute the operand. The
			// instruction ignores EXEC, hence the all-true mask rather than the enclosing one.
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U32) {
				return finish(Reject(RuntimeValueReject::MalformedInstruction, op));
			}
			if (!Validate(inst->Arg(1))) {
				return finish(false);
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
		} else if (DescriptorLoadDwords(op) != 0u) {
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsDescriptorDwordX4Load(m_program, *inst) || handle == nullptr ||
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
				// Four conditions share this reason; the one that fires is an unhandled producer,
				// and naming it is the fact triage needs.
				if (m_failure != nullptr && source != nullptr) {
					m_failure->entry_opcode      = source->GetOpcode();
					m_failure->other_opcode      = op;
					m_failure->has_entry_opcodes = true;
				}
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
			     source->GetOpcode() != ValueOpcode::IAddCarry32 &&
			     source->GetOpcode() != ValueOpcode::LoadBufferU32x2)) {
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
		           !(op == ValueOpcode::LoadBufferU32 && IsUniformBufferRead(m_program, *inst)) &&
		           !IsDescriptorDwordX4Load(m_program, *inst) && !IsRuntimeUniformOp(op)) {
			return finish(Reject(RuntimeValueReject::UnsupportedOpcode, op));
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	RuntimeValueFailure*            m_failure = nullptr;
	std::unordered_map<const Inst*, std::vector<Value>> m_validated_uniform;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};


} // namespace

SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     std::span<const uint8_t> clean_flat_slots, SrtWalker* clean_evaluator,
                     Value active_mask)
    : m_program(program), m_runtime(runtime), m_clean_flat_slots(clean_flat_slots),
      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()),
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
	// A value assumed by the fixpoint trial answers before the memo: the whole phi web has to
	// read as the candidate for the iteration under test.
	if (const auto assumed = m_assumed.find(inst); assumed != m_assumed.end()) {
		result = assumed->second;
		return true;
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
	if (!value.IsEmpty()) {
		return EvaluateWide(value, result);
	}
	const auto refuse_phi = [&](PhiReject reason) {
		if (m_phi_reject == PhiReject::None) {
			m_phi_reject = reason;
		}
		return false;
	};
	if (m_barred.contains(&inst)) {
		return refuse_phi(PhiReject::Barred);
	}
	// Loop-carried: assume the entry value and require every operand to reproduce it, which
	// makes it the value the phi holds on every iteration.
	std::vector<const Inst*> web;
	const auto entry = ResolveCyclicPhiEntry(m_program, Value(const_cast<Inst*>(&inst)), &web);
	uint64_t   candidate = 0;
	if (entry.IsEmpty()) {
		return refuse_phi(PhiReject::NoEntry);
	}
	if (!EvaluateWide(entry, candidate)) {
		return refuse_phi(PhiReject::EntryUnevaluable);
	}
	// A separate walk, because this one's memo already holds the operands the phi was reached
	// through and re-entering them would read as a cycle.
	SrtWalker trial(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator, m_active_mask);
	// Assumptions made further up the stack stay in force for any phi reached from here.
	// Assumptions made further up the stack stay in force for any phi reached from here, and
	// so does the lane the sweep is currently on.
	trial.m_assumed = m_assumed;
	trial.m_barred  = m_barred;
	trial.m_lane    = m_lane;
	for (const auto* member: web) {
		// The whole web holds the candidate on one iteration, so assume all of it at once.
		const auto assumed = trial.m_assumed.emplace(member, candidate);
		if (!assumed.second && assumed.first->second != candidate) {
			return refuse_phi(PhiReject::WebAssumptionConflict);
		}
	}
	for (const auto* member: web) {
		for (size_t index = 0; index < member->NumArgs(); index++) {
			uint64_t carried = 0;
			if (!trial.EvaluateWide(member->Arg(index), carried) || carried != candidate) {
				return refuse_phi(PhiReject::OperandVariesPerIteration);
			}
		}
	}
	result = candidate;
	return true;
}

bool SrtWalker::EvaluateExtract(const Inst& inst, uint64_t& result) {
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
	if (source->GetOpcode() == ValueOpcode::LoadBufferU32x2 &&
	    IsDescriptorDwordX4Load(m_program, *source)) {
		return EvaluateRawRead(*source, result, component * sizeof(uint32_t));
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

bool SrtWalker::EvaluateExtractU32x4(const Inst& inst, uint32_t component,
                                     uint64_t& result) {
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
		// A U32x4 does not fit the walk 64-bit value, so read the one dword asked for.
		return EvaluateRawRead(*source, result, component * sizeof(uint32_t));
	}
	return false;
}

bool SrtWalker::EvaluateRawRead(const Inst& inst, uint64_t& result,
                               uint32_t component_bytes) {
	const auto RefuseRawRead = [&](RawReadReject reason) {
		if (m_raw_read_reject == RawReadReject::None) {
			m_raw_read_reject = reason;
		}
		return false;
	};
	const auto flags = inst.Flags<MemoryFlags>();
	if (flags.index >= m_program.memory_info.size()) {
		return RefuseRawRead(RawReadReject::MemoryIndexOutOfRange);
	}
	const auto& mem    = m_program.memory_info[flags.index];
	const bool  vector = inst.GetOpcode() == ValueOpcode::LoadBufferU32 ||
	                    DescriptorLoadDwords(inst.GetOpcode()) != 0u;
	// A vector load under a literal false EXEC touches no memory and leaves zero.
	if (const auto guard = inst.Arg(inst.NumArgs() - 1u).Resolve();
	    vector && guard.IsImmediate() && guard.GetType() == Type::U1 && !guard.U1()) {
		result = 0u;
		return true;
	}
	const auto* handle = inst.Arg(0).ResolveInstruction();
	if (handle == nullptr) {
		return RefuseRawRead(RawReadReject::NoHandle);
	}
	uint64_t   low        = 0;
	uint64_t   high       = 0;
	uint64_t   offset     = 0;
	const auto offset_arg = RawReadOffsetArg(inst.GetOpcode());
	const auto refuse_operand = [&](const char* name, Value value) {
		if (m_raw_read_reject == RawReadReject::None) {
			m_raw_read_operand = name;
			ValidateRuntimeValue(m_program, value, RuntimeValueType::Any,
			                     &m_raw_read_operand_failure);
		}
		return RefuseRawRead(RawReadReject::HandleOperandUnavailable);
	};
	if (!Arg(*handle, 0, low)) {
		return refuse_operand("base low", handle->Arg(0));
	}
	if (!Arg(*handle, 1, high)) {
		return refuse_operand("base high", handle->Arg(1));
	}
	if (!Arg(inst, offset_arg, offset)) {
		return refuse_operand("offset", inst.Arg(offset_arg));
	}
	const auto base = (high << 32u) | static_cast<uint32_t>(low);
	const auto immediate =
	    static_cast<int64_t>(static_cast<int32_t>(mem.offset)) + component_bytes;
	uint64_t address = 0;
	if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer ||
	    inst.GetOpcode() == ValueOpcode::LoadBufferU32 ||
	    DescriptorLoadDwords(inst.GetOpcode()) != 0u) {
		uint64_t records = 0;
		uint64_t word3   = 0;
		if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
			return RefuseRawRead(RawReadReject::DescriptorOperandUnavailable);
		}
		if (immediate < 0) {
			return RefuseRawRead(RawReadReject::NegativeImmediate);
		}
		const bool vector_load = inst.GetOpcode() != ValueOpcode::ReadConstBuffer;
		const auto stride      = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
		// A vector load adds the thread ID to its index when V# asks, so no one value holds.
		if (vector_load && ((static_cast<uint32_t>(word3) >> 23u) & 1u) != 0u) {
			return RefuseRawRead(RawReadReject::AddTidIndexing);
		}
		if (vector_load && stride != 0u && (static_cast<uint32_t>(high) >> 31u) != 0u) {
			const auto element = static_cast<uint64_t>(immediate);
			if (static_cast<uint32_t>(records) == 0u || element + sizeof(uint32_t) > stride) {
				return RefuseRawRead(RawReadReject::SwizzledElementOutOfStride);
			}
			const auto index_stride = uint64_t {8}
			                          << ((static_cast<uint32_t>(word3) >> 21u) & 3u);
			const auto byte_offset = (element & ~uint64_t {3}) * index_stride + (element & 3u) +
			                         static_cast<uint32_t>(offset);
			address = ((base & AddressMask & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto byte_offset = (static_cast<uint64_t>(immediate) & ~uint64_t {3}) +
			                         (static_cast<uint32_t>(offset) & ~3u);
			const auto size =
			    stride == 0u ? static_cast<uint64_t>(static_cast<uint32_t>(records))
			                 : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
			// A descriptor chain that reads past its own descriptor is not trustworthy, so the
			// walk refuses rather than substituting hardware's zero. shader_cfg_tests asserts
			// this: "real S_BUFFER_LOAD walk ignored descriptor bounds".
			if (byte_offset > size || size - byte_offset < sizeof(uint32_t)) {
				return RefuseRawRead(RawReadReject::OutsideDescriptorBounds);
			}
			address = (base & ~uint64_t {3}) + byte_offset;
		}
	} else {
		const auto relative = (immediate & ~int64_t {3}) +
		                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
		if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
			return RefuseRawRead(RawReadReject::AddressOverflow);
		}
	}
	uint32_t word = 0;
	if (m_runtime.read_memory != nullptr) {
		if (!m_runtime.read_memory(m_runtime.userdata, address, {&word, 1})) {
			m_refused_read     = address;
			m_has_refused_read = true;
			return RefuseRawRead(RawReadReject::ReadRefused);
		}
	} else {
		std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
	}
	result = word;
	return true;
}

bool SrtWalker::EvaluateInst(const Inst& inst, uint64_t& result) {
	if (EvaluateInstRule(inst, result)) {
		return true;
	}
	if (!m_has_first_refusal) {
		m_first_refusal     = inst.GetOpcode();
		m_has_first_refusal = true;
	}
	return false;
}

bool SrtWalker::EvaluateInstRule(const Inst& inst, uint64_t& result) {
	uint64_t   a       = 0;
	uint64_t   b       = 0;
	uint64_t   c       = 0;
	const auto binary  = [&]() { return Arg(inst, 0, a) && Arg(inst, 1, b); };
	const auto ternary = [&]() {
		return Arg(inst, 0, a) && Arg(inst, 1, b) && Arg(inst, 2, c);
	};
	const auto s32 = [](uint64_t value) {
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
				SrtWalker clean_active(m_program, clean_runtime, {}, nullptr, inst.Arg(1));
				SrtWalker active(m_program, m_runtime, m_clean_flat_slots, &clean_active,
				                 inst.Arg(1));
				active.m_lane = &scope;
				// A select resolves its predicate through the clean evaluator, so the lane has to be
				// the same there: it is a property of the walk, not of the memory being read.
				clean_active.m_lane = &scope;
				// A lane walk resolves selects on its own mask; outer assumptions need not hold.
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
				// An operand that never asked for the lane gives the same answer for all of
				// them, so one walk settles it.
				if (!scope.dependent) {
					break;
				}
			}
			result = common;
			return true;
		}
		case ValueOpcode::ReadLane: {
			// The lane is an operand, so one walk with that lane bound settles it. No sweep and no
			// agreement test: readfirstlane has to guess which lane is first active, this does not.
			uint64_t lane = 0;
			if (!EvaluateWide(inst.Arg(1), lane) || lane >= m_program.wave_size) {
				return false;
			}
			LaneScope scope;
			scope.lane      = static_cast<uint32_t>(lane);
			scope.dependent = false;
			SrtWalker lane_walk(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
			                    Value(true));
			lane_walk.m_lane   = &scope;
			lane_walk.m_barred = m_barred;
			for (const auto& assumed: m_assumed) {
				lane_walk.m_barred.insert(assumed.first);
			}
			return lane_walk.EvaluateWide(inst.Arg(0), result);
		}
		case ValueOpcode::BitReverse32:
			if (Arg(inst, 0, a)) {
				auto value = static_cast<uint32_t>(a);
				value      = ((value & 0x55555555u) << 1u) | ((value >> 1u) & 0x55555555u);
				value      = ((value & 0x33333333u) << 2u) | ((value >> 2u) & 0x33333333u);
				value      = ((value & 0x0f0f0f0fu) << 4u) | ((value >> 4u) & 0x0f0f0f0fu);
				value      = ((value & 0x00ff00ffu) << 8u) | ((value >> 8u) & 0x00ff00ffu);
				result     = (value << 16u) | (value >> 16u);
				return true;
			}
			return false;
		case ValueOpcode::BitCount32:
			if (Arg(inst, 0, a)) {
				result = static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(a)));
				return true;
			}
			return false;
		case ValueOpcode::BitCount64:
			if (Arg(inst, 0, a)) {
				result = static_cast<uint32_t>(std::popcount(a));
				return true;
			}
			return false;
		case ValueOpcode::FindUMsb32:
			if (Arg(inst, 0, a)) {
				const auto value = static_cast<uint32_t>(a);
				result = value == 0u ? UINT32_MAX : static_cast<uint32_t>(31 - std::countl_zero(value));
				return true;
			}
			return false;
		case ValueOpcode::FindUMsb64:
			if (Arg(inst, 0, a)) {
				result = a == 0u ? UINT32_MAX : static_cast<uint32_t>(63 - std::countl_zero(a));
				return true;
			}
			return false;
		case ValueOpcode::FindILsb32:
			if (Arg(inst, 0, a)) {
				const auto value = static_cast<uint32_t>(a);
				result = value == 0u ? UINT32_MAX : static_cast<uint32_t>(std::countr_zero(value));
				return true;
			}
			return false;
		case ValueOpcode::FPAbs32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::fabs(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPNeg32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(-Float32(a)));
				return true;
			}
			return false;
		case ValueOpcode::FPSaturate32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::clamp(Float32(a), 0.0F, 1.0F)));
				return true;
			}
			return false;
		case ValueOpcode::FPRoundEven32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::nearbyint(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPFloor32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::floor(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPCeil32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::ceil(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPFract32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(Float32(a) - std::floor(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPSqrt:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::sqrt(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPRecip32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(1.0F / Float32(a)));
				return true;
			}
			return false;
		case ValueOpcode::FPRecipSqrt32:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(1.0F / std::sqrt(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPExp2:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::exp2(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPLog2:
			if (Arg(inst, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::log2(Float32(a))));
				return true;
			}
			return false;
		case ValueOpcode::FPFma32:
			if (ternary()) {
				result = std::bit_cast<uint32_t>(Float32(a) * Float32(b) + Float32(c));
				return true;
			}
			return false;
		case ValueOpcode::FPMad32:
			if (ternary()) {
				result = std::bit_cast<uint32_t>(Float32(a) * Float32(b) + Float32(c));
				return true;
			}
			return false;
		case ValueOpcode::FPMinTri32:
			if (ternary()) {
				result = std::bit_cast<uint32_t>(
				    std::fmin(std::fmin(Float32(a), Float32(b)), Float32(c)));
				return true;
			}
			return false;
		case ValueOpcode::FPMaxTri32:
			if (ternary()) {
				result = std::bit_cast<uint32_t>(
				    std::fmax(std::fmax(Float32(a), Float32(b)), Float32(c)));
				return true;
			}
			return false;
		case ValueOpcode::FPMedTri32:
			if (ternary()) {
				const auto lo = std::fmin(std::fmin(Float32(a), Float32(b)), Float32(c));
				const auto hi = std::fmax(std::fmax(Float32(a), Float32(b)), Float32(c));
				result = std::bit_cast<uint32_t>(Float32(a) + Float32(b) + Float32(c) - lo - hi);
				return true;
			}
			return false;
		case ValueOpcode::FPLdexp:
			if (binary()) {
				result = std::bit_cast<uint32_t>(
				    std::ldexp(Float32(a), static_cast<int32_t>(static_cast<uint32_t>(b))));
				return true;
			}
			return false;
		case ValueOpcode::FPAdd32:
			if (binary()) {
				result = std::bit_cast<uint32_t>(static_cast<float>(Float32(a) + Float32(b)));
				return true;
			}
			return false;
		case ValueOpcode::FPSub32:
			if (binary()) {
				result = std::bit_cast<uint32_t>(static_cast<float>(Float32(a) - Float32(b)));
				return true;
			}
			return false;
		case ValueOpcode::FPMin32:
			if (binary()) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::fmin(Float32(a), Float32(b))));
				return true;
			}
			return false;
		case ValueOpcode::FPMax32:
			if (binary()) {
				result = std::bit_cast<uint32_t>(static_cast<float>(std::fmax(Float32(a), Float32(b))));
				return true;
			}
			return false;
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32: return Arg(inst, 0, result);
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x4: return EvaluateExtract(inst, result);
		case ValueOpcode::CompositeConstructU64:
		// A U32x2 packs into the same 64-bit word the extract cases read back.
		case ValueOpcode::CompositeConstructU32x2:
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
		case ValueOpcode::Ballot: {
			// One bit per lane of the predicate, each lane re-executed with its own index. A
			// predicate that never asks for the lane is the same on every lane, so one walk
			// fills the whole wave. Same model as the readfirstlane sweep: all wave_size lanes.
			const auto lanes = m_program.wave_size;
			if (lanes == 0u || lanes > 64u) {
				return false;
			}
			const auto wave = lanes == 64u ? ~uint64_t {0} : (uint64_t {1} << lanes) - 1u;
			LaneScope  scope;
			uint64_t   mask = 0;
			for (scope.lane = 0; scope.lane < lanes; scope.lane++) {
				scope.dependent = false;
				SrtWalker lane_walk(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
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
		case ValueOpcode::LaneId:
			// Only the sweep above binds a lane. Anywhere else the value is per-lane with
			// nothing to pin it down, and RuntimeValidator refused it for the same reason.
			if (m_lane == nullptr) {
				return false;
			}
			m_lane->dependent = true;
			result            = m_lane->lane;
			return true;
		case ValueOpcode::LoadBufferU32:
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
		case ValueOpcode::FPOrdEqual32:
			if (binary()) {
				result = Float32(a) == Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPOrdNotEqual32:
			if (binary()) {
				result = !std::isnan(Float32(a)) && !std::isnan(Float32(b)) &&
				         Float32(a) != Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPOrdLessThan32:
			if (binary()) {
				result = Float32(a) < Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPOrdGreaterThan32:
			if (binary()) {
				result = Float32(a) > Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
			if (binary()) {
				const auto operand = [&](uint64_t bits) {
					if (inst.Flags<FPCompareFlags>().flush_input_denorms &&
					    (bits & 0x7fffffffu) < 0x00800000u) {
						bits &= 0x80000000u;
					}
					return Float32(bits);
				};
				result = inst.GetOpcode() == ValueOpcode::FPOrdLessThanEqual32
				             ? operand(a) <= operand(b)
				             : operand(a) >= operand(b);
				return true;
			}
			return false;
		case ValueOpcode::FPUnordEqual32:
			if (binary()) {
				result = std::isnan(Float32(a)) || std::isnan(Float32(b)) ||
				         Float32(a) == Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPUnordNotEqual32:
			if (binary()) {
				result = Float32(a) != Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPUnordLessThan32:
			if (binary()) {
				result = std::isnan(Float32(a)) || std::isnan(Float32(b)) ||
				         Float32(a) < Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPUnordGreaterThan32:
			if (binary()) {
				result = std::isnan(Float32(a)) || std::isnan(Float32(b)) ||
				         Float32(a) > Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPUnordLessThanEqual32:
			if (binary()) {
				result = std::isnan(Float32(a)) || std::isnan(Float32(b)) ||
				         Float32(a) <= Float32(b);
				return true;
			}
			return false;
		case ValueOpcode::FPUnordGreaterThanEqual32:
			if (binary()) {
				result = std::isnan(Float32(a)) || std::isnan(Float32(b)) ||
				         Float32(a) >= Float32(b);
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
		case ValueOpcode::WqmU64:
			// S_WQM_B64, RDNA 2 SOP1 opcode 10: "for i in 0 ... opcode_size_in_bits - 1 do
			// D[i] = (S0[(i & ~3):(i | 3)] != 0)" - every group of four bits becomes all ones
			// if any of them is set. A total function of one scalar word, with no lane
			// identity of its own: the quad grouping is fixed by bit position, not by which
			// lane is asking. So the walk re-executes it exactly rather than refusing it, and
			// exactly is what it takes - a mask with one lane live per quad is wave-wide only
			// after the expansion, which is how RESIDENT EVIL REQUIEM pixel shader
			// 0xaba519e634a11781 builds an image descriptor behind s_wqm_b64.
			if (Arg(inst, 0, a)) {
				const auto quads = [](uint32_t word) {
					auto bits = word | (word >> 1u);
					bits |= bits >> 2u;
					return static_cast<uint32_t>((bits & 0x11111111u) * 0x0fu);
				};
				result = quads(static_cast<uint32_t>(a)) |
				         (static_cast<uint64_t>(quads(static_cast<uint32_t>(a >> 32u))) << 32u);
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
				result = s32(a) >= s32(b);
				return true;
			}
			return false;
		case ValueOpcode::IAddCarry32:
			// Low half the truncated sum, high half the carry out: the 64-bit sum is both, and
			// matches what EvaluateExtract computes when this feeds a CompositeExtractU32x2.
			if (!binary()) {
				return false;
			}
			result = static_cast<uint64_t>(static_cast<uint32_t>(a)) + static_cast<uint32_t>(b);
			return true;
		case ValueOpcode::SMulHi:
			if (binary()) {
				const auto product = static_cast<int64_t>(s32(a)) * static_cast<int64_t>(s32(b));
				result = static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32u);
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
		case ValueOpcode::FPRecipIFlag32:
			if (Arg(inst, 0, a)) {
				const auto exponent = (a >> 23u) & 0xffu;
				// Only normal positive powers of two invert exactly under the backend's division.
				if ((a & 0x807fffffu) != 0u || exponent == 0u || exponent >= 254u) return false;
				result = (254u - exponent) << 23u;
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
		case ValueOpcode::UGreaterThanEqual32:
			if (binary()) {
				result = static_cast<uint32_t>(a) >= static_cast<uint32_t>(b);
				return true;
			}
			return false;
		case ValueOpcode::LogicalAnd: {
			const bool left = Arg(inst, 0, a);
			if (left && a == 0u) {
				result = 0u;
				return true;
			}
			if (!Arg(inst, 1, b) || (b != 0u && !left)) return false;
			result = b != 0u;
			return true;
		}
		case ValueOpcode::LogicalOr: {
			const bool left = Arg(inst, 0, a);
			if (left && a != 0u) {
				result = 1u;
				return true;
			}
			if (!Arg(inst, 1, b) || (b == 0u && !left)) return false;
			result = b != 0u;
			return true;
		}
		case ValueOpcode::LogicalXor:
			if (binary()) {
				result = (a != 0u) != (b != 0u);
				return true;
			}
			return false;
		case ValueOpcode::ConditionRef: return Arg(inst, 0, result);
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
	// Lockstep guard. Reaching here with a runtime-uniform opcode means the compile-time
	// gate accepts one this executor cannot run: the shader would pass planning and then
	// die at pipeline build with no opcode named. Compiled out of a final build.
	EXIT_IF(IsRuntimeUniformOp(inst.GetOpcode()));
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

bool SrtWalker::RefreshFlatBuffer(std::vector<uint32_t>& flat, FlatRefreshFailure* failure,
                                  bool prune) {
	const auto refuse = [&](FlatRefreshFailure::Stage stage, uint32_t flat_offset) {
		if (failure != nullptr) {
			failure->stage       = stage;
			failure->flat_offset = flat_offset;
		}
		return false;
	};
	if (failure != nullptr) {
		*failure = {};
	}
	m_has_refused_read = false;
	m_raw_read_reject  = RawReadReject::None;
	m_raw_read_operand  = nullptr;
	m_has_first_refusal = false;
	m_phi_reject        = PhiReject::None;
	if (m_clean_evaluator != nullptr) {
		m_clean_evaluator->m_has_refused_read = false;
		m_clean_evaluator->m_raw_read_reject  = RawReadReject::None;
		m_clean_evaluator->m_raw_read_operand  = nullptr;
		m_clean_evaluator->m_has_first_refusal = false;
		m_clean_evaluator->m_phi_reject        = PhiReject::None;
	}
	if (!m_program.srt_plan_complete) {
		return refuse(FlatRefreshFailure::Stage::PlanIncomplete, 0);
	}
	const auto refresh = [&](uint32_t slot) {
		if (slot >= m_program.srt_reads.size()) {
			return refuse(FlatRefreshFailure::Stage::OffsetOutOfRange, slot);
		}
		const auto& read  = m_program.srt_reads[slot];
		const bool  clean = read.flat_offset < m_clean_flat_slots.size() &&
		                   m_clean_flat_slots[read.flat_offset] != 0u;
		if (clean && (m_clean_evaluator == nullptr || m_runtime.read_specialization_memory == nullptr)) {
			return refuse(FlatRefreshFailure::Stage::CleanSlotUnreadable, read.flat_offset);
		}
		auto& evaluator = clean ? *m_clean_evaluator : *this;
		if (read.flat_offset >= flat.size()) {
			return refuse(FlatRefreshFailure::Stage::OffsetOutOfRange, read.flat_offset);
		}
		if (!evaluator.Evaluate(read.value, flat[read.flat_offset])) {
			if (failure != nullptr) {
				failure->value_is_expressible = ValidateRuntimeValue(
				    m_program, read.value, RuntimeValueType::Any, &failure->value);
				failure->raw_read         = evaluator.m_raw_read_reject;
				failure->raw_read_operand = evaluator.m_raw_read_operand;
				failure->raw_read_operand_failure = evaluator.m_raw_read_operand_failure;
				failure->first_refusal            = evaluator.m_first_refusal;
				failure->has_first_refusal        = evaluator.m_has_first_refusal;
				failure->phi                      = evaluator.m_phi_reject;
				if (evaluator.m_has_refused_read) {
					failure->read_address     = evaluator.m_refused_read;
					failure->has_read_address = true;
					if (m_runtime.describe_read_refusal != nullptr) {
						failure->read_refusal = m_runtime.describe_read_refusal(
						    m_runtime.userdata, evaluator.m_refused_read);
					}
				}
			}
			return refuse(FlatRefreshFailure::Stage::ValueUnevaluable, read.flat_offset);
		}
		return true;
	};
	auto& active = m_program.active_sources;
	if (m_program.control_flow.empty()) {
		active.clear();
		flat.resize(m_program.srt_reads.size());
		for (uint32_t slot = 0; slot < m_program.srt_reads.size(); ++slot) {
			if (!refresh(slot)) return false;
		}
		return true;
	}
	flat.assign(m_program.srt_reads.size(), 0u);
	active.assign(m_program.descriptor_sources.size(), 1u);
	for (const auto& block: m_program.control_flow) {
		for (const auto source: block.sources) active.at(source) = 0u;
	}
	auto& visited = m_program.visited_blocks;
	auto& pending = m_program.pending_blocks;
	visited.assign(m_program.control_flow.size(), 0u);
	pending.clear();
	pending.push_back(0u);
	while (!pending.empty()) {
		const auto index = pending.back();
		pending.pop_back();
		if (visited.at(index)) continue;
		visited[index] = 1u;
		const auto& block = m_program.control_flow[index];
		for (const auto source: block.sources) active[source] = 1u;
		for (const auto slot: block.srt_reads) {
			if (!refresh(slot)) return false;
		}
		uint32_t condition = 0;
		auto& predicate = m_clean_evaluator != nullptr ? *m_clean_evaluator : *this;
		if (prune && !block.condition.IsEmpty() &&
		    (m_runtime.read_specialization_memory != nullptr ||
		     !m_program.capture_specialization_reads) &&
		    predicate.Evaluate(block.condition, condition)) {
			pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
		} else {
			pending.insert(pending.end(), block.successors.begin(), block.successors.end());
		}
	}
	return true;
}

std::string_view PhiRejectName(PhiReject reason) {
	switch (reason) {
		case PhiReject::None: return "no recorded reason";
		case PhiReject::Barred: return "the phi is barred by an enclosing trial";
		case PhiReject::NoEntry: return "no operand enters the web from outside the loop";
		case PhiReject::EntryUnevaluable: return "the entry value could not be evaluated";
		case PhiReject::WebAssumptionConflict: return "the web already assumed a different value";
		case PhiReject::OperandVariesPerIteration:
			return "an operand does not reproduce the entry value, so it varies per iteration";
	}
	return "unknown reason";
}

std::string_view RawReadRejectName(RawReadReject reason) {
	switch (reason) {
		case RawReadReject::None: return "no recorded reason";
		case RawReadReject::MemoryIndexOutOfRange: return "the memory index is out of range";
		case RawReadReject::NoHandle: return "the read has no descriptor handle";
		case RawReadReject::HandleOperandUnavailable:
			return "a handle address operand could not be evaluated";
		case RawReadReject::DescriptorOperandUnavailable:
			return "the descriptor num_records or word3 could not be evaluated";
		case RawReadReject::NegativeImmediate: return "the immediate offset is negative";
		case RawReadReject::AddTidIndexing: return "the descriptor adds the thread id to its index";
		case RawReadReject::SwizzledElementOutOfStride:
			return "the swizzled element lies outside the descriptor stride";
		case RawReadReject::OutsideDescriptorBounds:
			return "the read lies outside the descriptor bounds";
		case RawReadReject::AddressOverflow: return "the address computation overflowed";
		case RawReadReject::ReadRefused: return "the guest read refused";
	}
	return "unknown reason";
}

std::string DescribeRuntimeFailure(const RuntimeValueFailure& failure) {
	if (failure.has_entry_opcodes) {
		const auto operand = [](ValueOpcode opcode) {
			return opcode == ValueOpcode::Void ? std::string_view("immediate")
			                                   : ValueOpcodeName(opcode);
		};
		return fmt::format("{} {}, entries {} and {}", RuntimeValueRejectName(failure.reason),
		                   ValueOpcodeName(failure.opcode), operand(failure.entry_opcode),
		                   operand(failure.other_opcode));
	}
	if (failure.has_opcode) {
		return fmt::format("{} {}", RuntimeValueRejectName(failure.reason),
		                   ValueOpcodeName(failure.opcode));
	}
	return std::string(RuntimeValueRejectName(failure.reason));
}

std::string DescribeFlatRefreshFailure(const FlatRefreshFailure& failure) {
	switch (failure.stage) {
		case FlatRefreshFailure::Stage::None: return "no recorded reason";
		case FlatRefreshFailure::Stage::PlanIncomplete: return "the SRT plan is incomplete";
		case FlatRefreshFailure::Stage::CleanSlotUnreadable:
			return fmt::format("slot {} is clean but has no strict reader", failure.flat_offset);
		case FlatRefreshFailure::Stage::OffsetOutOfRange:
			return fmt::format("slot {} is past the flat buffer", failure.flat_offset);
		case FlatRefreshFailure::Stage::ValueUnevaluable:
			// An expressible value that still refuses did not stop on an opcode: the guest read
			// behind it is what failed, which is a memory problem and not a recompiler gap.
			if (failure.has_read_address) {
				return fmt::format("slot {} could not read 0x{:012x}: {}", failure.flat_offset,
				                   failure.read_address,
				                   failure.read_refusal != nullptr ? failure.read_refusal
				                                                   : "no describer");
			}
			if (failure.raw_read != RawReadReject::None) {
				if (failure.raw_read_operand != nullptr) {
					return fmt::format(
					    "slot {} raw read refused: {} ({}: {}, deepest refusal {})",
					    failure.flat_offset, RawReadRejectName(failure.raw_read),
					    failure.raw_read_operand,
					    DescribeRuntimeFailure(failure.raw_read_operand_failure),
					    failure.has_first_refusal
					        ? (failure.phi != PhiReject::None
					               ? fmt::format("{} ({})", ValueOpcodeName(failure.first_refusal),
					                             PhiRejectName(failure.phi))
					               : std::string(ValueOpcodeName(failure.first_refusal)))
					        : std::string("none"));
				}
				return fmt::format("slot {} raw read refused: {}", failure.flat_offset,
				                   RawReadRejectName(failure.raw_read));
			}
			return failure.value_is_expressible
			           ? fmt::format("slot {} refused at evaluation with no recorded raw read",
			                         failure.flat_offset)
			           : fmt::format("slot {} {}", failure.flat_offset,
			                         DescribeRuntimeFailure(failure.value));
	}
	return "unknown reason";
}

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


} // namespace Libs::Graphics::ShaderRecompiler::IR
