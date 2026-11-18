#include "graphics/shader/recompiler/ir/passes/ParameterKeys.h"

#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/Reg.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;
// Larger vertex buffers or key sets are refused rather than scanned.
constexpr uint64_t MaxRecordBytes = 256ull << 20u;
constexpr size_t   MaxKeys        = MaxIndirectDescriptorProbes;

bool IsBufferLoad(ValueOpcode opcode) {
	return opcode == ValueOpcode::LoadBufferU32 || opcode == ValueOpcode::LoadBufferU32x2 ||
	       opcode == ValueOpcode::LoadBufferU32x3 || opcode == ValueOpcode::LoadBufferU32x4;
}

uint32_t LoadComponents(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::LoadBufferU32x2: return 2u;
		case ValueOpcode::LoadBufferU32x3: return 3u;
		case ValueOpcode::LoadBufferU32x4: return 4u;
		default: return 1u;
	}
}

bool IsVertexIndex(const Inst& inst) {
	return inst.GetOpcode() == ValueOpcode::GetBuiltin &&
	       inst.Arg(0).Resolve() == Value(static_cast<uint32_t>(StageInputKind::VertexIndex)) &&
	       inst.Arg(1).Resolve() == Value(0u);
}

class KeyCloner {
public:
	KeyCloner(const Program& program, ResourcePlan& plan, Value exec)
	    : m_program(program), m_plan(plan), m_exec(exec.Resolve()) {}

	Value Clone(Value value, uint32_t depth = 0) {
		if (!m_ok || depth > 256u) return Fail();
		value = value.Resolve();
		if (value.IsImmediate()) return value;
		// Only lanes of the export's EXEC write the parameter.
		if (value.GetType() == Type::U1 && Exec(value)) return Value(true);
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return Fail();
		const auto opcode = inst->GetOpcode();
		if ((opcode == ValueOpcode::SelectU32 || opcode == ValueOpcode::SelectU1) &&
		    Exec(inst->Arg(0)))
			return Clone(inst->Arg(1), depth + 1u);
		if (opcode == ValueOpcode::ReadConst) {
			// A scalar read the SRT plan retains, as ExtractResourcePlan resolves it.
			uint32_t   slot  = 0;
			const auto index = inst->Arg(1).Resolve();
			if (!index.IsImmediate() || index.GetType() != Type::U32 ||
			    (slot = index.U32()) >= m_program.srt_reads.size())
				return Fail();
			return Clone(m_program.srt_reads[slot].value, depth + 1u);
		}
		if (opcode == ValueOpcode::Phi) {
			const auto invariant = ResolveInvariantPhi(m_program, value);
			return invariant.IsEmpty() || invariant == value ? Fail()
			                                                 : Clone(invariant, depth + 1u);
		}
		if (!IsParameterKeyOpcode(opcode) ||
		    (opcode == ValueOpcode::GetBuiltin && !IsVertexIndex(*inst))) {
			return Fail();
		}
		if (const auto found = m_cloned.find(inst); found != m_cloned.end()) return found->second;
		auto&      target = m_plan.value_storage.emplace_back(opcode, inst->Flags<uint64_t>());
		const auto result = Value(&target);
		m_cloned.emplace(inst, result);
		for (size_t index = 0; index < inst->NumArgs(); ++index) {
			target.SetArg(index, Clone(inst->Arg(index), depth + 1u));
		}
		if (opcode == ValueOpcode::LoadAddressU32) {
			const auto& memory = m_program.memory_info.at(inst->Flags<MemoryFlags>().index);
			if (memory.kind != ResourceKind::ScalarAddress || inst->Arg(2).Resolve() != Value(0u))
				return Fail();
		}
		if (IsBufferLoad(opcode)) {
			const auto& memory = m_program.memory_info.at(inst->Flags<MemoryFlags>().index);
			// The evaluator reads unswizzled 32-bit buffer words; it decodes integer formats only.
			if (memory.kind != ResourceKind::Buffer || memory.data_bits != 32u ||
			    inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetBufferResource)
				return Fail();
		}
		return m_ok ? result : Value {};
	}

	[[nodiscard]] bool Ok() const { return m_ok; }

private:
	Value Fail() {
		m_ok = false;
		return {};
	}

	bool Exec(Value value) const {
		return !m_exec.IsEmpty() && EquivalentValue(m_program, value, m_exec);
	}

	const Program&                         m_program;
	ResourcePlan&                          m_plan;
	Value                                  m_exec;
	std::unordered_map<const Inst*, Value> m_cloned;
	bool                                   m_ok = true;
};

// The vertex index may only select buffer records; anything else leaves its domain unbounded.
bool VertexIndexOnlySelectsRecords(Value value) {
	std::vector<const Inst*> pending {value.TryInstruction()};
	std::vector<const Inst*> seen;
	while (!pending.empty()) {
		const auto* inst = pending.back();
		pending.pop_back();
		if (inst == nullptr || std::ranges::find(seen, inst) != seen.end()) continue;
		seen.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); ++index) {
			const auto* arg = inst->Arg(index).Resolve().TryInstruction();
			if (arg == nullptr) continue;
			if (IsVertexIndex(*arg) && (!IsBufferLoad(inst->GetOpcode()) || index != 1u))
				return false;
			pending.push_back(arg);
		}
	}
	return true;
}

struct Slot {
	std::array<uint32_t, 4> words {};
};

class KeyEvaluator {
public:
	KeyEvaluator(const ResourcePlan& plan, const SrtRuntime& runtime,
	             std::vector<std::pair<uint64_t, uint64_t>>& reads)
	    : m_plan(plan), m_runtime(runtime), m_reads(reads) {}

	// The uniform node values the keys depend on, besides the record buffers.
	bool Signature(Value root, std::vector<uint32_t>& signature) {
		root             = root.Resolve();
		const auto* inst = root.TryInstruction();
		if (inst == nullptr) return root.IsImmediate();
		if (!Order(inst)) return false;
		for (const auto* node: m_order) {
			if (m_varying.at(node)) continue;
			if (!Evaluate(*node, 0u)) return false;
			const auto& words = m_slots.at(node).words;
			signature.insert(signature.end(), words.begin(), words.end());
		}
		return true;
	}

	bool Run(Value root, std::vector<uint32_t>& keys) {
		root = root.Resolve();
		if (root.IsImmediate()) {
			if (root.GetType() != Type::U32) return false;
			keys.push_back(root.U32());
			return true;
		}
		const auto* inst = root.TryInstruction();
		if (inst == nullptr || !Order(inst)) return false;
		// Uniform nodes once; the varying ones per record, the last record lying past every buffer.
		for (const auto* node: m_order) {
			if (!m_varying.at(node) && !Evaluate(*node, 0u)) return false;
		}
		uint64_t records = 0;
		for (const auto* node: m_order) {
			if (!IsBufferLoad(node->GetOpcode()) || !m_varying.at(node)) continue;
			const auto* handle = node->Arg(0).Resolve().TryInstruction();
			auto&       buffer = m_buffers[node];
			if (handle == nullptr || m_varying.at(handle) ||
			    (!buffer.valid && !Prepare(*node, buffer)))
				return false;
			records = std::max<uint64_t>(
			    records, buffer.resource.Stride() == 0u ? 1u : buffer.resource.NumRecords());
		}
		const bool varying = m_varying.at(inst);
		for (uint64_t record = 0; record <= (varying ? records : 0u); ++record) {
			for (const auto* node: m_order) {
				if (m_varying.at(node) && !Evaluate(*node, static_cast<uint32_t>(record)))
					return false;
			}
			keys.push_back(m_slots.at(inst).words[0]);
			if (keys.size() > MaxKeys * 4u) {
				std::ranges::sort(keys);
				keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
				if (keys.size() > MaxKeys) return false;
			}
		}
		return true;
	}

private:
	struct Buffer {
		ShaderBufferResource  resource;
		std::vector<uint32_t> words; // The record bytes, from the descriptor base.
		bool                  valid = false;
	};

	// Arguments before users; a node varies when it depends on the vertex index.
	bool Order(const Inst* root) {
		std::vector<std::pair<const Inst*, bool>> stack {{root, false}};
		while (!stack.empty()) {
			const auto [inst, expanded] = stack.back();
			stack.pop_back();
			if (m_varying.contains(inst)) continue;
			if (expanded) {
				bool varying = IsVertexIndex(*inst);
				for (size_t index = 0; index < inst->NumArgs(); ++index) {
					const auto* arg = inst->Arg(index).Resolve().TryInstruction();
					if (arg != nullptr) varying |= m_varying.at(arg);
				}
				m_varying.emplace(inst, varying);
				m_order.push_back(inst);
				continue;
			}
			if (!IsParameterKeyOpcode(inst->GetOpcode()) || m_order.size() > 4096u) return false;
			stack.emplace_back(inst, true);
			for (size_t index = 0; index < inst->NumArgs(); ++index) {
				const auto* arg = inst->Arg(index).Resolve().TryInstruction();
				if (arg != nullptr && !m_varying.contains(arg)) stack.emplace_back(arg, false);
			}
		}
		return true;
	}

	uint32_t Word(Value value, uint32_t component = 0) const {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return value.GetType() == Type::U1 ? static_cast<uint32_t>(value.U1()) : value.U32();
		}
		return m_slots.at(value.TryInstruction()).words[component];
	}

	// Record buffer reads are reported; the signature covers uniform ones.
	bool Read(uint64_t address, std::span<uint32_t> words, bool record) {
		if (m_runtime.read_specialization_memory == nullptr ||
		    !m_runtime.read_specialization_memory(m_runtime.userdata, address, words))
			return false;
		if (record) m_reads.emplace_back(address, words.size_bytes());
		return true;
	}

	bool Evaluate(const Inst& inst, uint32_t record) {
		auto&      slot   = m_slots[&inst];
		const auto opcode = inst.GetOpcode();
		switch (opcode) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_plan.user_data_base ||
				    reg - m_plan.user_data_base >= m_runtime.user_data.size())
					return false;
				slot.words[0] = m_runtime.user_data[reg - m_plan.user_data_base];
				return true;
			}
			case ValueOpcode::GetBuiltin: slot.words[0] = record; return true;
			case ValueOpcode::GetAddressResource:
				slot.words[0] = Word(inst.Arg(0));
				slot.words[1] = Word(inst.Arg(1));
				return true;
			case ValueOpcode::GetBufferResource:
				for (uint32_t word = 0; word < 4u; ++word)
					slot.words[word] = Word(inst.Arg(word));
				return true;
			case ValueOpcode::LoadAddressU32: {
				// Mirrors the SRT walker: DWORD-aligned base, offset and immediate within 48 bits.
				const auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || !Word(inst.Arg(3))) {
					slot.words[0] = 0u;
					return handle != nullptr;
				}
				const auto& words  = m_slots.at(handle).words;
				const auto  memory = m_plan.memory_info.at(inst.Flags<MemoryFlags>().index);
				const auto  base = ((uint64_t {words[1]} << 32u) | (words[0] & ~3u)) & AddressMask;
				const auto  address =
				    base + (Word(inst.Arg(1)) & ~3u) +
				    static_cast<uint64_t>(
				        static_cast<int64_t>(static_cast<int32_t>(memory.offset)) & ~int64_t {3});
				return (address >> 48u) == 0u &&
				       Read(address, std::span(slot.words).first(1), false);
			}
			case ValueOpcode::CompositeExtractU32x2:
			case ValueOpcode::CompositeExtractU32x3:
			case ValueOpcode::CompositeExtractU32x4: {
				const auto component = inst.Arg(1).Resolve();
				if (!component.IsImmediate() || component.U32() >= 4u) return false;
				slot.words[0] = Word(inst.Arg(0), component.U32());
				return true;
			}
			default:
				if (IsBufferLoad(opcode)) return LoadBuffer(inst, slot);
				break;
		}
		std::array<uint32_t, 4> args {};
		if (inst.NumArgs() > args.size()) return false;
		for (size_t index = 0; index < inst.NumArgs(); ++index)
			args[index] = Word(inst.Arg(index));
		return EvaluateParameterKeyOp(opcode, std::span(args).first(inst.NumArgs()), slot.words[0]);
	}

	// RDNA2 MUBUF loads as the SPIR-V emitter performs them: out-of-range words and formats read
	// their dst_sel constants.
	bool LoadBuffer(const Inst& inst, Slot& slot) {
		slot.words.fill(0u);
		if (!Word(inst.Arg(4))) return true;
		auto& buffer = m_buffers[&inst];
		if (!buffer.valid && !Prepare(inst, buffer)) return false;
		const auto& memory     = m_plan.memory_info.at(inst.Flags<MemoryFlags>().index);
		const auto  components = LoadComponents(inst.GetOpcode());
		const auto  stride     = uint64_t {buffer.resource.Stride()};
		const auto  start      = uint64_t {Word(inst.Arg(1))} * stride + Word(inst.Arg(2)) +
		                         Word(inst.Arg(3)) + memory.offset;
		const auto  size       = buffer.resource.GetSize();
		const auto  word_at    = [&](uint64_t byte) {
			return byte / 4u < buffer.words.size() ? buffer.words[byte / 4u] : 0u;
		};
		const auto format =
		    memory.typed ? Format::DecodeTBufferFormat(memory.data_format, memory.number_format)
		                 : buffer.resource.Format();
		const auto info =
		    Format::GetFormatInfo(memory.formatted ? format : Prospero::BufferFormat::kInvalid);
		if (info.type == Format::ComponentType::Unknown) {
			for (uint32_t component = 0; component < components; ++component) {
				const auto byte       = (start & ~uint64_t {3}) + component * 4u;
				slot.words[component] = byte + 4u <= size ? word_at(byte) : 0u;
			}
			return true;
		}
		// Normalized and float conversions are not exact on the host GPU or need not match it.
		const bool exact =
		    info.type == Format::ComponentType::Uint || info.type == Format::ComponentType::Sint ||
		    (info.type == Format::ComponentType::Float && info.component_count != 0u &&
		     std::all_of(info.component_bits, info.component_bits + info.component_count,
		                 [](uint32_t bits) { return bits == 32u; }));
		if (!exact) return false;
		const bool in_bounds = start + info.byte_size <= size;
		const auto base      = start & ~uint64_t {std::min(4u, info.byte_size) - 1u};
		for (uint32_t output = 0; output < components; ++output) {
			const auto source =
			    memory.typed
			        ? (output < info.component_count
			               ? Format::FormattedSource {Format::FormattedSourceKind::Memory, output}
			               : Format::FormattedSource {})
			        : Format::ResolveFormattedSource(
			              info, GetDstSel(buffer.resource.DstSelXYZW(), output));
			if (source.kind == Format::FormattedSourceKind::Invalid) return false;
			if (source.kind != Format::FormattedSourceKind::Memory || !in_bounds) {
				slot.words[output] = Format::FormattedConstantBits(info, source.kind);
				continue;
			}
			const auto component = source.component;
			const auto bits      = info.component_bits[component];
			const auto byte      = base + Format::GetFormatComponentByteOffset(info, component);
			uint32_t   raw       = word_at(byte & ~uint64_t {3});
			const auto shift     = info.packed_bitfield ? info.component_bit_offset[component]
			                                            : static_cast<uint32_t>((byte & 3u) * 8u);
			if (bits < 32u) {
				raw = (raw >> shift) & ((1u << bits) - 1u);
				if (info.type == Format::ComponentType::Sint && (raw >> (bits - 1u)) != 0u)
					raw |= ~((1u << bits) - 1u);
			}
			slot.words[output] = raw;
		}
		return true;
	}

	bool Prepare(const Inst& inst, Buffer& buffer) {
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr) return false;
		std::copy_n(m_slots.at(handle).words.begin(), 4u, buffer.resource.fields);
		const auto& resource = buffer.resource;
		// Only plain, unswizzled buffers are evaluated, as the walker does.
		if (resource.SwizzleEnabled() || resource.AddTid() || resource.Type() != 0u ||
		    (resource.Base48() & 3u) != 0u)
			return false;
		const auto size = resource.GetSize();
		if (size > MaxRecordBytes || resource.Base48() + size > AddressMask) return false;
		buffer.words.resize((size + 3u) / 4u);
		if (!buffer.words.empty() && !Read(resource.Base48(), buffer.words, true)) return false;
		buffer.valid = true;
		return true;
	}

	const ResourcePlan&                         m_plan;
	const SrtRuntime&                           m_runtime;
	std::vector<std::pair<uint64_t, uint64_t>>& m_reads;
	std::vector<const Inst*>                    m_order;
	std::unordered_map<const Inst*, bool>       m_varying;
	std::unordered_map<const Inst*, Slot>       m_slots;
	std::unordered_map<const Inst*, Buffer>     m_buffers;
};

} // namespace

bool IsParameterKeyOpcode(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetUserData:
		case ValueOpcode::GetBuiltin:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::LoadBufferU32x4:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::IAdd32:
		case ValueOpcode::ISub32:
		case ValueOpcode::IMul32:
		case ValueOpcode::UMulHi:
		case ValueOpcode::SMulHi:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::UMin32:
		case ValueOpcode::UMax32:
		case ValueOpcode::SMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::IEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot: return true;
		default: return false;
	}
}

bool EvaluateParameterKeyOp(ValueOpcode opcode, std::span<const uint32_t> args, uint32_t& result) {
	const auto a  = args.size() > 0u ? args[0] : 0u;
	const auto b  = args.size() > 1u ? args[1] : 0u;
	const auto c  = args.size() > 2u ? args[2] : 0u;
	const auto sa = static_cast<int32_t>(a);
	const auto sb = static_cast<int32_t>(b);
	switch (opcode) {
		case ValueOpcode::IAdd32: result = a + b; return true;
		case ValueOpcode::ISub32: result = a - b; return true;
		case ValueOpcode::IMul32: result = a * b; return true;
		case ValueOpcode::UMulHi:
			result = static_cast<uint32_t>((uint64_t {a} * b) >> 32u);
			return true;
		case ValueOpcode::SMulHi:
			result = static_cast<uint32_t>(static_cast<uint64_t>(int64_t {sa} * sb) >> 32u);
			return true;
		case ValueOpcode::ShiftLeftLogical32: result = a << (b & 31u); return true;
		case ValueOpcode::ShiftRightLogical32: result = a >> (b & 31u); return true;
		case ValueOpcode::ShiftRightArithmetic32:
			result = static_cast<uint32_t>(sa >> (b & 31u));
			return true;
		case ValueOpcode::BitwiseAnd32: result = a & b; return true;
		case ValueOpcode::BitwiseOr32: result = a | b; return true;
		case ValueOpcode::BitwiseXor32: result = a ^ b; return true;
		case ValueOpcode::BitwiseNot32: result = ~a; return true;
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract: {
			// Only fields that lie within the word have a host result that matches.
			if (b > 32u || c > 32u - b) return false;
			if (c == 0u) {
				result = 0u;
				return true;
			}
			const auto field = c == 32u ? a : (a >> b) & ((1u << c) - 1u);
			const bool sign =
			    opcode == ValueOpcode::BitFieldSExtract && c < 32u && (field >> (c - 1u)) != 0u;
			result = sign ? field | ~((1u << c) - 1u) : field;
			return true;
		}
		case ValueOpcode::UMin32: result = std::min(a, b); return true;
		case ValueOpcode::UMax32: result = std::max(a, b); return true;
		case ValueOpcode::SMin32: result = static_cast<uint32_t>(std::min(sa, sb)); return true;
		case ValueOpcode::SMax32: result = static_cast<uint32_t>(std::max(sa, sb)); return true;
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectU1: result = a != 0u ? b : c; return true;
		case ValueOpcode::IEqual32: result = a == b; return true;
		case ValueOpcode::INotEqual32: result = a != b; return true;
		case ValueOpcode::ULessThan32: result = a < b; return true;
		case ValueOpcode::ULessThanEqual32: result = a <= b; return true;
		case ValueOpcode::UGreaterThan32: result = a > b; return true;
		case ValueOpcode::UGreaterThanEqual32: result = a >= b; return true;
		case ValueOpcode::SLessThan32: result = sa < sb; return true;
		case ValueOpcode::SLessThanEqual32: result = sa <= sb; return true;
		case ValueOpcode::SGreaterThan32: result = sa > sb; return true;
		case ValueOpcode::SGreaterThanEqual32: result = sa >= sb; return true;
		case ValueOpcode::LogicalAnd: result = (a != 0u) && (b != 0u); return true;
		case ValueOpcode::LogicalOr: result = (a != 0u) || (b != 0u); return true;
		case ValueOpcode::LogicalXor: result = (a != 0u) != (b != 0u); return true;
		case ValueOpcode::LogicalNot: result = a == 0u; return true;
		default: return false;
	}
}

void BuildParameterKeys(const Program& program, ResourcePlan& plan) {
	if (program.stage != ShaderType::Vertex) return;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::SetAttribute) continue;
			const auto index = inst.Flags<ExportFlags>().index;
			if (index >= program.export_info.size()) continue;
			const auto& exp = program.export_info[index];
			if (exp.kind != ExportTargetKind::Parameter || exp.index >= 32u) continue;
			const auto* data = inst.Arg(0).Resolve().TryInstruction();
			for (uint32_t channel = 0; channel < 4u; ++channel) {
				if ((exp.en & (1u << channel)) == 0u && !exp.compr) continue;
				Value value;
				if (!exp.compr && data != nullptr &&
				    data->GetOpcode() == ValueOpcode::CompositeConstructU32x4) {
					KeyCloner cloner(program, plan, inst.Arg(1));
					value = cloner.Clone(data->Arg(channel));
					if (!cloner.Ok() || value.IsEmpty() || value.GetType() != Type::U32 ||
					    !VertexIndexOnlySelectsRecords(value))
						value = {};
				}
				plan.parameter_keys.push_back({exp.index * 4u + channel, value});
			}
		}
	}
}

bool ParameterKeySignature(const ResourcePlan& plan, const SrtRuntime& runtime, uint32_t parameter,
                           std::vector<uint32_t>& signature) {
	signature.clear();
	bool exported = false;
	for (const auto& entry: plan.parameter_keys) {
		if (entry.parameter != parameter) continue;
		exported = true;
		std::vector<std::pair<uint64_t, uint64_t>> reads;
		KeyEvaluator                               evaluator(plan, runtime, reads);
		if (entry.value.IsEmpty() || !evaluator.Signature(entry.value, signature)) return false;
	}
	return exported;
}

bool EvaluateParameterKeys(const ResourcePlan& plan, const SrtRuntime& runtime, uint32_t parameter,
                           std::vector<uint32_t>&                      keys,
                           std::vector<std::pair<uint64_t, uint64_t>>& reads) {
	keys.clear();
	bool exported = false;
	for (const auto& entry: plan.parameter_keys) {
		if (entry.parameter != parameter) continue;
		exported = true;
		if (entry.value.IsEmpty()) return false;
		KeyEvaluator evaluator(plan, runtime, reads);
		if (!evaluator.Run(entry.value, keys)) return false;
	}
	std::ranges::sort(keys);
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	return exported && keys.size() <= MaxKeys;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
