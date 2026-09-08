#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

void ValidateNativeProgram(const IR::Program& program) {
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
			Fail(program, "native shader plan has an invalid image class");
		}
		present[static_cast<size_t>(*kind)] = true;
		const auto dynamic = program.info.images[i].mip_mode == IR::ImageMipMode::DynamicStorage;
		const auto count   = dynamic ? program.info.images[i].mip_count : 1u;
		if (count == 0u || (!dynamic && program.info.images[i].mip_count != 1u)) {
			Fail(program, "native shader plan has an invalid image mip descriptor count");
		}
		expected[static_cast<size_t>(*kind)].insert(expected[static_cast<size_t>(*kind)].end(),
		                                            count, i);
	}
	if (!program.info.samplers.empty()) {
		Expect(Kind::Samplers, Dense(program.info.samplers.size()));
	}
	bool uses_gds = false;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (IR::SharedAccessOf(inst.GetOpcode()) == IR::SharedAccess::None) {
				continue;
			}
			const auto index = inst.Flags<IR::MemoryFlags>().index;
			if (index >= program.memory_info.size()) {
				Fail(program, "shared operation has invalid memory metadata");
			}
			const auto kind = program.memory_info[index].kind;
			if (kind != IR::ResourceKind::Lds && kind != IR::ResourceKind::Gds) {
				Fail(program, "shared operation has invalid resource kind");
			}
			uses_gds |= kind == IR::ResourceKind::Gds;
		}
	}
	if (uses_gds || IR::NeedsWave64BallotStorage(program)) {
		Expect(Kind::Gds);
	}
	if (program.info.uses_dma) {
		Expect(Kind::BdaPagetable);
		Expect(Kind::FaultBuffer);
	}
	const bool uses_flattened_runtime =
	    !program.srt_reads.empty() ||
	    std::ranges::any_of(program.info.bounded_srt_reads, [](const auto& read) { return read.count != 0; }) ||
	    std::ranges::any_of(program.info.buffer_tables, [](const auto& table) { return table.count != 0; }) ||
	     std::ranges::any_of(program.info.images, [](const IR::ImageResource& image) {
		     return image.indirect_search_iterations != 0u;
	     });
	if (uses_flattened_runtime) {
		Expect(Kind::FlattenedSrt);
	}
	if (program.bindings.ShaderDataDwords() != 0 && !program.bindings.UsesPushData()) {
		Expect(Kind::ShaderData);
	}

	std::array<bool, KindCount> seen {};
	for (const auto& binding: program.bindings.descriptors) {
		const auto kind = static_cast<size_t>(binding.kind);
		if (kind >= KindCount || seen[kind] || !present[kind] ||
		    binding.resources != expected[kind]) {
			Fail(program, "native descriptor groups do not match shader topology");
		}
		seen[kind] = true;
	}
	for (size_t i = 0; i < KindCount; i++) {
		if (present[i] != seen[i]) {
			Fail(program, "native shader plan is missing a required descriptor group");
		}
	}
	const auto has_shader_data_storage = present[static_cast<size_t>(Kind::ShaderData)];
	const auto shader_data_dwords = program.bindings.ShaderDataDwords();
	if ((program.bindings.UsesPushData() &&
	     !IR::PushData::CanFit(program.bindings.push_data_start_dword, shader_data_dwords)) ||
	    program.bindings.memory_offset_dword != program.bindings.user_data_registers.size() ||
	    program.bindings.memory_offset_count != program.info.buffers.size() ||
	    program.bindings.memory_limit_dword !=
	        program.bindings.memory_offset_dword +
	            (program.bindings.memory_offset_count + 3u) / 4u ||
	    has_shader_data_storage != (shader_data_dwords != 0 && !program.bindings.UsesPushData()) ||
	    !std::is_sorted(program.bindings.user_data_registers.begin(),
	                    program.bindings.user_data_registers.end()) ||
	    std::adjacent_find(program.bindings.user_data_registers.begin(),
	                       program.bindings.user_data_registers.end()) !=
	        program.bindings.user_data_registers.end()) {
		Fail(program, "native shader-data layout is inconsistent");
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
			       return index < program.memory_info.size() &&
			              program.memory_info[index].planning_only;
		       });
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto dense = inst.Flags<uint32_t>();
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::GetBufferResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (!inst.Uses().empty() && std::ranges::any_of(inst.Uses(), [&](const IR::Use& use) {
						if (IR::BufferAccessOf(use.user->GetOpcode()) == IR::BufferAccess::None) { return false; }
						const auto index = use.user->Flags<IR::MemoryFlags>().index;
						return index < program.memory_info.size() &&
						       program.memory_info[index].buffer_table != UINT32_MAX;
					})) {
						if (dense >= program.info.buffer_tables.size()) {
							Fail(program, "typed buffer handle has an invalid bounded table");
						}
						for (const auto& use: inst.Uses()) {
							if (IR::BufferAccessOf(use.user->GetOpcode()) == IR::BufferAccess::None) {
								Fail(program, "bounded buffer handle has an unsupported use");
							}
							const auto index = use.user->Flags<IR::MemoryFlags>().index;
							if (index >= program.memory_info.size() ||
							    program.memory_info[index].buffer_table != dense) {
								Fail(program, "bounded buffer handle and memory table disagree");
							}
						}
					} else if (dense >= program.info.buffers.size()) {
						Fail(program, "typed buffer handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetAddressResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (inst.NumArgs() != 2 || !program.info.uses_dma) {
						Fail(program, "typed address handle has invalid DMA metadata");
					}
					break;
				case IR::ValueOpcode::GetScratchResource:
					if (inst.NumArgs() != 0 || program.scratch_dwords == 0) {
						Fail(program, "typed scratch handle has invalid shader metadata");
					}
					break;
				case IR::ValueOpcode::GetImageResource:
					if (dense >= program.info.images.size()) {
						Fail(program, "typed image handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetSamplerResource:
					if (dense >= program.info.samplers.size()) {
						Fail(program, "typed sampler handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::ReadConst: {
					const auto slot = inst.Arg(1).Resolve();
					if (!slot.IsImmediate() || slot.GetType() != IR::Type::U32 ||
					    slot.U32() >= program.srt_reads.size()) {
						Fail(program, "flattened SRT read has an invalid dense slot");
					}
					break;
				}
				default: break;
			}
		}
	}
}

} // namespace

void AnalyzeProgramRequirements(IR::Program& program) {
	program.spirv_requirements.reset();
	IR::SpirvRequirements requirements {};
	bool has_lds_append_consume = false;
	const auto MarkBallot = [&] { requirements.subgroup_ballot = true; };
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto address_access = IR::AddressOpcodeInfoOf(inst.GetOpcode()).access;
			if (address_access != IR::AddressAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					Fail(program, "address operation has invalid memory metadata");
				}
				if (program.memory_info[memory_index].kind == IR::ResourceKind::Scratch) {
					if (program.scratch_dwords == 0) {
						Fail(program, "scratch operation has no per-thread storage");
					}
					requirements.function_scratch = true;
				} else if (address_access == IR::AddressAccess::Write) {
					Fail(program, "writable FLAT/GLOBAL addresses require GPU ownership tracking");
				}
			}
			if (IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					Fail(program, "buffer operation has invalid memory metadata");
				}
				const auto& memory = program.memory_info[memory_index];
				const auto inspect_candidate = [&](uint32_t resource) {
					if (resource >= program.info.buffers.size()) {
						Fail(program, "buffer operation has invalid resource metadata");
					}
					if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::Atomic &&
					    inst.GetType() == IR::Type::U64) {
						requirements.buffer_int64_atomics = true;
					}
					if (memory.kind == IR::ResourceKind::Buffer &&
					    (program.info.buffers[resource].packed_stride & (1u << 20u)) != 0u) {
						if (program.stage != ShaderType::Compute) {
							Fail(program, "buffer ADD_TID is only valid for compute shaders");
						}
						requirements.subgroup_local_invocation_id = true;
					}
				};
				if (memory.buffer_table != UINT32_MAX) {
					if (memory.buffer_table >= program.info.buffer_tables.size()) {
						Fail(program, "buffer operation has invalid bounded table metadata");
					}
					const auto& table = program.info.buffer_tables[memory.buffer_table];
					if ((table.count == 0) != table.resources.empty()) {
						Fail(program, "bounded buffer count and candidates disagree");
					}
					for (const auto resource: table.resources) { inspect_candidate(resource); }
				} else if (memory.kind == IR::ResourceKind::Buffer) {
					inspect_candidate(memory.resource);
				}
			}
			const auto shared_access = IR::SharedAccessOf(inst.GetOpcode());
			if (shared_access != IR::SharedAccess::None) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.memory_info.size()) {
					Fail(program, "shared operation has invalid memory metadata");
				}
				const auto kind = program.memory_info[index].kind;
				if (kind != IR::ResourceKind::Lds && kind != IR::ResourceKind::Gds) {
					Fail(program, "shared operation has invalid resource kind");
				}
				if (program.stage != ShaderType::Compute && kind == IR::ResourceKind::Lds) {
					requirements.function_lds = true;
				}
				if (kind == IR::ResourceKind::Lds &&
				    IR::SharedComponentCount(inst.GetOpcode()) > 1u) {
					requirements.shared_multiword_lds = true;
				}
				if (inst.GetOpcode() == IR::ValueOpcode::SharedAtomicIAdd64 ||
				    inst.GetOpcode() == IR::ValueOpcode::SharedAtomicOr64) {
					if (program.stage != ShaderType::Compute || kind != IR::ResourceKind::Lds) {
						Fail(program, "64-bit shared atomics require compute LDS storage");
					}
					requirements.shared_int64_atomics = true;
				}
				if (shared_access == IR::SharedAccess::Append ||
				    shared_access == IR::SharedAccess::Consume) {
					has_lds_append_consume |= kind == IR::ResourceKind::Lds;
					MarkBallot();
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
				}
			}
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::Ballot: MarkBallot(); break;
				case IR::ValueOpcode::DppMoveU32:
				case IR::ValueOpcode::Dpp8MoveU32:
				case IR::ValueOpcode::ReadFirstLane:
				case IR::ValueOpcode::ReadLane: {
					MarkBallot();
					requirements.subgroup_shuffle = true;
					if (inst.GetOpcode() == IR::ValueOpcode::DppMoveU32 ||
					    inst.GetOpcode() == IR::ValueOpcode::Dpp8MoveU32) {
						requirements.subgroup_local_invocation_id = true;
					}
					break;
				}
				case IR::ValueOpcode::DppUpdateU32:
				case IR::ValueOpcode::Dpp8UpdateU32:
				case IR::ValueOpcode::WqmMask:
				case IR::ValueOpcode::WriteLane: {
					MarkBallot();
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::Permlane16U32: {
					MarkBallot();
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::SwizzleU32:
				case IR::ValueOpcode::BpermuteU32: {
					MarkBallot();
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
					if (index >= program.export_info.size()) {
						Fail(program, "attribute export has invalid metadata");
					}
					if (program.stage == ShaderType::Pixel &&
					    program.export_info[index].vm) {
						requirements.pixel_valid_mask = true;
					}
					break;
				}
				default: break;
			}
		}
	}
	if (requirements.shared_int64_atomics && has_lds_append_consume) {
		Fail(program, "64-bit shared atomics cannot share LDS with append/consume operations");
	}
	program.spirv_requirements.emplace(requirements);
}

std::vector<uint32_t> EmitProgram(const IR::Program& program, ShaderStageInputInfo input_info,
                                  const ComputeWorkgroupLimits& compute_workgroup_limits,
                                  const ShaderHostProfile& host_profile,
                                  const IR::ResourceSpecialization& specialization) {
	using namespace Emitter;

	if (program.stage != ShaderType::Compute && program.stage != ShaderType::Vertex &&
	    program.stage != ShaderType::Pixel) {
		Fail(program, "binary SPIR-V emitter supports compute, vertex, and pixel shaders");
	}
	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete || !program.binding_layout_complete ||
	    !program.spirv_requirements.has_value()) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	ValidateNativeProgram(program);
	IR::ValidateProgram(program, true);
	ShaderFloatingPointState initial_fp_state{};
	if (program.stage == ShaderType::Compute && input_info.compute != nullptr) {
		initial_fp_state = input_info.compute->initial_fp_state;
	} else if (program.stage == ShaderType::Vertex && input_info.vertex != nullptr) {
		initial_fp_state = input_info.vertex->initial_fp_state;
	} else if (program.stage == ShaderType::Pixel && input_info.pixel != nullptr) {
		initial_fp_state = input_info.pixel->initial_fp_state;
	}
	const auto f64 = IR::AnalyzeF64Program(program, initial_fp_state, host_profile);
	if (!f64.error.empty()) {
		Fail(program, f64.error.c_str());
	}
	EmitterState state(program, input_info, specialization);
	state.f64_certificate = f64;
	state.stage                = program.stage;
	state.wave_size            = program.wave_size;
	state.native_subgroup_size = compute_workgroup_limits.native_subgroup_size;
	if (state.stage != ShaderType::Compute && state.wave_size == 64u &&
	    state.native_subgroup_size == 32u) {
		LOGF("graphics wave64 partitioned over native subgroup32: hash=0x%016" PRIx64
		     " stage=%u\n",
		     program.shader_hash, static_cast<unsigned>(program.stage));
	}
	if (state.stage == ShaderType::Compute) {
		if (input_info.compute == nullptr) {
			Fail(program, "compute shader requires stage input information");
		}
		state.compute_execution = PlanComputeExecution(program, input_info, compute_workgroup_limits);
		if (!state.compute_execution.error.empty()) {
			Fail(program, state.compute_execution.error.c_str());
		}
		state.compute_workgroup = state.compute_execution.layout;
		if (state.compute_workgroup.IsReshaped() || state.compute_execution.IsSplitWave64()) {
			const auto& layout = state.compute_workgroup;
			LOGF("compute execution geometry: hash=0x%016" PRIx64
			     " guest=%ux%ux%u host=%ux%ux%u wave_partitions=%u split_wave64=%u cooperative_wave64=%u\n",
			     program.shader_hash, layout.guest_size[0], layout.guest_size[1], layout.guest_size[2],
			     layout.host_size[0], layout.host_size[1], layout.host_size[2],
			     state.compute_execution.wave_partition_factor,
			     state.compute_execution.IsSplitWave64() ? 1u : 0u,
			     state.compute_execution.IsCooperativeWave64() ? 1u : 0u);
		}
	}
	state.inputs.reserve(program.info.inputs.size());
	state.outputs.reserve(program.info.outputs.size());
	state.interface_variables.reserve(program.info.inputs.size() + program.info.outputs.size());
	CopyProgramInputsAndOutputs(state, program);
	if (state.compute_workgroup.IsReshaped() || state.compute_execution.IsSplitWave64()) {
		const auto HasInput = [&](IR::StageInputKind kind) {
			return std::ranges::any_of(state.inputs,
			                           [kind](const auto& input) { return input.kind == kind; });
		};
		const bool global_id = HasInput(IR::StageInputKind::GlobalInvocationId);
		if ((state.compute_execution.IsSplitWave64() || global_id || HasInput(IR::StageInputKind::LocalInvocationId)) &&
		    !HasInput(IR::StageInputKind::LocalInvocationIndex)) {
			state.inputs.push_back(
			    {IR::StageInputKind::LocalInvocationIndex, 0, 1, 0, "gl_LocalInvocationIndex"});
		}
		if ((global_id || state.compute_execution.IsSplitWave64()) && !HasInput(IR::StageInputKind::WorkgroupId)) {
			state.inputs.push_back({IR::StageInputKind::WorkgroupId, 0, 3, 0, "gl_WorkGroupID"});
		}
	}
	AllocateInputVariables(state);
	AllocateOutputVariables(state);
	DefineModule(state);
	EmitProgram(state, program);

	auto binary = state.builder.Build();
	if (binary.empty()) {
		Fail(program, "SPIR-V builder returned an empty module");
	}
	return binary;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
