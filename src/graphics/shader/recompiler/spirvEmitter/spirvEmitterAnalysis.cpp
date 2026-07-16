#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t PixelParameterMappedLocation(const EmitterState& state, uint32_t attr) {
	const auto* ps = state.pixel_input_info;
	if (state.stage != ShaderType::Pixel || ps == nullptr || attr >= ps->input_num) {
		return attr;
	}
	// VINTRP ATTR selects the PS input slot. SPI_PS_INPUT_CNTL maps that slot to a
	// VS parameter export, which is the SPIR-V location we must link against.
	return ps->interpolator_settings[attr] & PsInputOffsetMask;
}

uint32_t PixelParameterLocation(const EmitterState& state, uint32_t attr) {
	bool used_locations[32] = {};

	for (const auto& input: state.inputs) {
		if (input.kind != IR::StageInputKind::Parameter) {
			continue;
		}

		auto location = PixelParameterMappedLocation(state, input.location);
		if (location < std::size(used_locations) && used_locations[location]) {
			auto fallback_location = input.location;
			while (fallback_location < std::size(used_locations) &&
			       used_locations[fallback_location]) {
				fallback_location++;
			}
			EXIT_NOT_IMPLEMENTED(fallback_location >= std::size(used_locations));
			location = fallback_location;
		}

		if (input.location == attr) {
			return location;
		}

		if (location < std::size(used_locations)) {
			used_locations[location] = true;
		}
	}

	return PixelParameterMappedLocation(state, attr);
}

bool PixelParameterIsFlat(const EmitterState& state, uint32_t attr) {
	const auto* ps = state.pixel_input_info;
	if (state.stage != ShaderType::Pixel || ps == nullptr || attr >= ps->input_num) {
		return false;
	}
	return (ps->interpolator_settings[attr] & PsInputFlatShade) != 0;
}

void SetError(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
}

void CollectRegister(std::vector<RegisterBinding>* registers, IR::Register reg) {
	if (std::any_of(registers->begin(), registers->end(),
	                [reg](const RegisterBinding& binding) { return binding.reg == reg; })) {
		return;
	}
	registers->push_back({reg, 0});
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

bool IsCompareOpcode(IR::Opcode op);

void CollectMaskStateRegisters(std::vector<RegisterBinding>* registers) {
	CollectRegister(registers, {IR::RegisterFile::Exec, 0});
	CollectRegister(registers, {IR::RegisterFile::Exec, 1});
	CollectRegister(registers, {IR::RegisterFile::Vcc, 0});
	CollectRegister(registers, {IR::RegisterFile::Vcc, 1});
	CollectRegister(registers, SccRegister());
}

void CollectSequentialRegisters(std::vector<RegisterBinding>* registers, const IR::Operand& base,
                                uint32_t count) {
	if (base.kind != IR::OperandKind::Register) {
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		auto reg = base.reg;
		reg.index += i;
		CollectRegister(registers, reg);
	}
}

uint32_t MaxCollectedVectorRegisterEnd(const std::vector<RegisterBinding>& registers) {
	uint32_t max_end = 0;
	for (const auto& binding: registers) {
		if (binding.reg.file == IR::RegisterFile::Vector) {
			max_end = std::max(max_end, binding.reg.index + 1u);
		}
	}
	return std::min(max_end, 256u);
}

void CollectMoveRelSourceRegisters(const IR::Program&            program,
                                   std::vector<RegisterBinding>* registers) {
	const auto max_vector_end = MaxCollectedVectorRegisterEnd(*registers);
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op != IR::Opcode::MoveRelSourceU32 ||
			    inst.src[0].kind != IR::OperandKind::Register ||
			    inst.src[0].reg.file != IR::RegisterFile::Vector) {
				continue;
			}
			const auto base_index = inst.src[0].reg.index;
			const auto limit      = std::min(std::max(max_vector_end, base_index + 1u), 256u);
			CollectSequentialRegisters(registers, inst.src[0], limit - base_index);
		}
	}
}

bool IsPairDwordOpcode(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::MoveU64:
		case IR::Opcode::WqmB64:
		case IR::Opcode::SaveexecB64:
		case IR::Opcode::BitwiseAndU64:
		case IR::Opcode::BitwiseAndNotU64:
		case IR::Opcode::BitwiseOrU64:
		case IR::Opcode::BitwiseOrNotU64:
		case IR::Opcode::BitwiseXorU64:
		case IR::Opcode::BitwiseNandU64:
		case IR::Opcode::BitwiseNorU64:
		case IR::Opcode::BitwiseXnorU64:
		case IR::Opcode::BitwiseNotU64:
		case IR::Opcode::BitFieldMaskU64:
		case IR::Opcode::BitFieldExtractU64:
		case IR::Opcode::BitReplicateB64B32:
		case IR::Opcode::ShiftLeftLogicalU64:
		case IR::Opcode::ShiftRightLogicalU64:
		case IR::Opcode::SelectU64: return true;
		default: return false;
	}
}

uint32_t PairDwordSourceCount(IR::Opcode op, uint32_t src_count) {
	switch (op) {
		case IR::Opcode::BitFieldMaskU64: return 0;
		case IR::Opcode::BitReplicateB64B32: return 0;
		case IR::Opcode::BitFieldExtractU64: return 1;
		case IR::Opcode::ShiftLeftLogicalU64:
		case IR::Opcode::ShiftRightLogicalU64:
		case IR::Opcode::BitwiseNotU64:
		case IR::Opcode::SaveexecB64:
		case IR::Opcode::WqmB64:
		case IR::Opcode::MoveU64: return std::min<uint32_t>(src_count, 1u);
		default: return src_count;
	}
}

void CollectRegisters(const IR::Program& program, std::vector<RegisterBinding>* registers) {
	CollectMaskStateRegisters(registers);
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.dst.kind == IR::OperandKind::Register) {
				CollectRegister(registers, inst.dst.reg);
			}
			if (inst.dst2.kind == IR::OperandKind::Register) {
				CollectRegister(registers, inst.dst2.reg);
			}
			for (uint32_t i = 0; i < inst.src_count; i++) {
				if (inst.src[i].kind == IR::OperandKind::Register) {
					CollectRegister(registers, inst.src[i].reg);
				}
			}
			if (IsPairDwordOpcode(inst.op)) {
				CollectSequentialRegisters(registers, inst.dst, 2);
				const uint32_t first_pair_src = inst.op == IR::Opcode::SelectU64 ? 1u : 0u;
				const uint32_t pair_src_count = PairDwordSourceCount(inst.op, inst.src_count);
				for (uint32_t i = first_pair_src; i < pair_src_count; i++) {
					CollectSequentialRegisters(registers, inst.src[i], 2);
				}
			}
			if (inst.op == IR::Opcode::BitCountU64 || inst.op == IR::Opcode::FindMsbFromHighU64) {
				CollectSequentialRegisters(registers, inst.src[0], 2);
			}
			if (inst.op == IR::Opcode::CompareNeU64) {
				CollectSequentialRegisters(registers, inst.src[0], 2);
				CollectSequentialRegisters(registers, inst.src[1], 2);
			}
			if (IsCompareOpcode(inst.op) && inst.dst.kind == IR::OperandKind::Register &&
			    inst.dst.reg.file != IR::RegisterFile::Scc) {
				CollectSequentialRegisters(registers, inst.dst, 2);
			}
			if (inst.op == IR::Opcode::IAddCarryU32 || inst.op == IR::Opcode::ISubBorrowU32) {
				CollectSequentialRegisters(registers, inst.dst2, 2);
			}
			if (inst.op == IR::Opcode::UMadU64U32) {
				CollectSequentialRegisters(registers, inst.dst, 2);
				CollectSequentialRegisters(registers, inst.dst2, 2);
				CollectSequentialRegisters(registers, inst.src[2], 2);
			}
			if (inst.op == IR::Opcode::SBufferLoadDword &&
			    inst.memory.kind == IR::ResourceKind::ScalarBuffer) {
				CollectRegister(registers, {IR::RegisterFile::Scalar, inst.memory.resource * 4u});
			}
			if ((inst.op == IR::Opcode::ImageSample || inst.op == IR::Opcode::ImageGather4) &&
			    inst.memory.image_address_components > 1u) {
				CollectSequentialRegisters(registers, inst.dst, inst.memory.data_dwords);
				CollectSequentialRegisters(registers, inst.src[0],
				                           inst.memory.image_address_components);
			}
			if (inst.op == IR::Opcode::ImageGetLod) {
				CollectSequentialRegisters(registers, inst.dst, inst.memory.data_dwords);
				CollectSequentialRegisters(registers, inst.src[0],
				                           inst.memory.image_address_components);
			}
			if (inst.op == IR::Opcode::ImageLoad) {
				CollectSequentialRegisters(registers, inst.dst, inst.memory.data_dwords);
				CollectSequentialRegisters(registers, inst.src[0],
				                           inst.memory.image_address_components);
			}
			if (inst.op == IR::Opcode::ImageStore) {
				CollectSequentialRegisters(registers, inst.src[0], inst.memory.data_dwords);
				CollectSequentialRegisters(registers, inst.src[1],
				                           inst.memory.image_address_components);
			}
			if (inst.memory.kind == IR::ResourceKind::StorageImageUint) {
				CollectSequentialRegisters(registers, inst.src[1],
				                           inst.memory.image_address_components);
			}
		}
	}
	CollectMoveRelSourceRegisters(program, registers);
}

bool HasOutput(const std::vector<OutputBinding>& outputs, IR::StageOutputKind kind,
               uint32_t index) {
	return std::any_of(outputs.begin(), outputs.end(), [kind, index](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
}

void CopyProgramInputsAndOutputs(EmitterState* state, const IR::Program& program) {
	for (const auto& input: program.info.inputs) {
		state->inputs.push_back(
		    {input.kind, input.location, input.component_count, 0, input.debug_name});
	}
	for (const auto& output: program.info.outputs) {
		if (HasOutput(state->outputs, output.kind, output.index)) {
			continue;
		}
		state->outputs.push_back(
		    {output.kind, output.index, output.location, 0, output.debug_name});
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
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == IR::Opcode::ImageGetLod) {
				return true;
			}
		}
	}
	return false;
}

bool ProgramNeedsImageGatherExtended(const IR::Program& program) {
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == IR::Opcode::ImageGather4) {
				return true;
			}
		}
	}
	return false;
}

bool IsLdsOpcode(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::DsReadUbyte:
		case IR::Opcode::DsReadSbyte:
		case IR::Opcode::DsReadUshort:
		case IR::Opcode::DsReadSshort:
		case IR::Opcode::DsReadB32:
		case IR::Opcode::DsWriteByte:
		case IR::Opcode::DsWriteShort:
		case IR::Opcode::DsWriteB32:
		case IR::Opcode::DsMinF32:
		case IR::Opcode::DsMaxF32:
		case IR::Opcode::DsSwizzleB32:
		case IR::Opcode::DsWriteAddtidB32:
		case IR::Opcode::DsReadAddtidB32: return true;
		default: return false;
	}
}

bool ProgramNeedsFunctionLds(const IR::Program& program) {
	if (program.stage == ShaderType::Compute) {
		return false;
	}

	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (IsLdsOpcode(inst.op)) {
				return true;
			}
		}
	}
	return false;
}

bool ProgramNeedsPixelValidMask(const IR::Program& program) {
	if (program.stage != ShaderType::Pixel) {
		return false;
	}

	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == IR::Opcode::Export && inst.export_info.vm) {
				return true;
			}
		}
	}
	return false;
}

bool InstructionHasDppSource(const IR::Instruction& inst) {
	for (uint32_t i = 0; i < inst.src_count && i < 3u; i++) {
		if (inst.src[i].dpp) {
			return true;
		}
	}
	return false;
}

bool ProgramNeedsSubgroupBallot(const IR::Program& program) {
	if (program.lane_mask_mode == ShaderLaneMaskMode::PerInvocation) {
		for (const auto& block: program.blocks) {
			for (const auto& inst: block.instructions) {
				if (inst.op == IR::Opcode::WqmB64) {
					return true;
				}
			}
		}
	}
	return ProgramRequiresExactSubgroupSize(program);
}

bool ProgramNeedsSubgroupShuffle(const IR::Program& program) {
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == IR::Opcode::ReadFirstLaneU32 || inst.op == IR::Opcode::ReadLaneU32 ||
			    inst.op == IR::Opcode::Permlane16B32 || inst.op == IR::Opcode::Permlanex16B32 ||
			    inst.op == IR::Opcode::DsSwizzleB32 || inst.op == IR::Opcode::DsConsume ||
			    inst.op == IR::Opcode::DsAppend || InstructionHasDppSource(inst)) {
				return true;
			}
		}
	}
	return false;
}

bool IsCompareOpcode(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::CompareFalse:
		case IR::Opcode::CompareTrue:
		case IR::Opcode::CompareEqU32:
		case IR::Opcode::CompareNeU32:
		case IR::Opcode::CompareGtU32:
		case IR::Opcode::CompareGeU32:
		case IR::Opcode::CompareLtU32:
		case IR::Opcode::CompareLeU32:
		case IR::Opcode::CompareNeU64:
		case IR::Opcode::CompareMaskEqU32:
		case IR::Opcode::CompareMaskNeU32:
		case IR::Opcode::CompareMaskGtU32:
		case IR::Opcode::CompareMaskGeU32:
		case IR::Opcode::CompareMaskLtU32:
		case IR::Opcode::CompareMaskLeU32:
		case IR::Opcode::CompareEqI32:
		case IR::Opcode::CompareNeI32:
		case IR::Opcode::CompareGtI32:
		case IR::Opcode::CompareGeI32:
		case IR::Opcode::CompareLtI32:
		case IR::Opcode::CompareLeI32:
		case IR::Opcode::CompareEqI16:
		case IR::Opcode::CompareNeI16:
		case IR::Opcode::CompareGtI16:
		case IR::Opcode::CompareGeI16:
		case IR::Opcode::CompareLtI16:
		case IR::Opcode::CompareLeI16:
		case IR::Opcode::CompareMaskEqI32:
		case IR::Opcode::CompareMaskNeI32:
		case IR::Opcode::CompareMaskGtI32:
		case IR::Opcode::CompareMaskGeI32:
		case IR::Opcode::CompareMaskLtI32:
		case IR::Opcode::CompareMaskLeI32:
		case IR::Opcode::CompareEqU16:
		case IR::Opcode::CompareNeU16:
		case IR::Opcode::CompareGtU16:
		case IR::Opcode::CompareGeU16:
		case IR::Opcode::CompareLtU16:
		case IR::Opcode::CompareLeU16:
		case IR::Opcode::CompareEqF32:
		case IR::Opcode::CompareNeF32:
		case IR::Opcode::CompareGtF32:
		case IR::Opcode::CompareGeF32:
		case IR::Opcode::CompareLtF32:
		case IR::Opcode::CompareLeF32:
		case IR::Opcode::CompareOrderedF32:
		case IR::Opcode::CompareUnorderedF32:
		case IR::Opcode::CompareUnordEqF32:
		case IR::Opcode::CompareUnordNeF32:
		case IR::Opcode::CompareUnordGtF32:
		case IR::Opcode::CompareUnordGeF32:
		case IR::Opcode::CompareUnordLtF32:
		case IR::Opcode::CompareUnordLeF32:
		case IR::Opcode::CompareClassF32:
		case IR::Opcode::CompareEqF16:
		case IR::Opcode::CompareNeF16:
		case IR::Opcode::CompareGtF16:
		case IR::Opcode::CompareGeF16:
		case IR::Opcode::CompareLtF16:
		case IR::Opcode::CompareLeF16:
		case IR::Opcode::CompareUnordNeF16:
		case IR::Opcode::CompareMaskEqF16:
		case IR::Opcode::CompareMaskNeF16:
		case IR::Opcode::CompareMaskGtF16:
		case IR::Opcode::CompareMaskGeF16:
		case IR::Opcode::CompareMaskLtF16:
		case IR::Opcode::CompareMaskLeF16:
		case IR::Opcode::CompareMaskUnordNeF16:
		case IR::Opcode::CompareMaskUnordGeF16:
		case IR::Opcode::CompareMaskEqF32:
		case IR::Opcode::CompareMaskNeF32:
		case IR::Opcode::CompareMaskGtF32:
		case IR::Opcode::CompareMaskGeF32:
		case IR::Opcode::CompareMaskLtF32:
		case IR::Opcode::CompareMaskLeF32:
		case IR::Opcode::CompareMaskUnordEqF32:
		case IR::Opcode::CompareMaskUnordNeF32:
		case IR::Opcode::CompareMaskUnordGtF32:
		case IR::Opcode::CompareMaskUnordGeF32:
		case IR::Opcode::CompareMaskUnordLtF32:
		case IR::Opcode::CompareMaskUnordLeF32: return true;
		default: return false;
	}
}

bool ProgramNeedsSubgroupLocalInvocationId(const IR::Program& program) {
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == IR::Opcode::WqmB64 || inst.op == IR::Opcode::DsSwizzleB32 ||
			    inst.op == IR::Opcode::DsConsume || inst.op == IR::Opcode::DsAppend ||
			    inst.op == IR::Opcode::WriteLaneU32 || inst.op == IR::Opcode::Permlane16B32 ||
			    inst.op == IR::Opcode::Permlanex16B32 || inst.op == IR::Opcode::Export ||
			    InstructionHasDppSource(inst)) {
				return true;
			}
			if (IsCompareOpcode(inst.op)) {
				return true;
			}
			if (inst.dst.kind == IR::OperandKind::Register &&
			    inst.dst.reg.file == IR::RegisterFile::Vector) {
				return true;
			}
			if (inst.dst2.kind == IR::OperandKind::Register &&
			    inst.dst2.reg.file == IR::RegisterFile::Vector) {
				return true;
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

uint32_t          ConstantU32(EmitterState* state, uint32_t value);
void              EmitStoreU32(EmitterState* state, const IR::Operand& dst, uint32_t value);
uint32_t          EmitSubgroupLocalInvocationId(EmitterState* state);
uint32_t          EmitLaneIndexActiveBool(EmitterState* state, uint32_t lane);
[[noreturn]] void ExitDescriptorBindingFailure(const EmitterState&       state,
                                               IR::DescriptorBindingKind kind, uint32_t resource,
                                               const char* reason) {
	EXIT("shader binding resolution failed during SPIR-V emit: hash=0x%016" PRIx64
	     " stage=%u resource=%" PRIu32 " binding_kind=%u reason=%s\n",
	     state.program->shader_hash, static_cast<unsigned>(state.stage), resource,
	     static_cast<unsigned>(kind), reason);
	std::abort();
}

DescriptorResourceBinding ResourceForDescriptor(const EmitterState&       state,
                                                IR::DescriptorBindingKind kind, uint32_t resource) {
	const auto* descriptor = IR::FindBinding(state.program->bindings, kind);
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

uint32_t DescriptorElementPointer(EmitterState* state, uint32_t result_ptr_type,
                                  uint32_t variable_id, uint32_t array_index,
                                  IR::DescriptorBindingKind kind, uint32_t resource,
                                  const char* variable_name) {
	if (variable_id == 0) {
		ExitDescriptorBindingFailure(*state, kind, resource, variable_name);
	}
	const auto pointer = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpAccessChain, result_ptr_type, pointer, variable_id, ConstantU32(state, array_index)});
	return pointer;
}

ImageViewKind ImageViewKindFromDimension(Decoder::ImageDimension dimension) {
	switch (dimension) {
		case Decoder::ImageDimension::Dim2DArray: return ImageViewKind::Dim2DArray;
		case Decoder::ImageDimension::Dim3D: return ImageViewKind::Dim3D;
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
		case ImageViewKind::Dim2DArray:
		case ImageViewKind::Dim3D: return 3u;
		default: return 2u;
	}
}

uint32_t ImageViewImageType(const EmitterState& state, ImageViewKind view, bool integer) {
	return state.sampled_images[SampledImageIndex(integer, view)].image_type;
}

uint32_t ImageViewSampledImageType(const EmitterState& state, ImageViewKind view, bool integer) {
	return state.sampled_images[SampledImageIndex(integer, view)].sampled_image_type;
}

uint32_t ImageViewSizeType(const EmitterState& state, ImageViewKind view) {
	return ImageViewCoordinateComponents(view) == 3u ? state.vec3_uint_type : state.vec2_uint_type;
}

uint32_t StorageImageType(const EmitterState& state, bool uint_image, ImageViewKind view) {
	if (uint_image) {
		switch (view) {
			case ImageViewKind::Dim2DArray: return state.storage_image_uint_2d_array_type;
			case ImageViewKind::Dim3D: return state.storage_image_uint_3d_type;
			default: return state.storage_image_uint_type;
		}
	}
	switch (view) {
		case ImageViewKind::Dim2DArray: return state.storage_image_2d_array_type;
		case ImageViewKind::Dim3D: return state.storage_image_3d_type;
		default: return state.storage_image_type;
	}
}

uint32_t StorageImagePointerType(const EmitterState& state, bool uint_image, ImageViewKind view) {
	if (uint_image) {
		switch (view) {
			case ImageViewKind::Dim2DArray: return state.ptr_uniform_storage_image_uint_2d_array;
			case ImageViewKind::Dim3D: return state.ptr_uniform_storage_image_uint_3d;
			default: return state.ptr_uniform_storage_image_uint;
		}
	}
	switch (view) {
		case ImageViewKind::Dim2DArray: return state.ptr_uniform_storage_image_2d_array;
		case ImageViewKind::Dim3D: return state.ptr_uniform_storage_image_3d;
		default: return state.ptr_uniform_storage_image;
	}
}

uint32_t StorageImageVariable(const EmitterState& state, bool uint_image, ImageViewKind view) {
	if (uint_image) {
		switch (view) {
			case ImageViewKind::Dim2DArray: return state.storage_image_uint_2d_array_variable;
			case ImageViewKind::Dim3D: return state.storage_image_uint_3d_variable;
			default: return state.storage_image_uint_variable;
		}
	}
	switch (view) {
		case ImageViewKind::Dim2DArray: return state.storage_image_2d_array_variable;
		case ImageViewKind::Dim3D: return state.storage_image_3d_variable;
		default: return state.storage_image_variable;
	}
}

IR::DescriptorBindingKind StorageBindingKind(bool uint_image, ImageViewKind view) {
	if (uint_image) {
		switch (view) {
			case ImageViewKind::Dim2DArray: return IR::DescriptorBindingKind::StorageUint2DArray;
			case ImageViewKind::Dim3D: return IR::DescriptorBindingKind::StorageUint3D;
			default: return IR::DescriptorBindingKind::StorageUint2D;
		}
	}
	switch (view) {
		case ImageViewKind::Dim2DArray: return IR::DescriptorBindingKind::Storage2DArray;
		case ImageViewKind::Dim3D: return IR::DescriptorBindingKind::Storage3D;
		default: return IR::DescriptorBindingKind::Storage2D;
	}
}

uint32_t LoadSampledImageDescriptor(EmitterState* state, const IR::MemoryInfo& mem, uint32_t use_pc,
                                    ImageViewKind view) {
	(void)use_pc;
	const bool  integer     = mem.kind == IR::ResourceKind::ImageUint;
	const auto  kind        = SampledBindingKind(integer, view);
	const auto  binding     = ResourceForDescriptor(*state, kind, mem.resource);
	const auto& descriptors = state->sampled_images[SampledImageIndex(integer, view)];
	const auto  pointer     = DescriptorElementPointer(
	    state, descriptors.pointer_type, descriptors.variable, binding.array_index, kind,
	    mem.resource, "sampled image descriptor array was not emitted");
	const auto image = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, ImageViewImageType(*state, view, integer), image, pointer});
	return image;
}

uint32_t LoadSamplerDescriptor(EmitterState* state, uint32_t sampler, uint32_t use_pc) {
	(void)use_pc;
	const auto binding =
	    ResourceForDescriptor(*state, IR::DescriptorBindingKind::Samplers, sampler);
	const auto pointer = DescriptorElementPointer(
	    state, state->ptr_uniform_sampler, state->sampler_variable, binding.array_index,
	    IR::DescriptorBindingKind::Samplers, sampler, "sampler descriptor array was not emitted");
	const auto sampler_id = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->sampler_type, sampler_id, pointer});
	return sampler_id;
}

uint32_t MakeSampledImage(EmitterState* state, const IR::MemoryInfo& mem, uint32_t use_pc,
                          ImageViewKind view) {
	const auto image   = LoadSampledImageDescriptor(state, mem, use_pc, view);
	const auto sampler = LoadSamplerDescriptor(state, mem.sampler, use_pc);
	if (image == 0 || sampler == 0) {
		ExitDescriptorBindingFailure(
		    *state, SampledBindingKind(mem.kind == IR::ResourceKind::ImageUint, view), mem.resource,
		    "sampled image or sampler descriptor load failed");
	}
	const auto sampled_image = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpSampledImage,
	     ImageViewSampledImageType(*state, view, mem.kind == IR::ResourceKind::ImageUint),
	     sampled_image, image, sampler});
	return sampled_image;
}

uint32_t StorageImageDescriptorPointer(EmitterState* state, uint32_t resource, bool uint_image,
                                       uint32_t use_pc, ImageViewKind view) {
	(void)use_pc;
	const auto kind     = StorageBindingKind(uint_image, view);
	const auto binding  = ResourceForDescriptor(*state, kind, resource);
	const auto ptr_type = StorageImagePointerType(*state, uint_image, view);
	const auto variable = StorageImageVariable(*state, uint_image, view);
	return DescriptorElementPointer(state, ptr_type, variable, binding.array_index, kind, resource,
	                                "storage image descriptor array was not emitted");
}

uint32_t LoadStorageImageDescriptor(EmitterState* state, uint32_t resource, bool uint_image,
                                    uint32_t use_pc, ImageViewKind view) {
	const auto pointer = StorageImageDescriptorPointer(state, resource, uint_image, use_pc, view);
	if (pointer == 0) {
		ExitDescriptorBindingFailure(*state, StorageBindingKind(uint_image, view), resource,
		                             "storage image descriptor pointer creation failed");
	}
	const auto type  = StorageImageType(*state, uint_image, view);
	const auto image = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, type, image, pointer});
	return image;
}

uint32_t ExecutionModelForStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return ExecutionModelVertex;
		case ShaderType::Pixel: return ExecutionModelFragment;
		default: return ExecutionModelGLCompute;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
