#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

[[noreturn]] void Fail(std::string_view message) {
	EXIT("shader info collection failed: %s", std::string(message).c_str());
	std::abort();
}

void AddInput(ShaderInfo& info, StageInputKind kind, uint32_t location, uint32_t components,
              std::string name, bool per_vertex = false) {
	const auto input = std::find_if(info.inputs.begin(), info.inputs.end(), [=](const auto& value) {
		return value.kind == kind && value.location == location;
	});
	if (input == info.inputs.end()) {
		info.inputs.push_back({kind, location, components, std::move(name), per_vertex});
	} else {
		input->component_count = std::max(input->component_count, components);
		input->per_vertex      = input->per_vertex || per_vertex;
	}
}

bool HasOutput(const ShaderInfo& info, StageOutputKind kind, uint32_t index) {
	return std::any_of(info.outputs.begin(), info.outputs.end(), [=](const auto& output) {
		return output.kind == kind && output.index == index;
	});
}

void AddOutput(ShaderInfo& info, StageOutputKind kind, uint32_t index, uint32_t location,
               std::string name) {
	if (!HasOutput(info, kind, index)) {
		info.outputs.push_back({kind, index, location, std::move(name)});
	}
}

void ValidateOptions(const Program& program, ShaderStageInputInfo input_info) {
	switch (program.stage) {
		case ShaderType::Vertex:
		case ShaderType::Local:
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation:
		case ShaderType::Mesh:
			if (input_info.vertex == nullptr) {
				return Fail("vertex shader has no input metadata");
			}
			if (input_info.vertex->resources_num < 0 ||
			    input_info.vertex->resources_num > ShaderVertexInputInfo::RES_MAX) {
				return Fail("vertex resource count is out of range");
			}
			return;
		case ShaderType::Pixel:
			if (input_info.pixel == nullptr) {
				return Fail("pixel shader has no input metadata");
			}
			if (input_info.pixel->input_num > std::size(input_info.pixel->interpolator_settings)) {
				return Fail("pixel input count is out of range");
			}
			return;
		case ShaderType::Compute:
			if (input_info.compute == nullptr) {
				return Fail("compute shader has no input metadata");
			}
			if (input_info.compute->thread_ids_num < 0 || input_info.compute->thread_ids_num > 3) {
				return Fail("compute thread ID count is out of range");
			}
			return;
		default: return Fail("unsupported shader stage for info collection");
	}
}


void CollectVertexInputs(const ShaderVertexInputInfo* vertex, ShaderInfo& info,
                         const std::array<uint8_t, 32>& used_components) {
	AddInput(info, StageInputKind::VertexIndex, 0, 1, "gl_VertexIndex");
	AddInput(info, StageInputKind::InstanceIndex, 0, 1, "gl_InstanceIndex");
	for (uint32_t attr = 0; attr < static_cast<uint32_t>(vertex->resources_num); attr++) {
		if (used_components[attr] != 0) {
			AddInput(info, StageInputKind::Parameter, attr, used_components[attr],
			         fmt::format("in_attr_{}", attr));
		}
	}
}

void CollectPixelInputs(const ShaderPixelInputInfo* pixel, ShaderInfo& info,
                        std::array<bool, 32>& per_vertex,
                        const std::array<uint8_t, 32>& used_components) {
	if (pixel->HasPositionInput()) {
		AddInput(info, StageInputKind::FragCoord, 0, 4, "gl_FragCoord");
	}
	if (pixel->ps_front_face) {
		AddInput(info, StageInputKind::FrontFacing, 0, 1, "gl_FrontFacing");
	}
	// Mixed interpolation modes share raw vertices at their guest export slot.
	// Rectangle expansion alone supplies separate flat and smooth outputs.
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		for (uint32_t alias = 0; alias < pixel->input_num; alias++) {
			const bool same_mode = ShaderPixelParameterIsFlat(*pixel, input) ==
			                       ShaderPixelParameterIsFlat(*pixel, alias);
			if (ShaderPixelParameterMappedLocation(*pixel, input) ==
			        ShaderPixelParameterMappedLocation(*pixel, alias) &&
			    (pixel->parameter_mode != ShaderPixelParameterMode::Rectangle || same_mode)) {
				per_vertex[input] = per_vertex[input] || per_vertex[alias] || !same_mode;
			}
		}
	}
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		AddInput(info, StageInputKind::Parameter, input, 4, fmt::format("in_param_{}", input),
		         per_vertex[input]);
	}
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		if (used_components[input] != 0 && per_vertex[input] &&
		    !ShaderPixelParameterIsFlat(*pixel, input)) {
			const auto kind = pixel->ps_no_perspective ? StageInputKind::BaryCoordNoPerspective
			                                           : StageInputKind::BaryCoordSmooth;
			AddInput(info, kind, 0, 3,
			         pixel->ps_no_perspective ? "gl_BaryCoordNoPerspKHR" : "gl_BaryCoordKHR");
			break;
		}
	}
}

void CollectComputeInputs(const ShaderComputeInputInfo* compute, ShaderInfo& info) {
	if (compute->group_id[0] || compute->group_id[1] || compute->group_id[2]) {
		AddInput(info, StageInputKind::WorkgroupId, 0, 3, "gl_WorkGroupID");
	}
	if (compute->thread_ids_num > 0) {
		AddInput(info, StageInputKind::LocalInvocationId, 0, 3, "gl_LocalInvocationID");
	}
	if (compute->thread_ids_num > 0 || compute->tg_size_en) {
		AddInput(info, StageInputKind::LocalInvocationIndex, 0, 1, "gl_LocalInvocationIndex");
	}
	if (compute->dispatch_thread_dimensions) {
		AddInput(info, StageInputKind::GlobalInvocationId, 0, 3, "gl_GlobalInvocationID");
	}
}

void AddBuiltinInput(ShaderInfo& info, StageInputKind kind) {
	switch (kind) {
		case StageInputKind::VertexIndex:
			AddInput(info, kind, 0, 1, "gl_VertexIndex");
			break;
		case StageInputKind::InstanceIndex:
			AddInput(info, kind, 0, 1, "gl_InstanceIndex");
			break;
		case StageInputKind::InvocationId:
			AddInput(info, kind, 0, 1, "gl_InvocationID");
			break;
		case StageInputKind::PrimitiveId:
			AddInput(info, kind, 0, 1, "gl_PrimitiveID");
			break;
		case StageInputKind::TessCoord: AddInput(info, kind, 0, 3, "gl_TessCoord"); break;
		case StageInputKind::FragCoord: AddInput(info, kind, 0, 4, "gl_FragCoord"); break;
		case StageInputKind::FrontFacing:
			AddInput(info, kind, 0, 1, "gl_FrontFacing");
			break;
		case StageInputKind::Layer: AddInput(info, kind, 0, 1, "gl_Layer"); break;
		case StageInputKind::SampleId: AddInput(info, kind, 0, 1, "gl_SampleID"); break;
		case StageInputKind::BaryCoordSmoothSample:
			AddInput(info, StageInputKind::SampleId, 0, 1, "gl_SampleID");
			[[fallthrough]];
		case StageInputKind::BaryCoordSmooth:
		case StageInputKind::BaryCoordSmoothCentroid:
			AddInput(info, StageInputKind::BaryCoordSmooth, 0, 3, "gl_BaryCoordKHR");
			break;
		case StageInputKind::BaryCoordNoPerspective:
			AddInput(info, kind, 0, 3, "gl_BaryCoordNoPerspKHR");
			break;
		case StageInputKind::WorkgroupId:
			AddInput(info, kind, 0, 3, "gl_WorkGroupID");
			break;
		case StageInputKind::NumWorkgroups: AddInput(info, kind, 0, 3, "gl_NumWorkGroups"); break;
		case StageInputKind::LocalInvocationId:
			AddInput(info, kind, 0, 3, "gl_LocalInvocationID");
			break;
		case StageInputKind::LocalInvocationIndex:
			AddInput(info, kind, 0, 1, "gl_LocalInvocationIndex");
			break;
		case StageInputKind::GlobalInvocationId:
			AddInput(info, kind, 0, 3, "gl_GlobalInvocationID");
			break;
		case StageInputKind::PackedAncillary:
		case StageInputKind::Parameter: break;
	}
}

void CollectOutput(const Program& program, ShaderStageInputInfo input_info,
                   ShaderInfo& info, const Inst& inst) {
	const bool alpha_remap = program.stage == ShaderType::Pixel && input_info.pixel != nullptr &&
	                         input_info.pixel->alpha_blend_source != ShaderAlphaBlendSource::None;
	const auto& export_info = program.export_info[inst.Flags<ExportFlags>().index];
	if (export_info.kind == ExportTargetKind::MrtZ) {
		if (program.stage == ShaderType::Pixel && (export_info.en & 0x1u) != 0 &&
		    input_info.pixel->ps_depth_export_enable) {
			AddOutput(info, StageOutputKind::Depth, 0, 0, "gl_FragDepth");
		}
		if (program.stage == ShaderType::Pixel && (export_info.en & 0x4u) != 0 &&
		    input_info.pixel->ps_sample_mask_export_enable) {
			AddOutput(info, StageOutputKind::SampleMask, 0, 0, "gl_SampleMask");
		}
		return;
	}
	if (export_info.en == 0) {
		return;
	}
	switch (export_info.kind) {
		case ExportTargetKind::Position:
			if (export_info.index == 0) {
				AddOutput(info, StageOutputKind::Position, 0, 0, "out_position");
				break;
			}
			if (export_info.compr) {
				return Fail("compressed auxiliary position export is unsupported");
			}
			for (uint32_t component = 0; component < 4; component++) {
				if ((export_info.en & (1u << component)) == 0) {
					continue;
				}
				const auto output = DecodePositionExportComponent(
				    input_info.vertex->pa_cl_vs_out_cntl, export_info.index, component);
				if (output.viewport) {
					AddOutput(info, StageOutputKind::ViewportIndex, 0, 0, "gl_ViewportIndex");
				}
				if (output.point_size) {
					AddOutput(info, StageOutputKind::PointSize, 0, 0, "gl_PointSize");
				}
				if (output.layer) {
					AddOutput(info, StageOutputKind::Layer, 0, 0, "gl_Layer");
				}
				if (output.clip_distance != UINT32_MAX) {
					AddOutput(info, StageOutputKind::ClipDistance, output.clip_distance, 0,
					          "gl_ClipDistance");
				}
				if (output.cull_distance != UINT32_MAX) {
					AddOutput(info, StageOutputKind::CullDistance, output.cull_distance, 0,
					          "gl_CullDistance");
				}
			}
			break;
		case ExportTargetKind::Parameter:
			AddOutput(info, StageOutputKind::Parameter, export_info.index,
			          export_info.index, fmt::format("out_param_{}", export_info.index));
			break;
		case ExportTargetKind::Mrt: {
			if (alpha_remap && export_info.index != 0) {
				break;
			}
			const auto slot = input_info.pixel->dual_source_blending
			                      ? 0u : ShaderPixelExportTarget(input_info.pixel->target_shader_mask,
			                                                    export_info.index);
			if (slot >= 8) {
				break;
			}
			AddOutput(info, StageOutputKind::Mrt, export_info.index, slot,
			          fmt::format("out_mrt_{}", export_info.index));
			if (alpha_remap) {
				AddOutput(info, StageOutputKind::Mrt, 1, 0, "out_mrt_1");
			}
			break;
		}
		default: break;
	}
}

struct InputUsage {
	std::array<uint8_t, 32> components {};
	std::array<bool, 32> per_vertex {};
	std::array<bool, NumScalarRegs> user_data {};
	std::array<StageInputKind, static_cast<size_t>(StageInputKind::Parameter)> builtins {};
	uint32_t builtin_mask = 0;
	uint32_t builtin_count = 0;

	void AddBuiltin(StageInputKind kind) {
		const auto bit = 1u << static_cast<uint32_t>(kind);
		if ((builtin_mask & bit) == 0) {
			builtin_mask |= bit;
			builtins[builtin_count++] = kind;
		}
	}
};

void Visit(Program& program, ShaderStageInputInfo input_info, InputUsage& inputs,
           const Inst& inst) {
	auto& info = program.info;
	const auto op = inst.GetOpcode();
	const auto buffer_access = BufferAccessOf(op);
	const auto address_access = AddressOpcodeInfoOf(op).access;
	const auto shared_access = SharedAccessOf(op);
	info.float64 |= inst.GetType() == Type::F64;
	info.buffer_int64_atomics |= buffer_access == BufferAccess::Atomic &&
	                             inst.GetType() == Type::U64;
	if (buffer_access != BufferAccess::None || address_access != AddressAccess::None ||
	    shared_access != SharedAccess::None) {
		const auto index = inst.Flags<MemoryFlags>().index;
		if (index >= program.memory_info.size()) {
			return Fail("typed shader contains invalid memory metadata");
		}
		const auto& memory = program.memory_info[index];
		const auto kind = memory.kind;
		if (!memory.planning_only) {
			info.uses_lds |= kind == ResourceKind::FlatLocal;
			if (shared_access != SharedAccess::None) {
				info.uses_lds |= kind == ResourceKind::Lds;
				info.uses_gds |= kind == ResourceKind::Gds;
			} else if (kind == ResourceKind::Buffer || kind == ResourceKind::ScalarBuffer) {
				if (memory.resource >= info.buffers.size() || memory.resource >= ShaderInfo::MaxBuffers) {
					return Fail("buffer operation has invalid resource metadata");
				}
				info.live_buffers |= uint64_t{1} << memory.resource;
				for (const auto child: info.buffers[memory.resource].indirect_resources) {
					if (child >= info.buffers.size()) {
						return Fail("indirect buffer operation has invalid resource metadata");
					}
					info.live_buffers |= uint64_t{1} << child;
				}
			}
		}
		if (address_access != AddressAccess::None) {
			if (kind == ResourceKind::Scratch || kind == ResourceKind::FlatLocal) {
				if (program.scratch_dwords == 0) {
					return Fail("scratch operation has no per-thread storage");
				}
				info.function_scratch = true;
				info.function_lds |= kind == ResourceKind::FlatLocal &&
				                     program.stage != ShaderType::Compute && program.stage != ShaderType::Mesh;
			} else if (address_access == AddressAccess::Write) {
				return Fail("writable FLAT/GLOBAL addresses require GPU ownership tracking");
			} else {
				info.coherent_buffers |= memory.coherent;
			}
		}
		if (buffer_access != BufferAccess::None) {
			if (kind == ResourceKind::IndirectBuffer && op != ValueOpcode::ReadConstBuffer) {
				info.subgroup_local_invocation_id = true;
			}
			if (kind == ResourceKind::Buffer) {
				info.coherent_buffers |= memory.coherent;
				if (memory.resource >= info.buffers.size()) {
					return Fail("buffer operation has invalid resource metadata");
				}
				const auto bits = StorageBufferElementBits(program, memory);
				info.buffer_u8 |= bits == 8u;
				info.buffer_u16 |= bits == 16u;
				if ((info.buffers[memory.resource].packed_stride & (1u << 20u)) != 0u) {
					if (program.stage != ShaderType::Compute) {
						return Fail("buffer ADD_TID is only valid for compute shaders");
					}
					info.subgroup_local_invocation_id = true;
				}
			}
		}
		if (shared_access != SharedAccess::None) {
			if (kind != ResourceKind::Lds && kind != ResourceKind::Gds) {
				return Fail("shared operation has invalid resource kind");
			}
			if (shared_access == SharedAccess::Atomic && SharedComponentCount(op) == 2u) {
				if (kind != ResourceKind::Lds || program.stage != ShaderType::Compute) {
					return Fail("64-bit shared atomics require compute LDS");
				}
				info.shared_int64_atomics = true;
			}
			info.function_lds |= kind == ResourceKind::Lds &&
			                     program.stage != ShaderType::Compute && program.stage != ShaderType::Mesh;
			if (shared_access == SharedAccess::Append || shared_access == SharedAccess::Consume) {
				info.subgroup_ballot = true;
				info.subgroup_shuffle = true;
				info.subgroup_local_invocation_id = true;
			}
		}
	}
	switch (inst.GetOpcode()) {
		case ValueOpcode::GetAttribute: {
			if (!inst.Arg(0).IsImmediate() || inst.Arg(0).GetType() != Type::U32 ||
			    !inst.Arg(1).IsImmediate() || inst.Arg(1).GetType() != Type::U32) {
				return Fail("typed attribute reference is not constant");
			}
			if ((program.stage == ShaderType::Vertex ||
			     program.stage == ShaderType::Local) &&
			    (inst.Arg(1).U32() >= 4u ||
			     inst.Arg(0).U32() >=
			         static_cast<uint32_t>(input_info.vertex->resources_num))) {
				return Fail("vertex input reference is out of range");
			}
			if (program.stage == ShaderType::Pixel &&
			    (inst.Arg(1).U32() >= 4u ||
			     inst.Arg(0).U32() >= input_info.pixel->input_num)) {
				return Fail("pixel input reference is out of range");
			}
			if (program.stage == ShaderType::Vertex || program.stage == ShaderType::Local ||
			    program.stage == ShaderType::Pixel) {
				auto& components = inputs.components[inst.Arg(0).U32()];
				components = std::max(components, static_cast<uint8_t>(inst.Arg(1).U32() + 1u));
			}
			break;
		}
		case ValueOpcode::GetInterpolationParameter: {
			if (program.stage != ShaderType::Pixel || !inst.Arg(0).IsImmediate() ||
			    inst.Arg(0).GetType() != Type::U32 || !inst.Arg(1).IsImmediate() ||
			    inst.Arg(1).GetType() != Type::U32 || !inst.Arg(2).IsImmediate() ||
			    inst.Arg(2).GetType() != Type::U32) {
				return Fail("interpolation parameter reference is invalid");
			}
			if (inst.Arg(0).U32() >= input_info.pixel->input_num ||
			    inst.Arg(1).U32() >= 4u || inst.Arg(2).U32() >= 3u) {
				return Fail("interpolation parameter reference is out of range");
			}
			const auto input = inst.Arg(0).U32();
			inputs.per_vertex[input] |= inst.Arg(2).U32() < 2u ||
			                            !ShaderPixelParameterIsFlat(*input_info.pixel, input);
			break;
		}
		case ValueOpcode::GetBuiltin: {
			if (!inst.Arg(0).IsImmediate() || inst.Arg(0).GetType() != Type::U32 ||
			    !inst.Arg(1).IsImmediate() || inst.Arg(1).GetType() != Type::U32) {
				return Fail("typed builtin reference is not constant");
			}
			const auto kind      = static_cast<StageInputKind>(inst.Arg(0).U32());
			const auto component = inst.Arg(1).U32();
			switch (kind) {
				case StageInputKind::PackedAncillary:
					return Fail("packed pixel ancillary input has an unsupported live use");
				case StageInputKind::Layer:
				case StageInputKind::SampleId:
					if (program.stage != ShaderType::Pixel || component != 0u) {
						return Fail("typed pixel scalar input is invalid");
					}
					break;
				case StageInputKind::VertexIndex:
				case StageInputKind::InvocationId:
				case StageInputKind::PrimitiveId:
				case StageInputKind::InstanceIndex:
				case StageInputKind::FrontFacing:
				case StageInputKind::LocalInvocationIndex:
					if (component != 0u) {
						return Fail("typed scalar builtin component is out of range");
					}
					break;
				case StageInputKind::FragCoord:
					if (component >= 4u) {
						return Fail("typed fragment-coordinate component is out of range");
					}
					break;
				case StageInputKind::BaryCoordSmooth:
				case StageInputKind::BaryCoordSmoothSample:
				case StageInputKind::BaryCoordSmoothCentroid:
				case StageInputKind::BaryCoordNoPerspective:
					if (component >= 2u) {
						return Fail("typed barycentric component is out of range");
					}
					break;
				case StageInputKind::TessCoord:
				case StageInputKind::WorkgroupId:
				case StageInputKind::NumWorkgroups:
				case StageInputKind::LocalInvocationId:
				case StageInputKind::GlobalInvocationId:
					if (component >= 3u) {
						return Fail("typed invocation builtin component is out of range");
					}
					break;
				case StageInputKind::Parameter:
				default: return Fail("typed builtin kind is invalid");
			}
			inputs.AddBuiltin(kind);
			break;
		}
		case ValueOpcode::SetAttribute: {
			const auto index = inst.Flags<ExportFlags>().index;
			if (index >= program.export_info.size()) {
				return Fail("typed export metadata index is out of range");
			}
			const auto& exp = program.export_info[index];
			if (exp.kind == ExportTargetKind::Position && exp.index != 0 && exp.en != 0 &&
			    program.stage != ShaderType::Vertex && program.stage != ShaderType::Mesh &&
			    program.stage != ShaderType::TessellationEvaluation) {
				return Fail("auxiliary position export requires a vertex, mesh, or "
				            "tessellation evaluation shader");
			}
			info.pixel_valid_mask |= program.stage == ShaderType::Pixel && exp.vm;
			CollectOutput(program, input_info, info, inst);
			break;
		}
		case ValueOpcode::GetUserData: {
			if (!inst.HasUses()) break;
			if (inst.Arg(0).GetType() != Type::ScalarReg) {
				return Fail("typed shader contains an invalid user-data register");
			}
			const auto index = RegIndex(inst.Arg(0).ScalarRegister());
			if (index >= NumScalarRegs) {
				return Fail("typed shader contains an invalid user-data register");
			}
			inputs.user_data[index] = true;
			break;
		}
		case ValueOpcode::GetDispatchThreadExtent:
			info.uses_dispatch_threads |= inst.HasUses();
			break;
		case ValueOpcode::ReadConst: info.uses_flattened_srt = true; break;
		case ValueOpcode::BitwiseXor32: info.has_bitwise_xor = true; break;
		case ValueOpcode::StoreCompletion: info.subgroup_barrier = true; break;
		case ValueOpcode::BvhIntersect: info.bvh = true; break;
		case ValueOpcode::ConditionRef:
			info.subgroup_ballot |=
			    inst.Flags<CFG::BranchCondition>() != CFG::BranchCondition::ScalarInstruction;
			break;
		case ValueOpcode::Ballot: info.subgroup_ballot = true; break;
		case ValueOpcode::DppMoveU32:
		case ValueOpcode::ReadFirstLane:
		case ValueOpcode::ReadLane: {
			info.subgroup_ballot  = true;
			info.subgroup_shuffle = true;
			if (inst.GetOpcode() == ValueOpcode::DppMoveU32) {
				info.subgroup_local_invocation_id = true;
			}
			break;
		}
		case ValueOpcode::DppUpdateU32:
		case ValueOpcode::WriteLane: {
			info.subgroup_ballot              = true;
			info.subgroup_local_invocation_id = true;
			break;
		}
		case ValueOpcode::SwizzleU32:
			info.uses_swizzle = true;
			[[fallthrough]];
		case ValueOpcode::Permlane16U32:
		case ValueOpcode::PermuteU32:
		case ValueOpcode::BpermuteU32: {
			info.subgroup_ballot              = true;
			info.subgroup_shuffle             = true;
			info.subgroup_local_invocation_id = true;
			break;
		}
		case ValueOpcode::LaneId:
			if (program.stage == ShaderType::TessellationControl) {
				inputs.AddBuiltin(StageInputKind::InvocationId);
			} else {
				info.subgroup_local_invocation_id = true;
			}
			break;
		case ValueOpcode::ImageQueryLod: info.compute_derivatives = true; break;
		case ValueOpcode::ImageGatherRaw:
			info.image_gather_extended = true;
			break;
		default: break;
	}
}

} // namespace

void CollectShaderInfo(Program& program, ShaderStageInputInfo input_info) {
	if (!program.resource_tracking_complete || program.shader_info_complete) {
		return Fail(!program.resource_tracking_complete ? "shader resources were not tracked"
		                                                : "shader info already collected");
	}
	ValidateOptions(program, input_info);
	auto& info = program.info;
	if (info.buffers.size() > ShaderInfo::MaxBuffers) {
		return Fail("buffer resource limit exceeded");
	}
	info.inputs.clear();
	info.outputs.clear();
	info.user_data_registers.clear();
	info.live_buffers = 0;
	info.has_bitwise_xor = info.uses_lds = info.uses_gds = info.uses_flattened_srt =
	    info.uses_dispatch_threads = info.uses_swizzle = false;
	info.bvh = info.subgroup_ballot = info.subgroup_barrier = info.subgroup_shuffle =
	    info.subgroup_local_invocation_id = info.compute_derivatives = info.image_gather_extended =
	    info.function_lds = info.function_scratch = info.pixel_valid_mask = info.buffer_int64_atomics =
	    info.buffer_u8 = info.buffer_u16 = info.shared_int64_atomics = info.coherent_buffers = info.float64 = false;
	InputUsage inputs;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			Visit(program, input_info, inputs, inst);
		}
	}
	for (uint32_t index = 0; index < inputs.user_data.size(); index++) {
		if (inputs.user_data[index]) info.user_data_registers.push_back(index);
	}
	const auto uses_mapping = [](const auto& resource) {
		return resource.indirect_root != UINT32_MAX;
	};
	info.uses_flattened_srt |= std::ranges::any_of(info.buffers, uses_mapping) ||
	                           std::ranges::any_of(info.images, uses_mapping);
	switch (program.stage) {
		case ShaderType::Vertex:
		case ShaderType::Local: CollectVertexInputs(input_info.vertex, info, inputs.components); break;
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation:
		case ShaderType::Mesh: break;
		case ShaderType::Pixel:
			CollectPixelInputs(input_info.pixel, info, inputs.per_vertex, inputs.components);
			break;
		case ShaderType::Compute: CollectComputeInputs(input_info.compute, info); break;
		default: return Fail("unsupported shader stage for info collection");
	}
	for (uint32_t i = 0; i < inputs.builtin_count; i++) {
		AddBuiltinInput(info, inputs.builtins[i]);
	}
	program.shader_info_complete = true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
