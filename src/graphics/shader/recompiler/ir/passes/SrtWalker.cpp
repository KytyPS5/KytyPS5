#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#include "graphics/host_gpu/hostMemory.h"
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
	explicit RuntimeValidator(const ResourcePlan& program, Value active_mask = {})
	    : m_program(program), m_active_mask(active_mask.Resolve()) {}

	bool Run(Value value) { return Validate(value); }

private:
	bool Validate(Value value) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
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
		if (!m_visiting.insert(inst).second) {
			return false;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
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
			return finish(RuntimeValidator(m_program, inst->Arg(1)).Run(inst->Arg(0)));
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
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
		} else if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
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
		if (op == ValueOpcode::GetBufferResource || op == ValueOpcode::GetImageResource ||
		    op == ValueOpcode::GetSamplerResource || op == ValueOpcode::GetAddressResource) {
			const size_t expected = op == ValueOpcode::GetBufferResource    ? 4u
			                        : op == ValueOpcode::GetImageResource   ? 8u
			                        : op == ValueOpcode::GetSamplerResource ? 4u
			                                                                : 2u;
			if (inst->NumArgs() != expected) {
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			if (!Validate(inst->Arg(index))) {
				return finish(false);
			}
		}
		return finish(true);
	}

	const ResourcePlan&             m_program;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
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
		if (!EvaluateInst(*inst, out)) {
			return false;
		}
		m_visiting.pop_back();
		m_cache.emplace(inst, out);
		result = out;
		return true;
	}

	bool Arg(const Inst& inst, size_t index, uint64_t& result) {
		return EvaluateWide(inst.Arg(index), result);
	}

	bool EvaluatePhi(const Inst& inst, uint64_t& result) {
		const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
		return !value.IsEmpty() && EvaluateWide(value, result);
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result) {
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

	bool EvaluateRawRead(const Inst& inst, uint64_t& result) {
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
			if (!m_runtime.read_memory(m_runtime.userdata, address, &word)) {
				const auto message = Diagnostic(m_program, flags.pc,
				    fmt::format("cannot read descriptor memory at guest address 0x{:016x}", address));
				std::fprintf(stderr, "%s\n", message.c_str());
				return false;
			}
		} else {
			if (!HostMemoryRangeIsReadable(address, sizeof(word))) {
				return false;
			}
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
					return false;
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
			case ValueOpcode::UndefU64: return false;
			default: break;
		}
		return false;
	}

	const ResourcePlan&                       m_program;
	const SrtRuntime&                         m_runtime;
	std::span<const uint8_t>                  m_clean_flat_slots;
	Evaluator*                                m_clean_evaluator = nullptr;
	Value                                     m_active_mask;
	std::unordered_map<const Inst*, uint64_t> m_cache;
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
                                std::span<const uint8_t> clean_flat_slots) {
	if (!program.srt_plan_complete) {
		return false;
	}
	if (std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; }) &&
	    runtime.read_specialization_memory == nullptr) {
		return false;
	}
	SrtRuntime clean_runtime  = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	Evaluator                    clean_evaluator(program, clean_runtime);
	Evaluator                    evaluator(program, runtime, clean_flat_slots, &clean_evaluator);
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto* source = Source(program, source_index);
		if (source == nullptr) {
			return false;
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		for (uint32_t index = 0; index < source->dword_count; index++) {
			if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
				const auto message = Diagnostic(program, 0u,
				    fmt::format("descriptor source {} dword {} evaluation failed", source_index, index));
				std::fprintf(stderr, "%s\n", message.c_str());
				return false;
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
				const auto message = Diagnostic(
				    program, 0u,
				    fmt::format("flattened SRT slot {} evaluation failed (clean={})",
				                read.flat_offset, clean ? 1u : 0u));
				std::fprintf(stderr, "%s\n", message.c_str());
				return false;
			}
		}
	}
	results = std::move(evaluated);
	if (evaluate_flat) {
		flat = std::move(flattened);
	}
	return true;
}

// Accept one induction value, finite selector or exact WorkgroupId axis. Arithmetic stays modulo 2^32;
// the SMEM instruction's signed immediate is intentionally not folded here.
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
		if (m_program.stage != ShaderType::Compute || m_program.dispatcher_fallback || m_program.blocks.empty() ||
		    m_program.blocks.size() != m_program.block_info.size())
			return {};
		const auto opcode = read.GetOpcode();
		const bool address_read = opcode == ValueOpcode::LoadAddressU32 && read.NumArgs() == 4u;
		const bool buffer_read = opcode == ValueOpcode::ReadConstBuffer && read.NumArgs() == 2u;
		if (!address_read && !buffer_read) return {};
		const auto flags = read.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) return {};
		const auto& memory = m_program.memory_info[flags.index];
		const auto expected_kind = address_read ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer;
		if (memory.kind != expected_kind || memory.planning_only ||
		    memory.data_dwords != 1u || memory.data_bits != 32u ||
		    (address_read && (!Immediate(read.Arg(2), 0u) || read.Arg(3).Resolve() != Value(true)))) return {};
		const auto* address = read.Arg(0).Resolve().TryInstruction();
		const uint32_t source_dwords = address_read ? 2u : 4u;
		const auto expected_handle = address_read ? ValueOpcode::GetAddressResource : ValueOpcode::GetBufferResource;
		if (address == nullptr || address->GetOpcode() != expected_handle ||
		    address->NumArgs() != source_dwords) return {};
		for (uint32_t word = 0; word < source_dwords; ++word)
			if (!ValidateRuntimeValue(m_program, address->Arg(word))) return {};
		BoundedOffset offset;
		std::unordered_set<const Inst*> visiting;
		if (!ParseBoundedOffset(read.Arg(1), offset, visiting) || offset.index == nullptr) {
			return {};
		}
		if (!BuildGraph()) return {};
		const bool workgroup = offset.index->GetOpcode() == ValueOpcode::GetBuiltin;
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
		if (workgroup || maximum.has_value()) {
			// The dense enclosure includes every possible GPU value. Keep the key
			// live; only its constant bound is evaluated on the host. Existing
			// materialization budgets and coherent-read checks apply to every entry.
			if (maximum == UINT32_MAX) return {}; // max+1 must not wrap to an empty table.
			if (!Dominates(offset.index->Parent(), read.Parent()) ||
			    (offset.index->Parent() == read.Parent() && !Precedes(*offset.index, read))) return {};
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
			for (uint32_t word = 0; word < source_dwords; ++word)
				if (!RuntimeReadsDominate(address->Arg(word), source_scope, &read)) return {};
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
			}
		}
		if (initial == nullptr || latch == nullptr || initial == latch) return {};
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
		const auto index = Value(const_cast<Inst*>(phi));
		Value count;
		if (compare->GetOpcode() == ValueOpcode::ULessThan32 && compare->Arg(0).Resolve() == index)
			count = compare->Arg(1).Resolve();
		else if (compare->GetOpcode() == ValueOpcode::UGreaterThan32 && compare->Arg(1).Resolve() == index)
			count = compare->Arg(0).Resolve();
		else return {};
		if (count.GetType() != Type::U32 || !ValidateRuntimeValue(m_program, count)) return {};
		// Roots may be loaded in the preheader or in this unavoidable guard
		// chain. Both execute even when N is zero; success-only pointer loads
		// remain ineligible for eager snapshot evaluation.
		if (!RuntimeReadsDominate(count, guard)) return {};
		for (uint32_t word = 0; word < source_dwords; ++word)
			if (!RuntimeReadsDominate(address->Arg(word), guard)) return {};
		const auto success_id = invert ? info.terminator.false_block : info.terminator.true_block;
		const auto* success = m_by_id.at(success_id);
		// Removing the successful i<N edge must make the actual read unreachable.
		// Block dominance alone is insufficient when the guard's false path merges.
		if (Reachable(read.Parent(), nullptr, guard, success)) return {};
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
		    .memory_offset = memory.offset};
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
		if (!m_finite_visiting.insert(inst).second) return {};
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
		    inst->NumArgs() == inst->Parent()->ImmPredecessors().size()) {
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

	bool ProveActiveMaskNonempty(const Inst& read_first_lane, Value active) const {
		if (read_first_lane.Parent() == nullptr || !m_ids.contains(read_first_lane.Parent())) return false;
		std::unordered_set<const Block*> graph_active;
		std::unordered_set<const Block*> graph_complete;
		if (GraphHasCycle(m_program.blocks.front(), graph_active, graph_complete)) return false;
		Value low, high;
		if (!ParseThreadBit(active, low, high)) return false;
		std::unordered_set<const Inst*> low_visiting;
		std::unordered_set<const Inst*> high_visiting;
		const auto low_proof = ProveMaskWord(low, 0u, low_visiting);
		const auto high_proof = ProveMaskWord(high, 1u, high_visiting);
		if (!low_proof.uniform || !low_proof.subset || !high_proof.uniform || !high_proof.subset)
			return false;
		for (const auto* guard : m_program.blocks) {
			const auto& info = m_program.block_info[m_ids.at(guard)];
			if (info.terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
			    !IsZeroMaskGuard(info.condition, low, high)) continue;
			const auto* nonempty = m_by_id.at(info.terminator.false_block);
			if (!PostDominates(guard, m_program.blocks.front()) ||
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
	bool RuntimeReadsDominate(Value root, const Block* header, const Inst* before = nullptr) const {
		std::vector<Value> work {root};
		std::unordered_set<const Inst*> visited;
		while (!work.empty()) {
			const auto value = work.back().Resolve();
			work.pop_back();
			const auto* inst = value.TryInstruction();
			if (inst == nullptr || !visited.insert(inst).second) continue;
			if (inst->GetOpcode() == ValueOpcode::ReadConst) {
				const auto slot = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) return false;
				work.push_back(m_program.srt_reads[slot.U32()].value);
			}
			if (inst->GetOpcode() == ValueOpcode::LoadAddressU32 ||
			    inst->GetOpcode() == ValueOpcode::ReadConstBuffer) {
				if (inst->Parent() == nullptr || !Dominates(inst->Parent(), header)) return false;
				if (before != nullptr && (!Dominates(inst->Parent(), before->Parent()) ||
				    (inst->Parent() == before->Parent() && !Precedes(*inst, *before)))) return false;
			}
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) work.push_back(inst->Arg(arg));
		}
		return true;
	}
	bool BuildGraph() {
		for (uint32_t i = 0; i < m_program.blocks.size(); ++i) {
			if (m_program.blocks[i] == nullptr || !m_ids.emplace(m_program.blocks[i], i).second ||
			    m_program.block_info[i].id == UINT32_MAX ||
			    !m_by_id.emplace(m_program.block_info[i].id, m_program.blocks[i]).second) return false;
		}
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

bool ValidateRuntimeValue(const ResourcePlan& program, Value value) {
	return RuntimeValidator(program).Run(value);
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
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {});
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>&   flat,
                            std::span<const uint8_t> clean_flat_slots) {
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true,
	                                  clean_flat_slots);
}

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime,
             std::vector<uint32_t>& flat) {
	std::vector<DescriptorValue> ignored;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {});
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
