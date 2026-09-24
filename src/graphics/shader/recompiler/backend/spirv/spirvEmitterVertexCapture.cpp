#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

void DefineVertexCapture(EmitterState& state) {
	const auto& capture = *state.vertex_capture;
	EXIT_IF(capture.record_stride_vec4 == 0 ||
	        capture.num_attributes > ShaderVertexInputInfo::RES_MAX);
	EXIT_IF(capture.clip_slot != UINT32_MAX && capture.clip_slot >= capture.record_stride_vec4);
	for (const auto& [location, slot]: capture.parameter_slots) {
		EXIT_IF(slot == 0 || slot >= capture.record_stride_vec4 || slot == capture.clip_slot);
	}
	for (const auto& input: state.program.info.inputs) {
		EXIT_IF(input.kind != IR::StageInputKind::VertexIndex &&
		        input.kind != IR::StageInputKind::InstanceIndex &&
		        input.kind != IR::StageInputKind::Parameter);
		EXIT_IF(input.kind == IR::StageInputKind::Parameter &&
		        input.location >= capture.num_attributes);
	}
	for (uint32_t binding = 0; binding < 3; binding++) {
		const auto components = binding == 2 ? 2u : 4u;
		const auto element =
		    binding == 1 ? TypeF32Vector(state, 4) : TypeU32Vector(state, components);
		const auto array = state.builder.DecoratedType(
		    spv::OpTypeRuntimeArray,
		    {{spv::OpDecorate, {spv::DecorationArrayStride, components * 4}}}, element);
		const auto block =
		    state.builder.DecoratedType(spv::OpTypeStruct,
		                                {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
		                                 {spv::OpDecorate, {spv::DecorationBlock}}},
		                                array);
		const auto variable = state.builder.DefineGlobalVariable(
		    TypePointer(state, spv::StorageClassStorageBuffer, block),
		    spv::StorageClassStorageBuffer);
		state.capture_buffers[binding] = variable;
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, 1);
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, binding);
		if (binding != 1)
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationNonWritable);
	}
}

void InitializeVertexCapture(EmitterState& state) {
	if (state.vertex_capture == nullptr) return;
	const auto count = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpArrayLength, TypeU32(state), count, state.capture_buffers[2],
	                          0);
	const auto group = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
	const auto base  = EmitBinaryU32(state, spv::OpIMul, group, ConstantU32(state, 64));
	const auto low   = EmitAddU32(state, base, EmitLocalInvocationIndex(state));
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.capture_index[half] =
		    half == 0 ? low : EmitAddU32(state, low, ConstantU32(state, 32));
		state.capture_valid[half] =
		    Binary(state, spv::OpULessThan, TypeBool(state), state.capture_index[half], count);
	}
}

uint32_t MaskCaptureExecution(EmitterState& state, uint32_t predicate, uint32_t half) {
	return state.vertex_capture == nullptr ? predicate
	                                       : Binary(state, spv::OpLogicalAnd, TypeBool(state),
	                                                predicate, state.capture_valid[half]);
}

uint32_t CaptureInput(EmitterState& state, bool attribute, uint32_t location, uint32_t component) {
	// Inactive tail lanes still participate in subgroup operations, but never read past the draw.
	const auto index = Select(state, TypeU32(state), state.capture_valid[state.lane_half],
	                          state.capture_index[state.lane_half], ConstantU32(state, 0));
	const auto record =
	    attribute
	        ? EmitAddU32(state,
	                     EmitBinaryU32(state, spv::OpIMul, index,
	                                   ConstantU32(state, state.vertex_capture->num_attributes)),
	                     ConstantU32(state, location))
	        : index;
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.capture_buffers[attribute ? 0 : 2], ConstantU32(state, 0),
	                          record, ConstantU32(state, component));
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

uint32_t CaptureOutputPointer(EmitterState& state, uint32_t slot, uint32_t component) {
	const auto index =
	    EmitAddU32(state,
	               EmitBinaryU32(state, spv::OpIMul, state.capture_index[state.lane_half],
	                             ConstantU32(state, state.vertex_capture->record_stride_vec4)),
	               ConstantU32(state, slot));
	const auto pointer = state.builder.AllocateId();
	if (component == UINT32_MAX) {
		state.builder.AddFunction(
		    spv::OpAccessChain,
		    TypePointer(state, spv::StorageClassStorageBuffer, TypeF32Vector(state, 4)), pointer,
		    state.capture_buffers[1], ConstantU32(state, 0), index);
	} else {
		state.builder.AddFunction(
		    spv::OpAccessChain, TypePointer(state, spv::StorageClassStorageBuffer, TypeF32(state)),
		    pointer, state.capture_buffers[1], ConstantU32(state, 0), index,
		    ConstantU32(state, component));
	}
	return pointer;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
