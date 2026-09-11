#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

bool UserDataDwordIndex(const EmitterState& state, IR::ScalarReg reg, uint32_t& dword_index) {
	const auto register_index = IR::RegIndex(reg);
	const auto& registers = state.program.bindings.user_data_registers;
	const auto  found     = std::lower_bound(registers.begin(), registers.end(), register_index);
	if (found == registers.end() || *found != register_index) {
		return false;
	}
	dword_index = static_cast<uint32_t>(found - registers.begin());
	return true;
}

uint32_t EmitBuiltinU32(EmitterState& state, IR::StageInputKind kind, uint32_t component) {
	if (kind == IR::StageInputKind::LocalInvocationIndex) {
		return EmitLocalInvocationIndex(state);
	}
	if (state.lane_count == 2 && (kind == IR::StageInputKind::LocalInvocationId ||
	                              kind == IR::StageInputKind::GlobalInvocationId)) {
		const auto* cs      = ShaderWorkgroupInput(state.program.stage, state.input_info);
		uint32_t    divisor = 1;
		for (uint32_t axis = 0; axis < component; axis++) {
			divisor *= std::max(cs->threads_num[axis], 1u);
		}
		const auto size    = std::max(cs->threads_num[component], 1u);
		const auto divided = EmitBinaryU32(state, spv::OpUDiv, EmitLocalInvocationIndex(state),
		                                   ConstantU32(state, divisor));
		const auto local   = EmitBinaryU32(state, spv::OpUMod, divided, ConstantU32(state, size));
		if (kind == IR::StageInputKind::LocalInvocationId) {
			return local;
		}
		const auto group = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, component);
		return EmitAddU32(state, local,
		                  EmitBinaryU32(state, spv::OpIMul, group, ConstantU32(state, size)));
	}
	const auto variable = InputVariableForKind(state, kind);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	if (kind == IR::StageInputKind::FrontFacing) {
		const auto value = state.builder.AllocateId();
		const auto bits  = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpLoad, TypeBool(state), value, variable});
		state.builder.AddFunction({spv::OpSelect, TypeU32(state), bits, value,
		                           ConstantU32(state, 1), ConstantU32(state, 0)});
		return bits;
	}
	if (kind == IR::StageInputKind::VertexIndex || kind == IR::StageInputKind::InstanceIndex ||
	    kind == IR::StageInputKind::Layer || kind == IR::StageInputKind::SampleId) {
		const auto value = state.builder.AllocateId();
		const auto bits  = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpLoad, TypeI32(state), value, variable});
		state.builder.AddFunction({spv::OpBitcast, TypeU32(state), bits, value});
		return bits;
	}
	if (kind == IR::StageInputKind::FragCoord) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		const auto bits    = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpAccessChain,
		                           TypePointer(state, spv::StorageClassInput, TypeF32(state)),
		                           pointer, variable, ConstantU32(state, component)});
		state.builder.AddFunction({spv::OpLoad, TypeF32(state), value, pointer});
		state.builder.AddFunction({spv::OpBitcast, TypeU32(state), bits, value});
		return bits;
	}
	if (kind == IR::StageInputKind::BaryCoordSmooth ||
	    kind == IR::StageInputKind::BaryCoordNoPerspective) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		const auto bits    = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpAccessChain,
		                           TypePointer(state, spv::StorageClassInput, TypeF32(state)),
		                           pointer, variable, ConstantU32(state, component + 1u)});
		state.builder.AddFunction({spv::OpLoad, TypeF32(state), value, pointer});
		state.builder.AddFunction({spv::OpBitcast, TypeU32(state), bits, value});
		return bits;
	}
	return EmitInputComponentU32(state, kind, component);
}

uint32_t EmitDppWriteCondition(ValueEmitContext& ctx, const IR::DppMoveFlags& flags,
                               uint32_t exec) {
	auto&      state      = ctx.state;
	const auto lane       = EmitSubgroupLocalInvocationId(state);
	const auto bank_shift = state.builder.AllocateId();
	const auto row_shift  = state.builder.AllocateId();
	const auto bank       = state.builder.AllocateId();
	const auto row        = state.builder.AllocateId();
	const auto bank_bit   = state.builder.AllocateId();
	const auto row_bit    = state.builder.AllocateId();
	const auto bank_hit   = state.builder.AllocateId();
	const auto row_hit    = state.builder.AllocateId();
	const auto bank_ok    = state.builder.AllocateId();
	const auto row_ok     = state.builder.AllocateId();
	const auto masks_ok   = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpShiftRightLogical, TypeU32(state), bank_shift, lane, ConstantU32(state, 2)});
	state.builder.AddFunction(
	    {spv::OpShiftRightLogical, TypeU32(state), row_shift, lane, ConstantU32(state, 4)});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), bank, bank_shift, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), row, row_shift, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {spv::OpShiftLeftLogical, TypeU32(state), bank_bit, ConstantU32(state, 1), bank});
	state.builder.AddFunction(
	    {spv::OpShiftLeftLogical, TypeU32(state), row_bit, ConstantU32(state, 1), row});
	state.builder.AddFunction({spv::OpBitwiseAnd, TypeU32(state), bank_hit,
	                           ConstantU32(state, flags.bank_mask), bank_bit});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), row_hit, ConstantU32(state, flags.row_mask), row_bit});
	state.builder.AddFunction(
	    {spv::OpINotEqual, TypeBool(state), bank_ok, bank_hit, ConstantU32(state, 0)});
	state.builder.AddFunction(
	    {spv::OpINotEqual, TypeBool(state), row_ok, row_hit, ConstantU32(state, 0)});
	state.builder.AddFunction({spv::OpLogicalAnd, TypeBool(state), masks_ok, bank_ok, row_ok});
	uint32_t writable = masks_ok;
	if (!flags.bound_control) {
		const auto target  = EmitDppTargetLane(state, flags.control);
		const auto bounded = state.builder.AllocateId();
		state.builder.AddFunction(
		    {spv::OpLogicalAnd, TypeBool(state), bounded, writable, target.valid});
		writable = bounded;
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpLogicalAnd, TypeBool(state), result, exec, writable});
	return result;
}

uint32_t EmitAttribute(EmitterState& state, uint32_t attr, uint32_t chan) {
	const auto* input = InputBindingForParameter(state, attr);
	if (input == nullptr || input->variable_id == 0) {
		return ConstantU32(state, 0);
	}
	if (state.program.stage == ShaderType::Vertex) {
		return EmitVertexParameterComponentU32(state, *input, chan & 3u);
	}
	const auto load_per_vertex = [&](uint32_t vertex) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpAccessChain,
		                           TypePointer(state, spv::StorageClassInput, TypeF32(state)),
		                           pointer, input->variable_id, ConstantU32(state, vertex),
		                           ConstantU32(state, chan & 3u)});
		state.builder.AddFunction({spv::OpLoad, TypeF32(state), value, pointer});
		return value;
	};
	if (input->per_vertex) {
		const auto barycentric_kind = state.input_info.pixel->ps_no_perspective
		                                  ? IR::StageInputKind::BaryCoordNoPerspective
		                                  : IR::StageInputKind::BaryCoordSmooth;
		const auto barycentric      = InputVariableForKind(state, barycentric_kind);
		uint32_t   sum              = 0;
		for (uint32_t vertex = 0; vertex < 3u; vertex++) {
			const auto pointer = state.builder.AllocateId();
			const auto weight  = state.builder.AllocateId();
			const auto product = state.builder.AllocateId();
			state.builder.AddFunction({spv::OpAccessChain,
			                           TypePointer(state, spv::StorageClassInput, TypeF32(state)),
			                           pointer, barycentric, ConstantU32(state, vertex)});
			state.builder.AddFunction({spv::OpLoad, TypeF32(state), weight, pointer});
			state.builder.AddFunction(
			    {spv::OpFMul, TypeF32(state), product, load_per_vertex(vertex), weight});
			if (vertex == 0u) {
				sum = product;
			} else {
				const auto next = state.builder.AllocateId();
				state.builder.AddFunction({spv::OpFAdd, TypeF32(state), next, sum, product});
				sum = next;
			}
		}
		const auto bits = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpBitcast, TypeU32(state), bits, sum});
		return bits;
	}
	const auto vector    = state.builder.AllocateId();
	const auto component = state.builder.AllocateId();
	const auto bits      = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpLoad, TypeF32Vector(state, 4), vector, input->variable_id});
	state.builder.AddFunction(
	    {spv::OpCompositeExtract, TypeF32(state), component, vector, chan & 3u});
	state.builder.AddFunction({spv::OpBitcast, TypeU32(state), bits, component});
	return bits;
}

uint32_t EmitInterpolationParameter(ValueEmitContext& ctx, uint32_t attr, uint32_t chan,
                                    uint32_t mode) {
	auto&       state = ctx.state;
	const auto* input = InputBindingForParameter(state, attr);
	if (!input->per_vertex) {
		return EmitAttribute(ctx.state, attr, chan);
	}
	const auto load_vertex = [&](uint32_t vertex) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpAccessChain,
		                           TypePointer(state, spv::StorageClassInput, TypeF32(state)),
		                           pointer, input->variable_id, ConstantU32(state, vertex),
		                           ConstantU32(state, chan & 3u)});
		state.builder.AddFunction({spv::OpLoad, TypeF32(state), value, pointer});
		return value;
	};

	const auto selected_vertex = (mode + 1u) % 3u;
	uint32_t   value           = load_vertex(selected_vertex);
	if (!PixelParameterIsCustom(state, attr) && mode < 2u) {
		const auto delta = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpFSub, TypeF32(state), delta, value, load_vertex(0)});
		value = delta;
	}
	const auto bits = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpBitcast, TypeU32(state), bits, value});
	return bits;
}

uint32_t MrtOutputMode(const EmitterState& state, const IR::ExportInfo& exp) {
	if (state.program.stage != ShaderType::Pixel || exp.kind != IR::ExportTargetKind::Mrt ||
	    exp.index >= std::size(state.input_info.pixel->target_output_mode)) {
		return 0;
	}
	return state.input_info.pixel->target_output_mode[exp.index];
}

uint32_t ExportRawComponent(ValueEmitContext& ctx, uint32_t vector, uint32_t component) {
	const auto value = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(
	    {spv::OpCompositeExtract, TypeU32(ctx.state), value, vector, component});
	return value;
}

uint32_t ExportVector(ValueEmitContext& ctx, uint32_t data, const IR::ExportInfo& exp,
                      bool uint_output) {
	auto& state = ctx.state;
	if (exp.compr && !uint_output) {
		const auto unpack =
		    MrtOutputMode(state, exp) == 5u ? GLSLstd450UnpackUnorm2x16 : GLSLstd450UnpackHalf2x16;
		uint32_t f32[4] = {ConstantF32(state, 0), ConstantF32(state, 0), ConstantF32(state, 0),
		                   ConstantF32(state, 0x3f800000u)};
		for (uint32_t pair = 0; pair < 2u; pair++) {
			if ((exp.en & (3u << (pair * 2u))) == 0u) {
				continue;
			}
			const auto packed   = state.builder.AllocateId();
			const auto unpacked = state.builder.AllocateId();
			state.builder.AddFunction(
			    {spv::OpCompositeExtract, TypeU32(state), packed, data, pair});
			state.builder.AddFunction({spv::OpExtInst, TypeF32Vector(state, 2), unpacked,
			                           GlslStd450(state), unpack, packed});
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair * 2u + lane;
				if (((exp.en >> component) & 1u) != 0u) {
					f32[component] = state.builder.AllocateId();
					state.builder.AddFunction(
					    {spv::OpCompositeExtract, TypeF32(state), f32[component], unpacked, lane});
				}
			}
		}
		const auto vector = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpCompositeConstruct, TypeF32Vector(state, 4), vector,
		                           f32[0], f32[1], f32[2], f32[3]});
		return vector;
	}
	uint32_t raw[4] = {
	    ConstantU32(state, 0),
	    ConstantU32(state, 0),
	    ConstantU32(state, 0),
	    ConstantU32(state, uint_output ? 1u : 0x3f800000u),
	};
	if (exp.compr) {
		for (uint32_t pair = 0; pair < 2u; pair++) {
			if ((exp.en & (3u << (pair * 2u))) == 0u) {
				continue;
			}
			const auto packed = ExportRawComponent(ctx, data, pair);
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair * 2u + lane;
				if (((exp.en >> component) & 1u) == 0u) {
					continue;
				}
				raw[component] = state.builder.AllocateId();
				state.builder.AddFunction({spv::OpBitFieldUExtract, TypeU32(state), raw[component],
				                           packed, ConstantU32(state, lane * 16u),
				                           ConstantU32(state, 16)});
			}
		}
	} else {
		for (uint32_t component = 0; component < 4u; component++) {
			if (((exp.en >> component) & 1u) != 0u) {
				raw[component] = ExportRawComponent(ctx, data, component);
			}
		}
	}
	if (uint_output) {
		const auto vector = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpCompositeConstruct, TypeU32Vector(state, 4), vector,
		                           raw[0], raw[1], raw[2], raw[3]});
		return vector;
	}
	uint32_t f32[4] {};
	for (uint32_t component = 0; component < 4u; component++) {
		f32[component] = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpBitcast, TypeF32(state), f32[component], raw[component]});
	}
	const auto vector = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpCompositeConstruct, TypeF32Vector(state, 4), vector, f32[0],
	                           f32[1], f32[2], f32[3]});
	return vector;
}

void EmitAuxPositionExport(ValueEmitContext& ctx, uint32_t data, const IR::ExportInfo& exp) {
	auto& state = ctx.state;
	for (uint32_t component = 0; component < 4; component++) {
		if ((exp.en & (1u << component)) == 0) {
			continue;
		}
		const auto output = IR::DecodePositionExportComponent(
		    state.input_info.vertex->pa_cl_vs_out_cntl, exp.index, component);
		if (output.layer || output.viewport) {
			const auto raw = ExportRawComponent(ctx, data, component);
			if (output.layer) {
				const auto layer = state.builder.AllocateId();
				state.builder.AddFunction(
				    {spv::OpBitwiseAnd, TypeU32(state), layer, raw, ConstantU32(state, 0x7ffu)});
				const auto pointer = state.program.stage == ShaderType::Mesh
				                         ? MeshOutputPointer(state, IR::StageOutputKind::Layer)
				                         : state.layer_variable;
				state.builder.AddFunction({spv::OpStore, pointer, layer});
			}
			if (output.viewport) {
				// GFX10 MISC.z packs the viewport index in bits 16..19 alongside the layer.
				const auto viewport = state.builder.AllocateId();
				state.builder.AddFunction({spv::OpBitFieldUExtract, TypeU32(state), viewport, raw,
				                           ConstantU32(state, 16), ConstantU32(state, 4)});
				state.builder.AddFunction({spv::OpStore, state.viewport_index_variable, viewport});
			}
			continue;
		}
		if (!output.point_size && output.clip_distance == UINT32_MAX &&
		    output.cull_distance == UINT32_MAX) {
			continue;
		}

		const auto raw = ExportRawComponent(ctx, data, component);
		const auto f32 = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpBitcast, TypeF32(state), f32, raw});
		if (output.point_size) {
			state.builder.AddFunction({spv::OpStore, state.point_size_variable, f32});
			continue;
		}
		auto StoreDistance = [&](uint32_t variable, uint32_t index) {
			if (index == UINT32_MAX) {
				return;
			}
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction({spv::OpAccessChain,
			                           TypePointer(state, spv::StorageClassOutput, TypeF32(state)),
			                           pointer, variable, ConstantU32(state, index)});
			state.builder.AddFunction({spv::OpStore, pointer, f32});
		};
		StoreDistance(state.clip_distance_variable, output.clip_distance);
		StoreDistance(state.cull_distance_variable, output.cull_distance);
	}
}

uint32_t ConvertClipCoordinate(EmitterState& state, uint32_t coordinate, float scale,
                               float offset, float half_extent) {
	const auto window  = state.builder.AllocateId();
	const auto biased  = state.builder.AllocateId();
	const auto divided = state.builder.AllocateId();
	const auto ndc     = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpFMul, TypeF32(state), window, coordinate, ConstantF32Value(state, scale)});
	state.builder.AddFunction(
	    {spv::OpFAdd, TypeF32(state), biased, window, ConstantF32Value(state, offset)});
	state.builder.AddFunction(
	    {spv::OpFDiv, TypeF32(state), divided, biased, ConstantF32Value(state, half_extent)});
	state.builder.AddFunction(
	    {spv::OpFSub, TypeF32(state), ndc, divided, ConstantF32Value(state, 1.0f)});
	return ndc;
}

uint32_t ConvertPositionToClipSpace(EmitterState& state, uint32_t position) {
	const auto& transform = state.input_info.vertex->clip_space;
	uint32_t    components[4] {};
	for (uint32_t i = 0; i < 4; i++) {
		components[i] = state.builder.AllocateId();
		state.builder.AddFunction(
		    {spv::OpCompositeExtract, TypeF32(state), components[i], position, i});
	}
	components[0] = ConvertClipCoordinate(state, components[0], transform.scale[0],
	                                      transform.offset[0], transform.half_extent[0]);
	components[1] = ConvertClipCoordinate(state, components[1], transform.scale[1],
	                                      transform.offset[1], transform.half_extent[1]);
	const auto converted = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpCompositeConstruct, TypeF32Vector(state, 4), converted,
	                           components[0], components[1], components[2], components[3]});
	return converted;
}

} // namespace
uint32_t EmitWqmU64(EmitterState& state, uint32_t value) {
	const auto shifted_one = state.builder.AllocateId();
	const auto merged_one  = state.builder.AllocateId();
	const auto shifted_two = state.builder.AllocateId();
	const auto merged_two  = state.builder.AllocateId();
	const auto quad_bits   = state.builder.AllocateId();
	const auto result      = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpShiftRightLogical, TypeU64(state), shifted_one, value,
	                           ConstantU64(state, 0x0000000100000001ull)});
	state.builder.AddFunction({spv::OpBitwiseOr, TypeU64(state), merged_one, value, shifted_one});
	state.builder.AddFunction({spv::OpShiftRightLogical, TypeU64(state), shifted_two, merged_one,
	                           ConstantU64(state, 0x0000000200000002ull)});
	state.builder.AddFunction(
	    {spv::OpBitwiseOr, TypeU64(state), merged_two, merged_one, shifted_two});
	state.builder.AddFunction({spv::OpBitwiseAnd, TypeU64(state), quad_bits, merged_two,
	                           ConstantU64(state, 0x1111111111111111ull)});
	state.builder.AddFunction({spv::OpIMul, TypeU64(state), result, quad_bits,
	                           ConstantU64(state, 0x0000000f0000000full)});
	return result;
}

void EmitSetAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state = ctx.state;
	const auto& exp   = ctx.Export(inst);
	const auto  exec  = ctx.Arg(inst, 1);
	if (state.program.stage == ShaderType::Pixel && exp.vm && state.requirements.pixel_valid_mask &&
	    state.pixel_valid_mask_variable != 0) {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpSelect, TypeU32(state), value, exec,
		                           ConstantU32(state, 1), ConstantU32(state, 0)});
		state.builder.AddFunction({spv::OpStore, state.pixel_valid_mask_variable, value});
	}
	if (exp.kind == IR::ExportTargetKind::Null || exp.en == 0u) {
		return;
	}
	EmitIfCondition(state, exec, [&]() {
		const auto data = ctx.Arg(inst, 0);
		if (exp.kind == IR::ExportTargetKind::Primitive) {
			if (state.program.stage == ShaderType::Mesh) {
				state.builder.AddFunction(
				    {spv::OpStore, MeshPrimitivePointer(state), ExportRawComponent(ctx, data, 0)});
			}
			return;
		}
		if (exp.kind == IR::ExportTargetKind::Position && exp.index != 0) {
			EmitAuxPositionExport(ctx, data, exp);
			return;
		}
		if (exp.kind == IR::ExportTargetKind::MrtZ) {
			if ((exp.en & 1u) != 0u && state.depth_variable != 0) {
				const auto raw = ExportRawComponent(ctx, data, 0);
				const auto f32 = state.builder.AllocateId();
				state.builder.AddFunction({spv::OpBitcast, TypeF32(state), f32, raw});
				state.builder.AddFunction({spv::OpStore, state.depth_variable, f32});
			}
			if ((exp.en & 4u) != 0u && state.sample_mask_variable != 0) {
				const auto raw     = ExportRawComponent(ctx, data, 2);
				const auto value   = state.builder.AllocateId();
				const auto pointer = state.builder.AllocateId();
				state.builder.AddFunction({spv::OpBitcast, TypeI32(state), value, raw});
				state.builder.AddFunction(
				    {spv::OpAccessChain,
				     TypePointer(state, spv::StorageClassOutput, TypeI32(state)), pointer,
				     state.sample_mask_variable, ConstantU32(state, 0)});
				state.builder.AddFunction({spv::OpStore, pointer, value});
			}
			return;
		}
		const auto variable =
		    state.program.stage == ShaderType::Mesh ? 0u : OutputVariableForExport(state, exp);
		if (state.program.stage != ShaderType::Mesh && variable == 0) {
			return;
		}
		const bool uint_output = MrtOutputMode(state, exp) == 7u;
		const auto vector_type = uint_output ? TypeU32Vector(state, 4) : TypeF32Vector(state, 4);
		auto       value       = ExportVector(ctx, data, exp, uint_output);
		if (state.program.stage == ShaderType::Pixel && exp.kind == IR::ExportTargetKind::Mrt &&
		    exp.index < state.input_info.pixel->target_export_mapping.size()) {
			const auto mapping = state.input_info.pixel->target_export_mapping[exp.index];
			if (!mapping.IsIdentity()) {
				const auto mapped = state.builder.AllocateId();
				state.builder.AddFunction({spv::OpVectorShuffle, vector_type, mapped, value, value,
				                           mapping.Map(0), mapping.Map(1), mapping.Map(2),
				                           mapping.Map(3)});
				value = mapped;
			}
		}
		if (exp.kind == IR::ExportTargetKind::Position &&
		    state.input_info.vertex->clip_space.enabled) {
			value = ConvertPositionToClipSpace(state, value);
		}
		if (state.program.stage == ShaderType::Mesh) {
			const auto kind = exp.kind == IR::ExportTargetKind::Position
			                      ? IR::StageOutputKind::Position
			                      : IR::StageOutputKind::Parameter;
			state.builder.AddFunction(
			    {spv::OpStore, MeshOutputPointer(state, kind, exp.index), value});
		} else if (exp.kind == IR::ExportTargetKind::Position) {
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction(
			    {spv::OpAccessChain,
			     TypePointer(state, spv::StorageClassOutput, TypeF32Vector(state, 4)), pointer,
			     variable, ConstantU32(state, 0)});
			state.builder.AddFunction({spv::OpStore, pointer, value});
		} else {
			state.builder.AddFunction({spv::OpStore, variable, value});
		}
	});
}

uint32_t EmitIdentity(ValueEmitContext&, uint32_t value) {
	return value;
}

void EmitVoid(ValueEmitContext&) {}

void EmitBarrier(EmitterState& state) {
	const auto semantics =
	    spv::MemorySemanticsAcquireReleaseMask | spv::MemorySemanticsWorkgroupMemoryMask;
	state.builder.AddFunction({spv::OpControlBarrier, ConstantU32(state, spv::ScopeWorkgroup),
	                           ConstantU32(state, spv::ScopeWorkgroup),
	                           ConstantU32(state, semantics)});
}

uint32_t EmitMeshDrawParameter(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state  = ctx.state;
	const auto result = state.builder.AllocateId();
	const auto index  = inst.Arg(0).U32();
	if (state.program.stage != ShaderType::Mesh || index >= IR::PushData::MeshDrawDwordCount) {
		ctx.Fail(inst, "invalid mesh draw parameter");
	}
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpAccessChain, TypePushConstantElementPointer(state), pointer,
	                           state.push_constant_variable, ConstantU32(state, 0),
	                           ConstantU32(state, index)});
	state.builder.AddFunction({spv::OpLoad, TypeU32(state), result, pointer});
	return result;
}

uint32_t EmitGetUserData(EmitterState& state, IR::ScalarReg reg) {

	uint32_t dword = 0;
	if (!UserDataDwordIndex(state, reg, dword)) {
		return ConstantU32(state, 0);
	} else {
		return EmitShaderDataDwordLoad(state, dword);
	}
}

uint32_t EmitGetBuiltin(ValueEmitContext& ctx, IR::Value kind, IR::Value index) {
	return EmitBuiltinU32(ctx.state, static_cast<IR::StageInputKind>(kind.U32()), index.U32());
}

uint32_t EmitUndefU1(EmitterState& state, const IR::Inst& inst) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpUndef, TypeId(state, inst.GetType()), result});
	return result;
}

uint32_t EmitDppMoveU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state    = ctx.state;
	const auto flags    = inst.Flags<IR::DppMoveFlags>();
	const auto target   = EmitDppTargetLane(state, flags.control);
	const auto shuffled = ctx.Shuffle(inst, 0, target.lane);
	if (flags.fetch_inactive) {
		return shuffled;
	}
	const auto ballot        = ctx.Ballot(inst.Arg(1));
	const auto source_active = EmitBallotLaneActiveBool(state, ballot, target.lane);
	const auto can_fetch     = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpLogicalAnd, TypeBool(state), can_fetch, target.valid, source_active});
	return EmitNative<spv::OpSelect, IR::Type::U32>(ctx.state, can_fetch, shuffled,
	                                                ConstantU32(state, 0));
}

uint32_t EmitDppUpdateU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto flags = inst.Flags<IR::DppMoveFlags>();
	const auto write = EmitDppWriteCondition(ctx, flags, ctx.Arg(inst, 2));
	return EmitNative<spv::OpSelect, IR::Type::U32>(ctx.state, write, ctx.Arg(inst, 0),
	                                                ctx.Arg(inst, 1));
}

uint32_t EmitBallot(ValueEmitContext& ctx, IR::Value predicate) {
	return ctx.Ballot(predicate);
}

uint32_t EmitReadFirstLane(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto ballot = ctx.Ballot(inst.Arg(1));
	const auto lane   = ctx.FirstLane(ballot);
	return ctx.Shuffle(inst, 0, lane);
}

uint32_t EmitReadLane(ValueEmitContext& ctx, const IR::Inst& inst) {
	return ctx.Shuffle(inst, 0, ctx.Arg(inst, 1));
}

uint32_t EmitWriteLane(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto hit   = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpIEqual, TypeBool(state), hit,
	                           EmitSubgroupLocalInvocationId(state), ctx.Arg(inst, 2)});
	return EmitNative<spv::OpSelect, IR::Type::U32>(ctx.state, hit, ctx.Arg(inst, 1),
	                                                ctx.Arg(inst, 0));
}

uint32_t EmitPermlane16U32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state     = ctx.state;
	const auto flags     = inst.Flags<IR::PermlaneFlags>();
	const auto subid     = EmitSubgroupLocalInvocationId(state);
	const auto row       = state.builder.AllocateId();
	const auto row_value = state.builder.AllocateId();
	const auto lane      = state.builder.AllocateId();
	const auto lane8     = state.builder.AllocateId();
	const auto shift     = state.builder.AllocateId();
	const auto upper     = state.builder.AllocateId();
	const auto selected  = state.builder.AllocateId();
	const auto shifted   = state.builder.AllocateId();
	const auto index     = state.builder.AllocateId();
	const auto target    = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), row, subid, ConstantU32(state, 0xfffffff0u)});
	if (flags.x16) {
		state.builder.AddFunction(
		    {spv::OpBitwiseXor, TypeU32(state), row_value, row, ConstantU32(state, 16)});
	} else {
		state.builder.AddFunction({spv::OpCopyObject, TypeU32(state), row_value, row});
	}
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), lane, subid, ConstantU32(state, 15)});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), lane8, lane, ConstantU32(state, 7)});
	state.builder.AddFunction(
	    {spv::OpShiftLeftLogical, TypeU32(state), shift, lane8, ConstantU32(state, 2)});
	state.builder.AddFunction(
	    {spv::OpUGreaterThanEqual, TypeBool(state), upper, lane, ConstantU32(state, 8)});
	state.builder.AddFunction(
	    {spv::OpSelect, TypeU32(state), selected, upper, ctx.Arg(inst, 2), ctx.Arg(inst, 1)});
	state.builder.AddFunction({spv::OpShiftRightLogical, TypeU32(state), shifted, selected, shift});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), index, shifted, ConstantU32(state, 15)});
	state.builder.AddFunction({spv::OpBitwiseOr, TypeU32(state), target, row_value, index});
	const auto shuffled = ctx.Shuffle(inst, 0, target);
	uint32_t   result   = shuffled;
	if (!flags.fetch_inactive) {
		const auto source_exec = ctx.Shuffle(inst, 3, target);
		result                 = state.builder.AllocateId();
		state.builder.AddFunction(
		    {spv::OpSelect, TypeU32(state), result, source_exec, shuffled, ConstantU32(state, 0)});
	}
	return result;
}

uint32_t EmitGetAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitAttribute(ctx.state, inst.Arg(0).U32(), inst.Arg(1).U32());
}

uint32_t EmitGetInterpolationParameter(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitInterpolationParameter(ctx, inst.Arg(0).U32(), inst.Arg(1).U32(), inst.Arg(2).U32());
}

uint32_t EmitGetShaderBase(ValueEmitContext& ctx) {
	// Guest S_GETPC values stay shader-relative in SPIR-V, matching the runtime ABI. The
	// runtime descriptor evaluator supplies the mapped shader base for host-side planning.
	return ctx.Def(IR::Value(uint64_t {0}));
}

void EmitUnreachable(ValueEmitContext& ctx, const IR::Inst& inst) {
	ctx.Fail(inst, "must be lowered before SPIR-V emission");
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
