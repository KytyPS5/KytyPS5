#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t PixelParameterLocation(const EmitterState& state, uint32_t attr) {
	std::array<uint32_t, 32> active_inputs {};
	uint32_t                 active_count = 0;
	for (const auto& input: state.inputs) {
		if (input.kind == IR::StageInputKind::Parameter) {
			active_inputs[active_count++] = input.location;
		}
	}
	return state.program.stage == ShaderType::Pixel
	           ? ShaderPixelParameterLocation(*state.input_info.pixel,
	                                          {active_inputs.data(), active_count}, attr)
	           : attr;
}

bool PixelParameterIsFlat(const EmitterState& state, uint32_t attr) {
	return state.program.stage == ShaderType::Pixel &&
	       ShaderPixelParameterIsFlat(*state.input_info.pixel, attr);
}

bool PixelParameterIsCustom(const EmitterState& state, uint32_t attr) {
	return state.program.stage == ShaderType::Pixel &&
	       ShaderPixelParameterIsCustom(*state.input_info.pixel, attr);
}

uint32_t MrtOutputType(EmitterState& state, uint32_t index) {
	const auto& pixel = *state.input_info.pixel;
	if ((pixel.target_sint_mask & (1u << index)) != 0u) return TypeI32Vector(state, 4);
	if ((pixel.target_uint_mask & (1u << index)) != 0u) return TypeU32Vector(state, 4);
	return TypeF32Vector(state, 4);
}

uint32_t OutputVariableForExport(const EmitterState& state, const IR::ExportInfo& exp) {
	if (exp.kind == IR::ExportTargetKind::Position && exp.index == 0) {
		return state.per_vertex_variable;
	}
	if (exp.kind == IR::ExportTargetKind::MrtZ) {
		return state.depth_variable;
	}
	for (const auto& binding: state.outputs) {
		const auto expected_kind = exp.kind == IR::ExportTargetKind::Mrt
		                               ? IR::StageOutputKind::Mrt
		                               : IR::StageOutputKind::Parameter;
		if (binding.kind == expected_kind && binding.index == exp.index) {
			return binding.variable_id;
		}
	}
	return 0;
}

uint32_t          ConstantU32(EmitterState& state, uint32_t value);
[[noreturn]] void ExitDescriptorBindingFailure(const EmitterState&       state,
                                               IR::DescriptorBindingKind kind, uint32_t resource,
                                               const char* reason) {
	EXIT("shader binding resolution failed during SPIR-V emit: hash=0x%016" PRIx64
	     " stage=%u resource=%" PRIu32 " binding_kind=%u reason=%s\n",
	     state.program.shader_hash, static_cast<unsigned>(state.program.stage), resource,
	     static_cast<unsigned>(kind), reason);
	std::abort();
}

uint32_t DescriptorElementPointer(EmitterState& state, uint32_t result_ptr_type,
                                  uint32_t variable_id, uint32_t array_index,
                                  IR::DescriptorBindingKind kind, uint32_t resource,
                                  const char* variable_name) {
	if (variable_id == 0) {
		ExitDescriptorBindingFailure(state, kind, resource, variable_name);
	}
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, result_ptr_type, pointer, variable_id,
	                          array_index);
	return pointer;
}

const ImageDimensionInfo& ImageDimensionInfoFor(ImageDimension dimension) {
	for (const auto& info: ImageDimensions) {
		if (info.dimension == dimension) {
			return info;
		}
	}
	EXIT("SPIR-V image dimension %u is invalid\n", static_cast<uint32_t>(dimension));
	std::abort();
}

uint32_t ImageScalarType(EmitterState& state, Prospero::TextureNumericClass numeric_class) {
	switch (numeric_class) {
		case Prospero::TextureNumericClass::Float: return TypeF32(state);
		case Prospero::TextureNumericClass::Uint: return TypeU32(state);
		case Prospero::TextureNumericClass::Sint: return TypeI32(state);
		case Prospero::TextureNumericClass::Unsupported: break;
	}
	EXIT("invalid image numeric class");
}

uint32_t ImageVectorType(EmitterState& state, Prospero::TextureNumericClass numeric_class,
                         uint32_t components) {
	return state.builder.Type(spv::OpTypeVector, ImageScalarType(state, numeric_class), components);
}

uint32_t ImageType(EmitterState& state, const IR::ImageResource& image) {
	uint32_t sampled = 0;
	uint32_t format  = spv::ImageFormatUnknown;
	if (image.resource_class == IR::ImageResourceClass::Sampled) {
		EXIT_IF(image.atomic);
		sampled = 1;
	} else if (image.resource_class == IR::ImageResourceClass::Storage) {
		EXIT_IF(image.numeric_class == Prospero::TextureNumericClass::Sint ||
		        image.numeric_class == Prospero::TextureNumericClass::Unsupported);
		sampled = 2;
		if (image.atomic) {
			EXIT_IF(image.numeric_class != Prospero::TextureNumericClass::Uint);
			format = image.atomic64 ? spv::ImageFormatR64ui : spv::ImageFormatR32ui;
			if (image.atomic64) {
				state.builder.RequireExtension("SPV_EXT_shader_image_int64");
				state.builder.RequireCapability(spv::CapabilityInt64);
				state.builder.RequireCapability(spv::CapabilityInt64Atomics);
				state.builder.RequireCapability(spv::CapabilityInt64ImageEXT);
			}
		}
	} else {
		EXIT("invalid image resource class");
	}
	if (image.dimension == ImageDimension::Dim1D || image.dimension == ImageDimension::Dim1DArray) {
		state.builder.RequireCapability(sampled == 1 ? spv::CapabilitySampled1D
		                                             : spv::CapabilityImage1D);
	}
	const auto& info = ImageDimensionInfoFor(image.dimension);
	if (sampled == 2 && info.multisampled != 0u) {
		state.builder.RequireCapability(spv::CapabilityStorageImageMultisample);
	}
	const auto  scalar_type =
	    image.atomic64 ? TypeU64(state) : ImageScalarType(state, image.numeric_class);
	return state.builder.Type(spv::OpTypeImage, scalar_type,
	                          info.spirv_dimension, image.depth_compare ? 1u : 0u, info.arrayed,
	                          info.multisampled, sampled, format);
}

uint32_t ImageViewSizeType(EmitterState& state, ImageDimension dimension) {
	switch (ImageDimensionInfoFor(dimension).coordinate_components) {
		case 1u: return TypeU32(state);
		case 2u: return TypeU32Vector(state, 2);
		case 3u: return TypeU32Vector(state, 3);
		default: return 0;
	}
}

namespace {
void DecorateNonUniformDescriptor(EmitterState& state, uint32_t id, spv::Capability capability) {
	state.builder.RequireExtension("SPV_EXT_descriptor_indexing");
	state.builder.RequireCapability(spv::CapabilityShaderNonUniform);
	state.builder.RequireCapability(capability);
	state.builder.AddAnnotation(spv::OpDecorate, id, spv::DecorationNonUniform);
}

ImageDefinition& ImageDescriptorDefinition(EmitterState& state, const IR::ImageResource& image,
                                           IR::DescriptorBindingKind kind) {
	return (image.heap ? state.heap_images : state.images)[IR::ImageBindingIndex(kind)];
}
} // namespace

uint32_t LoadImageDescriptor(EmitterState& state, uint32_t resource, uint32_t mip,
                             uint32_t array_index) {
	const auto pointer = ImageDescriptorPointer(state, resource, mip, array_index);
	const auto image = state.builder.AllocateId();
	const auto& resource_info = state.program.info.images[resource];
	const auto kind = *IR::DescriptorBindingForImage(resource_info);
	state.builder.AddFunction(spv::OpLoad, ImageDescriptorDefinition(state, resource_info, kind).type,
	                          image, pointer);
	if (array_index != 0u) {
		DecorateNonUniformDescriptor(state, image,
		    IR::ImageBindingResourceClass(kind) == IR::ImageResourceClass::Storage
		        ? spv::CapabilityStorageImageArrayNonUniformIndexing
		        : spv::CapabilitySampledImageArrayNonUniformIndexing);
	}
	return image;
}

uint32_t LoadSamplerDescriptor(EmitterState& state, uint32_t sampler, uint32_t array_index) {
	if (sampler >= state.program.info.samplers.size()) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Samplers, sampler,
		                             "sampler resource index is out of range");
	}
	const auto pointer = DescriptorElementPointer(
	    state, state.sampler_pointer_type, state.sampler_variable,
	    array_index == 0u ? ConstantU32(state, sampler) : array_index,
	    IR::DescriptorBindingKind::Samplers, sampler, "sampler descriptor array was not emitted");
	const auto sampler_id = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, state.sampler_type, sampler_id, pointer);
	if (array_index != 0u)
		DecorateNonUniformDescriptor(state, sampler_id, spv::CapabilitySampledImageArrayNonUniformIndexing);
	return sampler_id;
}

uint32_t MakeSampledImage(EmitterState& state, uint32_t resource, uint32_t sampler_id, uint32_t mip,
                          uint32_t array_index, bool sampler_dynamic) {
	const auto& image_resource = state.program.info.images.at(resource);
	EXIT_IF(image_resource.resource_class != IR::ImageResourceClass::Sampled);
	const auto  image          = LoadImageDescriptor(state, resource, mip, array_index);
	const auto  sampled_image = state.builder.AllocateId();
	auto& definition = ImageDescriptorDefinition(state, image_resource,
	                                             *IR::DescriptorBindingForImage(image_resource));
	if (definition.sampled_type == 0) {
		definition.sampled_type = state.builder.Type(spv::OpTypeSampledImage, definition.type);
	}
	state.builder.AddFunction(spv::OpSampledImage, definition.sampled_type, sampled_image, image,
	                          sampler_id);
	if (array_index != 0u || sampler_dynamic) {
		DecorateNonUniformDescriptor(state, sampled_image, spv::CapabilitySampledImageArrayNonUniformIndexing);
	}
	return sampled_image;
}

uint32_t ImageDescriptorPointer(EmitterState& state, uint32_t resource, uint32_t mip,
                                uint32_t array_index) {
	const auto& image = state.program.info.images.at(resource);
	EXIT_IF(mip >= image.mip_count);
	const auto kind = IR::DescriptorBindingForImage(image);
	EXIT_IF(!kind.has_value() || (image.heap && array_index == 0u));
	if (array_index == 0u) {
		array_index = ConstantU32(state, image.descriptor_index + mip);
	}
	const auto& definition = ImageDescriptorDefinition(state, image, *kind);
	return DescriptorElementPointer(state, definition.pointer_type, definition.variable,
	                                array_index, *kind, resource,
	                                "image descriptor array was not emitted");
}

void EmitStorageImageWrite(EmitterState& state, uint32_t resource, uint32_t mip_lod, uint32_t coord,
                           uint32_t texel, uint32_t array_index, uint32_t sample) {
	const auto& image = state.program.info.images.at(resource);
	EXIT_IF(image.resource_class != IR::ImageResourceClass::Storage);
	if (!image.atomic) {
		state.builder.RequireCapability(spv::CapabilityStorageImageWriteWithoutFormat);
	}
	const auto EmitWrite = [&](uint32_t mip) {
		const auto descriptor = LoadImageDescriptor(state, resource, mip, array_index);
		if (sample == 0u) {
			state.builder.AddFunction(spv::OpImageWrite, descriptor, coord, texel);
		} else {
			state.builder.AddFunction(spv::OpImageWrite, descriptor, coord, texel,
			                          spv::ImageOperandsSampleMask, sample);
		}
	};
	if (image.mip_mode != IR::ImageMipMode::Dynamic) {
		EmitWrite(0);
		return;
	}
	EmitIndexSwitch(state, mip_lod, image.mip_count, 0, EmitWrite);
}

spv::ExecutionModel ExecutionModelForStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Local:
		case ShaderType::Vertex: return spv::ExecutionModelVertex;
		case ShaderType::TessellationControl: return spv::ExecutionModelTessellationControl;
		case ShaderType::TessellationEvaluation: return spv::ExecutionModelTessellationEvaluation;
		case ShaderType::Mesh: return spv::ExecutionModelMeshEXT; // MeshEXT
		case ShaderType::Pixel: return spv::ExecutionModelFragment;
		default: return spv::ExecutionModelGLCompute;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
