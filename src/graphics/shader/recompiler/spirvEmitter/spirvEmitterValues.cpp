#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

bool SdwaSelectorOffsetWidth(uint32_t sel, uint32_t& offset, uint32_t& width) {
	if (sel <= 3u) {
		offset = sel * 8u;
		width  = 8u;
		return true;
	}
	if (sel == 4u || sel == 5u) {
		offset = sel == 5u ? 16u : 0u;
		width  = 16u;
		return true;
	}
	if (sel == 6u) {
		offset = 0;
		width  = 32u;
		return true;
	}
	return false;
}

uint32_t EmitSdwaExtractU32(EmitterState* state, const IR::Operand& operand, uint32_t value) {
	if (operand.sdwa_sel == 6u && !operand.sdwa_sext) {
		return value;
	}

	uint32_t offset = 0;
	uint32_t width  = 0;
	if (!SdwaSelectorOffsetWidth(operand.sdwa_sel, offset, width) || width == 32u) {
		return value;
	}

	if (operand.sdwa_sext) {
		const auto signed_value = state->builder.AllocateId();
		const auto extracted    = state->builder.AllocateId();
		const auto bits         = state->builder.AllocateId();
		state->builder.AddFunction({OpBitcast, state->int_type, signed_value, value});
		state->builder.AddFunction({OpBitFieldSExtract, state->int_type, extracted, signed_value,
		                            ConstantU32(state, offset), ConstantU32(state, width)});
		state->builder.AddFunction({OpBitcast, state->uint_type, bits, extracted});
		return bits;
	}

	const auto extracted = state->builder.AllocateId();
	state->builder.AddFunction({OpBitFieldUExtract, state->uint_type, extracted, value,
	                            ConstantU32(state, offset), ConstantU32(state, width)});
	return extracted;
}

uint32_t EmitTrueBool(EmitterState* state) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, ret, ConstantU32(state, 1), ConstantU32(state, 0)});
	return ret;
}

DppTargetLane EmitDppQuadPermTargetLane(EmitterState* state, uint32_t subid, uint32_t control) {
	const auto quad_base = state->builder.AllocateId();
	const auto lane      = state->builder.AllocateId();
	const auto shift     = state->builder.AllocateId();
	const auto selected0 = state->builder.AllocateId();
	const auto selected  = state->builder.AllocateId();
	const auto target    = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, quad_base, subid, ConstantU32(state, 0xfffffffcu)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, 3)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, shift, lane, ConstantU32(state, 1)});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, selected0, ConstantU32(state, control), shift});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, selected, selected0, ConstantU32(state, 3)});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, target, quad_base, selected});
	return {target, EmitTrueBool(state)};
}

DppTargetLane EmitDppRowShiftTargetLane(EmitterState* state, uint32_t subid, uint32_t amount,
                                        bool left) {
	const auto row          = state->builder.AllocateId();
	const auto lane         = state->builder.AllocateId();
	const auto lane_shifted = state->builder.AllocateId();
	const auto target       = state->builder.AllocateId();
	const auto valid        = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, row, subid, ConstantU32(state, 0xfffffff0u)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, 15)});
	if (left) {
		state->builder.AddFunction(
		    {OpIAdd, state->uint_type, lane_shifted, lane, ConstantU32(state, amount)});
		state->builder.AddFunction(
		    {OpULessThan, state->bool_type, valid, lane, ConstantU32(state, 16u - amount)});
	} else {
		state->builder.AddFunction(
		    {OpISub, state->uint_type, lane_shifted, lane, ConstantU32(state, amount)});
		state->builder.AddFunction(
		    {OpUGreaterThanEqual, state->bool_type, valid, lane, ConstantU32(state, amount)});
	}
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, target, row, lane_shifted});
	return {target, valid};
}

DppTargetLane EmitDppRowRotateRightTargetLane(EmitterState* state, uint32_t subid,
                                              uint32_t amount) {
	const auto row      = state->builder.AllocateId();
	const auto lane     = state->builder.AllocateId();
	const auto in_high  = state->builder.AllocateId();
	const auto minus    = state->builder.AllocateId();
	const auto plus     = state->builder.AllocateId();
	const auto selected = state->builder.AllocateId();
	const auto target   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, row, subid, ConstantU32(state, 0xfffffff0u)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, 15)});
	state->builder.AddFunction(
	    {OpUGreaterThanEqual, state->bool_type, in_high, lane, ConstantU32(state, amount)});
	state->builder.AddFunction({OpISub, state->uint_type, minus, lane, ConstantU32(state, amount)});
	state->builder.AddFunction(
	    {OpIAdd, state->uint_type, plus, lane, ConstantU32(state, 16u - amount)});
	state->builder.AddFunction({OpSelect, state->uint_type, selected, in_high, minus, plus});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, target, row, selected});
	return {target, EmitTrueBool(state)};
}

DppTargetLane EmitDppMirrorTargetLane(EmitterState* state, uint32_t subid, bool half_row) {
	const auto base_mask = half_row ? 0xfffffff8u : 0xfffffff0u;
	const auto lane_mask = half_row ? 7u : 15u;
	const auto base      = state->builder.AllocateId();
	const auto lane      = state->builder.AllocateId();
	const auto mirrored  = state->builder.AllocateId();
	const auto target    = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, base, subid, ConstantU32(state, base_mask)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, lane_mask)});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, mirrored, ConstantU32(state, lane_mask), lane});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, target, base, mirrored});
	return {target, EmitTrueBool(state)};
}

DppTargetLane EmitDppTargetLane(EmitterState* state, uint32_t control) {
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
	return {subid, EmitTrueBool(state)};
}

uint32_t EmitDppValueU32(EmitterState* state, const IR::Operand& operand, uint32_t value) {
	if (!operand.dpp) {
		return value;
	}

	const auto target   = EmitDppTargetLane(state, operand.dpp_ctrl);
	const auto shuffled = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformShuffle, state->uint_type, shuffled,
	                            ConstantU32(state, ScopeSubgroup), value, target.lane});
	if (operand.dpp_fetch_inactive) {
		return shuffled;
	}

	const auto source_active = EmitLaneIndexActiveBool(state, target.lane);
	const auto can_fetch     = state->builder.AllocateId();
	const auto ret           = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpLogicalAnd, state->bool_type, can_fetch, target.valid, source_active});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, ret, can_fetch, shuffled, ConstantU32(state, 0)});
	return ret;
}

uint32_t EmitValueLoad(EmitterState* state, const IR::Operand& operand) {
	uint32_t value = 0;
	switch (operand.kind) {
		case IR::OperandKind::ImmediateU32:
		case IR::OperandKind::PcRelativeU32: value = ConstantU32(state, operand.imm); break;
		case IR::OperandKind::Register: {
			const auto pointer = PointerForRegister(*state, operand.reg);
			if (pointer == 0) {
				value = ConstantU32(state, 0);
				break;
			}
			value = state->builder.AllocateId();
			state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
			break;
		}
		case IR::OperandKind::Null: value = ConstantU32(state, 0); break;
		default: value = ConstantU32(state, 0); break;
	}
	value = EmitDppValueU32(state, operand, value);
	return EmitSdwaExtractU32(state, operand, value);
}

uint32_t EmitRegisterLoad(EmitterState* state, IR::Register reg) {
	if (IsInactiveWave32ExecHigh(*state, reg)) {
		return ConstantU32(state, 0);
	}
	const auto pointer = PointerForRegister(*state, reg);
	if (pointer == 0) {
		return ConstantU32(state, 0);
	}
	const auto value = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
	return value;
}

uint32_t WaveMaskHighLoad(EmitterState* state, IR::RegisterFile file) {
	if (state->wave_size == 32u && IsMaskRegisterFile(file)) {
		return ConstantU32(state, 0);
	}
	return EmitRegisterLoad(state, {file, 1});
}

uint32_t EmitLaneMaskPartU32(EmitterState* state, uint32_t part) {
	const auto subid = EmitSubgroupLocalInvocationId(state);
	const auto lane  = state->builder.AllocateId();
	const auto bit   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, bit, ConstantU32(state, 1), lane});
	if (part == 0) {
		const auto in_low = state->builder.AllocateId();
		const auto ret    = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpULessThan, state->bool_type, in_low, subid, ConstantU32(state, 32)});
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, ret, in_low, bit, ConstantU32(state, 0)});
		return ret;
	}
	if (state->wave_size == 32u) {
		return ConstantU32(state, 0);
	}
	const auto in_high = state->builder.AllocateId();
	const auto ret     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpUGreaterThanEqual, state->bool_type, in_high, subid, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, ret, in_high, bit, ConstantU32(state, 0)});
	return ret;
}

uint32_t EmitThreadMaskBelowPartU32(EmitterState* state, uint32_t part) {
	const auto subid = EmitSubgroupLocalInvocationId(state);
	const auto lane  = state->builder.AllocateId();
	const auto bit   = state->builder.AllocateId();
	const auto below = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, bit, ConstantU32(state, 1), lane});
	state->builder.AddFunction({OpISub, state->uint_type, below, bit, ConstantU32(state, 1)});

	if (part == 0) {
		const auto in_high = state->builder.AllocateId();
		const auto ret     = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpUGreaterThanEqual, state->bool_type, in_high, subid, ConstantU32(state, 32)});
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, ret, in_high, ConstantU32(state, 0xffffffffu), below});
		return ret;
	}
	if (state->wave_size == 32u) {
		return ConstantU32(state, 0);
	}
	const auto in_high = state->builder.AllocateId();
	const auto ret     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpUGreaterThanEqual, state->bool_type, in_high, subid, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, ret, in_high, below, ConstantU32(state, 0)});
	return ret;
}

uint32_t EmitMaskActiveBool(EmitterState* state, IR::RegisterFile file) {
	const auto low       = EmitRegisterLoad(state, {file, 0});
	const auto condition = state->builder.AllocateId();
	if (state->per_invocation_masks) {
		const auto combined = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpBitwiseOr, state->uint_type, combined, low, WaveMaskHighLoad(state, file)});
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, condition, combined, ConstantU32(state, 0)});
		return condition;
	}
	if (state->exact_subgroup_operations) {
		const auto high     = WaveMaskHighLoad(state, file);
		const auto low_hit  = state->builder.AllocateId();
		const auto high_hit = state->builder.AllocateId();
		const auto active   = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, low_hit, low, EmitLaneMaskPartU32(state, 0)});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, high_hit, high, EmitLaneMaskPartU32(state, 1)});
		state->builder.AddFunction({OpBitwiseOr, state->uint_type, active, low_hit, high_hit});
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, condition, active, ConstantU32(state, 0)});
		return condition;
	}
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, condition, low, ConstantU32(state, 0)});
	return condition;
}

uint32_t EmitMaskZeroBool(EmitterState* state, IR::RegisterFile file, bool zero) {
	const auto active = EmitMaskActiveBool(state, file);
	if (!zero) {
		return active;
	}
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalNot, state->bool_type, cond, active});
	return cond;
}

uint32_t EmitMaskSummaryZeroBool(EmitterState* state, IR::RegisterFile file, bool zero) {
	if (state->per_invocation_masks) {
		return EmitMaskZeroBool(state, file, zero);
	}
	if (state->exact_subgroup_operations) {
		const auto low      = EmitRegisterLoad(state, {file, 0});
		const auto high     = WaveMaskHighLoad(state, file);
		const auto combined = state->builder.AllocateId();
		const auto active   = state->builder.AllocateId();
		state->builder.AddFunction({OpBitwiseOr, state->uint_type, combined, low, high});
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, active, combined, ConstantU32(state, 0)});
		if (!zero) {
			return active;
		}
		const auto inactive = state->builder.AllocateId();
		state->builder.AddFunction({OpLogicalNot, state->bool_type, inactive, active});
		return inactive;
	}
	const auto active = EmitMaskActiveBool(state, file);
	if (!zero) {
		return active;
	}
	const auto inactive = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalNot, state->bool_type, inactive, active});
	return inactive;
}

uint32_t EmitExecActiveBool(EmitterState* state) {
	return EmitMaskZeroBool(state, IR::RegisterFile::Exec, false);
}

uint32_t EmitConditionBool(EmitterState* state, const IR::Operand& operand) {
	if (operand.kind == IR::OperandKind::Register) {
		if (operand.reg.file == IR::RegisterFile::Exec ||
		    operand.reg.file == IR::RegisterFile::Vcc) {
			return EmitMaskActiveBool(state, operand.reg.file);
		}
	}
	const auto cond_value = EmitValueLoad(state, operand);
	const auto cond       = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, cond, cond_value, ConstantU32(state, 0)});
	return cond;
}

uint32_t EmitSubgroupLocalInvocationId(EmitterState* state) {
	if (state->subgroup_local_invocation_id_variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto value = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpLoad, state->uint_type, value, state->subgroup_local_invocation_id_variable});
	return value;
}

uint32_t InputVariableForKind(const EmitterState& state, IR::StageInputKind kind) {
	for (const auto& input: state.inputs) {
		if (input.kind == kind) {
			return input.variable_id;
		}
	}
	return 0;
}

uint32_t InputVariableForParameter(const EmitterState& state, uint32_t location) {
	for (const auto& input: state.inputs) {
		if (input.kind == IR::StageInputKind::Parameter && input.location == location) {
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

uint32_t EmitInputComponentU32(EmitterState* state, IR::StageInputKind kind, uint32_t component) {
	const auto variable = InputVariableForKind(*state, kind);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto pointer = state->builder.AllocateId();
	const auto value   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpAccessChain, state->ptr_input_uint, pointer, variable, ConstantU32(state, component)});
	state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
	return value;
}

uint32_t EmitLocalInvocationIndex(EmitterState* state) {
	const auto variable = InputVariableForKind(*state, IR::StageInputKind::LocalInvocationIndex);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto value = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->uint_type, value, variable});
	return value;
}

uint32_t VertexInputDefaultComponentU32(EmitterState* state, VertexInputScalarKind kind,
                                        uint32_t component) {
	if ((component & 3u) != 3u) {
		return ConstantU32(state, 0);
	}
	return ConstantU32(state, kind == VertexInputScalarKind::Float ? 0x3f800000u : 1u);
}

uint32_t EmitVertexParameterComponentU32(EmitterState* state, const InputBinding& input,
                                         uint32_t component) {
	const auto count = VertexParameterComponentCount(*state, input);
	const auto kind  = VertexParameterScalarKind(*state, input.location);
	if (component >= count) {
		return VertexInputDefaultComponentU32(state, kind, component);
	}

	const auto scalar_type = VertexParameterScalarType(*state, kind);
	uint32_t   raw         = state->builder.AllocateId();
	if (count == 1u) {
		state->builder.AddFunction({OpLoad, scalar_type, raw, input.variable_id});
	} else {
		const auto pointer_type = VertexParameterScalarPointerType(*state, kind);
		const auto pointer      = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, pointer_type, pointer, input.variable_id,
		                            ConstantU32(state, component)});
		state->builder.AddFunction({OpLoad, scalar_type, raw, pointer});
	}

	if (kind == VertexInputScalarKind::Uint) {
		return raw;
	}

	const auto bits = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, raw});
	return bits;
}

void EmitLoadInputF32(EmitterState* state, const IR::Instruction& inst) {
	const auto* input = InputBindingForParameter(*state, inst.input_info.attr);
	if (input == nullptr || input->variable_id == 0) {
		EmitStoreU32(state, inst.dst, ConstantU32(state, 0));
		return;
	}

	if (state->stage == ShaderType::Vertex) {
		EmitStoreU32(state, inst.dst,
		             EmitVertexParameterComponentU32(state, *input, inst.input_info.chan & 3u));
		return;
	}

	const auto input_value = state->builder.AllocateId();
	const auto component   = state->builder.AllocateId();
	const auto bits        = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->vec4_float_type, input_value, input->variable_id});
	state->builder.AddFunction(
	    {OpCompositeExtract, state->float_type, component, input_value, inst.input_info.chan & 3u});
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, component});
	EmitStoreU32(state, inst.dst, bits);
}

uint32_t EmitSccBool(EmitterState* state, bool non_zero) {
	const auto value = EmitRegisterLoad(state, SccRegister());
	const auto cond  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {non_zero ? OpINotEqual : OpIEqual, state->bool_type, cond, value, ConstantU32(state, 0)});
	return cond;
}

uint32_t EmitFloatLoad(EmitterState* state, const IR::Operand& operand) {
	const auto uint_value  = EmitValueLoad(state, operand);
	auto       float_value = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->float_type, float_value, uint_value});
	if (operand.absolute) {
		const auto abs_value = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpExtInst, state->float_type, abs_value, state->glsl_std450, GlslFAbs, float_value});
		float_value = abs_value;
	}
	if (operand.negate) {
		const auto neg_value = state->builder.AllocateId();
		state->builder.AddFunction({OpFNegate, state->float_type, neg_value, float_value});
		float_value = neg_value;
	}
	return float_value;
}

uint32_t ApplyResultModifiersF32(EmitterState* state, uint32_t value, const IR::Operand& dst) {
	uint32_t result = value;
	if (dst.omod != 0) {
		const uint32_t bits   = dst.omod == 1u   ? 0x40000000u
		                        : dst.omod == 2u ? 0x40800000u
		                                         : 0x3f000000u;
		const auto     scaled = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpFMul, state->float_type, scaled, result, ConstantF32(state, bits)});
		result = scaled;
	}
	if (!dst.clamp) {
		return result;
	}
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpExtInst, state->float_type, ret, state->glsl_std450, GlslFClamp,
	                            result, ConstantF32(state, 0), ConstantF32(state, 0x3f800000u)});
	return ret;
}

uint32_t EmitMixF32Load(EmitterState* state, const IR::Operand& operand) {
	auto value_operand      = operand;
	value_operand.negate_hi = false;

	if (operand.op_sel_hi) {
		auto raw               = operand;
		raw.sdwa_sel           = 6;
		raw.sdwa_sext          = false;
		raw.op_sel             = false;
		raw.op_sel_hi          = false;
		raw.negate             = false;
		raw.negate_hi          = false;
		raw.absolute           = false;
		raw.dpp                = false;
		raw.dpp_fetch_inactive = false;
		raw.dpp_bound_ctrl     = false;

		const auto bits     = EmitValueLoad(state, raw);
		const auto unpacked = state->builder.AllocateId();
		auto       f32      = state->builder.AllocateId();
		state->builder.AddFunction({OpExtInst, state->vec2_float_type, unpacked, state->glsl_std450,
		                            GlslUnpackHalf2x16, bits});
		state->builder.AddFunction(
		    {OpCompositeExtract, state->float_type, f32, unpacked, operand.op_sel ? 1u : 0u});
		if (operand.absolute) {
			const auto abs_value = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpExtInst, state->float_type, abs_value, state->glsl_std450, GlslFAbs, f32});
			f32 = abs_value;
		}
		if (value_operand.negate) {
			const auto neg_value = state->builder.AllocateId();
			state->builder.AddFunction({OpFNegate, state->float_type, neg_value, f32});
			f32 = neg_value;
		}
		return f32;
	}

	value_operand.op_sel    = false;
	value_operand.op_sel_hi = false;
	return EmitFloatLoad(state, value_operand);
}

IR::Operand OffsetRegisterOperand(const IR::Operand& operand, uint32_t offset) {
	if (offset == 0 || operand.kind != IR::OperandKind::Register) {
		return operand;
	}
	auto ret = operand;
	ret.reg.index += offset;
	ret.sdwa_sel           = 6;
	ret.omod               = 0;
	ret.sdwa_sext          = false;
	ret.op_sel             = false;
	ret.op_sel_hi          = false;
	ret.negate             = false;
	ret.negate_hi          = false;
	ret.absolute           = false;
	ret.clamp              = false;
	ret.dpp_ctrl           = 0;
	ret.dpp_row_mask       = 0xf;
	ret.dpp_bank_mask      = 0xf;
	ret.dpp_fetch_inactive = false;
	ret.dpp_bound_ctrl     = false;
	ret.dpp                = false;
	return ret;
}

uint32_t EmitLaneMaskOperandActiveBool(EmitterState* state, const IR::Operand& operand) {
	const auto low    = EmitValueLoad(state, operand);
	const auto active = state->builder.AllocateId();
	if (state->per_invocation_masks || state->exact_subgroup_operations) {
		uint32_t high = ConstantU32(state, 0);
		if (operand.kind == IR::OperandKind::Register &&
		    (operand.reg.file == IR::RegisterFile::Scalar ||
		     operand.reg.file == IR::RegisterFile::Vcc ||
		     operand.reg.file == IR::RegisterFile::Exec)) {
			high = IsMaskRegisterFile(operand.reg.file)
			           ? WaveMaskHighLoad(state, operand.reg.file)
			           : EmitValueLoad(state, OffsetRegisterOperand(operand, 1));
		}
		if (state->per_invocation_masks) {
			const auto combined = state->builder.AllocateId();
			state->builder.AddFunction({OpBitwiseOr, state->uint_type, combined, low, high});
			state->builder.AddFunction(
			    {OpINotEqual, state->bool_type, active, combined, ConstantU32(state, 0)});
			return active;
		}
		const auto low_hit  = state->builder.AllocateId();
		const auto high_hit = state->builder.AllocateId();
		const auto hit      = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, low_hit, low, EmitLaneMaskPartU32(state, 0)});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, high_hit, high, EmitLaneMaskPartU32(state, 1)});
		state->builder.AddFunction({OpBitwiseOr, state->uint_type, hit, low_hit, high_hit});
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, active, hit, ConstantU32(state, 0)});
		return active;
	}
	state->builder.AddFunction({OpINotEqual, state->bool_type, active, low, ConstantU32(state, 0)});
	return active;
}

uint32_t EmitLaneIndexActiveBool(EmitterState* state, uint32_t lane) {
	if (state->per_invocation_masks) {
		const auto ballot = state->builder.AllocateId();
		state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
		                            ConstantU32(state, ScopeSubgroup), EmitExecActiveBool(state)});
		return EmitBallotLaneActiveBool(state, ballot, lane);
	}
	const auto lane_low = state->builder.AllocateId();
	const auto bit      = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane_low, lane, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, bit, ConstantU32(state, 1), lane_low});

	uint32_t mask = EmitRegisterLoad(state, {IR::RegisterFile::Exec, 0});
	if (state->wave_size == 64u) {
		const auto high     = EmitRegisterLoad(state, {IR::RegisterFile::Exec, 1});
		const auto in_high  = state->builder.AllocateId();
		const auto selected = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpUGreaterThanEqual, state->bool_type, in_high, lane, ConstantU32(state, 32)});
		state->builder.AddFunction({OpSelect, state->uint_type, selected, in_high, high, mask});
		mask = selected;
	}

	const auto hit = state->builder.AllocateId();
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, hit, mask, bit});
	state->builder.AddFunction({OpINotEqual, state->bool_type, ret, hit, ConstantU32(state, 0)});
	return ret;
}

uint32_t EmitSubgroupLaneActiveBool(EmitterState* state, uint32_t lane) {
	const auto active_ballot = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, active_ballot,
	                            ConstantU32(state, ScopeSubgroup), EmitTrueBool(state)});
	return EmitBallotLaneActiveBool(state, active_ballot, lane);
}
uint32_t EmitBallotLaneActiveBool(EmitterState* state, uint32_t active_ballot, uint32_t lane) {
	const auto low = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, low, active_ballot, 0});
	uint32_t mask = low;
	if (state->wave_size == 64u) {
		const auto high     = state->builder.AllocateId();
		const auto in_high  = state->builder.AllocateId();
		const auto selected = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeExtract, state->uint_type, high, active_ballot, 1});
		state->builder.AddFunction(
		    {OpUGreaterThanEqual, state->bool_type, in_high, lane, ConstantU32(state, 32)});
		state->builder.AddFunction({OpSelect, state->uint_type, selected, in_high, high, low});
		mask = selected;
	}

	const auto lane_low = state->builder.AllocateId();
	const auto bit      = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane_low, lane, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, bit, ConstantU32(state, 1), lane_low});

	const auto hit      = state->builder.AllocateId();
	const auto active   = state->builder.AllocateId();
	const auto in_range = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, hit, mask, bit});
	state->builder.AddFunction({OpINotEqual, state->bool_type, active, hit, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, in_range, lane, ConstantU32(state, state->wave_size)});
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, ret, active, in_range});
	return ret;
}

uint32_t EmitSequentialValueLoad(EmitterState* state, const IR::Operand& operand, uint32_t offset) {
	if (offset == 0 || operand.kind == IR::OperandKind::Register) {
		return EmitValueLoad(state, OffsetRegisterOperand(operand, offset));
	}
	if (operand.kind == IR::OperandKind::ImmediateU32 && offset == 1) {
		return ConstantU32(state, operand.sext_64 ? 0xffffffffu : 0u);
	}
	return ConstantU32(state, 0);
}

uint32_t EmitSequentialFloatLoad(EmitterState* state, const IR::Operand& operand, uint32_t offset) {
	const auto value = EmitSequentialValueLoad(state, operand, offset);
	const auto f32   = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->float_type, f32, value});
	return f32;
}

bool ImageAddressUsesA16(const IR::Instruction& inst) {
	return (inst.memory.image_sample_flags & Decoder::ImageSampleFlagA16) != 0u;
}

bool ImageAddressComponentUsesA16(const IR::Instruction& inst, uint32_t component) {
	if (!ImageAddressUsesA16(inst)) {
		return false;
	}

	uint32_t cursor = 0;
	if ((inst.memory.image_sample_flags & Decoder::ImageSampleFlagOffset) != 0u) {
		if (component == cursor) {
			return false;
		}
		cursor++;
	}
	if ((inst.memory.image_sample_flags & Decoder::ImageSampleFlagBias) != 0u) {
		if (component == cursor) {
			return true;
		}
		cursor++;
	}
	if ((inst.memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0u &&
	    component == cursor) {
		return false;
	}
	return true;
}

uint32_t ImageAddressHalfComponent(const IR::Instruction& inst, uint32_t component) {
	if (!ImageAddressUsesA16(inst)) {
		return component * 2u;
	}

	uint32_t half_component = 0;
	for (uint32_t i = 0; i < component; i++) {
		const auto half_width = ImageAddressComponentUsesA16(inst, i) ? 1u : 2u;
		if (half_width == 2u && (half_component & 1u) != 0u) {
			half_component++;
		}
		half_component += half_width;
	}
	if (!ImageAddressComponentUsesA16(inst, component) && (half_component & 1u) != 0u) {
		half_component++;
	}
	return half_component;
}

uint32_t ImageAddressPackedComponent(const IR::Instruction& inst, uint32_t component) {
	return ImageAddressHalfComponent(inst, component) / 2u;
}

IR::Operand ImageAddressOperand(const IR::Instruction& inst, const IR::Operand& base,
                                uint32_t component) {
	const auto packed_component = ImageAddressPackedComponent(inst, component);
	if (packed_component > 0 && inst.memory.image_nsa_dwords != 0) {
		const auto nsa_index = packed_component - 1u;
		const auto nsa_count = std::min<uint32_t>(inst.memory.image_nsa_dwords * 4u,
		                                          Decoder::MaxImageNsaAddressComponents);
		if (nsa_index < nsa_count) {
			return MakeRegisterOperand(IR::RegisterFile::Vector,
			                           inst.memory.image_nsa_addr[nsa_index]);
		}
	}
	return OffsetRegisterOperand(base, packed_component);
}

uint32_t EmitImageAddressValueLoad(EmitterState* state, const IR::Instruction& inst,
                                   const IR::Operand& base, uint32_t component) {
	const auto operand          = ImageAddressOperand(inst, base, component);
	const auto packed_component = ImageAddressPackedComponent(inst, component);
	if (packed_component == 0 || operand.kind == IR::OperandKind::Register) {
		auto value = EmitValueLoad(state, operand);
		if (ImageAddressComponentUsesA16(inst, component)) {
			if ((ImageAddressHalfComponent(inst, component) & 1u) != 0u) {
				value = EmitShiftRightConstant(state, value, 16u);
			}
			value = EmitAndConstant(state, value, 0xffffu);
		}
		return value;
	}
	return ConstantU32(state, 0);
}

uint32_t EmitImageAddressFloatLoad(EmitterState* state, const IR::Instruction& inst,
                                   const IR::Operand& base, uint32_t component) {
	const auto value = EmitImageAddressValueLoad(state, inst, base, component);
	if (ImageAddressComponentUsesA16(inst, component)) {
		return EmitF16BitsToF32(state, value);
	}
	const auto f32 = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->float_type, f32, value});
	return f32;
}

uint32_t EmitZeroF32(EmitterState* state) {
	const auto f32 = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->float_type, f32, ConstantU32(state, 0)});
	return f32;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
