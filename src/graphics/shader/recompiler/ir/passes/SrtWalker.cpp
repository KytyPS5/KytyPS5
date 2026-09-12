#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/abi.h"
#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <xbyak/xbyak.h>

#include <algorithm>
#include <bit>
#include <cmath>
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

// A vector-path buffer load whose address carries no lane identity: no index, no
// per-lane offset, one plain dword. Hardware reaches it as base + soffset + imm, which
// is the same shape the scalar reads below already re-execute, so the host can
// reproduce it. The gate used to refuse every MUBUF load on its resource kind alone,
// which dropped whole shaders whose only sin was fetching one uniform word through the
// vector path - see the two decoupled-lookback scan passes in SILENT HILL 2.
bool IsUniformBufferRead(const ResourcePlan& values, const Inst& inst) {
	if (inst.GetOpcode() != ValueOpcode::LoadBufferU32 || inst.NumArgs() != 5) {
		return false;
	}
	const auto flags = inst.Flags<MemoryFlags>();
	if (flags.index >= values.memory_info.size()) {
		return false;
	}
	const auto& mem = values.memory_info[flags.index];
	if (mem.kind != ResourceKind::Buffer || mem.idxen || mem.offen || mem.typed ||
	    mem.formatted || mem.data_bits != 32u || mem.data_dwords != 1u) {
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
	return op == ValueOpcode::LoadBufferU32 ? 3u : 1u;
}

// A MUBUF load carries the exec mask as its last operand. That is lane state, not part
// of the address, and walking it would reject the read for depending on lane identity.
size_t RawReadAddressArgs(const Inst& inst) {
	return inst.GetOpcode() == ValueOpcode::LoadBufferU32 ? 4u : inst.NumArgs();
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
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type)
	    : m_program(program), m_type(type) {}

	bool Run(Value value) { return Validate(value); }

private:
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
			return !require_uniform;
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
		if (op == ValueOpcode::Phi) {
			if (m_type != RuntimeValueType::Any && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				return finish(false);
			}
			return finish(Validate(entry));
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

SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     SrtWalker* clean_evaluator, Value active_mask)
    : m_program(program), m_runtime(runtime), m_clean_evaluator(clean_evaluator),
      m_active_mask(active_mask.Resolve()),
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
			return false;
		}
	}
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
	if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) ||
	    !Arg(inst, RawReadOffsetArg(inst.GetOpcode()), offset)) {
		return false;
	}
	const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
	const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
	uint64_t   address   = 0;
	if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer ||
	    inst.GetOpcode() == ValueOpcode::LoadBufferU32) {
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
				return false;
			}
			address = (base & ~uint64_t {3}) + byte_offset;
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
		case ValueOpcode::UMulHi:
			if (binary()) {
				result = (static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint32_t>(b)) >> 32u;
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
		case ValueOpcode::ULessThanEqual32:
			if (binary()) {
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
		case ValueOpcode::SGreaterThanEqual32:
			if (binary()) {
				result = std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >=
				         std::bit_cast<int32_t>(static_cast<uint32_t>(b));
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
		if (!block.condition.IsEmpty() &&
		    (m_runtime.read_specialization_memory != nullptr || !m_program.capture_specialization_reads) &&
		    predicate.Evaluate(block.condition, condition)) {
			pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
		} else {
			pending.insert(pending.end(), block.successors.begin(), block.successors.end());
		}
	}
	return true;
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}


} // namespace Libs::Graphics::ShaderRecompiler::IR
