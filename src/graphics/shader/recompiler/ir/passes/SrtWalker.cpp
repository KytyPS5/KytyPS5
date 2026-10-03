#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/abi.h"
#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <xbyak/xbyak.h>

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

thread_local uint32_t            g_srt_read_slot  = UINT32_MAX;
thread_local const RawReadEvent* g_raw_read_event = nullptr;

class SrtReadSlotScope {
public:
	explicit SrtReadSlotScope(uint32_t slot): m_saved(g_srt_read_slot) { g_srt_read_slot = slot; }
	~SrtReadSlotScope() { g_srt_read_slot = m_saved; }
	SrtReadSlotScope(const SrtReadSlotScope&)            = delete;
	SrtReadSlotScope& operator=(const SrtReadSlotScope&) = delete;

private:
	uint32_t m_saved;
};

std::string_view SrtRefusalName(SrtRefusal reason) {
	switch (reason) {
		case SrtRefusal::None: return "no recorded reason";
		case SrtRefusal::UnsupportedOpcode: return "unsupported opcode";
		case SrtRefusal::UnsupportedOperand: return "unsupported operand";
		case SrtRefusal::UndefinedValue: return "undefined value";
		case SrtRefusal::VaryingPhi: return "loop-carried value varies per iteration";
		case SrtRefusal::PhiNoEntry: return "loop-carried value has no entry value";
		case SrtRefusal::PhiMerge: return "loop-carried value has disagreeing entry values";
		case SrtRefusal::ReadOutOfBounds: return "read address out of range";
		case SrtRefusal::ReadRefused: return "guest read refused";
		case SrtRefusal::GpuOwned: return "GPU-owned data";
		case SrtRefusal::VaryingLane: return "value varies per lane";
	}
	return "unknown reason";
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

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer &&
	    op != ValueOpcode::LoadBufferU32) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto& memory = values.memory_info[index];
	if (op != ValueOpcode::LoadBufferU32) {
		return (op == ValueOpcode::LoadAddressU32 && memory.kind == ResourceKind::ScalarAddress) ||
		       (op == ValueOpcode::ReadConstBuffer && memory.kind == ResourceKind::ScalarBuffer);
	}
	if (inst.NumArgs() != 5u || memory.kind != ResourceKind::Buffer || memory.typed ||
	    memory.formatted || memory.coherent || memory.data_bits != 32u ||
	    memory.data_dwords != 1u || memory.idxen || memory.offen || memory.offset != 0u ||
	    inst.Arg(4).GetType() != Type::U1) {
		return false;
	}
	for (uint32_t arg = 1; arg <= 3; ++arg) {
		if (inst.Arg(arg).Resolve() != Value(0u)) return false;
	}
	return true;
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

uint32_t DescriptorLoadDwords(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::LoadBufferU32x2: return 2u;
		case ValueOpcode::LoadBufferU32x4: return 4u;
		default: return 0u;
	}
}

// A V# held in a buffer, read by one uniform DWORDX2/X4 load with no index or per-lane offset.
bool IsDescriptorLoad(const ResourcePlan& values, const Inst& inst) {
	const auto dwords = DescriptorLoadDwords(inst.GetOpcode());
	if (dwords == 0u || inst.NumArgs() != 5u || inst.Arg(4).GetType() != Type::U1) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto& memory = values.memory_info[index];
	return memory.kind == ResourceKind::Buffer && !memory.typed && !memory.formatted &&
	       !memory.idxen && !memory.offen && memory.data_bits == 32u &&
	       memory.data_dwords == dwords && memory.component_index == 0u &&
	       inst.Arg(1).Resolve() == Value(0u) && inst.Arg(2).Resolve() == Value(0u);
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
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::UMin32:
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
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::WqmU64:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
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
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPLdexp:
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

// Ops whose rule reads exactly this many operands, in order, before anything else.
uint32_t PlainArgCount(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ConditionRef:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::WqmU64:
		case ValueOpcode::LogicalNot:
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
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPTrunc32: return 1;
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::UMin32:
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
		case ValueOpcode::IEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::FPLdexp:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPMul32:
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
		case ValueOpcode::FPUnordGreaterThanEqual32: return 2;
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32: return 3;
		case ValueOpcode::BitFieldInsert: return 4;
		default: return 0;
	}
}

// The rule's operand order: ULessThan32 reads its bound first.
uint32_t ChainArgument(ValueOpcode op, uint32_t step) {
	return op == ValueOpcode::ULessThan32 ? 1u - step : step;
}

// An operand value that decides the result on its own, as the rule would short-circuit.
bool DecidesResult(ValueOpcode op, uint32_t argument, uint64_t value) {
	switch (op) {
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::ULessThanEqual32: return argument == 0u && value == 0u;
		case ValueOpcode::LogicalOr: return argument == 0u && value != 0u;
		case ValueOpcode::ULessThan32: return argument == 1u && static_cast<uint32_t>(value) == 0u;
		default: return false;
	}
}

// Whether the rule can still succeed after this operand failed.
bool ToleratesFailedOperand(ValueOpcode op) {
	return op == ValueOpcode::LogicalAnd || op == ValueOpcode::LogicalOr;
}

// Whether a predicate reads nothing but the lane index and constants.
bool IsClosedPredicate(Value root) {
	std::vector<const Inst*>        pending;
	std::unordered_set<const Inst*> seen;
	const auto                      push = [&](Value value) {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst != nullptr && seen.insert(inst).second) pending.push_back(inst);
	};
	push(root);
	while (!pending.empty()) {
		const auto* inst = pending.back();
		pending.pop_back();
		const auto op    = inst->GetOpcode();
		const auto plain = PlainArgCount(op);
		bool       pure  = plain != 0 && inst->NumArgs() >= plain;
		switch (op) {
			case ValueOpcode::LaneId:
			case ValueOpcode::DispatchThreadInRange:
			case ValueOpcode::Ballot:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectF32:
			case ValueOpcode::CompositeExtractU64: pure = true; break;
			case ValueOpcode::CompositeExtractU32x4: {
				const auto* source =
				    inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
				pure = source != nullptr && source->GetOpcode() == ValueOpcode::Ballot;
				break;
			}
			default: break;
		}
		if (!pure) return false;
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			push(inst->Arg(index));
		}
	}
	return true;
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type,
	                          bool wave_uniform = false)
	    : m_program(program), m_type(type), m_wave_uniform(wave_uniform) {}

	bool Run(Value value) { return Validate(value); }

private:
	// Operands are walked on an explicit stack: integer chains run tens of thousands deep.
	struct Scope {
		Value active_mask;
		bool  lane_bound                     = false;
		bool  operator==(const Scope&) const = default;
	};
	struct Frame {
		const Inst* inst;
		Scope       scope;
		uint32_t    next;
		uint32_t    count;
		bool        require_uniform;
	};

	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		const auto base = m_stack.size();
		if (const auto entered = Enter(value, require_uniform)) return *entered;
		bool failed = false;
		for (;;) {
			auto& frame = m_stack.back();
			if (!failed && frame.next < frame.count) {
				const auto arg     = frame.inst->Arg(frame.next++);
				const auto entered = Enter(arg, frame.require_uniform);
				failed             = entered.has_value() && !*entered;
				continue;
			}
			const auto done = frame;
			m_stack.pop_back();
			const bool valid = Finish(done.inst, done.require_uniform, done.scope, !failed);
			if (m_stack.size() == base) return valid;
			failed = !valid;
		}
	}

	bool Finish(const Inst* inst, bool require_uniform, const Scope& scope, bool valid) {
		m_visiting.erase(inst);
		if (valid && !require_uniform) m_validated_dependencies.insert(inst);
		if (valid && require_uniform) m_validated_uniform[inst].push_back(scope);
		return valid;
	}

	// Empty when the operands still have to be walked: their frame is on the stack.
	std::optional<bool> Enter(Value value, bool require_uniform) {
		value = value.Resolve();
		// Host floating-point evaluation does not model shader rounding/denormal modes.
		if (m_type != RuntimeValueType::Any &&
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
		if (require_uniform && !m_active_mask.IsEmpty() && value == m_active_mask) return true;
		// Integer-only dependency checks do not depend on the active EXEC mask.
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		if (require_uniform) {
			const auto cached = m_validated_uniform.find(inst);
			if (cached != m_validated_uniform.end() &&
			    std::ranges::find(cached->second, Scope {m_active_mask, m_lane_bound}) !=
			        cached->second.end()) {
				return true;
			}
		}
		if (!m_visiting.insert(inst).second) {
			return !require_uniform;
		}
		const Scope scope {m_active_mask, m_lane_bound};
		const auto finish = [&](bool valid) { return Finish(inst, require_uniform, scope, valid); };
		const auto operands = [&](bool uniform) -> std::optional<bool> {
			m_stack.push_back({inst, scope, 0u, static_cast<uint32_t>(inst->NumArgs()), uniform});
			return std::nullopt;
		};
		const auto under_mask = [&](Value mask, Value operand, bool lane_bound) {
			const Scope outer {m_active_mask, m_lane_bound};
			m_active_mask    = mask;
			m_lane_bound     = lane_bound;
			const bool valid = Validate(operand);
			m_active_mask    = outer.active_mask;
			m_lane_bound     = outer.lane_bound;
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
				return finish(false);
			}
			if (m_type != RuntimeValueType::Any &&
			    !under_mask({}, m_program.srt_reads[slot.U32()].value, false)) {
				return finish(false);
			}
		}
		if (!require_uniform) return operands(false);
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			// Empty EXEC reads lane zero, so ignored operands still require integer types.
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(2), false)) {
				return finish(false);
			}
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU32 || op == ValueOpcode::Void) {
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
		if (m_wave_uniform && op == ValueOpcode::GetBuiltin) {
			const auto axis = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
			return finish(m_program.stage == ShaderType::Compute && inst->NumArgs() == 2u &&
			              inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)) &&
			              axis.IsImmediate() && axis.GetType() == Type::U32 && axis.U32() < 3u);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type != RuntimeValueType::Any && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				return finish(false);
			}
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::Ballot) {
			// Re-executed per lane and every lane counts: an all-true mask, not the enclosing one.
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::U1) {
				return finish(false);
			}
			return finish(under_mask(Value(true), inst->Arg(0), true));
		}
		if (op == ValueOpcode::DispatchThreadInRange) {
			return finish(inst->NumArgs() == 0);
		}
		if (op == ValueOpcode::LaneId) {
			// Only a ballot re-executes its predicate per lane; elsewhere the value varies per
			// lane.
			return finish(inst->NumArgs() == 0 && m_lane_bound);
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			return finish(under_mask(inst->Arg(1).Resolve(), inst->Arg(0), false));
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
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
				return finish(false);
			}
			if (op == ValueOpcode::LoadBufferU32) {
				const auto guard = inst->Arg(4).Resolve();
				return finish(Validate(inst->Arg(0)) &&
				              ((!m_active_mask.IsEmpty() && guard == m_active_mask) || Validate(guard)));
			}
		} else if (DescriptorLoadDwords(op) != 0u) {
			if (!IsDescriptorLoad(m_program, *inst) ||
			    inst->Arg(0).ResolveInstruction() == nullptr ||
			    inst->Arg(0).ResolveInstruction()->GetOpcode() != ValueOpcode::GetBufferResource) {
				return finish(false);
			}
			const auto guard = inst->Arg(4).Resolve();
			return finish(
			    Validate(inst->Arg(0)) && Validate(inst->Arg(3)) &&
			    ((!m_active_mask.IsEmpty() && guard == m_active_mask) || Validate(guard)));
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2 ||
		           op == ValueOpcode::CompositeExtractU32x4) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			const bool  wide   = op == ValueOpcode::CompositeExtractU32x4;
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= (wide ? 4u : 2u)) {
				return finish(false);
			}
			const auto producer = source->GetOpcode();
			const bool known =
			    wide ? producer == ValueOpcode::Ballot || producer == ValueOpcode::LoadBufferU32x4
			         : producer == ValueOpcode::CompositeConstructU32x2 ||
			               producer == ValueOpcode::LoadBufferU32x2;
			if (!known) {
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
		return operands(true);
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	bool                            m_wave_uniform;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};


} // namespace

void CompileSrtWalker(ResourcePlan& program) {
	if (!program.resource_tracking_complete || !program.srt_plan_complete ||
	    program.capture_specialization_reads || !program.control_flow.empty() ||
	    program.srt_reads.empty()) return;

	using Reads = std::map<int32_t, const SrtRead*>;
	std::map<uint32_t, Reads> roots;
	std::unordered_map<const Inst*, Reads> pointer_uses;
	std::unordered_map<const Inst*, int32_t> offsets;
	for (const auto& read: program.srt_reads) {
		const auto* inst = read.value.Resolve().TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::LoadAddressU32 ||
		    inst->NumArgs() < 2 || !IsRawRead(program, *inst) ||
		    read.flat_offset >= program.srt_reads.size()) return;
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) return;
		const auto& memory = program.memory_info[inst->Flags<SrtReadFlags>().index];
		const auto relative =
		    (static_cast<int64_t>(static_cast<int32_t>(memory.offset)) & ~int64_t {3}) +
		    static_cast<int64_t>(offset.U32() & ~3u);
		if (relative < INT32_MIN || relative > INT32_MAX ||
		    !offsets.emplace(inst, static_cast<int32_t>(relative)).second) return;
	}
	uint32_t user_data_count = 0;
	for (const auto& read: program.srt_reads) {
		const auto* inst = read.value.ResolveInstruction();
		const auto* handle = inst->Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource ||
		    handle->NumArgs() != 2) return;
		const auto* low = handle->Arg(0).Resolve().TryInstruction();
		const auto* high = handle->Arg(1).Resolve().TryInstruction();
		if (low == nullptr || high == nullptr) return;
		Reads* uses = nullptr;
		if (low->GetOpcode() == ValueOpcode::GetUserData &&
		    high->GetOpcode() == ValueOpcode::GetUserData && low->NumArgs() == 1 &&
		    high->NumArgs() == 1 && low->Arg(0).GetType() == Type::ScalarReg &&
		    high->Arg(0).GetType() == Type::ScalarReg) {
			const auto reg = RegIndex(low->Arg(0).ScalarRegister());
			if (reg < program.user_data_base ||
			    RegIndex(high->Arg(0).ScalarRegister()) != reg + 1u) return;
			const auto base = reg - program.user_data_base;
			user_data_count = std::max(user_data_count, base + 2u);
			uses = &roots[base];
		} else {
			const auto lo = offsets.find(low);
			const auto hi = offsets.find(high);
			if (lo == offsets.end() || hi == offsets.end() ||
			    static_cast<int64_t>(lo->second) + 4 != hi->second ||
			    !EquivalentValue(program, low->Arg(0), high->Arg(0))) return;
			uses = &pointer_uses[low];
		}
		if (!uses->emplace(offsets.at(inst), &read).second) return;
	}
	std::unordered_set<const Inst*> visited;
	const auto reachable = [&](auto&& self, const Reads& reads) -> bool {
		for (const auto& [offset, read]: reads) {
			const auto* inst = read->value.ResolveInstruction();
			const auto child = pointer_uses.find(inst);
			if (child != pointer_uses.end() &&
			    (!visited.insert(inst).second || !self(self, child->second))) return false;
		}
		return true;
	};
	for (const auto& [reg, reads]: roots) {
		if (!reachable(reachable, reads)) return;
	}
	if (visited.size() != pointer_uses.size()) return;

	program.srt_code = std::make_unique<Xbyak::CodeGenerator>(4096, Xbyak::AutoGrow);
	auto& code = *program.srt_code;
	Xbyak::Label failed, finished;
	code.push(code.rbp);
	code.mov(code.rbp, code.rsp);
	code.cmp(code.r8, user_data_count);
	code.jb(failed, Xbyak::CodeGenerator::T_NEAR);
	code.mov(code.r9, AddressMask & ~uint64_t {3});
	// Follow shadPS4's VisitPointer: copy one table level in offset order, then
	// descend through its child pointers. Seed the descriptor evaluator's memo.
	const auto visit = [&](auto&& self, int32_t pointer_offset, const Reads& reads) -> void {
		code.push(code.rdi);
		code.mov(code.rdi, code.qword[code.rdi + pointer_offset]);
		code.and_(code.rdi, code.r9);
		const auto first = reads.begin()->first;
		const auto last = reads.rbegin()->first;
		if (first < 0) {
			code.mov(code.r10, -static_cast<int64_t>(first));
			code.cmp(code.rdi, code.r10);
			code.jb(failed, Xbyak::CodeGenerator::T_NEAR);
		}
		if (last > 0) {
			code.mov(code.r10, AddressMask - last);
			code.cmp(code.rdi, code.r10);
			code.ja(failed, Xbyak::CodeGenerator::T_NEAR);
		}
		for (const auto& [offset, read]: reads) {
			const auto index = read->value.ResolveInstruction()->EvaluationIndex(program.evaluation_value_count);
			using Entry = ResourcePlan::EvaluationContext::Entry;
			const auto slot = code.rdx + index * sizeof(Entry);
			code.mov(code.r10d, code.dword[code.rdi + offset]);
			code.mov(code.dword[code.rsi + read->flat_offset * sizeof(uint32_t)], code.r10d);
			code.mov(code.qword[slot + offsetof(Entry, value)], code.r10);
			code.mov(code.qword[slot + offsetof(Entry, generation)], code.rcx);
		}
		for (const auto& [offset, read]: reads) {
			const auto child = pointer_uses.find(read->value.ResolveInstruction());
			if (child == pointer_uses.end()) continue;
			self(self, offset, child->second);
		}
		code.pop(code.rdi);
	};
	for (const auto& [reg, reads]: roots) {
		visit(visit, reg * sizeof(uint32_t), reads);
	}
	code.mov(code.eax, 1);
	code.jmp(finished, Xbyak::CodeGenerator::T_NEAR);
	code.L(failed);
	code.xor_(code.eax, code.eax);
	code.L(finished);
	code.mov(code.rsp, code.rbp);
	code.pop(code.rbp);
	code.ret();
	code.readyRE();
	EXIT_IF(Xbyak::GetError() != 0);
}

size_t RawReadOffsetArgument(ValueOpcode op) {
	return DescriptorLoadDwords(op) != 0u ? 3u : 1u;
}

bool RawReadShapeOf(const ResourcePlan& program, const Inst& inst, uint32_t component_bytes,
                    RawReadShape& shape) {
	const auto index = inst.Flags<SrtReadFlags>().index;
	if (index >= program.memory_info.size()) {
		return false;
	}
	const auto op = inst.GetOpcode();
	shape.immediate =
	    static_cast<int64_t>(static_cast<int32_t>(program.memory_info[index].offset)) +
	    component_bytes;
	shape.kind = op == ValueOpcode::ReadConstBuffer ? RawReadShape::Kind::ConstBuffer
	             : op == ValueOpcode::LoadBufferU32 ? RawReadShape::Kind::VectorBuffer
	             : DescriptorLoadDwords(op) != 0u   ? RawReadShape::Kind::VectorDescriptor
	                                                : RawReadShape::Kind::Address;
	return true;
}

SrtRefusal RawReadAddressFrom(const RawReadShape& shape, const std::array<uint64_t, 5>& operands,
                              uint64_t& address, bool& zero) {
	zero              = false;
	const auto high   = operands[1];
	const auto offset = operands[2];
	const auto base   = ((high << 32u) | static_cast<uint32_t>(operands[0])) & AddressMask;
	if (shape.kind == RawReadShape::Kind::Address) {
		const auto relative = (shape.immediate & ~int64_t {3}) +
		                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
		return AddSignedAddress(base & ~uint64_t {3}, relative, address)
		           ? SrtRefusal::None
		           : SrtRefusal::ReadOutOfBounds;
	}
	const auto records = operands[3];
	const auto word3   = operands[4];
	if (shape.immediate < 0) {
		return SrtRefusal::UnsupportedOperand;
	}
	const auto byte_offset = (static_cast<uint64_t>(shape.immediate) & ~uint64_t {3}) +
	                         (static_cast<uint32_t>(offset) & ~3u);
	const auto stride      = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
	if (shape.kind != RawReadShape::Kind::ConstBuffer) {
		// Only the uniform, unswizzled DWORD address is evaluated on the host.
		if ((high & (1u << 31u)) != 0u || (word3 & ((1u << 23u) | 0xf0000000u)) != 0u) {
			return SrtRefusal::UnsupportedOperand;
		}
		if ((shape.kind == RawReadShape::Kind::VectorBuffer && stride == 0u) || records == 0u ||
		    ((word3 >> 12u) & 0x7fu) == 0u) {
			zero = true;
			return SrtRefusal::None;
		}
	}
	const auto size = stride == 0u ? static_cast<uint64_t>(static_cast<uint32_t>(records))
	                               : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
	if (byte_offset > size || size - byte_offset < sizeof(uint32_t)) {
		zero = true;
		return SrtRefusal::None;
	}
	address = (base & ~uint64_t {3}) + byte_offset;
	return SrtRefusal::None;
}

SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     SrtWalker* clean_evaluator)
    : m_program(program), m_runtime(runtime), m_clean_evaluator(clean_evaluator), m_session(this),
      m_read_policy(clean_evaluator != nullptr ? 2u : 0u), m_state(&m_own_state),
      m_context(AcquireContext()) {}

SrtWalker::SrtWalker(SrtWalker& parent, Value active_mask, bool clean, SrtWalker* clean_evaluator,
                     bool isolated)
    : m_program(parent.m_program),
      m_runtime(clean ? CleanRuntime(parent.m_runtime) : parent.m_runtime),
      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()),
      m_reachable_blocks(parent.m_reachable_blocks), m_session(parent.m_session),
      m_read_policy((parent.m_read_policy & 1u) | (clean ? 1u : 0u) |
                    (clean_evaluator != nullptr ? 2u : 0u)),
      m_isolated(isolated || parent.m_isolated), m_state(parent.m_state),
      m_context(AcquireContext()) {
	// A walker that takes phis on trust or binds a lane keeps its own memo, as do its children.
	if (!isolated && parent.m_isolated) {

SrtWalker::~SrtWalker() {
	if (m_session != this) return;
	for (auto& context: m_program.evaluation_contexts)
		if (context.session == this) context.session = nullptr;
}

bool SrtWalker::Evaluate(Value value, uint32_t& result) {
	uint64_t wide = 0;
	if (!EvaluateWide(value, wide)) {
		return false;
	}
	result = static_cast<uint32_t>(wide);
	return true;
}

ResourcePlan::EvaluationContext& SrtWalker::AcquireContext() {
	ResourcePlan::EvaluationContext* unused = nullptr;
	for (auto& context: m_program.evaluation_contexts) {
		if (context.session == m_session && context.active_mask == m_active_mask &&
		    context.read_policy == m_read_policy) return context;
		if (context.session == nullptr && unused == nullptr) unused = &context;
	}
	auto& context = unused != nullptr ? *unused : m_program.evaluation_contexts.emplace_back();
	context.session = m_session;
	context.active_mask = m_active_mask;
	context.read_policy = m_read_policy;
	context.generation += 4;
	return context;
}

void SrtWalker::ResetUnknowns() {
	for (auto& context: m_program.evaluation_contexts) {
		if (context.session != m_session) continue;
		for (auto& entry: context.values)
			if (entry.generation == (context.generation | 2u)) entry.generation = 0;
	}
}

float SrtWalker::Float32(uint64_t bits) {
	return std::bit_cast<float>(static_cast<uint32_t>(bits));
}

}

SrtWalker::Visit SrtWalker::BeginVisit(ResourcePlan::EvaluationContext::Entry& entry) {
	Visit visit {++m_state->visits, VolatileRefusals(), m_state->cycle_root, DependenceScope(),
	             false};
	entry.generation    = m_context.generation | 1u;
	entry.value         = visit.order;
	m_state->cycle_root = UINT64_MAX;
	if (visit.lane != nullptr) {
		visit.outer_dependent = visit.lane->dependent;
		visit.lane->dependent = false;
	}
	return visit;
}

void SrtWalker::EndVisit(const Visit& visit, uint32_t index, bool evaluated, uint64_t value) {
	const auto cycle_root = m_state->cycle_root;
	m_state->cycle_root   = std::min(visit.outer_cycle_root, cycle_root);
	const bool dependent  = visit.lane != nullptr && visit.lane->dependent;
	if (visit.lane != nullptr) visit.lane->dependent = visit.outer_dependent || dependent;
	// Evaluation may grow the dense memo vector.
	auto& memo          = m_context.values[index];
	memo.lane_dependent = dependent;
	if (evaluated) {
		memo.value      = value;
		memo.generation = m_context.generation;
	} else if (cycle_root < visit.order || VolatileRefusals() != visit.volatile_refusals ||
	           (dependent && m_shares_lanes)) {
		// A path-dependent refusal (outer cycle, GPU-owned slot) is retried, not remembered.
		memo.generation = 0;
		return;
	} else {
		memo.value      = static_cast<uint64_t>(m_state->last);
		memo.generation = m_context.generation | 2u;
	}
	if (dependent && m_shares_lanes) m_lane_entries.push_back(index);
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
			default: return Refuse(SrtRefusal::UnsupportedOperand);
		}
	}
	if (!m_active_mask.IsEmpty() && value == m_active_mask) {
		result = 1u;
		return true;
	}
	auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return Refuse(SrtRefusal::UnsupportedOperand);
	}
	if (!m_active_mask.IsEmpty() && IsRuntimeSelect(inst->GetOpcode()) &&
	    inst->NumArgs() == 3 && inst->Arg(0).Resolve() == m_active_mask) {
		return EvaluateWide(inst->Arg(1), result);
	}
	// A value a fixpoint trial assumes answers before the memo.
	if (const auto* assumed = m_assumed.find(inst); assumed != nullptr) {
		result = *assumed;
		return true;
	}
	const auto index = inst->EvaluationIndex(m_program.evaluation_value_count);
	if (index >= m_context.values.size()) {
		m_context.values.resize(m_program.evaluation_value_count);
	}
	if (m_context.values[index].generation == m_context.generation) {
	auto& entry = m_context.values[index];
	if (entry.generation == m_context.generation ||
	    entry.generation == (m_context.generation | 2u)) {
		if (auto* scope = DependenceScope(); scope != nullptr && entry.lane_dependent) {
			scope->dependent = true;
		}
		if (entry.generation != m_context.generation) {
			return Refuse(static_cast<SrtRefusal>(entry.value));
		}
		result = entry.value;
		return true;
	}
	// A value still being evaluated: a cycle, which only a loop phi can close.
	if (entry.generation == (m_context.generation | 1u)) {
		m_state->cycle_root = std::min(m_state->cycle_root, entry.value);
		return Refuse(SrtRefusal::VaryingPhi);
	}
	auto& memo = m_context.values[index];
		return false;
	const auto visit     = BeginVisit(entry);
	uint64_t   out       = 0;
	const auto plain     = PlainArgCount(inst->GetOpcode());
	const bool evaluated = plain != 0 && inst->NumArgs() >= plain ? EvaluateChain(*inst, out)
	                                                              : EvaluateInst(*inst, out);
	EndVisit(visit, index, evaluated, out);
	if (evaluated) result = out;
	return evaluated;
}

const Inst* SrtWalker::ColdPlainOperand(Value value, uint32_t& index) {
	value            = value.Resolve();
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || (!m_active_mask.IsEmpty() && value == m_active_mask)) {
		return nullptr;
	}
	const auto count = PlainArgCount(inst->GetOpcode());
	if (count == 0 || inst->NumArgs() < count || m_assumed.contains(inst)) {
		return nullptr;
	}
	index = inst->EvaluationIndex(m_program.evaluation_value_count);
	if (index >= m_context.values.size()) {
		m_context.values.resize(m_program.evaluation_value_count);
	}
	const auto generation = m_context.values[index].generation;
	return (generation | 3u) == (m_context.generation | 3u) ? nullptr : inst;
}

// EvaluateWide's recursion through plain operations, on an explicit stack in rule operand order.
bool SrtWalker::EvaluateChain(const Inst& root, uint64_t& result) {
	struct Frame {
		const Inst* inst;
		uint32_t    index;
		uint32_t    next;
		uint32_t    count;
		Visit       visit;
	};
	static thread_local std::vector<Frame> stack;
	struct Unwind {
		std::vector<Frame>& frames;
		size_t              base;
		~Unwind() { frames.resize(base); }
	} const unwind {stack, stack.size()};
	const auto base = unwind.base;
	stack.push_back({&root, 0u, 0u, PlainArgCount(root.GetOpcode()), {}});
	bool failed = false;
	// Applies an operand's outcome to the frame that asked for it.
	const auto settle = [&](bool ok, uint64_t value) {
		auto&      owner    = stack.back();
		const auto op       = owner.inst->GetOpcode();
		const auto argument = ChainArgument(op, owner.next - 1u);
		if (!ok) {
			failed = !ToleratesFailedOperand(op);
		} else if (DecidesResult(op, argument, value)) {
			owner.next = owner.count;
		}
	};
	for (;;) {
		auto& frame = stack.back();
		if (!failed && frame.next < frame.count) {
			const auto arg = frame.inst->Arg(ChainArgument(frame.inst->GetOpcode(), frame.next++));
			uint32_t   index = 0;
			if (const auto* cold = ColdPlainOperand(arg, index)) {
				const auto visit = BeginVisit(m_context.values[index]);
				stack.push_back({cold, index, 0u, PlainArgCount(cold->GetOpcode()), visit});
				continue;
			}
			// EvaluateWide can re-enter this walk and grow the stack, so `frame` may dangle.
			uint64_t   value = 0;
			const bool ok    = EvaluateWide(arg, value);
			settle(ok, value);
			continue;
		}
		const auto current   = frame;
		uint64_t   out       = 0;
		const bool evaluated = !failed && EvaluateInst(*current.inst, out);
		if (stack.size() == base + 1u) {
			result = out;
			return evaluated;
		}
		EndVisit(current.visit, current.index, evaluated, out);
		stack.pop_back();
		failed = false;
		settle(evaluated, out);
	}
}

bool SrtWalker::Arg(const Inst& inst, size_t index, uint64_t& result) {
	return EvaluateWide(inst.Arg(index), result);
}

bool SrtWalker::EvaluatePhi(const Inst& inst, uint64_t& result) {
	if (const auto control = m_program.control_phi_predecessors.find(&inst);
	    control != m_program.control_phi_predecessors.end()) {
		bool found = false;
		for (size_t index = 0; index < inst.NumArgs(); ++index) {
			if (m_reachable_blocks != nullptr &&
			    !m_reachable_blocks->at(control->second[index])) continue;
			uint64_t incoming = 0;
			if (!Arg(inst, index, incoming)) return false;
			if (found && incoming != result) return Refuse(SrtRefusal::VaryingPhi);
			result = incoming;
			found = true;
		}
		return found || Refuse(SrtRefusal::UndefinedValue);
	}
	const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
	const Value phi(const_cast<Inst*>(&inst));
	if (!value.IsEmpty()) {
		return EvaluateWide(value, result);
	}
	if (m_barred.contains(&inst)) {
		return Refuse(SrtRefusal::VaryingPhi);
	}
	// Loop-carried: assume the entry value and require every operand of the phi web to reproduce
	// it.
	if (!EvaluateWide(entry, candidate)) {
		return false;
	}
	// A separate memo: this walk already holds the operands the phi was reached through.
	SrtWalker trial(*this, m_active_mask, false, m_clean_evaluator, true);
	trial.m_assumed        = m_assumed;
	trial.m_barred         = m_barred;
	trial.m_lane           = m_lane;
	trial.m_suspended_lane = m_suspended_lane;
	for (const auto* member: web) {
		if (trial.m_assumed.emplace(member, candidate) != candidate) {
			return Refuse(SrtRefusal::VaryingPhi);
		}
	}
	for (const auto* member: web) {
		for (size_t index = 0; index < member->NumArgs(); index++) {
			uint64_t carried = 0;
			if (!trial.EvaluateWide(member->Arg(index), carried)) return false;
			if (carried != candidate) return Refuse(SrtRefusal::VaryingPhi);
		}
	}
	result = candidate;
	return true;
}

bool SrtWalker::EvaluateExtract(const Inst& inst, uint64_t& result) {
	const auto index = inst.Arg(1).Resolve();
	if (!index.IsImmediate() || index.GetType() != Type::U32) {
		return Refuse(SrtRefusal::UnsupportedOperand);
	}
	const auto component = index.U32();
	if (inst.GetOpcode() == ValueOpcode::CompositeExtractU32x4) {
		return EvaluateExtractU32x4(inst, component, result);
	}
	if (component >= 2u) {
		return Refuse(SrtRefusal::UnsupportedOperand);
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
		return Refuse(SrtRefusal::UnsupportedOperand);
	}
	if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
		return EvaluateWide(source->Arg(component), result);
	}
	if (source->GetOpcode() == ValueOpcode::LoadBufferU32x2 &&
	    IsDescriptorLoad(m_program, *source)) {
		if (m_clean_evaluator != nullptr && m_clean_evaluator->m_active_mask == m_active_mask) {
			return Delegate(*m_clean_evaluator, Value(const_cast<Inst*>(&inst)), result);
		}
		return EvaluateRawRead(*source, result, component * sizeof(uint32_t));
	}
}

	const auto* source = inst.Arg(0).ResolveInstruction();
	if (source == nullptr || component >= 4u) {
		return Refuse(SrtRefusal::UnsupportedOperand);
	}
	const auto& mem    = m_program.memory_info[flags.index];
	if (source->GetOpcode() == ValueOpcode::Ballot) {
		// The ballot packs lanes 0-31 and 32-63 into the low two dwords; the upper two are zero.
		uint64_t mask = 0;
		if (!Arg(inst, 0, mask)) {
			return false;
		}
		result = component < 2u ? static_cast<uint32_t>(mask >> (component * 32u)) : 0u;
		return true;
	}
	if (source->GetOpcode() == ValueOpcode::LoadBufferU32x4 &&
	    IsDescriptorLoad(m_program, *source)) {
		if (m_clean_evaluator != nullptr && m_clean_evaluator->m_active_mask == m_active_mask) {
			return Delegate(*m_clean_evaluator, Value(const_cast<Inst*>(&inst)), result);
		}
		return EvaluateRawRead(*source, result, component * sizeof(uint32_t));
	}
	return Refuse(SrtRefusal::UnsupportedOperand);
	RawReadShape shape;
	if (!RawReadShapeOf(m_program, inst, component_bytes, shape)) {
		return Refuse(SrtRefusal::UnsupportedOperand);
	}
	const bool vector = shape.kind == RawReadShape::Kind::VectorBuffer ||
	                    shape.kind == RawReadShape::Kind::VectorDescriptor;
	const auto guard = vector ? inst.Arg(4).Resolve() : Value {};
	if (vector && (guard.IsImmediate() || guard != m_active_mask)) {
		uint64_t enabled = 0;
		if (!Arg(inst, 4, enabled)) return false;
		if (enabled == 0u) {
			result = 0u;
			return true;
		}
	}
	const auto* handle = inst.Arg(0).ResolveInstruction();
	if (handle == nullptr) {
		return Refuse(SrtRefusal::UnsupportedOperand);
	}
	auto&        operands = event.operands;
	if (!Arg(*handle, 0, operands[0]) || !Arg(*handle, 1, operands[1]) ||
	    !Arg(inst, RawReadOffsetArgument(inst.GetOpcode()), operands[2])) {
		return false;
	}
	if (shape.kind != RawReadShape::Kind::Address) {
	}
		uint64_t records = 0;
		uint64_t word3   = 0;
		if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
		}
		if (immediate < 0) {
		}
			}
		}
			result = 0u;
			return true;
	} else {
		const auto relative = (immediate & ~int64_t {3}) +
		                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
		if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
		}
	}
	uint32_t word = 0;
	const auto reader = vector ? m_runtime.read_specialization_memory : m_runtime.read_memory;
	if (reader != nullptr) {
		if (!reader(m_runtime.userdata, address, {&word, 1})) {
			return Refuse(SrtRefusal::ReadRefused);
		}
	} else {
		if (vector) return Refuse(SrtRefusal::ReadRefused);
		std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
	}
	result = word;
	return true;
}

void SrtWalker::BarAssumptions(SrtWalker& walker) const {
	// A lane walk resolves selects on its own mask, so assumptions made above it need not hold.
	walker.m_barred = m_barred;
	for (const auto& assumed: m_assumed)
		walker.m_barred.insert(assumed.first);
}

uint32_t SrtWalker::ClosedBallotIndex(const Inst& ballot) {
	const auto index = ballot.EvaluationIndex(m_program.evaluation_value_count);
	auto&      table = m_program.closed_ballots;
	if (index >= table.size()) {
		table.resize(std::max<size_t>(index + 1u, m_program.evaluation_value_count));
	}
	auto& entry = table[index];
	if (entry.state == ResourcePlan::ClosedBallot::Unknown) {
		entry.state = IsClosedPredicate(ballot.Arg(0)) ? ResourcePlan::ClosedBallot::Closed
		                                               : ResourcePlan::ClosedBallot::Open;
	}
	return entry.state == ResourcePlan::ClosedBallot::Open ? UINT32_MAX : index;
}

void SrtWalker::ForgetLaneValues() {
	for (const auto index: m_lane_entries)
		m_context.values[index].generation = 0;
	m_lane_entries.clear();
}

bool SrtWalker::EvaluateBallot(const Inst& inst, uint64_t& result) {
	// One bit per lane; a predicate that never asks for the lane takes one walk for the wave.
	const auto lanes = m_program.wave_size;
	if (lanes == 0u || lanes > 64u) {
		return Refuse(SrtRefusal::UnsupportedOperand);
	}
	const auto wave = lanes == 64u ? ~uint64_t {0} : (uint64_t {1} << lanes) - 1u;
	// A predicate of the lane index and constants alone has one mask per frozen plan.
	const auto closed = m_program.frozen_values ? ClosedBallotIndex(inst) : UINT32_MAX;
	if (closed != UINT32_MAX &&
	    m_program.closed_ballots[closed].state == ResourcePlan::ClosedBallot::Known) {
		result = m_program.closed_ballots[closed].mask;
		return true;
	}
	LaneScope scope;
	SrtWalker clean_lane(*this, Value(true), true, nullptr, true);
	SrtWalker lane_walk(*this, Value(true), false, &clean_lane, true);
	for (auto* walker: {&clean_lane, &lane_walk}) {
		walker->m_lane         = &scope;
		walker->m_shares_lanes = true;
		BarAssumptions(*walker);
	}
	uint64_t mask = 0;
	for (scope.lane = 0; scope.lane < lanes; ++scope.lane) {
		scope.dependent = false;
		clean_lane.ForgetLaneValues();
		lane_walk.ForgetLaneValues();
		uint64_t taken = 0;
		if (!lane_walk.EvaluateWide(inst.Arg(0), taken)) {
			return false;
		}
		if (scope.lane == 0u && !scope.dependent) {
			mask = taken != 0u ? wave : 0u;
			break;
		}
		if (taken != 0u) mask |= uint64_t {1} << scope.lane;
	}
	if (closed != UINT32_MAX) {
		m_program.closed_ballots[closed] = {ResourcePlan::ClosedBallot::Known, mask};
	}
	result = mask;
	return true;
}

bool SrtWalker::EvaluateInst(const Inst& inst, uint64_t& result) {
	const auto refusals = m_state->refusals;
	if (EvaluateRule(inst, result)) return true;
	// The operands evaluated but this instruction cannot combine them.
	if (m_state->refusals == refusals) Refuse(SrtRefusal::UnsupportedOperand);
	return false;
}

bool SrtWalker::EvaluateRule(const Inst& inst, uint64_t& result) {
	uint64_t   a       = 0;
	uint64_t   b       = 0;
	uint64_t   c       = 0;
	const auto binary  = [&]() { return Arg(inst, 0, a) && Arg(inst, 1, b); };
	const auto ternary = [&]() {
		return Arg(inst, 0, a) && Arg(inst, 1, b) && Arg(inst, 2, c);
	};
		return std::bit_cast<int32_t>(static_cast<uint32_t>(value));
	};
	const auto f32 = [](float value) {
		return static_cast<uint64_t>(std::bit_cast<uint32_t>(value));
	};
	const auto nan2 = [&]() { return std::isnan(Float32(a)) || std::isnan(Float32(b)); };
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
			// A uniform operand, so a ballot lane bound above does not reach it.
			auto* lane       = m_lane;
			auto* suspended  = m_suspended_lane;
			m_suspended_lane = DependenceScope();
			m_lane           = nullptr;
			bool evaluated   = false;
			if (!m_active_mask.IsEmpty() && inst.Arg(1).Resolve() == m_active_mask) {
				evaluated = EvaluateWide(inst.Arg(0), result);
			} else {
				SrtWalker active(*this, inst.Arg(1), false, &clean_active);
				evaluated = active.EvaluateWide(inst.Arg(0), result);
			}
			m_lane           = lane;
			m_suspended_lane = suspended;
			return evaluated;
		}
		case ValueOpcode::Ballot: return EvaluateBallot(inst, result);
		case ValueOpcode::DispatchThreadInRange: result = 1u; return true;
		case ValueOpcode::LaneId:
			// Only a per-lane sweep binds the lane; elsewhere the value varies per lane.
			if (m_lane == nullptr) return Refuse(SrtRefusal::VaryingLane);
			m_lane->dependent = true;
			result            = m_lane->lane;
			return true;
		case ValueOpcode::ReadConst: {
			const auto slot = inst.Arg(1).Resolve();
			if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return false;
			}
			// A flat slot is also refreshed outside any lane sweep, so it must hold without a lane.
			auto* lane       = m_lane;
			auto* suspended  = m_suspended_lane;
			m_suspended_lane = DependenceScope();
			m_lane           = nullptr;
			const SrtReadSlotScope scope(slot.U32());
			const bool read  = EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
			m_lane           = lane;
			m_suspended_lane = suspended;
			return read;
		}
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
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU32:
			if (IsRawRead(m_program, inst)) {
				if (m_clean_evaluator != nullptr &&
				    (inst.Flags<SrtReadFlags>().clean != 0u ||
				     (inst.GetOpcode() == ValueOpcode::LoadBufferU32 &&
				      m_clean_evaluator->m_active_mask == m_active_mask))) {
					return Delegate(*m_clean_evaluator, Value(const_cast<Inst*>(&inst)), result);
				}
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
		case ValueOpcode::UMulHi:
			if (binary()) {
				result = (static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint32_t>(b)) >> 32u;
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
		case ValueOpcode::UMin32:
		case ValueOpcode::UMax32:
			if (binary()) {
				result = inst.GetOpcode() == ValueOpcode::UMin32
				    ? std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b))
				    : std::max(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
				return true;
			}
			return false;
		case ValueOpcode::SMin32:
		case ValueOpcode::SMax32:
			if (binary()) {
				result = static_cast<uint32_t>(inst.GetOpcode() == ValueOpcode::SMin32
				                                   ? std::min(s32(a), s32(b))
				                                   : std::max(s32(a), s32(b)));
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
				// The conversion saturates and maps NaN to zero.
				const auto value = Float32(a);
				result           = std::isnan(value) || value <= 0.0f ? 0u
				                   : value >= 4294967296.0f ? UINT32_MAX
				                                            : static_cast<uint32_t>(value);
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
		case ValueOpcode::FPRecipIFlag32:
			if (Arg(inst, 0, a)) {
				const auto exponent = (a >> 23u) & 0xffu;
				// Normal positive powers of two have exact normal reciprocals in every FP mode.
				if ((a & 0x807fffffu) != 0u || exponent == 0u || exponent >= 254u) return false;
				result = (254u - exponent) << 23u;
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
		case ValueOpcode::FPLog2: {
			if (!Arg(inst, 0, a)) return false;
			const auto x = Float32(a);
			switch (inst.GetOpcode()) {
				case ValueOpcode::FPAbs32: result = f32(std::fabs(x)); break;
				case ValueOpcode::FPNeg32: result = f32(-x); break;
				case ValueOpcode::FPSaturate32: result = f32(std::clamp(x, 0.0f, 1.0f)); break;
				case ValueOpcode::FPRoundEven32: result = f32(std::nearbyint(x)); break;
				case ValueOpcode::FPFloor32: result = f32(std::floor(x)); break;
				case ValueOpcode::FPCeil32: result = f32(std::ceil(x)); break;
				case ValueOpcode::FPFract32: result = f32(x - std::floor(x)); break;
				case ValueOpcode::FPSqrt: result = f32(std::sqrt(x)); break;
				case ValueOpcode::FPRecip32: result = f32(1.0f / x); break;
				case ValueOpcode::FPRecipSqrt32: result = f32(1.0f / std::sqrt(x)); break;
				case ValueOpcode::FPExp2: result = f32(std::exp2(x)); break;
				default: result = f32(std::log2(x)); break;
			}
			return true;
		}
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32: {
			if (!ternary()) return false;
			const auto x  = Float32(a);
			const auto y  = Float32(b);
			const auto z  = Float32(c);
			const auto lo = std::fmin(std::fmin(x, y), z);
			const auto hi = std::fmax(std::fmax(x, y), z);
			switch (inst.GetOpcode()) {
				case ValueOpcode::FPMinTri32: result = f32(lo); break;
				case ValueOpcode::FPMaxTri32: result = f32(hi); break;
				case ValueOpcode::FPMedTri32: result = f32(x + y + z - lo - hi); break;
				default: result = f32(x * y + z); break;
			}
			return true;
		}
		case ValueOpcode::FPLdexp:
			if (binary()) {
				result = f32(std::ldexp(Float32(a), s32(b)));
				return true;
			}
			return false;
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPUnordGreaterThanEqual32: {
			if (!binary()) return false;
			const auto x = Float32(a);
			const auto y = Float32(b);
			switch (inst.GetOpcode()) {
				case ValueOpcode::FPAdd32: result = f32(x + y); break;
				case ValueOpcode::FPSub32: result = f32(x - y); break;
			return true;
		}
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
		case ValueOpcode::FindUMsb32:
			if (Arg(inst, 0, a)) {
				const auto value = static_cast<uint32_t>(a);
				result =
				    value == 0u ? UINT32_MAX : static_cast<uint32_t>(31 - std::countl_zero(value));
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
		case ValueOpcode::WqmU64:
			// S_WQM_B64: every group of four mask bits becomes all ones if any of them is set.
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
			if (Delegate(predicate, inst.Arg(0), a)) {
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
		case ValueOpcode::IEqual64:
		case ValueOpcode::INotEqual64:
			if (!binary()) return false;
			result = inst.GetOpcode() == ValueOpcode::IEqual64 ? a == b : a != b;
			return true;
		case ValueOpcode::ULessThan32:
			// No U32 is below zero, including an unknown loop-carried value.
			if (!Arg(inst, 1, b)) return false;
			if (static_cast<uint32_t>(b) == 0u) {
				result = 0u;
				return true;
			}
			if (Arg(inst, 0, a)) {
				result = static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
				return true;
			}
			return false;
		case ValueOpcode::ULessThanEqual32:
			// A zero lower bound accepts every U32, including a varying lane value.
			if (!Arg(inst, 0, a)) return false;
			if (a == 0u) {
				result = 1u;
				return true;
			}
			if (Arg(inst, 1, b)) {
				result = static_cast<uint32_t>(a) <= static_cast<uint32_t>(b);
				return true;
			}
			return false;
		case ValueOpcode::UGreaterThan32:
			if (binary()) {
				result = static_cast<uint32_t>(a) > static_cast<uint32_t>(b);
				return true;
			}
			return false;
		case ValueOpcode::UGreaterThanEqual32:
			if (binary()) {
				result = static_cast<uint32_t>(a) >= static_cast<uint32_t>(b);
				return true;
			}
			return false;
		case ValueOpcode::ULessThan64:
		case ValueOpcode::UGreaterThan64:
			if (binary()) {
				result = inst.GetOpcode() == ValueOpcode::ULessThan64 ? a < b : a > b;
				return true;
			}
			return false;
		case ValueOpcode::SLessThan64:
			if (binary()) {
				result = std::bit_cast<int64_t>(a) < std::bit_cast<int64_t>(b);
				return true;
			}
			return false;
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
			if (binary()) {
				switch (inst.GetOpcode()) {
					case ValueOpcode::SLessThan32: result = s32(a) < s32(b); break;
					case ValueOpcode::SLessThanEqual32: result = s32(a) <= s32(b); break;
					case ValueOpcode::SGreaterThan32: result = s32(a) > s32(b); break;
					default: result = s32(a) >= s32(b); break;
				}
				return true;
			}
			return false;
		case ValueOpcode::LogicalAnd: {
			const bool left = Arg(inst, 0, a);
			if (left && a == 0u) {
				result = 0u;
				return true;
			}
			if (Arg(inst, 1, b) && (b == 0u || left)) {
				result = b != 0u;
				return true;
			}
			if (!left) {
				// Only lanes satisfying the left EXEC factor can make the conjunction
				// true. Use a separate memo so this assumption cannot escape its RHS.
				SrtWalker active(m_clean_evaluator != nullptr ? *m_clean_evaluator : *this,
				                 inst.Arg(0));
				if (active.EvaluateWide(inst.Arg(1), b) && b == 0u) {
					result = 0u;
					return true;
				}
			}
			return false;
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
		case ValueOpcode::UndefU32: return Refuse(SrtRefusal::UndefinedValue);
		default: break;
	}
	return Refuse(SrtRefusal::UnsupportedOpcode);
}
bool SrtWalker::EvaluateDescriptor(uint32_t source, DescriptorValue& result) {
	m_refusal = SrtRefusal::None;
	if (source >= m_program.descriptor_sources.size()) {
		m_refusal = SrtRefusal::UnsupportedOperand;
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

bool SrtWalker::RefreshFlatBuffer(std::vector<uint32_t>& flat, bool prune, SrtWalker* conditions,
                                  std::vector<uint32_t>* gpu_owned_slots) {
	if (!m_program.srt_plan_complete) return false;
	if (m_program.srt_code && m_runtime.read_memory == nullptr) {
		flat.resize(m_program.srt_reads.size());
		m_program.active_sources.clear();
		m_context.values.resize(m_program.evaluation_value_count);
		using Walker = bool KYTY_SYSV_ABI (*)(const uint32_t*, uint32_t*,
		    ResourcePlan::EvaluationContext::Entry*, uint64_t, size_t);
		return m_program.srt_code->getCode<Walker>()(m_runtime.user_data.data(), flat.data(),
		    m_context.values.data(), m_context.generation, m_runtime.user_data.size());
	}
	const auto fail = [&](SrtRefusal reason) {
		m_refusal = reason;
		return false;
	};
	bool       owned   = false;
	const auto refresh = [&](uint32_t slot) {
		if (slot >= m_program.srt_reads.size()) return fail(SrtRefusal::UnsupportedOperand);
		const auto& read = m_program.srt_reads[slot];
		const auto* inst = read.value.ResolveInstruction();
		const bool clean = inst != nullptr && inst->Flags<SrtReadFlags>().clean != 0u;
		if (clean && (m_clean_evaluator == nullptr || m_runtime.read_specialization_memory == nullptr))
			return fail(SrtRefusal::ReadRefused);
		auto& evaluator = clean ? *m_clean_evaluator : *this;
		if (read.flat_offset >= flat.size()) return fail(SrtRefusal::UnsupportedOperand);
		const SrtReadSlotScope scope(slot);
		if (evaluator.Evaluate(read.value, flat[read.flat_offset])) return true;
		if (gpu_owned_slots != nullptr && evaluator.m_refusal == SrtRefusal::GpuOwned) {
		return fail(evaluator.m_refusal);
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
	auto& visited = m_program.visited_blocks;
	auto& reachable = m_program.reachable_blocks;
	auto& pending = m_program.pending_blocks;
	visited.assign(m_program.control_flow.size(), 1u);
	ResetUnknowns();
	m_reachable_blocks = &visited;
	if (m_clean_evaluator != nullptr) {
		m_clean_evaluator->ResetUnknowns();
		m_clean_evaluator->m_reachable_blocks = &visited;
	}
	for (;;) {
		reachable.assign(m_program.control_flow.size(), 0u);
		pending.clear();
		pending.push_back(0u);
		while (!pending.empty()) {
			const auto index = pending.back();
			pending.pop_back();
			if (reachable.at(index)) continue;
			reachable[index] = 1u;
			const auto& block = m_program.control_flow[index];
			uint32_t condition = 0;
			auto& predicate = m_clean_evaluator != nullptr ? *m_clean_evaluator : *this;
			if (!block.condition.IsEmpty() &&
			    (m_runtime.read_specialization_memory != nullptr || !m_program.capture_specialization_reads) &&
			    predicate.Evaluate(block.condition, condition)) {
				pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
			} else {
				pending.insert(pending.end(), block.successors.begin(), block.successors.end());
			}
		}
		if (reachable == visited) break;
		visited.swap(reachable);
		if (m_program.control_phi_predecessors.empty()) break;
		// Only unknown values need another evaluation: successful Phi facts remain
		// true as their possible predecessors shrink. Keep uniform-read memos stable.
		ResetUnknowns();
		if (m_clean_evaluator != nullptr) m_clean_evaluator->ResetUnknowns();
	}
	flat.assign(m_program.srt_reads.size(), 0u);
	active.assign(m_program.descriptor_sources.size(), 1u);
	for (const auto& block: m_program.control_flow)
		for (const auto source: block.sources) active.at(source) = 0u;
	for (uint32_t index = 0; index < visited.size(); ++index) {
		if (!visited[index]) continue;
		const auto& block = m_program.control_flow[index];
		for (const auto source: block.sources) active[source] = 1u;
		for (const auto slot: block.srt_reads)
			if (!refresh(slot)) return false;
	}
	return true;
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}

bool ValidateWaveUniformValue(const ResourcePlan& program, Value value) {
	return RuntimeValidator(program, RuntimeValueType::Any, true).Run(value);
}


} // namespace Libs::Graphics::ShaderRecompiler::IR
