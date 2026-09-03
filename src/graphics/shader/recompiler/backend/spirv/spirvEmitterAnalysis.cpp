#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t PixelParameterMappedLocation(const EmitterState& state, uint32_t attr) {
	if (state.stage != ShaderType::Pixel) {
		return attr;
	}
	return ShaderPixelParameterMappedLocation(*state.input_info.pixel, attr);
}

uint32_t PixelParameterLocation(const EmitterState& state, uint32_t attr) {
	std::array<uint32_t, 32> active_inputs {};
	uint32_t                 active_count = 0;
	for (const auto& input: state.inputs) {
		if (input.kind == IR::StageInputKind::Parameter) {
			active_inputs[active_count++] = input.location;
		}
	}
	return state.stage == ShaderType::Pixel
	           ? ShaderPixelParameterLocation(*state.input_info.pixel,
	                                          {active_inputs.data(), active_count}, attr)
	           : attr;
}

bool PixelParameterIsFlat(const EmitterState& state, uint32_t attr) {
	return state.stage == ShaderType::Pixel &&
	       ShaderPixelParameterIsFlat(*state.input_info.pixel, attr);
}

bool PixelParameterIsCustom(const EmitterState& state, uint32_t attr) {
	return state.stage == ShaderType::Pixel &&
	       ShaderPixelParameterIsCustom(*state.input_info.pixel, attr);
}

bool HasOutput(const std::vector<OutputBinding>& outputs, IR::StageOutputKind kind,
               uint32_t index) {
	return std::any_of(outputs.begin(), outputs.end(), [kind, index](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
}

void CopyProgramInputsAndOutputs(EmitterState& state, const IR::Program& program) {
	for (const auto& input: program.info.inputs) {
		state.inputs.push_back({input.kind, input.location, input.component_count, 0,
		                        input.debug_name, input.per_vertex});
	}
	for (const auto& output: program.info.outputs) {
		if (HasOutput(state.outputs, output.kind, output.index)) {
			continue;
		}
		state.outputs.push_back({output.kind, output.index, output.location, 0, output.debug_name});
	}
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
	     state.program.shader_hash, static_cast<unsigned>(state.stage), resource,
	     static_cast<unsigned>(kind), reason);
	std::abort();
}

uint32_t ResourceForDescriptor(const EmitterState& state, IR::DescriptorBindingKind kind,
                               uint32_t resource) {
	const auto* descriptor = IR::FindBinding(state.program.bindings, kind);
	if (descriptor == nullptr) {
		ExitDescriptorBindingFailure(state, kind, resource, "descriptor group was not allocated");
	}
	const auto found =
	    std::find(descriptor->resources.begin(), descriptor->resources.end(), resource);
	if (found == descriptor->resources.end()) {
		ExitDescriptorBindingFailure(state, kind, resource,
		                             "resource is absent from descriptor group");
	}
	return static_cast<uint32_t>(found - descriptor->resources.begin());
}

uint32_t DescriptorElementPointer(EmitterState& state, uint32_t result_ptr_type,
                                  uint32_t variable_id, uint32_t array_index,
                                  IR::DescriptorBindingKind kind, uint32_t resource,
                                  const char* variable_name) {
	if (variable_id == 0) {
		ExitDescriptorBindingFailure(state, kind, resource, variable_name);
	}
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpAccessChain, result_ptr_type, pointer, variable_id, ConstantU32(state, array_index)});
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
	return state.builder.Type(OpTypeVector, {ImageScalarType(state, numeric_class), components});
}

uint32_t ImageType(EmitterState& state, const IR::ImageResource& image) {
	uint32_t sampled = 0;
	uint32_t format  = ImageFormatUnknown;
	if (image.resource_class == IR::ImageResourceClass::Sampled) {
		EXIT_IF(image.atomic);
		sampled = 1;
	} else if (image.resource_class == IR::ImageResourceClass::Storage) {
		EXIT_IF(image.numeric_class == Prospero::TextureNumericClass::Sint ||
		        image.numeric_class == Prospero::TextureNumericClass::Unsupported);
		sampled = 2;
		if (image.atomic) {
			EXIT_IF(image.numeric_class != Prospero::TextureNumericClass::Uint);
			format = ImageFormatR32ui;
		}
	} else {
		EXIT("invalid image resource class");
	}
	const auto& info = ImageDimensionInfoFor(image.dimension);
	return state.builder.Type(OpTypeImage,
	                          {ImageScalarType(state, image.numeric_class), info.spirv_dimension,
	                           image.depth_compare ? 1u : 0u,
	                           info.arrayed, info.multisampled, sampled, format});
}

uint32_t ImageViewSizeType(EmitterState& state, ImageDimension dimension) {
	switch (ImageDimensionInfoFor(dimension).coordinate_components) {
		case 1u: return TypeU32(state);
		case 2u: return TypeU32Vector(state, 2);
		case 3u: return TypeU32Vector(state, 3);
		default: return 0;
	}
}

uint32_t LoadSampledImageDescriptor(EmitterState& state, uint32_t resource) {
	const auto& image_resource = state.program.info.images.at(resource);
	EXIT_IF(image_resource.resource_class != IR::ImageResourceClass::Sampled);
	const auto kind = IR::DescriptorBindingForImage(image_resource);
	EXIT_IF(!kind.has_value());
	const auto array_index  = ResourceForDescriptor(state, *kind, resource);
	const auto variable     = state.image_variables[IR::ImageBindingIndex(*kind)];
	const auto pointer_type = state.builder.Type(
	    OpTypePointer, {StorageClassUniformConstant, ImageType(state, image_resource)});
	const auto pointer =
	    DescriptorElementPointer(state, pointer_type, variable, array_index, *kind, resource,
	                             "sampled image descriptor array was not emitted");
	const auto image = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, ImageType(state, image_resource), image, pointer});
	return image;
}

uint32_t LoadSamplerDescriptor(EmitterState& state, uint32_t sampler) {
	const auto array_index =
	    ResourceForDescriptor(state, IR::DescriptorBindingKind::Samplers, sampler);
	const auto sampler_type = state.builder.Type(OpTypeSampler);
	const auto pointer_type =
	    state.builder.Type(OpTypePointer, {StorageClassUniformConstant, sampler_type});
	const auto pointer = DescriptorElementPointer(
	    state, pointer_type, state.sampler_variable, array_index,
	    IR::DescriptorBindingKind::Samplers, sampler, "sampler descriptor array was not emitted");
	const auto sampler_id = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, sampler_type, sampler_id, pointer});
	return sampler_id;
}

uint32_t MakeSampledImage(EmitterState& state, uint32_t resource, uint32_t sampler) {
	const auto& image_resource = state.program.info.images.at(resource);
	const auto  image          = LoadSampledImageDescriptor(state, resource);
	const auto  sampler_id     = LoadSamplerDescriptor(state, sampler);
	const auto  sampled_image = state.builder.AllocateId();
	const auto  sampled_type  =
	    state.builder.Type(OpTypeSampledImage, {ImageType(state, image_resource)});
	state.builder.AddFunction({OpSampledImage, sampled_type, sampled_image, image, sampler_id});
	return sampled_image;
}

uint32_t StorageImageDescriptorPointer(EmitterState& state, uint32_t resource) {
	const auto& image = state.program.info.images.at(resource);
	EXIT_IF(image.resource_class != IR::ImageResourceClass::Storage);
	const auto kind = IR::DescriptorBindingForImage(image);
	EXIT_IF(!kind.has_value());
	const auto array_index = ResourceForDescriptor(state, *kind, resource);
	const auto pointer_type =
	    state.builder.Type(OpTypePointer, {StorageClassUniformConstant, ImageType(state, image)});
	const auto variable = state.image_variables[IR::ImageBindingIndex(*kind)];
	return DescriptorElementPointer(state, pointer_type, variable, array_index, *kind, resource,
	                                "storage image descriptor array was not emitted");
}

void EmitStorageImageWrite(EmitterState& state, uint32_t resource, uint32_t mip_lod, uint32_t coord,
                           uint32_t texel) {
	const auto& image = state.program.info.images.at(resource);
	EXIT_IF(image.resource_class != IR::ImageResourceClass::Storage);
	if (!image.atomic) {
		state.builder.RequireCapability(CapabilityStorageImageWriteWithoutFormat);
	}
	const auto kind = IR::DescriptorBindingForImage(image);
	EXIT_IF(!kind.has_value());
	const auto array_index = ResourceForDescriptor(state, *kind, resource);
	const auto image_type  = ImageType(state, image);
	const auto pointer_type =
	    state.builder.Type(OpTypePointer, {StorageClassUniformConstant, image_type});
	const auto variable = state.image_variables[IR::ImageBindingIndex(*kind)];
	const auto LoadAt   = [&](uint32_t array_index) {
		const auto pointer =
		    DescriptorElementPointer(state, pointer_type, variable, array_index, *kind, resource,
		                             "storage image descriptor array was not emitted");
		const auto descriptor = state.builder.AllocateId();
		state.builder.AddFunction({OpLoad, image_type, descriptor, pointer});
		return descriptor;
	};
	if (image.mip_mode != IR::ImageMipMode::DynamicStorage) {
		state.builder.AddFunction({OpImageWrite, LoadAt(array_index), coord, texel});
		return;
	}
	if (image.mip_count == 0u) {
		ExitDescriptorBindingFailure(state, *kind, resource,
		                             "dynamic storage image has no mip descriptors");
	}

	const auto            merge_label = state.builder.AllocateId();
	std::vector<uint32_t> labels(image.mip_count);
	std::vector<uint32_t> words {OpSwitch, mip_lod, merge_label};
	for (uint32_t mip = 0; mip < image.mip_count; mip++) {
		labels[mip] = state.builder.AllocateId();
		words.push_back(mip);
		words.push_back(labels[mip]);
	}
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction(words);
	for (uint32_t mip = 0; mip < image.mip_count; mip++) {
		EmitLabel(state, labels[mip]);
		state.builder.AddFunction({OpImageWrite, LoadAt(array_index + mip), coord, texel});
		state.builder.AddFunction({OpBranch, merge_label});
	}
	EmitLabel(state, merge_label);
}

uint32_t ExecutionModelForStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return ExecutionModelVertex;
		case ShaderType::Pixel: return ExecutionModelFragment;
		default: return ExecutionModelGLCompute;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
