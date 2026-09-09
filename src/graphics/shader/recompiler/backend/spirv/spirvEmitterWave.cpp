#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {
bool IsPartitionedGraphicsWave64(const EmitterState& state) {
	return state.stage != ShaderType::Compute && state.wave_size == 64u &&
	       state.native_subgroup_size == 32u;
}
void WaveBarrier(EmitterState& state) {
	state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
	                           ConstantU32(state, ScopeWorkgroup), ConstantU32(state,
	                           MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory)});
}
uint32_t WavePointer(EmitterState& state, uint32_t lane) {
	EXIT_IF(state.wave_scratch_variable == 0);
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeU32ElementPointer(state, StorageClassWorkgroup),
	                           pointer, state.wave_scratch_variable,
	                           EmitWaveScratchIndex(state, lane)});
	return pointer;
}
uint32_t WaveBallotWordPointer(EmitterState& state, uint32_t lane) {
	// Keep the aggregate words after the per-lane scratch area. Each invocation
	// selects its half dynamically, but the reset and accumulation are separated
	// by a workgroup barrier so the reset cannot race a contribution.
	const auto word = EmitBinaryU32(
	    state, OpBitwiseAnd,
	    EmitBinaryU32(state, OpShiftRightLogical, lane, ConstantU32(state, 5)),
	    ConstantU32(state, 1));
	const auto wave = state.compute_execution.IsCooperativeWave64()
	                      ? EmitBinaryU32(state, OpShiftRightLogical, lane,
	                                      ConstantU32(state, 6))
	                      : ConstantU32(state, 0);
	const auto wave_offset = EmitBinaryU32(state, OpShiftLeftLogical, wave, ConstantU32(state, 1));
	const auto relative = EmitBinaryU32(
	    state, OpIAdd, ConstantU32(state, state.wave_ballot_base_dwords),
	    EmitBinaryU32(state, OpIAdd, wave_offset, word));
	return WavePointer(state, relative);
}
uint32_t WaveBallotWordPointer(EmitterState& state, uint32_t wave_base, uint32_t word) {
	const auto wave = state.compute_execution.IsCooperativeWave64()
	                      ? EmitBinaryU32(state, OpShiftRightLogical, wave_base,
	                                      ConstantU32(state, 6))
	                      : ConstantU32(state, 0);
	const auto wave_offset = EmitBinaryU32(state, OpShiftLeftLogical, wave, ConstantU32(state, 1));
	const auto relative = EmitBinaryU32(
	    state, OpIAdd, ConstantU32(state, state.wave_ballot_base_dwords),
	    EmitBinaryU32(state, OpIAdd, wave_offset, ConstantU32(state, word)));
	return WavePointer(state, relative);
}
uint32_t WaveBase(EmitterState& state, uint32_t lane) {
	if (!state.compute_execution.IsCooperativeWave64()) {
		return ConstantU32(state, 0);
	}
	return EmitBinaryU32(state, OpBitwiseAnd, lane, ConstantU32(state, ~63u));
}
uint32_t WaveLoad(EmitterState& state, uint32_t lane, uint32_t wave_base) {
	// Cooperative workgroups contain multiple logical waves. Only reads are
	// wave-relative: publishing already uses the full host invocation index.
	const auto index = state.compute_execution.IsCooperativeWave64()
	                       ? EmitBinaryU32(state, OpIAdd, wave_base, lane) : lane;
	const auto pointer = WavePointer(state, index);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, TypeU32(state), result, pointer});
	return result;
}
uint32_t WaveBallotWordLoad(EmitterState& state, uint32_t wave_base, uint32_t word) {
	const auto pointer = WaveBallotWordPointer(state, wave_base, word);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, TypeU32(state), result, pointer});
	return result;
}
void WavePublish(EmitterState& state, uint32_t value, uint32_t lane) {
	state.builder.AddFunction({OpStore, WavePointer(state, lane), value});
	WaveBarrier(state);
}
uint32_t WordFirst(EmitterState& state, uint32_t word) {
	const auto first_i = state.builder.AllocateId();
	const auto first_u = state.builder.AllocateId();
	state.builder.AddFunction({OpExtInst, TypeI32(state), first_i, GlslStd450(state), GlslFindILsb, word});
	state.builder.AddFunction({OpBitcast, TypeU32(state), first_u, first_i});
	return first_u;
}

uint32_t EmitSplitWaveBallotBody(EmitterState& state, uint32_t predicate, uint32_t lane) {
	// A native subgroup is one half of the guest wave64 on the admitted split
	// workgroup. Reduce each half with a subgroup bitwise OR, then publish one
	// aggregate word through workgroup memory so every lane can see both halves.
	const auto bit_lane = EmitBinaryU32(state, OpBitwiseAnd, lane, ConstantU32(state, 31));
	const auto bit = EmitBinaryU32(state, OpShiftLeftLogical, ConstantU32(state, 1), bit_lane);
	const auto contribution = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, TypeU32(state), contribution, predicate, bit,
	                           ConstantU32(state, 0)});
	const auto subgroup_word = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformBitwiseOr, TypeU32(state), subgroup_word,
	                           ConstantU32(state, ScopeSubgroup), GroupOperationReduce,
	                           contribution});
	const auto subgroup_lane = EmitBinaryU32(state, OpBitwiseAnd, lane, ConstantU32(state, 31));
	const auto leader = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpIEqual, TypeBool(state), leader, subgroup_lane, ConstantU32(state, 0)});
	EmitIfCondition(state, leader, [&]() {
		const auto pointer = WaveBallotWordPointer(state, lane);
		state.builder.AddFunction({OpStore, pointer, subgroup_word});
	});
	WaveBarrier(state);
	const auto wave_base = WaveBase(state, lane);
	std::array<uint32_t, 2> words{WaveBallotWordLoad(state, wave_base, 0),
	                               WaveBallotWordLoad(state, wave_base, 1)};
	WaveBarrier(state);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 4), result,
	                           words[0], words[1], ConstantU32(state, 0), ConstantU32(state, 0)});
	return result;
}

uint32_t EmitSplitWaveReadLaneBody(EmitterState& state, uint32_t source, uint32_t target,
	                               uint32_t lane) {
	WavePublish(state, source, lane);
	// DPP can calculate an invalid boundary target before its destination rule
	// selects zero/preservation. Never issue an out-of-bounds scratch load even
	// for that discarded value; valid logical lanes retain their exact index.
	const auto valid = state.builder.AllocateId();
	const auto safe_target = state.builder.AllocateId();
	state.builder.AddFunction({OpULessThan, TypeBool(state), valid, target, ConstantU32(state, 64)});
	state.builder.AddFunction({OpSelect, TypeU32(state), safe_target, valid, target, ConstantU32(state, 0)});
	// Validate the logical target before adding this wave's scratch base.
	const auto loaded = WaveLoad(state, safe_target, WaveBase(state, lane));
	WaveBarrier(state);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, TypeU32(state), result, valid, loaded, ConstantU32(state, 0)});
	return result;
}
} // namespace

void DefineCooperativeWaveFunctions(EmitterState& state) {
	if (!state.compute_execution.IsCooperativeWave64()) {
		return;
	}
	if (state.requirements.subgroup_ballot) {
		const auto function_type = state.builder.Type(
		    OpTypeFunction, {TypeU32Vector(state, 4), TypeBool(state), TypeU32(state)});
		state.wave_ballot_function = state.builder.AllocateId();
		const auto predicate = state.builder.AllocateId();
		const auto lane = state.builder.AllocateId();
		const auto entry = state.builder.AllocateId();
		state.builder.AddName(state.wave_ballot_function, "cooperative_wave64_ballot");
		state.builder.AddFunction({OpFunction, TypeU32Vector(state, 4),
		                           state.wave_ballot_function, FunctionControlNone, function_type});
		state.builder.AddFunction({OpFunctionParameter, TypeBool(state), predicate});
		state.builder.AddFunction({OpFunctionParameter, TypeU32(state), lane});
		EmitLabel(state, entry);
		state.builder.AddFunction({OpReturnValue,
		                           EmitSplitWaveBallotBody(state, predicate, lane)});
		state.builder.AddFunction({OpFunctionEnd});
	}
	if (state.requirements.subgroup_shuffle) {
		const auto function_type = state.builder.Type(
		    OpTypeFunction, {TypeU32(state), TypeU32(state), TypeU32(state), TypeU32(state)});
		state.wave_read_lane_function = state.builder.AllocateId();
		const auto source = state.builder.AllocateId();
		const auto target = state.builder.AllocateId();
		const auto lane = state.builder.AllocateId();
		const auto entry = state.builder.AllocateId();
		state.builder.AddName(state.wave_read_lane_function, "cooperative_wave64_read_lane");
		state.builder.AddFunction({OpFunction, TypeU32(state), state.wave_read_lane_function,
		                           FunctionControlNone, function_type});
		state.builder.AddFunction({OpFunctionParameter, TypeU32(state), source});
		state.builder.AddFunction({OpFunctionParameter, TypeU32(state), target});
		state.builder.AddFunction({OpFunctionParameter, TypeU32(state), lane});
		EmitLabel(state, entry);
		state.builder.AddFunction({OpReturnValue,
		                           EmitSplitWaveReadLaneBody(state, source, target, lane)});
		state.builder.AddFunction({OpFunctionEnd});
	}
}

uint32_t EmitWaveBallot(EmitterState& state, uint32_t predicate) {
	if (!state.compute_execution.IsSplitWave64()) {
		const auto native = state.builder.AllocateId();
		state.builder.AddFunction({OpGroupNonUniformBallot, TypeU32Vector(state, 4), native,
		                           ConstantU32(state, ScopeSubgroup), predicate});
		if (!IsPartitionedGraphicsWave64(state)) {
			return native;
		}
		// Fragment and vertex stages cannot rendezvous two physical subgroups through
		// workgroup memory. Keep each 32-lane partition internally coherent and expose
		// its predicate bits through both halves of the guest wave64 mask.
		const auto bits = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeExtract, TypeU32(state), bits, native, 0});
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 4), result,
		                           bits, bits, ConstantU32(state, 0), ConstantU32(state, 0)});
		return result;
	}
	const auto lane = EmitHostLocalInvocationIndex(state);
	if (state.wave_ballot_function != 0) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction({OpFunctionCall, TypeU32Vector(state, 4), result,
		                           state.wave_ballot_function, predicate, lane});
		return result;
	}
	return EmitSplitWaveBallotBody(state, predicate, lane);
}

uint32_t NormalizeWaveLaneTarget(EmitterState& state, uint32_t target) {
	if (!IsPartitionedGraphicsWave64(state)) {
		return target;
	}
	// Map either guest half to the corresponding lane of this native half.
	return EmitBinaryU32(state, OpBitwiseAnd, target, ConstantU32(state, 31));
}

uint32_t EmitWaveReadLane(EmitterState& state, uint32_t source, uint32_t target) {
	if (!state.compute_execution.IsSplitWave64()) {
		target = NormalizeWaveLaneTarget(state, target);
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction({OpGroupNonUniformShuffle, TypeU32(state), result,
		                           ConstantU32(state, ScopeSubgroup), source, target});
		return result;
	}
	const auto lane = EmitHostLocalInvocationIndex(state);
	if (state.wave_read_lane_function != 0) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction({OpFunctionCall, TypeU32(state), result,
		                           state.wave_read_lane_function, source, target, lane});
		return result;
	}
	return EmitSplitWaveReadLaneBody(state, source, target, lane);
}

uint32_t EmitWaveFindFirst(EmitterState& state, uint32_t ballot) {
	if (!state.compute_execution.IsSplitWave64()) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction({OpGroupNonUniformBallotFindLSB, TypeU32(state), result,
		                           ConstantU32(state, ScopeSubgroup), ballot});
		return result;
	}
	const auto low = state.builder.AllocateId();
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, TypeU32(state), low, ballot, 0});
	state.builder.AddFunction({OpCompositeExtract, TypeU32(state), high, ballot, 1});
	const auto low_first = WordFirst(state, low);
	const auto high_first = EmitBinaryU32(state, OpIAdd, WordFirst(state, high), ConstantU32(state, 32));
	const auto low_empty = state.builder.AllocateId();
	const auto high_empty = state.builder.AllocateId();
	const auto upper = state.builder.AllocateId();
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpIEqual, TypeBool(state), low_empty, low, ConstantU32(state, 0)});
	state.builder.AddFunction({OpIEqual, TypeBool(state), high_empty, high, ConstantU32(state, 0)});
	state.builder.AddFunction({OpSelect, TypeU32(state), upper, high_empty, ConstantU32(state, UINT32_MAX), high_first});
	state.builder.AddFunction({OpSelect, TypeU32(state), result, low_empty, upper, low_first});
	return result;
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
