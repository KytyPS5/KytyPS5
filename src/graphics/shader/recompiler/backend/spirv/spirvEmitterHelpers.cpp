#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

DppTargetLane EmitDppPermTargetLane(EmitterState& state, uint32_t subid, uint32_t control,
                                    uint32_t lane_bits) {
	// Each lane of a group takes its lane_bits-wide source index from control.
	const auto lane = Binary(state, spv::OpBitwiseAnd, TypeU32(state), subid,
	                         ConstantU32(state, (1u << lane_bits) - 1u));
	const auto selected = EmitNative<spv::OpBitFieldUExtract, IR::Type::U32>(
	    state, ConstantU32(state, control),
	    Binary(state, spv::OpIMul, TypeU32(state), lane, ConstantU32(state, lane_bits)),
	    ConstantU32(state, lane_bits));
	return {EmitNative<spv::OpBitFieldInsert, IR::Type::U32>(
	            state, subid, selected, ConstantU32(state, 0), ConstantU32(state, lane_bits)),
	        ConstantBool(state, true)};
}

DppTargetLane EmitDppRowShiftTargetLane(EmitterState& state, uint32_t subid, uint32_t amount,
                                        bool left) {
	const auto row          = state.builder.AllocateId();
	const auto lane         = state.builder.AllocateId();
	const auto lane_shifted = state.builder.AllocateId();
	const auto target       = state.builder.AllocateId();
	const auto valid        = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), row, subid,
	                          ConstantU32(state, 0xfffffff0u));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane, subid,
	                          ConstantU32(state, 15));
	if (left) {
		state.builder.AddFunction(spv::OpIAdd, TypeU32(state), lane_shifted, lane,
		                          ConstantU32(state, amount));
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), valid, lane,
		                          ConstantU32(state, 16u - amount));
	} else {
		state.builder.AddFunction(spv::OpISub, TypeU32(state), lane_shifted, lane,
		                          ConstantU32(state, amount));
		state.builder.AddFunction(spv::OpUGreaterThanEqual, TypeBool(state), valid, lane,
		                          ConstantU32(state, amount));
	}
	state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, row, lane_shifted);
	return {target, valid};
}

DppTargetLane EmitDppRowRotateRightTargetLane(EmitterState& state, uint32_t subid,
                                              uint32_t amount) {
	// Rotation wraps within the row, so only the low four lane bits change.
	const auto rotated = Binary(state, spv::OpISub, TypeU32(state), subid, ConstantU32(state, amount));
	return {EmitNative<spv::OpBitFieldInsert, IR::Type::U32>(state, subid, rotated,
	                                                        ConstantU32(state, 0),
	                                                        ConstantU32(state, 4)),
	        ConstantBool(state, true)};
}

DppTargetLane EmitDppMirrorTargetLane(EmitterState& state, uint32_t subid, bool half_row) {
	// Mirroring lane i of a row or half row reads lane (size - 1 - i), which flips its low bits.
	return {Binary(state, spv::OpBitwiseXor, TypeU32(state), subid,
	               ConstantU32(state, half_row ? 7u : 15u)),
	        ConstantBool(state, true)};
}

DppTargetLane EmitDppTargetLane(EmitterState& state, const IR::DppMoveFlags& flags) {
	const auto subid = EmitSubgroupLocalInvocationId(state);
	const auto control = flags.control;
	if (flags.dpp8) {
		return EmitDppPermTargetLane(state, subid, control, 3u);
	}
	if (control <= 0xffu) {
		return EmitDppPermTargetLane(state, subid, control, 2u);
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
	// row_share:N reads lane N of the caller's row of 16.
	if (control >= 0x150u && control <= 0x15fu) {
		return {EmitNative<spv::OpBitFieldInsert, IR::Type::U32>(
		            state, subid, ConstantU32(state, control & 0xfu), ConstantU32(state, 0),
		            ConstantU32(state, 4)),
		        ConstantBool(state, true)};
	}
	if (control >= 0x160u && control <= 0x16fu) {
		const auto target = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpBitwiseXor, TypeU32(state), target, subid,
		                          ConstantU32(state, control & 0xfu));
		return {target, ConstantBool(state, true)};
	}
	return {subid, ConstantBool(state, true)};
}

void DeclareFloat16(EmitterState& state) {
	if (state.float16_declared) return;
	state.float16_declared = true;
	state.builder.RequireCapability(spv::CapabilityFloat16);
	state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeSignedZeroInfNanPreserve, 16u);
}

uint32_t EmitPackHalf2x16(EmitterState& state, uint32_t pair) {
	if (!state.host.capabilities.float16) {
		return EmitGlsl<GLSLstd450PackHalf2x16, IR::Type::U32>(state, pair);
	}
	DeclareFloat16(state);
	if (state.f16x2_type == 0) {
		const auto half  = state.builder.Type(spv::OpTypeFloat, 16);
		state.f16x2_type = state.builder.Type(spv::OpTypeVector, half, 2);
	}
	const auto halves = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFConvert, state.f16x2_type, halves, pair);
	const auto packed = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), packed, halves);
	return packed;
}

uint32_t EmitSubgroupLocalInvocationId(EmitterState& state) {
	if (state.single_lane) {
		return ConstantU32(state, 0);
	}
	if (state.subgroup_local_invocation_id_variable == 0) {
		EXIT("SubgroupLocalInvocationId was not declared before SPIR-V function emission\n");
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value,
	                          state.subgroup_local_invocation_id_variable);
	return state.lane_half == 0 ? value : EmitAddU32(state, value, ConstantU32(state, 32));
}

bool LaneIndexFromScan(const EmitterState& state) {
	return Spirv::LaneIndexFromScan(state.program.stage, state.host);
}

bool MeshEmulated(const EmitterState& state) {
	return state.program.stage == ShaderType::Mesh && state.input_info.vertex->mesh.emulated;
}

bool UsesDepthBounds(const EmitterState& state) {
	return state.program.stage == ShaderType::Pixel &&
	       state.input_info.pixel->ps_depth_bounds_format != 0;
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
	state.builder.AddFunction(spv::OpAccessChain,
	                          TypePointer(state, spv::StorageClassInput, TypeU32(state)), pointer,
	                          variable, ConstantU32(state, component));
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

uint32_t EmitLocalInvocationIndex(EmitterState& state) {
	const auto variable = InputVariableForKind(state, IR::StageInputKind::LocalInvocationIndex);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, variable);
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
	const auto kind  = VertexParameterScalarKind(state, input.location);
	if (IsPacked1010102(kind)) {
		// Fields at bits 0, 10, 20 and 30, 10 bits wide but the last, 2. As floats; the signed
		// form sign-extends each field.
		EXIT_IF(component > 3u);
		const bool sign  = kind == VertexInputScalarKind::Sscaled1010102;
		const auto word  = state.builder.AllocateId();
		const auto field = state.builder.AllocateId();
		const auto value = state.builder.AllocateId();
		const auto bits  = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), word, input.variable_id);
		state.builder.AddFunction(sign ? spv::OpBitFieldSExtract : spv::OpBitFieldUExtract,
		                          TypeU32(state), field, word, ConstantU32(state, component * 10u),
		                          ConstantU32(state, component == 3u ? 2u : 10u));
		state.builder.AddFunction(sign ? spv::OpConvertSToF : spv::OpConvertUToF, TypeF32(state),
		                          value, field);
		state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, value);
		return bits;
	}
	const auto count = VertexParameterComponentCount(input);
	const auto scalar_type = VertexParameterScalarType(state, kind);
	uint32_t   raw         = state.builder.AllocateId();
	if (count == 1u) {
		state.builder.AddFunction(spv::OpLoad, scalar_type, raw, input.variable_id);
	} else {
		const auto pointer_type = TypePointer(state, spv::StorageClassInput, scalar_type);
		const auto pointer      = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, pointer_type, pointer, input.variable_id,
		                          ConstantU32(state, component));
		state.builder.AddFunction(spv::OpLoad, scalar_type, raw, pointer);
	}

	if (kind == VertexInputScalarKind::Uint) {
		return raw;
	}

	const auto bits = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, raw);
	return bits;
}

uint32_t EmitSubgroupLaneActiveBool(EmitterState& state, uint32_t lane) {
	if (state.single_lane) {
		return ConstantBool(state, true);
	}
	const auto active_ballot = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformBallot, TypeU32Vector(state, 4), active_ballot,
	                          ConstantU32(state, spv::ScopeSubgroup), ConstantBool(state, true));
	return EmitBallotLaneActiveBool(state, active_ballot, lane);
}
uint32_t EmitBallotLaneActiveBool(EmitterState& state, uint32_t active_ballot, uint32_t lane) {
	const auto low = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, active_ballot, 0);
	uint32_t mask = low;
	if (state.program.wave_size == 64u) {
		const auto high     = state.builder.AllocateId();
		const auto in_high  = state.builder.AllocateId();
		const auto selected = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, active_ballot, 1);
		state.builder.AddFunction(spv::OpUGreaterThanEqual, TypeBool(state), in_high, lane,
		                          ConstantU32(state, 32));
		state.builder.AddFunction(spv::OpSelect, TypeU32(state), selected, in_high, high, low);
		mask = selected;
	}

	const auto lane_low = state.builder.AllocateId();
	const auto bit      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane_low, lane,
	                          ConstantU32(state, 31));
	state.builder.AddFunction(spv::OpShiftLeftLogical, TypeU32(state), bit, ConstantU32(state, 1),
	                          lane_low);

	const auto hit      = state.builder.AllocateId();
	const auto active   = state.builder.AllocateId();
	const auto in_range = state.builder.AllocateId();
	const auto ret      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), hit, mask, bit);
	state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), active, hit,
	                          ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpULessThan, TypeBool(state), in_range, lane,
	                          ConstantU32(state, state.program.wave_size));
	state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), ret, active, in_range);
	return ret;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
