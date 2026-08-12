#include "graphics/shader/recompiler/ir/ResourceTracking.h"

#include "graphics/shader/recompiler/ir/ScalarProvenance.h"

#include <algorithm>
#include <fmt/format.h>
#include <functional>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

bool IsAtomic(Opcode op) {
	switch (op) {
		case Opcode::AtomicSwapU32:
		case Opcode::AtomicCompareSwapU32:
		case Opcode::AtomicAddU32:
		case Opcode::AtomicSubU32:
		case Opcode::AtomicSMinI32:
		case Opcode::AtomicUMinU32:
		case Opcode::AtomicSMaxI32:
		case Opcode::AtomicUMaxU32:
		case Opcode::AtomicAndU32:
		case Opcode::AtomicOrU32:
		case Opcode::AtomicXorU32:
		case Opcode::AtomicFMinF32:
		case Opcode::AtomicFMaxF32: return true;
		default: return false;
	}
}

bool IsWrite(Opcode op) {
	switch (op) {
		case Opcode::BufferStoreByte:
		case Opcode::BufferStoreShort:
		case Opcode::BufferStoreDword:
		case Opcode::ImageStore: return true;
		default: return IsAtomic(op);
	}
}

bool NeedsSampler(Opcode op) {
	return op == Opcode::ImageSample || op == Opcode::ImageGather4 || op == Opcode::ImageGetLod;
}

bool IsDepthCompare(const Instruction& inst) {
	return (inst.memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0;
}

ImageMipMode MipMode(const Instruction& inst) {
	const bool storage = inst.memory.kind == ResourceKind::StorageImage ||
	                     inst.memory.kind == ResourceKind::StorageImageUint;
	return storage && inst.memory.image_has_mip ? ImageMipMode::DynamicStorage : ImageMipMode::None;
}

bool IsBuffer(const Instruction& inst) {
	return inst.memory.kind == ResourceKind::Buffer ||
	       (inst.memory.kind == ResourceKind::ScalarBuffer && inst.op == Opcode::SBufferLoadDword);
}

bool IsImage(const Instruction& inst) {
	return inst.memory.kind == ResourceKind::Image || inst.memory.kind == ResourceKind::ImageUint ||
	       inst.memory.kind == ResourceKind::StorageImage ||
	       inst.memory.kind == ResourceKind::StorageImageUint;
}

bool IsAddress(const Instruction& inst) {
	return inst.op == Opcode::SLoadDword || inst.memory.kind == ResourceKind::Flat ||
	       inst.memory.kind == ResourceKind::Global || inst.memory.kind == ResourceKind::Scratch;
}

uint32_t ByteExtent(const Instruction& inst) {
	const auto bytes = std::max((inst.memory.data_bits + 7u) / 8u, 1u);
	const auto count = std::max(inst.memory.data_dwords, 1u);
	const auto end =
	    static_cast<uint64_t>(inst.memory.offset) + static_cast<uint64_t>(bytes) * count;
	return end > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end);
}

bool ContainsUnknown(const ScalarProvenance& provenance, uint32_t id, std::vector<uint8_t>& visited,
                     std::vector<uint32_t>& path) {
	path.push_back(id);
	if (id <= ScalarProvenance::Unknown || id >= provenance.values.size()) {
		return true;
	}
	if (visited[id] != 0) {
		path.pop_back();
		return false;
	}
	visited[id]       = 1;
	const auto& value = provenance.values[id];
	if (value.op == ScalarValueOp::Phi) {
		for (const auto arg: value.phi_args) {
			if (ContainsUnknown(provenance, arg, visited, path)) {
				return true;
			}
		}
		path.pop_back();
		return false;
	}
	const auto args = ScalarValueArgCount(value.op);
	for (uint32_t i = 0; i < args; i++) {
		if (ContainsUnknown(provenance, value.args[i], visited, path)) {
			return true;
		}
	}
	path.pop_back();
	return false;
}

bool IsLoopInvariantValue(const ScalarProvenance& provenance, uint32_t id,
                          std::vector<uint8_t>& visiting) {
	if (id <= ScalarProvenance::Unknown || id >= provenance.values.size()) {
		return false;
	}
	if (visiting[id] != 0) {
		return true;
	}
	visiting[id]      = 1;
	const auto& value = provenance.values[id];
	if (value.op == ScalarValueOp::Phi) {
		uint32_t invariant = ScalarProvenance::Undefined;
		for (const auto arg: value.phi_args) {
			if (arg == id) {
				continue;
			}
			if (invariant == ScalarProvenance::Undefined) {
				invariant = arg;
			} else if (arg != invariant) {
				return false;
			}
		}
		return invariant != ScalarProvenance::Undefined &&
		       IsLoopInvariantValue(provenance, invariant, visiting);
	}
	for (uint32_t i = 0; i < ScalarValueArgCount(value.op); i++) {
		if (!IsLoopInvariantValue(provenance, value.args[i], visiting)) {
			return false;
		}
	}
	return true;
}

bool IsLoopInvariantDescriptor(const ScalarProvenance& provenance,
                               const DescriptorValue&  descriptor) {
	std::vector<uint8_t> visiting(provenance.values.size());
	for (uint32_t i = 0; i < descriptor.dword_count; i++) {
		if (!IsLoopInvariantValue(provenance, descriptor.dwords[i], visiting)) {
			return false;
		}
	}
	return true;
}

class Tracker {
public:
	explicit Tracker(Program& program): m_program(program) {}

	bool Run(std::string* error) {
		if (m_program.resource_tracking_complete) {
			return Fail(0, error, "resources already tracked");
		}
		if (!m_program.srt_plan_complete) {
			return Fail(0, error, "SRT plan is not ready");
		}
		if (!m_program.srt_patching_complete) {
			return Fail(0, error, "SRT reads were not patched");
		}
		for (auto& block: m_program.blocks) {
			for (auto& inst: block.instructions) {
				if (!Collect(inst, error)) {
					return false;
				}
			}
		}
		if (!LinkDynamicImageTables(error)) {
			return false;
		}
		LinkImageAliases();
		m_program.info = std::move(m_info);
		for (const auto& patch: m_patches) {
			auto& inst                  = patch.inst.get();
			inst.memory.resource        = patch.resource;
			inst.memory.sampler         = patch.sampler;
			inst.memory.dynamic_resource_offset = patch.dynamic_resource_offset;
			inst.memory.dynamic_resource_base_offset = patch.dynamic_resource_base_offset;
			inst.memory.resource_source = ScalarProvenance::Undefined;
			inst.memory.sampler_source  = ScalarProvenance::Undefined;
		}
		m_program.resource_tracking_complete = true;
		return true;
	}

private:
	struct DynamicImagePattern {
		uint32_t table_source = ScalarProvenance::Undefined;
		Operand  offset;
		uint32_t base_offset = 0;
		uint32_t table_address_offset = 0;
		uint32_t table_address_count = 0;
	};

	struct Patch {
		std::reference_wrapper<Instruction> inst;
		uint32_t                            resource = 0;
		uint32_t                            sampler  = 0;
		Operand                             dynamic_resource_offset;
		uint32_t                            dynamic_resource_base_offset = 0;
	};

	bool Fail(uint32_t pc, std::string* error, const std::string& reason) const {
		if (error != nullptr) {
			*error = fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
			                     m_program.shader_hash, StageName(m_program.stage), pc, reason);
		}
		return false;
	}

	uint32_t FindDescriptorSource(const DescriptorValue& descriptor) const {
		for (uint32_t i = 0; i < m_program.provenance.descriptors.size(); i++) {
			if (m_program.provenance.descriptors[i] == descriptor) {
				return i + 2u;
			}
		}
		return ScalarProvenance::Undefined;
	}

	const Instruction* FindScalarLoadProducer(uint32_t value) const {
		for (const auto& block: m_program.blocks) {
			for (const auto& inst: block.instructions) {
				if ((inst.op == Opcode::SBufferLoadDword || inst.op == Opcode::SLoadDword) &&
				    inst.scalar_value == value &&
				    inst.src_count == 1) {
					return &inst;
				}
			}
		}
		return nullptr;
	}

	bool ConstantValue(uint32_t id, uint32_t& result) const {
		if (id >= m_program.provenance.values.size()) {
			return false;
		}
		const auto& value = m_program.provenance.values[id];
		if (value.op != ScalarValueOp::Constant) {
			return false;
		}
		result = value.imm;
		return true;
	}

	bool DecodeDirectTableOffset(uint32_t id, uint32_t& index, uint32_t& count,
	                             uint32_t& constant) const {
		if (id >= m_program.provenance.values.size()) {
			return false;
		}
		const auto& value = m_program.provenance.values[id];
		if (value.op == ScalarValueOp::Add) {
			uint32_t folded = 0;
			if (ConstantValue(value.args[0], folded) &&
			    DecodeDirectTableOffset(value.args[1], index, count, constant)) {
				constant += folded;
				return true;
			}
			if (ConstantValue(value.args[1], folded) &&
			    DecodeDirectTableOffset(value.args[0], index, count, constant)) {
				constant += folded;
				return true;
			}
			return false;
		}
		if (value.op != ScalarValueOp::ShiftLeft) {
			return false;
		}
		uint32_t shift = 0;
		if (!ConstantValue(value.args[1], shift) || shift != 5u ||
		    value.args[0] >= m_program.provenance.values.size() ||
		    m_program.provenance.values[value.args[0]].op != ScalarValueOp::FindLsbU32) {
			return false;
		}
		// The official Prospero ISA operation s_ff1_i32_b32 selects a bit from a
		// 32-bit source. A non-zero source therefore yields a descriptor index in [0, 31].
		index    = value.args[0];
		count    = 32;
		constant = 0;
		return true;
	}

	bool DetectDirectImageTable(const DescriptorValue& descriptor,
	                            DynamicImagePattern& result) const {
		if (descriptor.dword_count != 8) {
			return false;
		}
		DescriptorValue table;
		table.dword_count = 2;
		uint32_t common_index = ScalarProvenance::Undefined;
		uint32_t descriptor_count = 0;
		uint32_t table_offset = 0;
		Operand  offset_operand;
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			const auto id = descriptor.dwords[i];
			if (id >= m_program.provenance.values.size()) {
				return false;
			}
			const auto& value = m_program.provenance.values[id];
			if (value.op != ScalarValueOp::ReadConst) {
				return false;
			}
			uint32_t index = 0;
			uint32_t count = 0;
			uint32_t offset = 0;
			if (!DecodeDirectTableOffset(value.args[2], index, count, offset)) {
				return false;
			}
			offset += value.imm;
			const auto* producer = FindScalarLoadProducer(id);
			if (producer == nullptr || producer->src[0].kind != OperandKind::Register) {
				return false;
			}
			if (i == 0) {
				table.dwords[0] = value.args[0];
				table.dwords[1] = value.args[1];
				common_index     = index;
				descriptor_count = count;
				table_offset     = offset;
				offset_operand   = producer->src[0];
			} else if (value.args[0] != table.dwords[0] ||
			           value.args[1] != table.dwords[1] || index != common_index ||
			           count != descriptor_count || offset != table_offset + i * sizeof(uint32_t)) {
				return false;
			}
		}
		const auto table_source = FindDescriptorSource(table);
		if (table_source == ScalarProvenance::Undefined ||
		    !DescriptorSourceResolved(m_program, table_source)) {
			return false;
		}
		result.table_source         = table_source;
		result.offset               = offset_operand;
		result.base_offset          = 0u - table_offset;
		result.table_address_offset = table_offset;
		result.table_address_count  = descriptor_count;
		return true;
	}

	bool DetectDynamicImageTable(const DescriptorValue& descriptor,
	                             DynamicImagePattern& result) const {
		if (DetectDirectImageTable(descriptor, result)) {
			return true;
		}
		if (descriptor.dword_count != 8) {
			return false;
		}
		DescriptorValue table;
		table.dword_count = 4;
		uint32_t common_offset = ScalarProvenance::Undefined;
		Operand  offset_operand;
		uint32_t base_offset = 0;
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			const auto id = descriptor.dwords[i];
			if (id >= m_program.provenance.values.size()) {
				return false;
			}
			const auto& value = m_program.provenance.values[id];
			if (value.op != ScalarValueOp::ReadConstBuffer) {
				return false;
			}
			if (i == 0) {
				std::copy_n(value.args.begin(), 4, table.dwords.begin());
				common_offset = value.args[4];
				base_offset   = value.imm;
				const auto* producer = FindScalarLoadProducer(id);
				if (producer == nullptr) {
					return false;
				}
				offset_operand = producer->src[0];
			} else {
				if (!std::equal(value.args.begin(), value.args.begin() + 4,
				                table.dwords.begin()) || value.args[4] != common_offset ||
				    value.imm != base_offset + i * sizeof(uint32_t)) {
					return false;
				}
				const auto* producer = FindScalarLoadProducer(id);
				if (producer == nullptr || producer->src[0] != offset_operand) {
					return false;
				}
			}
		}
		if ((base_offset & 31u) != 0 || offset_operand.kind != OperandKind::Register) {
			return false;
		}
		std::vector<uint8_t> visited(m_program.provenance.values.size());
		std::vector<uint32_t> path;
		if (!ContainsUnknown(m_program.provenance, common_offset, visited, path)) {
			return false;
		}
		const auto& offset_value = m_program.provenance.values[common_offset];
		const auto shift_id = offset_value.args[1];
		if (offset_value.op != ScalarValueOp::ShiftLeft ||
		    shift_id >= m_program.provenance.values.size() ||
		    m_program.provenance.values[shift_id].op != ScalarValueOp::Constant ||
		    m_program.provenance.values[shift_id].imm != 5u) {
			return false;
		}
		const auto table_source = FindDescriptorSource(table);
		if (table_source == ScalarProvenance::Undefined ||
		    !DescriptorSourceResolved(m_program, table_source)) {
			return false;
		}
		result = {table_source, offset_operand, base_offset};
		return true;
	}

	bool ValidateSource(uint32_t source, uint32_t dwords, uint32_t pc, std::string* error,
	                    DynamicImagePattern* dynamic_image = nullptr) const {
		const auto* descriptor = GetDescriptorSource(m_program, source);
		if (descriptor == nullptr || descriptor->dword_count != dwords) {
			return Fail(pc, error,
			            fmt::format("descriptor source {} is missing or has wrong width", source));
		}
		if (dynamic_image != nullptr && DetectDynamicImageTable(*descriptor, *dynamic_image)) {
			return true;
		}
		std::vector<uint8_t> visited(m_program.provenance.values.size());
		for (uint32_t i = 0; i < descriptor->dword_count; i++) {
			std::vector<uint32_t> path;
			if (ContainsUnknown(m_program.provenance, descriptor->dwords[i], visited, path)) {
				const auto  value = descriptor->dwords[i];
				std::string chain;
				for (const auto id: path) {
					const auto op = id < m_program.provenance.values.size()
					                    ? static_cast<uint32_t>(m_program.provenance.values[id].op)
					                    : UINT32_MAX;
					chain += fmt::format("{}{}:{}({})", chain.empty() ? "" : " -> ", id, op,
					                     ScalarValueToString(m_program.provenance, id));
				}
				return Fail(
				    pc, error,
				    fmt::format(
				        "descriptor source {} dword {} contains an unknown value {} ({}) path {}",
				        source, i, value, ScalarValueToString(m_program.provenance, value), chain));
			}
		}
		const auto dynamic =
		    std::find(m_program.srt.dynamic_sources.begin(), m_program.srt.dynamic_sources.end(),
		              source) != m_program.srt.dynamic_sources.end();
		if (dynamic && !IsLoopInvariantDescriptor(m_program.provenance, *descriptor)) {
			std::string detail;
			for (uint32_t i = 0; i < descriptor->dword_count; i++) {
				const auto id = descriptor->dwords[i];
				detail +=
				    fmt::format(" d{}={}({})", i, id,
				                id < m_program.provenance.values.size()
				                    ? static_cast<uint32_t>(m_program.provenance.values[id].op)
				                    : UINT32_MAX);
				if (id < m_program.provenance.values.size()) {
					for (const auto arg: m_program.provenance.values[id].phi_args) {
						detail += fmt::format(
						    "/{}({})", arg,
						    arg < m_program.provenance.values.size()
						        ? static_cast<uint32_t>(m_program.provenance.values[arg].op)
						        : UINT32_MAX);
					}
				}
			}
			return Fail(pc, error,
			            fmt::format("descriptor source {} requires unsupported GPU selection{}",
			                        source, detail));
		}
		if (!DescriptorSourceResolved(m_program, source) && !dynamic) {
			return Fail(pc, error, fmt::format("descriptor source {} is unresolved", source));
		}
		return true;
	}

	uint32_t AddBuffer(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.buffers.size(); i++) {
			if (m_info.buffers[i].source == inst.memory.resource_source) {
				Merge(&m_info.buffers[i], inst);
				return i;
			}
		}
		if (m_info.buffers.size() >= ShaderInfo::MaxBuffers) {
			return UINT32_MAX;
		}
		BufferResource resource;
		resource.source       = inst.memory.resource_source;
		resource.first_use_pc = inst.pc;
		Merge(&resource, inst);
		m_info.buffers.push_back(resource);
		return static_cast<uint32_t>(m_info.buffers.size() - 1);
	}

	void Merge(BufferResource* resource, const Instruction& inst) const {
		resource->first_use_pc    = std::min(resource->first_use_pc, inst.pc);
		resource->max_byte_extent = std::max(resource->max_byte_extent, ByteExtent(inst));
		resource->read            = resource->read || !IsWrite(inst.op) || IsAtomic(inst.op);
		resource->written         = resource->written || IsWrite(inst.op);
		resource->atomic          = resource->atomic || IsAtomic(inst.op);
		resource->formatted       = resource->formatted || inst.memory.formatted;
		resource->scalar = resource->scalar || inst.memory.kind == ResourceKind::ScalarBuffer;
	}

	void LinkImageAliases() {
		for (auto& buffer: m_info.buffers) {
			const auto* buffer_source = GetDescriptorSource(m_program, buffer.source);
			if (buffer_source == nullptr || buffer_source->dword_count != 4) {
				continue;
			}
			for (uint32_t i = 0; i < m_info.images.size(); i++) {
				const auto* image_source = GetDescriptorSource(m_program, m_info.images[i].source);
				if (image_source != nullptr && image_source->dword_count == 8 &&
				    std::equal(buffer_source->dwords.begin(), buffer_source->dwords.begin() + 4,
				               image_source->dwords.begin())) {
					buffer.image_alias = i;
					break;
				}
			}
		}
	}

	bool LinkDynamicImageTables(std::string* error) {
		for (auto& image: m_info.images) {
			if (image.dynamic_table_source == ScalarProvenance::Undefined) {
				continue;
			}
			if (image.dynamic_table_address_count != 0) {
				continue;
			}
			const auto found = std::find_if(m_info.buffers.begin(), m_info.buffers.end(),
			                                [&](const auto& buffer) {
				                                return buffer.source == image.dynamic_table_source;
			                                });
			if (found == m_info.buffers.end()) {
				return Fail(image.first_use_pc, error,
				            "dynamic image table has no tracked scalar-buffer resource");
			}
			image.dynamic_table_buffer =
			    static_cast<uint32_t>(found - m_info.buffers.begin());
		}
		return true;
	}

	uint32_t AddImage(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.images.size(); i++) {
			auto& image = m_info.images[i];
			if (image.source == inst.memory.resource_source && image.kind == inst.memory.kind &&
			    image.dimension == inst.memory.image_dimension && image.mip_mode == MipMode(inst) &&
			    image.depth_compare == IsDepthCompare(inst)) {
				Merge(&image, inst);
				return i;
			}
		}
		if (m_info.images.size() >= ShaderInfo::MaxImages) {
			return UINT32_MAX;
		}
		ImageResource image;
		image.source        = inst.memory.resource_source;
		image.first_use_pc  = inst.pc;
		image.kind          = inst.memory.kind;
		image.dimension     = inst.memory.image_dimension;
		image.mip_mode      = MipMode(inst);
		image.depth_compare = IsDepthCompare(inst);
		Merge(&image, inst);
		m_info.images.push_back(image);
		return static_cast<uint32_t>(m_info.images.size() - 1);
	}

	uint32_t AddAddress(const Instruction& inst) {
		auto immediate = static_cast<int32_t>(inst.memory.offset);
		if (inst.memory.kind == ResourceKind::ScalarBuffer) {
			immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
		}
		const auto min_offset =
		    inst.memory.resource_source == ScalarProvenance::Unknown ? 0 : std::min(immediate, 0);
		for (uint32_t i = 0; i < m_info.addresses.size(); i++) {
			auto& address = m_info.addresses[i];
			if (address.source == inst.memory.resource_source && address.kind == inst.memory.kind) {
				address.first_use_pc = std::min(address.first_use_pc, inst.pc);
				address.min_offset   = std::min(address.min_offset, min_offset);
				address.read         = address.read || !IsWrite(inst.op) || IsAtomic(inst.op);
				address.written      = address.written || IsWrite(inst.op);
				address.atomic       = address.atomic || IsAtomic(inst.op);
				return i;
			}
		}
		if (m_info.addresses.size() >= ShaderInfo::MaxAddresses) {
			return UINT32_MAX;
		}
		AddressResource address {inst.memory.resource_source, inst.pc, inst.memory.kind,
		                         min_offset};
		address.read    = !IsWrite(inst.op) || IsAtomic(inst.op);
		address.written = IsWrite(inst.op);
		address.atomic  = IsAtomic(inst.op);
		m_info.addresses.push_back(address);
		return static_cast<uint32_t>(m_info.addresses.size() - 1);
	}

	void Merge(ImageResource* resource, const Instruction& inst) const {
		resource->first_use_pc = std::min(resource->first_use_pc, inst.pc);
		resource->read         = resource->read || !IsWrite(inst.op) || IsAtomic(inst.op);
		resource->written      = resource->written || IsWrite(inst.op);
		resource->atomic       = resource->atomic || IsAtomic(inst.op);
	}

	uint32_t AddSampler(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.samplers.size(); i++) {
			if (m_info.samplers[i].source == inst.memory.sampler_source) {
				m_info.samplers[i].first_use_pc =
				    std::min(m_info.samplers[i].first_use_pc, inst.pc);
				return i;
			}
		}
		if (m_info.samplers.size() >= ShaderInfo::MaxSamplers) {
			return UINT32_MAX;
		}
		m_info.samplers.push_back({inst.memory.sampler_source, inst.pc});
		return static_cast<uint32_t>(m_info.samplers.size() - 1);
	}

	bool AddSampledPair(uint32_t image, uint32_t sampler, uint32_t pc, std::string* error) {
		for (auto& pair: m_info.sampled_pairs) {
			if (pair.image == image && pair.sampler == sampler) {
				pair.first_use_pc = std::min(pair.first_use_pc, pc);
				return true;
			}
		}
		if (m_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
			return Fail(pc, error, "sampled image/sampler pair limit exceeded");
		}
		m_info.sampled_pairs.push_back({image, sampler, pc});
		return true;
	}

	bool Collect(Instruction& inst, std::string* error) {
		if (IsAddress(inst)) {
			const bool unbased = inst.memory.resource_source == ScalarProvenance::Unknown;
			if ((!unbased && !ValidateSource(inst.memory.resource_source, 2, inst.pc, error)) ||
			    (unbased && inst.memory.kind != ResourceKind::Flat &&
			     inst.memory.kind != ResourceKind::Global &&
			     inst.memory.kind != ResourceKind::Scratch)) {
				return unbased ? Fail(inst.pc, error, "scalar memory base is unresolved") : false;
			}
			const auto resource = AddAddress(inst);
			if (resource == UINT32_MAX) {
				return Fail(inst.pc, error, "address resource limit exceeded");
			}
			m_patches.push_back({std::ref(inst), resource, 0});
			return true;
		}
		if (!IsBuffer(inst) && !IsImage(inst)) {
			return true;
		}
		DynamicImagePattern dynamic_image;
		if (!ValidateSource(inst.memory.resource_source, IsBuffer(inst) ? 4u : 8u, inst.pc,
		                    error, IsImage(inst) ? &dynamic_image : nullptr)) {
			return false;
		}
		const auto resource = IsBuffer(inst) ? AddBuffer(inst) : AddImage(inst);
		if (resource == UINT32_MAX) {
			return Fail(inst.pc, error,
			            IsBuffer(inst) ? "buffer resource limit exceeded"
			                           : "image resource limit exceeded");
		}
		if (dynamic_image.table_source != ScalarProvenance::Undefined) {
			auto& image = m_info.images[resource];
			if (image.written || image.atomic || image.mip_mode != ImageMipMode::None) {
				return Fail(inst.pc, error,
				            "GPU-selected storage-image descriptor tables are unsupported");
			}
			if (image.dynamic_table_source != ScalarProvenance::Undefined &&
			    image.dynamic_table_source != dynamic_image.table_source) {
				return Fail(inst.pc, error, "dynamic image resource changed descriptor table");
			}
			if (image.dynamic_table_source != ScalarProvenance::Undefined &&
			    (image.dynamic_table_address_offset != dynamic_image.table_address_offset ||
			     image.dynamic_table_address_count != dynamic_image.table_address_count)) {
				return Fail(inst.pc, error, "dynamic image resource changed descriptor table range");
			}
			image.dynamic_table_source = dynamic_image.table_source;
			image.dynamic_table_address_offset = dynamic_image.table_address_offset;
			image.dynamic_table_address_count = dynamic_image.table_address_count;
		}
		uint32_t sampler = 0;
		if (NeedsSampler(inst.op)) {
			if (!ValidateSource(inst.memory.sampler_source, 4, inst.pc, error)) {
				return false;
			}
			sampler = AddSampler(inst);
			if (sampler == UINT32_MAX) {
				return Fail(inst.pc, error, "sampler resource limit exceeded");
			}
			if (!AddSampledPair(resource, sampler, inst.pc, error)) {
				return false;
			}
		}
		m_patches.push_back({std::ref(inst), resource, sampler, dynamic_image.offset,
		                     dynamic_image.base_offset});
		return true;
	}

	Program&           m_program;
	ShaderInfo         m_info;
	std::vector<Patch> m_patches;
};

} // namespace

bool TrackResources(Program& program, std::string* error) {
	return Tracker(program).Run(error);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
