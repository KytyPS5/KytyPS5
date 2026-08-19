#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ValueProgram.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

using ShaderError::Fail;

namespace {

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
		const auto kind = IR::DescriptorBindingForImage(program.info.images[i]);
		if (!kind.has_value()) {
			return Fail(error, "native shader plan has an invalid image class");
		}
		present[static_cast<size_t>(*kind)] = true;
		const auto dynamic = program.info.images[i].mip_mode == IR::ImageMipMode::DynamicStorage;
		const auto count   = dynamic ? program.info.images[i].mip_count : 1u;
		if (count == 0u || (!dynamic && program.info.images[i].mip_count != 1u)) {
			return Fail(error, "native shader plan has an invalid image mip descriptor count");
		}
		expected[static_cast<size_t>(*kind)].insert(expected[static_cast<size_t>(*kind)].end(),
		                                            count, i);
	}
	if (!program.info.samplers.empty()) {
		Expect(Kind::Samplers, Dense(program.info.samplers.size()));
	}
	bool uses_gds = false;
	if (program.values != nullptr) {
		for (const auto* block: program.values->blocks) {
			for (const auto& inst: *block) {
				if (IR::SharedAccessOf(inst.GetOpcode()) == IR::SharedAccess::None) {
					continue;
				}
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.values->memory_info.size()) {
					return Fail(error, "shared operation has invalid memory metadata");
				}
				const auto kind = program.values->memory_info[index].kind;
				if (kind != IR::ResourceKind::Lds && kind != IR::ResourceKind::Gds) {
					return Fail(error, "shared operation has invalid resource kind");
				}
				uses_gds |= kind == IR::ResourceKind::Gds;
			}
		}
	}
	if (uses_gds) {
		Expect(Kind::Gds);
	}
	if (!program.info.addresses.empty()) {
		Expect(Kind::AddressMemory, Dense(program.info.addresses.size()));
	}
	const bool uses_flattened_runtime =
	    program.values != nullptr &&
	    (!program.values->srt_reads.empty() ||
	     std::ranges::any_of(program.info.images, [](const IR::ImageResource& image) {
		     return image.indirect_mapping_capacity != 0u;
	     }));
	if (uses_flattened_runtime) {
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

	if (program.values == nullptr) {
		return Fail(error, "native shader plan has no typed SSA");
	}
	const auto planning_only_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       const auto op = use.user->GetOpcode();
			       if (op != IR::ValueOpcode::LoadAddressU32 &&
			           op != IR::ValueOpcode::ReadConstBuffer) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.values->memory_info.size() &&
			              program.values->memory_info[index].planning_only;
		       });
	};
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			const auto dense = inst.Flags<uint32_t>();
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::GetBufferResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (dense >= program.info.buffers.size()) {
						return Fail(error, "typed buffer handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetAddressResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (dense >= program.info.addresses.size()) {
						return Fail(error, "typed address handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetImageResource:
					if (dense >= program.info.images.size()) {
						return Fail(error, "typed image handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetSamplerResource:
					if (dense >= program.info.samplers.size()) {
						return Fail(error, "typed sampler handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::ReadConst: {
					const auto slot = inst.Arg(1).Resolve();
					if (!slot.IsImmediate() || slot.GetType() != IR::Type::U32 ||
					    slot.U32() >= program.values->srt_reads.size()) {
						return Fail(error, "flattened SRT read has an invalid dense slot");
					}
					break;
				}
				default: break;
			}
		}
	}
	return true;
}

} // namespace

bool AnalyzeProgramRequirements(IR::Program& program, std::string* error) {
	program.spirv_requirements.reset();
	IR::SpirvRequirements requirements {};
	if (program.values == nullptr) {
		program.spirv_requirements.emplace(requirements);
		return true;
	}
	const auto MarkExactSubgroup = [&] {
		requirements.requires_exact_subgroup = true;
		requirements.subgroup_ballot         = true;
	};
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.values->memory_info.size()) {
					return Fail(error, "buffer operation has invalid memory metadata");
				}
				const auto& memory = program.values->memory_info[memory_index];
				if (memory.kind == IR::ResourceKind::Buffer) {
					if (memory.resource >= program.info.buffers.size()) {
						return Fail(error, "buffer operation has invalid resource metadata");
					}
					if ((program.info.buffers[memory.resource].packed_stride & (1u << 20u)) != 0u) {
						if (program.stage != ShaderType::Compute) {
							return Fail(error, "buffer ADD_TID is only valid for compute shaders");
						}
						requirements.requires_exact_subgroup      = true;
						requirements.subgroup_local_invocation_id = true;
					}
				}
			}
			const auto shared_access = IR::SharedAccessOf(inst.GetOpcode());
			if (shared_access != IR::SharedAccess::None) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.values->memory_info.size()) {
					return Fail(error, "shared operation has invalid memory metadata");
				}
				const auto kind = program.values->memory_info[index].kind;
				if (kind != IR::ResourceKind::Lds && kind != IR::ResourceKind::Gds) {
					return Fail(error, "shared operation has invalid resource kind");
				}
				if (program.stage != ShaderType::Compute && kind == IR::ResourceKind::Lds) {
					requirements.function_lds = true;
				}
				if (shared_access == IR::SharedAccess::Append ||
				    shared_access == IR::SharedAccess::Consume) {
					MarkExactSubgroup();
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
				}
			}
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::Ballot: MarkExactSubgroup(); break;
				case IR::ValueOpcode::DppMoveU32:
				case IR::ValueOpcode::ReadFirstLane:
				case IR::ValueOpcode::ReadLane: {
					MarkExactSubgroup();
					requirements.subgroup_shuffle = true;
					if (inst.GetOpcode() == IR::ValueOpcode::DppMoveU32) {
						requirements.subgroup_local_invocation_id = true;
					}
					break;
				}
				case IR::ValueOpcode::DppUpdateU32:
				case IR::ValueOpcode::WqmMask:
				case IR::ValueOpcode::WriteLane: {
					MarkExactSubgroup();
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::Permlane16U32: {
					MarkExactSubgroup();
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::SwizzleU32: {
					MarkExactSubgroup();
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::LaneId:
					requirements.subgroup_local_invocation_id = true;
					break;
				case IR::ValueOpcode::ImageQueryLod: requirements.compute_derivatives = true; break;
				case IR::ValueOpcode::ImageGatherRaw:
					requirements.image_gather_extended = true;
					break;
				case IR::ValueOpcode::SetAttribute: {
					const auto index = inst.Flags<IR::ExportFlags>().index;
					if (index >= program.values->export_info.size()) {
						return Fail(error, "attribute export has invalid metadata");
					}
					if (program.stage == ShaderType::Pixel &&
					    program.values->export_info[index].vm) {
						requirements.pixel_valid_mask = true;
					}
					break;
				}
				default: break;
			}
		}
	}
	program.spirv_requirements.emplace(requirements);
	return true;
}

bool EmitProgram(const IR::Program& program, const IR::ResourceSnapshot& resources,
                 ShaderStageInputInfo input_info, std::vector<uint32_t>& spirv,
                 std::string* error) {
	using namespace Emitter;

	if (program.stage != ShaderType::Compute && program.stage != ShaderType::Vertex &&
	    program.stage != ShaderType::Pixel) {
		SetError(error, "binary SPIR-V emitter supports compute, vertex, and pixel shaders");
		return false;
	}
	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete || !program.binding_layout_complete ||
	    !program.spirv_requirements.has_value()) {
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
	if (program.values == nullptr) {
		SetError(error, "SPIR-V emitter requires planned typed SSA");
		return false;
	}
	const auto& value_program = *program.values;
	if (!IR::ValidateValueProgram(value_program, true, error)) {
		return false;
	}
	EmitterState state(program, resources, input_info);
	state.stage                = program.stage;
	state.wave_size            = program.wave_size;
	state.per_invocation_masks = program.lane_mask_mode == ShaderLaneMaskMode::PerInvocation;
	state.inputs.reserve(program.info.inputs.size());
	state.outputs.reserve(program.info.outputs.size());
	state.interface_variables.reserve(program.info.inputs.size() + program.info.outputs.size());
	CopyProgramInputsAndOutputs(state, program);
	AllocateInputVariables(state);
	AllocateOutputVariables(state);
	DefineModule(state);
	if (!EmitValueProgram(state, value_program, error)) {
		return false;
	}

	auto binary = state.builder.Build();
	if (binary.empty()) {
		SetError(error, "SPIR-V builder returned an empty module");
		return false;
	}
	spirv = std::move(binary);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
