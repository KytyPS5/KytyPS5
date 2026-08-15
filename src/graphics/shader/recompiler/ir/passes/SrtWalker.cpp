#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "graphics/shader/recompiler/ir/ValueProgram.h"

#include <algorithm>
#include <bit>
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

std::string Diagnostic(const Program& program, uint32_t pc, const std::string& message) {
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

bool IsRawRead(const ValueProgram& values, const Inst& inst) {
	if (inst.GetOpcode() != ValueOpcode::LoadAddressU32 &&
	    inst.GetOpcode() != ValueOpcode::ReadConstBuffer) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	return index < values.memory_info.size() &&
	       values.memory_info[index].kind == ResourceKind::ScalarBuffer;
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

bool IsRuntimeIntegerOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::PackUint2x32:
		case ValueOpcode::UnpackUint2x32:
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
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseOr64:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	RuntimeValidator(const Program& program, const ValueProgram& values)
	    : m_program(program), m_values(values) {}

	bool Run(Value value, std::string& reason) { return Validate(value, reason); }

private:
	bool Validate(Value value, std::string& reason) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64: return true;
				default:
					reason = fmt::format("contains a non-integer immediate of type {}",
					                     TypeName(value.GetType()));
					return false;
			}
		}
		if (!m_visiting.insert(inst).second) {
			reason = fmt::format("contains a cyclic {} value", ValueOpcodeName(inst->GetOpcode()));
			return false;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU8 ||
		    op == ValueOpcode::UndefU16 || op == ValueOpcode::UndefU32 ||
		    op == ValueOpcode::UndefU64 || op == ValueOpcode::Void) {
			reason = fmt::format("contains {}", ValueOpcodeName(op));
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				reason = "contains a malformed GetUserData";
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				reason = fmt::format("references unavailable user SGPR {}", reg);
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				reason = "contains a malformed GetShaderBase";
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::Phi) {
			const auto invariant = ResolveInvariantPhi(m_values, value);
			if (invariant.IsEmpty()) {
				reason = "contains a control-dependent phi";
				return finish(false);
			}
			return finish(Validate(invariant, reason));
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				reason = "contains a malformed GetSrtResource";
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
			    slot.U32() >= m_values.srt_reads.size()) {
				reason = "contains a malformed flattened SRT read";
				return finish(false);
			}
		} else if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_values, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				reason = fmt::format("contains a non-scalar {}", ValueOpcodeName(op));
				return finish(false);
			}
		} else if (op == ValueOpcode::PackUint2x32) {
			const auto* pair = inst->NumArgs() == 1 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (pair == nullptr || pair->GetOpcode() != ValueOpcode::CompositeConstructU32x2) {
				reason = "contains an unsupported packed runtime value";
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u ||
			    (source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 &&
			     source->GetOpcode() != ValueOpcode::UnpackUint2x32 &&
			     source->GetOpcode() != ValueOpcode::IAddCarry32)) {
				reason = "contains an unsupported composite runtime source";
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
				reason = fmt::format("contains a malformed {}", ValueOpcodeName(op));
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeIntegerOp(op)) {
			reason =
			    fmt::format("contains unsupported or control-dependent {}", ValueOpcodeName(op));
			return finish(false);
		}
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			if (!Validate(inst->Arg(index), reason)) {
				return finish(false);
			}
		}
		return finish(true);
	}

	const Program&                  m_program;
	const ValueProgram&             m_values;
	std::unordered_set<const Inst*> m_visiting;
};

class PlanBuilder {
public:
	PlanBuilder(Program& program, ValueProgram& values): m_program(program), m_values(values) {}

	bool Run(std::string* error) {
		m_values.srt_reads.clear();
		m_values.dynamic_reads.clear();
		for (auto* block: m_values.blocks) {
			for (auto& inst: *block) {
				if (IsDescriptorHandle(inst.GetOpcode())) {
					for (size_t index = 0; index < inst.NumArgs(); index++) {
						if (!Collect(inst.Arg(index), 0, error)) {
							return false;
						}
					}
				}
			}
		}
		for (auto* block: m_values.blocks) {
			for (auto& inst: *block) {
				std::string reason;
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 && IsRawRead(m_values, inst) &&
				    inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst), reason) &&
				    !Collect(Value(&inst), inst.Flags<MemoryFlags>().pc, error)) {
					return false;
				}
			}
		}
		PatchReads();
		return true;
	}

private:
	struct Patch {
		Inst*    inst = nullptr;
		uint32_t slot = 0;
		bool     keep = false;
	};

	bool Fail(uint32_t pc, std::string* error, const std::string& message) const {
		if (error != nullptr) {
			*error = Diagnostic(m_program, pc, message);
		}
		return false;
	}

	bool Collect(Value value, uint32_t use_pc, std::string* error) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return true;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return Fail(use_pc, error, "invalid typed planning value");
		}
		if (std::ranges::find(m_visiting, inst) != m_visiting.end()) {
			return inst->GetOpcode() == ValueOpcode::Phi;
		}
		if (std::ranges::find(m_visited, inst) != m_visited.end()) {
			return true;
		}
		m_visiting.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			if (!Collect(inst->Arg(index), use_pc, error)) {
				return false;
			}
		}
		m_visiting.pop_back();
		m_visited.push_back(inst);
		if (!IsRawRead(m_values, *inst)) {
			return true;
		}
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) {
			if (std::ranges::find(m_values.dynamic_reads, value) == m_values.dynamic_reads.end()) {
				m_values.dynamic_reads.push_back(value);
			}
			return true;
		}
		use_pc = inst->Flags<MemoryFlags>().pc;
		for (uint32_t slot = 0; slot < m_values.srt_reads.size(); slot++) {
			if (EquivalentValue(m_values, value, m_values.srt_reads[slot].value)) {
				m_patches.push_back({inst, slot, false});
				return true;
			}
		}
		const auto slot = static_cast<uint32_t>(m_values.srt_reads.size());
		m_values.srt_reads.push_back({value, slot, use_pc});
		m_patches.push_back({inst, slot, true});
		return true;
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
			for (auto& info: m_values.block_info) {
				if (info.condition.Resolve() == Value(patch.inst)) {
					info.condition = flat;
				}
				if (info.indirect_target.Resolve() == Value(patch.inst)) {
					info.indirect_target = flat;
				}
			}
			if (patch.keep) {
				const auto memory = patch.inst->Flags<MemoryFlags>().index;
				if (memory < m_values.memory_info.size()) {
					m_values.memory_info[memory].planning_only = true;
				}
				block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(patch.inst)});
			}
		}
	}

	Program&           m_program;
	ValueProgram&      m_values;
	std::vector<Inst*> m_visiting;
	std::vector<Inst*> m_visited;
	std::vector<Patch> m_patches;
};

class Evaluator {
public:
	Evaluator(const Program& program, const SrtRuntime& runtime)
	    : m_program(program), m_values(*program.values), m_runtime(runtime) {}

	void SetUsePc(uint32_t pc) { m_use_pc = pc; }

	bool Evaluate(Value value, uint32_t& result, std::string* error) {
		uint64_t wide = 0;
		if (!EvaluateWide(value, wide, error)) {
			return false;
		}
		result = static_cast<uint32_t>(wide);
		return true;
	}

private:
	bool Fail(std::string* error, const std::string& message) const {
		if (error != nullptr) {
			*error = Diagnostic(m_program, m_use_pc, message);
		}
		return false;
	}

	bool EvaluateWide(Value value, uint64_t& result, std::string* error) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: result = value.U1(); return true;
				case Type::U8: result = value.U8(); return true;
				case Type::U16: result = value.U16(); return true;
				case Type::U32: result = value.U32(); return true;
				case Type::U64: result = value.U64(); return true;
				default: return Fail(error, "non-integer immediate in runtime expression");
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return Fail(error, "invalid typed runtime value");
		}
		if (const auto found = m_cache.find(inst); found != m_cache.end()) {
			result = found->second;
			return true;
		}
		if (std::ranges::find(m_visiting, inst) != m_visiting.end()) {
			return Fail(error, "cyclic typed runtime value");
		}
		m_visiting.push_back(inst);
		uint64_t out = 0;
		if (!EvaluateInst(*inst, out, error)) {
			return false;
		}
		m_visiting.pop_back();
		m_cache.emplace(inst, out);
		result = out;
		return true;
	}

	bool Arg(const Inst& inst, size_t index, uint64_t& result, std::string* error) {
		return EvaluateWide(inst.Arg(index), result, error);
	}

	bool EvaluatePhi(const Inst& inst, uint64_t& result, std::string* error) {
		const auto value = ResolveInvariantPhi(m_values, Value(const_cast<Inst*>(&inst)));
		return !value.IsEmpty() ? EvaluateWide(value, result, error)
		                        : Fail(error, "typed phi has runtime-dependent values");
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result, std::string* error) {
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return Fail(error, "dynamic composite extract in runtime expression");
		}
		const auto  component = index.U32();
		const auto* source    = inst.Arg(0).ResolveInstruction();
		if (source == nullptr || component >= 2u) {
			return Fail(error, "unsupported composite runtime source");
		}
		if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			return EvaluateWide(source->Arg(component), result, error);
		}
		if (source->GetOpcode() == ValueOpcode::UnpackUint2x32) {
			uint64_t packed = 0;
			if (!Arg(*source, 0, packed, error)) {
				return false;
			}
			result = static_cast<uint32_t>(packed >> (component * 32u));
			return true;
		}
		if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
			uint64_t lhs = 0;
			uint64_t rhs = 0;
			if (!Arg(*source, 0, lhs, error) || !Arg(*source, 1, rhs, error)) {
				return false;
			}
			const auto sum =
			    static_cast<uint64_t>(static_cast<uint32_t>(lhs)) + static_cast<uint32_t>(rhs);
			result =
			    component == 0u ? static_cast<uint32_t>(sum) : static_cast<uint32_t>(sum >> 32u);
			return true;
		}
		return Fail(error, "unsupported composite runtime source");
	}

	bool EvaluateRawRead(const Inst& inst, uint64_t& result, std::string* error) {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_values.memory_info.size()) {
			return Fail(error, "raw scalar read has invalid metadata");
		}
		const auto& mem    = m_values.memory_info[flags.index];
		const auto* handle = inst.Arg(0).ResolveInstruction();
		if (handle == nullptr) {
			return Fail(error, "raw scalar read has no descriptor handle");
		}
		uint64_t low    = 0;
		uint64_t high   = 0;
		uint64_t offset = 0;
		if (!Arg(*handle, 0, low, error) || !Arg(*handle, 1, high, error) ||
		    !Arg(inst, 1, offset, error)) {
			return false;
		}
		const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		uint64_t   address   = 0;
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			uint64_t records = 0;
			uint64_t word3   = 0;
			if (handle->NumArgs() != 4u || !Arg(*handle, 2, records, error) ||
			    !Arg(*handle, 3, word3, error)) {
				return Fail(error, "constant-buffer descriptor has invalid width");
			}
			if (immediate < 0) {
				return Fail(error, "constant-buffer read has a negative immediate offset");
			}
			const auto byte_offset =
			    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
			const auto aligned = byte_offset & ~uint64_t {3};
			const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			const auto size = stride == 0u
			                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
			                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
			if (aligned > size || size - aligned < sizeof(uint32_t)) {
				return Fail(
				    error, fmt::format("constant-buffer offset {} exceeds size {}", aligned, size));
			}
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto relative = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
			if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
				return Fail(error, "raw scalar read is outside the 48-bit address space");
			}
		}
		uint32_t word = 0;
		if (m_runtime.read_memory != nullptr) {
			if (!m_runtime.read_memory(m_runtime.userdata, address, &word)) {
				return Fail(error, fmt::format("constant read failed at 0x{:016x}", address));
			}
		} else {
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		result = word;
		return true;
	}

	bool EvaluateInst(const Inst& inst, uint64_t& result, std::string* error) {
		uint64_t   a       = 0;
		uint64_t   b       = 0;
		uint64_t   c       = 0;
		const auto binary  = [&]() { return Arg(inst, 0, a, error) && Arg(inst, 1, b, error); };
		const auto ternary = [&]() {
			return Arg(inst, 0, a, error) && Arg(inst, 1, b, error) && Arg(inst, 2, c, error);
		};
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base ||
				    reg - m_program.user_data_base >= m_runtime.user_data.size()) {
					return Fail(error, fmt::format("user SGPR {} is unavailable", reg));
				}
				result = m_runtime.user_data[reg - m_program.user_data_base];
				return true;
			}
			case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
			case ValueOpcode::Phi: return EvaluatePhi(inst, result, error);
			case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result, error);
			case ValueOpcode::PackUint2x32: {
				const auto* pair = inst.Arg(0).ResolveInstruction();
				if (pair == nullptr || pair->GetOpcode() != ValueOpcode::CompositeConstructU32x2 ||
				    !Arg(*pair, 0, a, error) || !Arg(*pair, 1, b, error)) {
					return Fail(error, "unsupported packed runtime value");
				}
				result = static_cast<uint32_t>(a) |
				         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
				return true;
			}
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_values.srt_reads.size()) {
					return Fail(error, "invalid flattened SRT index");
				}
				return EvaluateWide(m_values.srt_reads[slot.U32()].value, result, error);
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				if (IsRawRead(m_values, inst)) {
					return EvaluateRawRead(inst, result, error);
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
			case ValueOpcode::BitwiseOr64:
				if (binary()) {
					result = a | b;
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
				if (Arg(inst, 0, a, error)) {
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
						return Fail(error, "invalid unsigned bit-field range");
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
						return Fail(error, "invalid signed bit-field range");
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
				if (!ternary() || !Arg(inst, 3, d, error)) {
					return false;
				}
				const auto offset = static_cast<uint32_t>(c);
				const auto width  = static_cast<uint32_t>(d);
				if (offset > 32u || width > 32u - offset) {
					return Fail(error, "invalid inserted bit-field range");
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
				if (Arg(inst, 0, a, error)) {
					result = a == 0u;
					return true;
				}
				return false;
			case ValueOpcode::UndefU1:
			case ValueOpcode::UndefU8:
			case ValueOpcode::UndefU16:
			case ValueOpcode::UndefU32:
			case ValueOpcode::UndefU64: return Fail(error, "undefined typed runtime value");
			default: break;
		}
		return Fail(error, fmt::format("unsupported typed runtime opcode {}",
		                               ValueOpcodeName(inst.GetOpcode())));
	}

	const Program&                            m_program;
	const ValueProgram&                       m_values;
	const SrtRuntime&                         m_runtime;
	uint32_t                                  m_use_pc = 0;
	std::unordered_map<const Inst*, uint64_t> m_cache;
	std::vector<const Inst*>                  m_visiting;
};

const DescriptorSource* Source(const Program& program, uint32_t source) {
	if (program.values == nullptr || source >= program.values->descriptor_sources.size()) {
		return nullptr;
	}
	return &program.values->descriptor_sources[source];
}

bool EvaluateRuntimeSourcesImpl(const Program&                           program,
                                std::span<const DescriptorSourceRequest> requests,
                                const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                                std::vector<uint32_t>& flat, bool evaluate_flat,
                                std::string* error) {
	if (program.values == nullptr || !program.srt_plan_complete) {
		if (error != nullptr) {
			*error = Diagnostic(program, 0, "typed SRT plan is not ready");
		}
		return false;
	}
	Evaluator                    evaluator(program, runtime);
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(requests.size());
	for (const auto& request: requests) {
		const auto* source = Source(program, request.source);
		if (source == nullptr) {
			if (error != nullptr) {
				*error = Diagnostic(program, request.use_pc,
				                    fmt::format("invalid descriptor source {}", request.source));
			}
			return false;
		}
		evaluator.SetUsePc(request.use_pc);
		DescriptorValue value;
		value.dword_count = source->dword_count;
		for (uint32_t index = 0; index < source->dword_count; index++) {
			if (!evaluator.Evaluate(source->dwords[index], value.dwords[index], error)) {
				return false;
			}
		}
		evaluated.push_back(value);
	}
	std::vector<uint32_t> flattened;
	if (evaluate_flat) {
		flattened.resize(program.values->srt_reads.size());
		for (const auto& read: program.values->srt_reads) {
			evaluator.SetUsePc(read.use_pc);
			if (read.flat_offset >= flattened.size() ||
			    !evaluator.Evaluate(read.value, flattened[read.flat_offset], error)) {
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

} // namespace

bool ValidateRuntimeValue(const Program& program, Value value, std::string& reason) {
	if (program.values == nullptr) {
		reason = "typed SSA is not ready";
		return false;
	}
	return RuntimeValidator(program, *program.values).Run(value, reason);
}

bool BuildSrtPlan(Program& program, std::string* error) {
	if (program.values == nullptr || program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = program.values == nullptr ? "typed SSA is not ready"
			                                   : "cannot rebuild SRT after resource tracking";
		}
		return false;
	}
	program.srt_plan_complete = false;
	if (!PlanBuilder(program, *program.values).Run(error)) {
		return false;
	}
	program.srt_plan_complete = true;
	return true;
}

bool EvaluateDescriptorSource(const Program& program, uint32_t source, uint32_t use_pc,
                              const SrtRuntime& runtime, DescriptorValue& result,
                              std::string* error) {
	const DescriptorSourceRequest request {source, use_pc};
	std::vector<DescriptorValue>  results;
	if (!EvaluateDescriptorSources(program, std::span {&request, 1}, runtime, results, error)) {
		return false;
	}
	result = results.front();
	return true;
}

bool EvaluateDescriptorSources(const Program&                           program,
                               std::span<const DescriptorSourceRequest> requests,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                               std::string* error) {
	std::vector<uint32_t> ignored;
	return EvaluateRuntimeSourcesImpl(program, requests, runtime, results, ignored, false, error);
}

bool EvaluateRuntimeSources(const Program&                           program,
                            std::span<const DescriptorSourceRequest> requests,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::string* error) {
	return EvaluateRuntimeSourcesImpl(program, requests, runtime, results, flat, true, error);
}

bool WalkSrt(const Program& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
             std::string* error) {
	std::vector<DescriptorValue> ignored;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, error);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
