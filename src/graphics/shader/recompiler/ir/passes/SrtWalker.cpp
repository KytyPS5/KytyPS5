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

std::vector<std::pair<uint64_t, uint64_t>>*
SrtReadCapture::DirectRanges(const SrtRuntime& runtime) {
	if (runtime.read_memory != ReadOrdinary) return nullptr;
	auto& capture = *static_cast<SrtReadCapture*>(runtime.userdata);
	return capture.m_source.read_memory == nullptr ? &capture.m_ranges : nullptr;
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
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
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
		case ValueOpcode::IEqual64:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
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
			    index.U32() >= 2u || source->GetOpcode() != ValueOpcode::CompositeConstructU32x2) {
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

bool IsUserDataWord(const ResourcePlan& program, const Inst& inst) {
	return inst.GetOpcode() == ValueOpcode::GetUserData && inst.NumArgs() == 1 &&
	       inst.Arg(0).GetType() == Type::ScalarReg &&
	       RegIndex(inst.Arg(0).ScalarRegister()) >= program.user_data_base;
}

bool IsCompiledRead(const ResourcePlan& program, const Inst& inst, uint32_t depth = 0);

// User data or a compiled read: a handle word or a constant-buffer index.
bool IsCompiledWord(const ResourcePlan& program, Value value, uint32_t depth) {
	const auto* inst = value.Resolve().TryInstruction();
	return inst != nullptr &&
	       (IsUserDataWord(program, *inst) || IsCompiledRead(program, *inst, depth + 1));
}

// A scalar-address read with a constant offset, or a constant-buffer read at a constant or
// shifted-index offset, whose handle words are compiled words. Clean reads keep the strict
// reader of the clean evaluator; a negative buffer immediate fails in the walker.
bool IsCompiledRead(const ResourcePlan& program, const Inst& inst, uint32_t depth) {
	const auto op = inst.GetOpcode();
	if (depth > 16 || (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) ||
	    inst.NumArgs() < 2 || !IsRawRead(program, inst) || inst.Flags<SrtReadFlags>().clean != 0u)
		return false;
	const auto  offset    = inst.Arg(1).Resolve();
	const auto* handle    = inst.Arg(0).Resolve().TryInstruction();
	const bool  buffer    = op == ValueOpcode::ReadConstBuffer;
	const auto  immediate = [](Value value) {
		value = value.Resolve();
		return value.IsImmediate() && value.GetType() == Type::U32;
	};
	const auto* shift = offset.TryInstruction();
	const bool  index = buffer && shift != nullptr &&
	                    shift->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
	                    shift->NumArgs() == 2 && immediate(shift->Arg(1)) &&
	                    IsCompiledWord(program, shift->Arg(0), depth);
	if ((!immediate(offset) && !index) || handle == nullptr ||
	    handle->GetOpcode() !=
	        (buffer ? ValueOpcode::GetBufferResource : ValueOpcode::GetAddressResource) ||
	    handle->NumArgs() != (buffer ? 4u : 2u) ||
	    (buffer &&
	     static_cast<int32_t>(program.memory_info[inst.Flags<SrtReadFlags>().index].offset) < 0))
		return false;
	for (size_t word = 0; word < handle->NumArgs(); ++word) {
		if (!IsCompiledWord(program, handle->Arg(word), depth)) return false;
	}
	return true;
}

// Compiles the SRT slots of plans the whole-plan walker rejects (block conditions, captured
// reads) and the descriptors whose DWORDs are all such reads, user data or immediates: one entry
// per slot or descriptor, one memoized table read per read. Others stay interpreted.
void CompileSrtReads(ResourcePlan& program, bool compile_slots) {
	if (!program.resource_tracking_complete || !program.srt_plan_complete) return;
	std::vector<const Inst*> slots(compile_slots ? program.srt_reads.size() : 0u, nullptr);
	bool                     any = false;
	for (uint32_t slot = 0; slot < slots.size(); ++slot) {
		const auto& read = program.srt_reads[slot];
		const auto* inst = read.value.Resolve().TryInstruction();
		if (inst != nullptr && read.flat_offset < program.srt_reads.size() &&
		    IsCompiledRead(program, *inst)) {
			slots[slot] = inst;
			any         = true;
		}
	}
	const auto compiled_dword = [&](Value value) {
		value = value.Resolve();
		if (value.IsImmediate()) return value.GetType() == Type::U32;
		const auto* inst = value.TryInstruction();
		return inst != nullptr &&
		       (IsUserDataWord(program, *inst) || IsCompiledRead(program, *inst));
	};
	std::vector<bool> descriptors(program.descriptor_sources.size(), false);
	for (uint32_t source = 0; source < descriptors.size(); ++source) {
		const auto& descriptor = program.descriptor_sources[source];
		const auto  dwords =
		    std::span(descriptor.dwords)
		        .first(std::min<size_t>(descriptor.dword_count, descriptor.dwords.size()));
		descriptors[source] = descriptor.dword_count != 0u &&
		                      descriptor.dword_count <= descriptor.dwords.size() &&
		                      std::ranges::all_of(dwords, compiled_dword);
		any |= descriptors[source];
	}
	program.srt_read_entries.assign(program.srt_reads.size(), UINT32_MAX);
	program.srt_descriptor_entries.assign(program.descriptor_sources.size(), UINT32_MAX);
	if (!any) return;

	using Table            = ResourcePlan::SrtReadTable;
	program.srt_read_table = std::make_unique<Table>();
	auto& table            = *program.srt_read_table;
	// Read indices by instruction.
	std::unordered_map<const Inst*, uint32_t> indices;
	// A read follows the reads of its handle and index words, so one pass runs them in order.
	const auto read = [&](auto&& self, const Inst* inst) -> uint32_t {
		if (const auto found = indices.find(inst); found != indices.end()) return found->second;
		const auto word = [&](Value value) {
			const auto* source = value.Resolve().TryInstruction();
			return IsUserDataWord(program, *source)
			           ? Table::UserData |
			                 (RegIndex(source->Arg(0).ScalarRegister()) - program.user_data_base)
			           : self(self, source);
		};
		const auto* handle    = inst->Arg(0).Resolve().TryInstruction();
		const auto& memory    = program.memory_info[inst->Flags<SrtReadFlags>().index];
		const auto  immediate = static_cast<int64_t>(static_cast<int32_t>(memory.offset));
		Table::Read entry;
		entry.buffer = handle->NumArgs() == 4u;
		for (size_t index = 0; index < handle->NumArgs(); ++index) {
			entry.words[index] = word(handle->Arg(index));
			// Constant buffers read the offset after base and stride, as the walker does.
			if (entry.buffer && index == 1u) {
				const auto offset = inst->Arg(1).Resolve();
				if (offset.IsImmediate()) {
					entry.offset = offset.U32() & ~3u;
				} else {
					const auto* shift = offset.TryInstruction();
					entry.offset      = word(shift->Arg(0));
					entry.shift = static_cast<uint8_t>((shift->Arg(1).Resolve().U32() & 31u) + 1u);
				}
			}
		}
		entry.relative =
		    entry.buffer ? static_cast<int64_t>(static_cast<uint64_t>(immediate) & ~uint64_t {3})
		                 : (immediate & ~int64_t {3}) +
		                       static_cast<int64_t>(inst->Arg(1).Resolve().U32() & ~3u);
		entry.memo = inst->EvaluationIndex(program.evaluation_value_count);
		table.reads.push_back(entry);
		const auto index = static_cast<uint32_t>(table.reads.size() - 1u);
		indices.emplace(inst, index);
		return index;
	};
	for (uint32_t slot = 0; slot < slots.size(); ++slot) {
		if (slots[slot] != nullptr) program.srt_read_entries[slot] = read(read, slots[slot]);
	}
	for (uint32_t source = 0; source < descriptors.size(); ++source) {
		if (!descriptors[source]) continue;
		program.srt_descriptor_entries[source] = static_cast<uint32_t>(table.words.size());

		const auto& descriptor = program.descriptor_sources[source];
		for (uint32_t index = 0; index < descriptor.dword_count; ++index) {
			const auto value = descriptor.dwords[index].Resolve();
			if (value.IsImmediate()) {
				table.words.push_back(Table::Immediate);
				table.words.push_back(value.U32());
				continue;
			}
			const auto* inst = value.TryInstruction();
			table.words.push_back(IsUserDataWord(program, *inst)
			                          ? Table::UserData | (RegIndex(inst->Arg(0).ScalarRegister()) -
			                                               program.user_data_base)
			                          : read(read, inst));
		}
	}
	program.srt_read_addresses.resize(table.reads.size());
}

void CompileTreeWalker(ResourcePlan& program) {
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

} // namespace

void CompileSrtWalker(ResourcePlan& program) {
	CompileTreeWalker(program);
	CompileSrtReads(program, program.srt_code == nullptr);
}

SrtWalker::SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
                     SrtWalker* clean_evaluator)
    : m_program(program), m_runtime(runtime), m_clean_evaluator(clean_evaluator), m_session(this),
      m_read_policy(clean_evaluator != nullptr ? 2u : 0u), m_context(AcquireContext()) {
	// Compiled reads use mapped memory directly, as the ordinary reader of a capture does.
	// Child walkers carry an EXEC mask and keep the interpreted reads.
	m_captured_ranges = SrtReadCapture::DirectRanges(m_runtime);
	m_compiled_reads  = m_program.srt_read_table &&
	                    (m_runtime.read_memory == nullptr || m_captured_ranges != nullptr);
	if (m_compiled_reads) m_context.values.resize(m_program.evaluation_value_count);
}

SrtWalker::SrtWalker(SrtWalker& parent, Value active_mask, bool clean,
                     SrtWalker* clean_evaluator)
    : m_program(parent.m_program), m_runtime(clean ? CleanRuntime(parent.m_runtime) : parent.m_runtime),
      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()),
      m_reachable_blocks(parent.m_reachable_blocks), m_session(parent.m_session),
      m_read_policy((parent.m_read_policy & 1u) | (clean ? 1u : 0u) |
                    (clean_evaluator != nullptr ? 2u : 0u)), m_context(AcquireContext()) {}

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

// Each context lists its unknowns, so a reset does not depend on the size of the plan.
void SrtWalker::ResetUnknowns() {
	for (auto& context: m_program.evaluation_contexts) {
		if (context.session != m_session) continue;
		for (const auto index: context.unknowns)
			if (context.values[index].generation == (context.generation | 2u))
				context.values[index].generation = 0;
		context.unknowns.clear();
	}
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
	const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
	const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
	uint64_t   address   = 0;
	if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer || vector) {
		uint64_t records = 0;
		uint64_t word3   = 0;
		if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
			return false;
		}
		if (immediate < 0) {
			return false;
		}
		const auto byte_offset =
		    (static_cast<uint64_t>(immediate) & ~uint64_t {3}) + (static_cast<uint32_t>(offset) & ~3u);
		const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
		if (vector) {
			// Only the uniform, unswizzled structured DWORD address is evaluated on the host.
			if ((high & (1u << 31u)) != 0u || (word3 & ((1u << 23u) | 0xf0000000u)) != 0u)
				return false;
			if (stride == 0u || records == 0u || ((word3 >> 12u) & 0x7fu) == 0u) {
				result = 0u;
				return true;
			}
		}
		const auto size = stride == 0u
		                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
		                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
		if (byte_offset > size || size - byte_offset < sizeof(uint32_t)) {
			result = 0u;
			return true;
		}
		address = (base & ~uint64_t {3}) + byte_offset;
	} else {
		const auto relative = (immediate & ~int64_t {3}) +
		                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
		if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
			return false;
		}
	}
	uint32_t word = 0;
	const auto reader = vector ? m_runtime.read_specialization_memory : m_runtime.read_memory;
	if (reader != nullptr) {
		if (!reader(m_runtime.userdata, address, {&word, 1})) {
			return false;
		}
	} else {
		if (vector) return false;
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
			if (!m_active_mask.IsEmpty() && inst.Arg(1).Resolve() == m_active_mask) {
				return EvaluateWide(inst.Arg(0), result);
			}
			SrtWalker clean_active(*this, inst.Arg(1), true);
			SrtWalker active(*this, inst.Arg(1), false, &clean_active);
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
		case ValueOpcode::UMulHi:
			if (binary()) {
				result = (static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint32_t>(b)) >> 32u;
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
		case ValueOpcode::UndefU32: return false;
		default: break;
	}
	return false;
}

bool SrtWalker::CallCompiledRead(uint32_t entry, std::span<uint32_t> out, bool descriptor) {
	if (!m_compiled_reads) return false;
	using Table           = ResourcePlan::SrtReadTable;
	const auto& table     = *m_program.srt_read_table;
	auto* const addresses = m_program.srt_read_addresses.data();
	m_cursor              = addresses;
	bool ok               = true;
	if (!descriptor) {
		ok = CompiledRead(entry, out[0]);
	} else {
		// DWORDs are filled in order, as the interpreter does.
		for (auto word = table.words.begin() + entry; ok && !out.empty(); out = out.subspan(1)) {
			if (*word == Table::Immediate) {
				out[0] = word[1];
				word += 2;
			} else {
				ok = CompiledWord(*word++, out[0]);
			}
		}
	}
	if (m_captured_ranges != nullptr) {
		for (const auto* address = addresses; address != m_cursor; ++address) {
			m_captured_ranges->emplace_back(*address, sizeof(uint32_t));
		}
	}
	return ok;
}

bool SrtWalker::CompiledWord(uint32_t word, uint32_t& value) {
	using Table = ResourcePlan::SrtReadTable;
	if ((word & Table::UserData) == 0u) return CompiledRead(word, value);
	const auto index = word & ~Table::UserData;
	if (index >= m_runtime.user_data.size()) return false;
	value = m_runtime.user_data[index];
	return true;
}

// One memoized DWORD, read and recorded as EvaluateRawRead does. Words are evaluated in the
// walker's order: base, stride, offset, records, word 3.
bool SrtWalker::CompiledRead(uint32_t index, uint32_t& value) {
	const auto& read = m_program.srt_read_table->reads[index];
	if (const auto& memo = m_context.values[read.memo]; memo.generation == m_context.generation) {
		value = static_cast<uint32_t>(memo.value);
		return true;
	}
	std::array<uint32_t, 4> words {};
	if (!CompiledWord(read.words[0], words[0]) || !CompiledWord(read.words[1], words[1])) {
		return false;
	}
	uint64_t address = (uint64_t {words[1]} << 32u | words[0]) & AddressMask & ~uint64_t {3};
	if (read.buffer) {
		uint32_t offset = read.offset;
		if (read.shift != 0u) {
			if (!CompiledWord(read.offset, offset)) return false;
			offset = (offset << (read.shift - 1u)) & ~3u;
		}
		if (!CompiledWord(read.words[2], words[2]) || !CompiledWord(read.words[3], words[3])) {
			return false;
		}
		// The DWORD must fit in records, or stride * records bytes.
		const uint64_t stride = (words[1] >> 16u) & 0x3fffu;
		const uint64_t size   = stride != 0u ? uint64_t {words[2]} * stride : words[2];
		const uint64_t at     = offset + static_cast<uint64_t>(read.relative);
		if (size < sizeof(uint32_t) || size - sizeof(uint32_t) < at) return false;
		address += at;
	} else if (read.relative < 0) {
		const auto magnitude = static_cast<uint64_t>(-read.relative);
		if (address < magnitude) return false;
		address -= magnitude;
	} else if (read.relative > 0) {
		if (address > AddressMask - static_cast<uint64_t>(read.relative)) return false;
		address += static_cast<uint64_t>(read.relative);
	}
	*m_cursor++     = address;
	uint32_t loaded = 0;
	std::memcpy(&loaded, reinterpret_cast<const void*>(address), sizeof(loaded));
	auto& memo      = m_context.values[read.memo];
	memo.value      = loaded;
	memo.generation = m_context.generation;
	value           = loaded;
	return true;
}

bool SrtWalker::EvaluateDescriptor(uint32_t source, DescriptorValue& result) {
	if (source >= m_program.descriptor_sources.size()) {
		return false;
	}
	const auto& descriptor = m_program.descriptor_sources[source];
	result = {};
	result.dword_count = descriptor.dword_count;
	// A failed compiled read is evaluated again below, with the walker's own result.
	if (source < m_program.srt_descriptor_entries.size() &&
	    m_program.srt_descriptor_entries[source] != UINT32_MAX &&
	    CallCompiledRead(m_program.srt_descriptor_entries[source],
	                     std::span(result.dwords).first(descriptor.dword_count), true)) {
		return true;
	}
	result.dwords = {};
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
		// A failed compiled read is evaluated again below, with the walker's own result.
		if (slot < m_program.srt_read_entries.size() &&
		    m_program.srt_read_entries[slot] != UINT32_MAX &&
		    CallCompiledRead(m_program.srt_read_entries[slot],
		                     std::span(flat).subspan(m_program.srt_reads[slot].flat_offset, 1)))
			return true;
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
