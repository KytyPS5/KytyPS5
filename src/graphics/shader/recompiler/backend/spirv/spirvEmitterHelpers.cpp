#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

DppTargetLane EmitDppQuadPermTargetLane(EmitterState& state, uint32_t subid, uint32_t control) {
	const auto quad_base = state.builder.AllocateId();
	const auto lane      = state.builder.AllocateId();
	const auto shift     = state.builder.AllocateId();
	const auto selected0 = state.builder.AllocateId();
	const auto selected  = state.builder.AllocateId();
	const auto target    = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), quad_base, subid, ConstantU32(state, 0xfffffffcu)});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), lane, subid, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {spv::OpShiftLeftLogical, TypeU32(state), shift, lane, ConstantU32(state, 1)});
	state.builder.AddFunction(
	    {spv::OpShiftRightLogical, TypeU32(state), selected0, ConstantU32(state, control), shift});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), selected, selected0, ConstantU32(state, 3)});
	state.builder.AddFunction({spv::OpBitwiseOr, TypeU32(state), target, quad_base, selected});
	return {target, ConstantBool(state, true)};
}

DppTargetLane EmitDppRowShiftTargetLane(EmitterState& state, uint32_t subid, uint32_t amount,
                                        bool left) {
	const auto row          = state.builder.AllocateId();
	const auto lane         = state.builder.AllocateId();
	const auto lane_shifted = state.builder.AllocateId();
	const auto target       = state.builder.AllocateId();
	const auto valid        = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), row, subid, ConstantU32(state, 0xfffffff0u)});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), lane, subid, ConstantU32(state, 15)});
	if (left) {
		state.builder.AddFunction(
		    {spv::OpIAdd, TypeU32(state), lane_shifted, lane, ConstantU32(state, amount)});
		state.builder.AddFunction(
		    {spv::OpULessThan, TypeBool(state), valid, lane, ConstantU32(state, 16u - amount)});
	} else {
		state.builder.AddFunction(
		    {spv::OpISub, TypeU32(state), lane_shifted, lane, ConstantU32(state, amount)});
		state.builder.AddFunction(
		    {spv::OpUGreaterThanEqual, TypeBool(state), valid, lane, ConstantU32(state, amount)});
	}
	state.builder.AddFunction({spv::OpBitwiseOr, TypeU32(state), target, row, lane_shifted});
	return {target, valid};
}

DppTargetLane EmitDppRowRotateRightTargetLane(EmitterState& state, uint32_t subid,
                                              uint32_t amount) {
	const auto row      = state.builder.AllocateId();
	const auto lane     = state.builder.AllocateId();
	const auto in_high  = state.builder.AllocateId();
	const auto minus    = state.builder.AllocateId();
	const auto plus     = state.builder.AllocateId();
	const auto selected = state.builder.AllocateId();
	const auto target   = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), row, subid, ConstantU32(state, 0xfffffff0u)});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), lane, subid, ConstantU32(state, 15)});
	state.builder.AddFunction(
	    {spv::OpUGreaterThanEqual, TypeBool(state), in_high, lane, ConstantU32(state, amount)});
	state.builder.AddFunction(
	    {spv::OpISub, TypeU32(state), minus, lane, ConstantU32(state, amount)});
	state.builder.AddFunction(
	    {spv::OpIAdd, TypeU32(state), plus, lane, ConstantU32(state, 16u - amount)});
	state.builder.AddFunction({spv::OpSelect, TypeU32(state), selected, in_high, minus, plus});
	state.builder.AddFunction({spv::OpBitwiseOr, TypeU32(state), target, row, selected});
	return {target, ConstantBool(state, true)};
}

DppTargetLane EmitDppMirrorTargetLane(EmitterState& state, uint32_t subid, bool half_row) {
	const auto base_mask = half_row ? 0xfffffff8u : 0xfffffff0u;
	const auto lane_mask = half_row ? 7u : 15u;
	const auto base      = state.builder.AllocateId();
	const auto lane      = state.builder.AllocateId();
	const auto mirrored  = state.builder.AllocateId();
	const auto target    = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), base, subid, ConstantU32(state, base_mask)});
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), lane, subid, ConstantU32(state, lane_mask)});
	state.builder.AddFunction(
	    {spv::OpISub, TypeU32(state), mirrored, ConstantU32(state, lane_mask), lane});
	state.builder.AddFunction({spv::OpBitwiseOr, TypeU32(state), target, base, mirrored});
	return {target, ConstantBool(state, true)};
}

DppTargetLane EmitDppTargetLane(EmitterState& state, uint32_t control) {
	const auto subid = EmitSubgroupLocalInvocationId(state);
	if (control <= 0xffu) {
		return EmitDppQuadPermTargetLane(state, subid, control);
	}
	if (control >= 0x101u && control <= 0x10fu) {
		return EmitDppRowShiftTargetLane(state, subid, control & 0xfu, true);
	}
	if (control >= 0x111u && control <= 0x11fu) {
		return EmitDppRowShiftTargetLane(state, subid, control & 0xfu, false);
	}
	if (control >= 0x121u && control <= 0x12fu) {
		return EmitDppRowRotateRightTargetLane(state, subid, control & 0xfu);
	}
	if (control == 0x140u) {
		return EmitDppMirrorTargetLane(state, subid, false);
	}
	if (control == 0x141u) {
		return EmitDppMirrorTargetLane(state, subid, true);
	}
	if (control >= 0x160u && control <= 0x16fu) {
		const auto target = state.builder.AllocateId();
		state.builder.AddFunction(
		    {spv::OpBitwiseXor, TypeU32(state), target, subid, ConstantU32(state, control & 0xfu)});
		return {target, ConstantBool(state, true)};
	}
	return {subid, ConstantBool(state, true)};
}

uint32_t EmitSubgroupLocalInvocationId(EmitterState& state) {
	if (state.subgroup_local_invocation_id_variable == 0) {
		EXIT("SubgroupLocalInvocationId was not declared before SPIR-V function emission\n");
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpLoad, TypeU32(state), value, state.subgroup_local_invocation_id_variable});
	return state.lane_half == 0 ? value : EmitAddU32(state, value, ConstantU32(state, 32));
}

uint32_t InputVariableForKind(const EmitterState& state, IR::StageInputKind kind) {
	for (const auto& input: state.inputs) {
		if (input.kind == kind) {
			return input.variable_id;
		}
	}
	return 0;
}

const InputBinding* InputBindingForParameter(const EmitterState& state, uint32_t location) {
	for (const auto& input: state.inputs) {
		if (input.kind == IR::StageInputKind::Parameter && input.location == location) {
			return &input;
		}
	}
	return nullptr;
}

uint32_t EmitInputComponentU32(EmitterState& state, IR::StageInputKind kind, uint32_t component) {
	const auto variable = InputVariableForKind(state, kind);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto pointer = state.builder.AllocateId();
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpAccessChain,
	                           TypePointer(state, spv::StorageClassInput, TypeU32(state)), pointer,
	                           variable, ConstantU32(state, component)});
	state.builder.AddFunction({spv::OpLoad, TypeU32(state), value, pointer});
	return value;
}

uint32_t EmitLocalInvocationIndex(EmitterState& state) {
	const auto variable = InputVariableForKind(state, IR::StageInputKind::LocalInvocationIndex);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpLoad, TypeU32(state), value, variable});
	if (state.lane_count == 2) {
		const auto wave_base =
		    EmitBinaryU32(state, spv::OpBitwiseAnd, value, ConstantU32(state, ~31u));
		return EmitAddU32(state, EmitAddU32(state, value, wave_base),
		                  ConstantU32(state, state.lane_half * 32));
	}
	return value;
}

uint32_t EmitVertexParameterComponentU32(EmitterState& state, const InputBinding& input,
                                         uint32_t component) {
	const auto count = VertexParameterComponentCount(input);
	const auto kind  = VertexParameterScalarKind(state, input.location);
	const auto scalar_type = VertexParameterScalarType(state, kind);
	uint32_t   raw         = state.builder.AllocateId();
	if (count == 1u) {
		state.builder.AddFunction({spv::OpLoad, scalar_type, raw, input.variable_id});
	} else {
		const auto pointer_type = TypePointer(state, spv::StorageClassInput, scalar_type);
		const auto pointer      = state.builder.AllocateId();
		state.builder.AddFunction({spv::OpAccessChain, pointer_type, pointer, input.variable_id,
		                           ConstantU32(state, component)});
		state.builder.AddFunction({spv::OpLoad, scalar_type, raw, pointer});
	}

	if (kind == VertexInputScalarKind::Uint) {
		return raw;
	}

	const auto bits = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpBitcast, TypeU32(state), bits, raw});
	return bits;
}

uint32_t EmitSubgroupLaneActiveBool(EmitterState& state, uint32_t lane) {
	const auto active_ballot = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpGroupNonUniformBallot, TypeU32Vector(state, 4), active_ballot,
	                           ConstantU32(state, spv::ScopeSubgroup), ConstantBool(state, true)});
	return EmitBallotLaneActiveBool(state, active_ballot, lane);
}
uint32_t EmitBallotLaneActiveBool(EmitterState& state, uint32_t active_ballot, uint32_t lane) {
	const auto low = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpCompositeExtract, TypeU32(state), low, active_ballot, 0});
	uint32_t mask = low;
	if (state.program.wave_size == 64u) {
		const auto high     = state.builder.AllocateId();
		const auto in_high  = state.builder.AllocateId();
		const auto selected = state.builder.AllocateId();
		state.builder.AddFunction(
		    {spv::OpCompositeExtract, TypeU32(state), high, active_ballot, 1});
		state.builder.AddFunction(
		    {spv::OpUGreaterThanEqual, TypeBool(state), in_high, lane, ConstantU32(state, 32)});
		state.builder.AddFunction({spv::OpSelect, TypeU32(state), selected, in_high, high, low});
		mask = selected;
	}

	const auto lane_low = state.builder.AllocateId();
	const auto bit      = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpBitwiseAnd, TypeU32(state), lane_low, lane, ConstantU32(state, 31)});
	state.builder.AddFunction(
	    {spv::OpShiftLeftLogical, TypeU32(state), bit, ConstantU32(state, 1), lane_low});

	const auto hit      = state.builder.AllocateId();
	const auto active   = state.builder.AllocateId();
	const auto in_range = state.builder.AllocateId();
	const auto ret      = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpBitwiseAnd, TypeU32(state), hit, mask, bit});
	state.builder.AddFunction(
	    {spv::OpINotEqual, TypeBool(state), active, hit, ConstantU32(state, 0)});
	state.builder.AddFunction({spv::OpULessThan, TypeBool(state), in_range, lane,
	                           ConstantU32(state, state.program.wave_size)});
	state.builder.AddFunction({spv::OpLogicalAnd, TypeBool(state), ret, active, in_range});
	return ret;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
