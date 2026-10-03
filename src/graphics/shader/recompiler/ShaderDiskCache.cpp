#include "graphics/shader/recompiler/ShaderDiskCache.h"

#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::DiskCache {

namespace {

constexpr uint32_t kPlanMagic        = 0x4e4c5052; // "RPLN"
constexpr uint32_t kPermutationMagic = 0x4d525045; // "EPRM"

// One code path for both directions keeps the writer and the reader from drifting apart: every
// Serialize overload below reads or writes its fields depending on how the archive was opened.
class Archive {
public:
	explicit Archive(std::vector<uint8_t>& out): m_out(&out) {}
	explicit Archive(std::span<const uint8_t> in): m_in(in) {}

	[[nodiscard]] bool Writing() const { return m_out != nullptr; }
	[[nodiscard]] bool Ok() const { return m_ok; }
	void               Fail() { m_ok = false; }

	void Bytes(void* data, size_t size) {
		if (!m_ok) return;
		if (Writing()) {
			const auto* bytes = static_cast<const uint8_t*>(data);
			m_out->insert(m_out->end(), bytes, bytes + size);
		} else if (size > m_in.size() - m_pos) {
			m_ok = false;
		} else {
			if (size != 0) std::memcpy(data, m_in.data() + m_pos, size);
			m_pos += size;
		}
	}

	template <typename T>
	requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
	void Scalar(T& value) {
		Bytes(&value, sizeof(T));
	}

	// Element counts are checked against what is left in the input before anything is allocated.
	uint32_t Count(size_t size, size_t min_element_bytes) {
		auto count = static_cast<uint32_t>(size);
		Scalar(count);
		if (!Writing() && m_ok &&
		    (count > m_in.size() - m_pos ||
		     static_cast<uint64_t>(count) * min_element_bytes > m_in.size() - m_pos)) {
			m_ok = false;
		}
		return m_ok ? count : 0;
	}

	[[nodiscard]] bool AtEnd() const { return Writing() || m_pos == m_in.size(); }

private:
	std::vector<uint8_t>*      m_out = nullptr;
	std::span<const uint8_t>   m_in;
	size_t                     m_pos = 0;
	bool                       m_ok  = true;
};

template <typename T>
requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
void Serialize(Archive& ar, T& value) {
	ar.Scalar(value);
}

template <typename T, size_t N>
void Serialize(Archive& ar, std::array<T, N>& values) {
	for (auto& value: values) Serialize(ar, value);
}

template <typename T>
void Serialize(Archive& ar, std::vector<T>& values) {
	const auto count = ar.Count(values.size(), 1);
	if (!ar.Writing()) values.resize(count);
	for (auto& value: values) Serialize(ar, value);
}

template <typename A, typename B>
void Serialize(Archive& ar, std::pair<A, B>& value) {
	Serialize(ar, value.first);
	Serialize(ar, value.second);
}

void Serialize(Archive& ar, std::string& value) {
	const auto count = ar.Count(value.size(), 1);
	if (!ar.Writing()) value.resize(count);
	ar.Bytes(value.data(), count);
}

// Values refer to instructions by position in ResourcePlan::value_storage.
struct ValueTable {
	std::unordered_map<const IR::Inst*, uint32_t> index;
	std::vector<IR::Inst*>                        insts;
};

enum class ValueTag : uint8_t {
	Empty,
	Inst,
	ScalarReg,
	VectorReg,
	U1,
	U8,
	U16,
	U32,
	U64,
	F16,
	F32,
};

void Serialize(Archive& ar, IR::Value& value, ValueTable& table) {
	ValueTag tag  = ValueTag::Empty;
	uint64_t bits = 0;
	if (ar.Writing()) {
		if (const auto* inst = value.TryInstruction(); inst != nullptr) {
			const auto found = table.index.find(inst);
			if (found == table.index.end()) {
				ar.Fail();
				return;
			}
			tag  = ValueTag::Inst;
			bits = found->second;
		} else {
			switch (value.GetType()) {
				case IR::Type::Void: break;
				case IR::Type::ScalarReg:
					tag  = ValueTag::ScalarReg;
					bits = static_cast<uint16_t>(value.ScalarRegister());
					break;
				case IR::Type::VectorReg:
					tag  = ValueTag::VectorReg;
					bits = static_cast<uint16_t>(value.VectorRegister());
					break;
				case IR::Type::U1: tag = ValueTag::U1, bits = value.U1(); break;
				case IR::Type::U8: tag = ValueTag::U8, bits = value.U8(); break;
				case IR::Type::U16: tag = ValueTag::U16, bits = value.U16(); break;
				case IR::Type::U32: tag = ValueTag::U32, bits = value.U32(); break;
				case IR::Type::U64: tag = ValueTag::U64, bits = value.U64(); break;
				case IR::Type::F16: tag = ValueTag::F16, bits = value.F16Bits(); break;
				case IR::Type::F32: tag = ValueTag::F32, bits = std::bit_cast<uint32_t>(value.F32Value()); break;
				default: ar.Fail(); return;
			}
		}
	}
	ar.Scalar(tag);
	ar.Scalar(bits);
	if (ar.Writing() || !ar.Ok()) return;
	switch (tag) {
		case ValueTag::Empty: value = IR::Value(); break;
		case ValueTag::Inst:
			if (bits >= table.insts.size()) {
				ar.Fail();
				return;
			}
			value = IR::Value(table.insts[bits]);
			break;
		case ValueTag::ScalarReg: value = IR::Value(static_cast<IR::ScalarReg>(bits)); break;
		case ValueTag::VectorReg: value = IR::Value(static_cast<IR::VectorReg>(bits)); break;
		case ValueTag::U1: value = IR::Value(bits != 0); break;
		case ValueTag::U8: value = IR::Value(static_cast<uint8_t>(bits)); break;
		case ValueTag::U16: value = IR::Value(static_cast<uint16_t>(bits)); break;
		case ValueTag::U32: value = IR::Value(static_cast<uint32_t>(bits)); break;
		case ValueTag::U64: value = IR::Value(bits); break;
		case ValueTag::F16: value = IR::Value::F16(static_cast<uint16_t>(bits)); break;
		case ValueTag::F32:
			value = IR::Value::F32(std::bit_cast<float>(static_cast<uint32_t>(bits)));
			break;
		default: ar.Fail(); break;
	}
}

template <size_t N>
void Serialize(Archive& ar, std::array<IR::Value, N>& values, ValueTable& table) {
	for (auto& value: values) Serialize(ar, value, table);
}

void Serialize(Archive& ar, IR::MemoryInfo& v) {
	Serialize(ar, v.kind);
	Serialize(ar, v.resource);
	Serialize(ar, v.sampler);
	Serialize(ar, v.offset);
	Serialize(ar, v.secondary_offset);
	Serialize(ar, v.dmask);
	Serialize(ar, v.data_dwords);
	Serialize(ar, v.data_bits);
	Serialize(ar, v.component_index);
	Serialize(ar, v.component_count);
	Serialize(ar, v.data_format);
	Serialize(ar, v.number_format);
	Serialize(ar, v.image_sample_flags);
	Serialize(ar, v.image_dimension);
	Serialize(ar, v.image_address_components);
	Serialize(ar, v.address_is_full);
	Serialize(ar, v.data_signed);
	Serialize(ar, v.typed);
	Serialize(ar, v.formatted);
	Serialize(ar, v.image_has_mip);
	Serialize(ar, v.image_r128);
	Serialize(ar, v.idxen);
	Serialize(ar, v.offen);
	Serialize(ar, v.coherent);
	Serialize(ar, v.planning_only);
}

void Serialize(Archive& ar, IR::BufferResource& v) {
	Serialize(ar, v.source);
	Serialize(ar, v.first_use_pc);
	Serialize(ar, v.max_byte_extent);
	Serialize(ar, v.packed_stride);
	Serialize(ar, v.descriptor_format);
	Serialize(ar, v.descriptor_swizzle);
	Serialize(ar, v.image_alias);
	Serialize(ar, v.read);
	Serialize(ar, v.written);
	Serialize(ar, v.atomic);
	Serialize(ar, v.formatted);
	Serialize(ar, v.scalar);
}

void Serialize(Archive& ar, IR::ImageResource& v) {
	Serialize(ar, v.source);
	Serialize(ar, v.first_use_pc);
	Serialize(ar, v.resource_class);
	Serialize(ar, v.numeric_class);
	Serialize(ar, v.dimension);
	Serialize(ar, v.mip_mode);
	Serialize(ar, v.mip_count);
	Serialize(ar, v.conversion_format);
	Serialize(ar, v.shader_swizzle);
	Serialize(ar, v.read);
	Serialize(ar, v.written);
	Serialize(ar, v.atomic);
	Serialize(ar, v.depth_compare);
	Serialize(ar, v.cube);
	Serialize(ar, v.r128);
	Serialize(ar, v.indirect_root);
	Serialize(ar, v.indirect_mapping_offset);
	Serialize(ar, v.indirect_search_iterations);
	Serialize(ar, v.indirect_resources);
}

void Serialize(Archive& ar, IR::SamplerResource& v) {
	Serialize(ar, v.source);
	Serialize(ar, v.first_use_pc);
	Serialize(ar, v.force_point_filtering);
	Serialize(ar, v.depth_compare);
	Serialize(ar, v.integer_border);
}

void Serialize(Archive& ar, IR::SampledResourcePair& v) {
	Serialize(ar, v.image);
	Serialize(ar, v.sampler);
	Serialize(ar, v.first_use_pc);
}

void Serialize(Archive& ar, IR::StageInput& v) {
	Serialize(ar, v.kind);
	Serialize(ar, v.location);
	Serialize(ar, v.component_count);
	Serialize(ar, v.debug_name);
	Serialize(ar, v.per_vertex);
}

void Serialize(Archive& ar, IR::StageOutput& v) {
	Serialize(ar, v.kind);
	Serialize(ar, v.index);
	Serialize(ar, v.location);
	Serialize(ar, v.debug_name);
}

void Serialize(Archive& ar, IR::ShaderInfo& v) {
	Serialize(ar, v.buffers);
	Serialize(ar, v.images);
	Serialize(ar, v.samplers);
	Serialize(ar, v.sampled_pairs);
	Serialize(ar, v.inputs);
	Serialize(ar, v.outputs);
	Serialize(ar, v.vertex_fetch_components);
	Serialize(ar, v.vertex_offset_sgpr);
	Serialize(ar, v.instance_offset_sgpr);
	Serialize(ar, v.has_bitwise_xor);
	Serialize(ar, v.uses_dma);
}

void Serialize(Archive& ar, IR::DescriptorBinding& v) {
	Serialize(ar, v.kind);
	Serialize(ar, v.resources);
}

void Serialize(Archive& ar, IR::BindingLayout& v) {
	Serialize(ar, v.push_data_start_dword);
	Serialize(ar, v.memory_offset_dword);
	Serialize(ar, v.memory_offset_count);
	Serialize(ar, v.user_data_registers);
	Serialize(ar, v.descriptors);
}

void Serialize(Archive& ar, IR::UniformFill& v) {
	Serialize(ar, v.kind);
	Serialize(ar, v.resource);
	Serialize(ar, v.group_stride);
	Serialize(ar, v.words);
	Serialize(ar, v.value);
}

void Serialize(Archive& ar, IR::CompiledShaderInfo& v) {
	Serialize(ar, v.stage);
	Serialize(ar, v.shader_hash);
	Serialize(ar, v.wave_size);
	Serialize(ar, v.user_data_base);
	Serialize(ar, v.user_data_count);
	Serialize(ar, v.scratch_dwords);
	Serialize(ar, v.param_export_mask);
	Serialize(ar, v.has_address_writes);
	Serialize(ar, v.info);
	Serialize(ar, v.bindings);
}

void Serialize(Archive& ar, IR::DescriptorSource& v, ValueTable& table) {
	Serialize(ar, v.dwords, table);
	Serialize(ar, v.dword_count);
	bool has_indirect = v.indirect_image.has_value();
	Serialize(ar, has_indirect);
	if (!has_indirect) {
		v.indirect_image.reset();
		return;
	}
	if (!ar.Writing()) v.indirect_image.emplace();
	auto& image = *v.indirect_image;
	Serialize(ar, image.material_source);
	Serialize(ar, image.table_source);
	Serialize(ar, image.selector_stride);
	Serialize(ar, image.selector_offset);
	Serialize(ar, image.table_offset);
	Serialize(ar, image.key_count, table);
	Serialize(ar, image.selector_mask, table);
	Serialize(ar, image.sources);
}

void Serialize(Archive& ar, IR::ResourceBlock& v, ValueTable& table) {
	Serialize(ar, v.condition, table);
	Serialize(ar, v.successors);
	Serialize(ar, v.sources);
	Serialize(ar, v.srt_reads);
}

void Serialize(Archive& ar, IR::ResourcePlan& plan, ValueTable& table) {
	Serialize(ar, plan.stage);
	Serialize(ar, plan.shader_hash);
	Serialize(ar, plan.user_data_base);
	Serialize(ar, plan.user_data_count);

	// Instructions are created first so that operands may refer to any of them, including loops
	// through phis.
	const auto inst_count = ar.Count(plan.value_storage.size(), 4);
	if (ar.Writing()) {
		for (auto& inst: plan.value_storage) {
			table.index.emplace(&inst, static_cast<uint32_t>(table.insts.size()));
			table.insts.push_back(&inst);
		}
		for (auto* inst: table.insts) {
			auto opcode = inst->GetOpcode();
			auto flags  = inst->Flags<uint64_t>();
			auto args   = static_cast<uint32_t>(inst->NumArgs());
			Serialize(ar, opcode);
			Serialize(ar, flags);
			Serialize(ar, args);
		}
		for (auto* inst: table.insts) {
			for (size_t i = 0; i < inst->NumArgs(); ++i) {
				auto arg = inst->Arg(i);
				Serialize(ar, arg, table);
			}
		}
	} else {
		std::vector<uint32_t> arg_counts;
		for (uint32_t i = 0; i < inst_count && ar.Ok(); ++i) {
			IR::ValueOpcode opcode {};
			uint64_t        flags = 0;
			uint32_t        args  = 0;
			Serialize(ar, opcode);
			Serialize(ar, flags);
			Serialize(ar, args);
			if (!ar.Ok()) break;
			const auto fixed_args = IR::NumArgsOf(opcode);
			if (static_cast<uint32_t>(opcode) >= static_cast<uint32_t>(IR::ValueOpcode::Count) ||
			    (opcode != IR::ValueOpcode::Phi && fixed_args != args)) {
				ar.Fail();
				break;
			}
			table.insts.push_back(&plan.value_storage.emplace_back(opcode, flags));
			arg_counts.push_back(args);
		}
		for (uint32_t i = 0; i < table.insts.size() && ar.Ok(); ++i) {
			auto* inst = table.insts[i];
			for (uint32_t arg_index = 0; arg_index < arg_counts[i] && ar.Ok(); ++arg_index) {
				IR::Value arg;
				Serialize(ar, arg, table);
				if (!ar.Ok()) break;
				if (inst->GetOpcode() == IR::ValueOpcode::Phi) {
					inst->AddPhiOperand(nullptr, arg);
				} else {
					inst->SetArg(arg_index, arg);
				}
			}
		}
	}

	Serialize(ar, plan.memory_info);
	const auto source_count = ar.Count(plan.descriptor_sources.size(), 1);
	if (!ar.Writing()) plan.descriptor_sources.resize(source_count);
	for (auto& source: plan.descriptor_sources) Serialize(ar, source, table);

	const auto block_count = ar.Count(plan.control_flow.size(), 1);
	if (!ar.Writing()) plan.control_flow.resize(block_count);
	for (auto& block: plan.control_flow) Serialize(ar, block, table);

	const auto read_count = ar.Count(plan.srt_reads.size(), 1);
	if (!ar.Writing()) plan.srt_reads.resize(read_count);
	for (auto& read: plan.srt_reads) {
		Serialize(ar, read.value, table);
		Serialize(ar, read.flat_offset);
	}
	Serialize(ar, plan.clean_flat_slots);
	Serialize(ar, plan.requires_specialization_memory);
	Serialize(ar, plan.capture_specialization_reads);
	Serialize(ar, plan.srt_plan_complete);
	Serialize(ar, plan.resource_tracking_complete);
	Serialize(ar, plan.info);
	Serialize(ar, plan.uniform_fill.fill);
	Serialize(ar, plan.uniform_fill.values, table);
}

} // namespace

std::vector<uint8_t> SerializePlan(const IR::ResourcePlan& plan, bool ray_traced) {
	std::vector<uint8_t> out;
	Archive              ar(out);
	auto                 magic   = kPlanMagic;
	auto                 version = kFormatVersion;
	ar.Scalar(magic);
	ar.Scalar(version);
	ar.Scalar(ray_traced);
	ValueTable table;
	// The archive only reads from the plan while writing.
	Serialize(ar, const_cast<IR::ResourcePlan&>(plan), table);
	if (!ar.Ok()) return {};
	return out;
}

bool DeserializePlan(std::span<const uint8_t> data, IR::ResourcePlan& plan, bool& ray_traced) {
	Archive  ar(data);
	uint32_t magic   = 0;
	uint32_t version = 0;
	ar.Scalar(magic);
	ar.Scalar(version);
	ar.Scalar(ray_traced);
	if (!ar.Ok() || magic != kPlanMagic || version != kFormatVersion) return false;
	IR::ResourcePlan result;
	ValueTable       table;
	Serialize(ar, result, table);
	if (!ar.Ok() || !ar.AtEnd()) return false;
	plan = std::move(result);
	return true;
}

std::vector<uint8_t> SerializePermutation(const IR::CompiledShaderInfo& info,
                                          std::span<const uint32_t>     spirv) {
	std::vector<uint8_t> out;
	Archive              ar(out);
	auto                 magic   = kPermutationMagic;
	auto                 version = kFormatVersion;
	ar.Scalar(magic);
	ar.Scalar(version);
	Serialize(ar, const_cast<IR::CompiledShaderInfo&>(info));
	auto words = static_cast<uint32_t>(spirv.size());
	ar.Scalar(words);
	ar.Bytes(const_cast<uint32_t*>(spirv.data()), spirv.size_bytes());
	if (!ar.Ok()) return {};
	return out;
}

bool DeserializePermutation(std::span<const uint8_t> data, IR::CompiledShaderInfo& info,
                            std::vector<uint32_t>& spirv) {
	Archive  ar(data);
	uint32_t magic   = 0;
	uint32_t version = 0;
	ar.Scalar(magic);
	ar.Scalar(version);
	if (!ar.Ok() || magic != kPermutationMagic || version != kFormatVersion) return false;
	IR::CompiledShaderInfo result;
	Serialize(ar, result);
	const auto words = ar.Count(0, sizeof(uint32_t));
	std::vector<uint32_t> code(words);
	ar.Bytes(code.data(), code.size() * sizeof(uint32_t));
	if (!ar.Ok() || !ar.AtEnd()) return false;
	info  = std::move(result);
	spirv = std::move(code);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::DiskCache
