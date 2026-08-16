#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ValueProgram.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t PixelParameterMappedLocation(const EmitterState& state, uint32_t attr) {
	const auto* ps = state.pixel_input_info;
	if (state.stage != ShaderType::Pixel || ps == nullptr) {
		return attr;
	}
	return ShaderPixelParameterMappedLocation(*ps, attr);
}

uint32_t PixelParameterLocation(const EmitterState& state, uint32_t attr) {
	std::array<uint32_t, 32> active_inputs {};
	uint32_t                 active_count = 0;
	for (const auto& input: state.inputs) {
		if (input.kind == IR::StageInputKind::Parameter) {
			active_inputs[active_count++] = input.location;
		}
	}
	return state.stage == ShaderType::Pixel && state.pixel_input_info != nullptr
	           ? ShaderPixelParameterLocation(*state.pixel_input_info,
	                                          {active_inputs.data(), active_count}, attr)
	           : attr;
}

bool PixelParameterIsFlat(const EmitterState& state, uint32_t attr) {
	const auto* ps = state.pixel_input_info;
	return state.stage == ShaderType::Pixel && ps != nullptr &&
	       ShaderPixelParameterIsFlat(*ps, attr);
}

void SetError(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
}

void CollectRegister(std::vector<RegisterBinding>& registers, IR::Register reg) {
	if (std::any_of(registers.begin(), registers.end(),
	                [reg](const RegisterBinding& binding) { return binding.reg == reg; })) {
		return;
	}
	registers.push_back({reg, 0});
}

IR::Operand MakeRegisterOperand(IR::RegisterFile file, uint32_t index) {
	IR::Operand operand;
	operand.kind = IR::OperandKind::Register;
	operand.reg  = {file, index};
	return operand;
}

IR::Register SccRegister() {
	return {IR::RegisterFile::Scc, static_cast<uint32_t>(Decoder::OperandKind::Scc)};
}

IR::Operand SccOperand() {
	return MakeRegisterOperand(IR::RegisterFile::Scc,
	                           static_cast<uint32_t>(Decoder::OperandKind::Scc));
}

bool IsInactiveWave32ExecHigh(const EmitterState& state, IR::Register reg) {
	return state.wave_size == 32u && reg.file == IR::RegisterFile::Exec && reg.index == 1u;
}

bool IsMaskRegisterFile(IR::RegisterFile file) {
	return file == IR::RegisterFile::Exec || file == IR::RegisterFile::Vcc;
}

bool IsSccOperand(const IR::Operand& operand) {
	return operand.kind == IR::OperandKind::Register && operand.reg.file == IR::RegisterFile::Scc;
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

bool ProgramNeedsComputeDerivatives(const IR::Program& program) {
	if (program.values == nullptr) {
		return false;
	}
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == IR::ValueOpcode::ImageQueryLod) {
				return true;
			}
		}
	}
	return false;
}

bool ProgramNeedsImageGatherExtended(const IR::Program& program) {
	if (program.values == nullptr) {
		return false;
	}
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == IR::ValueOpcode::ImageGatherRaw) {
				return true;
			}
		}
	}
	return false;
}

bool ProgramNeedsFunctionLds(const IR::Program& program) {
	if (program.stage == ShaderType::Compute || program.values == nullptr) {
		return false;
	}

	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::SwizzleU32: return true;
				case IR::ValueOpcode::LoadSharedU8:
				case IR::ValueOpcode::LoadSharedU16:
				case IR::ValueOpcode::LoadSharedU32:
				case IR::ValueOpcode::WriteSharedU8:
				case IR::ValueOpcode::WriteSharedU16:
				case IR::ValueOpcode::WriteSharedU32:
				case IR::ValueOpcode::SharedAtomicFMin32:
				case IR::ValueOpcode::SharedAtomicFMax32:
				case IR::ValueOpcode::SharedAtomicSwap32:
				case IR::ValueOpcode::SharedAtomicIAdd32:
				case IR::ValueOpcode::SharedAtomicISub32:
				case IR::ValueOpcode::SharedAtomicSMin32:
				case IR::ValueOpcode::SharedAtomicUMin32:
				case IR::ValueOpcode::SharedAtomicSMax32:
				case IR::ValueOpcode::SharedAtomicUMax32:
				case IR::ValueOpcode::SharedAtomicAnd32:
				case IR::ValueOpcode::SharedAtomicOr32:
				case IR::ValueOpcode::SharedAtomicXor32:
				case IR::ValueOpcode::DataAppend:
				case IR::ValueOpcode::DataConsume: {
					const auto index = inst.Flags<IR::MemoryFlags>().index;
					if (index < program.values->memory_info.size() &&
					    program.values->memory_info[index].kind == IR::ResourceKind::Lds) {
						return true;
					}
					break;
				}
				default: break;
			}
		}
	}
	return false;
}

bool ProgramNeedsPixelValidMask(const IR::Program& program) {
	if (program.stage != ShaderType::Pixel || program.values == nullptr) {
		return false;
	}

	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != IR::ValueOpcode::SetAttribute) {
				continue;
			}
			const auto index = inst.Flags<IR::ExportFlags>().index;
			if (index < program.values->export_info.size() &&
			    program.values->export_info[index].vm) {
				return true;
			}
		}
	}
	return false;
}

bool ProgramNeedsSubgroupBallot(const IR::Program& program) {
	if (program.values == nullptr) {
		return false;
	}
	if (program.lane_mask_mode == ShaderLaneMaskMode::PerInvocation) {
		for (const auto* block: program.values->blocks) {
			for (const auto& inst: *block) {
				if (inst.GetOpcode() == IR::ValueOpcode::WqmMask) {
					return true;
				}
			}
		}
	}
	return ProgramRequiresExactSubgroupSize(program);
}

bool ProgramNeedsSubgroupShuffle(const IR::Program& program) {
	if (program.values == nullptr) {
		return false;
	}
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::DppMoveU32:
				case IR::ValueOpcode::ReadFirstLane:
				case IR::ValueOpcode::ReadLane:
				case IR::ValueOpcode::Permlane16U32:
				case IR::ValueOpcode::DataAppend:
				case IR::ValueOpcode::DataConsume:
				case IR::ValueOpcode::GdsDataAppend:
				case IR::ValueOpcode::GdsDataConsume:
				case IR::ValueOpcode::SwizzleU32: return true;
				default: break;
			}
		}
	}
	return false;
}

bool ProgramNeedsSubgroupLocalInvocationId(const IR::Program& program) {
	if (program.values == nullptr) {
		return false;
	}
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::DppMoveU32:
				case IR::ValueOpcode::DppUpdateU32:
				case IR::ValueOpcode::WqmMask:
				case IR::ValueOpcode::LaneId:
				case IR::ValueOpcode::WriteLane:
				case IR::ValueOpcode::Permlane16U32:
				case IR::ValueOpcode::DataAppend:
				case IR::ValueOpcode::DataConsume:
				case IR::ValueOpcode::GdsDataAppend:
				case IR::ValueOpcode::GdsDataConsume:
				case IR::ValueOpcode::SwizzleU32: return true;
				default: break;
			}
		}
	}
	return false;
}

uint32_t PointerForRegister(const EmitterState& state, IR::Register reg) {
	for (const auto& binding: state.registers) {
		if (binding.reg == reg) {
			return binding.pointer_id;
		}
	}
	return 0;
}

uint32_t          ConstantU32(EmitterState& state, uint32_t value);
void              EmitStoreU32(EmitterState& state, const IR::Operand& dst, uint32_t value);
uint32_t          EmitSubgroupLocalInvocationId(EmitterState& state);
uint32_t          EmitLaneIndexActiveBool(EmitterState& state, uint32_t lane);
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

ImageViewKind StorageImageViewKind(const EmitterState& state, const IR::MemoryInfo& mem,
                                   bool uint_image, uint32_t use_pc) {
	(void)state;
	(void)uint_image;
	(void)use_pc;
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

uint32_t ImageViewImageType(const EmitterState& state, ImageViewKind view, bool integer) {
	return state.sampled_images[SampledImageIndex(integer, view)].image_type;
}

uint32_t ImageViewSampledImageType(const EmitterState& state, ImageViewKind view, bool integer) {
	return state.sampled_images[SampledImageIndex(integer, view)].sampled_image_type;
}

uint32_t ImageViewSizeType(const EmitterState& state, ImageViewKind view) {
	switch (ImageViewCoordinateComponents(view)) {
		case 1u: return state.uint_type;
		case 2u: return state.vec2_uint_type;
		case 3u: return state.vec3_uint_type;
		default: return 0;
	}
}

uint32_t StorageImageType(const EmitterState& state, bool uint_image, ImageViewKind view) {
	return state.storage_images[StorageImageIndex(uint_image, view)].image_type;
}

uint32_t StorageImagePointerType(const EmitterState& state, bool uint_image, ImageViewKind view) {
	return state.storage_images[StorageImageIndex(uint_image, view)].pointer_type;
}

uint32_t StorageImageVariable(const EmitterState& state, bool uint_image, ImageViewKind view) {
	return state.storage_images[StorageImageIndex(uint_image, view)].variable;
}

uint32_t LoadSampledImageDescriptor(EmitterState& state, const IR::MemoryInfo& mem, uint32_t use_pc,
                                    ImageViewKind view) {
	(void)use_pc;
	const bool  integer     = mem.kind == IR::ResourceKind::ImageUint;
	const auto  kind        = SampledBindingKind(integer, view);
	const auto  binding     = ResourceForDescriptor(state, kind, mem.resource);
	const auto& descriptors = state.sampled_images[SampledImageIndex(integer, view)];
	const auto  pointer     = DescriptorElementPointer(
	    state, descriptors.pointer_type, descriptors.variable, binding.array_index, kind,
	    mem.resource, "sampled image descriptor array was not emitted");
	const auto image = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, ImageViewImageType(state, view, integer), image, pointer});
	return image;
}

uint32_t LoadSamplerDescriptor(EmitterState& state, uint32_t sampler, uint32_t use_pc) {
	(void)use_pc;
	const auto binding = ResourceForDescriptor(state, IR::DescriptorBindingKind::Samplers, sampler);
	const auto pointer = DescriptorElementPointer(
	    state, state.ptr_uniform_sampler, state.sampler_variable, binding.array_index,
	    IR::DescriptorBindingKind::Samplers, sampler, "sampler descriptor array was not emitted");
	const auto sampler_id = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, state.sampler_type, sampler_id, pointer});
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

uint32_t StorageImageDescriptorPointer(EmitterState& state, uint32_t resource, bool uint_image,
                                       uint32_t use_pc, ImageViewKind view) {
	(void)use_pc;
	const auto kind     = StorageBindingKind(uint_image, view);
	const auto binding  = ResourceForDescriptor(state, kind, resource);
	const auto ptr_type = StorageImagePointerType(state, uint_image, view);
	const auto variable = StorageImageVariable(state, uint_image, view);
	return DescriptorElementPointer(state, ptr_type, variable, binding.array_index, kind, resource,
	                                "storage image descriptor array was not emitted");
}

uint32_t LoadStorageImageDescriptor(EmitterState& state, uint32_t resource, bool uint_image,
                                    uint32_t use_pc, ImageViewKind view) {
	const auto pointer = StorageImageDescriptorPointer(state, resource, uint_image, use_pc, view);
	if (pointer == 0) {
		ExitDescriptorBindingFailure(state, StorageBindingKind(uint_image, view), resource,
		                             "storage image descriptor pointer creation failed");
	}
	const auto type  = StorageImageType(state, uint_image, view);
	const auto image = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, type, image, pointer});
	return image;
}

void EmitStorageImageWrite(EmitterState& state, uint32_t resource, bool uint_image,
                           ImageViewKind view, uint32_t mip_lod, uint32_t coord, uint32_t texel) {
	const auto  kind    = StorageBindingKind(uint_image, view);
	const auto  binding = ResourceForDescriptor(state, kind, resource);
	const auto& image   = state.program.info.images.at(resource);
	const auto  LoadAt  = [&](uint32_t array_index) {
		const auto pointer = DescriptorElementPointer(
		    state, StorageImagePointerType(state, uint_image, view),
		    StorageImageVariable(state, uint_image, view), array_index, kind, resource,
		    "storage image descriptor array was not emitted");
		const auto descriptor = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpLoad, StorageImageType(state, uint_image, view), descriptor, pointer});
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
		state.builder.AddFunction({OpLabel, labels[mip]});
		state.builder.AddFunction({OpImageWrite, LoadAt(binding.array_index + mip), coord, texel});
		state.builder.AddFunction({OpBranch, merge_label});
	}
	state.builder.AddFunction({OpLabel, merge_label});
}

uint32_t ExecutionModelForStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return ExecutionModelVertex;
		case ShaderType::Pixel: return ExecutionModelFragment;
		default: return ExecutionModelGLCompute;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
