#include "graphics/shader/recompiler/ShaderInfoCollection.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

bool HasInput(const ShaderInfo& info, StageInputKind kind, uint32_t location) {
	return std::any_of(info.inputs.begin(), info.inputs.end(), [=](const auto& input) {
		return input.kind == kind && input.location == location;
	});
}

void AddInput(ShaderInfo* info, StageInputKind kind, uint32_t location, uint32_t components,
              std::string name) {
	if (!HasInput(*info, kind, location)) {
		info->inputs.push_back({kind, location, components, std::move(name)});
	}
}

bool HasOutput(const ShaderInfo& info, StageOutputKind kind, uint32_t index) {
	return std::any_of(info.outputs.begin(), info.outputs.end(), [=](const auto& output) {
		return output.kind == kind && output.index == index;
	});
}

void AddOutput(ShaderInfo* info, StageOutputKind kind, uint32_t index, uint32_t location,
               std::string name) {
	if (!HasOutput(*info, kind, index)) {
		info->outputs.push_back({kind, index, location, std::move(name)});
	}
}

bool NeedsLocalInvocationIndex(const Program& program) {
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == Opcode::DsWriteAddtidB32 || inst.op == Opcode::DsReadAddtidB32) {
				return true;
			}
		}
	}
	return false;
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
			for (const auto& block: program.blocks) {
				for (const auto& inst: block.instructions) {
					if (inst.op == Opcode::LoadInputF32 &&
					    (inst.input_info.chan >= 4 ||
					     inst.input_info.attr >=
					         static_cast<uint32_t>(options.vertex->resources_num))) {
						return Fail("vertex input reference is out of range");
					}
				}
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

void CollectVertexInputs(const Program& program, const ShaderVertexInputInfo* vertex,
                         ShaderInfo* info) {
	AddInput(info, StageInputKind::VertexIndex, 0, 1, "gl_VertexIndex");
	AddInput(info, StageInputKind::InstanceIndex, 0, 1, "gl_InstanceIndex");
	if (vertex == nullptr) {
		return;
	}
	uint32_t used_components[ShaderVertexInputInfo::RES_MAX] = {};
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == Opcode::LoadInputF32 &&
			    inst.input_info.attr < static_cast<uint32_t>(vertex->resources_num) &&
			    inst.input_info.attr < ShaderVertexInputInfo::RES_MAX) {
				used_components[inst.input_info.attr] =
				    std::max(used_components[inst.input_info.attr], inst.input_info.chan + 1u);
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

void CollectPixelInputs(const ShaderPixelInputInfo* pixel, ShaderInfo* info) {
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

void CollectComputeInputs(const Program& program, const ShaderComputeInputInfo* compute,
                          ShaderInfo* info) {
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
	if (NeedsLocalInvocationIndex(program)) {
		AddInput(info, StageInputKind::LocalInvocationIndex, 0, 1, "gl_LocalInvocationIndex");
	}
}

void CollectOutputs(const Program& program, const ShaderPixelInputInfo* pixel, ShaderInfo* info) {
	for (const auto& block: program.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op != Opcode::Export) {
				continue;
			}
			if (inst.export_info.kind == ExportTargetKind::MrtZ) {
				if (pixel != nullptr && (inst.export_info.en & 0x1u) != 0 &&
				    pixel->ps_depth_export_enable) {
					AddOutput(info, StageOutputKind::Depth, 0, 0, "gl_FragDepth");
				}
				if (pixel != nullptr && (inst.export_info.en & 0x4u) != 0 &&
				    pixel->ps_sample_mask_export_enable) {
					AddOutput(info, StageOutputKind::SampleMask, 0, 0, "gl_SampleMask");
				}
				continue;
			}
			if (inst.export_info.en == 0) {
				continue;
			}
			switch (inst.export_info.kind) {
				case ExportTargetKind::Position:
					AddOutput(info, StageOutputKind::Position, inst.export_info.index, 0,
					          "out_position");
					break;
				case ExportTargetKind::Parameter:
					AddOutput(info, StageOutputKind::Parameter, inst.export_info.index,
					          inst.export_info.index,
					          fmt::format("out_param_{}", inst.export_info.index));
					break;
				case ExportTargetKind::Mrt:
					AddOutput(info, StageOutputKind::Mrt, inst.export_info.index,
					          inst.export_info.index,
					          fmt::format("out_mrt_{}", inst.export_info.index));
					break;
				default: break;
			}
		}
	}
}

} // namespace

bool CollectShaderInfo(Program* program, const ShaderInfoOptions& options, std::string* error) {
	if (program == nullptr || !program->resource_tracking_complete ||
	    program->shader_info_complete) {
		if (error != nullptr) {
			*error = program == nullptr                     ? "invalid shader info program"
			         : !program->resource_tracking_complete ? "shader resources were not tracked"
			                                                : "shader info already collected";
		}
		return false;
	}
	if (!ValidateOptions(*program, options, error)) {
		return false;
	}

	auto next = program->info;
	next.inputs.clear();
	next.outputs.clear();
	switch (program->stage) {
		case ShaderType::Vertex: CollectVertexInputs(*program, options.vertex, &next); break;
		case ShaderType::Pixel: CollectPixelInputs(options.pixel, &next); break;
		case ShaderType::Compute: CollectComputeInputs(*program, options.compute, &next); break;
		default: return false;
	}
	CollectOutputs(*program, options.pixel, &next);
	program->info                 = std::move(next);
	program->shader_info_complete = true;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
