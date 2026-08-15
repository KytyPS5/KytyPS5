#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"

#include "graphics/shader/recompiler/ir/ValueProgram.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

void AddInput(ShaderInfo& info, StageInputKind kind, uint32_t location, uint32_t components,
              std::string name) {
	const auto input = std::find_if(info.inputs.begin(), info.inputs.end(), [=](const auto& value) {
		return value.kind == kind && value.location == location;
	});
	if (input == info.inputs.end()) {
		info.inputs.push_back({kind, location, components, std::move(name)});
	} else {
		input->component_count = std::max(input->component_count, components);
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

bool ValidateOptions(const Program& program, const ShaderInfoOptions& options, std::string* error) {
	auto Fail = [&](const char* message) {
		if (error != nullptr) {
			*error = message;
		}
		return false;
	};
	switch (program.stage) {
		case ShaderType::Vertex:
			if (options.vertex == nullptr) {
				return Fail("vertex shader info requires vertex metadata");
			}
			if (options.vertex->resources_num < 0 ||
			    options.vertex->resources_num > ShaderVertexInputInfo::RES_MAX) {
				return Fail("vertex resource count is out of range");
			}
			return true;
		case ShaderType::Pixel:
			if (options.pixel == nullptr) {
				return Fail("pixel shader info requires pixel metadata");
			}
			return options.pixel->input_num <= std::size(options.pixel->interpolator_settings) ||
			       Fail("pixel input count is out of range");
		case ShaderType::Compute:
			if (options.compute == nullptr) {
				return Fail("compute shader info requires compute metadata");
			}
			return (options.compute->thread_ids_num >= 0 && options.compute->thread_ids_num <= 3) ||
			       Fail("compute thread ID count is out of range");
		default: return Fail("unsupported shader stage for info collection");
	}
}

bool ValidateValueReferences(const Program& program, const ShaderInfoOptions& options,
                             std::string* error) {
	const auto Fail = [&](const char* message) {
		if (error != nullptr) {
			*error = message;
		}
		return false;
	};
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			switch (inst.GetOpcode()) {
				case ValueOpcode::GetAttribute: {
					if (!inst.Arg(0).IsImmediate() || inst.Arg(0).GetType() != Type::U32 ||
					    !inst.Arg(1).IsImmediate() || inst.Arg(1).GetType() != Type::U32) {
						return Fail("typed attribute reference is not constant");
					}
					if (program.stage == ShaderType::Vertex &&
					    (inst.Arg(1).U32() >= 4u || options.vertex == nullptr ||
					     inst.Arg(0).U32() >=
					         static_cast<uint32_t>(options.vertex->resources_num))) {
						return Fail("vertex input reference is out of range");
					}
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
						case StageInputKind::VertexIndex:
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
						case StageInputKind::WorkgroupId:
						case StageInputKind::LocalInvocationId:
						case StageInputKind::GlobalInvocationId:
							if (component >= 3u) {
								return Fail("typed invocation builtin component is out of range");
							}
							break;
						case StageInputKind::Parameter:
						default: return Fail("typed builtin kind is invalid");
					}
					break;
				}
				case ValueOpcode::SetAttribute:
					if (inst.Flags<ExportFlags>().index >= program.values->export_info.size()) {
						return Fail("typed export metadata index is out of range");
					}
					break;
				default: break;
			}
		}
	}
	return true;
}

void CollectVertexInputs(const Program& program, const ShaderVertexInputInfo* vertex,
                         ShaderInfo& info) {
	AddInput(info, StageInputKind::VertexIndex, 0, 1, "gl_VertexIndex");
	AddInput(info, StageInputKind::InstanceIndex, 0, 1, "gl_InstanceIndex");
	if (vertex == nullptr) {
		return;
	}
	uint32_t used_components[ShaderVertexInputInfo::RES_MAX] = {};
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::GetAttribute) {
				const auto attr       = inst.Arg(0).U32();
				const auto chan       = inst.Arg(1).U32();
				used_components[attr] = std::max(used_components[attr], chan + 1u);
			}
		}
	}
	for (uint32_t attr = 0; attr < static_cast<uint32_t>(vertex->resources_num) &&
	                        attr < ShaderVertexInputInfo::RES_MAX;
	     attr++) {
		if (used_components[attr] != 0) {
			AddInput(info, StageInputKind::Parameter, attr, used_components[attr],
			         fmt::format("in_attr_{}", attr));
		}
	}
}

void CollectPixelInputs(const ShaderPixelInputInfo* pixel, ShaderInfo& info) {
	if (pixel == nullptr) {
		return;
	}
	if (pixel->HasPositionInput()) {
		AddInput(info, StageInputKind::FragCoord, 0, 4, "gl_FragCoord");
	}
	if (pixel->ps_front_face) {
		AddInput(info, StageInputKind::FrontFacing, 0, 1, "gl_FrontFacing");
	}
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		AddInput(info, StageInputKind::Parameter, input, 4, fmt::format("in_param_{}", input));
	}
}

void CollectComputeInputs(const ShaderComputeInputInfo* compute, ShaderInfo& info) {
	if (compute != nullptr) {
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
}

void CollectBuiltinInputs(const Program& program, ShaderInfo& info) {
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::GetBuiltin) {
				continue;
			}
			const auto kind = static_cast<StageInputKind>(inst.Arg(0).U32());
			switch (kind) {
				case StageInputKind::VertexIndex:
					AddInput(info, kind, 0, 1, "gl_VertexIndex");
					break;
				case StageInputKind::InstanceIndex:
					AddInput(info, kind, 0, 1, "gl_InstanceIndex");
					break;
				case StageInputKind::FragCoord: AddInput(info, kind, 0, 4, "gl_FragCoord"); break;
				case StageInputKind::FrontFacing:
					AddInput(info, kind, 0, 1, "gl_FrontFacing");
					break;
				case StageInputKind::WorkgroupId:
					AddInput(info, kind, 0, 3, "gl_WorkGroupID");
					break;
				case StageInputKind::LocalInvocationId:
					AddInput(info, kind, 0, 3, "gl_LocalInvocationID");
					break;
				case StageInputKind::LocalInvocationIndex:
					AddInput(info, kind, 0, 1, "gl_LocalInvocationIndex");
					break;
				case StageInputKind::GlobalInvocationId:
					AddInput(info, kind, 0, 3, "gl_GlobalInvocationID");
					break;
				case StageInputKind::Parameter: break;
			}
		}
	}
}

void CollectOutputs(const Program& program, const ShaderPixelInputInfo* pixel, ShaderInfo& info) {
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::SetAttribute) {
				continue;
			}
			const auto& export_info = program.values->export_info[inst.Flags<ExportFlags>().index];
			if (export_info.kind == ExportTargetKind::MrtZ) {
				if (pixel != nullptr && (export_info.en & 0x1u) != 0 &&
				    pixel->ps_depth_export_enable) {
					AddOutput(info, StageOutputKind::Depth, 0, 0, "gl_FragDepth");
				}
				if (pixel != nullptr && (export_info.en & 0x4u) != 0 &&
				    pixel->ps_sample_mask_export_enable) {
					AddOutput(info, StageOutputKind::SampleMask, 0, 0, "gl_SampleMask");
				}
				continue;
			}
			if (export_info.en == 0) {
				continue;
			}
			switch (export_info.kind) {
				case ExportTargetKind::Position:
					AddOutput(info, StageOutputKind::Position, export_info.index, 0,
					          "out_position");
					break;
				case ExportTargetKind::Parameter:
					AddOutput(info, StageOutputKind::Parameter, export_info.index,
					          export_info.index, fmt::format("out_param_{}", export_info.index));
					break;
				case ExportTargetKind::Mrt:
					AddOutput(info, StageOutputKind::Mrt, export_info.index, export_info.index,
					          fmt::format("out_mrt_{}", export_info.index));
					break;
				default: break;
			}
		}
	}
}

} // namespace

bool CollectShaderInfo(Program& program, const ShaderInfoOptions& options, std::string* error) {
	if (!program.resource_tracking_complete || program.shader_info_complete) {
		if (error != nullptr) {
			*error = !program.resource_tracking_complete ? "shader resources were not tracked"
			                                             : "shader info already collected";
		}
		return false;
	}
	if (program.values == nullptr) {
		if (error != nullptr) {
			*error = "typed value program is not available";
		}
		return false;
	}
	if (!ValidateOptions(program, options, error)) {
		return false;
	}
	if (!ValidateValueReferences(program, options, error)) {
		return false;
	}

	auto next = program.info;
	next.inputs.clear();
	next.outputs.clear();
	next.has_bitwise_xor = std::any_of(
	    program.values->blocks.begin(), program.values->blocks.end(), [](const auto* block) {
		    return std::any_of(block->begin(), block->end(), [](const auto& inst) {
			    return inst.GetOpcode() == ValueOpcode::BitwiseXor32;
		    });
	    });
	switch (program.stage) {
		case ShaderType::Vertex: CollectVertexInputs(program, options.vertex, next); break;
		case ShaderType::Pixel: CollectPixelInputs(options.pixel, next); break;
		case ShaderType::Compute: CollectComputeInputs(options.compute, next); break;
		default: return false;
	}
	CollectBuiltinInputs(program, next);
	CollectOutputs(program, options.pixel, next);
	program.info                 = std::move(next);
	program.shader_info_complete = true;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
