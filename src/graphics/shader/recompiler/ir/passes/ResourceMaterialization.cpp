#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/ir/ValueProgram.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <numeric>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;
constexpr uint64_t MaxIndirectImageProbes = 65536u;

Decoder::ImageDimension DescriptorDimension(const DescriptorValue&  descriptor,
                                            Decoder::ImageDimension requested) {
	const bool is_array = requested == Decoder::ImageDimension::Dim1DArray ||
	                      requested == Decoder::ImageDimension::Dim2DArray ||
	                      requested == Decoder::ImageDimension::Dim2DMsaaArray;
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor1D: return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor1DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim1DArray;
			}
			return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor2DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DArray;
			}
			return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaaArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DMsaaArray;
			}
			return Decoder::ImageDimension::Dim2DMsaa;
		case Prospero::ImageType::kColor2D: return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2DMsaa;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor) {
	const auto type   = static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu);
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	if (type < Prospero::ImageType::kColor1D || format == Prospero::BufferFormat::kInvalid) {
		return false;
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 && max_mip == fragments;
	}
	return true;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

uint32_t StorageMipCount(const ImageResource& image, const DescriptorValue& descriptor) {
	if (image.mip_mode != ImageMipMode::DynamicStorage || NullImageDescriptor(descriptor)) {
		return 1;
	}
	const auto base = (descriptor.dwords[3] >> 12u) & 0xfu;
	const auto last = (descriptor.dwords[3] >> 16u) & 0xfu;
	return base <= last ? last - base + 1u : 0u;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

const DescriptorSource* Source(const Program& program, uint32_t source) {
	if (program.values == nullptr || source >= program.values->descriptor_sources.size()) {
		return nullptr;
	}
	return &program.values->descriptor_sources[source];
}

void MarkCleanFlatSlots(const Program& program, const DescriptorSource* source,
                        std::span<uint8_t> slots) {
	if (source == nullptr) {
		return;
	}
	std::vector<Value>       pending(source->dwords.begin(),
	                                 source->dwords.begin() + source->dword_count);
	std::vector<const Inst*> visited;
	while (!pending.empty()) {
		auto value = pending.back().Resolve();
		pending.pop_back();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || std::ranges::find(visited, inst) != visited.end()) {
			continue;
		}
		visited.push_back(inst);
		if (inst->GetOpcode() == ValueOpcode::ReadConst) {
			const auto slot = inst->Arg(1).Resolve();
			if (slot.IsImmediate() && slot.GetType() == Type::U32 && slot.U32() < slots.size()) {
				slots[slot.U32()] = 1u;
				pending.push_back(program.values->srt_reads[slot.U32()].value);
			}
			continue;
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			pending.push_back(inst->Arg(arg));
		}
	}
}

uint64_t ScalarBufferSize(const ShaderBufferResource& descriptor) {
	return descriptor.Stride() == 0u
	           ? descriptor.NumRecords()
	           : static_cast<uint64_t>(descriptor.Stride()) * descriptor.NumRecords();
}

bool ReadSpecializationWord(const SrtRuntime& runtime, uint64_t address, uint32_t& word) {
	return runtime.read_specialization_memory != nullptr &&
	       runtime.read_specialization_memory(runtime.userdata, address, &word);
}

bool ReadScalarBufferWord(const ShaderBufferResource& descriptor, uint32_t dynamic_offset,
                          uint32_t immediate_offset, const SrtRuntime& runtime, uint32_t& word,
                          std::string* error, uint32_t use_pc) {
	const auto byte_offset = static_cast<uint64_t>(dynamic_offset) + immediate_offset;
	const auto aligned     = byte_offset & ~uint64_t {3};
	const auto size        = ScalarBufferSize(descriptor);
	if (aligned > size || size - aligned < sizeof(uint32_t)) {
		word = 0;
		return true;
	}
	const auto base = descriptor.Base48() & ~uint64_t {3};
	if (aligned > AddressMask - base) {
		if (error != nullptr) {
			*error = fmt::format("indirect image scalar read at pc 0x{:08x} exceeds the "
			                     "48-bit address space",
			                     use_pc);
		}
		return false;
	}
	const auto address = base + aligned;
	if (!ReadSpecializationWord(runtime, address, word)) {
		if (error != nullptr) {
			*error = fmt::format("indirect image scalar read at pc 0x{:08x} failed at "
			                     "0x{:016x}",
			                     use_pc, address);
		}
		return false;
	}
	return true;
}

bool MaterializeIndirectImage(const DescriptorSource::IndirectImage& indirect,
                              const DescriptorValue&                 material_value,
                              const DescriptorValue& heap_value, const SrtRuntime& runtime,
                              ResourceSnapshot::IndirectImage& result, std::string* error,
                              uint32_t use_pc) {
	ShaderBufferResource material;
	ShaderBufferResource heap;
	if (!DecodeBufferDescriptor(material_value, material) ||
	    !DecodeBufferDescriptor(heap_value, heap)) {
		if (error != nullptr) {
			*error = fmt::format("indirect image tables at pc 0x{:08x} have invalid descriptors",
			                     use_pc);
		}
		return false;
	}
	if (material.Stride() != indirect.selector_stride) {
		if (error != nullptr) {
			*error =
			    fmt::format("indirect image material table at pc 0x{:08x} changed stride", use_pc);
		}
		return false;
	}

	// S_BUFFER_LOAD ignores vector-buffer swizzle/add-thread fields. The shader computes the
	// record stride explicitly; enumerate every wrapped 32-bit offset that can pass bounds.
	const auto period      = uint64_t {1} << 32u;
	const auto step        = std::gcd<uint64_t>(indirect.selector_stride, period);
	const auto residue     = static_cast<uint64_t>(indirect.selector_offset) % step;
	const auto size        = ScalarBufferSize(material);
	const auto limit       = std::min<uint64_t>(UINT32_MAX, size + 3u);
	const auto probe_count = residue <= limit ? (limit - residue) / step + 1u : 0u;
	if (probe_count > MaxIndirectImageProbes) {
		if (error != nullptr) {
			*error = fmt::format("indirect image material table at pc 0x{:08x} requires {} "
			                     "bounded probes",
			                     use_pc, probe_count);
		}
		return false;
	}

	std::vector<uint32_t>        keys {0u};
	std::unordered_set<uint32_t> seen {0u};
	for (uint64_t offset = residue; offset <= limit && probe_count != 0u; offset += step) {
		uint32_t key = 0;
		if (!ReadScalarBufferWord(material, static_cast<uint32_t>(offset), 0u, runtime, key, error,
		                          use_pc)) {
			return false;
		}
		if (seen.insert(key).second) {
			keys.push_back(key);
		}
		if (limit - offset < step) {
			break;
		}
	}

	ResourceSnapshot::IndirectImage next;
	next.capacity = static_cast<uint32_t>(probe_count + 1u);
	next.keys     = std::move(keys);
	for (const auto key: next.keys) {
		DescriptorValue candidate;
		candidate.dword_count  = 8u;
		const auto heap_offset = key << 5u;
		for (uint32_t dword = 0; dword < candidate.dword_count; dword++) {
			if (!ReadScalarBufferWord(heap, heap_offset, dword * sizeof(uint32_t), runtime,
			                          candidate.dwords[dword], error, use_pc)) {
				return false;
			}
		}
		if (NullImageDescriptor(candidate) || !ValidImageDescriptor(candidate)) {
			candidate.dwords.fill(0);
		}
		const auto found = std::ranges::find(next.descriptors, candidate);
		if (found == next.descriptors.end()) {
			next.descriptors.push_back(candidate);
			next.candidates.push_back(static_cast<uint32_t>(next.descriptors.size() - 1u));
		} else {
			next.candidates.push_back(static_cast<uint32_t>(found - next.descriptors.begin()));
		}
	}
	result = std::move(next);
	return true;
}

uint64_t AddressSpecialization(const AddressResource&           resource,
                               const ResourceSnapshot::Address& snapshot) {
	return resource.kind == ResourceKind::Flat || resource.unbased
	           ? snapshot.binding_base
	           : snapshot.guest_base - snapshot.binding_base;
}

bool IsImageOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ImageQueryDimensions:
		case ValueOpcode::ImageQueryLod:
		case ValueOpcode::ImageRead:
		case ValueOpcode::ImageWrite:
		case ValueOpcode::ImageSampleRaw:
		case ValueOpcode::ImageGatherRaw:
		case ValueOpcode::ImageAtomicIAdd32:
		case ValueOpcode::ImageAtomicUMin32:
		case ValueOpcode::ImageAtomicUMax32:
		case ValueOpcode::ImageAtomicAnd32:
		case ValueOpcode::ImageAtomicOr32:
		case ValueOpcode::ImageAtomicXor32: return true;
		default: return false;
	}
}

size_t FlattenedRuntimeDwords(const Program& program) {
	size_t size = program.values != nullptr ? program.values->srt_reads.size() : 0u;
	for (uint32_t resource = 0; resource < program.info.images.size(); resource++) {
		const auto& image = program.info.images[resource];
		if (image.indirect_root == resource) {
			size = std::max(size, static_cast<size_t>(image.indirect_mapping_offset) + 1u +
			                          static_cast<size_t>(image.indirect_mapping_capacity) * 2u);
		}
	}
	return size;
}

} // namespace

bool ValidateResourceSnapshot(const Program& program, const ResourceSnapshot& snapshot,
                              std::string* error) {
	if (!program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = "shader resources were not tracked";
		}
		return false;
	}
	if (snapshot.buffers.size() != program.info.buffers.size() ||
	    snapshot.images.size() != program.info.images.size() ||
	    snapshot.samplers.size() != program.info.samplers.size() ||
	    snapshot.addresses.size() != program.info.addresses.size()) {
		if (error != nullptr) {
			*error = "resource snapshot does not match dense shader topology";
		}
		return false;
	}
	if (program.values == nullptr ||
	    snapshot.flattened_srt.size() != FlattenedRuntimeDwords(program)) {
		if (error != nullptr) {
			*error = "flattened SRT snapshot does not match the shader plan";
		}
		return false;
	}
	if (program.binding_layout_complete) {
		for (const auto reg: program.bindings.user_data_registers) {
			if (reg < program.user_data_base ||
			    reg - program.user_data_base >= snapshot.user_data.size()) {
				if (error != nullptr) {
					*error = fmt::format("runtime snapshot is missing user SGPR {}", reg);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto alias = program.info.buffers[i].image_alias;
		if (alias != BufferResource::NoImageAlias && alias >= program.info.images.size()) {
			if (error != nullptr) {
				*error = fmt::format("buffer resource {} has invalid image alias {}", i, alias);
			}
			return false;
		}
	}
	const auto CheckWidth = [&](const auto& values, uint32_t width, const char* kind) {
		for (uint32_t i = 0; i < values.size(); i++) {
			if (values[i].dword_count != width) {
				if (error != nullptr) {
					*error = fmt::format("{} descriptor {} has {} dwords", kind, i,
					                     values[i].dword_count);
				}
				return false;
			}
		}
		return true;
	};
	for (uint32_t i = 0; i < snapshot.addresses.size(); i++) {
		if (snapshot.addresses[i].binding_base > snapshot.addresses[i].guest_base) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} binds above its guest base", i);
			}
			return false;
		}
	}
	std::unordered_set<uint32_t> indirect_resources;
	for (const auto& table: snapshot.indirect_images) {
		const auto* source = table.resource < program.info.images.size()
		                         ? Source(program, program.info.images[table.resource].source)
		                         : nullptr;
		if (source == nullptr || !source->indirect_image.has_value() ||
		    program.info.images[table.resource].indirect_root != ImageResource::NoIndirectImage ||
		    table.keys.empty() || table.keys.size() != table.candidates.size() ||
		    table.capacity < table.keys.size() || table.descriptors.empty() ||
		    !indirect_resources.insert(table.resource).second) {
			if (error != nullptr) {
				*error = "indirect image snapshot does not match the shader plan";
			}
			return false;
		}
		for (const auto candidate: table.candidates) {
			if (candidate >= table.descriptors.size()) {
				if (error != nullptr) {
					*error = "indirect image snapshot has an invalid candidate";
				}
				return false;
			}
		}
		if (snapshot.images[table.resource] != table.descriptors[table.candidates[0]]) {
			if (error != nullptr) {
				*error = "indirect image snapshot has an inconsistent root descriptor";
			}
			return false;
		}
	}
	return CheckWidth(snapshot.buffers, 4, "buffer") && CheckWidth(snapshot.images, 8, "image") &&
	       CheckWidth(snapshot.samplers, 4, "sampler");
}

bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot,
                                    std::string* error) {
	if (!ValidateResourceSnapshot(program, snapshot, error)) {
		return false;
	}
	if (!snapshot.indirect_images.empty()) {
		if (error != nullptr) {
			*error = "indirect image snapshot was not specialized";
		}
		return false;
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto&          buffer = program.info.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} has invalid width", i);
			}
			return false;
		}
		if (buffer.packed_stride != descriptor.PackedStride() ||
		    buffer.descriptor_format != descriptor.Format() ||
		    buffer.descriptor_swizzle != descriptor.DstSelXYZW()) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} no longer matches specialization", i);
			}
			return false;
		}
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image      = program.info.images[i];
		const auto& descriptor = snapshot.images[i];
		const auto  mip_count  = StorageMipCount(image, descriptor);
		if (mip_count == 0u || mip_count != image.mip_count) {
			if (error != nullptr) {
				*error = fmt::format("storage image descriptor {} changed mip range", i);
			}
			return false;
		}
		if (NullImageDescriptor(descriptor)) {
			if (image.indirect_root != ImageResource::NoIndirectImage) {
				const auto root = image.indirect_root;
				if (root >= program.info.images.size() ||
				    program.info.images[root].kind != image.kind ||
				    program.info.images[root].dimension != image.dimension ||
				    program.info.images[root].mip_mode != image.mip_mode ||
				    program.info.images[root].mip_count != image.mip_count ||
				    program.info.images[root].storage_swizzle != image.storage_swizzle ||
				    program.info.images[root].cube != image.cube) {
					if (error != nullptr) {
						*error = fmt::format(
						    "null indirect image descriptor {} changed binding class", i);
					}
					return false;
				}
				continue;
			}
			bool canonical_kind =
			    image.kind == ResourceKind::Image || image.kind == ResourceKind::StorageImage;
			if (image.atomic) {
				canonical_kind = image.kind == ResourceKind::StorageImageUint;
			}
			if (image.dimension != Decoder::ImageDimension::Dim2D || image.cube ||
			    !canonical_kind) {
				if (error != nullptr) {
					*error = fmt::format(
					    "image descriptor {} no longer matches canonical null specialization", i);
				}
				return false;
			}
			continue;
		}
		const auto dimension = DescriptorDimension(descriptor, image.dimension);
		if (dimension == Decoder::ImageDimension::Unknown || dimension != image.dimension ||
		    DescriptorIsCube(descriptor) != image.cube) {
			if (error != nullptr) {
				*error =
				    fmt::format("image descriptor {} no longer matches specialized dimension: "
				                "{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
				                i, descriptor.dwords[0], descriptor.dwords[1], descriptor.dwords[2],
				                descriptor.dwords[3], descriptor.dwords[4], descriptor.dwords[5],
				                descriptor.dwords[6], descriptor.dwords[7]);
			}
			return false;
		}
		if (image.kind == ResourceKind::Image || image.kind == ResourceKind::ImageUint ||
		    image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			const bool storage = image.kind == ResourceKind::StorageImage ||
			                     image.kind == ResourceKind::StorageImageUint;
			if (storage && image.storage_swizzle != DescriptorImageSwizzle(descriptor)) {
				if (error != nullptr) {
					*error = fmt::format("storage image descriptor {} changed swizzle", i);
				}
				return false;
			}
			const auto format =
			    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
			const bool raw_sint_storage = storage && format == Prospero::BufferFormat::k32SInt &&
			                              !image.read && !image.atomic;
			const bool uint_descriptor  = Prospero::IsUintTextureFormat(format) || raw_sint_storage;
			const auto uint_program     = image.kind == ResourceKind::ImageUint ||
			                              image.kind == ResourceKind::StorageImageUint;
			if (uint_descriptor != uint_program && !(image.atomic && uint_program)) {
				if (error != nullptr) {
					*error = fmt::format(
					    "image descriptor {} no longer matches specialized format: "
					    "{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
					    i, descriptor.dwords[0], descriptor.dwords[1], descriptor.dwords[2],
					    descriptor.dwords[3], descriptor.dwords[4], descriptor.dwords[5],
					    descriptor.dwords[6], descriptor.dwords[7]);
				}
				return false;
			}
			if (image.depth_compare && uint_program) {
				if (error != nullptr) {
					*error = fmt::format("integer image descriptor {} uses depth comparison", i);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		if (program.info.addresses[i].specialized_base !=
		    AddressSpecialization(program.info.addresses[i], snapshot.addresses[i])) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} no longer matches specialization", i);
			}
			return false;
		}
	}
	return true;
}

bool MaterializeResources(const Program& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, std::string* error) {
	if (!program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = "shader resources were not tracked";
		}
		return false;
	}

	std::vector<DescriptorSourceRequest> requests;
	std::vector<uint8_t>                 clean_flat_slots(program.values->srt_reads.size());
	requests.reserve(program.info.buffers.size() + program.info.images.size() * 2u +
	                 program.info.samplers.size() + program.info.addresses.size());
	for (const auto& buffer: program.info.buffers) {
		requests.push_back({buffer.source, buffer.first_use_pc});
	}
	for (const auto& image: program.info.images) {
		const auto* source = Source(program, image.source);
		if (source != nullptr && source->indirect_image.has_value()) {
			if (runtime.read_specialization_memory == nullptr) {
				if (error != nullptr) {
					*error = fmt::format("indirect image table at pc 0x{:08x} has no clean "
					                     "memory reader",
					                     image.first_use_pc);
				}
				return false;
			}
			MarkCleanFlatSlots(program, Source(program, source->indirect_image->material_source),
			                   clean_flat_slots);
			MarkCleanFlatSlots(program, Source(program, source->indirect_image->heap_source),
			                   clean_flat_slots);
		} else {
			requests.push_back({image.source, image.first_use_pc});
		}
	}
	for (const auto& sampler: program.info.samplers) {
		requests.push_back({sampler.source, sampler.first_use_pc});
	}
	for (const auto& address: program.info.addresses) {
		if (!address.unbased) {
			requests.push_back({address.source, address.first_use_pc});
		}
	}

	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	if (!EvaluateRuntimeSources(program, requests, runtime, values, flattened_srt, clean_flat_slots,
	                            error)) {
		return false;
	}

	ResourceSnapshot next;
	auto             cursor = values.begin();
	next.buffers.assign(cursor, cursor + program.info.buffers.size());
	cursor += program.info.buffers.size();
	for (auto& descriptor: next.buffers) {
		ShaderBufferResource buffer;
		if (DecodeBufferDescriptor(descriptor, buffer) && buffer.Type() != 0) {
			descriptor.dwords.fill(0);
		}
	}
	next.flattened_srt = std::move(flattened_srt);
	next.flattened_srt.resize(FlattenedRuntimeDwords(program));
	next.images.resize(program.info.images.size());
	std::vector<uint8_t> image_written(program.info.images.size());
	for (uint32_t image_index = 0; image_index < program.info.images.size(); image_index++) {
		const auto& image  = program.info.images[image_index];
		const auto* source = Source(program, image.source);
		if (source != nullptr && source->indirect_image.has_value()) {
			if (image.indirect_root != ImageResource::NoIndirectImage &&
			    image.indirect_root != image_index) {
				continue;
			}
			const std::array requests {
			    DescriptorSourceRequest {source->indirect_image->material_source,
			                             image.first_use_pc},
			    DescriptorSourceRequest {source->indirect_image->heap_source, image.first_use_pc}};
			SrtRuntime clean_runtime  = runtime;
			clean_runtime.read_memory = runtime.read_specialization_memory;
			std::vector<DescriptorValue> tables;
			if (!EvaluateDescriptorSources(program, requests, clean_runtime, tables, error)) {
				return false;
			}
			const auto&                     material = tables[0];
			const auto&                     heap     = tables[1];
			ResourceSnapshot::IndirectImage table;
			if (!MaterializeIndirectImage(*source->indirect_image, material, heap, runtime, table,
			                              error, image.first_use_pc)) {
				return false;
			}
			if (image.indirect_root == ImageResource::NoIndirectImage) {
				next.images[image_index]   = table.descriptors[table.candidates[0]];
				image_written[image_index] = 1u;
				if (table.descriptors.size() > 1u) {
					table.resource = image_index;
					next.indirect_images.push_back(std::move(table));
				}
				continue;
			}
			if (table.keys.size() > image.indirect_mapping_capacity ||
			    table.descriptors.size() > image.indirect_resources.size()) {
				if (error != nullptr) {
					*error = fmt::format(
					    "indirect image table at pc 0x{:08x} changed candidate topology",
					    image.first_use_pc);
				}
				return false;
			}
			for (uint32_t candidate = 0; candidate < image.indirect_resources.size(); candidate++) {
				const auto resource = image.indirect_resources[candidate];
				if (resource >= program.info.images.size() ||
				    program.info.images[resource].indirect_root != image_index) {
					if (error != nullptr) {
						*error = "indirect image specialization has an invalid candidate resource";
					}
					return false;
				}
				next.images[resource] =
				    table.descriptors[candidate < table.descriptors.size() ? candidate : 0u];
				image_written[resource] = 1u;
			}
			std::vector<uint32_t> order(table.keys.size());
			std::iota(order.begin(), order.end(), 0u);
			std::ranges::sort(order, {}, [&](uint32_t index) { return table.keys[index]; });
			const auto mapping          = image.indirect_mapping_offset;
			next.flattened_srt[mapping] = static_cast<uint32_t>(table.keys.size());
			for (uint32_t entry = 0; entry < order.size(); entry++) {
				const auto source              = order[entry];
				const auto offset              = mapping + 1u + entry * 2u;
				next.flattened_srt[offset]     = table.keys[source];
				next.flattened_srt[offset + 1] = table.candidates[source];
			}
		} else {
			auto descriptor = *cursor++;
			if (!ValidImageDescriptor(descriptor)) {
				descriptor.dwords.fill(0);
			}
			next.images[image_index]   = descriptor;
			image_written[image_index] = 1u;
		}
	}
	if (std::ranges::find(image_written, uint8_t {0}) != image_written.end()) {
		if (error != nullptr) {
			*error = "indirect image specialization left an unmaterialized candidate";
		}
		return false;
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	cursor += program.info.samplers.size();
	for (const auto& address: program.info.addresses) {
		if (!address.unbased) {
			const auto value = *cursor++;
			auto       base  = (static_cast<uint64_t>(value.dwords[0]) |
			                    static_cast<uint64_t>(value.dwords[1]) << 32u) &
			                   AddressMask;
			if (address.kind == ResourceKind::ScalarAddress) {
				base &= ~uint64_t {3};
			}
			const auto before = static_cast<uint64_t>(-static_cast<int64_t>(address.min_offset));
			uint64_t   binding_base = 0;
			if (address.kind == ResourceKind::Flat) {
				binding_base = base & ~(FlatAddressWindowSize - 1u);
			} else if (base >= before) {
				binding_base = base - before;
			}
			next.addresses.push_back({base, binding_base});
		} else {
			if (!runtime.flat_memory_base.has_value()) {
				if (error != nullptr) {
					*error =
					    fmt::format("unbased {} address at pc 0x{:08x} requires runtime "
					                "guest-address translation",
					                address.kind == ResourceKind::Flat ? "FLAT" : "global/scratch",
					                address.first_use_pc);
				}
				return false;
			}
			next.addresses.push_back({*runtime.flat_memory_base, *runtime.flat_memory_base});
		}
	}
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	if (!ValidateResourceSnapshot(program, next, error)) {
		return false;
	}
	snapshot = std::move(next);
	return true;
}

bool SpecializeResources(Program& program, ResourceSnapshot& snapshot, std::string* error) {
	if (!program.resource_tracking_complete || program.shader_info_complete ||
	    program.binding_layout_complete) {
		if (error != nullptr) {
			*error = !program.resource_tracking_complete ? "shader resources were not tracked"
			                                             : "resource specialization is too late";
		}
		return false;
	}
	if (!ValidateResourceSnapshot(program, snapshot, error)) {
		return false;
	}

	auto next          = program.info;
	auto next_snapshot = snapshot;
	for (const auto& table: snapshot.indirect_images) {
		if (table.resource >= next.images.size() || table.descriptors.size() < 2u ||
		    next.images.size() + table.descriptors.size() - 1u > ShaderInfo::MaxImages) {
			if (error != nullptr) {
				*error = "indirect image candidates exceed the dense image resource limit";
			}
			return false;
		}
		const auto            root_image = next.images[table.resource];
		std::vector<uint32_t> resources(table.descriptors.size());
		resources[0] = table.resource;
		for (uint32_t candidate = 1; candidate < table.descriptors.size(); candidate++) {
			resources[candidate] = static_cast<uint32_t>(next.images.size());
			auto image           = root_image;
			image.indirect_root  = table.resource;
			next.images.push_back(std::move(image));
			next_snapshot.images.push_back(table.descriptors[candidate]);
		}
		auto& root                     = next.images[table.resource];
		root.indirect_root             = table.resource;
		root.indirect_mapping_offset   = static_cast<uint32_t>(next_snapshot.flattened_srt.size());
		root.indirect_mapping_capacity = table.capacity;
		root.indirect_resources        = std::move(resources);
		next_snapshot.flattened_srt.resize(next_snapshot.flattened_srt.size() + 1u +
		                                   static_cast<size_t>(table.capacity) * 2u);
		std::vector<uint32_t> order(table.keys.size());
		std::iota(order.begin(), order.end(), 0u);
		std::ranges::sort(order, {}, [&](uint32_t index) { return table.keys[index]; });
		next_snapshot.flattened_srt[root.indirect_mapping_offset] =
		    static_cast<uint32_t>(table.keys.size());
		for (uint32_t entry = 0; entry < order.size(); entry++) {
			const auto source                   = order[entry];
			const auto offset                   = root.indirect_mapping_offset + 1u + entry * 2u;
			next_snapshot.flattened_srt[offset] = table.keys[source];
			next_snapshot.flattened_srt[offset + 1] = table.candidates[source];
		}
		next_snapshot.images[table.resource] = table.descriptors[0];
	}
	next_snapshot.indirect_images.clear();
	for (uint32_t i = 0; i < next.buffers.size(); i++) {
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(next_snapshot.buffers[i], descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} has invalid width", i);
			}
			return false;
		}
		next.buffers[i].packed_stride      = descriptor.PackedStride();
		next.buffers[i].descriptor_format  = descriptor.Format();
		next.buffers[i].descriptor_swizzle = descriptor.DstSelXYZW();
	}
	for (uint32_t i = 0; i < next.addresses.size(); i++) {
		next.addresses[i].specialized_base =
		    AddressSpecialization(next.addresses[i], next_snapshot.addresses[i]);
	}
	for (uint32_t i = 0; i < next.images.size(); i++) {
		const auto& descriptor = next_snapshot.images[i];
		auto&       image      = next.images[i];
		image.mip_count        = StorageMipCount(image, descriptor);
		if (image.mip_count == 0u) {
			if (error != nullptr) {
				*error = fmt::format("storage image descriptor {} has an invalid mip range", i);
			}
			return false;
		}
		if (NullImageDescriptor(descriptor)) {
			image.dimension = Decoder::ImageDimension::Dim2D;
			image.cube      = false;
			switch (image.kind) {
				case ResourceKind::ImageUint: image.kind = ResourceKind::Image; break;
				case ResourceKind::StorageImageUint:
					if (!image.atomic) {
						image.kind = ResourceKind::StorageImage;
					}
					break;
				default: break;
			}
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor, image.dimension);
		if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
			if (error != nullptr) {
				*error = fmt::format(
				    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
				    "{:08x},{:08x},{:08x},{:08x}",
				    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0],
				    descriptor.dwords[1], descriptor.dwords[2], descriptor.dwords[3],
				    descriptor.dwords[4], descriptor.dwords[5], descriptor.dwords[6],
				    descriptor.dwords[7]);
			}
			return false;
		}
		image.dimension = descriptor_dimension;
		image.cube      = DescriptorIsCube(descriptor);
		if (image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			image.storage_swizzle = DescriptorImageSwizzle(descriptor);
		}
		const auto format =
		    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
		const bool storage = image.kind == ResourceKind::StorageImage ||
		                     image.kind == ResourceKind::StorageImageUint;
		const bool raw_sint_storage =
		    storage && format == Prospero::BufferFormat::k32SInt && !image.read && !image.atomic;
		const bool uint_image = Prospero::IsUintTextureFormat(format) || raw_sint_storage;
		if (uint_image) {
			switch (image.kind) {
				case ResourceKind::Image: image.kind = ResourceKind::ImageUint; break;
				case ResourceKind::StorageImage: image.kind = ResourceKind::StorageImageUint; break;
				default: break;
			}
		}
	}
	for (uint32_t root_index = 0; root_index < next.images.size(); root_index++) {
		auto& root = next.images[root_index];
		if (root.indirect_root != root_index) {
			continue;
		}
		if (root.indirect_mapping_capacity == 0u || root.indirect_resources.size() < 2u ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(root.indirect_mapping_capacity) * 2u >
		        next_snapshot.flattened_srt.size()) {
			if (error != nullptr) {
				*error = "indirect image specialization has an invalid key mapping";
			}
			return false;
		}
		uint32_t exemplar = ImageResource::NoIndirectImage;
		for (const auto resource: root.indirect_resources) {
			if (resource >= next.images.size() ||
			    next.images[resource].indirect_root != root_index) {
				if (error != nullptr) {
					*error = "indirect image specialization has an invalid candidate";
				}
				return false;
			}
			if (!NullImageDescriptor(next_snapshot.images[resource])) {
				exemplar = resource;
				break;
			}
		}
		if (exemplar == ImageResource::NoIndirectImage) {
			if (error != nullptr) {
				*error = "indirect image specialization has no typed candidate";
			}
			return false;
		}
		const auto& image_class = next.images[exemplar];
		for (uint32_t candidate = 0; candidate < next.images.size(); candidate++) {
			auto& image = next.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(next_snapshot.images[candidate])) {
				image.kind            = image_class.kind;
				image.dimension       = image_class.dimension;
				image.mip_count       = image_class.mip_count;
				image.storage_swizzle = image_class.storage_swizzle;
				image.cube            = image_class.cube;
			}
			if (image.kind != image_class.kind || image.dimension != image_class.dimension ||
			    image.mip_mode != image_class.mip_mode ||
			    image.mip_count != image_class.mip_count ||
			    image.storage_swizzle != image_class.storage_swizzle ||
			    image.depth_compare != image_class.depth_compare ||
			    image.cube != image_class.cube) {
				if (error != nullptr) {
					*error = fmt::format(
					    "indirect image table at pc 0x{:08x} has incompatible candidates",
					    root.first_use_pc);
				}
				return false;
			}
		}
	}
	if (program.values == nullptr) {
		if (error != nullptr) {
			*error = "typed SSA is not ready";
		}
		return false;
	}
	auto memory_info = program.values->memory_info;
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (!IsImageOp(inst.GetOpcode())) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= memory_info.size()) {
				if (error != nullptr) {
					*error = fmt::format("typed image instruction has invalid memory metadata {}",
					                     index);
				}
				return false;
			}
			auto& memory = memory_info[index];
			if (memory.resource >= next.images.size()) {
				if (error != nullptr) {
					*error = fmt::format("typed image instruction has invalid resource {}",
					                     memory.resource);
				}
				return false;
			}
			const auto& image = next.images[memory.resource];
			if (image.indirect_root == memory.resource &&
			    inst.GetOpcode() != ValueOpcode::ImageSampleRaw) {
				if (error != nullptr) {
					*error = fmt::format(
					    "indirect image table at pc 0x{:08x} is used by unsupported {}",
					    inst.Flags<MemoryFlags>().pc, ValueOpcodeName(inst.GetOpcode()));
				}
				return false;
			}
			memory.kind            = image.kind;
			memory.image_dimension = image.dimension;
			memory.image_cube      = image.cube;
		}
	}
	program.info                = std::move(next);
	program.values->memory_info = std::move(memory_info);
	snapshot                    = std::move(next_snapshot);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
