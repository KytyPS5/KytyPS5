#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"


namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

namespace {

uint32_t Binary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t lhs, uint32_t rhs) {
	const auto id = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, id, lhs, rhs});
	return id;
}

uint32_t Select(EmitterState& state, uint32_t type, uint32_t condition, uint32_t when_true,
                uint32_t when_false) {
	const auto id = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, type, id, condition, when_true, when_false});
	return id;
}

} // namespace

void PrepareLogicalWave64Storage(EmitterState& state) {
	if (!state.logical_wave64 || state.wave_exchange_variable != 0) {
		return;
	}
	state.wave_exchange_variable = state.builder.DefineGlobalVariable(
	    TypeU32ArrayPointer(state, StorageClassWorkgroup, LogicalWave64Lanes),
	    StorageClassWorkgroup);
	state.builder.AddName(state.wave_exchange_variable, "wave_exchange");

	state.wave_ballot_variable = state.builder.DefineGlobalVariable(
	    TypeU32ArrayPointer(state, StorageClassWorkgroup, 2), StorageClassWorkgroup);
	state.builder.AddName(state.wave_ballot_variable, "wave_ballot");
}

void EmitLogicalWaveBarrier(EmitterState& state) {
	const auto semantics = MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory;
	state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
	                           ConstantU32(state, ScopeWorkgroup),
	                           ConstantU32(state, semantics)});
}

void RejectUnsupportedLogicalWave64(const EmitterState& state, const char* operation) {
	if (!state.logical_wave64) {
		return;
	}
	EXIT("logical wave64: %s is not lowered for a 64-lane workgroup (shader hash=0x%016" PRIx64
	     "). It would resolve against one 32-lane host subgroup and silently return a wrong "
	     "result.\n",
	     operation, state.program.shader_hash);
}

uint32_t EmitCurrentLaneId(EmitterState& state) {
	return state.logical_wave64 ? EmitLogicalLaneId(state) : EmitSubgroupLocalInvocationId(state);
}

uint32_t EmitLogicalLaneId(EmitterState& state) {
	if (state.local_invocation_index_variable == 0) {
		if (InputVariableForKind(state, IR::StageInputKind::LocalInvocationIndex) != 0) {
			return EmitLocalInvocationIndex(state);
		}
		EXIT("LocalInvocationIndex was not declared before SPIR-V function emission\n");
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpLoad, TypeU32(state), value, state.local_invocation_index_variable});
	return value;
}

static uint32_t ExchangeSlot(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain,
	                           TypeU32ElementPointer(state, StorageClassWorkgroup), pointer,
	                           state.wave_exchange_variable, index});
	return pointer;
}

static uint32_t BallotSlot(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeU32ElementPointer(state, StorageClassWorkgroup),
	                           pointer, state.wave_ballot_variable, index});
	return pointer;
}

uint32_t EmitLogicalBallot(EmitterState& state, uint32_t condition) {
	PrepareLogicalWave64Storage(state);

	const auto lane = EmitLogicalLaneId(state);

	const auto is_first =
	    Binary(state, OpIEqual, TypeBool(state), lane, ConstantU32(state, 0));
	const auto clear_label = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, is_first, clear_label, merge_label});
	EmitLabel(state, clear_label);
	state.builder.AddFunction({OpStore, BallotSlot(state, ConstantU32(state, 0)),
	                           ConstantU32(state, 0)});
	state.builder.AddFunction({OpStore, BallotSlot(state, ConstantU32(state, 1)),
	                           ConstantU32(state, 0)});
	state.builder.AddFunction({OpBranch, merge_label});
	EmitLabel(state, merge_label);

	EmitLogicalWaveBarrier(state);

	const auto half = Binary(state, OpShiftRightLogical, TypeU32(state), lane,
	                         ConstantU32(state, 5));
	const auto bit_index =
	    Binary(state, OpBitwiseAnd, TypeU32(state), lane, ConstantU32(state, 31));
	const auto bit = Binary(state, OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	                        bit_index);
	const auto contribution = Select(state, TypeU32(state), condition, bit,
	                                 ConstantU32(state, 0));

	const auto atomic_semantics = MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory;
	const auto discard          = state.builder.AllocateId();
	state.builder.AddFunction({OpAtomicOr, TypeU32(state), discard, BallotSlot(state, half),
	                           ConstantU32(state, ScopeWorkgroup),
	                           ConstantU32(state, atomic_semantics), contribution});

	EmitLogicalWaveBarrier(state);

	const auto low  = state.builder.AllocateId();
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpLoad, TypeU32(state), low, BallotSlot(state, ConstantU32(state, 0))});
	state.builder.AddFunction(
	    {OpLoad, TypeU32(state), high, BallotSlot(state, ConstantU32(state, 1))});

	EmitLogicalWaveBarrier(state);

	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 4), result, low, high,
	                           ConstantU32(state, 0), ConstantU32(state, 0)});
	return result;
}

uint32_t EmitLogicalReadLane(EmitterState& state, uint32_t value, uint32_t lane) {
	PrepareLogicalWave64Storage(state);

	const auto self = EmitLogicalLaneId(state);
	state.builder.AddFunction({OpStore, ExchangeSlot(state, self), value});

	EmitLogicalWaveBarrier(state);

	const auto index = Binary(state, OpBitwiseAnd, TypeU32(state), lane,
	                          ConstantU32(state, LogicalWave64Lanes - 1));
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, TypeU32(state), result, ExchangeSlot(state, index)});

	EmitLogicalWaveBarrier(state);
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
