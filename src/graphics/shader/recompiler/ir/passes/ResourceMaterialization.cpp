#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
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

[[noreturn]] void SpecializationFail(std::string_view message) {
	EXIT("shader resource specialization failed: %s", std::string(message).c_str());
	std::abort();
}

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

bool ValidImageDescriptor(const DescriptorValue& descriptor, bool r128 = false) {
	const auto type   = static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu);
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	if (type < Prospero::ImageType::kColor1D || format == Prospero::BufferFormat::kInvalid ||
	    format > Prospero::BufferFormat::kBc7Srgb) {
		return false;
	}
	if (r128 && type != Prospero::ImageType::kColor1D && type != Prospero::ImageType::kColor2D &&
	    type != Prospero::ImageType::kColor2DMsaa) {
		return false;
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 &&
		       (r128 || max_mip == fragments);
	}
	return true;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

Prospero::BufferFormat ImageConversionFormat(Prospero::BufferFormat format) {
	return Prospero::RemapTextureFormat(format) != format ? format
	                                                      : Prospero::BufferFormat::kInvalid;
}

Prospero::TextureNumericClass SampledResourceNumericClass(ResourceKind kind) {
	switch (kind) {
		case ResourceKind::Image: return Prospero::TextureNumericClass::Float;
		case ResourceKind::ImageUint: return Prospero::TextureNumericClass::Uint;
		case ResourceKind::ImageSint: return Prospero::TextureNumericClass::Sint;
		default: return Prospero::TextureNumericClass::Unsupported;
	}
}

bool RequiresPointSampler(const ImageResource& image) {
	return image.kind == ResourceKind::ImageSint ||
	       image.conversion_format != Prospero::BufferFormat::kInvalid;
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
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
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
				pending.push_back(program.srt_reads[slot.U32()].value);
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
                          uint32_t immediate_offset, const SrtRuntime& runtime, uint32_t& word) {
	const auto byte_offset = static_cast<uint64_t>(dynamic_offset) + immediate_offset;
	const auto aligned     = byte_offset & ~uint64_t {3};
	const auto size        = ScalarBufferSize(descriptor);
	if (aligned > size || size - aligned < sizeof(uint32_t)) {
		word = 0;
		return true;
	}
	const auto base = descriptor.Base48() & ~uint64_t {3};
	if (aligned > AddressMask - base) {
		return false;
	}
	const auto address = base + aligned;
	if (!ReadSpecializationWord(runtime, address, word)) {
		return false;
	}
	return true;
}

bool MaterializeIndirectImage(const DescriptorSource::IndirectImage& indirect,
                              const DescriptorValue&                 material_value,
                              const DescriptorValue& heap_value, const SrtRuntime& runtime,
                              ResourceSnapshot::IndirectImage& result) {
	ShaderBufferResource material;
	ShaderBufferResource heap;
	if (!DecodeBufferDescriptor(material_value, material) ||
	    !DecodeBufferDescriptor(heap_value, heap)) {
		return false;
	}
	if (material.Stride() != indirect.selector_stride) {
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
		return false;
	}

	std::vector<uint32_t>        keys {0u};
	std::unordered_set<uint32_t> seen {0u};
	for (uint64_t offset = residue; offset <= limit && probe_count != 0u; offset += step) {
		uint32_t key = 0;
		if (!ReadScalarBufferWord(material, static_cast<uint32_t>(offset), 0u, runtime, key)) {
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
			                          candidate.dwords[dword])) {
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

size_t FlattenedRuntimeDwords(const Program& program) {
	size_t size = program.srt_reads.size();
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

bool ValidateResourceSnapshot(const Program& program, const ResourceSnapshot& snapshot) {
	if (!program.resource_tracking_complete) {
		return false;
	}
	if (snapshot.buffers.size() != program.info.buffers.size() ||
	    snapshot.images.size() != program.info.images.size() ||
	    snapshot.samplers.size() != program.info.samplers.size()) {
		return false;
	}
	if (snapshot.flattened_srt.size() != FlattenedRuntimeDwords(program)) {
		return false;
	}
	if (program.binding_layout_complete) {
		for (const auto reg: program.bindings.user_data_registers) {
			if (reg < program.user_data_base ||
			    reg - program.user_data_base >= snapshot.user_data.size()) {
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto alias = program.info.buffers[i].image_alias;
		if (alias != BufferResource::NoImageAlias && alias >= program.info.images.size()) {
			return false;
		}
	}
	const auto CheckWidth = [&](const auto& values, uint32_t width) {
		for (uint32_t i = 0; i < values.size(); i++) {
			if (values[i].dword_count != width) {
				return false;
			}
		}
		return true;
	};
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
			return false;
		}
		for (const auto candidate: table.candidates) {
			if (candidate >= table.descriptors.size()) {
				return false;
			}
		}
		if (snapshot.images[table.resource] != table.descriptors[table.candidates[0]]) {
			return false;
		}
	}
	return CheckWidth(snapshot.buffers, 4) && CheckWidth(snapshot.images, 8) &&
	       CheckWidth(snapshot.samplers, 4);
}

bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot) {
	if (!ValidateResourceSnapshot(program, snapshot)) {
		return false;
	}
	if (!snapshot.indirect_images.empty()) {
		return false;
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto&          buffer = program.info.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], descriptor)) {
			return false;
		}
		if (buffer.packed_stride != descriptor.PackedStride() ||
		    buffer.descriptor_format != descriptor.Format() ||
		    buffer.descriptor_swizzle != descriptor.DstSelXYZW()) {
			return false;
		}
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image      = program.info.images[i];
		const auto& descriptor = snapshot.images[i];
		const auto  mip_count  = StorageMipCount(image, descriptor);
		if (mip_count == 0u || mip_count != image.mip_count) {
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
				    program.info.images[root].conversion_format != image.conversion_format ||
				    program.info.images[root].shader_swizzle != image.shader_swizzle ||
				    program.info.images[root].cube != image.cube) {
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
				return false;
			}
			continue;
		}
		const auto dimension = DescriptorDimension(descriptor, image.dimension);
		if (dimension == Decoder::ImageDimension::Unknown || dimension != image.dimension ||
		    DescriptorIsCube(descriptor) != image.cube) {
			return false;
		}
		if (ImageResourceKindMatches(image.kind, ImageResourceClass::Sampled) ||
		    ImageResourceKindMatches(image.kind, ImageResourceClass::Storage)) {
			const bool storage = image.kind == ResourceKind::StorageImage ||
			                     image.kind == ResourceKind::StorageImageUint;
			const auto format =
			    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
			if (image.atomic && format != Prospero::BufferFormat::k32UInt) {
				return false;
			}
			const auto conversion_format = ImageConversionFormat(format);
			if (image.conversion_format != conversion_format) {
				return false;
			}
			if ((storage || conversion_format != Prospero::BufferFormat::kInvalid) &&
			    image.shader_swizzle != DescriptorImageSwizzle(descriptor)) {
				return false;
			}
			if (storage) {
				const bool raw_sint = format == Prospero::BufferFormat::k32SInt && image.written &&
				                      !image.read && !image.atomic;
				const auto numeric_class = Prospero::SampledTextureNumericClass(format);
				if ((!raw_sint && numeric_class == Prospero::TextureNumericClass::Sint) ||
				    numeric_class == Prospero::TextureNumericClass::Unsupported) {
					return false;
				}
				const bool uint_descriptor =
				    numeric_class == Prospero::TextureNumericClass::Uint || raw_sint;
				const bool uint_program = image.kind == ResourceKind::StorageImageUint;
				if (uint_descriptor != uint_program && !(image.atomic && uint_program)) {
					return false;
				}
			} else {
				const auto numeric_class = SampledResourceNumericClass(image.kind);
				if (numeric_class != Prospero::SampledTextureNumericClass(format) ||
				    (image.depth_compare &&
				     numeric_class != Prospero::TextureNumericClass::Float)) {
					return false;
				}
			}
		}
	}
	return true;
}

bool MaterializeResources(const Program& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot) {
	if (!program.resource_tracking_complete) {
		return false;
	}

	std::vector<DescriptorSourceRequest> requests;
	std::vector<uint8_t>                 clean_flat_slots(program.srt_reads.size());
	requests.reserve(program.info.buffers.size() + program.info.images.size() * 2u +
	                 program.info.samplers.size());
	for (const auto& buffer: program.info.buffers) {
		requests.push_back({buffer.source});
	}
	for (const auto& image: program.info.images) {
		const auto* source = Source(program, image.source);
		if (source != nullptr && source->indirect_image.has_value()) {
			if (runtime.read_specialization_memory == nullptr) {
				return false;
			}
			MarkCleanFlatSlots(program, Source(program, source->indirect_image->material_source),
			                   clean_flat_slots);
			MarkCleanFlatSlots(program, Source(program, source->indirect_image->heap_source),
			                   clean_flat_slots);
		} else {
			requests.push_back({image.source});
		}
	}
	for (const auto& sampler: program.info.samplers) {
		requests.push_back({sampler.source});
	}
	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	if (!EvaluateRuntimeSources(program, requests, runtime, values, flattened_srt,
	                            clean_flat_slots)) {
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
			    DescriptorSourceRequest {source->indirect_image->material_source},
			    DescriptorSourceRequest {source->indirect_image->heap_source}};
			SrtRuntime clean_runtime  = runtime;
			clean_runtime.read_memory = runtime.read_specialization_memory;
			std::vector<DescriptorValue> tables;
			if (!EvaluateDescriptorSources(program, requests, clean_runtime, tables)) {
				return false;
			}
			const auto&                     material = tables[0];
			const auto&                     heap     = tables[1];
			ResourceSnapshot::IndirectImage table;
			if (!MaterializeIndirectImage(*source->indirect_image, material, heap, runtime,
			                              table)) {
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
				return false;
			}
			for (uint32_t candidate = 0; candidate < image.indirect_resources.size(); candidate++) {
				const auto resource = image.indirect_resources[candidate];
				if (resource >= program.info.images.size() ||
				    program.info.images[resource].indirect_root != image_index) {
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
			if (!ValidImageDescriptor(descriptor, image.r128)) {
				descriptor.dwords.fill(0);
			}
			next.images[image_index]   = descriptor;
			image_written[image_index] = 1u;
		}
	}
	if (std::ranges::find(image_written, uint8_t {0}) != image_written.end()) {
		return false;
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	if (!ValidateResourceSnapshot(program, next)) {
		return false;
	}
	snapshot = std::move(next);
	return true;
}

void SpecializeResources(Program& program, ResourceSnapshot& snapshot) {
	if (!program.resource_tracking_complete || program.shader_info_complete ||
	    program.binding_layout_complete) {
		return SpecializationFail(!program.resource_tracking_complete
		                              ? "shader resources were not tracked"
		                              : "resource specialization is too late");
	}
	if (!ValidateResourceSnapshot(program, snapshot)) {
		return SpecializationFail("invalid resource snapshot");
	}

	auto next          = program.info;
	auto next_snapshot = snapshot;
	for (const auto& table: snapshot.indirect_images) {
		if (table.resource >= next.images.size() || table.descriptors.size() < 2u ||
		    next.images.size() + table.descriptors.size() - 1u > ShaderInfo::MaxImages) {
			return SpecializationFail(
			    "indirect image candidates exceed the dense image resource limit");
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
			return SpecializationFail(fmt::format("buffer descriptor {} has invalid width", i));
		}
		next.buffers[i].packed_stride      = descriptor.PackedStride();
		next.buffers[i].descriptor_format  = descriptor.Format();
		next.buffers[i].descriptor_swizzle = descriptor.DstSelXYZW();
	}
	for (uint32_t i = 0; i < next.images.size(); i++) {
		const auto& descriptor = next_snapshot.images[i];
		auto&       image      = next.images[i];
		image.mip_count        = StorageMipCount(image, descriptor);
		if (image.mip_count == 0u) {
			return SpecializationFail(
			    fmt::format("storage image descriptor {} has an invalid mip range", i));
		}
		if (NullImageDescriptor(descriptor)) {
			image.dimension = Decoder::ImageDimension::Dim2D;
			image.cube      = false;
			switch (image.kind) {
				case ResourceKind::ImageUint: image.kind = ResourceKind::Image; break;
				case ResourceKind::ImageSint: image.kind = ResourceKind::Image; break;
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
			return SpecializationFail(fmt::format(
			    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
			    "{:08x},{:08x},{:08x},{:08x}",
			    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0], descriptor.dwords[1],
			    descriptor.dwords[2], descriptor.dwords[3], descriptor.dwords[4],
			    descriptor.dwords[5], descriptor.dwords[6], descriptor.dwords[7]));
		}
		image.dimension = descriptor_dimension;
		image.cube      = DescriptorIsCube(descriptor);
		const auto format =
		    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
		if (image.atomic && format != Prospero::BufferFormat::k32UInt) {
			return SpecializationFail(
			    fmt::format("atomic image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
		const bool storage      = image.kind == ResourceKind::StorageImage ||
		                          image.kind == ResourceKind::StorageImageUint;
		image.conversion_format = ImageConversionFormat(format);
		if (storage || image.conversion_format != Prospero::BufferFormat::kInvalid) {
			image.shader_swizzle = DescriptorImageSwizzle(descriptor);
		}
		const bool raw_sint_storage = storage && format == Prospero::BufferFormat::k32SInt &&
		                              image.written && !image.read && !image.atomic;
		const auto numeric_class = Prospero::SampledTextureNumericClass(format);
		if (storage) {
			if ((!raw_sint_storage && numeric_class == Prospero::TextureNumericClass::Sint) ||
			    numeric_class == Prospero::TextureNumericClass::Unsupported) {
				return SpecializationFail(
				    fmt::format("storage image descriptor {} uses unsupported format {}", i,
				                static_cast<uint32_t>(format)));
			}
			if (numeric_class == Prospero::TextureNumericClass::Uint || raw_sint_storage) {
				image.kind = ResourceKind::StorageImageUint;
			}
		} else {
			switch (numeric_class) {
				case Prospero::TextureNumericClass::Uint:
					image.kind = ResourceKind::ImageUint;
					break;
				case Prospero::TextureNumericClass::Sint:
					image.kind = ResourceKind::ImageSint;
					break;
				case Prospero::TextureNumericClass::Float: break;
				case Prospero::TextureNumericClass::Unsupported:
					return SpecializationFail(
					    fmt::format("sampled image descriptor {} uses unsupported format {}", i,
					                static_cast<uint32_t>(format)));
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
			return SpecializationFail("indirect image specialization has an invalid key mapping");
		}
		uint32_t exemplar = ImageResource::NoIndirectImage;
		for (const auto resource: root.indirect_resources) {
			if (resource >= next.images.size() ||
			    next.images[resource].indirect_root != root_index) {
				return SpecializationFail("indirect image specialization has an invalid candidate");
			}
			if (!NullImageDescriptor(next_snapshot.images[resource])) {
				exemplar = resource;
				break;
			}
		}
		if (exemplar == ImageResource::NoIndirectImage) {
			return SpecializationFail("indirect image specialization has no typed candidate");
		}
		const auto& image_class = next.images[exemplar];
		for (uint32_t candidate = 0; candidate < next.images.size(); candidate++) {
			auto& image = next.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(next_snapshot.images[candidate])) {
				image.kind              = image_class.kind;
				image.dimension         = image_class.dimension;
				image.mip_count         = image_class.mip_count;
				image.conversion_format = image_class.conversion_format;
				image.shader_swizzle    = image_class.shader_swizzle;
				image.cube              = image_class.cube;
			}
			if (image.kind != image_class.kind || image.dimension != image_class.dimension ||
			    image.mip_mode != image_class.mip_mode ||
			    image.mip_count != image_class.mip_count ||
			    image.conversion_format != image_class.conversion_format ||
			    image.shader_swizzle != image_class.shader_swizzle ||
			    image.depth_compare != image_class.depth_compare ||
			    image.cube != image_class.cube) {
				return SpecializationFail(
				    fmt::format("indirect image table at pc 0x{:08x} has incompatible candidates",
				                root.first_use_pc));
			}
		}
	}
	auto memory_info = program.memory_info;

	struct SamplerUsage {
		bool native     = false;
		bool point_only = false;
	};
	std::vector<SamplerUsage> sampler_usage(next.samplers.size());
	for (const auto& pair: next.sampled_pairs) {
		if (pair.image >= next.images.size() || pair.sampler >= next.samplers.size()) {
			return SpecializationFail(
			    fmt::format("sampled resource pair at pc 0x{:08x} is invalid", pair.first_use_pc));
		}
		auto& usage = sampler_usage[pair.sampler];
		if (RequiresPointSampler(next.images[pair.image])) {
			usage.point_only = true;
		} else {
			usage.native = true;
		}
	}
	std::vector<uint32_t> point_sampler(next.samplers.size(), UINT32_MAX);
	for (uint32_t i = 0; i < sampler_usage.size(); i++) {
		if (!sampler_usage[i].point_only) {
			continue;
		}
		if (!sampler_usage[i].native) {
			next.samplers[i].force_point_filtering = true;
			point_sampler[i]                       = i;
			continue;
		}
		if (next.samplers.size() >= ShaderInfo::MaxSamplers) {
			return SpecializationFail(
			    "point-filter sampler variants exceed the dense sampler resource limit");
		}
		point_sampler[i]              = static_cast<uint32_t>(next.samplers.size());
		auto sampler                  = next.samplers[i];
		sampler.force_point_filtering = true;
		next.samplers.push_back(sampler);
		next_snapshot.samplers.push_back(next_snapshot.samplers[i]);
	}
	for (auto& pair: next.sampled_pairs) {
		if (RequiresPointSampler(next.images[pair.image])) {
			pair.sampler = point_sampler[pair.sampler];
		}
	}
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto image_opcode = ImageOpcodeInfoOf(inst.GetOpcode());
			if (image_opcode.access == ImageAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= memory_info.size()) {
				return SpecializationFail(
				    fmt::format("typed image instruction has invalid memory metadata {}", index));
			}
			auto& memory = memory_info[index];
			if (memory.resource >= next.images.size()) {
				return SpecializationFail(fmt::format(
				    "typed image instruction has invalid resource {}", memory.resource));
			}
			const auto& image = next.images[memory.resource];
			if (image_opcode.needs_sampler && RequiresPointSampler(image) &&
			    memory.sampler < point_sampler.size()) {
				memory.sampler = point_sampler[memory.sampler];
			}
			if (image.indirect_root == memory.resource &&
			    inst.GetOpcode() != ValueOpcode::ImageSampleRaw) {
				return SpecializationFail(
				    fmt::format("indirect image table at pc 0x{:08x} is used by unsupported {}",
				                inst.Flags<MemoryFlags>().pc, ValueOpcodeName(inst.GetOpcode())));
			}
			memory.kind            = image.kind;
			memory.image_dimension = image.dimension;
			memory.image_cube      = image.cube;
		}
	}
	program.info        = std::move(next);
	program.memory_info = std::move(memory_info);
	snapshot            = std::move(next_snapshot);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
