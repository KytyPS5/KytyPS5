#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/abi.h"
#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <xbyak/xbyak.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
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

// The bits of an immediate, as SrtWalker::EvaluateWide and the tape read them. False: a type
// they do not evaluate.
bool ImmediateBits(Value value, uint64_t& bits) {
	switch (value.GetType()) {
		case Type::U1: bits = value.U1(); return true;
		case Type::U8: bits = value.U8(); return true;
		case Type::U16: bits = value.U16(); return true;
		case Type::U32: bits = value.U32(); return true;
		case Type::U64: bits = value.U64(); return true;
		case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); return true;
		default: return false;
	}
}

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
		case ValueOpcode::UMulHi:
		case ValueOpcode::UMin32:
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
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThanEqual32:
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
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type,
	                          bool wave_uniform = false)
	    : m_program(program), m_type(type), m_wave_uniform(wave_uniform) {}

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
		if (!m_visiting.insert(inst).second) {
			return !require_uniform;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
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
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
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
	bool                            m_wave_uniform;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};



float Float32(uint64_t bits) {
	return std::bit_cast<float>(static_cast<uint32_t>(bits));
}

// Operations that evaluate each argument once, in the same walker, and read nothing else.
uint32_t PureArity(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConditionRef:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::LogicalNot: return 1;
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UMulHi:
		case ValueOpcode::UMin32:
		case ValueOpcode::UMax32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::IEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::LogicalXor: return 2;
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract: return 3;
		case ValueOpcode::BitFieldInsert: return 4;
		default: return 0;
	}
}

// The result of a pure operation (PureArity) that its first argument decides alone, when the
// other arguments are not known: a zero lower bound accepts every U32, also a value that varies
// from lane to lane.
bool DecidedByFirst(ValueOpcode op, uint64_t first, uint64_t& result) {
	if (op == ValueOpcode::ULessThanEqual32 && static_cast<uint32_t>(first) == 0u) {
		result = 1u;
		return true;
	}
	return false;
}

bool DecidedByFirst(const Inst& inst, uint64_t first, uint64_t& result) {
	return DecidedByFirst(inst.GetOpcode(), first, result);
}

// The result of a pure operation (PureArity) from its evaluated arguments. `flags`: those of
// the instruction (Inst::Flags).
bool ApplyPure(ValueOpcode op, uint64_t flags, const std::array<uint64_t, 4>& args,
               uint64_t& result) {
	const auto [a, b, c, d] = args;
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConditionRef: result = a; return true;
		case ValueOpcode::CompositeConstructU64:
			result = static_cast<uint32_t>(a) |
			         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
			return true;
		case ValueOpcode::IAdd32: result = static_cast<uint32_t>(a + b); return true;
		case ValueOpcode::IAdd64: result = a + b; return true;
		case ValueOpcode::ISub32: result = static_cast<uint32_t>(a - b); return true;
		case ValueOpcode::ISub64: result = a - b; return true;
		case ValueOpcode::IMul32: result = static_cast<uint32_t>(a * b); return true;
		case ValueOpcode::IMul64: result = a * b; return true;
		case ValueOpcode::UMulHi:
			result = (static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint32_t>(b)) >> 32u;
			return true;
		case ValueOpcode::UMin32:
			result = std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
			return true;
		case ValueOpcode::UMax32:
			result = std::max(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
			return true;
		case ValueOpcode::ConvertF32U32:
			result = std::bit_cast<uint32_t>(static_cast<float>(static_cast<uint32_t>(a)));
			return true;
		case ValueOpcode::ConvertU32F32: {
			const auto value = Float32(a);
			if (!std::isfinite(value) || value < 0.0f || static_cast<double>(value) > UINT32_MAX) {
				return false;
			}
			result = static_cast<uint32_t>(value);
			return true;
		}
		case ValueOpcode::FPMul32:
			result = std::bit_cast<uint32_t>(Float32(a) * Float32(b));
			return true;
		case ValueOpcode::FPTrunc32:
			result = std::bit_cast<uint32_t>(std::trunc(Float32(a)));
			return true;
		case ValueOpcode::FPRecipIFlag32: {
			const auto exponent = (a >> 23u) & 0xffu;
			// Normal positive powers of two have exact normal reciprocals in every FP mode.
			if ((a & 0x807fffffu) != 0u || exponent == 0u || exponent >= 254u) return false;
			result = (254u - exponent) << 23u;
			return true;
		}
		case ValueOpcode::FPIsNan32: result = std::isnan(Float32(a)); return true;
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32: {
			const auto operand = [&](uint64_t bits) {
				FPCompareFlags compare {};
				static_assert(sizeof(compare) <= sizeof(flags));
				std::memcpy(&compare, &flags, sizeof(compare));
				if (compare.flush_input_denorms &&
				    (bits & 0x7fffffffu) < 0x00800000u) {
					bits &= 0x80000000u;
				}
				return Float32(bits);
			};
			result = op == ValueOpcode::FPOrdLessThanEqual32
			             ? operand(a) <= operand(b)
			             : operand(a) >= operand(b);
			return true;
		}
		case ValueOpcode::BitwiseAnd32: result = static_cast<uint32_t>(a & b); return true;
		case ValueOpcode::BitwiseAnd64: result = a & b; return true;
		case ValueOpcode::BitwiseOr32: result = static_cast<uint32_t>(a | b); return true;
		case ValueOpcode::BitwiseXor32: result = static_cast<uint32_t>(a ^ b); return true;
		case ValueOpcode::BitwiseNot32: result = ~static_cast<uint32_t>(a); return true;
		case ValueOpcode::ShiftLeftLogical32:
			result = static_cast<uint32_t>(a) << (b & 31u);
			return true;
		case ValueOpcode::ShiftLeftLogical64: result = a << (b & 63u); return true;
		case ValueOpcode::ShiftRightLogical32:
			result = static_cast<uint32_t>(a) >> (b & 31u);
			return true;
		case ValueOpcode::ShiftRightLogical64: result = a >> (b & 63u); return true;
		case ValueOpcode::ShiftRightArithmetic32:
			result = static_cast<uint32_t>(std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >> (b & 31u));
			return true;
		case ValueOpcode::ShiftRightArithmetic64:
			result = static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
			return true;
		case ValueOpcode::BitFieldUExtract: {
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
		case ValueOpcode::BitFieldSExtract: {
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
		case ValueOpcode::BitFieldInsert: {
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
		case ValueOpcode::IEqual32:
			result = static_cast<uint32_t>(a) == static_cast<uint32_t>(b);
			return true;
		case ValueOpcode::INotEqual32:
			result = static_cast<uint32_t>(a) != static_cast<uint32_t>(b);
			return true;
		case ValueOpcode::ULessThan32:
			result = static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
			return true;
		case ValueOpcode::ULessThanEqual32:
			result = static_cast<uint32_t>(a) <= static_cast<uint32_t>(b);
			return true;
		case ValueOpcode::UGreaterThan32:
			result = static_cast<uint32_t>(a) > static_cast<uint32_t>(b);
			return true;
		case ValueOpcode::SGreaterThanEqual32:
			result = std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >=
			         std::bit_cast<int32_t>(static_cast<uint32_t>(b));
			return true;
		case ValueOpcode::LogicalXor: result = (a != 0u) != (b != 0u); return true;
		case ValueOpcode::LogicalNot: result = a == 0u; return true;
		default: return false;
	}
}

bool ApplyPure(const Inst& inst, const std::array<uint64_t, 4>& args, uint64_t& result) {
	return ApplyPure(inst.GetOpcode(), inst.Flags<uint64_t>(), args, result);
}

uint64_t ExtractHalf(uint64_t packed, uint32_t component) {
	return static_cast<uint32_t>(packed >> (component * 32u));
}

uint64_t AddCarryHalf(uint64_t lhs, uint64_t rhs, uint32_t component) {
	const auto sum = static_cast<uint64_t>(static_cast<uint32_t>(lhs)) + static_cast<uint32_t>(rhs);
	return component == 0u ? static_cast<uint32_t>(sum) : static_cast<uint32_t>(sum >> 32u);
}

// Constant buffer and uniform buffer reads check the buffer's bounds: they need its size words.
bool IsBoundedRead(ValueOpcode op) {
	return op == ValueOpcode::ReadConstBuffer || op == ValueOpcode::LoadBufferU32;
}

// Where a raw read reads. False: the read fails. `zero`: the buffer makes the read return 0
// without a memory access. `records` and `word3` are only used by bounded reads.
bool RawReadAddress(ValueOpcode op, const MemoryInfo& mem, uint64_t low, uint64_t high,
                    uint64_t offset, uint64_t records, uint64_t word3, uint64_t& address,
                    bool& zero) {
	zero                 = false;
	const bool vector    = op == ValueOpcode::LoadBufferU32;
	const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
	const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
	if (!IsBoundedRead(op)) {
		const auto relative = (immediate & ~int64_t {3}) +
		                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
		return AddSignedAddress(base & ~uint64_t {3}, relative, address);
	}
	if (immediate < 0) {
		return false;
	}
	const auto byte_offset =
	    (static_cast<uint64_t>(immediate) & ~uint64_t {3}) + (static_cast<uint32_t>(offset) & ~3u);
	const auto stride = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
	if (vector) {
		// Only the uniform, unswizzled structured DWORD address is evaluated on the host.
		if ((high & (1u << 31u)) != 0u || (word3 & ((1u << 23u) | 0xf0000000u)) != 0u)
			return false;
		if (stride == 0u || records == 0u || ((word3 >> 12u) & 0x7fu) == 0u) {
			zero = true;
			return true;
		}
	}
	const auto size = stride == 0u
	                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
	                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
	// A read past the end of the buffer reads zero, as on the GPU.
	if (byte_offset > size || size - byte_offset < sizeof(uint32_t)) {
		zero = true;
		return true;
	}
	address = (base & ~uint64_t {3}) + byte_offset;
	return true;
}

bool ReadRawWord(const SrtRuntime& runtime, bool vector, uint64_t address, uint64_t& result) {
	uint32_t   word   = 0;
	const auto reader = vector ? runtime.read_specialization_memory : runtime.read_memory;
	if (reader != nullptr) {
		if (!reader(runtime.userdata, address, {&word, 1})) {
			return false;
		}
	} else {
		if (vector) return false;
		std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
	}
	result = word;
	return true;
}

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

namespace {

std::atomic<bool> g_srt_nodes_enabled {[] {
	const char* value = std::getenv("KYTY_SRT_NODES");
	return value == nullptr || std::strcmp(value, "0") != 0;
}()};

std::atomic<bool> g_srt_nodes_verify {std::getenv("KYTY_SRT_VERIFY") != nullptr};
thread_local bool t_srt_nodes_disabled = false;

} // namespace

void SrtNodes::SetEnabled(bool enabled) {
	g_srt_nodes_enabled.store(enabled, std::memory_order_relaxed);
}

void SrtNodes::SetThreadDisabled(bool disabled) {
	t_srt_nodes_disabled = disabled;
}

void SrtNodes::SetVerify(bool verify) {
	g_srt_nodes_verify.store(verify, std::memory_order_relaxed);
}

bool SrtNodes::Verify() {
	return g_srt_nodes_verify.load(std::memory_order_relaxed);
}

bool SrtNodes::Enabled() {
	return !t_srt_nodes_disabled && g_srt_nodes_enabled.load(std::memory_order_relaxed);
}

// Mirrors SrtWalker::EvaluateInst value by value. A value that would make the walker stop the
// emulator (a malformed operand) gives no nodes, so the walker keeps its own behavior there.
class SrtNodesBuilder {
public:
	explicit SrtNodesBuilder(const ResourcePlan& program): m_program(program) {}

	std::shared_ptr<const SrtNodes> Build() {
		const auto& program = m_program;
		for (const auto& inst: program.value_storage) {
			(void)inst.EvaluationIndex(program.evaluation_value_count);
		}
		const auto count = program.evaluation_value_count;
		if (count >= SrtNodes::ConstantBit) return nullptr;
		auto nodes = std::make_shared<SrtNodes>();
		m_nodes    = nodes.get();
		m_nodes->m_nodes.resize(count);
		m_nodes->m_insts.assign(count, nullptr);
		for (const auto& inst: program.value_storage) {
			const auto index = inst.AssignedEvaluationIndex();
			if (index >= count || m_nodes->m_insts[index] != nullptr) return nullptr;
			m_nodes->m_insts[index] = &inst;
		}
		// Every index names a value of this plan: a node never stands for a foreign value.
		if (std::ranges::find(m_nodes->m_insts, nullptr) != m_nodes->m_insts.end()) return nullptr;
		for (const auto& inst: program.value_storage) {
			if (!BuildNode(inst, m_nodes->m_nodes[inst.AssignedEvaluationIndex()])) return nullptr;
		}
		for (const auto& read: program.srt_reads) {
			// The walker resolves the read to an instruction for its clean flag.
			const auto* inst = read.value.Resolve().TryInstruction();
			uint32_t    ref  = SrtNodes::NoRef;
			if (inst == nullptr || !Ref(read.value, ref)) return nullptr;
			m_nodes->m_read_refs.push_back(ref);
			m_nodes->m_read_clean.push_back(inst->Flags<SrtReadFlags>().clean != 0u ? 1u : 0u);
		}
		m_nodes->m_descriptor_refs.resize(program.descriptor_sources.size());
		for (size_t source = 0; source < program.descriptor_sources.size(); ++source) {
			const auto& descriptor = program.descriptor_sources[source];
			auto&       refs       = m_nodes->m_descriptor_refs[source];
			refs.fill(SrtNodes::NoRef);
			if (descriptor.dword_count > refs.size()) return nullptr;
			for (uint32_t index = 0; index < descriptor.dword_count; ++index) {
				if (!Ref(descriptor.dwords[index], refs[index])) return nullptr;
			}
		}
		for (const auto& block: program.control_flow) {
			uint32_t ref = SrtNodes::NoRef;
			if (!block.condition.IsEmpty() && !Ref(block.condition, ref)) return nullptr;
			m_nodes->m_condition_refs.push_back(ref);
		}
		return nodes;
	}

private:
	using Kind = SrtNodes::Kind;

	uint32_t Constant(Value value) {
		if (const auto found = m_nodes->FindConstant(value); found != SrtNodes::NoRef) {
			return found;
		}
		SrtNodes::Constant constant {};
		constant.ok = ImmediateBits(value, constant.bits);
		m_nodes->m_constants.push_back(constant);
		m_nodes->m_constant_values.push_back(value);
		return static_cast<uint32_t>(m_nodes->m_constants.size() - 1u) | SrtNodes::ConstantBit;
	}

	// The reference SrtWalker::EvaluateWide evaluates for `value`.
	bool Ref(Value value, uint32_t& ref) {
		value = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			ref = Constant(value);
			return true;
		}
		ref = inst->AssignedEvaluationIndex();
		return ref < m_nodes->m_nodes.size() && m_nodes->m_insts[ref] == inst;
	}

	bool Args(const Inst& inst, SrtNodes::Node& node, uint32_t count) {
		if (inst.NumArgs() < count) return false;
		for (uint32_t index = 0; index < count; ++index) {
			if (!Ref(inst.Arg(index), node.args[index])) return false;
		}
		return true;
	}

	bool BuildNode(const Inst& inst, SrtNodes::Node& node) {
		const auto op = inst.GetOpcode();
		node.op       = op;
		node.flags    = inst.Flags<uint64_t>();
		if (const auto arity = PureArity(op); arity != 0u) {
			node.kind  = Kind::Pure;
			node.arity = static_cast<uint8_t>(arity);
			return Args(inst, node, arity);
		}
		switch (op) {
			case ValueOpcode::GetUserData: {
				if (inst.NumArgs() < 1u || inst.Arg(0).GetType() != Type::ScalarReg) return false;
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base) return true;
				node.kind = Kind::UserData;
				node.aux  = reg - m_program.user_data_base;
				return true;
			}
			case ValueOpcode::GetShaderBase: node.kind = Kind::ShaderBase; return true;
			case ValueOpcode::Phi: {
				const auto control = m_program.control_phi_predecessors.find(&inst);
				if (control == m_program.control_phi_predecessors.end()) {
					node.kind = Kind::InvariantPhi;
					const auto invariant =
					    ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
					return invariant.IsEmpty() || Ref(invariant, node.args[0]);
				}
				if (control->second.size() < inst.NumArgs()) return false;
				node.kind    = Kind::Phi;
				node.aux     = static_cast<uint32_t>(m_nodes->m_extra.size());
				node.args[0] = static_cast<uint32_t>(inst.NumArgs());
				for (size_t index = 0; index < inst.NumArgs(); ++index) {
					uint32_t ref = SrtNodes::NoRef;
					if (!Ref(inst.Arg(index), ref)) return false;
					m_nodes->m_extra.push_back(control->second[index]);
					m_nodes->m_extra.push_back(ref);
				}
				return true;
			}
			case ValueOpcode::ReadFirstLane: node.kind = Kind::ReadFirstLane; return Args(inst, node, 2);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: {
				if (inst.NumArgs() < 2u) return false;
				const auto index = inst.Arg(1).Resolve();
				if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
					return true;
				}
				node.aux = index.U32();
				if (op == ValueOpcode::CompositeExtractU64) {
					node.kind = Kind::ExtractU64;
					return Ref(inst.Arg(0), node.args[0]);
				}
				const auto* source = inst.Arg(0).Resolve().TryInstruction();
				if (source == nullptr) return false;
				if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
					node.kind = Kind::ExtractConstruct;
					return source->NumArgs() > node.aux && Ref(source->Arg(node.aux), node.args[0]);
				}
				if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
					node.kind = Kind::ExtractAddCarry;
					return Args(*source, node, 2);
				}
				return true;
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
			case ValueOpcode::LoadBufferU32: return BuildRawRead(inst, node);
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32:
				node.kind  = Kind::Select;
				node.arity = inst.NumArgs() == 3u ? 3u : 0u;
				return Args(inst, node, 3);
			case ValueOpcode::LogicalAnd: node.kind = Kind::LogicalAnd; return Args(inst, node, 2);
			case ValueOpcode::LogicalOr: node.kind = Kind::LogicalOr; return Args(inst, node, 2);
			default: return true;
		}
	}

	bool BuildRawRead(const Inst& inst, SrtNodes::Node& node) {
		if (!IsRawRead(m_program, inst)) return true;
		const auto op    = inst.GetOpcode();
		const auto flags = inst.Flags<SrtReadFlags>();
		node.kind        = Kind::RawRead;
		node.flags       = flags.index;
		node.arity       = static_cast<uint8_t>((flags.clean != 0u ? SrtNodes::ReadClean : 0u) |
		                                        (IsBoundedRead(op) ? SrtNodes::ReadBounded : 0u));
		if (inst.NumArgs() < 2u) return false;
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->NumArgs() < 2u) return false;
		if (op == ValueOpcode::LoadBufferU32) {
			if (inst.NumArgs() < 5u) return false;
			const auto guard = inst.Arg(4).Resolve();
			node.arity |= SrtNodes::ReadVector;
			if (guard.IsImmediate()) node.arity |= SrtNodes::ReadGuardImm;
			if (!Ref(guard, node.args[3])) return false;
		}
		if (!Ref(handle->Arg(0), node.args[0]) || !Ref(handle->Arg(1), node.args[1]) ||
		    !Ref(inst.Arg(1), node.args[2])) {
			return false;
		}
		node.aux = static_cast<uint32_t>(m_nodes->m_extra.size());
		uint32_t records = SrtNodes::NoRef;
		uint32_t word3   = SrtNodes::NoRef;
		if (handle->NumArgs() == 4u) {
			node.arity |= SrtNodes::ReadHandleBounds;
			if (!Ref(handle->Arg(2), records) || !Ref(handle->Arg(3), word3)) return false;
		}
		m_nodes->m_extra.push_back(records);
		m_nodes->m_extra.push_back(word3);
		return true;
	}

	const ResourcePlan& m_program;
	SrtNodes*           m_nodes = nullptr;
};

uint32_t SrtNodes::FindConstant(Value value) const {
	for (size_t index = 0; index < m_constant_values.size(); ++index) {
		if (m_constant_values[index] == value) {
			return static_cast<uint32_t>(index) | ConstantBit;
		}
	}
	return NoRef;
}

std::shared_ptr<const SrtNodes> SrtNodes::Build(const ResourcePlan& program) {
	return SrtNodesBuilder(program).Build();
}

SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     SrtWalker* clean_evaluator)
    : m_program(program), m_runtime(runtime), m_clean_evaluator(clean_evaluator),
      m_session(this),
      m_read_policy(clean_evaluator != nullptr ? 2u : 0u), m_context(AcquireContext()) {
	// A walker delegates to its clean evaluator by node: both use nodes, or neither does.
	m_nodes = clean_evaluator != nullptr ? clean_evaluator->m_nodes
	          : SrtNodes::Enabled()      ? program.srt_nodes.get()
	                                     : nullptr;
}

SrtWalker::SrtWalker(SrtWalker& parent, Value active_mask, bool clean,
                     SrtWalker* clean_evaluator, uint32_t active_ref)
    : m_program(parent.m_program), m_runtime(clean ? CleanRuntime(parent.m_runtime) : parent.m_runtime),
      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()),
      m_reachable_blocks(parent.m_reachable_blocks), m_session(parent.m_session),
      m_read_policy((parent.m_read_policy & 1u) | (clean ? 1u : 0u) |
                    (clean_evaluator != nullptr ? 2u : 0u)), m_context(AcquireContext()) {
	m_nodes      = parent.m_nodes;
	m_active_ref = active_ref != SrtNodes::NoRef ? active_ref : RefOf(m_active_mask);
}

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
	context.unknowns.clear();
	return context;
}

uint32_t SrtWalker::RefOf(Value value) const {
	if (m_nodes == nullptr) return SrtNodes::NoRef;
	value = value.Resolve();
	if (const auto* inst = value.TryInstruction()) {
		const auto index = inst->AssignedEvaluationIndex();
		return index < m_nodes->m_nodes.size() && m_nodes->m_insts[index] == inst ? index
		                                                                         : SrtNodes::NoRef;
	}
	if (value.IsEmpty()) return SrtNodes::NoRef;
	return m_nodes->FindConstant(value);
}

Value SrtWalker::ValueOf(uint32_t ref) const {
	if (ref == SrtNodes::NoRef) return {};
	if ((ref & SrtNodes::ConstantBit) != 0u) {
		return m_nodes->m_constant_values[ref & ~SrtNodes::ConstantBit];
	}
	return Value(const_cast<Inst*>(m_nodes->m_insts[ref]));
}

bool SrtWalker::EvaluateWide(Value value, uint64_t& result) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		return ImmediateBits(value, result);
	}
	if (m_nodes != nullptr) {
		const auto* node_inst = value.TryInstruction();
		const auto  index     = node_inst->AssignedEvaluationIndex();
		if (index < m_nodes->m_nodes.size() && m_nodes->m_insts[index] == node_inst) {
			return EvaluateRef(index, result);
		}
	}
	if (!m_active_mask.IsEmpty() && value == m_active_mask) {
		result = 1u;
		return true;
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
	// Visiting and completed-unknown entries both stop repeated evaluation in this
	// context. Only completed unknowns are retried when reachability becomes tighter.
	if (m_context.values[index].generation == (m_context.generation | 1u) ||
	    m_context.values[index].generation == (m_context.generation | 2u)) {
		return false;
	}
	m_context.values[index].generation = m_context.generation | 1u;
	uint64_t out = 0;
	const bool evaluated = EvaluateInst(*inst, out);
	// Recursive evaluation may grow the dense memo vector.
	auto& memo = m_context.values[index];
	if (!evaluated) {
		memo.generation = m_context.generation | 2u;
		m_context.unknowns.push_back(index);
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

void SrtWalker::ResetUnknowns() {
	for (auto& context: m_program.evaluation_contexts) {
		if (context.session != m_session) continue;
		for (const auto index: context.unknowns)
			if (context.values[index].generation == (context.generation | 2u))
				context.values[index].generation = 0;
		context.unknowns.clear();
	}
}

bool SrtWalker::EvaluatePhi(const Inst& inst, uint64_t& result) {
	if (const auto control = m_program.control_phi_predecessors.find(&inst);
	    control != m_program.control_phi_predecessors.end()) {
		bool found = false;
		for (size_t index = 0; index < inst.NumArgs(); ++index) {
			if (m_reachable_blocks != nullptr &&
			    !m_reachable_blocks->at(control->second[index])) continue;
			uint64_t incoming = 0;
			if (!Arg(inst, index, incoming) || (found && incoming != result)) return false;
			result = incoming;
			found = true;
		}
		return found;
	}
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
		result = ExtractHalf(packed, component);
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
		result = AddCarryHalf(lhs, rhs, component);
		return true;
	}
	return false;
}

bool SrtWalker::EvaluateRawRead(const Inst& inst, uint64_t& result) {
	const auto flags = inst.Flags<SrtReadFlags>();
	if (flags.index >= m_program.memory_info.size()) {
		return false;
	}
	const auto& mem    = m_program.memory_info[flags.index];
	const bool vector = inst.GetOpcode() == ValueOpcode::LoadBufferU32;
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
		return false;
	}
	uint64_t low    = 0;
	uint64_t high   = 0;
	uint64_t offset = 0;
	if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
		return false;
	}
	uint64_t records = 0;
	uint64_t word3   = 0;
	if (IsBoundedRead(inst.GetOpcode()) &&
	    (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3))) {
		return false;
	}
	uint64_t address = 0;
	bool     zero    = false;
	if (!RawReadAddress(inst.GetOpcode(), mem, low, high, offset, records, word3, address, zero)) {
		return false;
	}
	if (zero) {
		result = 0u;
		return true;
	}
	return ReadRawWord(m_runtime, vector, address, result);
}

bool SrtWalker::EvaluateInst(const Inst& inst, uint64_t& result) {
	if (const auto arity = PureArity(inst.GetOpcode()); arity != 0u) {
		std::array<uint64_t, 4> args {};
		for (uint32_t index = 0; index < arity; ++index) {
			if (!Arg(inst, index, args[index])) {
				return false;
			}
			if (index == 0u && DecidedByFirst(inst, args[0], result)) {
				return true;
			}
		}
		return ApplyPure(inst, args, result);
	}
	uint64_t a = 0;
	uint64_t b = 0;
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
			if (!m_active_mask.IsEmpty() && inst.Arg(1).Resolve() == m_active_mask) {
				return EvaluateWide(inst.Arg(0), result);
			}
			SrtWalker clean_active(*this, inst.Arg(1), true);
			SrtWalker active(*this, inst.Arg(1), false, &clean_active);
			return active.EvaluateWide(inst.Arg(0), result);
		}
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU32:
			if (IsRawRead(m_program, inst)) {
				if (m_clean_evaluator != nullptr &&
				    (inst.Flags<SrtReadFlags>().clean != 0u ||
				     (inst.GetOpcode() == ValueOpcode::LoadBufferU32 &&
				      m_clean_evaluator->m_active_mask == m_active_mask))) {
					return m_clean_evaluator->EvaluateWide(Value(const_cast<Inst*>(&inst)), result);
				}
				return EvaluateRawRead(inst, result);
			}
			break;
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectF32: {
			auto& predicate = m_clean_evaluator != nullptr ? *m_clean_evaluator : *this;
			if (predicate.EvaluateWide(inst.Arg(0), a)) {
				return Arg(inst, a != 0u ? 1u : 2u, result);
			}
			return false;
		}
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
		case ValueOpcode::UndefU1:
		case ValueOpcode::UndefU8:
		case ValueOpcode::UndefU16:
		case ValueOpcode::UndefU32:
		case ValueOpcode::UndefU64: return false;
		default: break;
	}
	return false;
}
bool SrtWalker::EvaluateRef(uint32_t ref, uint64_t& result) {
	using Kind = SrtNodes::Kind;
	if ((ref & SrtNodes::ConstantBit) != 0u) {
		if (ref == SrtNodes::NoRef) return false;
		const auto& constant = m_nodes->m_constants[ref & ~SrtNodes::ConstantBit];
		if (!constant.ok) return false;
		result = constant.bits;
		return true;
	}
	if (ref == m_active_ref) {
		result = 1u;
		return true;
	}
	const auto& node = m_nodes->m_nodes[ref];
	if (m_active_ref != SrtNodes::NoRef && node.kind == Kind::Select && node.arity == 3u &&
	    node.args[0] == m_active_ref) {
		return EvaluateRef(node.args[1], result);
	}
	if (ref >= m_context.values.size()) {
		m_context.values.resize(m_program.evaluation_value_count);
	}
	if (m_context.values[ref].generation == m_context.generation) {
		result = m_context.values[ref].value;
		return true;
	}
	if (m_context.values[ref].generation == (m_context.generation | 1u) ||
	    m_context.values[ref].generation == (m_context.generation | 2u)) {
		return false;
	}
	m_context.values[ref].generation = m_context.generation | 1u;
	uint64_t   out       = 0;
	const bool evaluated = EvaluateNode(ref, out);
	auto&      memo      = m_context.values[ref];
	if (!evaluated) {
		memo.generation = m_context.generation | 2u;
		m_context.unknowns.push_back(ref);
		return false;
	}
	memo.value      = out;
	memo.generation = m_context.generation;
	result          = out;
	return true;
}

bool SrtWalker::EvaluateNode(uint32_t index, uint64_t& result) {
	using Kind       = SrtNodes::Kind;
	const auto& node = m_nodes->m_nodes[index];
	uint64_t    a    = 0;
	uint64_t    b    = 0;
	switch (node.kind) {
		case Kind::Fail: return false;
		case Kind::Pure: {
			std::array<uint64_t, 4> args {};
			for (uint32_t arg = 0; arg < node.arity; ++arg) {
				if (!EvaluateRef(node.args[arg], args[arg])) return false;
				if (arg == 0u && DecidedByFirst(node.op, args[0], result)) return true;
			}
			return ApplyPure(node.op, node.flags, args, result);
		}
		case Kind::UserData:
			if (node.aux >= m_runtime.user_data.size()) return false;
			result = m_runtime.user_data[node.aux];
			return true;
		case Kind::ShaderBase: result = m_runtime.shader_base; return true;
		case Kind::Phi: {
			bool found = false;
			for (uint32_t arg = 0; arg < node.args[0]; ++arg) {
				const auto predecessor = m_nodes->m_extra[node.aux + 2u * arg];
				if (m_reachable_blocks != nullptr && !m_reachable_blocks->at(predecessor)) continue;
				uint64_t incoming = 0;
				if (!EvaluateRef(m_nodes->m_extra[node.aux + 2u * arg + 1u], incoming) ||
				    (found && incoming != result)) {
					return false;
				}
				result = incoming;
				found  = true;
			}
			return found;
		}
		case Kind::InvariantPhi: return EvaluateRef(node.args[0], result);
		case Kind::ReadFirstLane: {
			if (m_active_ref != SrtNodes::NoRef && node.args[1] == m_active_ref) {
				return EvaluateRef(node.args[0], result);
			}
			const auto mask = ValueOf(node.args[1]);
			SrtWalker  clean_active(*this, mask, true, nullptr, node.args[1]);
			SrtWalker  active(*this, mask, false, &clean_active, node.args[1]);
			return active.EvaluateRef(node.args[0], result);
		}
		case Kind::ExtractU64:
			if (!EvaluateRef(node.args[0], a)) return false;
			result = ExtractHalf(a, node.aux);
			return true;
		case Kind::ExtractConstruct: return EvaluateRef(node.args[0], result);
		case Kind::ExtractAddCarry:
			if (!EvaluateRef(node.args[0], a) || !EvaluateRef(node.args[1], b)) return false;
			result = AddCarryHalf(a, b, node.aux);
			return true;
		case Kind::RawRead:
			if (m_clean_evaluator != nullptr &&
			    ((node.arity & SrtNodes::ReadClean) != 0u ||
			     ((node.arity & SrtNodes::ReadVector) != 0u &&
			      m_clean_evaluator->m_active_mask == m_active_mask))) {
				return m_clean_evaluator->EvaluateRef(index, result);
			}
			return EvaluateNodeRawRead(node, result);
		case Kind::Select: {
			auto& predicate = m_clean_evaluator != nullptr ? *m_clean_evaluator : *this;
			if (predicate.EvaluateRef(node.args[0], a)) {
				return EvaluateRef(node.args[a != 0u ? 1u : 2u], result);
			}
			return false;
		}
		case Kind::LogicalAnd: {
			const bool left = EvaluateRef(node.args[0], a);
			if (left && a == 0u) {
				result = 0u;
				return true;
			}
			if (EvaluateRef(node.args[1], b) && (b == 0u || left)) {
				result = b != 0u;
				return true;
			}
			if (!left) {
				SrtWalker active(m_clean_evaluator != nullptr ? *m_clean_evaluator : *this,
				                 ValueOf(node.args[0]), false, nullptr, node.args[0]);
				if (active.EvaluateRef(node.args[1], b) && b == 0u) {
					result = 0u;
					return true;
				}
			}
			return false;
		}
		case Kind::LogicalOr: {
			const bool left = EvaluateRef(node.args[0], a);
			if (left && a != 0u) {
				result = 1u;
				return true;
			}
			if (!EvaluateRef(node.args[1], b) || (b == 0u && !left)) return false;
			result = b != 0u;
			return true;
		}
	}
	return false;
}

bool SrtWalker::EvaluateNodeRawRead(const SrtNodes::Node& node, uint64_t& result) {
	if (node.flags >= m_program.memory_info.size()) {
		return false;
	}
	const auto& mem    = m_program.memory_info[node.flags];
	const bool  vector = (node.arity & SrtNodes::ReadVector) != 0u;
	if (vector && ((node.arity & SrtNodes::ReadGuardImm) != 0u || node.args[3] != m_active_ref)) {
		uint64_t enabled = 0;
		if (!EvaluateRef(node.args[3], enabled)) return false;
		if (enabled == 0u) {
			result = 0u;
			return true;
		}
	}
	uint64_t low    = 0;
	uint64_t high   = 0;
	uint64_t offset = 0;
	if (!EvaluateRef(node.args[0], low) || !EvaluateRef(node.args[1], high) ||
	    !EvaluateRef(node.args[2], offset)) {
		return false;
	}
	uint64_t records = 0;
	uint64_t word3   = 0;
	if ((node.arity & SrtNodes::ReadBounded) != 0u &&
	    ((node.arity & SrtNodes::ReadHandleBounds) == 0u ||
	     !EvaluateRef(m_nodes->m_extra[node.aux], records) ||
	     !EvaluateRef(m_nodes->m_extra[node.aux + 1u], word3))) {
		return false;
	}
	uint64_t address = 0;
	bool     zero    = false;
	if (!RawReadAddress(node.op, mem, low, high, offset, records, word3, address, zero)) {
		return false;
	}
	if (zero) {
		result = 0u;
		return true;
	}
	return ReadRawWord(m_runtime, vector, address, result);
}

bool SrtWalker::EvaluateDescriptor(uint32_t source, DescriptorValue& result) {
	if (source >= m_program.descriptor_sources.size()) {
		return false;
	}
	const auto& descriptor = m_program.descriptor_sources[source];
	result = {};
	result.dword_count = descriptor.dword_count;
	for (uint32_t index = 0; index < descriptor.dword_count; ++index) {
		// A root added after the nodes were built (a test edits a plan) is walked by value.
		if (m_nodes != nullptr && source < m_nodes->m_descriptor_refs.size()) {
			uint64_t wide = 0;
			if (!EvaluateRef(m_nodes->m_descriptor_refs[source][index], wide)) return false;
			result.dwords[index] = static_cast<uint32_t>(wide);
		} else if (!Evaluate(descriptor.dwords[index], result.dwords[index])) {
			return false;
		}
	}
	return true;
}

bool SrtWalker::RefreshFlatBuffer(std::vector<uint32_t>& flat) {
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
	const auto refresh = [&](uint32_t slot) {
		if (slot >= m_program.srt_reads.size()) return false;
		const auto& read = m_program.srt_reads[slot];
		if (m_nodes != nullptr && slot < m_nodes->m_read_refs.size() &&
		    (m_clean_evaluator == nullptr || m_clean_evaluator->m_nodes != nullptr)) {
			const bool clean = m_nodes->m_read_clean[slot] != 0u;
			if (clean && (m_clean_evaluator == nullptr || m_runtime.read_specialization_memory == nullptr))
				return false;
			auto&    evaluator = clean ? *m_clean_evaluator : *this;
			uint64_t wide      = 0;
			if (read.flat_offset >= flat.size() || !evaluator.EvaluateRef(m_nodes->m_read_refs[slot], wide))
				return false;
			flat[read.flat_offset] = static_cast<uint32_t>(wide);
			return true;
		}
		const auto* inst = read.value.ResolveInstruction();
		const bool clean = inst != nullptr && inst->Flags<SrtReadFlags>().clean != 0u;
		if (clean && (m_clean_evaluator == nullptr || m_runtime.read_specialization_memory == nullptr))
			return false;
		auto& evaluator = clean ? *m_clean_evaluator : *this;
		return read.flat_offset < flat.size() && evaluator.Evaluate(read.value, flat[read.flat_offset]);
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
			const auto evaluate_condition = [&] {
				if (m_nodes == nullptr || predicate.m_nodes == nullptr ||
				    index >= m_nodes->m_condition_refs.size())
					return predicate.Evaluate(block.condition, condition);
				uint64_t wide = 0;
				if (!predicate.EvaluateRef(m_nodes->m_condition_refs[index], wide)) return false;
				condition = static_cast<uint32_t>(wide);
				return true;
			};
			if (!block.condition.IsEmpty() &&
			    (m_runtime.read_specialization_memory != nullptr || !m_program.capture_specialization_reads) &&
			    evaluate_condition()) {
				pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
			} else {
				// One or two successors: an insert costs a library copy per block.
				for (const auto successor: block.successors) pending.push_back(successor);
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


// Follows the walk MaterializeResources makes, from the same roots, and records one step per
// value. A plan for which the walker could take another path at runtime gets no tape.
class SrtTapeBuilder {
public:
	explicit SrtTapeBuilder(const ResourcePlan& program): m_program(program) {}

	std::shared_ptr<const SrtTape> Build() {
		const auto& program = m_program;
		// The walker of these plans has no clean evaluator and refreshes every SRT read.
		if (!program.srt_plan_complete || !program.resource_tracking_complete ||
		    program.capture_specialization_reads || program.requires_specialization_memory ||
		    !program.control_flow.empty() || program.uniform_fill.fill.words != 0u) {
			return nullptr;
		}
		auto tape = std::make_shared<SrtTape>();
		m_tape    = tape.get();
		for (const auto& read: program.srt_reads) {
			const auto* inst = read.value.Resolve().TryInstruction();
			uint32_t    slot = SrtTape::NoSlot;
			// A clean root needs the clean evaluator that these plans don't have.
			if (inst == nullptr || inst->Flags<SrtReadFlags>().clean != 0u ||
			    read.flat_offset >= program.srt_reads.size() || !Slot(read.value, slot)) {
				return nullptr;
			}
			tape->m_srt_slots.push_back(slot);
		}
		tape->m_descriptor_slots.resize(program.descriptor_sources.size());
		for (auto& slots: tape->m_descriptor_slots) {
			slots.fill(SrtTape::NoSlot);
		}
		for (const auto& buffer: program.info.buffers) {
			if (!AddDescriptor(buffer.source)) return nullptr;
		}
		for (const auto& image: program.info.images) {
			if (!AddDescriptor(image.source)) return nullptr;
		}
		for (const auto& sampler: program.info.samplers) {
			if (!AddDescriptor(sampler.source)) return nullptr;
		}
		tape->m_ok.resize(tape->m_values.size(), 0u);
		for (const auto& [value, slot]: m_constants) {
			tape->m_ok[slot] = 1u;
		}
		return tape;
	}

private:
	bool AddDescriptor(uint32_t source) {
		if (source >= m_program.descriptor_sources.size()) return false;
		const auto& descriptor = m_program.descriptor_sources[source];
		if (descriptor.indirect_descriptor.has_value() || descriptor.dword_count > 8u) return false;
		auto& slots = m_tape->m_descriptor_slots[source];
		for (uint32_t index = 0; index < descriptor.dword_count; ++index) {
			if (!Slot(descriptor.dwords[index], slots[index])) return false;
		}
		return true;
	}

	uint32_t NewSlot() {
		m_tape->m_values.push_back(0);
		return static_cast<uint32_t>(m_tape->m_values.size() - 1u);
	}

	uint32_t Constant(uint64_t value) {
		const auto [found, added] = m_constants.try_emplace(value, 0u);
		if (added) {
			found->second                  = NewSlot();
			m_tape->m_values[found->second] = value;
		}
		return found->second;
	}

	bool Step(SrtTape::StepKind kind, const Inst& inst, std::span<const Value> args,
	          uint32_t operand, uint32_t& slot) {
		// A raw read keeps its memory index in `flags` (SrtReadFlags::index).
		SrtTape::Step step {.flags   = kind == SrtTape::StepKind::RawRead
		                                   ? inst.Flags<SrtReadFlags>().index
		                                   : inst.Flags<uint64_t>(),
		                    .op      = inst.GetOpcode(),
		                    .kind    = kind,
		                    .operand = operand};
		for (const auto arg: args) {
			if (!Slot(arg, step.args[step.arg_count])) return false;
			step.arg_count++;
		}
		step.out = slot = NewSlot();
		m_tape->m_steps.push_back(step);
		return true;
	}

	// The slot of a value, as SrtWalker::EvaluateWide finds it. False: no tape.
	bool Slot(Value value, uint32_t& slot) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			uint64_t bits = 0;
			if (!ImmediateBits(value, bits)) return false;
			slot = Constant(bits);
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (const auto found = m_slots.find(inst); found != m_slots.end()) {
			slot = found->second;
			return true;
		}
		if (!m_visiting.insert(inst).second) return false;
		const bool built = BuildStep(*inst, slot);
		m_visiting.erase(inst);
		if (built) m_slots.emplace(inst, slot);
		return built;
	}

	// Mirrors SrtWalker::EvaluateInst for a walker without a clean evaluator or EXEC mask.
	bool BuildStep(const Inst& inst, uint32_t& slot) {
		using Kind    = SrtTape::StepKind;
		const auto op = inst.GetOpcode();
		if (const auto arity = PureArity(op); arity != 0u) {
			std::array<Value, 4> args {};
			for (uint32_t index = 0; index < arity; ++index) {
				args[index] = inst.Arg(index);
			}
			return Step(Kind::Pure, inst, std::span(args).first(arity), 0u, slot);
		}
		switch (op) {
			case ValueOpcode::GetUserData: {
				if (inst.Arg(0).GetType() != Type::ScalarReg) return false;
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base) return false;
				return Step(Kind::UserData, inst, {}, reg - m_program.user_data_base, slot);
			}
			case ValueOpcode::GetShaderBase: return Step(Kind::ShaderBase, inst, {}, 0u, slot);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: {
				const auto index = inst.Arg(1).Resolve();
				if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
					return false;
				}
				const auto component = index.U32();
				if (op == ValueOpcode::CompositeExtractU64) {
					const std::array args {inst.Arg(0)};
					return Step(Kind::ExtractHalf, inst, args, component, slot);
				}
				const auto* source = inst.Arg(0).Resolve().TryInstruction();
				if (source == nullptr) return false;
				if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
					// The walker evaluates the chosen part itself: the extract shares its slot.
					return Slot(source->Arg(component), slot);
				}
				if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
					const std::array args {source->Arg(0), source->Arg(1)};
					return Step(Kind::AddCarryHalf, inst, args, component, slot);
				}
				return false;
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer: {
				// A guarded uniform buffer read (LoadBufferU32) needs the strict reader: no tape.
				if (!IsRawRead(m_program, inst) ||
				    inst.Flags<SrtReadFlags>().index >= m_program.memory_info.size()) {
					return false;
				}
				const auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr) return false;
				if (!IsBoundedRead(op)) {
					const std::array args {handle->Arg(0), handle->Arg(1), inst.Arg(1)};
					return Step(Kind::RawRead, inst, args, 0u, slot);
				}
				if (handle->NumArgs() != 4u) return false;
				const std::array args {handle->Arg(0), handle->Arg(1), inst.Arg(1), handle->Arg(2),
				                       handle->Arg(3)};
				return Step(Kind::RawRead, inst, args, 0u, slot);
			}
			default: return false;
		}
	}

	const ResourcePlan&                            m_program;
	SrtTape*                                       m_tape = nullptr;
	std::unordered_map<const Inst*, uint32_t>      m_slots;
	std::unordered_set<const Inst*>                m_visiting;
	std::unordered_map<uint64_t, uint32_t>         m_constants;
};

std::shared_ptr<const SrtTape> SrtTape::Build(const ResourcePlan& program) {
	return SrtTapeBuilder(program).Build();
}

void SrtTape::Run(const ResourcePlan& program, const SrtRuntime& runtime) const {
	for (const auto& step: m_steps) {
		std::array<uint64_t, 5> args {};
		bool                    ok = true;
		for (uint32_t index = 0; index < step.arg_count && ok; ++index) {
			ok          = m_ok[step.args[index]] != 0u;
			args[index] = m_values[step.args[index]];
		}
		uint64_t out = 0;
		// As the walker: a pure operation that its first argument decides needs no other.
		if (step.kind == StepKind::Pure && step.arg_count != 0u && m_ok[step.args[0]] != 0u &&
		    DecidedByFirst(step.op, args[0], out)) {
			m_values[step.out] = out;
			m_ok[step.out]     = 1u;
			continue;
		}
		if (ok) {
			switch (step.kind) {
				case StepKind::UserData:
					ok = step.operand < runtime.user_data.size();
					if (ok) out = runtime.user_data[step.operand];
					break;
				case StepKind::ShaderBase: out = runtime.shader_base; break;
				case StepKind::Pure:
					ok = ApplyPure(step.op, step.flags, {args[0], args[1], args[2], args[3]}, out);
					break;
				case StepKind::ExtractHalf: out = ExtractHalf(args[0], step.operand); break;
				case StepKind::AddCarryHalf:
					out = AddCarryHalf(args[0], args[1], step.operand);
					break;
				case StepKind::RawRead: {
					const auto& mem = program.memory_info[step.flags];
					uint64_t    address = 0;
					bool        zero    = false;
					ok = RawReadAddress(step.op, mem, args[0], args[1], args[2],
					                    args[3], args[4], address, zero) &&
					     (zero || ReadRawWord(runtime, false, address, out));
					break;
				}
			}
		}
		m_values[step.out] = ok ? out : 0u;
		m_ok[step.out]     = ok ? 1u : 0u;
	}
}

bool SrtTape::Root(uint32_t slot, uint32_t& value) const {
	if (slot == NoSlot || m_ok[slot] == 0u) return false;
	value = static_cast<uint32_t>(m_values[slot]);
	return true;
}

bool SrtTape::FlatBuffer(const ResourcePlan& program, std::vector<uint32_t>& flat) const {
	flat.resize(program.srt_reads.size());
	for (size_t index = 0; index < m_srt_slots.size(); ++index) {
		if (!Root(m_srt_slots[index], flat[program.srt_reads[index].flat_offset])) return false;
	}
	return true;
}

bool SrtTape::Descriptor(const ResourcePlan& program, uint32_t source,
                         DescriptorValue& result) const {
	if (source >= m_descriptor_slots.size()) {
		return false;
	}
	result             = {};
	result.dword_count = program.descriptor_sources[source].dword_count;
	for (uint32_t index = 0; index < result.dword_count; ++index) {
		if (!Root(m_descriptor_slots[source][index], result.dwords[index])) return false;
	}
	return true;
}


} // namespace Libs::Graphics::ShaderRecompiler::IR
