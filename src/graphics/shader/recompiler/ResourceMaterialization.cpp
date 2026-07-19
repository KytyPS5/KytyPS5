#include "graphics/shader/recompiler/ResourceMaterialization.h"

#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

Decoder::ImageDimension DescriptorDimension(const DescriptorValue& descriptor) {
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube:
		case Prospero::ImageType::kColor1DArray:
		case Prospero::ImageType::kColor2DArray:
		case Prospero::ImageType::kColor2DMsaaArray: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor1D:
		case Prospero::ImageType::kColor2D:
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2D;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor) {
	return ((descriptor.dwords[3] >> 28u) & 0x8u) != 0;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
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
	if (snapshot.buffers.size() != program.info.buffers.size() ||
	    snapshot.images.size() != program.info.images.size() ||
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
		if (NullImageDescriptor(descriptor)) {
			continue;
		}
		const auto dimension = DescriptorDimension(descriptor);
		if (dimension == Decoder::ImageDimension::Unknown || dimension != image.dimension) {
			if (error != nullptr) {
				*error =
				    fmt::format("image descriptor {} no longer matches specialized dimension", i);
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
			const auto uint_descriptor =
			    Prospero::IsUintTextureFormat((descriptor.dwords[1] >> 20u) & 0x1ffu);
			const auto uint_program = image.kind == ResourceKind::ImageUint ||
			                          image.kind == ResourceKind::StorageImageUint;
			if (uint_descriptor != uint_program && !(image.atomic && uint_program)) {
				if (error != nullptr) {
					*error =
					    fmt::format("image descriptor {} no longer matches specialized format", i);
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
                          ResourceSnapshot* snapshot, std::string* error) {
	if (snapshot == nullptr || !program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = snapshot == nullptr ? "invalid resource snapshot"
			                             : "shader resources were not tracked";
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
		requests.push_back({image.source, image.first_use_pc});
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
	if (!EvaluateRuntimeSources(program, requests, runtime, &values, &flattened_srt, error)) {
		return false;
	}

	ResourceSnapshot next;
	auto             cursor = values.begin();
	next.buffers.assign(cursor, cursor + program.info.buffers.size());
	cursor += program.info.buffers.size();
	next.images.assign(cursor, cursor + program.info.images.size());
	cursor += program.info.images.size();
	for (auto& descriptor: next.images) {
		if (!ValidImageDescriptor(descriptor)) {
			descriptor.dwords.fill(0);
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
			const auto binding_base = address.kind == ResourceKind::Flat
			                              ? base & ~(FlatAddressWindowSize - 1u)
			                          : base >= before ? base - before
			                                           : 0;
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
	*snapshot = std::move(next);
	return true;
}

bool SpecializeResources(Program* program, const ResourceSnapshot& snapshot, std::string* error) {
	if (program == nullptr || !program->resource_tracking_complete ||
	    program->shader_info_complete || program->binding_layout_complete) {
		if (error != nullptr) {
			*error = program == nullptr ? "invalid resource specialization program"
			         : !program->resource_tracking_complete ? "shader resources were not tracked"
			                                                : "resource specialization is too late";
		}
		return false;
	}
	if (!ValidateResourceSnapshot(*program, snapshot, error)) {
		return false;
	}

	auto next = program->info;
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
		if (NullImageDescriptor(descriptor)) {
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor);
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
		if (image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			image.storage_swizzle = DescriptorImageSwizzle(descriptor);
		}
		if (Prospero::IsUintTextureFormat((descriptor.dwords[1] >> 20u) & 0x1ffu)) {
			switch (image.kind) {
				case ResourceKind::Image: image.kind = ResourceKind::ImageUint; break;
				case ResourceKind::StorageImage: image.kind = ResourceKind::StorageImageUint; break;
				default: break;
			}
		}
	}
	struct ImagePatch {
		Instruction*            inst;
		ResourceKind            kind;
		Decoder::ImageDimension dimension;
	};
	std::vector<ImagePatch> patches;
	for (auto& block: program->blocks) {
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
			patches.push_back({&inst, image.kind, image.dimension});
		}
	}
	program->info = std::move(next);
	for (const auto& patch: patches) {
		patch.inst->memory.kind            = patch.kind;
		patch.inst->memory.image_dimension = patch.dimension;
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
