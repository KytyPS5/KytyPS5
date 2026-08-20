#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ValueProgram.h"

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

void SetError(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
}

bool HasOutput(const std::vector<OutputBinding>& outputs, IR::StageOutputKind kind,
               uint32_t index) {
	return std::any_of(outputs.begin(), outputs.end(), [kind, index](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
}

void CopyProgramInputsAndOutputs(EmitterState& state, const IR::Program& program) {
	for (const auto& input: program.info.inputs) {
		state.inputs.push_back(
		    {input.kind, input.location, input.component_count, 0, input.debug_name});
	}
	for (const auto& output: program.info.outputs) {
		if (HasOutput(state.outputs, output.kind, output.index)) {
			continue;
		}
		state.outputs.push_back({output.kind, output.index, output.location, 0, output.debug_name});
	}
}

uint32_t OutputVariableForExport(const EmitterState& state, const IR::ExportInfo& exp) {
	if (exp.kind == IR::ExportTargetKind::Position) {
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

DescriptorResourceBinding ResourceForDescriptor(const EmitterState&       state,
                                                IR::DescriptorBindingKind kind, uint32_t resource) {
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
	return {descriptor, static_cast<uint32_t>(found - descriptor->resources.begin())};
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

ImageViewKind ImageViewKindFromDimension(Decoder::ImageDimension dimension) {
	switch (dimension) {
		case Decoder::ImageDimension::Dim1D: return ImageViewKind::Dim1D;
		case Decoder::ImageDimension::Dim1DArray: return ImageViewKind::Dim1DArray;
		case Decoder::ImageDimension::Dim2DArray: return ImageViewKind::Dim2DArray;
		case Decoder::ImageDimension::Dim3D: return ImageViewKind::Dim3D;
		case Decoder::ImageDimension::Dim2DMsaa: return ImageViewKind::Dim2DMsaa;
		case Decoder::ImageDimension::Dim2DMsaaArray: return ImageViewKind::Dim2DMsaaArray;
		default: return ImageViewKind::Dim2D;
	}
}

ImageViewKind SampledImageViewKind(const EmitterState& state, const IR::MemoryInfo& mem,
                                   uint32_t use_pc) {
	(void)state;
	(void)use_pc;
	return ImageViewKindFromDimension(mem.image_dimension);
}

ImageViewKind StorageImageViewKind(const IR::MemoryInfo& mem) {
	return ImageViewKindFromDimension(mem.image_dimension);
}

uint32_t ImageViewCoordinateComponents(ImageViewKind view) {
	switch (view) {
		case ImageViewKind::Dim1D: return 1u;
		case ImageViewKind::Dim1DArray:
		case ImageViewKind::Dim2D: return 2u;
		case ImageViewKind::Dim2DArray:
		case ImageViewKind::Dim2DMsaaArray:
		case ImageViewKind::Dim3D: return 3u;
		case ImageViewKind::Dim2DMsaa: return 2u;
		default: return 0u;
	}
}

uint32_t ImageViewSpatialComponents(ImageViewKind view) {
	switch (view) {
		case ImageViewKind::Dim1D:
		case ImageViewKind::Dim1DArray: return 1u;
		case ImageViewKind::Dim2D:
		case ImageViewKind::Dim2DArray:
		case ImageViewKind::Dim2DMsaa:
		case ImageViewKind::Dim2DMsaaArray: return 2u;
		case ImageViewKind::Dim3D: return 3u;
		default: return 0u;
	}
}

uint32_t ImageViewImageType(EmitterState& state, ImageViewKind view, bool integer) {
	const auto component = integer ? TypeU32(state) : TypeF32(state);
	return state.builder.Type(OpTypeImage,
	                          {component, ImageSpirvDimension(view), 0, ImageSpirvArrayed(view),
	                           ImageSpirvMultisampled(view), 1, ImageFormatUnknown});
}

uint32_t ImageViewSampledImageType(EmitterState& state, ImageViewKind view, bool integer) {
	return state.builder.Type(OpTypeSampledImage, {ImageViewImageType(state, view, integer)});
}

uint32_t ImageViewSizeType(EmitterState& state, ImageViewKind view) {
	switch (ImageViewCoordinateComponents(view)) {
		case 1u: return TypeU32(state);
		case 2u: return TypeU32Vector(state, 2);
		case 3u: return TypeU32Vector(state, 3);
		default: return 0;
	}
}

uint32_t StorageImageType(EmitterState& state, StorageImageClass image_class, ImageViewKind view) {
	uint32_t component = TypeF32(state);
	uint32_t format    = ImageFormatUnknown;
	if (image_class == StorageImageClass::FormatlessUint) {
		component = TypeU32(state);
	} else if (image_class == StorageImageClass::AtomicUint) {
		component = TypeU32(state);
		format    = ImageFormatR32ui;
	}
	return state.builder.Type(OpTypeImage, {component, ImageSpirvDimension(view), 0,
	                                        ImageSpirvArrayed(view), 0, 2, format});
}

uint32_t StorageImagePointerType(EmitterState& state, StorageImageClass image_class,
                                 ImageViewKind view) {
	return state.builder.Type(
	    OpTypePointer, {StorageClassUniformConstant, StorageImageType(state, image_class, view)});
}

StorageImageClass StorageImageClassForResource(const EmitterState& state, uint32_t resource) {
	const auto& image      = state.program.info.images.at(resource);
	const bool  uint_image = image.kind == IR::ResourceKind::StorageImageUint;
	EXIT_IF(image.kind != IR::ResourceKind::StorageImage && !uint_image);
	EXIT_IF(image.atomic && !uint_image);
	return StorageImageClassFor(uint_image, image.atomic);
}

uint32_t StorageImageVariable(const EmitterState& state, StorageImageClass image_class,
                              ImageViewKind view) {
	return state.storage_image_variables[StorageImageIndex(image_class, view)];
}

uint32_t LoadSampledImageDescriptor(EmitterState& state, const IR::MemoryInfo& mem, uint32_t use_pc,
                                    ImageViewKind view) {
	(void)use_pc;
	const bool integer      = mem.kind == IR::ResourceKind::ImageUint;
	const auto kind         = SampledBindingKind(integer, view);
	const auto binding      = ResourceForDescriptor(state, kind, mem.resource);
	const auto variable     = state.sampled_image_variables[SampledImageIndex(integer, view)];
	const auto pointer_type = state.builder.Type(
	    OpTypePointer, {StorageClassUniformConstant, ImageViewImageType(state, view, integer)});
	const auto pointer =
	    DescriptorElementPointer(state, pointer_type, variable, binding.array_index, kind,
	                             mem.resource, "sampled image descriptor array was not emitted");
	const auto image = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, ImageViewImageType(state, view, integer), image, pointer});
	return image;
}

uint32_t LoadSamplerDescriptor(EmitterState& state, uint32_t sampler, uint32_t use_pc) {
	(void)use_pc;
	const auto binding = ResourceForDescriptor(state, IR::DescriptorBindingKind::Samplers, sampler);
	const auto sampler_type = state.builder.Type(OpTypeSampler);
	const auto pointer_type =
	    state.builder.Type(OpTypePointer, {StorageClassUniformConstant, sampler_type});
	const auto pointer = DescriptorElementPointer(
	    state, pointer_type, state.sampler_variable, binding.array_index,
	    IR::DescriptorBindingKind::Samplers, sampler, "sampler descriptor array was not emitted");
	const auto sampler_id = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, sampler_type, sampler_id, pointer});
	return sampler_id;
}

uint32_t MakeSampledImage(EmitterState& state, const IR::MemoryInfo& mem, uint32_t use_pc,
                          ImageViewKind view) {
	const auto image   = LoadSampledImageDescriptor(state, mem, use_pc, view);
	const auto sampler = LoadSamplerDescriptor(state, mem.sampler, use_pc);
	if (image == 0 || sampler == 0) {
		ExitDescriptorBindingFailure(
		    state, SampledBindingKind(mem.kind == IR::ResourceKind::ImageUint, view), mem.resource,
		    "sampled image or sampler descriptor load failed");
	}
	const auto sampled_image = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpSampledImage,
	     ImageViewSampledImageType(state, view, mem.kind == IR::ResourceKind::ImageUint),
	     sampled_image, image, sampler});
	return sampled_image;
}

uint32_t MakeSampledImage(EmitterState& state, const IR::MemoryInfo& mem, uint32_t use_pc,
                          ImageViewKind view, uint32_t image_resource) {
	auto selected     = mem;
	selected.resource = image_resource;
	return MakeSampledImage(state, selected, use_pc, view);
}

uint32_t StorageImageDescriptorPointer(EmitterState& state, uint32_t resource, ImageViewKind view) {
	const auto image_class = StorageImageClassForResource(state, resource);
	const auto kind        = StorageBindingKind(image_class, view);
	const auto binding  = ResourceForDescriptor(state, kind, resource);
	const auto ptr_type    = StorageImagePointerType(state, image_class, view);
	const auto variable    = StorageImageVariable(state, image_class, view);
	return DescriptorElementPointer(state, ptr_type, variable, binding.array_index, kind, resource,
	                                "storage image descriptor array was not emitted");
}

void EmitStorageImageWrite(EmitterState& state, uint32_t resource, ImageViewKind view,
                           uint32_t mip_lod, uint32_t coord, uint32_t texel) {
	const auto image_class = StorageImageClassForResource(state, resource);
	if (image_class != StorageImageClass::AtomicUint) {
		state.builder.RequireCapability(CapabilityStorageImageWriteWithoutFormat);
	}
	const auto  kind    = StorageBindingKind(image_class, view);
	const auto  binding = ResourceForDescriptor(state, kind, resource);
	const auto& image   = state.program.info.images.at(resource);
	const auto  LoadAt  = [&](uint32_t array_index) {
		const auto pointer = DescriptorElementPointer(
		    state, StorageImagePointerType(state, image_class, view),
		    StorageImageVariable(state, image_class, view), array_index, kind, resource,
		    "storage image descriptor array was not emitted");
		const auto descriptor = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpLoad, StorageImageType(state, image_class, view), descriptor, pointer});
		return descriptor;
	};
	if (image.mip_mode != IR::ImageMipMode::DynamicStorage) {
		state.builder.AddFunction({OpImageWrite, LoadAt(binding.array_index), coord, texel});
		return;
	}
	if (image.mip_count == 0u) {
		ExitDescriptorBindingFailure(state, kind, resource,
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
		state.builder.AddFunction({OpImageWrite, LoadAt(binding.array_index + mip), coord, texel});
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
