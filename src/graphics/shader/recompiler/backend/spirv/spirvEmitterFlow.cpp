#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

bool UserDataDwordIndex(const EmitterState& state, IR::Register reg, uint32_t& dword_index) {
	if (reg.file != IR::RegisterFile::Scalar) {
		return false;
	}
	const auto& registers = state.program.bindings.user_data_registers;
	const auto  found     = std::lower_bound(registers.begin(), registers.end(), reg.index);
	if (found == registers.end() || *found != reg.index) {
		return false;
	}
	dword_index = static_cast<uint32_t>(found - registers.begin());
	return true;
}

uint32_t EmitWqmWordU32(EmitterState& state, uint32_t value) {
	const auto shifted_one = state.builder.AllocateId();
	const auto merged_one  = state.builder.AllocateId();
	const auto shifted_two = state.builder.AllocateId();
	const auto merged_two  = state.builder.AllocateId();
	const auto quad_bits   = state.builder.AllocateId();
	const auto result      = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpShiftRightLogical, TypeU32(state), shifted_one, value, ConstantU32(state, 1)});
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), merged_one, value, shifted_one});
	state.builder.AddFunction(
	    {OpShiftRightLogical, TypeU32(state), shifted_two, merged_one, ConstantU32(state, 2)});
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), merged_two, merged_one, shifted_two});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), quad_bits, merged_two, ConstantU32(state, 0x11111111u)});
	state.builder.AddFunction(
	    {OpIMul, TypeU32(state), result, quad_bits, ConstantU32(state, 0x0fu)});
	return result;
}

uint32_t EmitWqmU64(EmitterState& state, uint32_t value) {
	const auto shifted_one = state.builder.AllocateId();
	const auto merged_one  = state.builder.AllocateId();
	const auto shifted_two = state.builder.AllocateId();
	const auto merged_two  = state.builder.AllocateId();
	const auto quad_bits   = state.builder.AllocateId();
	const auto result      = state.builder.AllocateId();
	state.builder.AddFunction({OpShiftRightLogical, TypeU64(state), shifted_one, value,
	                           ConstantU64(state, 0x0000000100000001ull)});
	state.builder.AddFunction({OpBitwiseOr, TypeU64(state), merged_one, value, shifted_one});
	state.builder.AddFunction({OpShiftRightLogical, TypeU64(state), shifted_two, merged_one,
	                           ConstantU64(state, 0x0000000200000002ull)});
	state.builder.AddFunction({OpBitwiseOr, TypeU64(state), merged_two, merged_one, shifted_two});
	state.builder.AddFunction({OpBitwiseAnd, TypeU64(state), quad_bits, merged_two,
	                           ConstantU64(state, 0x1111111111111111ull)});
	state.builder.AddFunction(
	    {OpIMul, TypeU64(state), result, quad_bits, ConstantU64(state, 0x0000000f0000000full)});
	return result;
}

uint32_t BoolAsU32(ValueEmitContext& ctx, uint32_t value) {
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpSelect, TypeU32(ctx.state), result, value,
	                               ConstantU32(ctx.state, 1), ConstantU32(ctx.state, 0)});
	return result;
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
		state.builder.AddFunction({OpLoad, TypeBool(state), value, variable});
		state.builder.AddFunction(
		    {OpSelect, TypeU32(state), bits, value, ConstantU32(state, 1), ConstantU32(state, 0)});
		return bits;
	}
	if (kind == IR::StageInputKind::VertexIndex || kind == IR::StageInputKind::InstanceIndex) {
		const auto value = state.builder.AllocateId();
		const auto bits  = state.builder.AllocateId();
		state.builder.AddFunction({OpLoad, TypeI32(state), value, variable});
		state.builder.AddFunction({OpBitcast, TypeU32(state), bits, value});
		return bits;
	}
	if (kind == IR::StageInputKind::FragCoord) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		const auto bits    = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain,
		                           TypePointer(state, StorageClassInput, TypeF32(state)), pointer,
		                           variable, ConstantU32(state, component)});
		state.builder.AddFunction({OpLoad, TypeF32(state), value, pointer});
		state.builder.AddFunction({OpBitcast, TypeU32(state), bits, value});
		return bits;
	}
	return EmitInputComponentU32(state, kind, component);
}

uint32_t EmitWqm(ValueEmitContext& ctx, uint32_t active) {
	auto&      state  = ctx.state;
	const auto ballot = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
	                           ConstantU32(state, ScopeSubgroup), active});
	const auto low = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, TypeU32(state), low, ballot, 0});
	const auto wqm_low  = EmitWqmWordU32(state, low);
	const auto lane     = EmitSubgroupLocalInvocationId(state);
	uint32_t   mask     = wqm_low;
	uint32_t   bit_lane = lane;
	if (state.wave_size == 64u) {
		const auto high = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeExtract, TypeU32(state), high, ballot, 1});
		const auto wqm_high = EmitWqmWordU32(state, high);
		const auto upper    = state.builder.AllocateId();
		mask                = state.builder.AllocateId();
		bit_lane            = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpUGreaterThanEqual, TypeBool(state), upper, lane, ConstantU32(state, 32)});
		state.builder.AddFunction({OpSelect, TypeU32(state), mask, upper, wqm_high, wqm_low});
		state.builder.AddFunction(
		    {OpBitwiseAnd, TypeU32(state), bit_lane, lane, ConstantU32(state, 31)});
	}
	const auto bit    = state.builder.AllocateId();
	const auto hit    = state.builder.AllocateId();
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpShiftLeftLogical, TypeU32(state), bit, ConstantU32(state, 1), bit_lane});
	state.builder.AddFunction({OpBitwiseAnd, TypeU32(state), hit, mask, bit});
	state.builder.AddFunction({OpINotEqual, TypeBool(state), result, hit, ConstantU32(state, 0)});
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
	    {OpShiftRightLogical, TypeU32(state), bank_shift, lane, ConstantU32(state, 2)});
	state.builder.AddFunction(
	    {OpShiftRightLogical, TypeU32(state), row_shift, lane, ConstantU32(state, 4)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), bank, bank_shift, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), row, row_shift, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {OpShiftLeftLogical, TypeU32(state), bank_bit, ConstantU32(state, 1), bank});
	state.builder.AddFunction(
	    {OpShiftLeftLogical, TypeU32(state), row_bit, ConstantU32(state, 1), row});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), bank_hit, ConstantU32(state, flags.bank_mask), bank_bit});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), row_hit, ConstantU32(state, flags.row_mask), row_bit});
	state.builder.AddFunction(
	    {OpINotEqual, TypeBool(state), bank_ok, bank_hit, ConstantU32(state, 0)});
	state.builder.AddFunction(
	    {OpINotEqual, TypeBool(state), row_ok, row_hit, ConstantU32(state, 0)});
	state.builder.AddFunction({OpLogicalAnd, TypeBool(state), masks_ok, bank_ok, row_ok});
	uint32_t writable = masks_ok;
	if (!flags.bound_control) {
		const auto target  = EmitDppTargetLane(state, flags.control);
		const auto bounded = state.builder.AllocateId();
		state.builder.AddFunction({OpLogicalAnd, TypeBool(state), bounded, writable, target.valid});
		writable = bounded;
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalAnd, TypeBool(state), result, exec, writable});
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
	state.builder.AddFunction({OpLoad, TypeF32Vector(state, 4), vector, input->variable_id});
	state.builder.AddFunction({OpCompositeExtract, TypeF32(state), component, vector, chan & 3u});
	state.builder.AddFunction({OpBitcast, TypeU32(state), bits, component});
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
	    {OpCompositeExtract, TypeU32(ctx.state), value, vector, component});
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
			state.builder.AddFunction({OpCompositeExtract, TypeU32(state), packed, data, pair});
			state.builder.AddFunction({OpExtInst, TypeF32Vector(state, 2), unpacked,
			                           GlslStd450(state), GlslUnpackHalf2x16, packed});
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair * 2u + lane;
				if (((exp.en >> component) & 1u) != 0u) {
					f32[component] = state.builder.AllocateId();
					state.builder.AddFunction(
					    {OpCompositeExtract, TypeF32(state), f32[component], unpacked, lane});
				}
			}
		}
		const auto vector = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeConstruct, TypeF32Vector(state, 4), vector, f32[0],
		                           f32[1], f32[2], f32[3]});
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
				state.builder.AddFunction({OpBitFieldUExtract, TypeU32(state), raw[component],
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
		state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 4), vector, raw[0],
		                           raw[1], raw[2], raw[3]});
		return vector;
	}
	uint32_t f32[4] {};
	for (uint32_t component = 0; component < 4u; component++) {
		f32[component] = state.builder.AllocateId();
		state.builder.AddFunction({OpBitcast, TypeF32(state), f32[component], raw[component]});
	}
	const auto vector = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpCompositeConstruct, TypeF32Vector(state, 4), vector, f32[0], f32[1], f32[2], f32[3]});
	return vector;
}

void EmitExport(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state = ctx.state;
	const auto& exp   = ctx.Export(inst);
	const auto  exec  = ctx.Arg(inst, 1);
	if (state.stage == ShaderType::Pixel && exp.vm && state.requirements.pixel_valid_mask &&
	    state.pixel_valid_mask_variable != 0) {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpSelect, TypeU32(state), value, exec, ConstantU32(state, 1), ConstantU32(state, 0)});
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
				state.builder.AddFunction({OpBitcast, TypeF32(state), f32, raw});
				state.builder.AddFunction({OpStore, state.depth_variable, f32});
			}
			if ((exp.en & 4u) != 0u && state.sample_mask_variable != 0) {
				const auto raw     = ExportRawComponent(ctx, data, 2);
				const auto value   = state.builder.AllocateId();
				const auto pointer = state.builder.AllocateId();
				state.builder.AddFunction({OpBitcast, TypeI32(state), value, raw});
				state.builder.AddFunction(
				    {OpAccessChain, TypePointer(state, StorageClassOutput, TypeI32(state)), pointer,
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
		const auto vector_type = uint_output ? TypeU32Vector(state, 4) : TypeF32Vector(state, 4);
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
			state.builder.AddFunction(
			    {OpAccessChain, TypePointer(state, StorageClassOutput, TypeF32Vector(state, 4)),
			     pointer, variable, ConstantU32(state, 0)});
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
		case IR::ValueOpcode::Void:
		case IR::ValueOpcode::Reference:
		case IR::ValueOpcode::ReferenceU32:
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
			state.builder.AddFunction({OpGroupNonUniformShuffle, TypeU32(state), shuffled,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 0),
			                           target.lane});
			if (flags.fetch_inactive) {
				ctx.Define(inst, shuffled);
				return true;
			}
			const auto ballot = state.builder.AllocateId();
			state.builder.AddFunction({OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 1)});
			const auto source_active = EmitBallotLaneActiveBool(state, ballot, target.lane);
			const auto can_fetch     = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpLogicalAnd, TypeBool(state), can_fetch, target.valid, source_active});
			ctx.Emit(inst, OpSelect, IR::Type::U32, {can_fetch, shuffled, ConstantU32(state, 0)});
			return true;
		}
		case IR::ValueOpcode::DppUpdateU32: {
			const auto flags = inst.Flags<IR::DppMoveFlags>();
			const auto write = EmitDppWriteCondition(ctx, flags, ctx.Arg(inst, 2));
			ctx.Emit(inst, OpSelect, IR::Type::U32, {write, ctx.Arg(inst, 0), ctx.Arg(inst, 1)});
			return true;
		}
		case IR::ValueOpcode::WqmU64:
			ctx.Define(inst, EmitWqmU64(ctx.state, ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::WqmMask:
			ctx.Define(inst, EmitWqm(ctx, ctx.Arg(inst, 0)));
			return true;
		case IR::ValueOpcode::LaneId:
			ctx.Define(inst, EmitSubgroupLocalInvocationId(state));
			return true;
		case IR::ValueOpcode::Ballot:
			if (state.per_invocation_masks) {
				const auto result = state.builder.AllocateId();
				state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 4), result,
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
			state.builder.AddFunction({OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 1)});
			state.builder.AddFunction({OpGroupNonUniformBallotFindLSB, TypeU32(state), lane,
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
			state.builder.AddFunction({OpIEqual, TypeBool(state), hit,
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
			    {OpBitwiseAnd, TypeU32(state), row, subid, ConstantU32(state, 0xfffffff0u)});
			if (flags.x16) {
				state.builder.AddFunction(
				    {OpBitwiseXor, TypeU32(state), row_value, row, ConstantU32(state, 16)});
			} else {
				state.builder.AddFunction({OpCopyObject, TypeU32(state), row_value, row});
			}
			state.builder.AddFunction(
			    {OpBitwiseAnd, TypeU32(state), lane, subid, ConstantU32(state, 15)});
			state.builder.AddFunction(
			    {OpBitwiseAnd, TypeU32(state), lane8, lane, ConstantU32(state, 7)});
			state.builder.AddFunction(
			    {OpShiftLeftLogical, TypeU32(state), shift, lane8, ConstantU32(state, 2)});
			state.builder.AddFunction(
			    {OpUGreaterThanEqual, TypeBool(state), upper, lane, ConstantU32(state, 8)});
			state.builder.AddFunction(
			    {OpSelect, TypeU32(state), selected, upper, ctx.Arg(inst, 2), ctx.Arg(inst, 1)});
			state.builder.AddFunction(
			    {OpShiftRightLogical, TypeU32(state), shifted, selected, shift});
			state.builder.AddFunction(
			    {OpBitwiseAnd, TypeU32(state), index, shifted, ConstantU32(state, 15)});
			state.builder.AddFunction({OpBitwiseOr, TypeU32(state), target, row_value, index});
			const auto shuffled = state.builder.AllocateId();
			state.builder.AddFunction({OpGroupNonUniformShuffle, TypeU32(state), shuffled,
			                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 0),
			                           target});
			uint32_t result = shuffled;
			if (!flags.fetch_inactive) {
				const auto source_exec = state.builder.AllocateId();
				state.builder.AddFunction({OpGroupNonUniformShuffle, TypeBool(state), source_exec,
				                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 3),
				                           target});
				result = state.builder.AllocateId();
				state.builder.AddFunction({OpSelect, TypeU32(state), result, source_exec, shuffled,
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
		case IR::ValueOpcode::MakeImageAddress: return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
