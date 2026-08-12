#include "graphics/shader/recompiler/emitter/SpirvEmitter.h"

#include "graphics/shader/recompiler/emitter/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/SrtWalker.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

bool Fail(std::string* error, const std::string& message) {
	if (error != nullptr) {
		*error = message;
	}
	return false;
}

bool UsesBinkDiagnosticTrace(const IR::Program& program) {
	const char* const enabled = std::getenv("KYTY_BINK_TRACE_DIR");
	return enabled != nullptr && enabled[0] != '\0' &&
	       program.shader_hash == 0x0000000208a64d00ULL;
}

bool ImageBinding(const IR::ImageResource& image, IR::DescriptorBindingKind& kind) {
	using Kind = IR::DescriptorBindingKind;
	using Dim  = Decoder::ImageDimension;
	if (image.kind == IR::ResourceKind::Image || image.kind == IR::ResourceKind::ImageUint) {
		const bool integer = image.kind == IR::ResourceKind::ImageUint;
		switch (image.dimension) {
			case Dim::Dim1D: kind = integer ? Kind::SampledUint1D : Kind::Sampled1D; return true;
			case Dim::Dim1DArray:
				kind = integer ? Kind::SampledUint1DArray : Kind::Sampled1DArray;
				return true;
			case Dim::Dim2D: kind = integer ? Kind::SampledUint2D : Kind::Sampled2D; return true;
			case Dim::Dim2DMsaa:
				kind = integer ? Kind::SampledUint2DMsaa : Kind::Sampled2DMsaa;
				return true;
			case Dim::Dim3D: kind = integer ? Kind::SampledUint3D : Kind::Sampled3D; return true;
			case Dim::Dim2DArray:
				kind = integer ? Kind::SampledUint2DArray : Kind::Sampled2DArray;
				return true;
			case Dim::Dim2DMsaaArray:
				kind = integer ? Kind::SampledUint2DMsaaArray : Kind::Sampled2DMsaaArray;
				return true;
			case Dim::Unknown: return false;
		}
	}
	const bool uint_image = image.kind == IR::ResourceKind::StorageImageUint;
	if (image.kind != IR::ResourceKind::StorageImage && !uint_image) {
		return false;
	}
	switch (image.dimension) {
		case Dim::Dim1D: kind = uint_image ? Kind::StorageUint1D : Kind::Storage1D; return true;
		case Dim::Dim1DArray:
			kind = uint_image ? Kind::StorageUint1DArray : Kind::Storage1DArray;
			return true;
		case Dim::Dim2D: kind = uint_image ? Kind::StorageUint2D : Kind::Storage2D; return true;
		case Dim::Dim3D: kind = uint_image ? Kind::StorageUint3D : Kind::Storage3D; return true;
		case Dim::Dim2DArray:
			kind = uint_image ? Kind::StorageUint2DArray : Kind::Storage2DArray;
			return true;
		case Dim::Dim2DMsaa:
		case Dim::Dim2DMsaaArray: return false;
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
		case IR::Opcode::AtomicXorU32:
		case IR::Opcode::AtomicFMinF32:
		case IR::Opcode::AtomicFMaxF32: return true;
		default: return false;
	}
}

uint32_t BufferAddressOperandCount(const IR::Instruction& inst) {
	return 1u + (inst.memory.idxen ? 1u : 0u) + (inst.memory.offen ? 1u : 0u);
}

bool ValidateInstructionContract(const IR::Instruction& inst, std::string* error) {
	if (static_cast<size_t>(inst.op) >= static_cast<size_t>(IR::Opcode::Count)) {
		return Fail(error, "IR instruction opcode is out of range");
	}
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
	      inst.dst.kind != IR::OperandKind::Register)) ||
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
	if (inst.op == IR::Opcode::ImageGather4 &&
	    (inst.memory.image_dimension == Decoder::ImageDimension::Dim1D ||
	     inst.memory.image_dimension == Decoder::ImageDimension::Dim1DArray)) {
		return Fail(error, "1D image gather is not supported by SPIR-V");
	}
	if (inst.op == IR::Opcode::ImageStore &&
	    ((kind != IR::ResourceKind::StorageImage && kind != IR::ResourceKind::StorageImageUint) ||
	     inst.src_count != 2)) {
		return Fail(error, "storage image instruction has an invalid resource class");
	}
	const auto float_atomic =
	    inst.op == IR::Opcode::AtomicFMinF32 || inst.op == IR::Opcode::AtomicFMaxF32;
	if (float_atomic &&
	    (kind != IR::ResourceKind::Buffer || inst.src_count != buffer_address_count + 1u)) {
		return Fail(error, "floating-point atomic instruction must target a buffer");
	}
	if (IsAtomic(inst.op) &&
	    !((kind == IR::ResourceKind::Buffer &&
	       inst.src_count == buffer_address_count + 1u) ||
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
	const auto uses_gds = UsesBinkDiagnosticTrace(program) ||
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
	if (program.bindings.ShaderDataDwords() != 0 && program.bindings.push_constant_size == 0) {
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
	if (program.bindings.buffer_offset_dword != program.bindings.user_data_registers.size() ||
	    program.bindings.buffer_offset_count != program.info.buffers.size() ||
	    (!has_user_storage &&
	     program.bindings.push_constant_size != program.bindings.ShaderDataDwords() * 4u) ||
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
	return IR::FoldScalarConstant(program.provenance, inst.scalar_sources[0], count) &&
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
			if (IR::IsCompareOpcode(inst.op) && !Emitter::IsSccOperand(inst.dst)) {
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
				case IR::Opcode::DsReadUbyte:
				case IR::Opcode::DsReadSbyte:
				case IR::Opcode::DsReadUshort:
				case IR::Opcode::DsReadSshort:
				case IR::Opcode::DsReadB32:
				case IR::Opcode::DsReadAddtidB32:
					// LDS read visibility uses a subgroup control barrier. It is only
					// correct when one host subgroup represents one complete guest wave.
					if (program.stage == ShaderType::Compute &&
					    inst.memory.kind == IR::ResourceKind::Lds) {
						return true;
					}
					break;
				case IR::Opcode::AtomicSwapU32:
				case IR::Opcode::AtomicAddU32:
				case IR::Opcode::AtomicSubU32:
				case IR::Opcode::AtomicSMinI32:
				case IR::Opcode::AtomicUMinU32:
				case IR::Opcode::AtomicSMaxI32:
				case IR::Opcode::AtomicUMaxU32:
				case IR::Opcode::AtomicAndU32:
				case IR::Opcode::AtomicOrU32:
				case IR::Opcode::AtomicXorU32:
				case IR::Opcode::AtomicFMinF32:
				case IR::Opcode::AtomicFMaxF32:
				case IR::Opcode::DsMinF32:
				case IR::Opcode::DsMaxF32:
					// Compute LDS atomics use a subgroup phase barrier outside EXEC.
					if (program.stage == ShaderType::Compute &&
					    inst.memory.kind == IR::ResourceKind::Lds) {
						return true;
					}
					break;
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

bool ProgramSupportsHalfWaveSeparable(const IR::Program& program) {
	if (program.stage != ShaderType::Compute || program.wave_size != 64u ||
	    program.lane_mask_mode != ShaderLaneMaskMode::NativeWave || program.blocks.empty() ||
	    !program.compute_linear_local64 || !ProgramRequiresExactSubgroupSize(program)) {
		return false;
	}

	auto find_block = [&](uint32_t id) -> const IR::BasicBlock* {
		const auto it = std::find_if(program.blocks.begin(), program.blocks.end(),
		                             [id](const auto& block) { return block.id == id; });
		return it == program.blocks.end() ? nullptr : &*it;
	};
	const IR::BasicBlock* branch_header = nullptr;
	for (const auto& block: program.blocks) {
		const auto& term = block.terminator;
		if (term.kind == CFG::TerminatorKind::IndirectBranch ||
		    term.kind == CFG::TerminatorKind::Unsupported || term.loop_header) {
			return false;
		}
		if (term.kind == CFG::TerminatorKind::ConditionalBranch) {
			if (branch_header != nullptr ||
			    (term.condition != CFG::BranchCondition::ExecZero &&
			     term.condition != CFG::BranchCondition::ExecNonZero)) {
				return false;
			}
			branch_header = &block;
		}
	}
	if (branch_header == nullptr) {
		return false;
	}

	// Accept only an acyclic inactive-half bypass: EXECZ jumps directly to merge,
	// while the active half executes one straight-line block and then merges.
	const auto& branch = branch_header->terminator;
	if (branch.merge_block == UINT32_MAX || branch_header->successors.size() != 2u) {
		return false;
	}
	const auto bypass_id = branch.condition == CFG::BranchCondition::ExecZero
	                           ? branch.true_block
	                           : branch.false_block;
	const auto active_id = branch.condition == CFG::BranchCondition::ExecZero
	                           ? branch.false_block
	                           : branch.true_block;
	const auto* active_block = find_block(active_id);
	if (bypass_id != branch.merge_block || active_block == nullptr ||
	    active_block->successors.size() != 1u ||
	    active_block->successors[0] != branch.merge_block ||
	    active_block->terminator.kind != CFG::TerminatorKind::Branch) {
		return false;
	}
	std::vector<uint32_t> tail_blocks;
	const auto*           tail = find_block(branch.merge_block);
	while (tail != nullptr) {
		if (!tail->instructions.empty() ||
		    std::find(tail_blocks.begin(), tail_blocks.end(), tail->id) != tail_blocks.end()) {
			return false;
		}
		tail_blocks.push_back(tail->id);
		if (tail->terminator.kind == CFG::TerminatorKind::Return) {
			break;
		}
		if (tail->terminator.kind != CFG::TerminatorKind::Branch ||
		    tail->successors.size() != 1u) {
			return false;
		}
		tail = find_block(tail->successors[0]);
	}
	if (tail == nullptr || tail->terminator.kind != CFG::TerminatorKind::Return) {
		return false;
	}
	for (const auto& block: program.blocks) {
		if (block.id != branch_header->id && block.id != active_id &&
		    std::find(tail_blocks.begin(), tail_blocks.end(), block.id) == tail_blocks.end()) {
			return false;
		}
	}

	auto is_register_file = [](const IR::Operand& operand, IR::RegisterFile file) {
		return operand.kind == IR::OperandKind::Register && operand.reg.file == file;
	};
	auto is_scalar_register = [](const IR::Operand& operand) {
		return operand.kind == IR::OperandKind::Register &&
		       operand.reg.file != IR::RegisterFile::Vector &&
		       operand.reg.file != IR::RegisterFile::Exec;
	};
	uint32_t     exec_compare_count = 0;
	IR::Register gating_lane {};
	bool         has_gating_lane = false;
	uint32_t     guarded_store_count = 0;
	for (const auto& block: program.blocks) {
		const bool in_active_block = block.id == active_id;
		for (const auto& inst: block.instructions) {
			if (inst.memory.kind == IR::ResourceKind::Lds ||
			    inst.memory.kind == IR::ResourceKind::Gds ||
			    Emitter::InstructionHasDppSource(inst) || IsAtomic(inst.op) ||
			    inst.op == IR::Opcode::ImageStore || inst.op == IR::Opcode::FlatStoreByte ||
			    inst.op == IR::Opcode::FlatStoreShort || inst.op == IR::Opcode::FlatStoreDword ||
			    inst.op == IR::Opcode::Export) {
				return false;
			}
			for (uint32_t source = 0; source < inst.src_count; source++) {
				if (is_register_file(inst.src[source], IR::RegisterFile::Exec)) {
					return false;
				}
			}
			if (is_register_file(inst.dst2, IR::RegisterFile::Exec) || ProducesLaneMask(inst) ||
			    inst.op == IR::Opcode::SelectMaskU32 ||
			    inst.op == IR::Opcode::SelectMaskF32Bits ||
			    inst.op == IR::Opcode::SaveexecB32 || inst.op == IR::Opcode::SaveexecB64) {
				return false;
			}
			if (is_register_file(inst.dst, IR::RegisterFile::Exec)) {
				if (&block != branch_header || !IR::IsCompareOpcode(inst.op)) {
					return false;
				}
				exec_compare_count++;
				for (uint32_t source = 0; source < inst.src_count; source++) {
					if (is_register_file(inst.src[source], IR::RegisterFile::Vector)) {
						if (has_gating_lane) {
							return false;
						}
						gating_lane     = inst.src[source].reg;
						has_gating_lane = true;
					}
				}
			} else if (IR::IsCompareOpcode(inst.op) && !Emitter::IsSccOperand(inst.dst)) {
				return false;
			}

			switch (inst.op) {
				case IR::Opcode::Barrier:
				case IR::Opcode::WqmB64:
				case IR::Opcode::MaskedBitCountLowU32:
				case IR::Opcode::MaskedBitCountHighU32:
				case IR::Opcode::ReadFirstLaneU32:
				case IR::Opcode::ReadLaneU32:
				case IR::Opcode::WriteLaneU32:
				case IR::Opcode::Permlane16B32:
				case IR::Opcode::Permlanex16B32:
				case IR::Opcode::DsSwizzleB32:
				case IR::Opcode::DsConsume:
				case IR::Opcode::DsAppend:
				case IR::Opcode::ImageGetLod: return false;
				default: break;
			}

			const bool buffer_store = inst.op == IR::Opcode::BufferStoreByte ||
			                          inst.op == IR::Opcode::BufferStoreShort ||
			                          inst.op == IR::Opcode::BufferStoreDword;
			if (buffer_store) {
				if (!in_active_block || !has_gating_lane || !inst.memory.idxen ||
				    inst.memory.offen || inst.src_count < 2u ||
				    inst.src[1].kind != IR::OperandKind::Register ||
				    inst.src[1].reg != gating_lane ||
				    inst.memory.resource >= program.info.buffers.size() ||
				    (program.info.buffers[inst.memory.resource].packed_stride & 0x3fffu) == 0u) {
					return false;
				}
				guarded_store_count++;
			}

			if (in_active_block && is_scalar_register(inst.dst)) {
				for (uint32_t source = 0; source < inst.src_count; source++) {
					if (is_register_file(inst.src[source], IR::RegisterFile::Vector)) {
						return false;
					}
				}
			}
		}
	}
	if (exec_compare_count != 1u || !has_gating_lane || guarded_store_count != 1u) {
		return false;
	}
	if (branch_header->instructions.empty() ||
	    !is_register_file(branch_header->instructions.back().dst, IR::RegisterFile::Exec)) {
		return false;
	}
	uint32_t               index_definition_count = 0;
	const IR::Instruction* index_definition       = nullptr;
	IR::Register           workgroup_register {};
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			const bool defines_gate =
			    (inst.dst.kind == IR::OperandKind::Register && inst.dst.reg == gating_lane) ||
			    (inst.dst2.kind == IR::OperandKind::Register && inst.dst2.reg == gating_lane);
			if (!defines_gate) {
				continue;
			}
			if (&block != branch_header || inst.op != IR::Opcode::ShiftLeftAddU32 ||
			    inst.src_count != 3u ||
			    !is_register_file(inst.src[0], IR::RegisterFile::Scalar) ||
			    inst.src[1].kind != IR::OperandKind::ImmediateU32 || inst.src[1].imm != 6u ||
			    inst.src[2].kind != IR::OperandKind::Register || inst.src[2].reg != gating_lane) {
				return false;
			}
			index_definition_count++;
			index_definition = &inst;
			workgroup_register = inst.src[0].reg;
		}
	}
	if (index_definition_count != 1u || gating_lane.index != 0u ||
	    program.compute_thread_ids_num < 1 || program.compute_workgroup_register < 0 ||
	    workgroup_register.index != static_cast<uint32_t>(program.compute_workgroup_register)) {
		return false;
	}
	const auto has_input = [&](IR::StageInputKind kind) {
		return std::any_of(program.info.inputs.begin(), program.info.inputs.end(),
		                   [kind](const auto& input) { return input.kind == kind; });
	};
	if (!has_input(IR::StageInputKind::WorkgroupId) ||
	    !has_input(IR::StageInputKind::LocalInvocationId)) {
		return false;
	}
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (&inst == index_definition) {
				continue;
			}
			if ((inst.dst.kind == IR::OperandKind::Register &&
			     inst.dst.reg == workgroup_register) ||
			    (inst.dst2.kind == IR::OperandKind::Register &&
			     inst.dst2.reg == workgroup_register)) {
				return false;
			}
		}
	}

	// The merge tail is instruction-free, so active-half scalar definitions cannot
	// be observed after reconvergence. Scalar defs are still required to be uniform
	// within the active block above.
	return true;
}

namespace {

struct LogicalMaskTaint {
	std::array<bool, 256> scalar {};
	std::array<bool, 2>   vcc {};
	std::array<bool, 2>   exec {};

	bool operator==(const LogicalMaskTaint&) const = default;
};

bool LogicalMaskPairOpcode(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::MoveU64:
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
		case IR::Opcode::SelectU64: return true;
		default: return false;
	}
}

bool LogicalMaskPointwiseConsumer(IR::Opcode op) {
	switch (op) {
		case IR::Opcode::MoveU32:
		case IR::Opcode::MoveU64:
		case IR::Opcode::SaveexecB32:
		case IR::Opcode::SaveexecB64:
		case IR::Opcode::BitwiseAndU32:
		case IR::Opcode::BitwiseAndU64:
		case IR::Opcode::BitwiseAndNotU32:
		case IR::Opcode::BitwiseAndNotU64:
		case IR::Opcode::BitwiseOrU32:
		case IR::Opcode::BitwiseOrU64:
		case IR::Opcode::BitwiseOrNotU32:
		case IR::Opcode::BitwiseOrNotU64:
		case IR::Opcode::BitwiseXorU32:
		case IR::Opcode::BitwiseXorU64:
		case IR::Opcode::BitwiseNandU32:
		case IR::Opcode::BitwiseNandU64:
		case IR::Opcode::BitwiseNorU32:
		case IR::Opcode::BitwiseNorU64:
		case IR::Opcode::BitwiseXnorU32:
		case IR::Opcode::BitwiseXnorU64:
		case IR::Opcode::BitwiseNotU32:
		case IR::Opcode::BitwiseNotU64:
		case IR::Opcode::SelectU32:
		case IR::Opcode::SelectU64:
		case IR::Opcode::SelectMaskU32:
		case IR::Opcode::SelectMaskF32Bits:
			return true;
		// A per-lane nonzero test followed by the logical SCC reduction exactly
		// implements "any bit set". Equality of one numeric mask dword does not.
		case IR::Opcode::CompareNeU32:
		case IR::Opcode::CompareNeU64:
		case IR::Opcode::CompareEqU64: return true;
		default: return false;
	}
}

bool LogicalMaskRegisterTainted(const LogicalMaskTaint& taint, IR::Register reg,
	                              bool pair) {
	auto part = [&](uint32_t offset) {
		switch (reg.file) {
			case IR::RegisterFile::Scalar:
				return reg.index + offset < taint.scalar.size() &&
			       taint.scalar[reg.index + offset];
			case IR::RegisterFile::Vcc:
				return reg.index + offset < taint.vcc.size() && taint.vcc[reg.index + offset];
			case IR::RegisterFile::Exec:
				return reg.index + offset < taint.exec.size() &&
			       taint.exec[reg.index + offset];
			default: return false;
		}
	};
	return part(0) || (pair && part(1));
}

void SetLogicalMaskRegisterTaint(LogicalMaskTaint& taint, const IR::Operand& operand,
	                               bool pair, bool value) {
	if (operand.kind != IR::OperandKind::Register) {
		return;
	}
	auto set_part = [&](uint32_t offset) {
		const auto index = operand.reg.index + offset;
		switch (operand.reg.file) {
			case IR::RegisterFile::Scalar:
				if (index < taint.scalar.size()) taint.scalar[index] = value;
				break;
			case IR::RegisterFile::Vcc:
				if (index < taint.vcc.size()) taint.vcc[index] = value;
				break;
			case IR::RegisterFile::Exec:
				if (index < taint.exec.size()) taint.exec[index] = true;
				break;
			default: break;
		}
	};
	set_part(0);
	if (pair) set_part(1);
}

bool LogicalMaskInstructionSourceTainted(const LogicalMaskTaint& taint,
	                                       const IR::Instruction& inst) {
	const bool pair = LogicalMaskPairOpcode(inst.op) || IR::IsCompare64Opcode(inst.op);
	for (uint32_t i = 0; i < inst.src_count; i++) {
		if (inst.src[i].kind == IR::OperandKind::Register &&
		    LogicalMaskRegisterTainted(taint, inst.src[i].reg, pair)) {
			return true;
		}
	}
	return false;
}

bool LogicalMaskTaintTransfer(const IR::Instruction& inst, LogicalMaskTaint& taint) {
	const bool source_tainted = LogicalMaskInstructionSourceTainted(taint, inst);
	if (source_tainted && !LogicalMaskPointwiseConsumer(inst.op)) {
		return false;
	}

	const bool compare_mask = IR::IsCompareOpcode(inst.op) && !Emitter::IsSccOperand(inst.dst);
	const bool pair         = compare_mask || LogicalMaskPairOpcode(inst.op);
	if (inst.op == IR::Opcode::SaveexecB32 || inst.op == IR::Opcode::SaveexecB64) {
		SetLogicalMaskRegisterTaint(taint, inst.dst, pair, true);
		return true;
	}
	SetLogicalMaskRegisterTaint(taint, inst.dst, pair, compare_mask || source_tainted);
	SetLogicalMaskRegisterTaint(taint, inst.dst2, false,
	                           ProducesLaneMask(inst) || source_tainted);
	return true;
}

bool ProgramPreservesLogicalScalarMasks(const IR::Program& program) {
	if (program.blocks.empty()) {
		return true;
	}
	const auto block_index = [&](uint32_t id) {
		const auto it = std::find_if(program.blocks.begin(), program.blocks.end(),
		                             [id](const auto& block) { return block.id == id; });
		return it == program.blocks.end() ? program.blocks.size()
		                                  : static_cast<size_t>(it - program.blocks.begin());
	};
	std::vector<LogicalMaskTaint> input(program.blocks.size());
	std::vector<bool>             reachable(program.blocks.size(), false);
	reachable[0]       = true;
	input[0].exec[0]  = true;
	input[0].exec[1]  = true;
	bool changed       = true;
	while (changed) {
		changed = false;
		for (size_t i = 0; i < program.blocks.size(); i++) {
			if (!reachable[i]) continue;
			auto output = input[i];
			for (const auto& inst: program.blocks[i].instructions) {
				if (!LogicalMaskTaintTransfer(inst, output)) return false;
			}
			for (const auto successor: program.blocks[i].successors) {
				const auto target = block_index(successor);
				if (target == program.blocks.size()) return false;
				if (!reachable[target]) {
					reachable[target] = true;
					input[target]     = output;
					changed           = true;
					continue;
				}
				auto merged = input[target];
				for (size_t reg = 0; reg < merged.scalar.size(); reg++)
					merged.scalar[reg] = merged.scalar[reg] || output.scalar[reg];
				for (size_t part = 0; part < 2; part++) {
					merged.vcc[part]  = merged.vcc[part] || output.vcc[part];
					merged.exec[part] = merged.exec[part] || output.exec[part];
				}
				if (!(merged == input[target])) {
					input[target] = merged;
					changed       = true;
				}
			}
		}
	}
	return true;
}

} // namespace

bool ProgramSupportsLogicalSingleWaveWorkgroup(const IR::Program& program) {
	if (program.stage != ShaderType::Compute || program.wave_size != 64u ||
	    program.lane_mask_mode != ShaderLaneMaskMode::PerInvocation) {
		return false;
	}
	if (!ProgramPreservesLogicalScalarMasks(program)) {
		return false;
	}
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
				auto safe_dpp = [](const IR::Operand& operand) {
				if (!operand.dpp) {
					return true;
				}
				const auto control = operand.dpp_ctrl;
				return control <= 0xffu || (control >= 0x101u && control <= 0x10fu) ||
				       (control >= 0x111u && control <= 0x11fu) ||
				       (control >= 0x121u && control <= 0x12fu) || control == 0x140u ||
				       control == 0x141u || (control >= 0x160u && control <= 0x16fu);
			};
			if (!safe_dpp(inst.dst) || !safe_dpp(inst.dst2)) {
				return false;
			}
			for (uint32_t source = 0; source < inst.src_count; source++) {
				if (!safe_dpp(inst.src[source])) {
					return false;
				}
			}
			if ((inst.op == IR::Opcode::MaskedBitCountLowU32 ||
			     inst.op == IR::Opcode::MaskedBitCountHighU32) &&
			    inst.src_count != 0u && inst.src[0].kind == IR::OperandKind::Register &&
			    (inst.src[0].reg.file == IR::RegisterFile::Exec ||
			     inst.src[0].reg.file == IR::RegisterFile::Vcc)) {
				// Logical masks are stored as one Boolean per invocation, not as the
				// numeric 32-bit mask half required by V_MBCNT.
				return false;
			}
			switch (inst.op) {
				// These operations still use host subgroup collectives/shuffles. Their
				// source lane may live in the other subgroup32 half, so accepting them
				// would silently produce a wrong wave64 result.
				case IR::Opcode::WqmB64:
				case IR::Opcode::DsSwizzleB32:
				case IR::Opcode::DsConsume:
				case IR::Opcode::DsAppend:
				case IR::Opcode::ImageGetLod: return false;
				default: break;
			}
		}
	}
	return true;
}

namespace {

bool OperandIsScc(const IR::Operand& operand) {
	return operand.kind == IR::OperandKind::Register &&
	       operand.reg.file == IR::RegisterFile::Scc;
}

std::array<bool, 256> FindNonUniformScalarRegisters(const IR::Program& program) {
	std::array<bool, 256> nonuniform {};
	bool                  changed = true;
	while (changed) {
		changed = false;
		for (const auto& block: program.blocks) {
			for (const auto& inst: block.instructions) {
				auto source_nonuniform = [&](const IR::Operand& source) {
					if (source.kind != IR::OperandKind::Register) {
						return false;
					}
					if (source.reg.file != IR::RegisterFile::Scalar) {
						return true;
					}
					return source.reg.index < nonuniform.size() && nonuniform[source.reg.index];
				};
				bool tainted = false;
				for (uint32_t i = 0; i < inst.src_count; i++) {
					tainted = tainted || source_nonuniform(inst.src[i]);
				}
				auto taint_destination = [&](const IR::Operand& destination) {
					if (!tainted || destination.kind != IR::OperandKind::Register ||
					    destination.reg.file != IR::RegisterFile::Scalar ||
					    destination.reg.index >= nonuniform.size() ||
					    nonuniform[destination.reg.index]) {
						return;
					}
					nonuniform[destination.reg.index] = true;
					changed                               = true;
				};
				taint_destination(inst.dst);
				taint_destination(inst.dst2);
			}
		}
	}
	return nonuniform;
}

bool SccBranchIsWorkgroupUniform(const IR::BasicBlock& block,
	                              const std::array<bool, 256>& nonuniform_scalar) {
	for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
		const auto& inst = *it;
		if (!Emitter::IsSccOperand(inst.dst)) {
			continue;
		}
		for (uint32_t i = 0; i < inst.src_count; i++) {
			const auto& source = inst.src[i];
			if (source.kind != IR::OperandKind::Register) {
				continue;
			}
			if (source.reg.file != IR::RegisterFile::Scalar ||
			    source.reg.index >= nonuniform_scalar.size() ||
			    nonuniform_scalar[source.reg.index]) {
				return false;
			}
		}
		return true;
	}
	return false;
}

const IR::BasicBlock* FindIrBlock(const IR::Program& program, uint32_t id) {
	const auto it = std::find_if(program.blocks.begin(), program.blocks.end(),
	                             [id](const auto& block) { return block.id == id; });
	return it == program.blocks.end() ? nullptr : &*it;
}

bool ReachesByStraightLine(const IR::Program& program, uint32_t from, uint32_t target) {
	std::vector<uint32_t> seen;
	while (from != UINT32_MAX && from != target && seen.size() <= program.blocks.size()) {
		if (std::find(seen.begin(), seen.end(), from) != seen.end()) {
			return false;
		}
		seen.push_back(from);
		const auto* block = FindIrBlock(program, from);
		if (block == nullptr || block->successors.size() != 1u) {
			return false;
		}
		from = block->successors[0];
	}
	return from == target;
}

bool InstructionNeedsLogicalWorkgroupCollective(const IR::Instruction& inst) {
	if (inst.op == IR::Opcode::Barrier || inst.op == IR::Opcode::ReadLaneU32 ||
	    inst.op == IR::Opcode::ReadFirstLaneU32 ||
	    (inst.op == IR::Opcode::CompareEqU64 && Emitter::IsSccOperand(inst.dst))) {
		return true;
	}
	for (uint32_t i = 0; i < inst.src_count; i++) {
		if (OperandIsScc(inst.src[i])) {
			return true;
		}
	}
	return false;
}

bool ProgramHasUnsynchronizedLogicalLdsRead(const IR::Program& program) {
	std::vector<bool> dirty_in(program.blocks.size(), false);
	std::vector<bool> reachable(program.blocks.size(), false);
	if (!program.blocks.empty()) {
		reachable[0] = true;
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (size_t i = 0; i < program.blocks.size(); i++) {
			if (!reachable[i]) {
				continue;
			}
			bool dirty = dirty_in[i];
			for (const auto& inst: program.blocks[i].instructions) {
				if (inst.op == IR::Opcode::Barrier) {
					dirty = false;
					continue;
				}
				if (inst.memory.kind != IR::ResourceKind::Lds) {
					continue;
				}
				if (IsDsRead(inst.op) && dirty) {
					return true;
				}
				if (IsDsWrite(inst.op) || IsAtomic(inst.op)) {
					dirty = true;
				}
			}
			for (const auto successor: program.blocks[i].successors) {
				const auto index = std::find_if(
				    program.blocks.begin(), program.blocks.end(),
				    [successor](const auto& block) { return block.id == successor; });
				if (index == program.blocks.end()) {
					return true;
				}
				const auto target = static_cast<size_t>(index - program.blocks.begin());
				if (!reachable[target] || (dirty && !dirty_in[target])) {
					reachable[target] = true;
					dirty_in[target]  = dirty_in[target] || dirty;
					changed           = true;
				}
			}
		}
	}
	return false;
}

} // namespace

bool ProgramSupportsLogicalMultiWaveWorkgroup(const IR::Program& program) {
	if (!ProgramSupportsLogicalSingleWaveWorkgroup(program) || program.blocks.empty()) {
		return false;
	}
	if (ProgramHasUnsynchronizedLogicalLdsRead(program)) {
		return false;
	}

	const auto nonuniform_scalar = FindNonUniformScalarRegisters(program);
	std::vector<bool> nonuniform_control(program.blocks.size(), false);
	auto block_index = [&](uint32_t id) -> size_t {
		const auto it = std::find_if(program.blocks.begin(), program.blocks.end(),
		                             [id](const auto& block) { return block.id == id; });
		return it == program.blocks.end() ? program.blocks.size()
		                                  : static_cast<size_t>(it - program.blocks.begin());
	};

	for (const auto& block: program.blocks) {
		const auto& term = block.terminator;
		if (term.kind == CFG::TerminatorKind::IndirectBranch ||
		    term.kind == CFG::TerminatorKind::Unsupported) {
			return false;
		}
		if (term.kind != CFG::TerminatorKind::ConditionalBranch) {
			continue;
		}
		bool uniform = false;
		switch (term.condition) {
			case CFG::BranchCondition::SccZero:
			case CFG::BranchCondition::SccNonZero:
				uniform = SccBranchIsWorkgroupUniform(block, nonuniform_scalar);
				break;
			default: break;
		}
		if (uniform) {
			continue;
		}
		if (term.loop_header) {
			return false;
		}
		uint32_t merge_block = term.merge_block;
		if (merge_block == UINT32_MAX) {
			if (ReachesByStraightLine(program, term.false_block, term.true_block)) {
				merge_block = term.true_block;
			} else if (ReachesByStraightLine(program, term.true_block, term.false_block)) {
				merge_block = term.false_block;
			} else {
				return false;
			}
		}
		std::vector<uint32_t> pending {term.true_block, term.false_block};
		std::vector<uint32_t> seen;
		while (!pending.empty()) {
			const auto id = pending.back();
			pending.pop_back();
			if (id == merge_block ||
			    std::find(seen.begin(), seen.end(), id) != seen.end()) {
				continue;
			}
			seen.push_back(id);
			const auto index = block_index(id);
			if (index == program.blocks.size()) {
				return false;
			}
			nonuniform_control[index] = true;
			const auto* region_block  = FindIrBlock(program, id);
			pending.insert(pending.end(), region_block->successors.begin(),
			               region_block->successors.end());
		}
	}

	for (size_t i = 0; i < program.blocks.size(); i++) {
		if (!nonuniform_control[i]) {
			continue;
		}
		const auto& block = program.blocks[i];
		if (block.terminator.kind == CFG::TerminatorKind::ConditionalBranch) {
			return false;
		}
		if (std::any_of(block.instructions.begin(), block.instructions.end(),
		                InstructionNeedsLogicalWorkgroupCollective)) {
			return false;
		}
	}
	return true;
}

bool EmitProgram(const IR::Program& program, const IR::ResourceSnapshot& resources,
                 const ShaderVertexInputInfo*  vertex_input_info,
                 const ShaderPixelInputInfo*   pixel_input_info,
                 const ShaderComputeInputInfo* compute_input_info, std::vector<uint32_t>& spirv,
                 std::string* error) {
	using namespace Emitter;

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

	EmitterState state(program, resources);
	state.vertex_input_info    = vertex_input_info;
	state.pixel_input_info     = pixel_input_info;
	state.compute_input_info   = compute_input_info;
	state.stage                = program.stage;
	state.wave_size            = program.wave_size;
	state.per_invocation_masks = program.lane_mask_mode == ShaderLaneMaskMode::PerInvocation;
	if (program.stage == ShaderType::Compute && state.per_invocation_masks &&
	    program.wave_size == 64u && compute_input_info != nullptr) {
		const auto local_threads =
		    std::max(compute_input_info->threads_num[0], 1u) *
		    std::max(compute_input_info->threads_num[1], 1u) *
		    std::max(compute_input_info->threads_num[2], 1u);
		state.logical_local_threads         = local_threads;
		state.logical_wave_count            = (local_threads + program.wave_size - 1u) /
		                                      program.wave_size;
		state.logical_single_wave_workgroup = local_threads == program.wave_size;
		state.logical_multi_wave_workgroup  = local_threads > program.wave_size &&
		                                         local_threads % program.wave_size == 0u &&
		                                         ProgramSupportsLogicalMultiWaveWorkgroup(program);
		if (local_threads > program.wave_size && !state.logical_multi_wave_workgroup) {
			return Fail(error,
			            "logical multi-wave workgroup contains a non-uniform collective site");
		}
	}
	state.exact_subgroup_operations =
	    !state.per_invocation_masks && ProgramRequiresExactSubgroupSize(program);
	state.registers.reserve(InitialEmitterVectorReserve);
	state.inputs.reserve(InitialEmitterVectorReserve);
	state.outputs.reserve(InitialEmitterVectorReserve);
	state.interface_variables.reserve(InitialEmitterVectorReserve);
	state.reachable_blocks.reserve(InitialEmitterVectorReserve);
	CollectRegisters(program, state.registers);
	CopyProgramInputsAndOutputs(state, program);
	state.needs_subgroup_ballot              = ProgramNeedsSubgroupBallot(program);
	state.needs_subgroup_shuffle             = ProgramNeedsSubgroupShuffle(program);
	state.needs_subgroup_local_invocation_id = ProgramNeedsSubgroupLocalInvocationId(program);
	state.needs_compute_derivatives          = ProgramNeedsComputeDerivatives(program);
	state.needs_image_gather_extended        = ProgramNeedsImageGatherExtended(program);
	state.needs_sampled_image_nonuniform = false;
	state.needs_function_lds                 = ProgramNeedsFunctionLds(program);
	state.needs_pixel_valid_mask             = ProgramNeedsPixelValidMask(program);
	ComputeReachableBlocks(state, program);
	AllocateInputVariables(state);
	AllocateOutputVariables(state);
	AllocateDescriptorVariables(state);
	EmitHeaderAndTypes(state);
	AllocateRegisterVariables(state);
	AllocateBlockLabels(state, program);
	AllocateDispatcherState(state, program);
	EmitFunction(state, program);
	if (state.unsupported_ir_instruction) {
		char pc[9] {};
		std::snprintf(pc, sizeof(pc), "%08x", state.unsupported_ir_pc);
		return Fail(error, "SPIR-V emitter has no handler for IR opcode " +
		                       std::string(IR::OpcodeName(state.unsupported_ir_opcode)) +
		                       " at pc 0x" + pc);
	}

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
	spirv = std::move(binary);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
