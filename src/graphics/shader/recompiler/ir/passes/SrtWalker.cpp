#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/abi.h"
#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <xbyak/xbyak.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
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
	// As with ordinary reads, paths the GPU does not take are evaluated too, and a pointer there
	// can be stale or garbage. Memory that is not mapped cannot be what the GPU reads: it reads
	// as zero, and specialization does not depend on it.
	if (capture.m_source.is_mapped != nullptr &&
	    !capture.m_source.is_mapped(address, values.size_bytes())) {
		std::ranges::fill(values, 0u);
		return true;
	}
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
	} else if (capture.m_source.is_mapped != nullptr &&
	           !capture.m_source.is_mapped(address, values.size_bytes())) {
		// Reads are evaluated on every path, including ones the GPU does not take with this data;
		// a pointer there can be stale or garbage. No mapped memory reads as zero.
		std::ranges::fill(values, 0u);
		return true;
	} else if (values.size() == 1u) {
		// Most reads are one DWORD; a constant-size copy is a plain load instead of a call.
		std::memcpy(values.data(), reinterpret_cast<const void*>(address), sizeof(uint32_t));
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
	RuntimeValidator(const ResourcePlan& program, RuntimeValueType type, Value counter = {})
	    : m_program(program), m_type(type), m_counter(counter.Resolve()) {}

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
		if (!m_counter.IsEmpty() && value == m_counter) return true;
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
	Value                           m_counter;
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

void SrtWalker::Bind(Value value, uint64_t result) {
	auto* inst = value.Resolve().TryInstruction();
	EXIT_IF(inst == nullptr);
	const auto index = inst->EvaluationIndex(m_program.evaluation_value_count);
	if (index >= m_context.values.size()) {
		m_context.values.resize(m_program.evaluation_value_count);
	}
	m_context.values[index] = {result, m_context.generation};
	m_memo_used = true;
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
	// The low generation bit marks an instruction that is still being evaluated.
	if (m_context.values[index].generation == (m_context.generation | 1u)) {
		return false;
	}
	m_context.values[index].generation = m_context.generation | 1u;
	m_memo_used = true;
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
	// The invariant value is a property of the plan, which does not change; resolving it walks the
	// phi web and allocates on every evaluation.
	auto [found, inserted] = m_program.invariant_phis.try_emplace(&inst);
	if (inserted) {
		found->second = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
	}
	const auto value = found->second;
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

// The address of a scalar raw read (s_load from an address, or s_buffer_load from a V#), from
// its evaluated operands. Const-buffer reads are bounded by the V#.
static bool ScalarReadAddress(bool const_buffer, uint32_t memory_offset, uint64_t low, uint64_t high,
                              uint64_t records, uint64_t offset, uint64_t& address) {
	const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
	const auto immediate = static_cast<int64_t>(static_cast<int32_t>(memory_offset));
	if (const_buffer) {
		if (immediate < 0) {
			return false;
		}
		const auto byte_offset =
		    (static_cast<uint64_t>(immediate) & ~uint64_t {3}) + (static_cast<uint32_t>(offset) & ~3u);
		const auto stride = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
		const auto size   = stride == 0u
		                        ? static_cast<uint64_t>(static_cast<uint32_t>(records))
		                        : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
		if (byte_offset > size || size - byte_offset < sizeof(uint32_t)) {
			return false;
		}
		address = (base & ~uint64_t {3}) + byte_offset;
		return true;
	}
	const auto relative =
	    (immediate & ~int64_t {3}) + static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
	return AddSignedAddress(base & ~uint64_t {3}, relative, address);
}

// A scalar read through the runtime's ordinary reader; without one, unmapped memory reads as zero.
static bool ReadScalarWord(const SrtRuntime& runtime, uint64_t address, uint32_t& word) {
	word = 0;
	if (runtime.read_memory != nullptr) {
		return runtime.read_memory(runtime.userdata, address, {&word, 1});
	}
	// As in SrtReadCapture::ReadOrdinary: a pointer on a path the GPU does not take can be
	// garbage, and no mapped memory reads as zero.
	if (runtime.is_mapped == nullptr || runtime.is_mapped(address, sizeof(word))) {
		std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
	}
	return true;
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
	if (!vector) {
		const bool const_buffer = inst.GetOpcode() == ValueOpcode::ReadConstBuffer;
		uint64_t   records      = 0;
		uint64_t   word3        = 0;
		uint64_t   address      = 0;
		uint32_t   word         = 0;
		if ((const_buffer && (handle->NumArgs() != 4u || !Arg(*handle, 2, records) ||
		                      !Arg(*handle, 3, word3))) ||
		    !ScalarReadAddress(const_buffer, mem.offset, low, high, records, offset, address) ||
		    !ReadScalarWord(m_runtime, address, word)) {
			return false;
		}
		result = word;
		return true;
	}
	// A structured vector DWORD: only its uniform, unswizzled address is evaluated on the host.
	uint64_t records = 0;
	uint64_t word3   = 0;
	if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3) ||
	    static_cast<int32_t>(mem.offset) < 0) {
		return false;
	}
	if ((high & (1u << 31u)) != 0u || (word3 & ((1u << 23u) | 0xf0000000u)) != 0u) {
		return false;
	}
	const auto stride = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
	if (stride == 0u || records == 0u || ((word3 >> 12u) & 0x7fu) == 0u) {
		result = 0u;
		return true;
	}
	uint64_t address = 0;
	uint32_t word    = 0;
	if (!ScalarReadAddress(true, mem.offset, low, high, records, offset, address) ||
	    m_runtime.read_specialization_memory == nullptr ||
	    !m_runtime.read_specialization_memory(m_runtime.userdata, address, {&word, 1})) {
		return false;
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
			if (!m_active_mask.IsEmpty() && inst.Arg(1).Resolve() == m_active_mask) {
				return EvaluateWide(inst.Arg(0), result);
			}
			const auto clean_runtime = CleanRuntime(m_runtime);
			SrtWalker  clean_active(m_program, clean_runtime, nullptr, inst.Arg(1));
			SrtWalker  active(m_program, m_runtime, &clean_active, inst.Arg(1));
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
// The walker evaluates a plan's value graph recursively, memoizing every node, for every draw.
// Plans read scalar memory through plain integer arithmetic, so the refresh is compiled once into
// nodes in the walker's own depth-first evaluation order; they read memory in the same order and
// through the same readers, and are evaluated without recursion. A plan with control flow keeps
// the walker's block order and lets the walker evaluate its branch conditions. The results seed
// the walkers' memos for those conditions and for the descriptor evaluation that follows.
struct SrtTape {
	enum class Op : uint8_t {
		Const,
		UserData,
		ShaderBase,
		Copy,
		Add32,
		Add64,
		Sub32,
		Sub64,
		Mul32,
		Shl32,
		Shl64,
		Shr32,
		Shr64,
		And32,
		And64,
		Or32,
		Xor32,
		BitFieldUExtract,
		Pack64,
		Extract64,
		AddCarry,
		ScalarRead,
		ConstBufferRead,
	};
	struct Node {
		Op          op;
		bool        clean = false;
		uint32_t    a = 0, b = 0, c = 0, d = 0, e = 0;
		uint64_t    immediate = 0;
		const Inst* inst      = nullptr;
		// The instruction's slot in the walkers' memos, so evaluation does not touch the IR.
		uint32_t index = UINT32_MAX;
		// Also memoized by the ordinary walker, which delegates clean reads.
		bool        seed_ordinary = false;
	};
	// A plan with control flow is refreshed block by block in the walker's order. Each block
	// lists the nodes its reads need, in evaluation order, including nodes shared with others.
	struct Block {
		std::vector<uint32_t>                      nodes;
		std::vector<std::pair<uint32_t, uint32_t>> slots;
	};
	std::vector<Node>                          nodes;
	// For a tape without control flow: the nodes a refresh computes, in evaluation order.
	// Constant nodes are left out; their values are written once.
	std::vector<uint32_t>                      order;
	std::vector<std::pair<uint32_t, uint32_t>> slots; // flat offset, node
	std::vector<Block>                         blocks;
	// For a tape without control flow: per descriptor source, the node computing each DWORD as
	// the ordinary walker evaluates it, or NoNode.
	static constexpr uint32_t                  NoNode = UINT32_MAX;
	std::vector<std::array<uint32_t, 8>>       descriptors;
	bool                                       has_clean = false;

	// The nodes a node is computed from, in evaluation order.
	static size_t Operands(const Node& node, std::array<uint32_t, 5>& operands) {
		operands = {node.a, node.b, node.c, node.d, node.e};
		switch (node.op) {
			case Op::Const:
			case Op::UserData:
			case Op::ShaderBase: return 0;
			case Op::Copy:
			case Op::Extract64: return 1;
			case Op::BitFieldUExtract:
			case Op::ScalarRead: return 3;
			case Op::ConstBufferRead: return 5;
			default: return 2;
		}
	}
};

namespace {

class SrtTapeCompiler {
public:
	SrtTapeCompiler(const ResourcePlan& program, bool has_clean): m_program(program) {
		m_tape.has_clean = has_clean;
	}

	std::shared_ptr<const SrtTape> Compile() {
		if (m_program.control_flow.empty()) {
			for (const auto& read: m_program.srt_reads) {
				uint32_t node = 0;
				if (!Slot(read, node)) return nullptr;
				m_tape.slots.emplace_back(read.flat_offset, node);
			}
			// Nodes are emitted after their operands, so their indices are an evaluation order.
			for (uint32_t index = 0; index < m_tape.nodes.size(); ++index) {
				if (m_tape.nodes[index].op != Op::Const) m_tape.order.push_back(index);
			}
			MapDescriptors();
			return std::make_shared<const SrtTape>(std::move(m_tape));
		}
		m_tape.blocks.resize(m_program.control_flow.size());
		for (size_t index = 0; index < m_program.control_flow.size(); ++index) {
			for (const auto slot: m_program.control_flow[index].srt_reads) {
				uint32_t node = 0;
				if (slot >= m_program.srt_reads.size() || !Slot(m_program.srt_reads[slot], node)) {
					return nullptr;
				}
				m_tape.blocks[index].slots.emplace_back(m_program.srt_reads[slot].flat_offset,
				                                        node);
			}
		}
		std::vector<uint8_t> listed;
		for (auto& block: m_tape.blocks) {
			listed.assign(m_tape.nodes.size(), 0u);
			for (const auto& [offset, node]: block.slots) {
				ListEvaluationOrder(node, listed, block.nodes);
			}
		}
		return std::make_shared<const SrtTape>(std::move(m_tape));
	}

private:
	using Op = SrtTape::Op;

	// The ordinary walker finds a value the tape computed in its memo: an ordinary node, or a clean
	// read the tape also seeds into the ordinary memo.
	void MapDescriptors() {
		m_tape.descriptors.resize(m_program.descriptor_sources.size());
		for (size_t source = 0; source < m_program.descriptor_sources.size(); ++source) {
			const auto& descriptor = m_program.descriptor_sources[source];
			auto&       nodes      = m_tape.descriptors[source];
			nodes.fill(SrtTape::NoNode);
			for (uint32_t index = 0; index < descriptor.dword_count && index < nodes.size();
			     ++index) {
				const auto* inst = descriptor.dwords[index].Resolve().TryInstruction();
				if (inst == nullptr) continue;
				if (const auto found = m_nodes.find({inst, false}); found != m_nodes.end()) {
					nodes[index] = found->second;
				} else if (const auto clean = m_nodes.find({inst, true});
				           clean != m_nodes.end() && m_tape.nodes[clean->second].seed_ordinary) {
					nodes[index] = clean->second;
				}
			}
		}
	}

	bool Slot(const SrtRead& read, uint32_t& node) {
		const auto* inst  = read.value.Resolve().TryInstruction();
		const bool  clean = inst != nullptr && inst->Flags<SrtReadFlags>().clean != 0u;
		// The walker fails a clean slot it has no clean reader for; leave that to it.
		if ((clean && !m_tape.has_clean) || read.flat_offset >= m_program.srt_reads.size()) {
			return false;
		}
		return Node(read.value, clean, node);
	}

	// Appends a node after the nodes it is computed from, as the walker evaluates them.
	void ListEvaluationOrder(uint32_t node, std::vector<uint8_t>& listed,
	                         std::vector<uint32_t>& order) const {
		if (listed[node] != 0u) return;
		listed[node] = 1u;
		std::array<uint32_t, 5> operands {};
		const auto              count = SrtTape::Operands(m_tape.nodes[node], operands);
		for (size_t index = 0; index < count; ++index) {
			ListEvaluationOrder(operands[index], listed, order);
		}
		if (m_tape.nodes[node].op != Op::Const) order.push_back(node);
	}

	static Op BinaryOp(ValueOpcode opcode) {
		switch (opcode) {
			case ValueOpcode::IAdd32: return Op::Add32;
			case ValueOpcode::IAdd64: return Op::Add64;
			case ValueOpcode::ISub32: return Op::Sub32;
			case ValueOpcode::ISub64: return Op::Sub64;
			case ValueOpcode::IMul32: return Op::Mul32;
			case ValueOpcode::ShiftLeftLogical32: return Op::Shl32;
			case ValueOpcode::ShiftLeftLogical64: return Op::Shl64;
			case ValueOpcode::ShiftRightLogical32: return Op::Shr32;
			case ValueOpcode::ShiftRightLogical64: return Op::Shr64;
			case ValueOpcode::BitwiseAnd32: return Op::And32;
			case ValueOpcode::BitwiseAnd64: return Op::And64;
			case ValueOpcode::BitwiseOr32: return Op::Or32;
			case ValueOpcode::BitwiseXor32: return Op::Xor32;
			default: return Op::Pack64;
		}
	}

	uint32_t Emit(SrtTape::Node node) {
		m_tape.nodes.push_back(node);
		return static_cast<uint32_t>(m_tape.nodes.size() - 1u);
	}

	bool Args(const Inst& inst, bool clean, std::initializer_list<uint32_t*> outputs) {
		uint32_t index = 0;
		for (auto* output: outputs) {
			if (!Node(inst.Arg(index++), clean, *output)) return false;
		}
		return true;
	}

	bool Node(Value value, bool clean, uint32_t& result) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			uint64_t bits = 0;
			switch (value.GetType()) {
				case Type::U1: bits = value.U1(); break;
				case Type::U8: bits = value.U8(); break;
				case Type::U16: bits = value.U16(); break;
				case Type::U32: bits = value.U32(); break;
				case Type::U64: bits = value.U64(); break;
				case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); break;
				default: return false;
			}
			result = Emit({.op = Op::Const, .immediate = bits});
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		// The ordinary walker hands a clean read, and everything it depends on, to the clean one.
		const bool clean_read = m_tape.has_clean && inst->Flags<SrtReadFlags>().clean != 0u &&
		                        (inst->GetOpcode() == ValueOpcode::LoadAddressU32 ||
		                         inst->GetOpcode() == ValueOpcode::ReadConstBuffer);
		const bool mode = clean || clean_read;
		const auto key  = std::pair {inst, mode};
		if (const auto found = m_nodes.find(key); found != m_nodes.end()) {
			result = found->second;
			if (!clean && mode) m_tape.nodes[result].seed_ordinary = true;
			return true;
		}
		SrtTape::Node node {.clean = mode,
		                    .inst  = inst,
		                    .index = inst->EvaluationIndex(m_program.evaluation_value_count)};
		switch (inst->GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base) return false;
				node.op = Op::UserData;
				node.a  = reg - m_program.user_data_base;
				break;
			}
			case ValueOpcode::GetShaderBase: node.op = Op::ShaderBase; break;
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32:
				node.op = Op::Copy;
				if (!Args(*inst, mode, {&node.a})) return false;
				break;
			case ValueOpcode::IAdd32:
			case ValueOpcode::IAdd64:
			case ValueOpcode::ISub32:
			case ValueOpcode::ISub64:
			case ValueOpcode::IMul32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftLeftLogical64:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftRightLogical64:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseAnd64:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32:
			case ValueOpcode::CompositeConstructU64:
				node.op = BinaryOp(inst->GetOpcode());
				if (!Args(*inst, mode, {&node.a, &node.b})) return false;
				break;
			case ValueOpcode::BitFieldUExtract:
				node.op = Op::BitFieldUExtract;
				if (!Args(*inst, mode, {&node.a, &node.b, &node.c})) return false;
				break;
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: {
				// As SrtWalker::EvaluateExtract.
				const auto index = inst->Arg(1).Resolve();
				if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
					return false;
				}
				const auto component = index.U32();
				if (inst->GetOpcode() == ValueOpcode::CompositeExtractU64) {
					node.op        = Op::Extract64;
					node.immediate = component;
					if (!Node(inst->Arg(0), mode, node.a)) return false;
					break;
				}
				const auto source = inst->Arg(0).Resolve();
				const auto* pair  = source.TryInstruction();
				if (pair == nullptr) return false;
				if (pair->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
					node.op = Op::Copy;
					if (!Node(pair->Arg(component), mode, node.a)) return false;
				} else if (pair->GetOpcode() == ValueOpcode::IAddCarry32) {
					node.op        = Op::AddCarry;
					node.immediate = component;
					if (!Args(*pair, mode, {&node.a, &node.b})) return false;
				} else {
					return false;
				}
				break;
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer: {
				if (!IsRawRead(m_program, *inst)) return false;
				const auto* handle = inst->Arg(0).ResolveInstruction();
				if (handle == nullptr) return false;
				const auto& memory = m_program.memory_info[inst->Flags<SrtReadFlags>().index];
				node.immediate     = memory.offset;
				// The walker evaluates the base, the offset, then a const buffer's size words.
				if (!Node(handle->Arg(0), mode, node.a) || !Node(handle->Arg(1), mode, node.b) ||
				    !Node(inst->Arg(1), mode, node.c)) {
					return false;
				}
				if (inst->GetOpcode() == ValueOpcode::ReadConstBuffer) {
					node.op = Op::ConstBufferRead;
					if (handle->NumArgs() != 4u || !Node(handle->Arg(2), mode, node.d) ||
					    !Node(handle->Arg(3), mode, node.e)) {
						return false;
					}
				} else {
					node.op = Op::ScalarRead;
				}
				break;
			}
			default: return false;
		}
		node.seed_ordinary = !clean && mode;
		result             = Emit(node);
		m_nodes.emplace(key, result);
		return true;
	}

	struct KeyHash {
		size_t operator()(const std::pair<const Inst*, bool>& key) const {
			return std::hash<const Inst*> {}(key.first) * 2u + key.second;
		}
	};

	const ResourcePlan&                                                    m_program;
	SrtTape                                                                m_tape;
	std::unordered_map<std::pair<const Inst*, bool>, uint32_t, KeyHash>   m_nodes;
};

} // namespace

static bool ComputeTapeNode(const SrtTape::Node& node, const std::vector<uint64_t>& values,
                            const SrtRuntime& runtime, uint64_t& out);

SrtWalker::TapeResult SrtWalker::RefreshFlatBufferTape(std::vector<uint32_t>& flat) {
	const bool has_clean = m_clean_evaluator != nullptr;
	if (!m_program.srt_tape_built) {
		m_program.srt_tape_built = true;
		m_program.srt_tape       = SrtTapeCompiler(m_program, has_clean).Compile();
	}
	const auto* tape = m_program.srt_tape.get();
	if (tape == nullptr || tape->has_clean != has_clean || !m_active_mask.IsEmpty()) {
		return TapeResult::Unsupported;
	}
	if (has_clean && m_clean_evaluator->m_runtime.read_specialization_memory == nullptr) {
		return TapeResult::Unsupported;
	}
	auto& values = m_program.srt_tape_values;
	if (values.size() != tape->nodes.size()) {
		// Constant nodes are written once; refreshes skip them.
		values.assign(tape->nodes.size(), 0u);
		for (size_t index = 0; index < tape->nodes.size(); ++index) {
			if (tape->nodes[index].op == SrtTape::Op::Const) {
				values[index] = tape->nodes[index].immediate;
			}
		}
	}
	// Every node's memo slot was assigned when the tape was compiled.
	const auto memo_slots = m_program.evaluation_value_count;
	if (m_context.values.size() < memo_slots) {
		m_context.values.resize(memo_slots);
	}
	if (has_clean && m_clean_evaluator->m_context.values.size() < memo_slots) {
		m_clean_evaluator->m_context.values.resize(memo_slots);
	}
	const auto recall = [](const ResourcePlan::EvaluationContext& context, uint32_t slot,
	                       uint64_t& value) {
		const auto& entry = context.values[slot];
		if (entry.generation != context.generation) return false;
		value = entry.value;
		return true;
	};
	const auto remember = [](ResourcePlan::EvaluationContext& context, uint32_t slot,
	                         uint64_t value) {
		context.values[slot] = {value, context.generation};
	};
	// Computes one node and seeds the walkers' memos with it, so a branch condition the walker
	// evaluates, and the descriptor evaluation that follows, find it.
	const auto compute = [&](size_t index) {
		const auto& node = tape->nodes[index];
		uint64_t    out  = 0;
		switch (node.op) {
			case SrtTape::Op::Const: out = node.immediate; break;
			case SrtTape::Op::UserData:
				// The operand is a register, not a node.
				if (node.a >= m_runtime.user_data.size()) return false;
				out = m_runtime.user_data[node.a];
				break;
			case SrtTape::Op::ShaderBase: out = m_runtime.shader_base; break;
			default:
				if (!ComputeTapeNode(node, values,
				                     node.clean ? m_clean_evaluator->m_runtime : m_runtime, out)) {
					return false;
				}
				break;
		}
		values[index] = out;
		if (node.inst != nullptr) {
			if (node.clean) {
				remember(m_clean_evaluator->m_context, node.index, out);
				if (node.seed_ordinary) remember(m_context, node.index, out);
			} else {
				remember(m_context, node.index, out);
			}
		}
		return true;
	};
	const auto evaluate = [&](size_t index) {
		const auto& node = tape->nodes[index];
		// A value the walkers already evaluated in this session is not read again.
		if (node.inst != nullptr &&
		    ((node.seed_ordinary && recall(m_context, node.index, values[index])) ||
		     recall(node.clean ? m_clean_evaluator->m_context : m_context, node.index,
		            values[index]))) {
			return true;
		}
		return compute(index);
	};
	if (tape->blocks.empty()) {
		// When the flat refresh opens the walkers' session, each node is listed once and no value
		// can be in their memos yet.
		const bool fresh =
		    !m_memo_used && (m_clean_evaluator == nullptr || !m_clean_evaluator->m_memo_used);
		for (const auto index: tape->order) {
			if (!(fresh ? compute(index) : evaluate(index))) return TapeResult::Failed;
		}
		flat.resize(m_program.srt_reads.size());
		for (const auto& [offset, node]: tape->slots) {
			flat[offset] = static_cast<uint32_t>(values[node]);
		}
		m_program.active_sources.clear();
		m_tape_complete = true;
		return TapeResult::Done;
	}
	// As the walker's control-flow refresh, with each block's reads taken from the tape.
	flat.assign(m_program.srt_reads.size(), 0u);
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
		if (visited.at(index)) continue;
		visited[index]    = 1u;
		const auto& block = m_program.control_flow[index];
		for (const auto source: block.sources) {
			active[source] = 1u;
		}
		const auto& compiled = tape->blocks[index];
		for (const auto node: compiled.nodes) {
			if (!evaluate(node)) return TapeResult::Failed;
		}
		for (const auto& [offset, node]: compiled.slots) {
			flat[offset] = static_cast<uint32_t>(values[node]);
		}
		uint32_t condition = 0;
		auto&    predicate = m_clean_evaluator != nullptr ? *m_clean_evaluator : *this;
		if (!block.condition.IsEmpty() &&
		    (m_runtime.read_specialization_memory != nullptr ||
		     !m_program.capture_specialization_reads) &&
		    predicate.Evaluate(block.condition, condition)) {
			pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
		} else {
			pending.insert(pending.end(), block.successors.begin(), block.successors.end());
		}
	}
	return TapeResult::Done;
}

// A node computed from other nodes; reads go through the runtime of the node's walker.
static bool ComputeTapeNode(const SrtTape::Node& node, const std::vector<uint64_t>& values,
                            const SrtRuntime& runtime, uint64_t& out) {
	const auto a = values[node.a];
	const auto b = values[node.b];
	switch (node.op) {
		case SrtTape::Op::Const:
		case SrtTape::Op::UserData:
		case SrtTape::Op::ShaderBase: return false;
		case SrtTape::Op::Copy: out = a; break;
		case SrtTape::Op::Add32: out = static_cast<uint32_t>(a + b); break;
		case SrtTape::Op::Add64: out = a + b; break;
		case SrtTape::Op::Sub32: out = static_cast<uint32_t>(a - b); break;
		case SrtTape::Op::Sub64: out = a - b; break;
		case SrtTape::Op::Mul32: out = static_cast<uint32_t>(a * b); break;
		case SrtTape::Op::Shl32: out = static_cast<uint32_t>(a) << (b & 31u); break;
		case SrtTape::Op::Shl64: out = a << (b & 63u); break;
		case SrtTape::Op::Shr32: out = static_cast<uint32_t>(a) >> (b & 31u); break;
		case SrtTape::Op::Shr64: out = a >> (b & 63u); break;
		case SrtTape::Op::And32: out = static_cast<uint32_t>(a & b); break;
		case SrtTape::Op::And64: out = a & b; break;
		case SrtTape::Op::Or32: out = static_cast<uint32_t>(a | b); break;
		case SrtTape::Op::Xor32: out = static_cast<uint32_t>(a ^ b); break;
		case SrtTape::Op::BitFieldUExtract: {
			const auto offset = static_cast<uint32_t>(b);
			const auto width  = static_cast<uint32_t>(values[node.c]);
			if (offset > 32u || width > 32u - offset) return false;
			const auto mask = width == 32u  ? UINT32_MAX
			                  : width == 0u ? 0u
			                                : (uint32_t {1} << width) - 1u;
			out             = width == 0u ? 0u : (static_cast<uint32_t>(a) >> offset) & mask;
			break;
		}
		case SrtTape::Op::Pack64:
			out =
			    static_cast<uint32_t>(a) | (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
			break;
		case SrtTape::Op::Extract64:
			out = static_cast<uint32_t>(a >> (node.immediate * 32u));
			break;
		case SrtTape::Op::AddCarry: {
			const auto sum =
			    static_cast<uint64_t>(static_cast<uint32_t>(a)) + static_cast<uint32_t>(b);
			out = node.immediate == 0u ? static_cast<uint32_t>(sum)
			                           : static_cast<uint32_t>(sum >> 32u);
			break;
		}
		case SrtTape::Op::ScalarRead:
		case SrtTape::Op::ConstBufferRead: {
			const bool const_buffer = node.op == SrtTape::Op::ConstBufferRead;
			uint64_t   address      = 0;
			uint32_t   word         = 0;
			if (!ScalarReadAddress(const_buffer, static_cast<uint32_t>(node.immediate), a, b,
			                       const_buffer ? values[node.d] : 0u, values[node.c], address) ||
			    !ReadScalarWord(runtime, address, word)) {
				return false;
			}
			out = word;
			break;
		}
	}
	return true;
}

bool SrtWalker::EvaluateDescriptor(uint32_t source, DescriptorValue& result) {
	if (source >= m_program.descriptor_sources.size()) {
		return false;
	}
	const auto& descriptor = m_program.descriptor_sources[source];
	result                 = {};
	result.dword_count     = descriptor.dword_count;
	// Values this session's tape computed are read from it rather than found again in the memo.
	const auto* tape = m_tape_complete ? m_program.srt_tape.get() : nullptr;
	const auto* nodes =
	    tape != nullptr && source < tape->descriptors.size() ? &tape->descriptors[source] : nullptr;
	for (uint32_t index = 0; index < descriptor.dword_count; ++index) {
		if (nodes != nullptr && index < nodes->size() && (*nodes)[index] != SrtTape::NoNode) {
			result.dwords[index] =
			    static_cast<uint32_t>(m_program.srt_tape_values[(*nodes)[index]]);
			continue;
		}
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
		// The compiled walker seeds this session's memo.
		m_memo_used = true;
		using Walker = bool KYTY_SYSV_ABI (*)(const uint32_t*, uint32_t*,
		    ResourcePlan::EvaluationContext::Entry*, uint64_t, size_t);
		return m_program.srt_code->getCode<Walker>()(m_runtime.user_data.data(), flat.data(),
		    m_context.values.data(), m_context.generation, m_runtime.user_data.size());
	}
	// The tape fails exactly where the walker would, after the same reads.
	switch (RefreshFlatBufferTape(flat)) {
		case TapeResult::Done: return true;
		case TapeResult::Failed: return false;
		case TapeResult::Unsupported: break;
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

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type,
                          Value counter) {
	return RuntimeValidator(program, type, counter).Run(value);
}


} // namespace Libs::Graphics::ShaderRecompiler::IR
