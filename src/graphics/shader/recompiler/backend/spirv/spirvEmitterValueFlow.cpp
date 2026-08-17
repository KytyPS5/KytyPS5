#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t EmitWqmLaneU32(EmitterState& state, uint32_t src) {
	uint32_t ret = ConstantU32(state, 0);
	for (uint32_t i = 0; i < 8u; i++) {
		const auto mask     = 0x0fu << (i * 4u);
		const auto masked   = state.builder.AllocateId();
		const auto non_zero = state.builder.AllocateId();
		const auto expanded = state.builder.AllocateId();
		const auto combined = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpBitwiseAnd, state.uint_type, masked, src, ConstantU32(state, mask)});
		state.builder.AddFunction(
		    {OpINotEqual, state.bool_type, non_zero, masked, ConstantU32(state, 0)});
		state.builder.AddFunction({OpSelect, state.uint_type, expanded, non_zero,
		                           ConstantU32(state, mask), ConstantU32(state, 0)});
		state.builder.AddFunction({OpBitwiseOr, state.uint_type, combined, ret, expanded});
		ret = combined;
	}
	return ret;
}

uint32_t BoolAsU32(ValueEmitContext& ctx, uint32_t value) {
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpSelect, ctx.state.uint_type, result, value,
	                               ConstantU32(ctx.state, 1), ConstantU32(ctx.state, 0)});
	return result;
}

void StoreRegisterMirror(ValueEmitContext& ctx, IR::Register reg, uint32_t value) {
	if (const auto pointer = PointerForRegister(ctx.state, reg); pointer != 0) {
		if (IsInactiveWave32ExecHigh(ctx.state, reg)) {
			value = ConstantU32(ctx.state, 0);
		}
		ctx.state.builder.AddFunction({OpStore, pointer, value});
	}
}

bool IsPrologue(const ValueEmitContext& ctx) {
	return !ctx.program.blocks.empty() && ctx.current_block == ctx.program.blocks.front();
}

uint32_t EmitBuiltinU32(ValueEmitContext& ctx, IR::StageInputKind kind, uint32_t component) {
	auto& state = ctx.state;
	if (kind == IR::StageInputKind::LocalInvocationIndex) {
		return EmitLocalInvocationIndex(state);
	}
	const auto variable = InputVariableForKind(state, kind);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	if (kind == IR::StageInputKind::FrontFacing) {
		const auto value = state.builder.AllocateId();
		const auto bits  = state.builder.AllocateId();
		state.builder.AddFunction({OpLoad, state.bool_type, value, variable});
		state.builder.AddFunction(
		    {OpSelect, state.uint_type, bits, value, ConstantU32(state, 1), ConstantU32(state, 0)});
		return bits;
	}
	if (kind == IR::StageInputKind::VertexIndex || kind == IR::StageInputKind::InstanceIndex) {
		const auto value = state.builder.AllocateId();
		const auto bits  = state.builder.AllocateId();
		state.builder.AddFunction({OpLoad, state.int_type, value, variable});
		state.builder.AddFunction({OpBitcast, state.uint_type, bits, value});
		return bits;
	}
	if (kind == IR::StageInputKind::FragCoord) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		const auto bits    = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, state.ptr_input_float, pointer, variable,
		                           ConstantU32(state, component)});
		state.builder.AddFunction({OpLoad, state.float_type, value, pointer});
		state.builder.AddFunction({OpBitcast, state.uint_type, bits, value});
		return bits;
	}
	return EmitInputComponentU32(state, kind, component);
}

uint32_t EmitWqm(ValueEmitContext& ctx, uint32_t active) {
	auto&      state  = ctx.state;
	const auto ballot = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformBallot, state.vec4_uint_type, ballot,
	                           ConstantU32(state, ScopeSubgroup), active});
	const auto low  = state.builder.AllocateId();
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, state.uint_type, low, ballot, 0});
	state.builder.AddFunction({OpCompositeExtract, state.uint_type, high, ballot, 1});
	const auto wqm_low  = EmitWqmLaneU32(state, low);
	const auto wqm_high = EmitWqmLaneU32(state, high);
	const auto lane     = EmitSubgroupLocalInvocationId(state);
	const auto upper    = state.builder.AllocateId();
	const auto mask     = state.builder.AllocateId();
	const auto bit_lane = state.builder.AllocateId();
	const auto bit      = state.builder.AllocateId();
	const auto hit      = state.builder.AllocateId();
	const auto result   = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpUGreaterThanEqual, state.bool_type, upper, lane, ConstantU32(state, 32)});
	state.builder.AddFunction({OpSelect, state.uint_type, mask, upper, wqm_high, wqm_low});
	state.builder.AddFunction(
	    {OpBitwiseAnd, state.uint_type, bit_lane, lane, ConstantU32(state, 31)});
	state.builder.AddFunction(
	    {OpShiftLeftLogical, state.uint_type, bit, ConstantU32(state, 1), bit_lane});
	state.builder.AddFunction({OpBitwiseAnd, state.uint_type, hit, mask, bit});
	state.builder.AddFunction({OpINotEqual, state.bool_type, result, hit, ConstantU32(state, 0)});
	return result;
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
	    {OpShiftRightLogical, state.uint_type, bank_shift, lane, ConstantU32(state, 2)});
	state.builder.AddFunction(
	    {OpShiftRightLogical, state.uint_type, row_shift, lane, ConstantU32(state, 4)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, state.uint_type, bank, bank_shift, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, state.uint_type, row, row_shift, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {OpShiftLeftLogical, state.uint_type, bank_bit, ConstantU32(state, 1), bank});
	state.builder.AddFunction(
	    {OpShiftLeftLogical, state.uint_type, row_bit, ConstantU32(state, 1), row});
	state.builder.AddFunction(
	    {OpBitwiseAnd, state.uint_type, bank_hit, ConstantU32(state, flags.bank_mask), bank_bit});
	state.builder.AddFunction(
	    {OpBitwiseAnd, state.uint_type, row_hit, ConstantU32(state, flags.row_mask), row_bit});
	state.builder.AddFunction(
	    {OpINotEqual, state.bool_type, bank_ok, bank_hit, ConstantU32(state, 0)});
	state.builder.AddFunction(
	    {OpINotEqual, state.bool_type, row_ok, row_hit, ConstantU32(state, 0)});
	state.builder.AddFunction({OpLogicalAnd, state.bool_type, masks_ok, bank_ok, row_ok});
	uint32_t writable = masks_ok;
	if (!flags.bound_control) {
		const auto target  = EmitDppTargetLane(state, flags.control);
		const auto bounded = state.builder.AllocateId();
		state.builder.AddFunction({OpLogicalAnd, state.bool_type, bounded, writable, target.valid});
		writable = bounded;
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalAnd, state.bool_type, result, exec, writable});
	return result;
}

uint32_t EmitAttribute(ValueEmitContext& ctx, uint32_t attr, uint32_t chan) {
	auto&       state = ctx.state;
	const auto* input = InputBindingForParameter(state, attr);
	if (input == nullptr || input->variable_id == 0) {
		return ConstantU32(state, 0);
	}
	if (state.stage == ShaderType::Vertex) {
		return EmitVertexParameterComponentU32(state, *input, chan & 3u);
	}
	const auto vector    = state.builder.AllocateId();
	const auto component = state.builder.AllocateId();
	const auto bits      = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, state.vec4_float_type, vector, input->variable_id});
	state.builder.AddFunction({OpCompositeExtract, state.float_type, component, vector, chan & 3u});
	state.builder.AddFunction({OpBitcast, state.uint_type, bits, component});
	return bits;
}

bool MrtUsesUint(const EmitterState& state, const IR::ExportInfo& exp) {
	return state.stage == ShaderType::Pixel && exp.kind == IR::ExportTargetKind::Mrt &&
	       exp.index < std::size(state.input_info.pixel->target_output_mode) &&
	       state.input_info.pixel->target_output_mode[exp.index] == 7u;
}

uint32_t ExportRawComponent(ValueEmitContext& ctx, uint32_t vector, uint32_t component) {
	const auto value = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(
	    {OpCompositeExtract, ctx.state.uint_type, value, vector, component});
	return value;
}

uint32_t ExportVector(ValueEmitContext& ctx, uint32_t data, const IR::ExportInfo& exp,
                      bool uint_output) {
	auto& state = ctx.state;
	if (exp.compr && !uint_output) {
		uint32_t f32[4] = {ConstantF32(state, 0), ConstantF32(state, 0), ConstantF32(state, 0),
		                   ConstantF32(state, 0x3f800000u)};
		for (uint32_t pair = 0; pair < 2u; pair++) {
			if ((exp.en & (3u << (pair * 2u))) == 0u) {
				continue;
			}
			const auto packed   = state.builder.AllocateId();
			const auto unpacked = state.builder.AllocateId();
			state.builder.AddFunction({OpCompositeExtract, state.uint_type, packed, data, pair});
			state.builder.AddFunction({OpExtInst, state.vec2_float_type, unpacked,
			                           state.glsl_std450, GlslUnpackHalf2x16, packed});
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair * 2u + lane;
				if (((exp.en >> component) & 1u) != 0u) {
					f32[component] = state.builder.AllocateId();
					state.builder.AddFunction(
					    {OpCompositeExtract, state.float_type, f32[component], unpacked, lane});
				}
			}
		}
		const auto vector = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpCompositeConstruct, state.vec4_float_type, vector, f32[0], f32[1], f32[2], f32[3]});
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
				state.builder.AddFunction({OpBitFieldUExtract, state.uint_type, raw[component],
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
		state.builder.AddFunction(
		    {OpCompositeConstruct, state.vec4_uint_type, vector, raw[0], raw[1], raw[2], raw[3]});
		return vector;
	}
	uint32_t f32[4] {};
	for (uint32_t component = 0; component < 4u; component++) {
		f32[component] = state.builder.AllocateId();
		state.builder.AddFunction({OpBitcast, state.float_type, f32[component], raw[component]});
	}
	const auto vector = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpCompositeConstruct, state.vec4_float_type, vector, f32[0], f32[1], f32[2], f32[3]});
	return vector;
}

void EmitExport(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state = ctx.state;
	const auto& exp   = ctx.Export(inst);
	const auto  exec  = ctx.Arg(inst, 1);
	if (state.stage == ShaderType::Pixel && exp.vm && state.needs_pixel_valid_mask &&
	    state.pixel_valid_mask_variable != 0) {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpSelect, state.uint_type, value, exec, ConstantU32(state, 1), ConstantU32(state, 0)});
		state.builder.AddFunction({OpStore, state.pixel_valid_mask_variable, value});
	}
	if (exp.kind == IR::ExportTargetKind::Null || exp.kind == IR::ExportTargetKind::Primitive ||
	    exp.en == 0u) {
		return;
	}
	EmitIfCondition(state, exec, [&]() {
		const auto data = ctx.Arg(inst, 0);
		if (exp.kind == IR::ExportTargetKind::MrtZ) {
			if ((exp.en & 1u) != 0u && state.depth_variable != 0) {
				const auto raw = ExportRawComponent(ctx, data, 0);
				const auto f32 = state.builder.AllocateId();
				state.builder.AddFunction({OpBitcast, state.float_type, f32, raw});
				state.builder.AddFunction({OpStore, state.depth_variable, f32});
			}
			if ((exp.en & 4u) != 0u && state.sample_mask_variable != 0) {
				const auto raw     = ExportRawComponent(ctx, data, 2);
				const auto value   = state.builder.AllocateId();
				const auto pointer = state.builder.AllocateId();
				state.builder.AddFunction({OpBitcast, state.int_type, value, raw});
				state.builder.AddFunction({OpAccessChain, state.ptr_output_int, pointer,
				                           state.sample_mask_variable, ConstantU32(state, 0)});
				state.builder.AddFunction({OpStore, pointer, value});
			}
			return;
		}
		const auto variable = OutputVariableForExport(state, exp);
		if (variable == 0) {
			return;
		}
		const bool uint_output = MrtUsesUint(state, exp);
		const auto vector_type = uint_output ? state.vec4_uint_type : state.vec4_float_type;
		auto       value       = ExportVector(ctx, data, exp, uint_output);
		if (state.stage == ShaderType::Pixel && exp.kind == IR::ExportTargetKind::Mrt &&
		    exp.index < state.input_info.pixel->target_export_mapping.size()) {
			const auto mapping = state.input_info.pixel->target_export_mapping[exp.index];
			if (!mapping.IsIdentity()) {
				const auto mapped = state.builder.AllocateId();
				state.builder.AddFunction({OpVectorShuffle, vector_type, mapped, value, value,
				                           mapping.Map(0), mapping.Map(1), mapping.Map(2),
				                           mapping.Map(3)});
				value = mapped;
			}
		}
		if (exp.kind == IR::ExportTargetKind::Position) {
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction({OpAccessChain, state.ptr_output_vec4_float, pointer,
			                           variable, ConstantU32(state, 0)});
			state.builder.AddFunction({OpStore, pointer, value});
		} else {
			state.builder.AddFunction({OpStore, variable, value});
		}
	});
}

} // namespace

bool EmitValueFlow(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	switch (inst.GetOpcode()) {
		case IR::ValueOpcode::Identity:
		case IR::ValueOpcode::ConditionRef: ctx.Define(inst, ctx.Arg(inst, 0)); return true;
		case IR::ValueOpcode::SetScalarRegister:
			StoreRegisterMirror(
			    ctx, {IR::RegisterFile::Scalar, IR::RegIndex(inst.Arg(0).ScalarRegister())},
			    ctx.Arg(inst, 1));
			return true;
		case IR::ValueOpcode::SetThreadBitScalarRegister:
			StoreRegisterMirror(
			    ctx, {IR::RegisterFile::Scalar, IR::RegIndex(inst.Arg(0).ScalarRegister())},
			    BoolAsU32(ctx, ctx.Arg(inst, 1)));
			return true;
		case IR::ValueOpcode::SetVectorRegister:
			StoreRegisterMirror(
			    ctx, {IR::RegisterFile::Vector, IR::RegIndex(inst.Arg(0).VectorRegister())},
			    ctx.Arg(inst, 1));
			return true;
		case IR::ValueOpcode::SetScc: ctx.Arg(inst, 0); return true;
		case IR::ValueOpcode::SetExec: ctx.Arg(inst, 0); return true;
		case IR::ValueOpcode::SetExecLo:
		case IR::ValueOpcode::SetExecHi:
			if (!IsPrologue(ctx)) {
				StoreRegisterMirror(ctx,
				                    {IR::RegisterFile::Exec,
				                     inst.GetOpcode() == IR::ValueOpcode::SetExecLo ? 0u : 1u},
				                    ctx.Arg(inst, 0));
			}
			return true;
		case IR::ValueOpcode::SetVcc: ctx.Arg(inst, 0); return true;
		case IR::ValueOpcode::SetVccLo:
		case IR::ValueOpcode::SetVccHi:
			if (!IsPrologue(ctx)) {
				StoreRegisterMirror(ctx,
				                    {IR::RegisterFile::Vcc,
				                     inst.GetOpcode() == IR::ValueOpcode::SetVccLo ? 0u : 1u},
				                    ctx.Arg(inst, 0));
			}
			return true;
		case IR::ValueOpcode::SetM0:
			StoreRegisterMirror(ctx, {IR::RegisterFile::M0, 0}, ctx.Arg(inst, 0));
			return true;
		case IR::ValueOpcode::GetScalarRegister:
		case IR::ValueOpcode::GetThreadBitScalarRegister:
		case IR::ValueOpcode::GetVectorRegister:
		case IR::ValueOpcode::GetScc:
		case IR::ValueOpcode::GetExec:
		case IR::ValueOpcode::GetExecLo:
		case IR::ValueOpcode::GetExecHi:
		case IR::ValueOpcode::GetVcc:
		case IR::ValueOpcode::GetVccLo:
		case IR::ValueOpcode::GetVccHi:
		case IR::ValueOpcode::GetM0: return true;
		case IR::ValueOpcode::Void:
		case IR::ValueOpcode::Reference:
		case IR::ValueOpcode::ReferenceU32:
		case IR::ValueOpcode::Prologue:
		case IR::ValueOpcode::Epilogue:
		case IR::ValueOpcode::ControlNop:
		case IR::ValueOpcode::Waitcnt:
		case IR::ValueOpcode::Sendmsg:
		case IR::ValueOpcode::TtraceData:
		case IR::ValueOpcode::InstPrefetch: return true;
		case IR::ValueOpcode::Barrier: {
			const auto semantics = MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory;
			state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
			                           ConstantU32(state, ScopeWorkgroup),
			                           ConstantU32(state, semantics)});
			return true;
		}
		case IR::ValueOpcode::GetUserData: {
			const auto reg   = inst.Arg(0).ScalarRegister();
			uint32_t   dword = 0;
			if (!UserDataDwordIndex(state, {IR::RegisterFile::Scalar, IR::RegIndex(reg)}, dword)) {
				ctx.Define(inst, ConstantU32(state, 0));
			} else {
				ctx.Define(inst, EmitShaderDataDwordLoad(state, dword));
			}
			return true;
		}
		case IR::ValueOpcode::GetBuiltin:
			ctx.Define(inst, EmitBuiltinU32(ctx, static_cast<IR::StageInputKind>(inst.Arg(0).U32()),
			                                inst.Arg(1).U32()));
			return true;
		case IR::ValueOpcode::UndefU1:
		case IR::ValueOpcode::UndefU8:
		case IR::ValueOpcode::UndefU16:
		case IR::ValueOpcode::UndefU32:
		case IR::ValueOpcode::UndefU64:
			state.builder.AddFunction({OpUndef, ctx.TypeId(inst.GetType()), ctx.Result(inst)});
			return true;
		case IR::ValueOpcode::DppMoveU32: {
			const auto flags    = inst.Flags<IR::DppMoveFlags>();
			const auto target   = EmitDppTargetLane(state, flags.control);
			const auto shuffled = state.builder.AllocateId();
			state.builder.AddFunction({OpGroupNonUniformShuffle, state.uint_type, shuffled,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 0),
			                           target.lane});
			if (flags.fetch_inactive) {
				ctx.Define(inst, shuffled);
				return true;
			}
			const auto ballot = state.builder.AllocateId();
			state.builder.AddFunction({OpGroupNonUniformBallot, state.vec4_uint_type, ballot,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 1)});
			const auto source_active = EmitBallotLaneActiveBool(state, ballot, target.lane);
			const auto can_fetch     = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpLogicalAnd, state.bool_type, can_fetch, target.valid, source_active});
			ctx.Emit(inst, OpSelect, IR::Type::U32, {can_fetch, shuffled, ConstantU32(state, 0)});
			return true;
		}
		case IR::ValueOpcode::DppUpdateU32: {
			const auto flags = inst.Flags<IR::DppMoveFlags>();
			const auto write = EmitDppWriteCondition(ctx, flags, ctx.Arg(inst, 2));
			ctx.Emit(inst, OpSelect, IR::Type::U32, {write, ctx.Arg(inst, 0), ctx.Arg(inst, 1)});
			return true;
		}
		case IR::ValueOpcode::WqmMask:
			ctx.Define(inst, EmitWqm(ctx, ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::LaneId:
			ctx.Define(inst, EmitSubgroupLocalInvocationId(state));
			return true;
		case IR::ValueOpcode::Ballot:
			if (state.per_invocation_masks) {
				const auto result = state.builder.AllocateId();
				state.builder.AddFunction({OpCompositeConstruct, state.vec4_uint_type, result,
				                           BoolAsU32(ctx, ctx.Arg(inst, 0)), ConstantU32(state, 0),
				                           ConstantU32(state, 0), ConstantU32(state, 0)});
				ctx.Define(inst, result);
			} else {
				ctx.Emit(inst, OpGroupNonUniformBallot, IR::Type::U32x4,
				         {ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 0)});
			}
			return true;
		case IR::ValueOpcode::ReadFirstLane: {
			const auto ballot = state.builder.AllocateId();
			const auto lane   = state.builder.AllocateId();
			state.builder.AddFunction({OpGroupNonUniformBallot, state.vec4_uint_type, ballot,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 1)});
			state.builder.AddFunction({OpGroupNonUniformBallotFindLSB, state.uint_type, lane,
			                           ConstantU32(state, ScopeSubgroup), ballot});
			ctx.Emit(inst, OpGroupNonUniformShuffle, IR::Type::U32,
			         {ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 0), lane});
			return true;
		}
		case IR::ValueOpcode::ReadLane:
			ctx.Emit(inst, OpGroupNonUniformShuffle, IR::Type::U32,
			         {ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 0), ctx.Arg(inst, 1)});
			return true;
		case IR::ValueOpcode::WriteLane: {
			const auto hit = state.builder.AllocateId();
			state.builder.AddFunction({OpIEqual, state.bool_type, hit,
			                           EmitSubgroupLocalInvocationId(state), ctx.Arg(inst, 2)});
			ctx.Emit(inst, OpSelect, IR::Type::U32, {hit, ctx.Arg(inst, 1), ctx.Arg(inst, 0)});
			return true;
		}
		case IR::ValueOpcode::Permlane16U32: {
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
			    {OpBitwiseAnd, state.uint_type, row, subid, ConstantU32(state, 0xfffffff0u)});
			if (flags.x16) {
				state.builder.AddFunction(
				    {OpBitwiseXor, state.uint_type, row_value, row, ConstantU32(state, 16)});
			} else {
				state.builder.AddFunction({OpCopyObject, state.uint_type, row_value, row});
			}
			state.builder.AddFunction(
			    {OpBitwiseAnd, state.uint_type, lane, subid, ConstantU32(state, 15)});
			state.builder.AddFunction(
			    {OpBitwiseAnd, state.uint_type, lane8, lane, ConstantU32(state, 7)});
			state.builder.AddFunction(
			    {OpShiftLeftLogical, state.uint_type, shift, lane8, ConstantU32(state, 2)});
			state.builder.AddFunction(
			    {OpUGreaterThanEqual, state.bool_type, upper, lane, ConstantU32(state, 8)});
			state.builder.AddFunction(
			    {OpSelect, state.uint_type, selected, upper, ctx.Arg(inst, 2), ctx.Arg(inst, 1)});
			state.builder.AddFunction(
			    {OpShiftRightLogical, state.uint_type, shifted, selected, shift});
			state.builder.AddFunction(
			    {OpBitwiseAnd, state.uint_type, index, shifted, ConstantU32(state, 15)});
			state.builder.AddFunction({OpBitwiseOr, state.uint_type, target, row_value, index});
			const auto shuffled = state.builder.AllocateId();
			state.builder.AddFunction({OpGroupNonUniformShuffle, state.uint_type, shuffled,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 0),
			                           target});
			uint32_t result = shuffled;
			if (!flags.fetch_inactive) {
				const auto source_exec = state.builder.AllocateId();
				state.builder.AddFunction({OpGroupNonUniformShuffle, state.bool_type, source_exec,
				                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 3),
				                           target});
				result = state.builder.AllocateId();
				state.builder.AddFunction({OpSelect, state.uint_type, result, source_exec, shuffled,
				                           ConstantU32(state, 0)});
			}
			ctx.Define(inst, result);
			return true;
		}
		case IR::ValueOpcode::GetAttribute:
			ctx.Define(inst, EmitAttribute(ctx, inst.Arg(0).U32(), inst.Arg(1).U32()));
			return true;
		case IR::ValueOpcode::SetAttribute: EmitExport(ctx, inst); return true;
		case IR::ValueOpcode::GetShaderBase:
			// Guest S_GETPC values stay shader-relative in SPIR-V, matching the runtime ABI. The
			// runtime descriptor evaluator supplies the mapped shader base for host-side planning.
			ctx.Define(inst, ctx.Def(IR::Value(uint64_t {0})));
			return true;
		case IR::ValueOpcode::GetSrtResource:
		case IR::ValueOpcode::GetBufferResource:
		case IR::ValueOpcode::GetAddressResource:
		case IR::ValueOpcode::GetImageResource:
		case IR::ValueOpcode::GetSamplerResource:
		case IR::ValueOpcode::GetLdsResource:
		case IR::ValueOpcode::GetGdsResource:
		case IR::ValueOpcode::MakeImageAddress: return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
