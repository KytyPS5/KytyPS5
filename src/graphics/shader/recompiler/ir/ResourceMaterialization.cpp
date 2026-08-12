#include "graphics/shader/recompiler/ir/ResourceMaterialization.h"

#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <functional>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask                 = 0x0000ffffffffffffull;
constexpr uint32_t DynamicImageDescriptorBytes = 8u * sizeof(uint32_t);
constexpr uint32_t MaxDynamicImageDescriptors  = 65536;

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
	if (type < Prospero::ImageType::kColor1D || !Prospero::IsValidBufferFormat(format)) {
		return false;
	}
	// Agc::Core::Texture::setArrayView defines these as absolute first/last array-slice indices and
	// the GNMP interop wrapper applies it only to array/cubemap descriptor types.
	const auto base_array = (descriptor.dwords[4] >> 16u) & 0x1fffu;
	const auto last_array = descriptor.dwords[4] & 0x1fffu;
	const bool array_view = type == Prospero::ImageType::kCube ||
	                        type == Prospero::ImageType::kColor1DArray ||
	                        type == Prospero::ImageType::kColor2DArray ||
	                        type == Prospero::ImageType::kColor2DMsaaArray;
	if (array_view && (base_array > last_array || (type == Prospero::ImageType::kCube &&
	                                               (last_array - base_array + 1u) % 6u != 0))) {
		return false;
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 && max_mip == fragments;
	}
	// Agc::Core::Texture::setMipLevelRange requires an absolute base level no greater than the
	// absolute last level. Resource-table storage is frequently reused, so rejecting this relation
	// here prevents stale non-texture words from reaching Vulkan image-view construction.
	const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
	const auto last_level = (descriptor.dwords[3] >> 16u) & 0xfu;
	return base_level <= last_level;
}

bool DynamicDescriptorIsTexture(const DescriptorValue& descriptor) {
	// SCE Agc::Core::ResourceDescriptor stores its two-bit mixed-resource tag in byte 23.
	// Texture is tag zero; buffers, samplers, and unused slots must not be decoded as images.
	return descriptor.dword_count == 8 && ((descriptor.dwords[5] >> 27u) & 0x3u) == 0;
}

bool CompatibleDynamicImageDescriptor(const ImageResource&   image,
                                      const DescriptorValue& descriptor) {
	if (!DynamicDescriptorIsTexture(descriptor)) {
		return false;
	}
	if (NullImageDescriptor(descriptor)) {
		return true;
	}
	if (!ValidImageDescriptor(descriptor) ||
	    DescriptorDimension(descriptor, image.dimension) != image.dimension) {
		return false;
	}
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	const auto integer = image.kind == ResourceKind::ImageUint;
	return Prospero::IsUintTextureFormat(format) == integer && !(image.depth_compare && integer);
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

bool ReadRuntimeDword(const SrtRuntime& runtime, uint64_t address, uint32_t& result) {
	if (runtime.read_memory != nullptr) {
		return runtime.read_memory(runtime.userdata, address, &result);
	}
	std::memcpy(&result, reinterpret_cast<const void*>(address), sizeof(result));
	return true;
}

bool MaterializeDynamicImageTable(const ImageResource& image, const DescriptorValue& table_value,
                                  const SrtRuntime& runtime, std::vector<DescriptorValue>& result,
                                  std::string* error) {
	ShaderBufferResource table;
	if (!DecodeBufferDescriptor(table_value, table)) {
		if (error != nullptr) {
			*error = "dynamic image table has an invalid buffer descriptor";
		}
		return false;
	}
	const auto stride  = static_cast<uint64_t>(table.Stride());
	const auto records = static_cast<uint64_t>(table.NumRecords());
	if (stride != 0 && records > UINT64_MAX / stride) {
		if (error != nullptr) {
			*error = "dynamic image table footprint overflows";
		}
		return false;
	}
	const auto bytes = stride == 0 ? records : stride * records;
	if (bytes == 0 || bytes % DynamicImageDescriptorBytes != 0) {
		if (error != nullptr) {
			*error = fmt::format("dynamic image table has unsupported byte size {}", bytes);
		}
		return false;
	}
	const auto count = bytes / DynamicImageDescriptorBytes;
	if (count > MaxDynamicImageDescriptors) {
		if (error != nullptr) {
			*error = fmt::format("dynamic image table has {} descriptors; limit is {}", count,
			                     MaxDynamicImageDescriptors);
		}
		return false;
	}
	const auto base = table.Base48();
	if (base == 0 || base > AddressMask - bytes) {
		if (error != nullptr) {
			*error = "dynamic image table address is invalid";
		}
		return false;
	}
	std::vector<DescriptorValue> next;
	next.reserve(static_cast<size_t>(count) + 1u);
	for (uint64_t entry = 0; entry < count; entry++) {
		DescriptorValue descriptor;
		descriptor.dword_count = 8;
		for (uint32_t dword = 0; dword < descriptor.dword_count; dword++) {
			const auto address =
			    base + entry * DynamicImageDescriptorBytes + dword * sizeof(uint32_t);
			if (!ReadRuntimeDword(runtime, address, descriptor.dwords[dword])) {
				if (error != nullptr) {
					*error = fmt::format("dynamic image table read failed at 0x{:016x}", address);
				}
				return false;
			}
		}
		if (!CompatibleDynamicImageDescriptor(image, descriptor)) {
			descriptor.dwords.fill(0);
		}
		next.push_back(descriptor);
	}
	DescriptorValue sentinel;
	sentinel.dword_count = 8;
	next.push_back(sentinel);
	result = std::move(next);
	return true;
}

bool MaterializeDynamicImageAddressTable(const ImageResource&          image,
                                         const DescriptorValue&        table_address,
                                         const SrtRuntime&             runtime,
                                         std::vector<DescriptorValue>& result, std::string* error) {
	if (table_address.dword_count != 2 || image.dynamic_table_address_count == 0) {
		if (error != nullptr) {
			*error = "dynamic image address table has invalid metadata";
		}
		return false;
	}
	const auto pointer = (static_cast<uint64_t>(table_address.dwords[0]) |
	                      static_cast<uint64_t>(table_address.dwords[1]) << 32u) &
	                     AddressMask;
	const auto bytes =
	    static_cast<uint64_t>(image.dynamic_table_address_count) * DynamicImageDescriptorBytes;
	if (pointer == 0 || image.dynamic_table_address_offset > AddressMask - pointer ||
	    bytes > AddressMask - (pointer + image.dynamic_table_address_offset)) {
		if (error != nullptr) {
			*error = "dynamic image address table range is invalid";
		}
		return false;
	}
	const auto                   base = pointer + image.dynamic_table_address_offset;
	std::vector<DescriptorValue> next;
	next.reserve(static_cast<size_t>(image.dynamic_table_address_count) + 1u);
	for (uint32_t entry = 0; entry < image.dynamic_table_address_count; entry++) {
		DescriptorValue descriptor;
		descriptor.dword_count = 8;
		for (uint32_t dword = 0; dword < descriptor.dword_count; dword++) {
			const auto address = base + static_cast<uint64_t>(entry) * DynamicImageDescriptorBytes +
			                     dword * sizeof(uint32_t);
			if (!ReadRuntimeDword(runtime, address, descriptor.dwords[dword])) {
				if (error != nullptr) {
					*error = fmt::format("dynamic image address table read failed at 0x{:016x}",
					                     address);
				}
				return false;
			}
		}
		if (!CompatibleDynamicImageDescriptor(image, descriptor)) {
			descriptor.dwords.fill(0);
		}
		next.push_back(descriptor);
	}
	DescriptorValue sentinel;
	sentinel.dword_count = 8;
	next.push_back(sentinel);
	result = std::move(next);
	return true;
}

uint64_t AddressSpecialization(const AddressResource&           resource,
                               const ResourceSnapshot::Address& snapshot) {
	return resource.kind == ResourceKind::Flat || resource.source == ScalarProvenance::Unknown
	           ? snapshot.binding_base
	           : snapshot.guest_base - snapshot.binding_base;
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
	const bool has_dynamic_images =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) { return image.HasDynamicTable(); });
	if (snapshot.buffers.size() != program.info.buffers.size() ||
	    snapshot.images.size() != program.info.images.size() ||
	    (snapshot.image_tables.size() != program.info.images.size() &&
	     (has_dynamic_images || !snapshot.image_tables.empty())) ||
	    snapshot.samplers.size() != program.info.samplers.size() ||
	    snapshot.addresses.size() != program.info.addresses.size()) {
		if (error != nullptr) {
			*error = "resource snapshot does not match dense shader topology";
		}
		return false;
	}
	if (snapshot.flattened_srt.size() != program.srt.reads.size()) {
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
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto&                        image = program.info.images[i];
		const std::vector<DescriptorValue> empty_table;
		const auto& table = snapshot.image_tables.empty() ? empty_table : snapshot.image_tables[i];
		if (!image.HasDynamicTable()) {
			if (!table.empty()) {
				if (error != nullptr) {
					*error = fmt::format("static image resource {} has a dynamic table", i);
				}
				return false;
			}
		} else if (table.empty() ||
		           (image.dynamic_descriptor_count != 0 &&
		            table.size() != static_cast<size_t>(image.dynamic_descriptor_count) + 1u)) {
			if (error != nullptr) {
				*error = fmt::format("dynamic image resource {} has an incompatible table size", i);
			}
			return false;
		}
		for (const auto& descriptor: table) {
			if (descriptor.dword_count != 8) {
				if (error != nullptr) {
					*error = fmt::format("dynamic image resource {} has a malformed descriptor", i);
				}
				return false;
			}
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
	return CheckWidth(snapshot.buffers, 4, "buffer") && CheckWidth(snapshot.images, 8, "image") &&
	       CheckWidth(snapshot.samplers, 4, "sampler");
}

bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot,
                                    std::string* error) {
	if (!ValidateResourceSnapshot(program, snapshot, error)) {
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
		    buffer.descriptor_format != descriptor.Format()) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} no longer matches specialization", i);
			}
			return false;
		}
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image      = program.info.images[i];
		const auto& descriptor = snapshot.images[i];
		if (image.HasDynamicTable()) {
			if (snapshot.image_tables[i].size() !=
			        static_cast<size_t>(image.dynamic_descriptor_count) + 1u ||
			    !std::all_of(snapshot.image_tables[i].begin(), snapshot.image_tables[i].end(),
			                 [&](const auto& value) {
				                 return CompatibleDynamicImageDescriptor(image, value);
			                 })) {
				if (error != nullptr) {
					*error = fmt::format(
					    "dynamic image descriptor table {} no longer matches specialization", i);
				}
				return false;
			}
			continue;
		}
		if (NullImageDescriptor(descriptor)) {
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
	requests.reserve(program.info.buffers.size() + program.info.images.size() +
	                 program.info.samplers.size() + program.info.addresses.size());
	for (const auto& buffer: program.info.buffers) {
		requests.push_back({buffer.source, buffer.first_use_pc});
	}
	for (const auto& image: program.info.images) {
		if (image.dynamic_table_address_count != 0) {
			requests.push_back({image.dynamic_table_source, image.first_use_pc});
		} else if (!image.HasDynamicTable()) {
			requests.push_back({image.source, image.first_use_pc});
		}
	}
	for (const auto& sampler: program.info.samplers) {
		requests.push_back({sampler.source, sampler.first_use_pc});
	}
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			requests.push_back({address.source, address.first_use_pc});
		}
	}

	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	if (!EvaluateRuntimeSources(program, requests, runtime, values, flattened_srt, error)) {
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
	next.images.reserve(program.info.images.size());
	next.image_tables.resize(program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image = program.info.images[i];
		if (image.HasDynamicTable()) {
			DescriptorValue null_descriptor;
			null_descriptor.dword_count = 8;
			next.images.push_back(null_descriptor);
			if (image.dynamic_table_address_count != 0) {
				if (!MaterializeDynamicImageAddressTable(image, *cursor++, runtime,
				                                         next.image_tables[i], error)) {
					return false;
				}
			} else if (image.dynamic_table_buffer >= next.buffers.size() ||
			           !MaterializeDynamicImageTable(image,
			                                         next.buffers[image.dynamic_table_buffer],
			                                         runtime, next.image_tables[i], error)) {
				return false;
			}
		} else {
			auto descriptor = *cursor++;
			if (!ValidImageDescriptor(descriptor)) {
				descriptor.dwords.fill(0);
			}
			next.images.push_back(descriptor);
		}
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	cursor += program.info.samplers.size();
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			const auto value = *cursor++;
			auto       base  = (static_cast<uint64_t>(value.dwords[0]) |
                         static_cast<uint64_t>(value.dwords[1]) << 32u) &
			            AddressMask;
			if (address.kind == ResourceKind::ScalarBuffer) {
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
	next.flattened_srt = std::move(flattened_srt);
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	if (!ValidateResourceSnapshot(program, next, error)) {
		return false;
	}
	snapshot = std::move(next);
	return true;
}

bool SpecializeResources(Program& program, const ResourceSnapshot& snapshot, std::string* error) {
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

	auto next = program.info;
	for (uint32_t i = 0; i < next.buffers.size(); i++) {
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} has invalid width", i);
			}
			return false;
		}
		next.buffers[i].packed_stride     = descriptor.PackedStride();
		next.buffers[i].descriptor_format = descriptor.Format();
	}
	for (uint32_t i = 0; i < next.addresses.size(); i++) {
		next.addresses[i].specialized_base =
		    AddressSpecialization(next.addresses[i], snapshot.addresses[i]);
	}
	for (uint32_t i = 0; i < next.images.size(); i++) {
		const auto& descriptor = snapshot.images[i];
		auto&       image      = next.images[i];
		if (image.HasDynamicTable()) {
			if (snapshot.image_tables[i].empty()) {
				if (error != nullptr) {
					*error = fmt::format("dynamic image resource {} has no table entries", i);
				}
				return false;
			}
			image.dynamic_descriptor_count =
			    static_cast<uint32_t>(snapshot.image_tables[i].size() - 1u);
			continue;
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
	struct ImagePatch {
		std::reference_wrapper<Instruction> inst;
		ResourceKind                        kind;
		Decoder::ImageDimension             dimension;
		bool                                cube;
	};
	std::vector<ImagePatch> patches;
	for (auto& block: program.blocks) {
		for (auto& inst: block.instructions) {
			if (inst.memory.kind != ResourceKind::Image &&
			    inst.memory.kind != ResourceKind::ImageUint &&
			    inst.memory.kind != ResourceKind::StorageImage &&
			    inst.memory.kind != ResourceKind::StorageImageUint) {
				continue;
			}
			if (inst.memory.resource >= next.images.size()) {
				if (error != nullptr) {
					*error = fmt::format("image instruction at pc 0x{:08x} has invalid resource {}",
					                     inst.pc, inst.memory.resource);
				}
				return false;
			}
			const auto& image = next.images[inst.memory.resource];
			patches.push_back({std::ref(inst), image.kind, image.dimension, image.cube});
		}
	}
	program.info = std::move(next);
	for (const auto& patch: patches) {
		patch.inst.get().memory.kind            = patch.kind;
		patch.inst.get().memory.image_dimension = patch.dimension;
		patch.inst.get().memory.image_cube      = patch.cube;
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
