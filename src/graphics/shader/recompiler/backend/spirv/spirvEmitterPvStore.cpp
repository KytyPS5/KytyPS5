#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

// VS-store: the guest VS stores its PerVertex parameters to cap[record * stride +
// slot]; the guest PS reads the three corners of its primitive back. The record arithmetic mirrors
// graphics/host_gpu/renderer/perVertexVsStore.h (PvVsStoreVertexRecord / PvVsStoreCornerRecord).

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

namespace {

bool IsVertex(const EmitterState& state) {
	return state.pv_store->role == PvStoreInfo::Role::Vertex;
}

uint32_t U(EmitterState& state, spv::Op op, uint32_t lhs, uint32_t rhs) {
	return Binary(state, op, TypeU32(state), lhs, rhs);
}

uint32_t C(EmitterState& state, uint32_t value) {
	return ConstantU32(state, value);
}

uint32_t Push(EmitterState& state, uint32_t dword) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state), pointer,
	                          state.push_constant_variable, C(state, 1), C(state, dword));
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

uint32_t LoadSigned(EmitterState& state, uint32_t variable) {
	const auto value = state.builder.AllocateId();
	const auto bits  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeI32(state), value, variable);
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, value);
	return bits;
}

// base + (instance - first_instance) * span + (vertex - first)
uint32_t Record(EmitterState& state, uint32_t instance, uint32_t vertex) {
	const auto relative_instance =
	    U(state, spv::OpISub, instance, Push(state, kPvPushFirstInstance));
	const auto instance_part   = U(state, spv::OpIMul, relative_instance, Push(state, kPvPushSpan));
	const auto relative_vertex = U(state, spv::OpISub, vertex, Push(state, kPvPushFirst));
	return U(state, spv::OpIAdd, U(state, spv::OpIAdd, Push(state, kPvPushBase), instance_part),
	         relative_vertex);
}

uint32_t CapComponentPointer(EmitterState& state, uint32_t record, uint32_t slot,
                             uint32_t component) {
	// stride 0 = the stride comes with the draw (the two variants are emitted independently).
	constexpr uint32_t kStrideDword = kPvPushStride;
	const auto         stride       = state.pv_store->record_stride_vec4 != 0
	                                      ? C(state, state.pv_store->record_stride_vec4)
	                                      : Push(state, kStrideDword);
	const auto         element =
	    U(state, spv::OpIAdd, U(state, spv::OpIMul, record, stride), C(state, slot));
	const auto pointer = state.builder.AllocateId();
	if (component == UINT32_MAX) {
		state.builder.AddFunction(
		    spv::OpAccessChain,
		    TypePointer(state, spv::StorageClassStorageBuffer, TypeU32Vector(state, 4)), pointer,
		    state.pv_cap_buffer, C(state, 0), element);
	} else {
		state.builder.AddFunction(
		    spv::OpAccessChain, TypePointer(state, spv::StorageClassStorageBuffer, TypeU32(state)),
		    pointer, state.pv_cap_buffer, C(state, 0), element, C(state, component));
	}
	return pointer;
}

uint32_t SlotOf(const EmitterState& state, uint32_t host_location) {
	if (state.pv_store->record_stride_vec4 == 0) {
		// Dynamic stride: slot == host location. Location 31 carries the instance index: a stage
		// that uses it is still emitted (the location is recorded) and ClassifyPvVsStoreInterface
		// rejects the pair.
		EXIT_IF(host_location > kPvVsStoreInstanceLocation);
		if (state.pv_store->location_mask != nullptr)
			*state.pv_store->location_mask |= 1u << host_location;
		return host_location;
	}
	const auto slot = state.pv_store->parameter_slots.find(host_location);
	EXIT_IF(slot == state.pv_store->parameter_slots.end() ||
	        slot->second >= state.pv_store->record_stride_vec4);
	return slot->second;
}

// Index value of element `element` (16- or 32-bit buffer, chosen by the push flag).
uint32_t FetchIndex(EmitterState& state, uint32_t element) {
	const auto index32 =
	    Binary(state, spv::OpINotEqual, TypeBool(state), Push(state, kPvPushIndex32), C(state, 0));
	const auto word   = Select(state, TypeU32(state), index32, element,
	                           U(state, spv::OpShiftRightLogical, element, C(state, 1)));
	const auto length = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpArrayLength, TypeU32(state), length, state.pv_index_buffer, 0);
	const auto clamped = EmitGlsl<GLSLstd450UMin, IR::Type::U32>(
	    state, word, U(state, spv::OpISub, length, C(state, 1)));
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain,
	                          TypePointer(state, spv::StorageClassStorageBuffer, TypeU32(state)),
	                          pointer, state.pv_index_buffer, C(state, 0), clamped);
	const auto raw = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), raw, pointer);
	const auto half_shift = U(state, spv::OpShiftLeftLogical,
	                          U(state, spv::OpBitwiseAnd, element, C(state, 1)), C(state, 4));
	const auto shifted    = U(state, spv::OpShiftRightLogical, raw,
	                          Select(state, TypeU32(state), index32, C(state, 0), half_shift));
	return U(state, spv::OpBitwiseAnd, shifted,
	         Select(state, TypeU32(state), index32, C(state, 0xffffffffu), C(state, 0xffffu)));
}

} // namespace

void DefinePvStore(EmitterState& state) {
	const bool vertex = IsVertex(state);
	const auto uvec4  = TypeU32Vector(state, 4);
	const auto array  = state.builder.DecoratedType(
	    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 16}}}, uvec4);
	const auto block =
	    state.builder.DecoratedType(spv::OpTypeStruct,
	                                {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                 {spv::OpDecorate, {spv::DecorationBlock}}},
	                                array);
	state.pv_cap_buffer = state.builder.DefineGlobalVariable(
	    TypePointer(state, spv::StorageClassStorageBuffer, block), spv::StorageClassStorageBuffer);
	state.builder.AddName(state.pv_cap_buffer, "pv_cap");
	state.builder.AddAnnotation(spv::OpDecorate, state.pv_cap_buffer, spv::DecorationDescriptorSet,
	                            1);
	state.builder.AddAnnotation(spv::OpDecorate, state.pv_cap_buffer, spv::DecorationBinding, 0);
	if (!vertex)
		state.builder.AddAnnotation(spv::OpDecorate, state.pv_cap_buffer,
		                            spv::DecorationNonWritable);
	if (state.pv_store->coherent) {
		state.builder.AddAnnotation(spv::OpDecorate, state.pv_cap_buffer, spv::DecorationCoherent);
	}
	if (!vertex) {
		const auto words = state.builder.DecoratedType(
		    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4}}},
		    TypeU32(state));
		const auto index_block =
		    state.builder.DecoratedType(spv::OpTypeStruct,
		                                {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
		                                 {spv::OpDecorate, {spv::DecorationBlock}}},
		                                words);
		state.pv_index_buffer = state.builder.DefineGlobalVariable(
		    TypePointer(state, spv::StorageClassStorageBuffer, index_block),
		    spv::StorageClassStorageBuffer);
		state.builder.AddName(state.pv_index_buffer, "pv_indices");
		state.builder.AddAnnotation(spv::OpDecorate, state.pv_index_buffer,
		                            spv::DecorationDescriptorSet, 1);
		state.builder.AddAnnotation(spv::OpDecorate, state.pv_index_buffer, spv::DecorationBinding,
		                            1);
		state.builder.AddAnnotation(spv::OpDecorate, state.pv_index_buffer,
		                            spv::DecorationNonWritable);
		// PrimitiveId in a fragment shader needs the Geometry or the Tessellation capability; the
		// device enables tessellationShader (MoltenVK has no geometry shaders).
		state.builder.RequireCapability(spv::CapabilityTessellation);
	}
	// The instance index travels VS -> PS as a flat varying (the PS has no instance builtin).
	state.pv_instance_variable = DefineInterfaceVariable(
	    state, TypeU32(state), vertex ? spv::StorageClassOutput : spv::StorageClassInput,
	    "pv_instance");
	state.builder.AddAnnotation(spv::OpDecorate, state.pv_instance_variable, spv::DecorationFlat);
	state.builder.AddAnnotation(spv::OpDecorate, state.pv_instance_variable,
	                            spv::DecorationLocation, kPvVsStoreInstanceLocation);
}

void InitializePvStore(EmitterState& state) {
	if (state.pv_store == nullptr || !IsVertex(state)) return;
	const auto vertex =
	    LoadSigned(state, InputVariableForKind(state, IR::StageInputKind::VertexIndex));
	const auto instance =
	    LoadSigned(state, InputVariableForKind(state, IR::StageInputKind::InstanceIndex));
	state.builder.AddFunction(spv::OpStore, state.pv_instance_variable, instance);
	state.pv_vertex_record = Record(state, instance, vertex);
}

void EmitPvStoreParameter(EmitterState& state, uint32_t host_location, uint32_t value) {
	EXIT_IF(state.pv_store == nullptr || !IsVertex(state) || state.pv_vertex_record == 0);
	const auto bits = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeU32Vector(state, 4), bits, value);
	state.builder.AddFunction(spv::OpStore,
	                          CapComponentPointer(state, state.pv_vertex_record,
	                                              SlotOf(state, host_location), UINT32_MAX),
	                          bits);
	if (state.pv_store->coherent) {
		state.builder.AddFunction(spv::OpMemoryBarrier, C(state, spv::ScopeDevice),
		                          C(state, spv::MemorySemanticsAcquireReleaseMask |
		                                       spv::MemorySemanticsUniformMemoryMask));
	}
}

uint32_t EmitPvLoadCorner(EmitterState& state, uint32_t host_location, uint32_t corner,
                          uint32_t chan) {
	EXIT_IF(state.pv_store == nullptr || IsVertex(state) || corner >= kPvVsStoreCorners);
	const auto primitive =
	    LoadSigned(state, InputVariableForKind(state, IR::StageInputKind::PrimitiveId));
	const auto instance = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), instance, state.pv_instance_variable);
	// element = first_index + primitive * 3 + corner
	const auto element = U(state, spv::OpIAdd,
	                       U(state, spv::OpIAdd, Push(state, kPvPushFirstIndex),
	                         U(state, spv::OpIMul, primitive, C(state, kPvVsStoreCorners))),
	                       C(state, PvVsStoreCornerElement(corner)));
	// gl_VertexIndex of that corner in the VS: index + vertex_offset
	const auto vertex =
	    U(state, spv::OpIAdd, FetchIndex(state, element), Push(state, kPvPushVertexOffset));
	const auto record  = Record(state, instance, vertex);
	const auto pointer = CapComponentPointer(state, record, SlotOf(state, host_location), chan);
	const auto bits    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), bits, pointer);
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeF32(state), value, bits);
	return value;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
