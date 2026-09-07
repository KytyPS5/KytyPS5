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
	                           pointer, state.wave_scratch_variable, lane});
	return pointer;
}
uint32_t WaveBase(EmitterState& state) {
	if (!state.compute_execution.IsCooperativeWave64()) {
		return ConstantU32(state, 0);
	}
	return EmitBinaryU32(state, OpBitwiseAnd, EmitHostLocalInvocationIndex(state),
	                     ConstantU32(state, ~63u));
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
void WavePublish(EmitterState& state, uint32_t value) {
	state.builder.AddFunction({OpStore, WavePointer(state, EmitHostLocalInvocationIndex(state)), value});
	WaveBarrier(state);
}
uint32_t WordFirst(EmitterState& state, uint32_t word) {
	const auto first_i = state.builder.AllocateId();
	const auto first_u = state.builder.AllocateId();
	state.builder.AddFunction({OpExtInst, TypeI32(state), first_i, GlslStd450(state), GlslFindILsb, word});
	state.builder.AddFunction({OpBitcast, TypeU32(state), first_u, first_i});
	return first_u;
}
} // namespace

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
	// Every actual guest lane has its own host invocation and scratch slot. This
	// does not depend on the driver's mapping of invocations to native subgroups.
	const auto lane = EmitHostLocalInvocationIndex(state);
	const auto bit_lane = EmitBinaryU32(state, OpBitwiseAnd, lane, ConstantU32(state, 31));
	const auto bit = EmitBinaryU32(state, OpShiftLeftLogical, ConstantU32(state, 1), bit_lane);
	const auto contribution = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, TypeU32(state), contribution, predicate, bit, ConstantU32(state, 0)});
	WavePublish(state, contribution);
	const auto wave_base = WaveBase(state);
	std::array<uint32_t, 2> words{ConstantU32(state, 0), ConstantU32(state, 0)};
	for (uint32_t index = 0; index < 64; ++index) {
		words[index / 32] = EmitBinaryU32(state, OpBitwiseOr, words[index / 32],
		                                 WaveLoad(state, ConstantU32(state, index), wave_base));
	}
	// No invocation may overwrite this shared array until all peers have read it.
	WaveBarrier(state);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 4), result,
	                           words[0], words[1], ConstantU32(state, 0), ConstantU32(state, 0)});
	return result;
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
	WavePublish(state, source);
	// DPP can calculate an invalid boundary target before its destination rule
	// selects zero/preservation. Never issue an out-of-bounds scratch load even
	// for that discarded value; valid logical lanes retain their exact index.
	const auto valid = state.builder.AllocateId();
	const auto safe_target = state.builder.AllocateId();
	state.builder.AddFunction({OpULessThan, TypeBool(state), valid, target, ConstantU32(state, 64)});
	state.builder.AddFunction({OpSelect, TypeU32(state), safe_target, valid, target, ConstantU32(state, 0)});
	// Validate the logical target before adding this wave's scratch base.
	const auto loaded = WaveLoad(state, safe_target, WaveBase(state));
	WaveBarrier(state);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, TypeU32(state), result, valid, loaded, ConstantU32(state, 0)});
	return result;
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
