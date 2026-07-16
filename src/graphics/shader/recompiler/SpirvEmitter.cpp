#include "graphics/shader/recompiler/SpirvEmitter.h"

#include "graphics/shader/recompiler/SrtWalker.h"
#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

bool Fail(std::string* error, const std::string& message) {
	if (error != nullptr) {
		*error = message;
	}
	return false;
}

bool ImageBinding(const IR::ImageResource& image, IR::DescriptorBindingKind& kind) {
	using Kind = IR::DescriptorBindingKind;
	using Dim  = Decoder::ImageDimension;
	if (image.kind == IR::ResourceKind::Image || image.kind == IR::ResourceKind::ImageUint) {
		const bool integer = image.kind == IR::ResourceKind::ImageUint;
		switch (image.dimension) {
			case Dim::Dim2D: kind = integer ? Kind::SampledUint2D : Kind::Sampled2D; return true;
			case Dim::Dim3D: kind = integer ? Kind::SampledUint3D : Kind::Sampled3D; return true;
			case Dim::Dim2DArray:
				kind = integer ? Kind::SampledUint2DArray : Kind::Sampled2DArray;
				return true;
			case Dim::Unknown: return false;
		}
	}
	const bool uint_image = image.kind == IR::ResourceKind::StorageImageUint;
	if (image.kind != IR::ResourceKind::StorageImage && !uint_image) {
		return false;
	}
	switch (image.dimension) {
		case Dim::Dim2D: kind = uint_image ? Kind::StorageUint2D : Kind::Storage2D; return true;
		case Dim::Dim3D: kind = uint_image ? Kind::StorageUint3D : Kind::Storage3D; return true;
		case Dim::Dim2DArray:
			kind = uint_image ? Kind::StorageUint2DArray : Kind::Storage2DArray;
			return true;
		case Dim::Unknown: return false;
	}
	return false;
}

bool NeedsSampler(IR::Opcode op) {
	return op == IR::Opcode::ImageSample || op == IR::Opcode::ImageGather4 ||
	       op == IR::Opcode::ImageGetLod;
}

bool IsBufferLoad(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::BufferLoadUbyte:
		case IR::Opcode::BufferLoadSbyte:
		case IR::Opcode::BufferLoadUshort:
		case IR::Opcode::BufferLoadSshort:
		case IR::Opcode::BufferLoadDword: return true;
		default: return false;
	}
}

bool IsBufferStore(IR::Opcode op) {
	return op == IR::Opcode::BufferStoreByte || op == IR::Opcode::BufferStoreShort ||
	       op == IR::Opcode::BufferStoreDword;
}

bool IsFlatLoad(IR::Opcode op) {
	return op == IR::Opcode::FlatLoadUbyte || op == IR::Opcode::FlatLoadSbyte ||
	       op == IR::Opcode::FlatLoadUshort || op == IR::Opcode::FlatLoadSshort ||
	       op == IR::Opcode::FlatLoadDword;
}

bool IsFlatStore(IR::Opcode op) {
	return op == IR::Opcode::FlatStoreByte || op == IR::Opcode::FlatStoreShort ||
	       op == IR::Opcode::FlatStoreDword;
}

bool IsDsKind(IR::ResourceKind kind) {
	return kind == IR::ResourceKind::Lds || kind == IR::ResourceKind::Gds;
}

bool IsDsRead(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::DsReadUbyte:
		case IR::Opcode::DsReadSbyte:
		case IR::Opcode::DsReadUshort:
		case IR::Opcode::DsReadSshort:
		case IR::Opcode::DsReadB32: return true;
		default: return false;
	}
}

bool IsDsWrite(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::DsWriteByte:
		case IR::Opcode::DsWriteShort:
		case IR::Opcode::DsWriteB32: return true;
		default: return false;
	}
}

bool IsAtomic(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::AtomicSwapU32:
		case IR::Opcode::AtomicAddU32:
		case IR::Opcode::AtomicSubU32:
		case IR::Opcode::AtomicSMinI32:
		case IR::Opcode::AtomicUMinU32:
		case IR::Opcode::AtomicSMaxI32:
		case IR::Opcode::AtomicUMaxU32:
		case IR::Opcode::AtomicAndU32:
		case IR::Opcode::AtomicOrU32:
		case IR::Opcode::AtomicXorU32: return true;
		default: return false;
	}
}

uint32_t BufferAddressOperandCount(const IR::Instruction& inst) {
	return 1u + (inst.memory.idxen ? 1u : 0u) + (inst.memory.offen ? 1u : 0u);
}

bool ValidateInstructionContract(const IR::Instruction& inst, std::string* error) {
	if (inst.src_count > std::size(inst.src)) {
		return Fail(error, "instruction source count exceeds fixed IR storage");
	}
	const auto kind      = inst.memory.kind;
	const auto flat_kind = kind == IR::ResourceKind::Flat || kind == IR::ResourceKind::Global ||
	                       kind == IR::ResourceKind::Scratch;
	const auto buffer_address_count = BufferAddressOperandCount(inst);
	if ((IsBufferLoad(inst.op) &&
	     (kind != IR::ResourceKind::Buffer || inst.src_count != buffer_address_count)) ||
	    (IsBufferStore(inst.op) &&
	     (kind != IR::ResourceKind::Buffer || inst.src_count != buffer_address_count + 1u)) ||
	    (inst.op == IR::Opcode::SLoadDword &&
	     (kind != IR::ResourceKind::ScalarBuffer || inst.src_count != 1)) ||
	    (inst.op == IR::Opcode::SBufferLoadDword &&
	     (kind != IR::ResourceKind::ScalarBuffer || inst.src_count != 1)) ||
	    (IsFlatLoad(inst.op) && (!flat_kind || inst.src_count != 2)) ||
	    (IsFlatStore(inst.op) && (!flat_kind || inst.src_count != 3))) {
		return Fail(error, "memory instruction opcode/kind/operand contract is invalid");
	}
	const auto ds_kind     = IsDsKind(kind);
	const auto ds_resource = !ds_kind || inst.memory.resource == 0;
	if ((IsDsRead(inst.op) && (!ds_kind || !ds_resource || inst.src_count != 1 ||
	                           inst.dst.kind != IR::OperandKind::Register)) ||
	    (IsDsWrite(inst.op) && (!ds_kind || !ds_resource || inst.src_count != 2 ||
	                            inst.dst.kind != IR::OperandKind::Null)) ||
	    ((inst.op == IR::Opcode::DsAppend || inst.op == IR::Opcode::DsConsume) &&
	     (!ds_kind || !ds_resource || inst.src_count != 1 ||
	      inst.dst.kind != IR::OperandKind::Register ||
	      (kind == IR::ResourceKind::Gds && inst.memory.offset != 0))) ||
	    ((inst.op == IR::Opcode::DsMinF32 || inst.op == IR::Opcode::DsMaxF32) &&
	     (!ds_kind || !ds_resource || inst.src_count != 3 ||
	      inst.dst.kind != IR::OperandKind::Null)) ||
	    (inst.op == IR::Opcode::DsWriteAddtidB32 &&
	     (kind != IR::ResourceKind::Lds || !ds_resource || inst.src_count != 2 ||
	      inst.dst.kind != IR::OperandKind::Null)) ||
	    (inst.op == IR::Opcode::DsReadAddtidB32 &&
	     (kind != IR::ResourceKind::Lds || !ds_resource || inst.src_count != 1 ||
	      inst.dst.kind != IR::OperandKind::Register)) ||
	    (inst.op == IR::Opcode::DsSwizzleB32 &&
	     (kind != IR::ResourceKind::None || inst.src_count != 2 ||
	      inst.dst.kind != IR::OperandKind::Register))) {
		return Fail(error, "DS instruction has an invalid resource/operand contract");
	}
	if ((inst.op == IR::Opcode::ImageSample || inst.op == IR::Opcode::ImageGather4 ||
	     inst.op == IR::Opcode::ImageGetLod || inst.op == IR::Opcode::ImageLoad ||
	     inst.op == IR::Opcode::ImageGetResinfo) &&
	    ((kind != IR::ResourceKind::Image && kind != IR::ResourceKind::ImageUint) ||
	     inst.src_count != 1)) {
		return Fail(error, "sampled image instruction has an invalid resource class");
	}
	if (inst.op == IR::Opcode::ImageStore &&
	    ((kind != IR::ResourceKind::StorageImage && kind != IR::ResourceKind::StorageImageUint) ||
	     inst.src_count != 2)) {
		return Fail(error, "storage image instruction has an invalid resource class");
	}
	if (IsAtomic(inst.op) &&
	    !((kind == IR::ResourceKind::Buffer && inst.src_count == buffer_address_count + 1u) ||
	      ((kind == IR::ResourceKind::Lds || kind == IR::ResourceKind::Gds) &&
	       inst.memory.resource == 0 && inst.src_count == 2 &&
	       (inst.dst.kind == IR::OperandKind::Null ||
	        inst.dst.kind == IR::OperandKind::Register)) ||
	      (kind == IR::ResourceKind::StorageImageUint && inst.src_count == 2))) {
		return Fail(error, "atomic instruction has an invalid resource/operand contract");
	}
	return true;
}

bool ValidateNativeProgram(const IR::Program& program, std::string* error) {
	using Kind                                             = IR::DescriptorBindingKind;
	constexpr auto                               KindCount = static_cast<size_t>(Kind::Count);
	std::array<std::vector<uint32_t>, KindCount> expected;
	std::array<bool, KindCount>                  present {};
	const auto                                   Dense = [](size_t size) {
		std::vector<uint32_t> values(size);
		for (uint32_t i = 0; i < values.size(); i++) {
			values[i] = i;
		}
		return values;
	};
	auto Expect = [&](Kind kind, std::vector<uint32_t> resources = {}) {
		const auto index = static_cast<size_t>(kind);
		present[index]   = true;
		expected[index]  = std::move(resources);
	};
	if (!program.info.buffers.empty()) {
		Expect(Kind::Buffers, Dense(program.info.buffers.size()));
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		Kind kind;
		if (!ImageBinding(program.info.images[i], kind)) {
			return Fail(error, "native shader plan has an invalid image class");
		}
		present[static_cast<size_t>(kind)] = true;
		expected[static_cast<size_t>(kind)].push_back(i);
	}
	if (!program.info.samplers.empty()) {
		Expect(Kind::Samplers, Dense(program.info.samplers.size()));
	}
	const auto uses_gds =
	    std::any_of(program.blocks.begin(), program.blocks.end(), [](const auto& block) {
		    return std::any_of(
		        block.instructions.begin(), block.instructions.end(),
		        [](const auto& inst) { return inst.memory.kind == IR::ResourceKind::Gds; });
	    });
	if (uses_gds) {
		Expect(Kind::Gds);
	}
	if (!program.info.addresses.empty()) {
		Expect(Kind::AddressMemory, Dense(program.info.addresses.size()));
	}
	if (!program.srt.reads.empty()) {
		Expect(Kind::FlattenedSrt);
	}
	if (!program.bindings.user_data_registers.empty() && program.bindings.push_constant_size == 0) {
		Expect(Kind::UserData);
	}

	std::array<bool, KindCount> seen {};
	for (uint32_t i = 0; i < program.bindings.descriptors.size(); i++) {
		const auto& binding = program.bindings.descriptors[i];
		const auto  kind    = static_cast<size_t>(binding.kind);
		if (kind >= KindCount || seen[kind] || !present[kind] ||
		    binding.resources != expected[kind]) {
			return Fail(error, "native descriptor groups do not match shader topology");
		}
		for (uint32_t j = 0; j < i; j++) {
			if (program.bindings.descriptors[j].binding == binding.binding) {
				return Fail(error, "native descriptor binding numbers are not unique");
			}
		}
		seen[kind] = true;
	}
	for (size_t i = 0; i < KindCount; i++) {
		if (present[i] != seen[i]) {
			return Fail(error, "native shader plan is missing a required descriptor group");
		}
	}
	const auto has_user_storage = present[static_cast<size_t>(Kind::UserData)];
	if ((!has_user_storage &&
	     program.bindings.push_constant_size != program.bindings.user_data_registers.size() * 4u) ||
	    (has_user_storage && program.bindings.push_constant_size != 0) ||
	    !std::is_sorted(program.bindings.user_data_registers.begin(),
	                    program.bindings.user_data_registers.end()) ||
	    std::adjacent_find(program.bindings.user_data_registers.begin(),
	                       program.bindings.user_data_registers.end()) !=
	        program.bindings.user_data_registers.end()) {
		return Fail(error, "native user-data layout is inconsistent");
	}

	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (!ValidateInstructionContract(inst, error)) {
				return false;
			}
			const auto memory = inst.memory.kind;
			if ((memory == IR::ResourceKind::Buffer || (memory == IR::ResourceKind::ScalarBuffer &&
			                                            inst.op == IR::Opcode::SBufferLoadDword)) &&
			    inst.memory.resource >= program.info.buffers.size()) {
				return Fail(error, "buffer instruction has an invalid dense resource");
			}
			if ((memory == IR::ResourceKind::Image || memory == IR::ResourceKind::ImageUint ||
			     memory == IR::ResourceKind::StorageImage ||
			     memory == IR::ResourceKind::StorageImageUint) &&
			    (inst.memory.resource >= program.info.images.size() ||
			     program.info.images[inst.memory.resource].kind != memory ||
			     program.info.images[inst.memory.resource].dimension !=
			         inst.memory.image_dimension)) {
				return Fail(error, "image instruction has an invalid dense resource");
			}
			const bool address =
			    inst.op == IR::Opcode::SLoadDword || memory == IR::ResourceKind::Flat ||
			    memory == IR::ResourceKind::Global || memory == IR::ResourceKind::Scratch;
			if (address && (inst.memory.resource >= program.info.addresses.size() ||
			                program.info.addresses[inst.memory.resource].kind != memory)) {
				return Fail(error, "address instruction has an invalid dense resource");
			}
			if (NeedsSampler(inst.op) && inst.memory.sampler >= program.info.samplers.size()) {
				return Fail(error, "sample instruction has an invalid dense sampler");
			}
			if (inst.op == IR::Opcode::LoadSrtDword &&
			    (inst.src_count != 1 || inst.src[0].kind != IR::OperandKind::ImmediateU32 ||
			     inst.src[0].imm >= program.srt.reads.size())) {
				return Fail(error, "flattened SRT instruction has an invalid dense slot");
			}
		}
	}
	return true;
}

} // namespace

static constexpr size_t InitialEmitterVectorReserve = 4096;

static void ReportReserveExceeded(const IR::Program& program, const char* vector_name,
                                  size_t size) {
	if (size <= InitialEmitterVectorReserve) {
		return;
	}
	std::printf("SPIR-V emitter reserve exceeded: hash=0x%016" PRIx64
	            " stage=%u vector=%s size=%zu reserve=%zu\n",
	            program.shader_hash, static_cast<unsigned>(program.stage), vector_name, size,
	            InitialEmitterVectorReserve);
}

static bool IsExecDestination(const IR::Operand& operand) {
	return operand.kind == IR::OperandKind::Register && operand.reg.file == IR::RegisterFile::Exec;
}

static bool IsUniformZeroExecWrite(const IR::Program& program, const IR::Instruction& inst) {
	if (inst.op != IR::Opcode::BitFieldMaskU64 || !IsExecDestination(inst.dst) ||
	    inst.src_count < 1u) {
		return false;
	}
	uint32_t count = 0;
	return IR::FoldScalarConstant(program.provenance, inst.scalar_sources[0], &count) &&
	       (count & 63u) == 0u;
}

static bool ProducesLaneMask(const IR::Instruction& inst) {
	switch (inst.op) {
		case IR::Opcode::IAddCarryU32:
		case IR::Opcode::ISubBorrowU32:
		case IR::Opcode::UMadU64U32: return inst.dst2.kind != IR::OperandKind::Null;
		default: return false;
	}
}

bool ProgramRequiresExactSubgroupSize(const IR::Program& program) {
	for (const auto& block: program.blocks) {
		if (block.terminator.kind == CFG::TerminatorKind::ConditionalBranch) {
			switch (block.terminator.condition) {
				case CFG::BranchCondition::VccZero:
				case CFG::BranchCondition::VccNonZero:
				case CFG::BranchCondition::ExecZero:
				case CFG::BranchCondition::ExecNonZero: return true;
				default: break;
			}
		}
		for (const auto& inst: block.instructions) {
			if (Emitter::InstructionHasDppSource(inst)) {
				return true;
			}
			if (Emitter::IsCompareOpcode(inst.op) && !Emitter::IsSccOperand(inst.dst)) {
				return true;
			}
			if ((IsExecDestination(inst.dst) || IsExecDestination(inst.dst2)) &&
			    !IsUniformZeroExecWrite(program, inst)) {
				return true;
			}
			if (ProducesLaneMask(inst)) {
				return true;
			}
			if (inst.op == IR::Opcode::SelectMaskU32 || inst.op == IR::Opcode::SelectMaskF32Bits) {
				return true;
			}
			if (inst.op == IR::Opcode::SaveexecB32 || inst.op == IR::Opcode::SaveexecB64) {
				return true;
			}
			switch (inst.op) {
				case IR::Opcode::WqmB64:
					if (inst.dst.kind == IR::OperandKind::Register &&
					    inst.dst.reg.file != IR::RegisterFile::Scalar) {
						return true;
					}
					break;
				case IR::Opcode::MaskedBitCountLowU32:
				case IR::Opcode::MaskedBitCountHighU32:
				case IR::Opcode::ReadFirstLaneU32:
				case IR::Opcode::ReadLaneU32:
				case IR::Opcode::WriteLaneU32:
				case IR::Opcode::Permlane16B32:
				case IR::Opcode::Permlanex16B32:
				case IR::Opcode::DsSwizzleB32:
				case IR::Opcode::DsConsume:
				case IR::Opcode::DsAppend: return true;
				default: break;
			}
		}
	}
	return false;
}

bool EmitProgram(const IR::Program& program, const IR::ResourceSnapshot& resources,
                 const ShaderVertexInputInfo*  vertex_input_info,
                 const ShaderPixelInputInfo*   pixel_input_info,
                 const ShaderComputeInputInfo* compute_input_info, std::vector<uint32_t>* spirv,
                 std::string* error) {
	using namespace Emitter;

	if (spirv == nullptr) {
		SetError(error, "invalid SPIR-V output");
		return false;
	}
	if (program.stage != ShaderType::Compute && program.stage != ShaderType::Vertex &&
	    program.stage != ShaderType::Pixel) {
		SetError(error, "binary SPIR-V emitter supports compute, vertex, and pixel shaders");
		return false;
	}
	if (!program.srt_patching_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete || !program.binding_layout_complete) {
		SetError(error, "SPIR-V emitter requires a fully planned native shader program");
		return false;
	}
	if (!IR::ValidateResourceSnapshot(program, resources, error)) {
		return false;
	}
	if (!IR::ValidateResourceSpecialization(program, resources, error)) {
		return false;
	}
	if (!ValidateNativeProgram(program, error)) {
		return false;
	}

	EmitterState state;
	state.program              = &program;
	state.resources            = &resources;
	state.vertex_input_info    = vertex_input_info;
	state.pixel_input_info     = pixel_input_info;
	state.compute_input_info   = compute_input_info;
	state.stage                = program.stage;
	state.wave_size            = program.wave_size;
	state.per_invocation_masks = program.lane_mask_mode == ShaderLaneMaskMode::PerInvocation;
	state.exact_subgroup_operations =
	    !state.per_invocation_masks && ProgramRequiresExactSubgroupSize(program);
	state.registers.reserve(InitialEmitterVectorReserve);
	state.inputs.reserve(InitialEmitterVectorReserve);
	state.outputs.reserve(InitialEmitterVectorReserve);
	state.interface_variables.reserve(InitialEmitterVectorReserve);
	state.reachable_blocks.reserve(InitialEmitterVectorReserve);
	CollectRegisters(program, &state.registers);
	CopyProgramInputsAndOutputs(&state, program);
	state.needs_subgroup_ballot              = ProgramNeedsSubgroupBallot(program);
	state.needs_subgroup_shuffle             = ProgramNeedsSubgroupShuffle(program);
	state.needs_subgroup_local_invocation_id = ProgramNeedsSubgroupLocalInvocationId(program);
	state.needs_compute_derivatives          = ProgramNeedsComputeDerivatives(program);
	state.needs_image_gather_extended        = ProgramNeedsImageGatherExtended(program);
	state.needs_function_lds                 = ProgramNeedsFunctionLds(program);
	state.needs_pixel_valid_mask             = ProgramNeedsPixelValidMask(program);
	ComputeReachableBlocks(&state, program);
	AllocateInputVariables(&state);
	AllocateOutputVariables(&state);
	AllocateDescriptorVariables(&state);
	EmitHeaderAndTypes(&state);
	AllocateRegisterVariables(&state);
	AllocateBlockLabels(&state, program);
	AllocateDispatcherState(&state, program);
	EmitFunction(&state, program);

	ReportReserveExceeded(program, "registers", state.registers.size());
	ReportReserveExceeded(program, "inputs", state.inputs.size());
	ReportReserveExceeded(program, "outputs", state.outputs.size());
	ReportReserveExceeded(program, "interface_variables", state.interface_variables.size());
	ReportReserveExceeded(program, "reachable_blocks", state.reachable_blocks.size());

	auto binary = state.builder.Build();
	if (binary.empty()) {
		SetError(error, "SPIR-V builder returned an empty module");
		return false;
	}
	*spirv = std::move(binary);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
