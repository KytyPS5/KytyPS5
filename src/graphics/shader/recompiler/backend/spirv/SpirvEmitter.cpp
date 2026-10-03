#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

void ValidateNativeProgram(const IR::Program& program, bool lds_storage) {
	uint64_t live_buffers = 0;
	bool uses_lds = false;
	bool uses_gds = false;
	const auto uses_mapping = [](const auto& resource) {
		return resource.indirect_root != UINT32_MAX;
	};
	bool uses_flattened_srt = std::ranges::any_of(program.info.buffers, uses_mapping) ||
	                          std::ranges::any_of(program.info.images, uses_mapping);
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
	const auto indirect_buffer_handle = [&](const IR::Inst& handle) {
		return program.info.uses_dma && handle.NumArgs() == 4u && !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::BufferAccessOf(use.user->GetOpcode()) != IR::BufferAccess::Read) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::IndirectBuffer;
		       });
	};
	const auto local_flat_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::AddressOpcodeInfoOf(use.user->GetOpcode()).access == IR::AddressAccess::None)
				       return false;
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::FlatLocal;
		       });
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op = inst.GetOpcode();
			uses_flattened_srt |= op == IR::ValueOpcode::ReadConst;
			if (IR::BufferAccessOf(op) != IR::BufferAccess::None ||
			    IR::SharedAccessOf(op) != IR::SharedAccess::None ||
			    IR::AddressOpcodeInfoOf(op).access != IR::AddressAccess::None) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.memory_info.size()) {
					Fail(program, "typed shader contains invalid memory metadata");
				}
				const auto& memory = program.memory_info[index];
				if (!memory.planning_only) {
					uses_lds |= memory.kind == IR::ResourceKind::FlatLocal;
					if (IR::SharedAccessOf(op) != IR::SharedAccess::None) {
						if (memory.kind != IR::ResourceKind::Lds && memory.kind != IR::ResourceKind::Gds) {
							Fail(program, "typed shader contains invalid shared-memory metadata");
						}
						uses_gds |= memory.kind == IR::ResourceKind::Gds;
						uses_lds |= memory.kind == IR::ResourceKind::Lds;
					} else if (memory.kind == IR::ResourceKind::Buffer ||
					           memory.kind == IR::ResourceKind::ScalarBuffer) {
						if (memory.resource >= program.info.buffers.size()) {
							Fail(program, "typed shader contains an invalid buffer resource");
						}
						live_buffers |= uint64_t{1} << memory.resource;
						for (const auto child: program.info.buffers[memory.resource].indirect_resources) {
							if (child >= program.info.buffers.size()) {
								Fail(program, "typed shader contains an invalid indirect buffer resource");
							}
							live_buffers |= uint64_t{1} << child;
						}
					}
				}
			}
			const auto dense = inst.Flags<uint32_t>();
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::GetBufferResource:
					if (planning_only_handle(inst) || indirect_buffer_handle(inst)) {
						break;
					}
					if (dense >= program.info.buffers.size()) {
						Fail(program, "typed buffer handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetAddressResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (inst.NumArgs() != 2 || (!program.info.uses_dma && !local_flat_handle(inst))) {
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

Emitter::SpirvRequirements Emitter::AnalyzeProgramRequirements(const IR::Program& program) {
	SpirvRequirements requirements {};
	const auto refuse = [&](const IR::Inst& inst, const char* reason) {
		if (requirements.refusal == nullptr) {
			requirements.refusal      = reason;
			requirements.refused_inst = &inst;
		}
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			requirements.float64 |= inst.GetType() == IR::Type::F64;
			if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::Atomic &&
			    inst.GetType() == IR::Type::U64) {
				requirements.buffer_int64_atomics = true;
			}
			const auto address_access = IR::AddressOpcodeInfoOf(inst.GetOpcode()).access;
			if (address_access != IR::AddressAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					Fail(program, "address operation has invalid memory metadata");
				}
				const auto kind = program.memory_info[memory_index].kind;
				if (kind == IR::ResourceKind::Scratch || kind == IR::ResourceKind::FlatLocal) {
					if (program.scratch_dwords == 0) {
						Fail(program, "scratch operation has no per-thread storage");
					}
					requirements.function_scratch = true;
					if (kind == IR::ResourceKind::FlatLocal && program.stage != ShaderType::Compute &&
					    program.stage != ShaderType::Mesh) {
						requirements.function_lds = true;
					}
				}
			}
			if (IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					Fail(program, "buffer operation has invalid memory metadata");
				}
				const auto& memory = program.memory_info[memory_index];
				if (memory.kind == IR::ResourceKind::IndirectBuffer &&
				    inst.GetOpcode() != IR::ValueOpcode::ReadConstBuffer) {
					requirements.subgroup_local_invocation_id = true;
				}
				if (memory.kind == IR::ResourceKind::Buffer) {
					requirements.coherent_buffers |= memory.coherent;
					if (memory.resource >= program.info.buffers.size()) {
						Fail(program, "buffer operation has invalid resource metadata");
					}
					const auto bits = StorageBufferElementBits(program, memory);
					requirements.buffer_u8 |= bits == 8u;
					requirements.buffer_u16 |= bits == 16u;
					if ((program.info.buffers[memory.resource].packed_stride & (1u << 20u)) != 0u) {
						if (program.stage != ShaderType::Compute) {
							refuse(inst, "buffer ADD_TID is only valid for compute shaders");
						}
						requirements.subgroup_local_invocation_id = true;
					}
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
				if (shared_access == IR::SharedAccess::Atomic &&
				    IR::SharedComponentCount(inst.GetOpcode()) == 2u) {
					if (kind != IR::ResourceKind::Lds || program.stage != ShaderType::Compute) {
						refuse(inst, "64-bit shared atomics require compute LDS");
					}
					requirements.shared_int64_atomics = true;
				}
				if (program.stage != ShaderType::Compute && program.stage != ShaderType::Mesh &&
				    kind == IR::ResourceKind::Lds) {
					requirements.function_lds = true;
				}
				if (shared_access == IR::SharedAccess::Append ||
				    shared_access == IR::SharedAccess::Consume) {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
				}
			}
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::StoreCompletion: requirements.subgroup_barrier = true; break;
				case IR::ValueOpcode::BvhIntersect: requirements.bvh = true; break;
				case IR::ValueOpcode::Ballot: requirements.subgroup_ballot = true; break;
				case IR::ValueOpcode::DppMoveU32:
				case IR::ValueOpcode::ReadFirstLane:
				case IR::ValueOpcode::ReadLane: {
					requirements.subgroup_ballot  = true;
					requirements.subgroup_shuffle = true;
					if (inst.GetOpcode() == IR::ValueOpcode::DppMoveU32) {
						requirements.subgroup_local_invocation_id = true;
					}
					break;
				}
				case IR::ValueOpcode::DppUpdateU32:
				case IR::ValueOpcode::WriteLane: {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::Permlane16U32: {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::SwizzleU32:
				case IR::ValueOpcode::PermuteU32:
				case IR::ValueOpcode::BpermuteU32: {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::LaneId:
					requirements.subgroup_local_invocation_id |=
					    program.stage != ShaderType::TessellationControl;
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
		if (image.mip_count == 0u ||
		    (image.mip_mode != IR::ImageMipMode::Dynamic && image.mip_count != 1u)) {
			Fail(program, "native shader plan has an invalid image mip descriptor count");
		}
		auto& count = expected[static_cast<size_t>(*kind)];
		if (image.descriptor_index != count) {
			Fail(program, "native image descriptor index does not match shader topology");
		}
		count += image.mip_count;
	}
	expected[static_cast<size_t>(Kind::Samplers)] = static_cast<uint32_t>(program.info.samplers.size());
	expected[static_cast<size_t>(Kind::Gds)] = uses_gds;
	expected[static_cast<size_t>(Kind::SharedMemory)] = uses_lds && lds_storage;
	expected[static_cast<size_t>(Kind::BdaPagetable)] = program.info.uses_dma;
	expected[static_cast<size_t>(Kind::FaultBuffer)] = program.info.uses_dma;
	expected[static_cast<size_t>(Kind::FlattenedSrt)] = uses_flattened_srt;
	expected[static_cast<size_t>(Kind::ShaderData)] =
	    program.bindings.ShaderDataDwords() != 0 && !program.bindings.UsesPushData();
	uint64_t descriptor_mask = 0;
	for (uint32_t index = 0; index < expected.size(); ++index) {
		if (expected[index] != 0) descriptor_mask |= uint64_t{1} << index;
	}
	if (program.bindings.descriptor_counts != expected ||
	    program.bindings.descriptor_mask != descriptor_mask) {
		Fail(program, "native descriptor counts do not match shader topology");
	}
	const auto shader_data_dwords = program.bindings.ShaderDataDwords();
	const auto& registers = program.info.user_data_registers;
	const bool has_dispatch_threads = program.bindings.dispatch_thread_dword != IR::PushData::NoStart;
	if ((program.bindings.UsesPushData() &&
	     !IR::PushData::CanFit(program.bindings.push_data_start_dword, shader_data_dwords)) ||
	    (has_dispatch_threads && (program.stage != ShaderType::Compute ||
	                              program.bindings.dispatch_thread_dword != registers.size())) ||
	    program.bindings.memory_offset_dword != registers.size() + (has_dispatch_threads ? 3u : 0u) ||
	    !std::is_sorted(registers.begin(), registers.end()) ||
	    std::adjacent_find(registers.begin(), registers.end()) != registers.end()) {
		Fail(program, "native shader-data layout is inconsistent");
	}
}

std::vector<uint32_t> EmitProgram(const IR::Program& program, ShaderStageInputInfo input_info,
                                  std::string* refusal) {
	using namespace Emitter;

	if (program.stage != ShaderType::Compute && program.stage != ShaderType::Vertex &&
	    program.stage != ShaderType::Pixel && program.stage != ShaderType::Mesh &&
	    program.stage != ShaderType::Local && program.stage != ShaderType::TessellationControl &&
	    program.stage != ShaderType::TessellationEvaluation) {
		Fail(program, "binary SPIR-V emitter received an unsupported shader stage");
	}
	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	if (program.info.buffers.size() > IR::ShaderInfo::MaxBuffers) {
		Fail(program, "native shader exceeds the buffer resource limit");
	}
	IR::ValidateProgram(program, true);
	program.bindings = {.push_data_start_dword = push_data_start_dword};
	EmitterState state(program, input_info);
	const auto refused = [&]() -> std::vector<uint32_t> {
		const auto* inst = state.refused_inst;
		auto        text = fmt::format("SPIR-V emission refused: opcode={} reason={}",
		                               inst != nullptr ? IR::ValueOpcodeName(inst->GetOpcode())
		                                               : std::string_view {"none"},
		                               state.refusal);
		if (refusal == nullptr) {
			EXIT("%s hash=0x%016" PRIx64 " stage=%u\n", text.c_str(), program.shader_hash,
			     static_cast<unsigned>(program.stage));
		}
		*refusal = std::move(text);
		return {};
	};
	if (state.requirements.refusal != nullptr) {
		state.Refuse(state.requirements.refusal, state.requirements.refused_inst);
		return refused();
	}
	const auto* workgroup = ShaderWorkgroupInput(program.stage, input_info);
	state.lane_count =
	    workgroup != nullptr && program.wave_size == 64u && workgroup->host_subgroup_size == 32u
	        ? 2u
	        : 1u;
	DefineModule(state);
	if (!state.Refused()) {
		EmitProgram(state);
	}
	if (state.Refused()) {
		return refused();
	}
	state.builder.AddEntryPoint(ExecutionModelForStage(state.program.stage), state.main_func,
	                            "main", state.interface_variables);

	return state.builder.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
