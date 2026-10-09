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

// DWORD-aligned base address of a read handle's address DWORDs.
uint64_t ReadBase(uint64_t low, uint64_t high) {
	return ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask & ~uint64_t {3};
}

// Byte offset of a scalar-address DWORD read: aligned immediate plus aligned offset operand.
int64_t ScalarReadOffset(uint32_t immediate, uint64_t offset) {
	return (static_cast<int64_t>(static_cast<int32_t>(immediate)) & ~int64_t {3}) +
	       static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
}

// Byte offset of a constant-buffer DWORD read, for a non-negative immediate.
uint64_t BufferReadOffset(uint32_t immediate, uint64_t offset) {
	return (uint64_t {immediate} & ~uint64_t {3}) + (static_cast<uint32_t>(offset) & ~3u);
}

// A DWORD at byte_offset lies within the buffer: stride (in high) times records, or records
// bytes without a stride.
bool BufferDwordInBounds(uint64_t byte_offset, uint64_t high, uint64_t records) {
	const auto stride = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
	const auto size = stride == 0u ? static_cast<uint64_t>(static_cast<uint32_t>(records))
	                               : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
	return byte_offset <= size && size - byte_offset >= sizeof(uint32_t);
}

// Byte offset of a scalar-address or constant-buffer DWORD read.
int64_t RawReadOffset(bool buffer, uint32_t immediate, uint64_t offset) {
	return buffer ? static_cast<int64_t>(BufferReadOffset(immediate, offset))
	              : ScalarReadOffset(immediate, offset);
}

// Address of a raw DWORD read: within the 48-bit address space, and within the buffer's records
// for a buffer read.
bool RawReadAddress(bool buffer, int64_t offset, uint64_t low, uint64_t high, uint64_t records,
                    uint64_t& address) {
	if (buffer && !BufferDwordInBounds(static_cast<uint64_t>(offset), high, records)) {
		return false;
	}
	return AddSignedAddress(ReadBase(low, high), offset, address);
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
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};

} // namespace

namespace {

using WalkerNode      = ResourcePlan::WalkerNode;
using WalkerImmediate = ResourcePlan::WalkerImmediate;

// Reference encoding, as in SrtWalker.
constexpr uint32_t ImmediateRef = 0x80000000u;
constexpr uint32_t InlineRef    = 0xc0000000u;
constexpr uint32_t NoRef        = UINT32_MAX;

enum WalkerKind : uint8_t {
	WalkerOpcode,  // evaluated by opcode
	WalkerRawRead, // read opcode accepted by IsRawRead
	WalkerFail,    // evaluation fails before touching any operand
	WalkerExit,    // GetUserData without a scalar register: fail fast like Value::ScalarRegister
};

bool IsImmediateRef(uint32_t ref) {
	return ref >= ImmediateRef && ref != NoRef;
}

WalkerImmediate MakeImmediate(Value value) {
	WalkerImmediate immediate;
	immediate.type = value.GetType();
	switch (immediate.type) {
		case Type::U1: immediate.payload = value.U1() ? 1u : 0u; break;
		case Type::U8: immediate.payload = value.U8(); break;
		case Type::U16: immediate.payload = value.U16(); break;
		case Type::U32: immediate.payload = value.U32(); break;
		case Type::U64: immediate.payload = value.U64(); break;
		case Type::F32: immediate.payload = std::bit_cast<uint32_t>(value.F32Value()); break;
		case Type::F16: immediate.payload = value.F16Bits(); break;
		case Type::ScalarReg: immediate.payload = RegIndex(value.ScalarRegister()); break;
		case Type::VectorReg: immediate.payload = RegIndex(value.VectorRegister()); break;
		case Type::Void: break;
		default: return immediate;
	}
	// Value::operator== compares these types by payload; only integer and F32 values evaluate.
	immediate.comparable = true;
	switch (immediate.type) {
		case Type::U1:
		case Type::U8:
		case Type::U16:
		case Type::U32:
		case Type::U64:
		case Type::F32: immediate.valid = true; break;
		default: break;
	}
	return immediate;
}

// Maps a value to its walker reference, allocating the node slot of a new instruction.
uint32_t WalkerRef(const ResourcePlan& plan, Value value) {
	value = value.Resolve();
	if (const auto* inst = value.TryInstruction(); inst != nullptr) {
		const auto index = inst->EvaluationIndex(plan.evaluation_value_count);
		EXIT_IF(index >= ImmediateRef);
		if (index >= plan.walker_nodes.size()) {
			plan.walker_nodes.resize(plan.evaluation_value_count);
			++plan.walker_layout;
		}
		auto& node = plan.walker_nodes[index];
		// Evaluation indices belong to the plan that owns the instruction.
		EXIT_IF(node.inst != nullptr && node.inst != inst);
		node.inst = inst;
		return index;
	}
	const auto immediate = MakeImmediate(value);
	if (immediate.type == Type::U32 && immediate.payload < (NoRef & ~InlineRef)) {
		return InlineRef | static_cast<uint32_t>(immediate.payload);
	}
	plan.walker_immediates.push_back(immediate);
	++plan.walker_layout;
	EXIT_IF(plan.walker_immediates.size() > InlineRef - ImmediateRef);
	return ImmediateRef | static_cast<uint32_t>(plan.walker_immediates.size() - 1u);
}

WalkerImmediate ImmediateOf(const ResourcePlan& plan, uint32_t ref) {
	if (ref >= InlineRef) {
		return {.type = Type::U32, .valid = true, .comparable = true, .payload = ref & ~InlineRef};
	}
	return plan.walker_immediates[ref & ~ImmediateRef];
}

// Same result as Value::operator== on the referenced values.
bool RefEquals(const ResourcePlan& plan, uint32_t left, uint32_t right) {
	if (left == NoRef || right == NoRef) {
		return false;
	}
	if (!IsImmediateRef(left) || !IsImmediateRef(right)) {
		return left == right;
	}
	const auto a = ImmediateOf(plan, left);
	const auto b = ImmediateOf(plan, right);
	return a.type == b.type && a.comparable && b.comparable && a.payload == b.payload;
}

// Value::operator== on two active masks, where NoRef stands for the empty mask.
bool MaskEquals(const ResourcePlan& plan, uint32_t left, uint32_t right) {
	return left == NoRef || right == NoRef ? left == right : RefEquals(plan, left, right);
}

// An immediate Void mask is the empty mask.
uint32_t MaskRef(const ResourcePlan& plan, uint32_t ref) {
	return IsImmediateRef(ref) && ImmediateOf(plan, ref).type == Type::Void ? NoRef : ref;
}

bool ImmediateU32(const ResourcePlan& plan, uint32_t ref, uint32_t& value) {
	if (!IsImmediateRef(ref)) {
		return false;
	}
	const auto immediate = ImmediateOf(plan, ref);
	if (immediate.type != Type::U32) {
		return false;
	}
	value = static_cast<uint32_t>(immediate.payload);
	return true;
}

void CompileNode(const ResourcePlan& plan, uint32_t index) {
	const Inst& inst = *plan.walker_nodes[index].inst;
	WalkerNode  node;
	node.inst      = &inst;
	node.opcode    = inst.GetOpcode();
	node.kind      = WalkerOpcode;
	node.arg_count = static_cast<uint8_t>(std::min<size_t>(inst.NumArgs(), WalkerNode::MaxArgs));
	for (uint32_t arg = 0; arg < node.arg_count; ++arg) {
		node.args[arg] = WalkerRef(plan, inst.Arg(arg));
	}
	if (node.opcode == ValueOpcode::Phi) {
		const auto invariant = ResolveInvariantPhi(plan, Value(const_cast<Inst*>(&inst)));
		node.target          = invariant.IsEmpty() ? NoRef : WalkerRef(plan, invariant);
	}
	switch (node.opcode) {
		case ValueOpcode::GetUserData: {
			const auto reg = inst.Arg(0);
			if (!reg.IsImmediate() || reg.GetType() != Type::ScalarReg) {
				node.kind = WalkerExit;
				break;
			}
			const auto index_reg = RegIndex(reg.ScalarRegister());
			node.aux = index_reg >= plan.user_data_base ? index_reg - plan.user_data_base : NoRef;
			break;
		}
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2: {
			uint32_t component = 0;
			node.aux =
			    ImmediateU32(plan, node.args[1], component) && component < 2u ? component : NoRef;
			break;
		}
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU32:
			if (IsRawRead(plan, inst)) {
				const auto flags = inst.Flags<SrtReadFlags>();
				node.kind        = WalkerRawRead;
				node.aux         = flags.clean;
				node.target      = plan.memory_info[flags.index].offset;
			} else {
				node.kind = WalkerFail;
			}
			break;
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
			node.aux = inst.Flags<FPCompareFlags>().flush_input_denorms ? 1u : 0u;
			break;
		default: break;
	}
	node.compiled            = true;
	plan.walker_nodes[index] = node;
}

// Compiles every node the walker can reach from `root`, so evaluation never adds nodes.
void CompileClosure(const ResourcePlan& plan, uint32_t root) {
	if (IsImmediateRef(root) || plan.walker_nodes[root].compiled) {
		return;
	}
	std::vector<uint32_t> pending {root};
	while (!pending.empty()) {
		const auto index = pending.back();
		pending.pop_back();
		if (plan.walker_nodes[index].compiled) {
			continue;
		}
		CompileNode(plan, index);
		const auto& node = plan.walker_nodes[index];
		for (uint32_t arg = 0; arg < node.arg_count; ++arg) {
			if (!IsImmediateRef(node.args[arg])) {
				pending.push_back(node.args[arg]);
			}
		}
		if (node.opcode == ValueOpcode::Phi && node.target != NoRef &&
		    !IsImmediateRef(node.target)) {
			pending.push_back(node.target);
		}
	}
}

} // namespace

SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     SrtWalker* clean_evaluator, Value active_mask)
    : SrtWalker(program, runtime, clean_evaluator, NoRef) {
	if (m_program.evaluation_depth == 1) {
		// A walk starts. Compiled reads of SRT slots hold the slot values: a replaced value
		// drops the compiled form.
		auto&       roots   = m_program.walker_srt_reads;
		const auto& reads   = m_program.srt_reads;
		bool        changed = roots.size() != reads.size();
		for (size_t slot = 0; !changed && slot < reads.size(); ++slot) {
			changed = !SameRoot(roots[slot].value, reads[slot].value);
		}
		if (changed) {
			m_program.walker_nodes.clear();
			m_program.walker_immediates.clear();
			m_program.walker_descriptors.clear();
			roots.assign(reads.size(), {});
			m_program.walker_reads.assign(reads.size(), {});
			for (size_t slot = 0; slot < reads.size(); ++slot) {
				CompileRoot(roots[slot], reads[slot].value);
				const auto* inst  = reads[slot].value.ResolveInstruction();
				roots[slot].clean = inst != nullptr && inst->Flags<SrtReadFlags>().clean != 0u;
				PrepareRead(roots[slot], m_program.walker_reads[slot]);
			}
		}
	}
	if (!active_mask.Resolve().IsEmpty()) {
		m_active_mask = MaskRef(m_program, WalkerRef(m_program, active_mask));
		CompileClosure(m_program, m_active_mask);
	}
	Refresh();
}

// Also used during evaluation: it must not compile nodes.
SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     SrtWalker* clean_evaluator, Ref active_mask)
    : m_program(program), m_runtime(runtime), m_clean_evaluator(clean_evaluator),
      m_active_mask(MaskRef(program, active_mask)), m_context(AcquireContext(program)),
      m_generation(m_context.generation) {
	Refresh();
}

SrtWalker::~SrtWalker() {
	if (--m_program.evaluation_depth == 0 && !m_program.walker_cached) {
		m_program.walker_nodes.clear();
		m_program.walker_immediates.clear();
		m_program.walker_srt_reads.clear();
		m_program.walker_reads.clear();
		m_program.walker_descriptors.clear();
	}
}

void SrtWalker::Refresh() {
	const auto size = m_program.walker_nodes.size();
	for (auto* walker = this; walker != nullptr; walker = walker->m_clean_evaluator) {
		if (walker->m_context.values.size() < size) {
			walker->m_context.values.resize(size);
		}
		walker->m_nodes      = m_program.walker_nodes.data();
		walker->m_immediates = m_program.walker_immediates.data();
		walker->m_values     = walker->m_context.values.data();
		walker->m_layout     = m_program.walker_layout;
	}
}

void SrtWalker::PrepareRead(const ResourcePlan::WalkerRoot& root,
                            ResourcePlan::WalkerRead&       read) const {
	read = {};
	if (root.ref >= ImmediateRef) {
		return;
	}
	const auto& node   = m_program.walker_nodes[root.ref];
	const bool  buffer = node.opcode == ValueOpcode::ReadConstBuffer;
	if (!node.compiled || node.kind != WalkerRawRead ||
	    (!buffer && node.opcode != ValueOpcode::LoadAddressU32) || node.args[0] >= ImmediateRef ||
	    node.args[1] < InlineRef) {
		return;
	}
	const auto& handle = m_program.walker_nodes[node.args[0]];
	const auto  offset = node.args[1] & ~InlineRef;
	if (!handle.compiled || handle.arg_count < 2u ||
	    (buffer && (handle.arg_count != 4u || static_cast<int32_t>(node.target) < 0))) {
		return;
	}
	read.low    = handle.args[0];
	read.high   = handle.args[1];
	read.offset = RawReadOffset(buffer, node.target, offset);
	if (buffer) {
		read.records = handle.args[2];
		read.word3   = handle.args[3];
	}
}

// Out of line, so that the callers of Get keep their values in registers.
[[gnu::noinline]] SrtWalker::Result SrtWalker::GetSlow(Ref ref) {
	uint64_t   value = 0;
	const bool ok    = EvaluateRef(ref, value);
	return {value, ok};
}

SrtWalker::Result SrtWalker::EvaluateRead(const ResourcePlan::WalkerRoot& root,
                                          const ResourcePlan::WalkerRead& read) {
	const auto ref = root.ref;
	// The clean flag of a root is the aux of its read node: such a read is delegated.
	if (m_active_mask != NoRef || (m_clean_evaluator != nullptr && root.clean)) {
		return GetSlow(ref);
	}
	auto& memo = m_values[ref];
	if (memo.generation == m_generation) {
		return {memo.value, true};
	}
	// EvaluateNodeRef and EvaluateRawRead for this node shape: same memo, order and failures.
	EXIT_IF(m_layout != m_program.walker_layout);
	if (memo.generation == (m_generation | 1u)) {
		return {};
	}
	memo.generation  = m_generation | 1u;
	const auto value = ComputeFastRead(read);
	auto&      entry = m_values[ref];
	entry.value      = value.value;
	entry.generation = value.ok ? m_generation : 0u;
	return value;
}

SrtWalker::Result SrtWalker::ComputeFastRead(const ResourcePlan::WalkerRead& read) {
	const auto low = Get(read.low);
	if (!low.ok) {
		return {};
	}
	const auto high = Get(read.high);
	if (!high.ok) {
		return {};
	}
	const bool buffer  = read.records != NoRef;
	Result     records = {};
	if (buffer) {
		records = Get(read.records);
		// The buffer's fourth DWORD is evaluated as EvaluateRawRead does, but not used here.
		if (!records.ok || !Get(read.word3).ok) {
			return {};
		}
	}
	uint64_t address = 0;
	if (!RawReadAddress(buffer, read.offset, low.value, high.value, records.value, address)) {
		return {};
	}
	return ReadWord(address);
}

SrtWalker::Ref SrtWalker::CompileRoot(ResourcePlan::WalkerRoot& root, Value value) {
	root.value = value;
	root.ref   = WalkerRef(m_program, value);
	CompileClosure(m_program, root.ref);
	Refresh();
	return root.ref;
}

bool SrtWalker::Evaluate(Value value, uint32_t& result) {
	value         = value.Resolve();
	uint64_t wide = 0;
	if (value.IsImmediate()) {
		const auto immediate = MakeImmediate(value);
		if (!immediate.valid) {
			return false;
		}
		wide = immediate.payload;
	} else {
		const auto ref = WalkerRef(m_program, value);
		CompileClosure(m_program, ref);
		Refresh();
		if (!EvaluateRef(ref, wide)) {
			return false;
		}
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

bool SrtWalker::EvaluateNodeRef(Ref ref, uint64_t& result) {
	EXIT_IF(m_layout != m_program.walker_layout);
	if (ref == m_active_mask) {
		result = 1u;
		return true;
	}
	const auto& node = m_nodes[ref];
	if (m_active_mask != NoRef && IsRuntimeSelect(node.opcode) &&
	    RefEquals(m_program, node.args[0], m_active_mask)) {
		return EvaluateRef(node.args[1], result);
	}
	auto& memo = m_values[ref];
	if (memo.generation == m_generation) {
		result = memo.value;
		return true;
	}
	// The low generation bit marks a node that is still being evaluated.
	if (memo.generation == (m_generation | 1u)) {
		return false;
	}
	memo.generation      = m_generation | 1u;
	uint64_t   out       = 0;
	const bool evaluated = EvaluateNode(node, out);
	auto&      entry     = m_values[ref];
	if (!evaluated) {
		entry.generation = 0;
		return false;
	}
	entry.value      = out;
	entry.generation = m_generation;
	result           = out;
	return true;
}

bool SrtWalker::EvaluateExtract(const ResourcePlan::WalkerNode& node, uint64_t& result) {
	if (node.aux == NoRef) {
		return false;
	}
	const auto component = node.aux;
	if (node.opcode == ValueOpcode::CompositeExtractU64) {
		uint64_t packed = 0;
		if (!Arg(node, 0, packed)) {
			return false;
		}
		result = static_cast<uint32_t>(packed >> (component * 32u));
		return true;
	}
	if (IsImmediateRef(node.args[0])) {
		// Fails fast on a non-instruction source, as Value::ResolveInstruction does.
		(void)node.inst->Arg(0).ResolveInstruction();
		return false;
	}
	const auto& source = m_nodes[node.args[0]];
	if (source.opcode == ValueOpcode::CompositeConstructU32x2) {
		return Arg(source, component, result);
	}
	if (source.opcode == ValueOpcode::IAddCarry32) {
		uint64_t lhs = 0;
		uint64_t rhs = 0;
		if (!Arg(source, 0, lhs) || !Arg(source, 1, rhs)) {
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

bool SrtWalker::EvaluateRawRead(const ResourcePlan::WalkerNode& node, uint64_t& result) {
	const bool vector = node.opcode == ValueOpcode::LoadBufferU32;
	if (vector) {
		const auto guard = node.args[4];
		if (IsImmediateRef(guard) || !RefEquals(m_program, guard, m_active_mask)) {
			uint64_t enabled = 0;
			if (!EvaluateRef(guard, enabled)) return false;
			if (enabled == 0u) {
				result = 0u;
				return true;
			}
		}
	}
	if (IsImmediateRef(node.args[0])) {
		// Fails fast on a non-instruction handle, as Value::ResolveInstruction does.
		(void)node.inst->Arg(0).ResolveInstruction();
		return false;
	}
	const auto& handle     = m_nodes[node.args[0]];
	const auto  handle_arg = [&](uint32_t index, uint64_t& value) {
		if (index >= handle.arg_count) {
			// Same failure as Inst::Arg past the handle's arity.
			(void)handle.inst->Arg(index);
			return false;
		}
		return EvaluateRef(handle.args[index], value);
	};
	uint64_t low    = 0;
	uint64_t high   = 0;
	uint64_t offset = 0;
	if (!handle_arg(0, low) || !handle_arg(1, high) || !Arg(node, 1, offset)) {
		return false;
	}
	// ComputeFastRead repeats the scalar and constant-buffer steps for prepared reads: keep them in
	// step.
	const auto immediate = static_cast<int64_t>(static_cast<int32_t>(node.target));
	const bool buffer    = node.opcode == ValueOpcode::ReadConstBuffer || vector;
	uint64_t   records   = 0;
	if (buffer) {
		uint64_t word3 = 0;
		if (handle.arg_count != 4u || !handle_arg(2, records) || !handle_arg(3, word3)) {
			return false;
		}
		if (immediate < 0) {
			return false;
		}
		if (vector) {
			const auto stride = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			// Only the uniform, unswizzled structured DWORD address is evaluated on the host.
			if ((high & (1u << 31u)) != 0u || (word3 & ((1u << 23u) | 0xf0000000u)) != 0u)
				return false;
			if (stride == 0u || records == 0u || ((word3 >> 12u) & 0x7fu) == 0u) {
				result = 0u;
				return true;
			}
		}
	}
	uint64_t address = 0;
	if (!RawReadAddress(buffer, RawReadOffset(buffer, node.target, offset), low, high, records,
	                    address)) {
		return false;
	}
	if (!vector) {
		const auto word = ReadWord(address);
		result          = word.value;
		return word.ok;
	}
	uint32_t word = 0;
	const auto reader = m_runtime.read_specialization_memory;
	if (reader == nullptr || !reader(m_runtime.userdata, address, {&word, 1})) {
		return false;
	}
	result = word;
	return true;
}

// Out of line: the reader takes the address of the word.
[[gnu::noinline]] SrtWalker::Result SrtWalker::ReadThrough(const SrtRuntime& runtime,
                                                           uint64_t          address) {
	uint32_t word = 0;
	if (!runtime.read_memory(runtime.userdata, address, {&word, 1})) {
		return {};
	}
	return {word, true};
}

SrtWalker::Result SrtWalker::ReadWord(uint64_t address) const {
	if (m_runtime.read_memory != nullptr) {
		return ReadThrough(m_runtime, address);
	}
	uint32_t word = 0;
	std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
	return {word, true};
}

bool SrtWalker::EvaluateNode(const ResourcePlan::WalkerNode& node, uint64_t& result) {
	uint64_t   a       = 0;
	uint64_t   b       = 0;
	uint64_t   c       = 0;
	const auto binary  = [&]() { return Arg(node, 0, a) && Arg(node, 1, b); };
	const auto ternary = [&]() { return Arg(node, 0, a) && Arg(node, 1, b) && Arg(node, 2, c); };
	switch (node.kind) {
		case WalkerFail: return false;
		case WalkerExit: (void)node.inst->Arg(0).ScalarRegister(); return false;
		case WalkerRawRead:
			if (m_clean_evaluator != nullptr &&
			    (node.aux != 0u ||
			     (node.opcode == ValueOpcode::LoadBufferU32 &&
			      MaskEquals(m_program, m_clean_evaluator->m_active_mask, m_active_mask)))) {
				const auto self = static_cast<Ref>(&node - m_nodes);
				return m_clean_evaluator->EvaluateRef(self, result);
			}
			return EvaluateRawRead(node, result);
		default: break;
	}
	switch (node.opcode) {
		case ValueOpcode::GetUserData:
			if (node.aux == NoRef || node.aux >= m_runtime.user_data.size()) {
				return false;
			}
			result = m_runtime.user_data[node.aux];
			return true;
		case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
		case ValueOpcode::Phi: return node.target != NoRef && EvaluateRef(node.target, result);
		case ValueOpcode::ReadFirstLane: {
			if (m_active_mask != NoRef && RefEquals(m_program, node.args[1], m_active_mask)) {
				return Arg(node, 0, result);
			}
			const auto clean_runtime = CleanRuntime(m_runtime);
			SrtWalker  clean_active(m_program, clean_runtime, nullptr, node.args[1]);
			SrtWalker  active(m_program, m_runtime, &clean_active, node.args[1]);
			return active.Arg(node, 0, result);
		}
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32: return Arg(node, 0, result);
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(node, result);
		case ValueOpcode::CompositeConstructU64:
			if (!binary()) {
				return false;
			}
			result = static_cast<uint32_t>(a) |
			         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
			return true;
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
			if (Arg(node, 0, a)) {
				result = std::bit_cast<uint32_t>(static_cast<float>(static_cast<uint32_t>(a)));
				return true;
			}
			return false;
		case ValueOpcode::ConvertU32F32:
			if (Arg(node, 0, a)) {
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
			if (Arg(node, 0, a)) {
				result = std::bit_cast<uint32_t>(std::trunc(Float32(a)));
				return true;
			}
			return false;
		case ValueOpcode::FPRecipIFlag32:
			if (Arg(node, 0, a)) {
				const auto exponent = (a >> 23u) & 0xffu;
				// Normal positive powers of two have exact normal reciprocals in every FP mode.
				if ((a & 0x807fffffu) != 0u || exponent == 0u || exponent >= 254u) return false;
				result = (254u - exponent) << 23u;
				return true;
			}
			return false;
		case ValueOpcode::FPIsNan32:
			if (Arg(node, 0, a)) {
				result = std::isnan(Float32(a));
				return true;
			}
			return false;
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
			if (binary()) {
				const auto operand = [&](uint64_t bits) {
					if (node.aux != 0u && (bits & 0x7fffffffu) < 0x00800000u) {
						bits &= 0x80000000u;
					}
					return Float32(bits);
				};
				result = node.opcode == ValueOpcode::FPOrdLessThanEqual32
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
			if (Arg(node, 0, a)) {
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
			if (!ternary() || !Arg(node, 3, d)) {
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
			if (predicate.Arg(node, 0, a)) {
				return Arg(node, a != 0u ? 1u : 2u, result);
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
			const bool left = Arg(node, 0, a);
			if (left && a == 0u) {
				result = 0u;
				return true;
			}
			if (!Arg(node, 1, b) || (b != 0u && !left)) return false;
			result = b != 0u;
			return true;
		}
		case ValueOpcode::LogicalOr: {
			const bool left = Arg(node, 0, a);
			if (left && a != 0u) {
				result = 1u;
				return true;
			}
			if (!Arg(node, 1, b) || (b == 0u && !left)) return false;
			result = b != 0u;
			return true;
		}
		case ValueOpcode::LogicalXor:
			if (binary()) {
				result = (a != 0u) != (b != 0u);
				return true;
			}
			return false;
		case ValueOpcode::ConditionRef: return Arg(node, 0, result);
		case ValueOpcode::LogicalNot:
			if (Arg(node, 0, a)) {
				result = a == 0u;
				return true;
			}
			return false;
		default: break;
	}
	return false;
}

bool SrtWalker::EvaluateDescriptor(uint32_t source, DescriptorValue& result) {
	if (source >= m_program.descriptor_sources.size()) {
		return false;
	}
	auto& roots = m_program.walker_descriptors;
	if (roots.size() < m_program.descriptor_sources.size() * 8u) {
		roots.resize(m_program.descriptor_sources.size() * 8u);
	}
	// Another walker of this plan may have compiled nodes since this one last ran.
	Refresh();
	const auto& descriptor = m_program.descriptor_sources[source];
	result                 = {};
	result.dword_count     = descriptor.dword_count;
	for (uint32_t index = 0; index < descriptor.dword_count; ++index) {
		uint64_t   wide = 0;
		const auto ref  = RootRef(roots, size_t {source} * 8u + index, descriptor.dwords[index]);
		if (!EvaluateRef(ref, wide)) {
			return false;
		}
		result.dwords[index] = static_cast<uint32_t>(wide);
	}
	return true;
}

bool SrtWalker::RefreshFlatBuffer(std::vector<uint32_t>& flat) {
	if (!m_program.srt_plan_complete) return false;
	const auto& roots = m_program.walker_srt_reads;
	Refresh();
	const auto refresh = [&](uint32_t slot) {
		if (slot >= m_program.srt_reads.size()) return false;
		const auto& read = m_program.srt_reads[slot];
		const auto& root = roots[slot];
		if (root.clean &&
		    (m_clean_evaluator == nullptr || m_runtime.read_specialization_memory == nullptr))
			return false;
		if (read.flat_offset >= flat.size()) return false;
		auto&    evaluator = root.clean ? *m_clean_evaluator : *this;
		uint64_t wide      = 0;
		const auto& fast      = m_program.walker_reads[slot];
		if (fast.low != NoRef) {
			const auto value = evaluator.EvaluateRead(root, fast);
			if (!value.ok) {
				return false;
			}
			wide = value.value;
		} else if (!evaluator.EvaluateRef(root.ref, wide)) {
			return false;
		}
		flat[read.flat_offset] = static_cast<uint32_t>(wide);
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
		if (!block.condition.IsEmpty() &&
		    (m_runtime.read_specialization_memory != nullptr || !m_program.capture_specialization_reads) &&
		    predicate.Evaluate(block.condition, condition)) {
			pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
		} else {
			pending.insert(pending.end(), block.successors.begin(), block.successors.end());
		}
		Refresh();
	}
	return true;
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}


} // namespace Libs::Graphics::ShaderRecompiler::IR
