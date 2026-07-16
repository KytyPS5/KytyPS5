#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitExportComponentF32(EmitterState* state, const IR::Instruction& inst,
                                uint32_t component) {
	const bool enabled = ((inst.export_info.en >> component) & 1u) != 0;
	if (!enabled || component >= inst.src_count || component >= 4u) {
		return ConstantF32(state, component == 3u ? 0x3f800000u : 0u);
	}
	return EmitFloatLoad(state, inst.src[component]);
}

uint32_t EmitExportVec4F32(EmitterState* state, const IR::Instruction& inst) {
	if (inst.export_info.compr) {
		uint32_t components[4] = {
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0x3f800000u),
		};
		for (uint32_t pair_index = 0; pair_index < 2u && pair_index < inst.src_count;
		     pair_index++) {
			const auto raw      = EmitValueLoad(state, inst.src[pair_index]);
			const auto unpacked = state->builder.AllocateId();
			state->builder.AddFunction({OpExtInst, state->vec2_float_type, unpacked,
			                            state->glsl_std450, GlslUnpackHalf2x16, raw});
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair_index * 2u + lane;
				if (((inst.export_info.en >> component) & 1u) == 0) {
					continue;
				}
				components[component] = state->builder.AllocateId();
				state->builder.AddFunction(
				    {OpCompositeExtract, state->float_type, components[component], unpacked, lane});
			}
		}
		const auto vec = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec,
		                            components[0], components[1], components[2], components[3]});
		return vec;
	}

	const auto x   = EmitExportComponentF32(state, inst, 0);
	const auto y   = EmitExportComponentF32(state, inst, 1);
	const auto z   = EmitExportComponentF32(state, inst, 2);
	const auto w   = EmitExportComponentF32(state, inst, 3);
	const auto vec = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, x, y, z, w});
	return vec;
}

bool ExportWritesData(const IR::Instruction& inst) {
	switch (inst.export_info.kind) {
		case IR::ExportTargetKind::Null:
		case IR::ExportTargetKind::Primitive: return false;
		case IR::ExportTargetKind::MrtZ: return (inst.export_info.en & 0x5u) != 0;
		default: return inst.export_info.en != 0;
	}
}

void EmitMrtZExport(EmitterState* state, const IR::Instruction& inst) {
	if ((inst.export_info.en & 0x1u) != 0 && state->depth_variable != 0) {
		state->builder.AddFunction(
		    {OpStore, state->depth_variable, EmitExportComponentF32(state, inst, 0)});
	}

	if ((inst.export_info.en & 0x4u) != 0 && state->sample_mask_variable != 0) {
		const auto raw =
		    inst.src_count > 2u ? EmitValueLoad(state, inst.src[2]) : ConstantU32(state, 0);
		const auto mask = state->builder.AllocateId();
		const auto ptr  = state->builder.AllocateId();
		state->builder.AddFunction({OpBitcast, state->int_type, mask, raw});
		state->builder.AddFunction({OpAccessChain, state->ptr_output_int, ptr,
		                            state->sample_mask_variable, ConstantU32(state, 0)});
		state->builder.AddFunction({OpStore, ptr, mask});
	}
}

void EmitExport(EmitterState* state, const IR::Instruction& inst) {
	if (inst.export_info.kind == IR::ExportTargetKind::Null ||
	    inst.export_info.kind == IR::ExportTargetKind::Primitive) {
		return;
	}

	if (!ExportWritesData(inst)) {
		return;
	}

	if (inst.export_info.kind == IR::ExportTargetKind::MrtZ) {
		EmitMrtZExport(state, inst);
		return;
	}

	const auto variable = OutputVariableForExport(*state, inst.export_info);
	if (variable == 0) {
		return;
	}

	const auto value = EmitExportVec4F32(state, inst);
	if (inst.export_info.kind == IR::ExportTargetKind::Position) {
		const auto pointer = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, pointer, variable,
		                            ConstantU32(state, 0)});
		state->builder.AddFunction({OpStore, pointer, value});
		return;
	}

	state->builder.AddFunction({OpStore, variable, value});
}

bool ExportUsesPixelValidMask(const EmitterState& state, const IR::Instruction& inst) {
	return state.stage == ShaderType::Pixel && inst.export_info.vm && state.needs_pixel_valid_mask;
}

void EmitKillIfBoolFalse(EmitterState* state, uint32_t active) {
	const auto kill_label  = state->builder.AllocateId();
	const auto merge_label = state->builder.AllocateId();
	const auto inactive    = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalNot, state->bool_type, inactive, active});
	state->builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state->builder.AddFunction({OpBranchConditional, inactive, kill_label, merge_label});
	state->builder.AddFunction({OpLabel, kill_label});
	state->builder.AddFunction({OpKill});
	state->builder.AddFunction({OpLabel, merge_label});
}

void EmitUpdatePixelValidMask(EmitterState* state) {
	if (state->pixel_valid_mask_variable == 0) {
		return;
	}

	const auto active = EmitExecActiveBool(state);
	const auto value  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, value, active, ConstantU32(state, 1), ConstantU32(state, 0)});
	state->builder.AddFunction({OpStore, state->pixel_valid_mask_variable, value});
}

void EmitKillIfPixelValidMaskInactive(EmitterState* state) {
	if (state->pixel_valid_mask_variable == 0) {
		return;
	}

	const auto mask_value = state->builder.AllocateId();
	const auto active     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpLoad, state->uint_type, mask_value, state->pixel_valid_mask_variable});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, active, mask_value, ConstantU32(state, 0)});
	EmitKillIfBoolFalse(state, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
